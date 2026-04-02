# Phase 79: Send Queue & Push Notifications - Research

**Researched:** 2026-04-02
**Domain:** Per-connection send queue serialization + BlobNotify fan-out for push-based peer sync
**Confidence:** HIGH

## Summary

Phase 79 introduces the foundational safety mechanism for all concurrent send paths in chromatindb: a per-connection send queue that serializes all outbound messages to guarantee AEAD nonce ordering. On top of this queue, it adds BlobNotify (type 59) fan-out to all connected peers on every blob ingest, with source exclusion and storm suppression during active reconciliation.

The send queue is the single most structurally impactful change to the Connection class in 78 phases. Currently, `send_message()` directly calls `send_encrypted()` which increments `send_counter_++` and then `co_await`s the TCP write. Multiple concurrent coroutines (Data handler, Delete handler, Pong in message_loop, notify_subscribers via co_spawn detached, sync responder) can all call `send_encrypted()` on the same connection. The co_await suspension points in TCP writes allow interleaving that produces out-of-order nonces on the wire, killing the connection. The fix: every outbound message goes through a queue, and a single drain coroutine serializes writes. The existing Pong send in `message_loop()` must also go through this queue.

The notification system unifies two existing paths: (1) `notify_subscribers()` for client pub/sub (type 21 Notification) and (2) the `sync_proto_.set_on_blob_ingested()` callback. Both are replaced by a single `BlobEngine::on_blob_ingested` callback that PeerManager registers. This callback handles both BlobNotify fan-out to all TCP peers and Notification dispatch to subscribed clients.

**Primary recommendation:** Implement the send queue first as a standalone change to Connection, then layer BlobNotify and the unified callback on top. The queue is a safety mechanism; BlobNotify is a feature. They should be separable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Send queue serializes ALL outbound messages on a connection (not just notifications). Every `send_message()` call goes through the queue. This fixes the existing AEAD nonce race in pub/sub `co_spawn(detached)` notifications too.
- **D-02:** Queue is internal to Connection -- transparent to callers. `send_message()` enqueues and a dedicated drain coroutine serializes writes. All existing callers work unchanged.
- **D-03:** `send_message()` remains awaitable -- enqueues the message and `co_await`s until the drain coroutine has written it to the socket. Caller gets backpressure signal and knows delivery succeeded.
- **D-04:** Bounded queue with disconnect. Cap at 1024 messages. If a peer falls behind beyond the cap, disconnect it -- it will recover via reconcile-on-connect (Phase 82).
- **D-05:** Simple message count cap (not byte-based). At 77 bytes per notification, 1024 = ~77 KiB. Sync blobs are larger but one-at-a-time so queue rarely holds more than a few.
- **D-06:** Suppress BlobNotify to the syncing peer only (the peer that sent the blob via sync). All other connected peers still receive BlobNotify for sync-received blobs -- they need to know for fast propagation.
- **D-07:** Client-written blobs are NOT suppressed during sync. If a client writes a blob to node B while B is syncing with peer A, B still sends BlobNotify to all peers (including A). Only blobs arriving via sync from a specific peer are suppressed back to that peer.
- **D-08:** Engine callback -- `BlobEngine::ingest()` fires an `on_blob_ingested` callback on every successful ingest (client write or sync). PeerManager registers the callback and handles all fan-out. Single hook point, no missed paths.
- **D-09:** `ingest()` takes an optional `Connection::Ptr source` parameter (nullptr for client writes via relay/UDS). The callback passes it through so PeerManager can exclude the source from BlobNotify fan-out.
- **D-10:** Unify existing pub/sub Notification (client subscriptions) with BlobNotify (peer-to-peer) under the same engine callback. One callback fires on every ingest, PeerManager handles both: (1) BlobNotify fan-out to all peers, and (2) Notification to subscribed clients. Current sync_protocol callback path replaced.

### Claude's Discretion
- Queue data structure (std::deque, asio::experimental::channel, custom ring buffer)
- Drain coroutine lifecycle (started on connection init, stopped on close)
- How handshake messages bypass the queue (they run before the message loop starts, so naturally excluded)
- Error handling when queue is full (log level, disconnect message)
- How the existing `notify_subscribers()` is refactored into the unified callback

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PUSH-01 | Node notifies all connected peers immediately when a new blob is ingested | Engine callback (D-08/D-10) fires on every ingest; PeerManager fan-out sends BlobNotify to all TCP peers |
| PUSH-02 | Notification contains namespace, blob hash, seq_num, size, tombstone flag (77-byte payload) | Existing `encode_notification()` already produces this exact format -- reuse directly |
| PUSH-03 | Notifications suppressed during active reconciliation to prevent storms | Storm suppression via D-06/D-07: suppress BlobNotify only to the peer that sent the blob via sync |
| PUSH-04 | Per-connection send queue serializes all outbound messages to prevent AEAD nonce desync | Send queue in Connection class (D-01/D-02/D-03); drain coroutine serializes all writes |
| PUSH-07 | Node does not send BlobNotify back to the peer that originated the blob | Source exclusion via D-09: `ingest()` takes `Connection::Ptr source`, callback excludes source from fan-out |
| PUSH-08 | Push notifications delivered to currently-connected peers only | Fan-out iterates `peers_` deque which contains only live connections; disconnected peers recover via reconcile-on-connect |
| WIRE-01 | New message type BlobNotify (type 59) | Add `BlobNotify = 59` to `transport.fbs` TransportMsgType enum; regenerate headers |
| WIRE-04 | Relay message filter updated to block type 59 | Add `TransportMsgType_BlobNotify` to blocklist switch in `message_filter.cpp` |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | Co_spawn, steady_timer, io_context dispatch | Already used everywhere; send queue built on existing coroutine patterns |
| FlatBuffers | (existing) | TransportMsgType enum extension | Add BlobNotify=59 to existing enum; regenerate headers |
| libsodium | (existing) | AEAD nonce via send_counter_ | Not directly modified, but the nonce desync fix is the entire point |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| spdlog | (existing) | Log queue full disconnect, notification fan-out | Standard logging for new paths |
| Catch2 | (existing) | Unit tests for send queue, notifications | All new behavior needs test coverage |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| std::deque for send queue | asio::experimental::channel | Channel adds experimental API risk, unnecessary for single-threaded dispatch; deque is simpler and proven in codebase |
| std::deque for send queue | Custom ring buffer | Ring buffer has fixed capacity but deque grows/shrinks naturally; 1024 cap is enforced by count check not container capacity |

**Installation:** No new packages. Zero new dependencies.

## Architecture Patterns

### Recommended Modification Structure
```
db/
  net/
    connection.h       # Add: send queue members, drain coroutine declaration
    connection.cpp     # Modify: send_message() -> enqueue; add drain_send_queue()
                       # Modify: message_loop() Pong -> use send_message() not send_encrypted()
  engine/
    engine.h           # Add: on_blob_ingested callback typedef and setter; modify ingest() signature
    engine.cpp         # Add: callback invocation after successful ingest
  peer/
    peer_manager.h     # Add: on_blob_ingested() unified callback method
    peer_manager.cpp   # Modify: constructor callback registration
                       # Modify: Data/Delete handlers to pass source connection
                       # Remove: direct notify_subscribers() calls from Data/Delete handlers
                       # Modify: notify_subscribers() -> unified fan-out
  schemas/
    transport.fbs      # Add: BlobNotify = 59
  relay/core/
    message_filter.cpp # Add: BlobNotify to blocklist
```

### Pattern 1: Send Queue with Drain Coroutine

**What:** Every outbound message is enqueued into a per-connection deque. A single drain coroutine (started when the message loop starts) pops messages and calls `send_encrypted()` in strict FIFO order. The drain coroutine is the only code that touches `send_counter_`.

**When to use:** All post-handshake message sends -- `send_message()`, Pong replies, close_gracefully goodbye.

**Key design:**
```cpp
// In connection.h -- new members:
struct PendingMessage {
    std::vector<uint8_t> encoded;  // TransportCodec::encode() result
    std::shared_ptr<std::promise<bool>> result;  // For awaitable callers
};

std::deque<PendingMessage> send_queue_;
asio::steady_timer send_signal_;  // Timer-cancel pattern to wake drain coroutine
bool drain_running_ = false;
static constexpr size_t MAX_SEND_QUEUE = 1024;

// send_message() becomes:
asio::awaitable<bool> Connection::send_message(
    wire::TransportMsgType type,
    std::span<const uint8_t> payload,
    uint32_t request_id) {
    if (closed_) co_return false;
    auto encoded = TransportCodec::encode(type, payload, request_id);
    co_return co_await enqueue_send(std::move(encoded));
}

// enqueue_send() adds to queue and waits for drain:
asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    if (send_queue_.size() >= MAX_SEND_QUEUE) {
        spdlog::warn("send queue full ({} messages), disconnecting {}", MAX_SEND_QUEUE, remote_addr_);
        close();
        co_return false;
    }
    // ... enqueue and signal drain coroutine ...
}

// drain_send_queue() -- the single writer:
asio::awaitable<void> Connection::drain_send_queue() {
    while (!closed_) {
        while (!send_queue_.empty() && !closed_) {
            auto msg = std::move(send_queue_.front());
            send_queue_.pop_front();
            bool ok = co_await send_encrypted(msg.encoded);
            // Signal result to awaiting caller
            // ...
        }
        // Wait for signal (timer-cancel pattern)
        send_signal_.expires_after(std::chrono::hours(24));
        auto [ec] = co_await send_signal_.async_wait(use_nothrow);
        // ec == operation_aborted means new message enqueued (cancel woke us)
    }
}
```

**Important detail -- Timer-cancel wakeup pattern:** The codebase already uses this pattern extensively (sync_inbox with sync_notify timer, expiry_scan_loop). The drain coroutine waits on a steady_timer with a long expiry. When a message is enqueued, the timer is cancelled, waking the drain coroutine. This avoids polling.

**Important detail -- Handshake messages bypass the queue:** The handshake runs BEFORE `message_loop()` starts. The drain coroutine is started at the beginning of `message_loop()` (or at `run()` after handshake succeeds). Handshake messages use `send_raw()` (unencrypted) and `send_encrypted()` directly, which is safe because the message loop and drain coroutine aren't running yet. No changes to handshake code needed.

### Pattern 2: Awaitable Send with Backpressure

**What:** D-03 requires `send_message()` to remain awaitable. The caller enqueues and then `co_await`s a signal from the drain coroutine confirming the write completed (or failed). This preserves the existing contract where callers know if their send succeeded.

**Recommended approach:** Use a per-message `asio::steady_timer` as a completion signal (same pattern as sync_inbox). The drain coroutine cancels the timer after writing the message, waking the caller. Alternatively, use a `std::promise<bool>` / `std::future<bool>` but wrapped in asio-compatible signaling.

**Simpler alternative using co_await on the timer:**
```cpp
// Each pending message has its own completion timer
struct PendingMessage {
    std::vector<uint8_t> encoded;
    asio::steady_timer* completion;  // Owned by the enqueue_send coroutine's stack
    bool* result_ptr;               // Points to local variable in enqueue_send
};

asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    if (send_queue_.size() >= MAX_SEND_QUEUE) {
        close();
        co_return false;
    }
    bool result = false;
    asio::steady_timer completion(socket_.get_executor());
    completion.expires_after(std::chrono::hours(24));  // Will be cancelled by drain

    send_queue_.push_back({std::move(encoded), &completion, &result});
    send_signal_.cancel();  // Wake drain coroutine

    auto [ec] = co_await completion.async_wait(use_nothrow);
    co_return result;  // Set by drain coroutine before cancelling our timer
}
```

This is safe because: (1) the enqueue_send coroutine keeps `completion` and `result` alive on its stack until the co_await returns, (2) the drain coroutine sets `*result_ptr` before cancelling the timer, (3) both run on the same io_context thread so no data race on `result`.

### Pattern 3: Engine Callback for Unified Ingest Notification

**What:** `BlobEngine::ingest()` takes an optional `Connection::Ptr source` parameter. On successful ingest of a NEW blob (not duplicate), it fires an `on_blob_ingested` callback with namespace_id, blob_hash, seq_num, blob_size, is_tombstone, and source.

**Key change to engine.h:**
```cpp
using OnBlobIngested = std::function<void(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    net::Connection::Ptr source)>;  // nullptr for client writes

void set_on_blob_ingested(OnBlobIngested cb);

// Modified signature:
asio::awaitable<IngestResult> ingest(
    const wire::BlobData& blob,
    net::Connection::Ptr source = nullptr);
```

**Engine fires callback at the end of successful ingest (Status::Stored only):**
```cpp
case storage::StoreResult::Status::Stored: {
    WriteAck ack;
    // ... fill ack ...
    if (on_blob_ingested_) {
        on_blob_ingested_(blob.namespace_id, store_result.blob_hash,
                          store_result.seq_num,
                          static_cast<uint32_t>(blob.data.size()),
                          wire::is_tombstone(blob.data), source);
    }
    co_return IngestResult::success(std::move(ack));
}
```

### Pattern 4: PeerManager Unified Fan-Out

**What:** PeerManager registers a single callback on BlobEngine. This callback handles both BlobNotify (type 59) to all TCP peers and Notification (type 21) to subscribed clients.

**Source exclusion (PUSH-07, D-06/D-07):** The callback receives `source` Connection::Ptr. If non-null, skip that connection in the peer fan-out. This handles both:
- Sync-received blobs: source = the syncing peer's connection (skip BlobNotify back to that peer)
- Client writes: source = nullptr (notify ALL peers)

**Storm suppression (PUSH-03, D-06):** The D-06/D-07 decisions REFINE PUSH-03's requirement. Storm suppression means: don't send BlobNotify for a sync-received blob BACK to the peer that sent it. All other peers still get BlobNotify. This is naturally handled by source exclusion.

**Fan-out code:**
```cpp
void PeerManager::on_blob_ingested(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    net::Connection::Ptr source) {

    // Build notification payload once (77 bytes)
    auto payload = encode_notification(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);

    // BlobNotify (type 59) to all TCP peers except source
    for (auto& peer : peers_) {
        if (peer.connection == source) continue;  // Source exclusion
        if (peer.connection->is_uds()) continue;  // UDS = client, not peer

        auto conn = peer.connection;
        auto payload_copy = payload;
        asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
            co_await conn->send_message(wire::TransportMsgType_BlobNotify,
                                         std::span<const uint8_t>(p));
        }, asio::detached);
    }

    // Notification (type 21) to subscribed clients (existing pub/sub)
    for (auto& peer : peers_) {
        if (peer.subscribed_namespaces.count(namespace_id)) {
            auto conn = peer.connection;
            auto payload_copy = payload;
            asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
                co_await conn->send_message(wire::TransportMsgType_Notification,
                                             std::span<const uint8_t>(p));
            }, asio::detached);
        }
    }

    // Test hook
    if (on_notification_) {
        on_notification_(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
    }
}
```

**Note:** The `co_spawn(detached)` pattern is now SAFE because `send_message()` goes through the send queue, not directly to `send_encrypted()`.

### Pattern 5: Message Loop Pong Fix

**What:** The existing Pong reply in `message_loop()` calls `send_encrypted()` directly, bypassing any future send queue. This must be changed to use `send_message()` so it goes through the queue.

**Current (broken under concurrent sends):**
```cpp
case wire::TransportMsgType_Ping: {
    std::span<const uint8_t> empty{};
    auto pong = TransportCodec::encode(wire::TransportMsgType_Pong, empty);
    co_await send_encrypted(pong);  // BYPASSES QUEUE
    break;
}
```

**Fixed:**
```cpp
case wire::TransportMsgType_Ping: {
    std::span<const uint8_t> empty{};
    co_await send_message(wire::TransportMsgType_Pong, empty);  // THROUGH QUEUE
    break;
}
```

Similarly, `close_gracefully()` sends Goodbye via `send_encrypted()` -- this should also go through the queue (or drain the queue first then send directly, since close_gracefully should be the last message).

### Anti-Patterns to Avoid
- **DO NOT make send_encrypted() public or call it from outside Connection after this phase:** All sends must go through `send_message()` which routes through the queue.
- **DO NOT start the drain coroutine during handshake:** Handshake messages use `send_raw()` (unencrypted) and a few `send_encrypted()` calls for auth. The drain coroutine should start only when the message loop begins. Since handshake is sequential (no concurrent sends possible), this is safe.
- **DO NOT use a mutex instead of a queue:** An async mutex would still allow nonce ordering issues if two coroutines both acquire the mutex and send -- the wire order depends on scheduling. The queue guarantees FIFO order.
- **DO NOT fire the engine callback before storage commit:** The callback must fire AFTER `storage_.store_blob()` returns `Stored`, not before. The blob must be retrievable when peers try to fetch it.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Coroutine-safe async mutex | Custom lock primitive | Send queue with single drain coroutine | Queue guarantees strict FIFO ordering; mutex allows non-deterministic scheduling |
| Channel-based message passing | asio::experimental::channel wrapper | std::deque + timer-cancel wakeup | Proven pattern in codebase (sync_inbox); experimental API risk avoided |
| Custom notification payload format | New binary format | Existing `encode_notification()` 77-byte format | Already produces the exact payload needed for BlobNotify |

## Common Pitfalls

### Pitfall 1: Drain Coroutine Lifetime vs Connection Destruction
**What goes wrong:** The drain coroutine holds `shared_from_this()`. If the connection is closed and all external references are dropped, the drain coroutine keeps the Connection alive via its shared_ptr until it exits. If the drain coroutine is stuck waiting on the timer, the Connection leaks.
**Why it happens:** Timer-cancel pattern with long timeout means the coroutine can block indefinitely after close.
**How to avoid:** In `close()`, cancel the `send_signal_` timer. The drain coroutine checks `closed_` on wake and exits. Also set `closed_ = true` before cancelling to prevent race between cancel and check.
**Warning signs:** Connection objects not being destroyed after close (check with shared_ptr use_count in tests).

### Pitfall 2: Completion Timer Dangling Pointer
**What goes wrong:** `PendingMessage` stores a raw pointer to the caller's stack-local `completion` timer and `result` bool. If the caller is destroyed before the drain coroutine processes the message (e.g., connection closed while messages are queued), the drain coroutine writes through a dangling pointer.
**Why it happens:** The caller's coroutine may be destroyed when the connection closes (timer cancellation propagates).
**How to avoid:** When clearing the queue on close, do NOT write to result_ptr. Check `closed_` in the drain coroutine before writing through the pointer. Or use `shared_ptr<bool>` for the result.
**Warning signs:** ASAN use-after-free on send queue drain during connection teardown.

### Pitfall 3: Queue Full Disconnect During Active Sync
**What goes wrong:** A syncing peer that is slow to read falls behind on BlobNotify messages. The queue hits 1024 and the node disconnects the peer, interrupting an active sync session mid-transfer.
**Why it happens:** BlobNotify messages accumulate while sync blobs are being transferred (large payloads take time to write).
**How to avoid:** This is the intended behavior per D-04 (disconnect and let reconcile-on-connect in Phase 82 handle recovery). Log clearly so operators understand the disconnect reason. The cap of 1024 at 77 bytes each is conservative -- slow peers should be disconnected rather than buffered indefinitely.
**Warning signs:** Repeated connect-disconnect cycles in logs from a slow peer.

### Pitfall 4: Notify Subscribers Still Called Directly in Data/Delete Handlers
**What goes wrong:** After implementing the engine callback (D-10), the existing `notify_subscribers()` calls in the Data and Delete handlers (lines 1651-1658 and 692-699 in peer_manager.cpp) are still present. Notifications fire twice: once from the engine callback, once from the handler.
**Why it happens:** Incomplete refactoring -- the old call sites are not removed.
**How to avoid:** When wiring the engine callback, search for ALL existing `notify_subscribers()` call sites and remove them. There are three: (1) Data handler line 1653, (2) Delete handler line 693, (3) sync_proto callback in constructor line 158-162. All three must be removed.
**Warning signs:** Duplicate notifications in tests, double the expected notification count.

### Pitfall 5: Sync Protocol On-Blob-Ingested Callback Not Removed
**What goes wrong:** The existing `sync_proto_.set_on_blob_ingested()` callback (peer_manager.cpp line 158) fires `notify_subscribers()` for sync-ingested blobs. If the engine callback (D-08) is also active, sync-ingested blobs trigger notifications from both paths.
**Why it happens:** D-10 says "unify" but the old callback path in SyncProtocol is still wired.
**How to avoid:** Remove `sync_proto_.set_on_blob_ingested()` from PeerManager constructor. The SyncProtocol::ingest_blobs() call will need to pass the source connection to `engine_.ingest()` so the engine callback provides source exclusion. This may require SyncProtocol to take a source Connection::Ptr parameter.
**Warning signs:** BlobNotify sent back to the peer that provided the blob via sync (source exclusion fails because source is only set in the engine callback path, not the sync_proto callback path).

### Pitfall 6: Engine Callback Fires on IO Thread vs Pool Thread
**What goes wrong:** `BlobEngine::ingest()` offloads crypto to the thread pool. After pool work, the coroutine resumes on the pool thread (not the IO thread). If the callback fires at that point, PeerManager's `peers_` deque is accessed from the pool thread -- data race.
**Why it happens:** The existing Data handler uses `co_await asio::post(ioc_, asio::use_awaitable)` to transfer BACK to the IO thread before calling `notify_subscribers()`. The engine callback must also fire on the IO thread.
**How to avoid:** Fire the engine callback AFTER the store operation completes. The store operation (`storage_.store_blob()`) runs on the IO thread (storage is not offloaded). So if the callback is placed after store_result, it's already on the IO thread. BUT -- the ingest() function itself may be called from a co_spawn that resumed on the pool. The safest approach: fire the callback from within `ingest()` ONLY after the storage write, which happens on whichever thread called ingest(). The PeerManager Data/Delete handlers already do `co_await asio::post(ioc_)` before calling notify_subscribers, so the callback in the handlers is on the IO thread. For the sync path, SyncProtocol::ingest_blobs() runs on the IO thread already (it's dispatched via co_spawn on ioc_).

**Resolution:** The callback should fire from PeerManager code (after `co_await engine_.ingest()` returns and after `co_await asio::post(ioc_)` transfer), NOT from inside engine_.ingest() itself. This keeps the callback always on the IO thread. Engine provides the ingest result; PeerManager decides what to do with it. This is simpler than D-08's suggestion of firing from inside ingest(), but achieves the same "single hook point" goal by having PeerManager be the single place that calls the unified fan-out after any ingest.

**Alternative (per D-08 literally):** If the callback fires from inside ingest(), it must be documented that the callback may run on either the IO thread or the pool thread. PeerManager's callback would then need to `asio::post(ioc_, ...)` the actual fan-out. This adds complexity.

**Recommendation:** Keep the callback in PeerManager (the "after ingest" hook), not inside engine_.ingest(). A helper method `PeerManager::handle_ingest_result()` that takes the ingest result, blob metadata, and source connection, then does fan-out. Called from Data handler, Delete handler, and SyncProtocol path. This achieves D-08's goal (single hook point) without the thread-safety concern.

### Pitfall 7: close_gracefully() Sends Goodbye After Queue Drain
**What goes wrong:** `close_gracefully()` currently calls `send_encrypted()` directly to send Goodbye. With the send queue, Goodbye must either (a) go through the queue as the last message, or (b) drain the queue first then send directly.
**Why it happens:** Goodbye is special -- it must be the last message. If it goes through the queue while other messages are queued, it might not be last.
**How to avoid:** Enqueue Goodbye as a regular message via `send_message()`. Set a "closing" flag that prevents new messages from being enqueued after Goodbye. The drain coroutine processes Goodbye in FIFO order (after all pending messages), then closes.
**Warning signs:** Goodbye arrives before queued notifications; peer sees unexpected message after Goodbye.

## Code Examples

### Existing Pattern: Timer-Cancel Wakeup (from sync_inbox)
```cpp
// peer_manager.cpp -- recv_sync_msg() uses timer-cancel to wait for messages
asio::awaitable<std::optional<SyncMessage>> PeerManager::recv_sync_msg(
    PeerInfo* peer, std::chrono::seconds timeout) {
    if (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }
    // Wait for message to arrive (timer-cancel pattern)
    asio::steady_timer timer(ioc_);
    peer->sync_notify = &timer;
    timer.expires_after(timeout);
    auto [ec] = co_await timer.async_wait(use_nothrow);
    peer->sync_notify = nullptr;
    if (ec == asio::error::operation_aborted && !peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }
    co_return std::nullopt;
}
```

This is the exact same pattern the send queue drain coroutine should use. Message enqueued -> cancel timer -> drain coroutine wakes.

### Existing Pattern: Notification Encoding (from peer_manager.h)
```cpp
// peer_manager.h line 152-159 -- 77-byte notification payload
static std::vector<uint8_t> encode_notification(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t, 32> blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone);
```
Reuse this for BlobNotify payload. Same format, different TransportMsgType.

### Existing Pattern: Source Connection in Sync (from sync_protocol.h)
```cpp
// sync_protocol.h -- callback already carries namespace, hash, seq, size, tombstone
using OnBlobIngested = std::function<void(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_data_size,
    bool is_tombstone)>;
```
This callback does NOT have source connection. To implement D-09, either:
1. SyncProtocol::ingest_blobs() takes a `Connection::Ptr source` and passes it through
2. Or the callback is removed entirely and PeerManager's sync handler calls the unified fan-out directly

Option 2 is cleaner per D-10.

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Direct send_encrypted() from any coroutine | Queued send via drain coroutine | Phase 79 | Eliminates AEAD nonce desync permanently |
| notify_subscribers() from Data/Delete handlers + sync_proto callback | Unified engine callback with source exclusion | Phase 79 | Single notification path; impossible to miss an ingest |
| Timer-paced sync only | Push notification + safety-net timer | Phase 79 (wire types), Phase 80 (fetch) | Sub-second propagation latency |

## Open Questions

1. **Drain coroutine start timing**
   - What we know: Must start after handshake, before message_loop processes any messages
   - What's unclear: Whether to start it as part of `run()` (after handshake, co_spawned alongside message_loop) or lazily on first enqueue
   - Recommendation: Start in `run()` after successful handshake, co_spawned with `using namespace asio::experimental::awaitable_operators; co_await (message_loop() || drain_send_queue())`. Or start it via co_spawn from the ready callback. Starting eagerly is simpler and avoids edge cases with lazy initialization.

2. **close_gracefully() interaction with send queue**
   - What we know: Goodbye must be the last message on the wire
   - What's unclear: Whether to drain existing queue then send Goodbye, or enqueue Goodbye and prevent new messages
   - Recommendation: Enqueue Goodbye via send_message(), set a `closing_` flag that rejects new enqueues. Drain coroutine processes Goodbye in order then exits. The caller co_awaits the enqueue which completes when Goodbye is written.

3. **Where exactly does the engine callback fire**
   - What we know: D-08 says "engine fires callback" but thread safety requires IO thread execution
   - What's unclear: Inside engine_.ingest() vs in PeerManager after ingest returns
   - Recommendation: PeerManager helper method (see Pitfall 6). Achieves single hook point without thread safety issues.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3 |
| Config file | db/CMakeLists.txt (catch_discover_tests) |
| Quick run command | `cd build && ctest -R "peer" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PUSH-01 | BlobNotify sent to all peers on ingest | unit | `cd build && ctest -R "peer" --output-on-failure` | Exists (test_peer_manager.cpp) -- needs new test cases |
| PUSH-02 | 77-byte notification payload format | unit | `cd build && ctest -R "peer" --output-on-failure` | Exists -- encode_notification already tested |
| PUSH-03 | Storm suppression during sync | unit | `cd build && ctest -R "peer" --output-on-failure` | Needs new test case |
| PUSH-04 | Send queue serialization prevents nonce desync | unit | `cd build && ctest -R "connection" --output-on-failure` | Needs new test case in test_connection.cpp |
| PUSH-07 | Source exclusion (no BlobNotify back to originator) | unit | `cd build && ctest -R "peer" --output-on-failure` | Needs new test case |
| PUSH-08 | Only connected peers notified | unit | `cd build && ctest -R "peer" --output-on-failure` | Implicit -- fan-out iterates peers_ which is only connected |
| WIRE-01 | BlobNotify = 59 in TransportMsgType | unit | `cd build && ctest -R "codec" --output-on-failure` | Needs verification after FlatBuffer regen |
| WIRE-04 | Relay blocks type 59 | unit | `cd build && ctest -R "message_filter" --output-on-failure` | Exists (test_message_filter.cpp) -- needs new test case |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "peer\|connection\|message_filter\|codec" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before verification

### Wave 0 Gaps
- [ ] New test cases in `db/tests/net/test_connection.cpp` -- send queue serialization, queue full disconnect, drain on close
- [ ] New test cases in `db/tests/peer/test_peer_manager.cpp` -- BlobNotify fan-out, source exclusion, storm suppression, unified callback
- [ ] New test case in `db/tests/relay/test_message_filter.cpp` -- BlobNotify blocked

## Sources

### Primary (HIGH confidence)
- Direct codebase inspection: connection.h/cpp, engine.h/cpp, peer_manager.h/cpp, sync_protocol.h/cpp, transport.fbs, message_filter.cpp
- `.planning/research/SUMMARY.md` -- project-level v2.0.0 research (HIGH confidence, same-day)
- `.planning/research/PITFALLS.md` -- AEAD nonce desync analysis (Pitfall 1) and notification storm analysis (Pitfall 2)
- `.planning/research/STACK.md` -- Asio pattern analysis for push notifications and send queue

### Secondary (MEDIUM confidence)
- Asio 1.38.0 documentation -- steady_timer, co_spawn, awaitable_operators (verified present in codebase headers)
- Timer-cancel pattern -- proven across 8+ coroutine loops in peer_manager.cpp

### Tertiary (LOW confidence)
- None. All findings based on direct source code analysis.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- zero new deps, all patterns already used in codebase
- Architecture: HIGH -- based on line-by-line analysis of connection.cpp, engine.cpp, peer_manager.cpp
- Pitfalls: HIGH -- derived from known v1.0.0 PEX SIGSEGV history and AEAD nonce desync decisions across 78 phases

**Research date:** 2026-04-02
**Valid until:** 2026-05-02 (stable codebase, no external dependency changes)
