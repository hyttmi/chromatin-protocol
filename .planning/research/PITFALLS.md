# Domain Pitfalls: Event-Driven Sync for chromatindb

**Domain:** Retrofitting push-based sync, event-driven expiry, and SDK auto-reconnect onto existing timer-paced distributed database node
**Researched:** 2026-04-02
**Confidence:** HIGH (based on direct codebase analysis, known past bugs, AEAD nonce desync history, and Asio coroutine patterns established across 78 phases)

---

## Critical Pitfalls

Mistakes in this category cause AEAD nonce desync (connection death), data loss, or require architectural rewrites.

### Pitfall 1: Push Notifications Introduce a Second Send Path That Races with Sync

**What goes wrong:**
The current system has exactly one active send path per peer connection at any time. Sync runs as a single coroutine that owns the connection's send side for its duration -- PEX is inline after sync, and SyncRejected is silently dropped (not sent) when a sync is in progress. This was a hard-won design after the v1.0.0 PEX SIGSEGV: sending SyncRejected via a detached coroutine raced with the sync initiator's writes, causing AEAD nonce desync.

Push notifications add a *permanent* second send path. When a blob is ingested, `notify_subscribers()` currently spawns a detached `co_spawn` per subscriber to call `conn->send_message()`. If that subscriber is simultaneously being synced with (a coroutine already holding the "send side"), two coroutines call `send_encrypted()` concurrently. Both read `send_counter_`, both increment it, and the nonces collide or skip. ChaCha20-Poly1305 with a reused nonce leaks the XOR of plaintexts. With a skipped nonce, the receiver's `recv_counter_` will never match, and every subsequent message fails decryption -- the connection is dead.

**Why it happens:**
The single-io_context-thread model gives the *illusion* of thread safety. All coroutines run on one thread, so no two coroutines execute simultaneously. But `send_encrypted` is itself a coroutine -- it `co_await`s `send_raw()` (which `co_await`s the TCP write). At that suspension point, another coroutine (the notification sender) can resume and call `send_encrypted()` with the same counter value that the first coroutine already read but hasn't yet incremented for the wire.

The key insight: `send_counter_++` happens *before* `co_await send_raw()`, so the counter itself doesn't race in the traditional sense. But if two coroutines both call `send_encrypted()` "concurrently" (interleaved at co_await points), they will both proceed to `send_raw()` with different nonces, but the *byte ordering on the wire* may be non-deterministic. The receiver expects nonce N then N+1 in wire order. If notification's frame (nonce N+1) arrives before sync's frame (nonce N) due to coroutine scheduling, the receiver will try to decrypt nonce-N+1-data with nonce-N key and fail.

More precisely: two coroutines each call `send_encrypted`. Coroutine A gets nonce 42, coroutine B gets nonce 43. Coroutine A suspends on TCP write. Coroutine B's TCP write completes first (smaller payload, or just scheduling order). Receiver gets nonce-43 ciphertext first but expects nonce 42. Decryption fails. Connection dies.

**Consequences:**
- AEAD decrypt failure on every subsequent message
- Connection teardown, triggering reconnect cycle
- Under high ingestion rate, *every* subscriber connection breaks repeatedly
- System degrades to worse-than-timer-sync because connections keep dying

**Prevention:**
Per-connection send serialization. There are two approaches:

1. **Send queue per connection** (recommended): Add a `std::deque<PendingMessage>` to Connection. All callers enqueue messages. A single writer coroutine drains the queue in order. `send_counter_` is only touched by this one coroutine. This is the same pattern the SDK already uses (`_send_lock` in Python Transport).

2. **Mutex-like coroutine lock**: An asio-compatible mutex (like a channel of capacity 1) that serializes access to `send_encrypted()`. Simpler to retrofit but adds co_await overhead per send.

The send queue approach is better because it also naturally handles backpressure (queue depth monitoring) and batching (multiple notifications in one frame).

**Detection:**
- AEAD decrypt failures in logs (`"AEAD decrypt failed (nonce desync or tampered)"`)
- Connections dropping immediately after blob ingestion
- TSAN will NOT catch this because it runs on a single thread -- the race is at the coroutine scheduling level, not the thread level

**Phase assignment:** This MUST be the first phase. Every subsequent push-notification feature depends on safe concurrent sends. Building push sync without solving this creates a system that crashes under load.

---

### Pitfall 2: Notification Storm During Reconcile-on-Connect

**What goes wrong:**
The new architecture calls for "reconcile-on-connect" -- full reconciliation when a peer connects or reconnects. During reconciliation, the responder ingests potentially thousands of blobs it was missing. Each ingested blob triggers `notify_subscribers()`, which fires push notifications to all other connected peers subscribed to those namespaces. If a peer reconnects after being offline for hours and has 10,000 blobs to catch up on, every subscriber gets 10,000 notifications in rapid succession.

This creates three cascading problems:
1. **SDK notification queue overflow**: The Python SDK `Transport._notifications` queue has `maxsize=1000`. Notifications beyond 1000 are silently dropped (`put_nowait` with `except QueueFull: pass`). The SDK user misses events with no indication.
2. **Notification bandwidth dominates the wire**: 10,000 notifications at 77 bytes each = ~750 KB of notification traffic competing with sync traffic on other connections, potentially triggering rate limiting.
3. **Duplicate fetch amplification**: If SDK clients receive notifications and attempt to fetch the notified blobs, they may issue thousands of ReadRequests concurrently, overwhelming the node.

**Why it happens:**
Timer-based sync spreads reconciliation load over time (one round per interval). Push-based notifications concentrate the load at connection events. The existing `notify_subscribers()` was designed for individual client writes (low frequency), not for batch sync ingestion (thousands of blobs per second).

**Consequences:**
- Silent notification loss (SDK users miss events)
- Rate limit triggers during catch-up
- Quadratic message amplification in N-node networks during mass reconnect (e.g., after network partition heals)
- Node appears unresponsive to SDK clients during peer catch-up

**Prevention:**
- **Suppress notifications during sync ingestion**: Add a flag or use a different ingest path during sync that does not fire `notify_subscribers()`. Peers receiving synced blobs don't need notifications -- they already have the data. Only client-originated writes (via Data/Delete handlers) should notify.
- **Or batch notifications**: Accumulate ingested blob metadata during sync, then send a single "catch-up complete" summary notification (new message type) instead of per-blob notifications.
- **SDK-side**: Increase queue size and/or make the queue unbounded with a separate high-water-mark warning log. But fixing the source (suppress during sync) is better.

**Detection:**
- `asyncio.QueueFull` silently swallowed in SDK
- Rate limiting spikes in node metrics immediately after peer reconnection
- Notification count per second in metrics exceeds write rate

**Phase assignment:** Must be addressed in the same phase as push notifications. If push notifications ship without sync-suppression, the first peer reconnection will trigger a notification flood.

---

### Pitfall 3: Event-Driven Expiry Timer Races with Blob Ingestion

**What goes wrong:**
The current expiry system scans the entire expiry index periodically (default 60s). The new design replaces this with a "next-expiry timer" -- a single timer set to fire at the earliest blob's expiry time. When the timer fires, it purges that blob and resets the timer to the next-earliest.

The race: a blob is ingested with `expiry_time = now + 5`. The ingestion code must update the expiry timer if this new blob expires sooner than the currently scheduled timer. But `store_blob()` runs inside `BlobEngine::ingest()`, which is a coroutine that co_awaits crypto offload and then posts back to the IO thread. The timer update must happen on the IO thread (where the timer lives). If the timer fires between the blob being stored and the timer being reset, the blob won't be purged until... there's no "until" -- the timer was already consumed and nobody reschedules it.

Concrete scenario:
1. Timer is set for T+300 (next expiry in 5 minutes)
2. Blob arrives with TTL=2, so expiry_time = now + 2
3. Blob is stored in storage (expiry index updated)
4. Before the code resets the timer to T+2, the old timer fires at T+300 (it was already past)
5. The old timer's handler runs, scans, finds the new blob is not yet expired (it was just stored), does nothing
6. Timer is now consumed. The code that was about to reset it... sets it to T+2, which is already in the past
7. Timer fires immediately, purges the blob. This is actually OK in this case.

But the *real* race is more subtle:
1. Timer set for T+10
2. Blob A ingested with expiry T+5
3. IO-thread transfer via `co_await asio::post(ioc_)` happens
4. Between the post and the timer update, the T+10 timer fires
5. Expiry scan runs, finds blob A is not expired yet (T+5 is in the future)
6. The blob ingestion code now tries to cancel and reset the timer -- but the timer already fired and its handler is running
7. If the handler is still in `storage_.run_expiry_scan()` (which accesses the database), and the ingestion code also accesses the database to check the new earliest expiry, both run on the IO thread and won't actually interleave (single-threaded). But the handler will have already scanned and found nothing to purge, and now the timer needs to be rearmed. Who rearms it?

The fundamental problem: *ownership of the timer rearm*. If both the ingestion path and the expiry handler can rearm the timer, you need a clear protocol for who does it and when.

**Consequences:**
- Blobs with short TTLs linger past their expiry time (undead data)
- Or blobs get purged prematurely if timestamps are mishandled
- Expiry timer "stalls" -- once it fires and nobody rearms it, expired blobs accumulate forever until the safety-net scan catches them (but if the safety-net interval is 10-15 minutes, that's a long window)

**Prevention:**
- **Always rearm in the expiry handler**: After each expiry scan (even if nothing was purged), query storage for the next-earliest expiry time and rearm. The ingestion path *also* checks if the new blob's expiry is earlier than the current timer and cancels + rearms if so. On the single-threaded IO context, these operations cannot actually interleave mid-function, so the "race" is really about sequencing. Use the timer-cancel pattern already established: store the timer pointer, cancel it to wake the expiry coroutine, let the coroutine handle the rearm.
- **Keep the safety-net scan**: The event-driven timer is an optimization for latency. The periodic scan (at 10-15 min) is the correctness backstop. Without it, any bug in the event-driven timer means expired data lives forever.
- **Do NOT remove the periodic scan**: This is the most likely mistake -- assuming the event-driven approach is "complete" and the periodic scan is unnecessary overhead.

**Detection:**
- Blobs surviving past their `timestamp + ttl`
- Expiry timer handler not firing (no log output from expiry scan)
- Gap between expected and actual purge count in metrics

**Phase assignment:** Event-driven expiry phase. Requires careful integration testing with sub-second TTLs.

---

### Pitfall 4: SDK Auto-Reconnect Resurrects Stale Subscription State

**What goes wrong:**
The SDK maintains subscription state in `ChromatinClient._subscriptions` (a Python `set[bytes]`). When the transport layer reconnects, the SDK needs to re-subscribe to all previously subscribed namespaces. But the node's subscription state is *connection-scoped* -- when the TCP connection drops, the node forgets all subscriptions for that peer. On reconnect, the SDK has a new connection (new AEAD session, new nonce counters), and the node has zero subscriptions.

The naive auto-reconnect sends Subscribe messages for all entries in `_subscriptions`. The problems:

1. **Missed notifications during the reconnect window**: Between disconnect and re-subscribe completion, all notifications for those namespaces are lost. The SDK user's `async for notif in conn.notifications()` loop silently misses events. There is no mechanism to know what was missed.

2. **Stale subscription set**: If the application unsubscribed from a namespace just before the disconnect (the Unsubscribe message may not have been sent), the auto-reconnect will re-subscribe to it. The `_subscriptions` set was updated optimistically before the Unsubscribe was actually delivered.

3. **Pending request futures become invalid**: On disconnect, `_cancel_all_pending()` sets exceptions on all pending futures. The application code gets `ConnectionError`. But if the application is in a tight retry loop (common pattern: `while True: try: await conn.write_blob(...)`) and auto-reconnect happens transparently, the application may not realize the connection changed. Worse, if there are pending `send_request()` calls and the transport reconnects, the `_next_request_id` counter resets. The node won't correlate old request_ids from the new connection -- but the pending futures dict was already cleared, so this is OK. The real issue is that the application's retry loop may resubmit a write that already succeeded on the old connection (duplicate write).

4. **Directory cache invalidation**: If the application uses a `Directory` (v1.7.0), the directory's subscribe-before-scan pattern depends on receiving notifications for cache invalidation. After reconnect, the directory must re-subscribe and re-scan. If auto-reconnect doesn't trigger directory re-initialization, the directory cache becomes permanently stale.

**Why it happens:**
Connection-scoped state (subscriptions, request_id, nonce counters) cannot be transparently restored. Auto-reconnect creates a *new* connection that looks like the old one to the application but has none of the old state on the server side.

**Consequences:**
- Silent notification gaps (most dangerous -- application logic that depends on notifications being complete will have invisible bugs)
- Stale directory caches leading to failed encryption (can't find users)
- Duplicate writes if application retries without idempotency checks
- Application logic that assumes subscription continuity silently breaks

**Prevention:**
- **Auto-reconnect is NOT transparent**: The SDK should emit a reconnection event (callback or sentinel notification) so the application knows state was lost. The application must re-establish its logical state (re-subscribe, re-scan directory, invalidate caches).
- **Re-subscribe automatically but notify the application**: The SDK can re-issue Subscribe messages for `_subscriptions` after reconnect, AND emit a "reconnected" event so the application can do a catch-up scan.
- **Provide a "last_seen_seq" per namespace**: If the SDK tracks the highest seq_num seen per subscribed namespace, after reconnect it can issue a ListRequest(namespace, after=last_seen_seq) to discover what it missed. This catch-up pattern is the standard solution for notification gaps.
- **Reset `_next_request_id` to 1 on reconnect**: The node only correlates request_ids within a connection. New connection = new request_id space. The SDK already clears pending futures on disconnect, so this is safe.

**Detection:**
- Missing notifications after network blip (requires application-level monitoring)
- Directory.get_user returning None for known users after reconnect
- Application receiving "connection lost" exceptions after supposedly-transparent reconnect

**Phase assignment:** SDK auto-reconnect phase. This is an SDK-only change, but it has protocol implications (catch-up mechanism needs a point-in-time query, which TimeRangeRequest or ListRequest already provides).

---

## Moderate Pitfalls

Mistakes in this category cause performance degradation, subtle bugs, or require significant rework of a single component.

### Pitfall 5: Relay Double-Decrypt/Re-Encrypt for Push Notifications

**What goes wrong:**
The relay sits between SDK clients and the chromatindb node, forwarding messages over two separate AEAD channels (client-to-relay and relay-to-node). Currently, the node sends Notification messages to the relay (over the UDS channel), and the relay forwards them to subscribed SDK clients (over their individual PQ-authenticated channels).

When the node adds peer-to-peer push notifications (BlobNotify messages between nodes), these are *node-to-node* messages on AEAD-encrypted peer connections. They never touch the relay. But if the v2.0.0 design adds a new "targeted fetch" message (peer requests a specific blob by hash from a notification), the relay's message filter must be updated to either allow or block these new message types.

The pitfall: introducing a new message type for push sync (e.g., `BlobNotify`, `BlobFetchRequest`, `BlobFetchResponse`) and forgetting to classify them in the relay's blocklist. The relay uses a blocklist approach -- it blocks 21 peer-internal message types and passes everything else through. A new peer-internal message type that isn't blocked will be forwarded to SDK clients, who can't process it and may choke.

**Why it happens:**
The relay filter was last expanded in v1.4.0 (Phase 67) to 38 client-allowed types. New message types default to "allowed" unless explicitly blocked. The blocklist approach is future-proof for *client* message types but actively dangerous for *peer-internal* types.

**Prevention:**
- Add all new peer-internal message types to the relay blocklist in the same phase they're created
- Add integration tests that verify the relay rejects the new types from client connections
- Consider whether any new types should be exposed to clients (BlobNotify might be useful for SDK-level push)

**Detection:**
- SDK receiving unknown message types (they'd go to the notification queue as unmatched responses)
- Integration test with relay in the path

**Phase assignment:** Same phase as new wire types. Must be coordinated.

---

### Pitfall 6: Cursor Cleanup on Disconnect Interacts with Reconnect Timing

**What goes wrong:**
The current system compacts cursors every 6 hours, pruning cursors for disconnected peers. The v2.0.0 design moves this to "disconnect-triggered" -- when a peer disconnects, immediately compact its cursors. But peers frequently disconnect and reconnect (network blips, restarts). If the cursor is compacted on disconnect, and the peer reconnects 2 seconds later, the reconnection triggers a full reconciliation (no cursor), which triggers a notification storm (Pitfall 2), which can cascade.

The 6-hour timer was a buffer against this exact scenario. Cursors for recently-disconnected peers are kept warm so reconnections are cheap (cursor hit = skip namespace).

**Why it happens:**
The optimization (immediate cleanup) is correct for peers that truly left the network. It's wrong for peers experiencing transient disconnects. Without a grace period, every network blip becomes a full resync.

**Consequences:**
- Full reconciliation on every transient reconnect instead of cursor-based O(new) sync
- Increased sync traffic proportional to disconnection frequency
- Combined with notification suppression (if not implemented), cascade into notification storms

**Prevention:**
- **Grace period before compaction**: Mark cursor as "pending compaction" on disconnect. Start a 5-minute timer. If the peer reconnects within the grace period, cancel the timer and keep the cursor. If the timer fires, compact.
- **Do NOT compact on disconnect for bootstrap peers**: Bootstrap peers always reconnect. Their cursors should be permanent.
- **Alternative**: Keep the 6-hour compaction as-is and just *add* immediate compaction for peers that have been disconnected AND have no bootstrap entry AND no persisted peer entry. This preserves the cursor warmth for known peers.

**Detection:**
- `cursor_misses` metric spikes after peer reconnection events
- `full_resyncs` metric increases relative to stable state
- Sync traffic volume higher than expected for a reconnecting peer that was recently connected

**Phase assignment:** Disconnect-triggered cleanup phase. Should follow the push notification phase so the interaction is understood.

---

### Pitfall 7: Push Sync Bypasses Sync Rate Limiting

**What goes wrong:**
The current system has sync rate limiting: per-peer cooldown (`sync_cooldown_seconds`), session limits (`max_sync_sessions`), and byte-rate accounting. These controls exist to prevent a misbehaving peer from overwhelming the node with sync traffic.

Push-based sync changes the model: instead of the peer initiating sync (which the node can reject via SyncRejected), the *node* pushes notifications to the peer, and the peer fetches specific blobs. The fetch requests are regular client-style requests (Data/ReadRequest or a new BlobFetchRequest), not sync protocol messages. The existing sync rate limiter doesn't apply to these.

A malicious peer could subscribe to all namespaces, receive every notification, then issue thousands of fetch requests. The fetch requests bypass the sync rate limiter because they're classified as client operations.

**Why it happens:**
The sync rate limiter was designed for the old pull model where the peer initiates. Push-based sync inverts the control flow. The node pushes notifications; the peer pulls data. The pull side looks like regular client reads, not sync operations.

**Consequences:**
- No protection against notification-amplified fetch floods
- Rate limiting becomes ineffective for the primary data transfer mechanism
- A single misbehaving subscriber can saturate the node's read path

**Prevention:**
- **Count fetch requests toward sync byte budget**: If a peer requests more than X bytes per second of blob data via fetch, throttle or disconnect.
- **Or limit notification rate per peer**: Cap the number of notifications sent per second per subscriber connection.
- **Or both**: Rate limit outgoing notifications AND incoming fetch requests.
- The existing `bucket_tokens` / `bucket_last_refill` rate limiter in PeerInfo already meters all messages universally. Verify that fetch requests flow through the same `on_peer_message` top-level handler that updates the rate limiter. If they do, the existing rate limiter already applies. If fetch is a new handler path, it must be added.

**Detection:**
- Bandwidth spikes from a single peer after push sync is enabled
- Missing rate_limited metric increments for clearly excessive traffic
- Memory pressure from serving many concurrent blob reads

**Phase assignment:** Push sync phase. Rate limiting implications must be verified when the new fetch mechanism is implemented.

---

### Pitfall 8: Notification Ordering Is Not Causal

**What goes wrong:**
In a multi-node network, events propagate through sync. Node A ingests blob X, notifies its subscribers. Node B receives blob X via sync from Node A, ingests it, notifies its subscribers. An SDK client connected to Node B sees notification for blob X. But if another blob Y was written to Node A *after* X, and the SDK client is subscribed on Node A for Y's namespace, the client might receive notification Y before notification X (because the direct notification from A arrives before the sync-then-notify chain through B).

More concretely: notifications carry `seq_num`, but seq_nums are per-node, per-namespace. There is no global ordering across nodes or namespaces. A client subscribed to the same namespace on two different nodes will see different seq_num sequences.

**Why it happens:**
The system provides eventual consistency with no causal ordering guarantees. Timer-based sync masked this because everything was batch-processed. Push notifications surface the ordering problem because events arrive individually and in real-time.

**Consequences:**
- Application logic that assumes "if I see notification for seq 5, I've seen everything up to seq 4" breaks across multiple nodes
- Directory cache invalidation may process updates out of order, resulting in an older UserEntry overwriting a newer one (the directory already uses latest-timestamp-wins, which mitigates this, but only for the directory)
- Chat-like applications built on chromatindb see messages out of order

**Prevention:**
- **Document the guarantee**: Push notifications provide at-most-once delivery with no ordering guarantee across nodes. Same-node, same-namespace notifications are ordered by seq_num.
- **SDK-side reordering**: If the application needs ordering, it must buffer notifications and reorder by timestamp or seq_num before processing.
- **Do NOT try to implement global ordering**: It requires consensus (Paxos/Raft) or logical clocks, neither of which fits this architecture. The blob's `timestamp` field is the application's ordering hint.

**Detection:**
- Application-level tests that verify notification ordering fail intermittently in multi-node setups
- Directory cache returning stale data after what should have been an update

**Phase assignment:** Documentation phase. The ordering semantics must be clearly specified in PROTOCOL.md.

---

### Pitfall 9: Keepalive Ping/Pong Races with Push Notifications

**What goes wrong:**
The v2.0.0 design adds bidirectional ping/pong keepalive for faster dead connection detection. Currently, the node uses receiver-side inactivity detection (one-way: if no message received within timeout, disconnect). This was explicitly chosen to avoid AEAD nonce desync from bidirectional keepalive messages (documented in Key Decisions).

Adding node-initiated Ping messages means the node now actively sends on the connection. If the node sends a Ping while a push notification coroutine is also sending, the same nonce-desync race from Pitfall 1 applies. Ping is just another message that needs the send path.

**Why it happens:**
The current receiver-side inactivity approach has zero send-side impact. It only reads `last_message_time` (updated at the top of `on_peer_message`). Switching to sender-side keepalive adds another send path -- the third, after sync and notifications.

**Consequences:**
Same as Pitfall 1 -- AEAD nonce desync and connection death. The keepalive mechanism designed to detect dead connections ends up killing live ones.

**Prevention:**
- Solve Pitfall 1 first (send queue). Once all sends go through a single queue, Ping is just another enqueued message.
- **Alternative**: Keep receiver-side inactivity but have the *other side* send keepalive. I.e., the SDK sends Pings (it already does), the node just checks for activity. This is the current model and works. Only add node-initiated Pings if the SDK is unreliable at sending them (e.g., application paused but TCP connection alive).

**Detection:**
- Connection drops correlating with keepalive timer intervals
- AEAD failures in logs at regular (keepalive) intervals

**Phase assignment:** Same phase as send queue implementation (Pitfall 1). Keepalive should NOT be implemented before the send queue.

---

### Pitfall 10: Targeted Fetch Creates an Unverified Trust Assumption

**What goes wrong:**
In the current sync model, blob transfer during reconciliation goes through the full `SyncProtocol::ingest_blobs()` path, which validates every blob (namespace ownership, signature verification, timestamp, dedup). The new "targeted fetch" model allows a peer to request a specific blob by hash based on a notification.

If the fetch response handler uses a different code path than sync ingestion (e.g., directly calling `storage_.store_blob()` or a simplified `engine_.ingest()` variant), it might skip validation steps. A malicious peer could send notifications for blobs that don't exist, or respond to fetch requests with invalid blobs.

**Why it happens:**
Developers naturally optimize the fetch path: "we already know this blob exists because we got a notification for it, so we can skip some checks." This is wrong because the notification comes from a peer, and peers can be malicious or buggy.

**Consequences:**
- Invalid blobs stored (no signature verification)
- Namespace pollution (blobs stored under wrong namespace)
- The entire security model (verify-before-store) bypassed

**Prevention:**
- **All ingestion MUST go through BlobEngine::ingest()**: There is exactly one ingest path. Targeted fetch responses are deserialized into `wire::BlobData` and passed through the same `ingest()` pipeline as sync-transferred blobs.
- **No special-case fetch handler**: The fetch response handler should be identical to the sync blob transfer handler, just for a single blob instead of a batch.
- **Test with malicious payloads**: Send a fetch response with a wrong signature, wrong namespace, expired timestamp. All must be rejected.

**Detection:**
- Blobs in storage that fail re-verification
- Missing `engine_.ingest()` call in fetch response handler code
- Code review: any path that calls `storage_.store_blob()` directly from a network handler

**Phase assignment:** Targeted fetch phase. Must use existing ingest pipeline.

---

## Minor Pitfalls

### Pitfall 11: Safety-Net Reconciliation Timer Drift

**What goes wrong:**
The safety-net reconciliation (10-15 min background check) uses a `steady_timer`. If the timer handler itself takes significant time (large network, many namespaces), the effective interval drifts. After a reconciliation that takes 30 seconds, the next one fires 10 minutes after the *end* of the previous one, not 10 minutes after the *start*. Over time, the interval grows.

**Prevention:**
Calculate next fire time from `start_of_round + interval`, not `end_of_round + interval`. The current `sync_timer_loop` already exhibits this pattern (timer fires after interval, then sync runs, then timer fires after interval). For a safety-net check that runs infrequently, this is acceptable. Only flag if the safety-net becomes a correctness requirement rather than a monitoring signal.

**Phase assignment:** Safety-net reconciliation phase. Low priority.

---

### Pitfall 12: Notification Queue Backpressure in SDK

**What goes wrong:**
The SDK `Transport._notifications` queue is bounded at 1000 entries. Under push-based sync, a node experiencing high write volume could send hundreds of notifications per second. If the SDK application's `async for notif in conn.notifications()` loop processes slowly (e.g., each notification triggers a ReadRequest that takes 50ms), notifications accumulate and eventually drop.

**Prevention:**
- Make the queue size configurable (constructor parameter)
- Log a warning on first drop (not per drop -- that would flood logs)
- Consider an unbounded queue with a high-water-mark metric
- Document that notification delivery is at-most-once, not at-least-once

**Phase assignment:** SDK auto-reconnect phase. Queue sizing is part of the reliability story.

---

### Pitfall 13: FlatBuffers Schema Changes Require Relay Rebuild

**What goes wrong:**
New message types for push sync (BlobNotify, BlobFetchRequest, etc.) require FlatBuffers schema updates (transport.fbs). The relay includes the same `transport_generated.h` and must be rebuilt. If the relay binary is deployed without rebuilding, it won't recognize the new type IDs and may misroute or drop messages.

**Prevention:**
- The relay's blocklist approach means unrecognized types pass through (they're not in the blocklist). This is actually the correct behavior for new client-facing types. But new peer-internal types that should be blocked will also pass through, which is Pitfall 5.
- Rebuild relay whenever transport.fbs changes. Add this to the build process documentation.

**Phase assignment:** First phase that adds new message types.

---

### Pitfall 14: Co_await Between Notification and Timer Reset

**What goes wrong:**
When implementing event-driven expiry (Pitfall 3), the code must query the next-earliest expiry time after each ingestion. If this query involves a storage read (scanning the expiry index), it happens on the IO thread synchronously (no co_await). But if the implementation wraps it in a coroutine with `co_await`, another coroutine can run between the query and the timer reset, potentially storing another blob with an even earlier expiry. The timer gets set to the wrong time.

**Prevention:**
- Keep the "query next expiry + reset timer" path synchronous (no co_await). Since storage reads are single-threaded and fast (libmdbx cursor seek), there's no need for async here.
- The expiry index is already sorted by `[expiry_ts_be:8][hash:32]`, so finding the minimum is a single cursor seek to the beginning -- O(1).

**Phase assignment:** Event-driven expiry phase.

---

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Severity | Mitigation |
|---|---|---|---|
| Push notifications (BlobNotify) | Nonce desync from concurrent sends (Pitfall 1) | CRITICAL | Implement send queue first |
| Push notifications (BlobNotify) | Notification storm during reconciliation (Pitfall 2) | CRITICAL | Suppress notifications during sync ingest |
| Reconcile-on-connect | Full resync floods from cursor loss (Pitfall 6) | MODERATE | Grace period before cursor compaction |
| Targeted blob fetch | Validation bypass (Pitfall 10) | MODERATE | Force all fetches through BlobEngine::ingest() |
| Targeted blob fetch | Sync rate limiter bypass (Pitfall 7) | MODERATE | Verify fetch requests go through rate limiter |
| Event-driven expiry | Timer stall from rearm race (Pitfall 3) | CRITICAL | Keep safety-net scan, rearm in both paths |
| Disconnect cursor cleanup | Cursor loss on transient disconnect (Pitfall 6) | MODERATE | Grace period timer, preserve bootstrap cursors |
| SDK auto-reconnect | Stale subscriptions, missed notifications (Pitfall 4) | CRITICAL | Expose reconnect event, catch-up scan |
| Connection keepalive | Third send path races with notifications (Pitfall 9) | MODERATE | Depends on send queue (Pitfall 1) |
| New wire types | Relay filter not updated (Pitfall 5) | MODERATE | Update blocklist + integration test |
| Documentation | Ordering guarantees not specified (Pitfall 8) | MODERATE | Document explicitly in PROTOCOL.md |

## AEAD Nonce Management: Detailed Analysis

The v2.0.0 changes fundamentally alter the AEAD nonce safety model. Here is the current state and what changes:

### Current State (Safe)

| Send Path | When | Safety Mechanism |
|---|---|---|
| Sync initiator | Timer-triggered, one coroutine per peer | `peer->syncing` flag prevents second sync |
| PEX exchange | Inline after sync (same coroutine) | No concurrent path exists |
| Notification (to subscribers) | After client write | `co_spawn` per subscriber, but only fires when no sync is active (client writes don't overlap with sync because sync is initiator-only) |
| Client response | After request processing | Inline in handler coroutine |
| SyncRejected | When sync request arrives during active sync | **Silently dropped** (not sent -- learned from v1.0.0 SIGSEGV) |

The key invariant: *at most one coroutine writes to a connection at any time*. This invariant holds because:
1. Sync is initiator-only (the initiator controls when to send)
2. Notifications are client-originated (low frequency, outside sync windows)
3. SyncRejected is not sent during active sync

### After v2.0.0 (Requires Mitigation)

| New Send Path | When | Conflict With |
|---|---|---|
| Push notification to peers | On every blob ingest (including sync ingest) | Sync coroutine (if peer is being synced AND subscribed) |
| Keepalive Ping | Periodic timer | Push notification, sync |
| Targeted fetch response | On peer fetch request | Push notification for same peer |

The invariant breaks. Multiple coroutines will want to send on the same connection simultaneously.

### Required Mitigation: Connection Send Queue

```
Connection {
    send_queue_: deque<PendingFrame>
    writer_active_: bool

    enqueue_send(type, payload, request_id):
        send_queue_.push_back({type, payload, request_id})
        if !writer_active_:
            writer_active_ = true
            co_spawn(drain_send_queue())

    drain_send_queue():
        while !send_queue_.empty():
            frame = send_queue_.pop_front()
            encrypted = encrypt(frame, send_counter_++)
            co_await send_raw(encrypted)
        writer_active_ = false
}
```

This guarantees:
- `send_counter_` is only incremented by one coroutine
- Frames arrive on the wire in `send_counter_` order
- No two `send_raw()` calls interleave

The SDK already has this pattern (`_send_lock` mutex in Transport). The C++ node needs it too.

## Sources

- Direct codebase analysis: `db/peer/peer_manager.cpp`, `db/net/connection.cpp`, `db/net/connection.h`, `db/sync/sync_protocol.h`, `sdk/python/chromatindb/_transport.py`, `sdk/python/chromatindb/client.py`
- Project history: v1.0.0 PEX SIGSEGV root cause (AEAD nonce desync from concurrent SyncRejected writes)
- Key Decision: "Silent SyncRequest drop when peer syncing" -- prevents nonce desync
- Key Decision: "Receiver-side inactivity (not Ping sender)" -- avoids AEAD nonce desync from bidirectional keepalive
- Key Decision: "Inline PEX after sync" -- prevents AEAD nonce desync from concurrent message streams
- [AEAD nonce reuse vulnerability in hpke-js](https://github.com/dajiaji/hpke-js/security/advisories/GHSA-73g8-5h73-26h4) -- concurrent Seal() with same sequence number
- [RFC 9771: Properties of AEAD Algorithms](https://www.rfc-editor.org/rfc/rfc9771.html)
- [Event-Driven Architecture: The Hard Parts](https://threedots.tech/episode/event-driven-architecture/)
- [Integrating Event-Driven Architectures with Existing Systems](https://www.oreilly.com/library/view/building-event-driven-microservices/9781492057888/ch04.html)
