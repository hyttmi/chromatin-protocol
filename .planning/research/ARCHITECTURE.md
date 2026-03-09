# Architecture Patterns: v0.4.0 Production Readiness

**Domain:** Production hardening of an existing chromatindb daemon
**Researched:** 2026-03-08
**Confidence:** HIGH — All findings based on direct codebase inspection of 14,152 LOC across all headers
and key implementation files.

---

## Integration Overview

This milestone adds 8 features to an already-complete system. None require architectural changes to
the three-layer model (chromatindb / Relay / Client). All integrate within the existing component
boundary. `peer_manager.cpp` is the central integration point for most features; `storage.cpp` and
`engine.h` handle storage-side concerns.

The key constraint: no new subsystem-level abstractions are needed. All features slot into existing
component slots or extend them with new methods/fields.

---

## Existing Component Map

```
main.cpp
  |
  +-- Storage          (db/storage/storage.{h,cpp})   — libmdbx, 4 sub-databases
  |                     blobs_map, seq_map, expiry_map, delegation_map
  |
  +-- BlobEngine       (db/engine/engine.{h,cpp})     — ingest pipeline, fail-fast validation
  |
  +-- AccessControl    (db/acl/access_control.{h,cpp})— open/closed mode, allowed_keys
  |
  +-- PeerManager      (db/peer/peer_manager.{h,cpp}) — central orchestrator
       |
       +-- Server      (db/net/server.{h,cpp})          — TCP accept/connect, SIGINT/SIGTERM
       |    +-- Connection (db/net/connection.{h,cpp})  — handshake, AEAD IO, heartbeat
       |
       +-- SyncProtocol (db/sync/sync_protocol.{h,cpp}) — hash-list diff, blob transfer
       |
       +-- [sighup_loop, pex_timer_loop, sync_timer_loop] — coroutines in peer_manager.cpp
```

**Write data flow:** Connection -> PeerManager message router -> BlobEngine.ingest -> Storage.store_blob

**Key state:** `PeerInfo` (in peer_manager.h) is the per-connection state bag. `Storage::Impl` is the
internal libmdbx handle struct (pimpl). Both are the primary modification targets in this milestone.

---

## Feature-by-Feature Integration Analysis

### 1. Tombstone Index (O(n) -> O(1) has_tombstone_for)

**Current problem:** `Storage::has_tombstone_for` scans all blobs in a namespace linearly, decoding
each blob to check if it is a tombstone for the target hash. Called on every ingest to prevent
resurrection of deleted blobs. O(n) in namespace size.

**Integration point:** `Storage::Impl` (pimpl struct in storage.cpp).

**Solution:** Fifth sub-database `tombstone_map`, indexed by `[namespace:32][target_blob_hash:32]`.

**Why this structure:** Matches the existing `delegation_map` pattern exactly — same composite key
encoding, same population location (inside `store_blob`), same lookup replacement pattern. Delegations
are indexed by `[namespace:32][delegate_pk_hash:32]`; tombstones follow the same form with target hash.

**Modified components:**
- `Storage::Impl` — open `tombstone_map` in constructor write transaction alongside delegation_map.
  Increment `operate_params.max_maps` from 5 to 6.
- `Storage::store_blob` — when `wire::is_tombstone(blob.data)`, extract target hash via
  `wire::extract_tombstone_target(blob.data)` and upsert `[namespace:32][target_hash:32] -> [tombstone_blob_hash:32]`
  into tombstone_map inside the existing write transaction (zero extra transactions).
- `Storage::has_tombstone_for` — replace cursor scan with single btree lookup: build the 64-byte key,
  call `txn.get(tombstone_map, key, sentinel)`, return `val.data() != nullptr`.

**Zero protocol changes, zero API surface changes.** The `has_tombstone_for` signature is unchanged.

**Build order:** First. All other features depend on correct tombstone behavior. Smallest, most isolated
change in this milestone — one new sub-database, one additional upsert in store_blob, one lookup
replacement. Zero coroutine changes, zero networking changes.

---

### 2. Global Storage Limits

**Current state:** `Storage::Impl` configures libmdbx geometry with `size_upper = 64 GiB`. No
application-level byte tracking. `StoreResult` has no capacity concept. `BlobEngine::ingest` has no
storage-full rejection path.

**Integration points:** `Storage` (new method) + `BlobEngine::ingest` (new Step 0 check) + `Config`
(new field).

**Measuring used storage:** Two approaches exist:

Option A — Track bytes in an in-memory counter, initialized from libmdbx stats at startup.
Option B — Query libmdbx `env.stat()` per-map for leaf/overflow page counts and multiply by page size.

Recommendation: Option B. `env.stat()` is accurate after crashes and restarts because it reads actual
on-disk state. libmdbx exposes `env.info().mi_geo.current` (current mapped file size) which is an
upper bound, or per-map `stat.ms_leaf_pages + stat.ms_overflow_pages` multiplied by `stat.ms_psize`
for a tighter estimate of actual stored bytes. A new `Storage::used_bytes()` method encapsulates this
with a read transaction.

**New `StorageStats` struct:**
```cpp
struct StorageStats {
    uint64_t used_bytes = 0;
    uint64_t max_bytes = 0;  // from config, 0 = unlimited
    uint64_t blob_count = 0; // from seq_map entry count
};
```

**Modified components:**
- `Config` — add `uint64_t max_storage_bytes = 0` (0 = unlimited, backwards compatible).
- `StoreResult::Status` — add `StorageFull` variant.
- `IngestError` — add `storage_full` error code.
- `BlobEngine` — take `uint64_t max_storage_bytes` in constructor or via setter. Add Step 0 capacity
  check: `if (max_storage_bytes > 0 && storage.used_bytes() >= max_storage_bytes)` return rejection.
  This check belongs in BlobEngine before namespace derivation, following the existing Step 0 pattern
  (integer comparison before SHA3-256 computation).
- `Storage` — add `StorageStats storage_stats()` and `uint64_t used_bytes()` methods.

**Step 0 ordering (updated):** structural checks -> capacity check -> namespace derivation -> signature.
Capacity check is a single libmdbx stat read (cheap) vs. SHA3-256 (expensive).

**Build order:** Second. Disk-full reporting (feature 3) depends on the `storage_full` error code.

---

### 3. Disk-Full Reporting to Peers

**Current state:** Ingest rejections return `IngestError` variants. The `Data` message handler in
`PeerManager::on_peer_message` calls `engine_.ingest()` but there is no wire protocol path to inform
the sender that the node is full. The connection continues silently.

**Integration points:** `wire/transport.fbs` (new message type) + `PeerManager::on_peer_message`.

**New wire message:** `StorageFull = 23` — sent in response to a `Data` message when local storage
is at capacity. Payload: `[used_bytes:u64be][max_bytes:u64be]` (16 bytes, informational). This lets
the sender know the rejection is permanent until space is freed, and allows logging useful diagnostics.

**Modified components:**
- `wire/transport.fbs` — add `StorageFull = 23` to the `TransportMsgType` enum. Regenerate the
  FlatBuffers header. This is additive — old nodes receiving this message type log and skip it in
  `on_peer_message` (unknown type handling already exists).
- `PeerManager::on_peer_message` on `Data` type — when `engine_.ingest()` returns `storage_full`,
  encode and send a `StorageFull` response before returning. Do NOT close the connection — the peer
  may still be a valid sync partner for reads.
- `PeerInfo` — add `bool peer_is_full = false`. Set when a `StorageFull` message is received from
  that peer. `run_sync_with_peer` skips sending blobs to full peers (pointless round-trips).

**Receiving side in PeerManager:**
- Add `StorageFull` case to `on_peer_message` — set `peer->peer_is_full = true`, log warning.
- Clear `peer_is_full` on next successful write ACK from that peer (not needed in v0.4.0 scope, but
  the flag persists per-session, which is correct: if a peer is full, assume it stays full this session).

**Build order:** Third, after storage limits (needs `storage_full` error code and `used_bytes()` method).

---

### 4. Persistent Peer List

**Current state (already largely implemented):** `PeerManager` has `load_persisted_peers()`,
`save_persisted_peers()`, `update_persisted_peer()`, `PersistedPeer` struct, `persisted_peers_` member,
`MAX_PERSISTED_PEERS = 100`, `MAX_PERSIST_FAILURES = 3`, and `peers_file_path()`. The JSON file format
is implemented. `load_persisted_peers()` is called in `PeerManager::start()` and bootstrap peers from
the file are connected. This is substantially complete from v3.0.

**Gap to close:** `save_persisted_peers()` must be called at shutdown and after successful connections.
The shutdown wiring is the missing piece. Currently, `PeerManager::stop()` does not call it, and if
the process receives SIGINT, the peer list never gets saved to disk.

**Modified components:**
- `PeerManager::stop()` — call `save_persisted_peers()` before initiating drain. This must happen
  first because after drain starts, peers_ may be modified by disconnect callbacks.
- `PeerManager::on_peer_disconnected()` — verify whether `update_persisted_peer` is already being
  called here for non-bootstrap peers. If not, add it. This keeps the file current across the session.

**Build order:** Fourth, or done in conjunction with graceful shutdown (they share `stop()`).

---

### 5. Graceful Shutdown

**Current state:** `Server::stop()` sets `draining_ = true`, cancels the TCP acceptor, spawns a
`drain(std::chrono::seconds(5))` coroutine that calls `close_gracefully()` on all connections. Signal
handling: SIGINT/SIGTERM are caught by `Server::signals_` and call `server.stop()`. A second signal
forces `std::_Exit(1)`. The `drain()` coroutine sends `Goodbye` to each peer and waits.

**Gaps:**
1. `PeerManager::stop()` is never called from the signal path or from `main.cpp`. In-flight sync
   coroutines (`run_sync_with_peer`, `handle_sync_as_responder`) are running inside PeerManager and
   will be aborted without setting `stopping_ = true`. They may access `peers_` as it drains, risking
   use-after-free if a connection closes while a sync coroutine iterates it.
2. `save_persisted_peers()` is not called before exit (see feature 4).
3. The expiry scan coroutine in `main.cpp` is spawned as `asio::detached` and never cancelled. It will
   attempt to fire every 60 seconds. After the Server stops and all connections close, `ioc.run()`
   should still drain this timer — but it prevents immediate clean exit.

**Integration point:** `main.cpp::cmd_run()` and `PeerManager::stop()`.

**Required changes:**
- `Server::stop()` should call a registered shutdown callback instead of (or in addition to) calling
  drain directly. Simplest: `Server` fires `on_shutdown_started_` callback, PeerManager registers it
  and calls `pm.stop()` first, then `server_.stop()`. Alternatively: make `PeerManager::stop()` call
  `server_.stop()` as its last action (PeerManager already owns server_).
- `PeerManager::stop()` sequence: (a) set `stopping_ = true`, (b) call `save_persisted_peers()`,
  (c) cancel sync timer, PEX timer, SIGHUP signal set, (d) call `server_.stop()` (starts drain).
- The expiry scan coroutine in `main.cpp` needs a cancellation path. Add a `asio::cancellation_signal`
  or, simpler, check a shared `std::atomic<bool> running` flag in the coroutine before each sleep.
  PeerManager exposes `is_stopping()` or main sets `running = false` before `pm.stop()`.

**Simplest wiring change in main.cpp:**
```cpp
// Before pm.start():
asio::cancellation_signal expiry_cancel;

// Expiry scan coroutine uses cancellation_signal
asio::co_spawn(ioc,
    asio::bind_cancellation_slot(expiry_cancel.slot(), expiry_loop(storage, ioc)),
    asio::detached);

// At shutdown (called from pm.stop() or signal handler):
expiry_cancel.emit(asio::cancellation_type::terminal);
```

The SIGHUP signal set in PeerManager should be cancelled in `stop()` — currently it is a
`asio::signal_set sighup_signal_` member. Calling `sighup_signal_.cancel()` stops the `sighup_loop()`
coroutine.

**Build order:** Fifth, done with or after persistent peer list (both touch `stop()`). Can be a single
phase covering both features 4 and 5.

---

### 6. Metrics/Observability

**Current state:** Zero runtime metrics. Only spdlog event logging. No way to query operational state
without grepping logs.

**Integration point:** New `Metrics` struct + counters at existing code paths in PeerManager +
SIGUSR1 dump coroutine.

**Recommended approach:** A lightweight `Metrics` struct with `std::atomic<uint64_t>` counters,
updated inline at existing integration points. No external library. No HTTP endpoint. Dump to spdlog
on SIGUSR1.

```cpp
// db/metrics/metrics.h  (or inline in peer_manager.h)
struct Metrics {
    std::atomic<uint64_t> blobs_ingested{0};
    std::atomic<uint64_t> blobs_rejected_invalid{0};
    std::atomic<uint64_t> blobs_rejected_storage_full{0};
    std::atomic<uint64_t> blobs_rate_limited{0};
    std::atomic<uint64_t> sync_rounds_initiated{0};
    std::atomic<uint64_t> sync_rounds_completed{0};
    std::atomic<uint64_t> blobs_sent_sync{0};
    std::atomic<uint64_t> blobs_received_sync{0};
    std::atomic<uint64_t> connections_accepted{0};
    std::atomic<uint64_t> connections_rejected_acl{0};
    std::atomic<uint64_t> peers_discovered_pex{0};
};
```

**SIGUSR1 dump coroutine:** Follows the `sighup_loop()` pattern exactly — dedicated coroutine member
function in PeerManager, `asio::signal_set sigusr1_signal_`, awaits signal in a loop, dumps metrics
via `spdlog::info`. Avoids the stack-use-after-return issue documented in RETROSPECTIVE for SIGHUP.

**Counter placement:**
- `blobs_ingested` / `blobs_rejected_*` — in `PeerManager::on_peer_message` after `engine_.ingest()`
- `sync_rounds_*` / `blobs_sent/received_sync` — in `run_sync_with_peer` using `SyncStats` return value
- `connections_accepted` — in `on_peer_connected`
- `connections_rejected_acl` — in `on_peer_connected` when ACL check fires (currently silent TCP close)
- `peers_discovered_pex` — in `handle_peer_list_response`
- `blobs_rate_limited` — in rate limit check (feature 7)

**Storage stats exposure:** `metrics_dump()` also calls `storage.storage_stats()` and logs used/max bytes.

**Build order:** Can be done any time after storage limits (for the storage stats integration). Good to
build before rate limiting so drop counters are already wired.

---

### 7. Rate Limiting

**Current state:** No write rate limiting. The strike system (`STRIKE_THRESHOLD = 10`) fires on
malformed/invalid messages — abuse prevention for correctness violations, not volume. A single peer
can send unlimited `Data` messages per second.

**What to limit:** `Data` messages (direct writes) per peer per second. `BlobTransfer` (sync) is
exempt — it is already bounded by one-blob-at-a-time and the sequential Phase A/B/C protocol.

**Integration point:** `PeerInfo` struct (token bucket state) + `PeerManager::on_peer_message`
handler for `Data` and `Delete` message types.

**Algorithm:** Token bucket, per-peer, O(1) state.
```cpp
// Fields added to PeerInfo:
uint32_t write_tokens = RATE_LIMIT_BURST;
uint64_t last_token_refill_us = 0;   // microseconds for finer granularity

// On each Data/Delete message:
uint64_t now_us = current_time_us();
uint64_t elapsed_us = now_us - peer->last_token_refill_us;
uint32_t new_tokens = std::min<uint64_t>(RATE_LIMIT_BURST,
    peer->write_tokens + (elapsed_us * RATE_LIMIT_PER_SEC) / 1'000'000);
peer->write_tokens = new_tokens;
peer->last_token_refill_us = now_us;

if (peer->write_tokens == 0) {
    metrics_.blobs_rate_limited++;
    // Drop silently or record_strike? Drop silently — rate limiting is not an error.
    co_return;  // or return from message handler
}
peer->write_tokens--;
// proceed to engine_.ingest()
```

**Why token bucket:** O(1) state (two uint64_t fields per peer), no ring buffer, no sorted containers.
Naturally handles burst traffic. Constant-time on every message. Proven pattern for network rate limiting.

**Config fields:**
- `Config::rate_limit_writes_per_sec = 10` (default: 10 writes/second per peer)
- `Config::rate_limit_burst = 50` (default: burst of 50 before throttling)
- Both 0 = disabled (backwards compatible default).

**Modified components:**
- `PeerInfo` — add `write_tokens` and `last_token_refill_us` fields.
- `PeerManager::on_peer_message` — token bucket check before `engine_.ingest()` on `Data` and
  `Delete` message types. Consult `config_.rate_limit_writes_per_sec == 0` to skip when disabled.
- `Config` — add two rate limit fields.
- Metrics — increment `blobs_rate_limited`.

**Build order:** After metrics (so drops are counted). Depends on config fields from feature 6 cycle
if done together, otherwise independent.

---

### 8. Namespace-Scoped Sync

**Current state:** `PeerManager::run_sync_with_peer` calls `engine_.list_namespaces()` and syncs ALL
namespaces with ALL peers. No per-peer filter exists.

**What this adds:** A per-connection namespace filter — a node can declare which namespaces it wants
to replicate with a specific peer. This is operationally distinct from pub/sub (real-time notifications)
and from ACL (which controls who can connect). Namespace-scoped sync controls what gets replicated
during the hash-list diff phase.

**Integration point:** `PeerInfo` struct + `PeerManager::run_sync_with_peer` + `Config`.

**Protocol options:**

Option A — Negotiated filter (new wire messages `SyncFilterRequest = 24`, `SyncFilterResponse = 25`):
Requires protocol changes, regenerating FlatBuffers, and a new sync handshake step.

Option B — Local-only filter (no new wire messages):
The initiator already controls which namespaces it requests hashes for in `run_sync_with_peer`.
Adding a `std::set<std::array<uint8_t, 32>> sync_namespaces` field to `PeerInfo` and filtering the
`list_namespaces()` result before entering the hash-exchange phase requires zero protocol changes.

**Recommendation: Option B.** The existing sync protocol's Phase A already sends the local namespace
list to the responder. The initiator then requests hash lists only for namespaces in the intersection.
Adding a filter at the initiator side (`if (!peer->sync_namespaces.empty() && !peer->sync_namespaces.count(ns.namespace_id)) continue;`) is a 3-line change in `run_sync_with_peer`. The responder
still receives hash requests only for filtered namespaces — naturally scoped.

**Config design:**
```json
{
  "sync_namespaces": ["aabbcc...hex32bytes...", "11223344...hex32bytes..."]
}
```
Global filter applied to all peers. Empty = sync all (current behavior, backwards compatible).

**Modified components:**
- `Config` — add `std::vector<std::string> sync_namespaces` (hex namespace IDs, validated like `allowed_keys`).
- `PeerManager` — on start, convert `config_.sync_namespaces` to a `std::set<std::array<uint8_t,32>>`.
- `PeerManager::run_sync_with_peer` — filter the namespace list from `engine_.list_namespaces()` against
  the set before entering Phase B hash exchange. Zero protocol changes.
- Per-peer scoping (if needed later): add `sync_namespaces` to `PeerInfo` and populate from a
  per-peer config section. Out of scope for v0.4.0 — global filter is sufficient.

**Build order:** Independent, no deps on other v0.4.0 features. Last or concurrent with metrics/rate
limiting.

---

## Component Modification Summary

| Component | File | Change Type | Feature(s) |
|-----------|------|-------------|------------|
| `Storage::Impl` | storage.cpp | Add 5th sub-database (`tombstone_map`), max_maps 5->6 | Tombstone index |
| `Storage::store_blob` | storage.cpp | Populate tombstone_map on tombstone writes | Tombstone index |
| `Storage::has_tombstone_for` | storage.cpp | O(n) scan replaced with O(1) btree lookup | Tombstone index |
| `Storage` | storage.h | New `storage_stats()` returning `StorageStats` struct | Storage limits |
| `StorageStats` | storage.h | New struct: used_bytes, max_bytes, blob_count | Storage limits |
| `StoreResult::Status` | storage.h | Add `StorageFull` variant | Storage limits |
| `IngestError` | engine.h | Add `storage_full` error code | Storage limits |
| `BlobEngine` | engine.h/.cpp | Step 0 capacity check before namespace derivation | Storage limits |
| `wire/transport.fbs` | transport.fbs | Add `StorageFull = 23` to TransportMsgType enum | Disk-full reporting |
| `transport_generated.h` | wire/ | Regenerate from updated transport.fbs | Disk-full reporting |
| `PeerManager::on_peer_message` | peer_manager.cpp | Handle storage_full: send StorageFull response | Disk-full reporting |
| `PeerInfo` | peer_manager.h | Add `bool peer_is_full = false` | Disk-full reporting |
| `PeerManager::stop()` | peer_manager.cpp | Call save_persisted_peers() before drain | Persistent peer list |
| `PeerManager::on_peer_disconnected` | peer_manager.cpp | Verify update_persisted_peer is called | Persistent peer list |
| `PeerManager::stop()` | peer_manager.cpp | Cancel all timers, set stopping_, call server_.stop() | Graceful shutdown |
| `main.cpp::cmd_run` | main.cpp | Wire expiry coroutine cancellation | Graceful shutdown |
| `Metrics` struct | metrics/metrics.h | New: atomic counters for all key events | Metrics |
| `PeerManager` | peer_manager.h | Add `Metrics metrics_` member + SIGUSR1 signal set | Metrics |
| `PeerManager` | peer_manager.cpp | Increment counters at event sites, add sigusr1_loop() | Metrics |
| `PeerInfo` | peer_manager.h | Add token bucket fields (write_tokens, last_refill_us) | Rate limiting |
| `PeerManager::on_peer_message` | peer_manager.cpp | Token bucket check before Data/Delete ingest | Rate limiting |
| `Config` | config.h | Add max_storage_bytes, rate limit fields, sync_namespaces | All config fields |
| `PeerManager` | peer_manager.cpp | Parse sync_namespaces into set, filter in run_sync_with_peer | Namespace-scoped sync |

---

## New Components Required

| Component | Location | Purpose |
|-----------|----------|---------|
| `tombstone_map` sub-database | `Storage::Impl` | O(1) tombstone lookup index |
| `StorageStats` struct | storage.h | Return type for `storage_stats()` |
| `Metrics` struct | `db/metrics/metrics.h` | Runtime atomic counters |
| SIGUSR1 signal handler | peer_manager.cpp | Metrics dump coroutine (follows sighup_loop pattern) |
| `StorageFull = 23` wire message | transport.fbs | Protocol-level rejection signal for storage capacity |

A dedicated `db/metrics/metrics.h` is cleaner for testability. Alternatively, `Metrics` can be
defined inline in `peer_manager.h` since PeerManager owns all integration points — either is
acceptable given the simple struct design.

---

## Data Flow Changes

### Write Path (after v0.4.0)
```
Data message -> on_peer_message
  -> rate limit check (token bucket per peer)     [NEW — drop+count if exhausted]
  -> engine_.ingest()
      -> Step 0: structural size check
      -> Step 0: capacity check (used_bytes >= max) [NEW — storage_full rejection]
      -> namespace derivation (SHA3-256)
      -> signature verification (ML-DSA-87)
      -> storage.store_blob()
          -> tombstone_map upsert (if tombstone)   [NEW — O(1) index population]
      -> WriteAck OR StorageFull wire response      [NEW — new rejection + wire message]
  -> metrics.blobs_ingested++ OR blobs_rejected_* [NEW]
```

### Shutdown Path (after v0.4.0)
```
SIGINT -> Server::stop() called -> PeerManager::stop() triggered
  -> set stopping_ = true
  -> save_persisted_peers()                        [NEW — before drain]
  -> cancel sync_timer, pex_timer, sighup_signal, sigusr1_signal
  -> server_.stop() -> drain(5s)
       -> Goodbye to all peers
       -> close all connections
  -> expiry_cancel.emit() -> expiry scan coroutine exits [NEW — cancellation wired]
  -> ioc.run() returns cleanly
```

### Sync Path (after v0.4.0, namespace-scoped)
```
run_sync_with_peer(conn)
  -> engine_.list_namespaces()
  -> [NEW] filter: remove namespaces not in config_.sync_namespace_set (if non-empty)
  -> Phase A: send filtered NamespaceList
  -> Phase B: hash exchange for each namespace in filtered set
  -> Phase C: diff -> BlobRequest -> BlobTransfer
  (no protocol changes — filtering is local to initiator)
```

---

## Suggested Build Order

This order minimizes blocked work and front-loads correctness improvements:

| Order | Feature | Rationale |
|-------|---------|-----------|
| 1 | Tombstone index | Isolated, no deps, correctness fix. Touches storage.cpp only. |
| 2 | Storage limits | Extends storage.h/engine.h. No wire changes yet. |
| 3 | Disk-full reporting | Depends on storage_full error code (feature 2). Adds wire message. |
| 4 | Persistent peer list + Graceful shutdown | Coupled at PeerManager::stop(). Single phase. |
| 5 | Metrics | No deps. Add early so counters are in place for remaining features. |
| 6 | Rate limiting | Depends on metrics (for drop counting). |
| 7 | Namespace-scoped sync | Independent. No deps on other v0.4.0 features. |
| 8 | README + version bump | After all features stable. Documentation-only. |

**Estimated phase structure:** 3-4 phases covering 8 plans.
- Phase A: Storage (tombstone index + storage limits + disk-full reporting) — 3 plans
- Phase B: Operations (graceful shutdown + persistent peers + metrics) — 2-3 plans
- Phase C: Abuse prevention + sync control (rate limiting + namespace-scoped sync) — 2 plans
- Phase D: Docs + version (README + version.h) — 1 plan

---

## Architectural Constraints to Preserve

| Constraint | Application in this milestone |
|------------|-------------------------------|
| Pimpl for Storage | `tombstone_map` lives in `Storage::Impl`. Never exposed in storage.h. |
| Step 0 pattern | Capacity check (stat read) before namespace derivation (SHA3-256). |
| Deque for `peers_` | Token bucket fields in PeerInfo do not change this. |
| No new coroutine primitives | SIGUSR1 follows SIGHUP pattern — member function coroutine, not lambda. |
| Sequential sync protocol | Namespace filter applies before Phase A, does not change Phase A/B/C ordering. |
| One-blob-at-a-time | Rate limiting applies to client Data messages only, not to sync BlobTransfer. |
| Silent TCP close for ACL | StorageFull is NOT silent — it sends a wire response. Correct: capacity is operational, not a security boundary. |
| FlatBuffers additive versioning | `StorageFull = 23` is additive. Old nodes log-and-skip unknown types in on_peer_message. |
| No new external dependencies | All features use existing components: libmdbx stats, Asio signals, atomic counters. |

---

## Anti-Patterns to Avoid

### Anti-Pattern: In-Memory Byte Counter for Storage Limits
**What:** Maintain a `std::atomic<uint64_t>` tracking bytes stored, increment on write, decrement on
expiry.
**Why bad:** The counter is wrong after a crash and restart. libmdbx has the truth; the counter is a
stale shadow. The expiry scanner runs periodically — in the gap between expiry events and counter
updates, the limit may be incorrectly enforced.
**Instead:** Query libmdbx stats directly via `env.stat()`. A read transaction is cheap and always
accurate.

### Anti-Pattern: StorageFull Causes Connection Close
**What:** When a peer's Data message hits the storage limit, close the connection.
**Why bad:** The peer may be a valid sync partner for hash-list diff (reading). Closing forces
reconnection overhead and disrupts the sync session. The node is still useful for read operations.
**Instead:** Send StorageFull wire message, set `peer_is_full = true`, continue accepting sync.

### Anti-Pattern: Rate Limiting Sync Traffic
**What:** Apply the token bucket to BlobTransfer (sync) messages as well as Data messages.
**Why bad:** Sync is already bounded by the one-blob-at-a-time sequential protocol. Rate limiting it
adds latency to legitimate replication with no abuse prevention benefit — sync is authenticated and
initiated by the peer, not a write endpoint.
**Instead:** Rate limit only Data and Delete messages (direct client writes).

### Anti-Pattern: Negotiated Namespace Filter (Option A) for Sync Scoping
**What:** Add SyncFilterRequest/SyncFilterResponse wire messages for namespace-scoped sync.
**Why bad:** Adds protocol complexity and two new message types. Requires changes on both sides of
every connection. The initiator already controls what it requests — a local filter achieves the same
result with zero protocol changes.
**Instead:** Local filter in `run_sync_with_peer`. The responder receives hash requests only for
namespaces the initiator chose to include — naturally scoped without negotiation.

### Anti-Pattern: External Metrics Exporter (Prometheus/StatsD)
**What:** Adding a Prometheus endpoint or StatsD UDP export for metrics.
**Why bad:** Adds external dependencies, network attack surface, and configuration complexity. The
daemon has no HTTP server and should not gain one (PROJECT.md explicitly excludes HTTP/REST).
**Instead:** SIGUSR1 dump to spdlog. Operators can scrape logs. Simple, zero new dependencies.

---

## Patterns to Follow

### Extend PeerInfo for Per-Connection State
Previous examples: `strike_count`, `syncing`, `sync_inbox`, `sync_notify`, `subscribed_namespaces`,
`is_bootstrap`. New additions: `peer_is_full`, token bucket fields. All are connection-scoped,
populated in `on_peer_connected`, accessed in message handlers. No locking needed — single io_context
thread.

### New Sub-Database Follows delegation_map Pattern
`tombstone_map` creation mirrors `delegation_map` exactly: open in `Impl` constructor write
transaction, increment `max_maps`, populate in `store_blob` using existing data from the blob being
written. The blob provides all needed data (namespace, tombstone flag, target hash).

### SIGHUP Coroutine Pattern for Signal Handling
SIGUSR1 metrics dump follows `sighup_loop()` exactly: dedicated member function (not lambda —
avoids stack-use-after-return), `asio::signal_set` on the io_context, awaits signal in a loop.
Cancel via `sigusr1_signal_.cancel()` in `stop()`.

### Step 0 Ordering in BlobEngine::ingest
Current: structural -> namespace -> signature -> storage.
Updated: structural -> **capacity** -> namespace -> signature -> storage.
Capacity check (libmdbx stat read, ~microseconds) is cheaper than SHA3-256 (hundreds of microseconds
for large blobs). Put cheaper rejections first.

---

## Sources

All findings from direct codebase inspection — no external sources required.

- `db/storage/storage.h` — Storage API, StoreResult, sub-database structure
- `db/storage/storage.cpp` — libmdbx geometry config (size_upper=64 GiB, max_maps=5), store_blob
  implementation (delegation_map population pattern), has_tombstone_for O(n) scan
- `db/engine/engine.h` — IngestError enum, BlobEngine ingest pipeline
- `db/peer/peer_manager.h` — PeerInfo struct (all fields), PeerManager methods, persistence
  infrastructure (load/save/update_persisted_peer), MAX constants
- `db/peer/peer_manager.cpp` — Constructor wiring, start() sequence, signal handling, sighup_loop
  pattern, sync orchestration structure
- `db/net/server.h` / `server.cpp` — drain() coroutine, SIGINT/SIGTERM handling, draining_ flag
- `db/net/connection.h` — Connection lifecycle, peer_pubkey_, close_gracefully()
- `db/sync/sync_protocol.h` — SyncProtocol API, SyncStats struct
- `db/config/config.h` — Config struct (current fields: allowed_keys, max_peers, sync_interval)
- `db/wire/transport_generated.h` — TransportMsgType enum (current max: Notification = 22)
- `db/main.cpp` — Component wiring, expiry scan coroutine (asio::detached, never cancelled)
- `.planning/PROJECT.md` — Requirements, constraints, key decisions table
- `.planning/RETROSPECTIVE.md` — Patterns established, lessons learned across all milestones
