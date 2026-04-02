# Architecture Patterns

**Domain:** Event-driven sync integration for chromatindb v2.0.0
**Researched:** 2026-04-02
**Overall confidence:** HIGH (analysis based on direct codebase inspection, not external sources)

## Recommended Architecture

Replace timer-paced sync with push-based notifications and targeted fetch. The existing single-threaded Asio event loop, PeerManager, Connection, and Storage are preserved. Changes are surgical: new message types, new notification paths, modified timer loops, one new Storage API, and SDK reconnect logic.

### Architecture Overview

```
CLIENT WRITE                PEER SYNC (existing reconciliation)
     |                              |
     v                              v
  engine_.ingest()            sync_proto_.ingest_blobs()
     |                              |
     v                              v
  [WriteAck to client]        [SyncComplete to peer]
     |                              |
     +------> notify_all_peers() <--+  (NEW: replaces notify_subscribers scope)
                    |
          +---------+---------+
          |                   |
     [BlobNotify to     [BlobNotify to
      peer A]             peer B]
          |                   |
          v                   v
     on_peer_message     on_peer_message
     (BlobNotify)        (BlobNotify)
          |                   |
          v                   v
     targeted_fetch()    targeted_fetch()
     (BlobFetch for      (BlobFetch for
      single hash)        single hash)
```

### Key Architectural Principle

**Push notifications reuse the existing pub/sub fan-out pattern.** The current `notify_subscribers()` only notifies peers with explicit subscriptions. Push sync extends this: after ingest, notify ALL connected peers (not just subscribers). This is a broadening of scope, not a new mechanism. The existing `encode_notification()` wire format already contains all the metadata a peer needs to decide whether to fetch.

## Component Boundaries

### New Components

| Component | File | Responsibility | Communicates With |
|-----------|------|---------------|-------------------|
| `BlobNotify` message type | `transport.fbs` | Push notification to peers on ingest | PeerManager (sender), Connection (wire) |
| `BlobFetch` message type | `transport.fbs` | Targeted single-blob fetch request | PeerManager (handler), SyncProtocol (data) |
| `BlobFetchResponse` message type | `transport.fbs` | Single-blob response to BlobFetch | PeerManager (encoder), Connection (wire) |
| `next_expiry_time()` Storage API | `storage.h/.cpp` | Peek at earliest expiry key from MDBX | PeerManager (timer scheduling) |
| SDK auto-reconnect | `client.py` | Transparent reconnection on connection loss | Transport, Handshake |
| Connection keepalive | `peer_manager.cpp` | Bidirectional ping/pong heartbeat | Connection (send), PeerManager (dead detect) |

### Modified Components

| Component | File | What Changes | Why |
|-----------|------|-------------|-----|
| `PeerManager::notify_all_peers()` | `peer_manager.cpp` | New method: fan-out BlobNotify to ALL connected peers | Push sync |
| `PeerManager::on_peer_message()` | `peer_manager.cpp` | Handle BlobNotify + BlobFetch message types | Receive push notifications, serve targeted fetches |
| `PeerManager::on_peer_connected()` | `peer_manager.cpp` | Trigger full reconciliation on connect (instead of sync timer) | Reconcile-on-connect |
| `PeerManager::sync_timer_loop()` | `peer_manager.cpp` | Change interval from 60s to 600-900s (safety net only) | No longer primary sync mechanism |
| `PeerManager::expiry_scan_loop()` | `peer_manager.cpp` | Replace periodic scan with next-expiry timer scheduling | Event-driven expiry |
| `PeerManager::cursor_compaction_loop()` | `peer_manager.cpp` | Replace 6h timer with disconnect-triggered compaction | Immediate cleanup |
| `PeerManager::on_peer_disconnected()` | `peer_manager.cpp` | Add cursor compaction call | Disconnect-triggered cleanup |
| `Storage::run_expiry_scan()` | `storage.cpp` | Keep as-is (batch scan still used by next-expiry approach) | No change needed |
| `transport.fbs` | `db/schemas/` | Add BlobNotify=59, BlobFetch=60, BlobFetchResponse=61 | Wire protocol |
| `message_filter.cpp` | `relay/core/` | Block BlobNotify, BlobFetch, BlobFetchResponse (peer-internal) | Relay security |
| SDK `Transport._reader_loop()` | `_transport.py` | Detect connection loss, signal reconnect | Auto-reconnect |
| SDK `ChromatinClient` | `client.py` | Add reconnect loop, re-subscribe on reconnect | Transparent recovery |

### Unchanged Components

| Component | Why Unchanged |
|-----------|--------------|
| `Connection` class | Message loop already dispatches arbitrary types; no structural change |
| `SyncProtocol` class | Reconciliation algorithm unchanged; still used on-connect and safety-net |
| `Storage::store_blob()` | Already creates expiry index entries; no schema change needed |
| `BlobEngine` | Ingest pipeline unchanged; notification happens in PeerManager after ingest |
| `Reconciliation` | XOR-fingerprint algorithm unchanged; just invoked at different times |
| `Handshake` | No protocol changes to connection establishment |
| `Relay session forwarding` | Blocklist approach means new peer-internal types auto-blocked if added to blocklist |

## Data Flow: Push-Based Sync

### Flow 1: Client writes a blob (happy path)

```
1. Client sends Data message via relay
2. Relay forwards to node via UDS
3. PeerManager::on_peer_message(Data) spawns coroutine:
   a. engine_.ingest(blob) -> co_await (thread pool offload)
   b. co_await asio::post(ioc_) -> back on IO thread
   c. conn->send_message(WriteAck) -> ack to client
   d. notify_all_peers(namespace_id, blob_hash, seq_num, size, is_tombstone)
      -> for each peer in peers_:
         -> co_spawn: conn->send_message(BlobNotify, encoded_notification)
```

### Flow 2: Peer receives push notification

```
1. PeerManager::on_peer_message(BlobNotify):
   a. Decode notification: namespace_id, blob_hash, seq_num, size, is_tombstone
   b. Check: storage_.has_blob(namespace_id, blob_hash)?
      - YES: already have it, ignore
      - NO: fetch it
   c. Send BlobFetch request: [namespace_id:32][blob_hash:32]
   d. Wait for BlobFetchResponse (reuse sync message queue or inline response)
   e. engine_.ingest(blob) -> validate + store
   f. On success: notify_all_peers() -> cascade to other peers
```

### Flow 3: Reconciliation on connect

```
1. PeerManager::on_peer_connected() (initiator side):
   a. co_spawn: run_sync_with_peer(conn)  [EXISTING - no change]
2. Full reconciliation runs as today (Phase A/B/C with XOR fingerprints)
3. After reconciliation: PEX exchange [EXISTING - no change]
```

### Flow 4: Event-driven expiry

```
1. On startup / after each expiry scan / after blob ingest:
   a. Call storage_.next_expiry_time() -> peek expiry_map first key
   b. If has_expiry: schedule timer for (expiry_time - now)
   c. If no_expiry: no timer (wait for next ingest to re-check)
2. Timer fires:
   a. Call storage_.run_expiry_scan() -> purge all expired blobs
   b. Re-schedule: call next_expiry_time() again, set new timer
```

### Flow 5: SDK auto-reconnect

```
1. Transport._reader_loop() catches connection error:
   a. Sets _closed = True
   b. Cancels all pending futures with ConnectionError
   c. Signals reconnect event
2. ChromatinClient reconnect loop:
   a. Wait jittered backoff (1s, 2s, 4s, ... up to 60s)
   b. TCP connect + PQ handshake
   c. Create new Transport, start reader
   d. Re-subscribe to previously tracked namespaces
   e. Resume normal operation
```

### Flow 6: Connection keepalive

```
1. PeerManager sends Ping to all peers periodically (30s interval)
2. Connection::message_loop() handles Pong (already exists for client path)
3. Receiver-side inactivity timeout (EXISTING) catches dead peers
4. SDK responds to Ping with Pong (already exists in Transport._reader_loop())
```

## New Message Types

### BlobNotify (type = 59)

Push notification sent to ALL connected peers when a blob is successfully ingested (stored, not duplicate).

**Wire format:** Identical to existing Notification (type 21) payload:
```
[namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
```
Total: 77 bytes.

**Rationale for separate type vs reusing Notification:** Notification (type 21) is subscription-gated and client-facing. BlobNotify (type 59) is peer-internal and unconditional. Separate types allow:
- Relay to block BlobNotify (peer-internal) while allowing Notification (client-facing)
- Peers to handle them differently (BlobNotify triggers fetch; Notification is informational)
- No behavioral change to existing pub/sub subscription semantics

### BlobFetch (type = 60)

Targeted single-blob fetch request. Sent by a peer that received a BlobNotify for a blob it doesn't have.

**Wire format:**
```
[namespace_id:32][blob_hash:32]
```
Total: 64 bytes.

**Differences from existing BlobRequest (type 12):**
- BlobRequest is sync-protocol-only, gated by syncing state, and carries multiple hashes
- BlobFetch is standalone, requires no active sync session, and fetches exactly one blob
- BlobFetch is handled inline (no sync session needed), making it fast

### BlobFetchResponse (type = 61)

Response to BlobFetch. Contains the full FlatBuffer-encoded blob, or empty if not found.

**Wire format:**
```
[found:1][blob_flatbuffer:N]  (if found=1)
[found:1]                     (if found=0, total 1 byte)
```

**Why not reuse BlobTransfer (type 13):** BlobTransfer uses count-prefixed multi-blob encoding with length prefixes. BlobFetchResponse is simpler -- always exactly one blob (or not found). Different encoding avoids confusing sync state.

## Patterns to Follow

### Pattern 1: Timer-Cancel for Event-Driven Expiry

**What:** Replace the fixed-interval `expiry_scan_loop()` with a next-expiry-targeted timer.

**When:** Any time the next expiry time changes (blob ingest, expiry scan completion).

**Current code (to replace):**
```cpp
// Current: fixed interval, scans everything
asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        timer.expires_after(std::chrono::seconds(expiry_scan_interval_seconds_));
        co_await timer.async_wait(...);
        storage_.run_expiry_scan();
    }
}
```

**New code (sketch):**
```cpp
asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        auto next = storage_.next_expiry_time();
        if (!next.has_value()) {
            // No blobs with TTL -- wait for wake signal
            timer.expires_at(asio::steady_timer::time_point::max());
        } else {
            auto now = static_cast<uint64_t>(std::time(nullptr));
            auto delay = std::chrono::seconds(
                next.value() > now ? next.value() - now : 0);
            timer.expires_after(delay);
        }
        co_await timer.async_wait(...);
        if (stopping_) co_return;
        storage_.run_expiry_scan();
        // Loop re-peeks next_expiry_time on next iteration
    }
}
```

**Wake mechanism:** After `engine_.ingest()` stores a blob with TTL > 0, cancel the expiry timer (`expiry_timer_->cancel()`) to force re-evaluation. This reuses the existing timer-cancel pattern already used for SIGHUP and shutdown.

### Pattern 2: Notification Fan-Out (Broadened Scope)

**What:** Extend the existing `notify_subscribers()` pattern to notify ALL peers.

**Current:** `notify_subscribers()` iterates `peers_` and sends only to those with matching `subscribed_namespaces`.

**New:** `notify_all_peers()` iterates `peers_` and sends BlobNotify to ALL peers (no subscription check). This is the same pattern but with the filter removed.

```cpp
void PeerManager::notify_all_peers(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    net::Connection::Ptr source = nullptr) {
    auto payload = encode_notification(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
    for (auto& peer : peers_) {
        if (peer.connection == source) continue;  // Don't notify source
        if (peer.connection->is_uds()) continue;  // Skip client connections
        auto conn = peer.connection;
        auto payload_copy = payload;
        asio::co_spawn(ioc_, [conn, payload_copy = std::move(payload_copy)]() -> asio::awaitable<void> {
            co_await conn->send_message(
                wire::TransportMsgType_BlobNotify,
                std::span<const uint8_t>(payload_copy));
        }, asio::detached);
    }
    // Also fire existing subscriber notifications for client-facing pub/sub
    notify_subscribers(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
}
```

**Critical:** Must skip UDS connections. Push sync is peer-to-peer; clients use pub/sub subscriptions. Also must still call `notify_subscribers()` for backward-compatible client notifications.

### Pattern 3: Inline BlobFetch Handling (No Sync Session)

**What:** Handle BlobFetch in the message dispatch without requiring a sync session.

**Current model:** BlobRequest (type 12) is only processed within an active sync session (routed through `route_sync_message()` to `handle_sync_as_responder()`).

**New model:** BlobFetch is handled inline in `on_peer_message()`, like ReadRequest or ExistsRequest. No sync session needed. This is the key performance win -- no SyncRequest/SyncAccept handshake, just request/respond.

```cpp
if (type == wire::TransportMsgType_BlobFetch) {
    asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
        if (payload.size() != 64) { record_strike(conn, "bad BlobFetch"); co_return; }
        std::array<uint8_t, 32> ns, hash;
        std::memcpy(ns.data(), payload.data(), 32);
        std::memcpy(hash.data(), payload.data() + 32, 32);
        auto blob = engine_.get_blob(ns, hash);
        co_await asio::post(ioc_, asio::use_awaitable);
        if (blob.has_value()) {
            auto encoded = wire::encode_blob(blob.value());
            std::vector<uint8_t> response(1 + encoded.size());
            response[0] = 1; // found
            std::memcpy(response.data() + 1, encoded.data(), encoded.size());
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse, response, request_id);
        } else {
            uint8_t not_found = 0;
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse, {&not_found, 1}, request_id);
        }
    }, asio::detached);
    return;
}
```

**Dispatch model:** coroutine-offload-transfer (same as Data/Delete handlers). `engine_.get_blob()` reads from MDBX. Must post back to IO thread before send_message (AEAD nonce safety).

### Pattern 4: Disconnect-Triggered Cursor Compaction

**What:** Move cursor cleanup from periodic timer to disconnect event.

**Current:** 6-hour timer, scans all cursor peers, prunes disconnected ones.

**New:** On `on_peer_disconnected()`, immediately clean up cursors for the disconnecting peer.

```cpp
void PeerManager::on_peer_disconnected(net::Connection::Ptr conn) {
    // Existing: remove from peers_, log, metrics
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    // ... existing peer removal logic ...

    // Immediate cursor cleanup for disconnected peer
    auto removed = storage_.delete_peer_cursors(peer_hash);
    if (removed > 0) {
        spdlog::debug("cursor cleanup: removed {} entries for disconnected peer", removed);
    }
}
```

**Remove:** The `cursor_compaction_loop()` coroutine and `cursor_compaction_timer_` member. No longer needed.

### Pattern 5: SDK Reconnect with State Preservation

**What:** Auto-reconnect in ChromatinClient that preserves subscription state.

**Design:**

```python
class ChromatinClient:
    def __init__(self, ...):
        self._subscriptions: set[bytes] = set()  # EXISTING
        self._auto_reconnect = True               # NEW
        self._reconnect_task: asyncio.Task | None = None  # NEW
        self._max_reconnect_delay = 60.0           # NEW
        self._intentional_close = False            # NEW

    async def _reconnect_loop(self):
        delay = 1.0
        while self._auto_reconnect and not self._intentional_close:
            jitter = random.uniform(0, delay * 0.1)
            await asyncio.sleep(delay + jitter)
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self._host, self._port, ...),
                    timeout=self._timeout)
                result = await asyncio.wait_for(
                    perform_handshake(reader, writer, self._identity),
                    timeout=self._timeout)
                self._transport = Transport(reader, writer, *result[:4])
                self._transport.start()
                if self._subscriptions:
                    payload = encode_subscribe(list(self._subscriptions))
                    await self._transport.send_message(TransportMsgType.Subscribe, payload)
                return  # Success
            except Exception:
                delay = min(delay * 2, self._max_reconnect_delay)
```

**Key decisions:**
- Subscriptions tracked as `set[bytes]` (already exists) -- re-issued on reconnect
- Pending requests at disconnect time get ConnectionError (existing behavior in `_cancel_all_pending`)
- New requests during reconnect raise ConnectionError (existing `_closed` check)
- Intentional close (`__aexit__` / `goodbye()`) sets `_intentional_close = True` to stop reconnect loop

### Pattern 6: Server-Initiated Keepalive (Ping From Node)

**What:** Node sends Ping to all connected peers periodically.

**Current:** Inactivity timeout is receiver-side only (check `last_message_time`). No active keepalive from the node. Client SDK can `send_ping()` but the node never initiates Ping to peers.

**Concern:** Adding node-initiated Ping introduces bidirectional keepalive messages. The project memory states "Receiver-side inactivity (not Ping sender) avoids AEAD nonce desync from bidirectional keepalive messages." This was the v0.9.0 design decision.

**Resolution:** The nonce desync issue was about concurrent writes on a single connection (two coroutines writing Ping + sync data simultaneously). In the current architecture, all send_message calls happen on the IO thread (via `co_await asio::post(ioc_)`). The co_spawn-on-ioc model serializes writes. The original concern was valid when sends could happen from multiple threads -- that is no longer the case. Each co_spawn'd coroutine yields at co_await, allowing other coroutines to run, but never concurrently on the same connection. The send_counter_ is accessed only from the IO thread, so there is no race.

**Implementation:** Add a `keepalive_loop()` coroutine in PeerManager:

```cpp
asio::awaitable<void> PeerManager::keepalive_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        keepalive_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        keepalive_timer_ = nullptr;
        if (ec || stopping_) co_return;
        for (auto& peer : peers_) {
            if (peer.connection->is_uds()) continue;
            auto conn = peer.connection;
            asio::co_spawn(ioc_, [conn]() -> asio::awaitable<void> {
                std::span<const uint8_t> empty{};
                co_await conn->send_message(wire::TransportMsgType_Ping, empty);
            }, asio::detached);
        }
    }
}
```

**Effect:** With node sending Ping every 30s, the receiver-side inactivity timeout (default 120s) on the REMOTE node will detect dead connections within 120s instead of waiting for sync timer or PEX. This gives faster dead peer detection without changing the existing inactivity mechanism.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Fetch Loop on BlobNotify

**What:** Receiving a BlobNotify and immediately sending BlobFetch without checking local storage.

**Why bad:** Leads to redundant fetches. If three peers all notify us about the same blob, we'd fetch it three times. Only the first succeeds; the other two are wasted bandwidth.

**Instead:** Always check `storage_.has_blob()` before sending BlobFetch. This is an O(1) MDBX key lookup (no data read) -- the exact same `has_blob()` used by ExistsRequest.

### Anti-Pattern 2: Cascading BlobNotify Storm

**What:** When a fetched blob is ingested, calling `notify_all_peers()` which notifies the peer we just fetched from.

**Why bad:** The source peer already has the blob. Sending BlobNotify back to it creates a pointless round-trip (they'll check has_blob() and ignore, but it wastes bandwidth).

**Instead:** In `notify_all_peers()`, accept an optional source connection pointer and skip it in the fan-out loop.

### Anti-Pattern 3: Expiry Timer Precision Obsession

**What:** Trying to fire expiry exactly at the millisecond a blob expires.

**Why bad:** Over-engineering. The expiry_map is already keyed by [expiry_ts_be:8][hash:32], so the first key is the earliest expiry. But blobs with the same second-resolution timestamp would all fire together anyway.

**Instead:** Round to the nearest second (which is what we already have -- timestamps are seconds). Fire the timer, run a batch scan that catches everything expired up to `now`. Simple, efficient, correct.

### Anti-Pattern 4: Reconnect Without Backoff

**What:** SDK reconnecting immediately and repeatedly on connection loss.

**Why bad:** If the relay is down, tight reconnect loops waste CPU, battery, and can trigger rate limiting when the relay comes back up.

**Instead:** Jittered exponential backoff (1s base, 2x growth, 60s cap, 10% jitter). This matches the existing C++ node reconnect design (auto-reconnect with jittered exponential backoff, 1s-60s, validated in v0.9.0).

### Anti-Pattern 5: Holding MDBX Write Transaction During Network IO

**What:** Opening an MDBX write transaction, then co_await-ing a network send before committing.

**Why bad:** MDBX write transactions block all other writes. If the network send blocks (slow peer), all blob storage is stalled.

**Instead:** Complete the MDBX transaction first, THEN do network IO. This is already the pattern used everywhere (ingest -> post to IO thread -> send), but must be maintained for the new BlobFetch handler.

### Anti-Pattern 6: BlobNotify During Active Sync

**What:** Processing BlobNotify messages from a peer while a reconciliation session with that peer is in progress.

**Why bad:** Could lead to duplicate fetches (reconciliation discovers the same blob the push notification told us about). Wastes bandwidth and creates confusing log output.

**Instead:** When `peer->syncing == true`, silently drop BlobNotify from that peer. Reconciliation will handle catch-up. This is the same pattern used for SyncRequest (silently dropped when peer is syncing, to avoid AEAD nonce desync).

## MDBX Schema Changes for Event-Driven Expiry

### No Schema Changes Required

The existing expiry sub-database already has the perfect structure for event-driven expiry:

```
expiry_map key:   [expiry_ts_be:8][blob_hash:32]  (40 bytes)
expiry_map value: [namespace_id:32]                 (32 bytes)
```

Keys are sorted by `expiry_ts_be` (big-endian) then `blob_hash`. This means:
- **Cursor to first = earliest expiry.** `cursor.to_first()` gives the blob that expires soonest. This is O(1) -- MDBX B-tree lookup.
- **No secondary index needed.** The expiry_map IS the index, already sorted by time.
- **Batch scan works unchanged.** `run_expiry_scan()` walks from first until `expiry_ts > now`.

### New Storage API: next_expiry_time()

```cpp
/// Peek at the earliest expiry timestamp in the expiry index.
/// O(1) B-tree lookup (cursor to first key, read first 8 bytes).
/// @return Expiry timestamp in seconds, or nullopt if no blobs have TTL.
std::optional<uint64_t> Storage::next_expiry_time();
```

Implementation:

```cpp
std::optional<uint64_t> Storage::next_expiry_time() {
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->expiry_map);
        auto first = cursor.to_first(false);
        if (!first.done) return std::nullopt;  // Empty expiry map
        auto key = cursor.current(false).key;
        if (key.length() < 8) return std::nullopt;
        return decode_be_u64(static_cast<const uint8_t*>(key.data()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
```

This is the only Storage change. No new sub-databases, no schema migration, no index rebuild.

## Notification Trigger Points

Blob ingestion can happen through three existing paths plus one new path. All must trigger `notify_all_peers()`:

| Path | Current Notification | New Notification |
|------|---------------------|------------------|
| Client Data (via relay) | `notify_subscribers()` in Data handler (line ~1653) | Replace with `notify_all_peers()` which also calls `notify_subscribers()` |
| Client Delete (via relay) | `notify_subscribers()` in Delete handler (line ~693) | Replace with `notify_all_peers()` which also calls `notify_subscribers()` |
| Sync ingest (peer-to-peer) | `on_blob_ingested_` callback -> `notify_subscribers()` (line ~161) | Replace with `notify_all_peers()` with source exclusion |
| BlobFetch response ingest | N/A (new path) | `notify_all_peers()` after ingest, with source exclusion |

The `on_blob_ingested_` callback in SyncProtocol (set up at PeerManager constructor, line 159-162) already fires `notify_subscribers()`. Changing this to call `notify_all_peers(source_conn)` ensures sync-received blobs propagate via push to other peers, excluding the peer we received them from.

## FlatBuffer Schema Changes

Add three new message types to `db/schemas/transport.fbs`:

```flatbuffers
// Phase 79: Push-based sync
BlobNotify = 59,
BlobFetch = 60,
BlobFetchResponse = 61,
```

These are peer-internal types. The relay message filter must block all three (add to the switch block in `message_filter.cpp`).

The relay's `default: return true;` pattern means all other new types pass through, but these three MUST be explicitly blocked since they carry peer-internal sync data.

## Safety Net Reconciliation

The existing `sync_timer_loop()` becomes a safety net. Changes:
- **Interval:** from `sync_interval_seconds` (default 60s) to a longer interval (600-900s configurable)
- **Purpose:** from "primary sync mechanism" to "monitoring signal + catch-up for missed pushes"
- **Behavior:** unchanged (full reconciliation via XOR-fingerprint algorithm)

Consider: add a new config field `safety_net_interval_seconds` (default 600) and deprecate using `sync_interval_seconds` for this purpose. Or just increase the default and document it.

## Suggested Build Order

Dependencies flow downward. Each phase depends on those above it.

### Phase 1: Push Notification Infrastructure (Foundation)

**Scope:**
1. Add BlobNotify/BlobFetch/BlobFetchResponse to transport.fbs, regenerate flatc headers
2. Add `notify_all_peers()` method to PeerManager (with source exclusion and UDS skip)
3. Wire notify_all_peers() into all three existing ingest paths (client Data, client Delete, sync ingest callback)
4. Update relay message filter to block three new types
5. Update NodeInfoResponse supported_types

**Why first:** Everything else depends on the new message types existing. This phase is purely additive -- no existing behavior changes. Push notifications go out, but no peer acts on them yet.

**Test strategy:** Verify BlobNotify is sent to all peers after client write. Verify relay blocks new types. Verify existing pub/sub notifications still work.

### Phase 2: Targeted Blob Fetch (Complete the Push Loop)

**Scope:**
1. BlobFetch handler in on_peer_message() (inline coroutine, no sync session)
2. BlobFetchResponse handler in on_peer_message() (decode, ingest, cascade)
3. BlobNotify handler: check has_blob(), send BlobFetch if missing, receive response, ingest
4. Dedup guard: skip BlobNotify from syncing peers
5. Dedup guard: has_blob() check before fetch

**Why second:** Depends on Phase 1 message types. This completes the push sync loop: notify -> check -> fetch -> ingest -> cascade.

**Test strategy:** Two-node: write blob on A, verify B receives via push (not timer sync). Three-node: write on A, verify B gets it via push, C gets it via cascade from B.

### Phase 3: Event-Driven Expiry

**Scope:**
1. Add `Storage::next_expiry_time()` method
2. Rewrite `expiry_scan_loop()` to use next-expiry timer instead of fixed interval
3. Add expiry timer wake on blob ingest (cancel timer when new blob has TTL)
4. Deprecate or repurpose `expiry_scan_interval_seconds` config

**Why third:** Independent of push sync. No dependencies on Phase 1 or 2. Can technically be built in parallel.

**Test strategy:** Ingest blob with TTL=5. Verify expiry fires within 1-2s of expiry time (not on 60s scan). Ingest blob with TTL=0 (permanent). Verify no timer scheduled when no TTL blobs exist.

### Phase 4: Reconcile-on-Connect + Safety Net

**Scope:**
1. Verify on_peer_connected() already triggers reconciliation for initiator (it does)
2. Change sync_timer_loop() interval to configurable safety-net value (default 600s)
3. Add config field for safety net interval
4. Disconnect-triggered cursor compaction (add to on_peer_disconnected())
5. Remove cursor_compaction_loop() and cursor_compaction_timer_ member

**Why fourth:** Depends on push sync working (Phase 2). Once push sync handles ongoing propagation, the sync timer becomes redundant for normal operation.

**Test strategy:** Connect two nodes, verify reconciliation runs immediately. Disconnect, write on A, reconnect, verify B catches up. Verify cursor cleaned on disconnect. Verify safety-net reconciliation fires after 600s.

### Phase 5: Connection Keepalive

**Scope:**
1. Add `keepalive_loop()` coroutine to PeerManager
2. Add `keepalive_timer_` member, wire into cancel_all_timers() and stop()
3. Send Ping to all TCP peers every 30s
4. Verify existing inactivity detection catches dead peers faster with keepalive traffic

**Why fifth:** Independent of push sync but enhances reliability. The existing receiver-side inactivity timeout already exists; keepalive just ensures regular traffic flow.

**Test strategy:** Kill node A's network interface. Verify node B detects dead connection within 120s (inactivity timeout). Measure detection time with vs without keepalive.

### Phase 6: SDK Auto-Reconnect

**Scope:**
1. Add reconnect loop to ChromatinClient (jittered exponential backoff)
2. Track subscriptions for re-subscribe on reconnect (set already exists)
3. Handle connection loss signal from Transport to Client
4. Distinguish intentional close from connection loss
5. Test with relay restart scenario

**Why sixth:** Depends on nothing on the C++ side. Building it last means the server-side architecture is stable and tested.

**Test strategy:** Connect SDK client. Kill relay. Verify auto-reconnect with backoff. Verify subscriptions restored after reconnect. Verify pending requests fail with ConnectionError.

### Phase 7: Documentation Refresh

**Scope:**
1. PROTOCOL.md: new message types (BlobNotify, BlobFetch, BlobFetchResponse), updated sync model description
2. README.md: updated architecture description, new sync behavior
3. SDK README: auto-reconnect API
4. SDK getting-started tutorial: reconnect handling

**Why last:** Documents the final state after all behavior is implemented and tested.

## Phase Dependency Graph

```
Phase 1 (Push Infra) -----> Phase 2 (Blob Fetch) -----> Phase 4 (Reconcile + Safety Net)

Phase 3 (Event Expiry)  [independent, can parallel with 1-2]

Phase 5 (Keepalive)      [independent, can parallel with 3-4]

Phase 6 (SDK Reconnect)  [independent, can parallel with 3-5]

Phase 7 (Docs)           [depends on all above]
```

**Parallelizable groups:**
- Group A: Phase 1, Phase 3 (no dependency between them)
- Group B: Phase 2 (needs Phase 1), Phase 5 (independent)
- Group C: Phase 4 (needs Phase 2), Phase 6 (independent)
- Group D: Phase 7 (needs all)

## Scalability Considerations

| Concern | At 3 nodes (current KVM) | At 10 nodes | At 100 nodes |
|---------|--------------------------|-------------|--------------|
| BlobNotify fan-out | 2 messages per ingest | 9 messages | 99 messages -- acceptable for small blobs |
| BlobFetch after notify | At most 2 fetches | At most 9 | 99 concurrent fetches of same blob from source |
| has_blob() dedup check | Trivial | Trivial | 99 MDBX reads -- still O(1) each |
| Notification bandwidth | 77 bytes x 2 = 154B overhead | 77 x 9 = 693B | 77 x 99 = 7.6 KB per ingest |
| Expiry timer precision | Exact (O(1) MDBX peek) | Exact | Exact |
| Cursor storage on disconnect | ~2 peers x N ns | ~10 peers | Delete large cursor sets on disconnect |

**At 100 nodes (future concern, not current):** The fan-out of 99 BlobFetch requests to a single source node could be a bottleneck for large blobs. Mitigation options for the future: fetch from any peer that has it (gossip-style), or batch-acknowledge multiple BlobNotify before fetching. Not needed for the 3-node KVM deployment.

## Sources

All analysis based on direct codebase inspection:
- `db/peer/peer_manager.h` / `.cpp` -- PeerManager, sync, expiry, notification, cursor compaction
- `db/storage/storage.h` / `.cpp` -- MDBX schema (7 sub-databases), expiry_map key format, run_expiry_scan()
- `db/engine/engine.h` / `.cpp` -- ingest pipeline, get_blob()
- `db/sync/sync_protocol.h` / `.cpp` -- reconciliation, on_blob_ingested callback
- `db/sync/reconciliation.h` / `.cpp` -- XOR-fingerprint algorithm
- `db/net/connection.h` / `.cpp` -- message loop, send_message, Connection::Ptr
- `db/wire/codec.h` -- blob encoding/decoding, tombstone/delegation utilities
- `db/schemas/transport.fbs` -- current 58 message types (None through TimeRangeResponse)
- `db/config/config.h` -- timer configuration, all SIGHUP-reloadable fields
- `relay/core/message_filter.h` / `.cpp` -- blocklist approach, 21 blocked types
- `sdk/python/chromatindb/_transport.py` -- background reader loop, notification queue, send_lock
- `sdk/python/chromatindb/client.py` -- ChromatinClient lifecycle, subscription tracking, connect()
