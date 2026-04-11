# db/ Coroutine Safety Findings - Phase 106

**Date:** 2026-04-11
**Scope:** db/peer/ component files (~3631 lines across 5 files, 10 .h + .cpp)
**Purpose:** Read-only audit per D-06. No db/ code changes in v3.1.0 (db/ is frozen).
**Action required:** User manual review for any HIGH/CRITICAL findings.

## Severity Levels

- CRITICAL: Use-after-free or data race confirmed
- HIGH: Pattern likely to cause UB under specific conditions
- MEDIUM: Defensive improvement possible
- LOW: Style/documentation only
- SAFE: Reviewed and confirmed safe

## Summary

- Files audited: 5 (10 files: 5 .h + 5 .cpp)
- Total findings: 22
- CRITICAL: 0 | HIGH: 0 | MEDIUM: 2 | LOW: 1 | SAFE: 19

## Known-Safe Patterns

These patterns are consistently applied across all db/peer/ code and provide strong coroutine safety:

1. **PeerInfo re-lookup after every co_await:** Every coroutine that accesses `PeerInfo*` calls `find_peer(conn)` after each suspension point. This prevents use-after-invalidation if peers_ is modified during suspension (disconnect, new connection). Pattern verified in sync_orchestrator.cpp (50+ re-lookup sites), connection_manager.cpp (5 sites), pex_manager.cpp (4 sites).

2. **recv_sync_msg takes Connection::Ptr (shared_ptr):** All sync message receive functions take `const net::Connection::Ptr&` (which is `std::shared_ptr`), not `PeerInfo*`. The Connection object is reference-counted and survives disconnect from peers_ deque.

3. **std::deque<std::unique_ptr<PeerInfo>> for heap stability:** PeerInfo objects are heap-allocated via unique_ptr stored in deque. Deque guarantees pointer stability on push_back/erase (unlike vector). PeerInfo pointers obtained via find_peer() remain valid until the corresponding element is erased.

4. **Connection snapshot before iteration with co_await:** `sync_all_peers()` and `request_peers_from_all()` snapshot `Connection::Ptr` vectors before iterating with co_await. This prevents iterator invalidation from peers_ modification during sync/PEX.

5. **Post-co_await null checks:** All coroutine paths check `find_peer(conn)` return for nullptr after co_await and co_return early if the peer disconnected during suspension.

## Findings

### Peer Component: connection_manager.cpp (426 lines)

### [D-001] [SAFE] connection_manager.cpp:57 [Container invalidation]

**Pattern:** `on_peer_connected()` iterates `peers_` with a for loop for dedup check, then erases one element and breaks.
**Risk:** None -- the function breaks immediately after erase (no continued iteration). The dedup loop uses `it = peers_.begin()` fresh each time. Non-coroutine function (no co_await).
**Status:** Safe. Iterator invalidation is handled correctly by breaking.

### [D-002] [SAFE] connection_manager.cpp:185-187 [Lambda captures]

**Pattern:** `on_peer_connected()` spawns `announce_and_sync()` via `co_spawn(ioc_, [this, conn]() -> awaitable<void> { ... })`.
**Risk:** None -- `conn` is `Connection::Ptr` (shared_ptr, copied into lambda). `this` is `ConnectionManager`, which is owned by PeerManager facade (stack-allocated in main). Lifetime guaranteed by shutdown ordering: io_context drained before PeerManager destroyed.
**Status:** Safe. Same lifetime model as relay's UdsMultiplexer.

### [D-003] [SAFE] connection_manager.cpp:222-260 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `announce_and_sync()` is a coroutine that calls `find_peer(conn)` before each co_await, and re-lookups after each co_await (`co_await conn->send_message`, `co_await timer.async_wait`).
**Risk:** None -- consistent re-lookup pattern. The `announce_notify` timer pointer is set to a stack-local timer, which stays alive for the duration of the co_await. After co_await, re-lookup occurs before accessing the timer pointer again.
**Status:** Safe. Exemplary re-lookup-after-co_await pattern.

### [D-004] [SAFE] connection_manager.cpp:266-293 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `recv_sync_msg()` sets `peer->sync_notify = &timer` (raw pointer to stack-local timer), then co_awaits the timer. After co_await, re-lookups peer and nulls out sync_notify.
**Risk:** None -- the stack-local timer lives exactly as long as the co_await. If the peer is disconnected during the wait (sync_notify pointer used by route_sync_message to cancel), the timer cancel is called on the still-valid stack-local timer. The coroutine then re-lookups and finds peer gone, returns nullopt.
**Status:** Safe. Timer lifetime matches co_await scope.

### [D-005] [SAFE] connection_manager.cpp:328-350 [Container invalidation]

**Pattern:** `disconnect_unauthorized_peers()` takes a snapshot of `Connection::Ptr` before closing, to avoid modifying `peers_` during iteration.
**Risk:** None -- snapshot-before-modify pattern correctly used. `conn->close()` triggers `on_peer_disconnected()` which modifies `peers_`, but the snapshot iteration is unaffected.
**Status:** Safe. Correct snapshot pattern.

### [D-006] [SAFE] connection_manager.cpp:357-404 [Container invalidation across co_await]

**Pattern:** `keepalive_loop()` snapshots `tcp_peers` vector of Connection::Ptr before iterating with `co_await conn->send_message()`.
**Risk:** None -- the snapshot is a vector of shared_ptrs. Even if peers_ is modified during co_await, the snapshot remains valid. After sending Ping, the loop continues to the next snapshot entry.
**Status:** Safe. Correct snapshot-before-co_await pattern.

### Peer Component: sync_orchestrator.cpp (1299 lines)

### [D-007] [SAFE] sync_orchestrator.cpp:136-608 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `run_sync_with_peer()` is a ~470-line coroutine with 50+ co_await points. Every co_await is followed by `peer = find_peer(conn)` re-lookup and null check. Uses `conn` (shared_ptr) throughout rather than storing PeerInfo* across suspensions.
**Risk:** None -- the re-lookup pattern is applied exhaustively. Verified: every `co_await conn->send_message(...)` and `co_await recv_sync_msg(...)` is followed by re-lookup.
**Status:** Safe. The most coroutine-intensive function in the codebase, but the safety pattern is consistently applied.

### [D-008] [SAFE] sync_orchestrator.cpp:610-1081 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `handle_sync_as_responder()` mirrors the initiator pattern with identical re-lookup discipline.
**Risk:** None -- same pattern as D-007.
**Status:** Safe.

### [D-009] [SAFE] sync_orchestrator.cpp:1083-1095 [Container invalidation across co_await]

**Pattern:** `sync_all_peers()` snapshots connections vector before iterating with `co_await run_sync_with_peer()`.
**Risk:** None -- snapshot-before-co_await pattern. Each iteration re-lookups via find_peer before proceeding.
**Status:** Safe.

### [D-010] [MEDIUM] sync_orchestrator.cpp:268-270 [Container invalidation]

**Pattern:** In `run_sync_with_peer()` Phase B reconciliation loop, `peer->bucket_tokens` is checked at the top of the loop but `peer` was obtained by `find_peer()` before the loop started (line 137-138). After the first namespace's reconciliation (which includes multiple co_await calls), the `peer` pointer is re-looked-up, but the byte budget check uses the most recent re-lookup. However, if reconciliation for a namespace's final co_await is followed by the loop's next iteration, the `peer` pointer from the last re-lookup is used for the budget check without a fresh re-lookup at the exact point of the check.
**Risk:** In practice, the re-lookup from the previous iteration's last co_await provides a sufficiently fresh pointer. The worst case is a single extra namespace reconciliation after the peer disconnects (find_peer returns nullptr and co_return fires immediately). Not a correctness issue.
**Recommendation:** Add a `peer = find_peer(conn); if (!peer) co_return;` before the byte budget check for consistency with the rest of the function.
**Status:** MEDIUM -- defensive improvement. Current code is not unsafe (find_peer returns nullptr = early exit on next co_await re-lookup).

### [D-011] [SAFE] sync_orchestrator.cpp:1097-1124 [Container invalidation]

**Pattern:** `sync_timer_loop()` calls `sync_all_peers()` then iterates `disconnected_peers_` to clean stale entries.
**Risk:** None -- `disconnected_peers_` iteration uses proper erase-while-iterate pattern (`it = disconnected_peers_.erase(it)`). No co_await during iteration.
**Status:** Safe.

### [D-012] [SAFE] sync_orchestrator.cpp:1130-1138 [Lambda captures]

**Pattern:** `send_sync_rejected()` spawns a detached coroutine capturing `conn` (shared_ptr) and `payload` (vector, moved).
**Risk:** None -- all captures are by value. Connection lifetime extends via shared_ptr.
**Status:** Safe.

### [D-013] [SAFE] sync_orchestrator.cpp:1155-1189 [Strand confinement]

**Pattern:** `expiry_scan_loop()` accesses `next_expiry_target_`, `expiry_loop_running_`, and calls `storage_.run_expiry_scan()`.
**Risk:** None -- all access is on the io_context thread. `rearm_expiry_timer()` also runs on the io_context thread (called from `on_blob_ingested` in BlobPushManager, which runs on io_context).
**Status:** Safe. Single-threaded access via io_context.

### [D-014] [SAFE] sync_orchestrator.cpp:1195-1228 [Container invalidation across co_await]

**Pattern:** `cursor_compaction_loop()` iterates `peers_` to build `connected` vector, then iterates `disconnected_peers_`, all after a co_await (timer wait).
**Risk:** None -- no co_await during the iteration itself. The `peers_` iteration builds a snapshot vector. The `disconnected_peers_` iteration is synchronous.
**Status:** Safe.

### Peer Component: message_dispatcher.cpp (1252 lines)

### [D-015] [SAFE] message_dispatcher.cpp:111-194 [Lambda captures]

**Pattern:** `on_peer_message()` is not a coroutine. It dispatches messages by type, using `co_spawn` for handlers that need co_await (sync_as_responder, pex_as_responder).
**Risk:** None -- `co_spawn` lambdas capture `this` (lifetime guaranteed by main scope) and `conn` (shared_ptr).
**Status:** Safe.

### [D-016] [SAFE] message_dispatcher.cpp:241-244 [Lambda captures]

**Pattern:** Subscribe handler spawns `co_spawn` for QuotaExceeded rejection, capturing `conn` (shared_ptr) and `request_id` (uint32_t value).
**Risk:** None -- all captures by value. No references to dispatcher state in the lambda.
**Status:** Safe.

### [D-017] [SAFE] message_dispatcher.cpp:307-348 [Lambda captures / shared_ptr lifetimes]

**Pattern:** Delete handler `co_spawn` captures `this`, `conn`, `request_id`, and `payload` (moved). Inside, calls `engine_.delete_blob()` (co_await to thread pool), then `co_await asio::post(ioc_, ...)` to return to IO thread.
**Risk:** None -- `conn` (shared_ptr) keeps connection alive. `this` (MessageDispatcher) outlives io_context. After returning to IO thread, `on_blob_ingested_` callback runs on io_context.
**Status:** Safe. Follows the CONC-03 offload-transfer pattern.

### [D-018] [MEDIUM] message_dispatcher.cpp:368-400 [Lambda captures / shared_ptr lifetimes]

**Pattern:** ReadRequest handler `co_spawn` captures `this`, `conn`, `request_id`, and `payload`. Calls `engine_.get_blob()` (synchronous, not offloaded to thread pool unlike Data/Delete) and then `co_await conn->send_message()`.
**Risk:** `engine_.get_blob()` accesses MDBX storage synchronously on the io_context thread. For large databases, this could block the event loop briefly. However, get_blob is a key-value lookup (O(log N) B-tree) which completes in microseconds. Not a coroutine safety issue per se, but worth noting for completeness.
**Recommendation:** If latency becomes an issue under load, consider offloading get_blob to the thread pool (same pattern as Data handler with `co_await asio::post(pool_, ...)`).
**Status:** MEDIUM -- performance consideration, not a safety bug. Current behavior is correct.

### Peer Component: blob_push_manager.cpp (265 lines)

### [D-019] [SAFE] blob_push_manager.cpp:42-97 [Container invalidation]

**Pattern:** `on_blob_ingested()` iterates `peers_` to fan out BlobNotify and Notification messages. Each iteration spawns a detached `co_spawn` with `conn` (shared_ptr) and `payload_copy` (vector, moved).
**Risk:** None -- the iteration is synchronous (no co_await during the loop). The co_spawn lambdas capture data by value. Even if peers_ changes during iteration, the loop operates on the current snapshot (range-based for on deque -- deque doesn't invalidate non-erased element references on push_back).
**Status:** Safe. Note: if a peer disconnects and is erased from peers_ while this loop runs, the shared_ptr in the lambda still keeps the Connection alive for the send.

### [D-020] [LOW] blob_push_manager.cpp:133-138 [Lambda captures]

**Pattern:** `on_blob_notify()` spawns a detached `co_spawn` with `this` and copies of `conn`, `ns`, `hash` for the BlobFetch send.
**Risk:** None functionally, but captures `this` (BlobPushManager) without shared_ptr. Lifetime guaranteed by shutdown ordering (same as other raw-this patterns in db/).
**Status:** LOW -- consistent with the codebase convention for non-shared_ptr managed objects.

### [D-021] [SAFE] blob_push_manager.cpp:146-169 [Lambda captures / shared_ptr lifetimes]

**Pattern:** `handle_blob_fetch()` spawns a detached `co_spawn` for storage lookup + send, capturing `this`, `conn`, and `payload` (moved).
**Risk:** None -- storage_.get_blob() is synchronous. co_await conn->send_message() is the only suspension point, and conn (shared_ptr) keeps the connection alive.
**Status:** Safe.

### [D-022] [SAFE] blob_push_manager.cpp:185-232 [Lambda captures / shared_ptr lifetimes / Container invalidation]

**Pattern:** `handle_blob_fetch_response()` spawns a detached `co_spawn` that calls `engine_.ingest(blob, conn)` (co_await to thread pool), then `co_await asio::post(ioc_, ...)` to return to IO thread. After returning, accesses `pending_fetches_` to clean up.
**Risk:** None -- follows the CONC-03 offload-transfer pattern. `pending_fetches_` is accessed only after returning to IO thread. `conn` (shared_ptr) keeps connection alive.
**Status:** Safe. SYNC-01 unconditional cleanup correctly applied.

### Peer Component: pex_manager.cpp (389 lines)

### [D-023] [SAFE] pex_manager.cpp:49-72 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `run_pex_with_peer()` is a coroutine that sets `peer->syncing = true`, sends PeerListRequest, waits for response, then clears syncing. Re-lookups peer after co_await.
**Risk:** None -- standard re-lookup-after-co_await pattern.
**Status:** Safe.

### [D-024] [SAFE] pex_manager.cpp:97-121 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `handle_pex_as_responder()` reads from sync_inbox, builds response, sends. Re-lookups peer after each co_await.
**Risk:** None -- standard pattern.
**Status:** Safe.

### [D-025] [SAFE] pex_manager.cpp:168-181 [Container invalidation across co_await]

**Pattern:** `request_peers_from_all()` snapshots connections vector before iterating with co_await (identical to sync_all_peers pattern).
**Risk:** None -- correct snapshot-before-co_await pattern.
**Status:** Safe.

### [D-026] [SAFE] pex_manager.cpp:353-382 [shared_ptr lifetimes / Container invalidation]

**Pattern:** `recv_sync_msg()` identical to SyncOrchestrator's and ConnectionManager's implementations. Re-lookups peer after co_await timer.
**Risk:** None -- same safe pattern repeated across components.
**Status:** Safe.

## Focus Area Analysis

### 1. Peer Decomposition (Phase 96)

All 5 component files share references to `peers_` (deque owned by ConnectionManager). Cross-component access is safe because all components run on the same io_context thread. No component holds iterators across co_await boundaries. The `find_peer()` function is duplicated in each component (returns raw pointer), which is intentional -- each component re-lookups independently after its own co_await points.

### 2. Sync Protocol (Phase A/B/C)

The sync_orchestrator.cpp `run_sync_with_peer()` and `handle_sync_as_responder()` are the most coroutine-intensive functions in the codebase (~470 lines each, 50+ co_await points). Both consistently apply the re-lookup pattern. `recv_sync_msg` takes `Connection::Ptr` (not `PeerInfo*`). MDBX cursors are not held across co_await -- all storage operations (`get_sync_cursor`, `set_sync_cursor`, `get_blob`) complete synchronously within a single transaction.

### 3. Connection Lifecycle

`on_peer_connected()` and `on_peer_disconnected()` are non-coroutine functions. They modify `peers_` directly. The dedup logic in `on_peer_connected()` correctly breaks after erasing one element. `disconnect_unauthorized_peers()` uses the snapshot-before-modify pattern. `keepalive_loop()` snapshots connections before iterating with co_await.

## Files Audited

| File | Lines | Findings | Notes |
|------|-------|----------|-------|
| connection_manager.cpp | 426 | 6 (D-001 to D-006) | Focus: on_peer_connected dedup, keepalive_loop, disconnect lifecycle |
| sync_orchestrator.cpp | 1299 | 8 (D-007 to D-014) | Focus: Phase A/B/C re-lookup pattern, cursor management, expiry |
| message_dispatcher.cpp | 1252 | 4 (D-015 to D-018) | Focus: co_spawn dispatch, CONC-03 offload pattern |
| blob_push_manager.cpp | 265 | 4 (D-019 to D-022) | Focus: pending_fetches_ cleanup, fan-out iteration |
| pex_manager.cpp | 389 | 4 (D-023 to D-026) | Focus: PEX coroutines, recv_sync_msg pattern |
