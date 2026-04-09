# Phase 99: Sync, Resource & Concurrency Correctness - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning
**Source:** Auto-mode (recommended defaults selected)

<domain>
## Phase Boundary

Fix specific bugs and correctness gaps in sync state management, resource limit enforcement, and coroutine safety. This is a hardening phase — no new features, no protocol changes. Every requirement maps to a known bug or race condition identified during the v2.2.0 audit.

</domain>

<decisions>
## Implementation Decisions

### Sync State Cleanup (SYNC-01, SYNC-02)
- **D-01:** pending_fetches_ cleanup on rejected ingest: in handle_blob_fetch_response, erase the pending entry for ALL ingest outcomes (not just stored). Currently only erases on `result.accepted && result.ack.has_value()` — rejected blobs leak entries.
- **D-02:** pending_fetches_ key must include namespace: change from `unordered_map<array<uint8_t,32>, Connection::Ptr>` to a composite key that includes both namespace (32 bytes) and hash (32 bytes). Use `array<uint8_t,64>` as key (namespace||hash concatenation) with a 64-byte hash functor. This prevents cross-namespace hash collision where two different blobs in different namespaces share the same content hash.
- **D-03:** clean_pending_fetches(conn) already handles disconnect cleanup — verify it covers all map entries after the key type change.

### Phase B Snapshot Consistency (SYNC-03)
- **D-04:** collect_namespace_hashes returns `vector<array<uint8_t,32>>` by value — the returned vector is already a snapshot. The concern is whether concurrent store_blob calls during Phase A/B can mutate the MDBX cursor mid-iteration. MDBX read transactions provide snapshot isolation, so if collect_namespace_hashes opens a read txn, it's already consistent. Verify this is the case — if not, wrap in explicit read transaction.
- **D-05:** The sync protocol already uses sequential phases (A, B, C) with no concurrent writes from the same sync session. The risk is from other connections ingesting blobs concurrently. MDBX MVCC handles this, but verify the read transaction scope covers the full hash collection.

### Subscription Limit (RES-01)
- **D-06:** Enforce per-connection subscription limit in the Subscribe message handler (message_dispatcher.cpp). When a Subscribe arrives and the connection already has >= max subscriptions, send an error response and don't add the subscription.
- **D-07:** Default limit: 256 namespaces per connection (matches relay's per-session cap from Phase 88). Make it configurable via config field `max_subscriptions_per_connection` (0 = unlimited for backward compat).
- **D-08:** Error response: use existing protocol error mechanism — no new message type needed.

### Bootstrap & TOCTOU Fixes (RES-02, RES-03, RES-04)
- **D-09:** Bootstrap peer detection: compare full endpoint (host + port), not just host string. A machine running multiple nodes on different ports should be detected as separate peers.
- **D-10:** TOCTOU on capacity check: currently `if (storage_.used_bytes() >= max_storage_bytes_)` runs before the actual store, allowing a concurrent ingest to exceed the limit. Fix by making the capacity check part of the store transaction — check-and-reserve atomically inside store_blob.
- **D-11:** TOCTOU on quota check: same pattern as D-10 — move quota check into the store transaction so it can't race.
- **D-12:** Quota rebuild iterator bug: the clear loop likely has an increment-after-erase pattern (erasing from a container while iterating without adjusting the iterator). Fix with standard erase-returns-next-iterator pattern.

### Coroutine Counter Safety (CORO-01)
- **D-13:** NodeMetrics uses plain uint64_t and documents "single io_context thread, no races" in peer_types.h:57. Verify this holds: all metrics increments must be on the io_context strand (not in thread_pool offload callbacks). If any increment happens after co_await asio::post(pool_), it's a data race.
- **D-14:** If any counter is incremented off-strand, the fix is to move the increment before the offload or after the co_await transfer back to io_context. Do NOT use std::atomic — that papers over the design issue. Fix the strand confinement.
- **D-15:** Run full test suite under TSAN to verify no data races on NodeMetrics fields.

### Test Strategy
- **D-16:** Each bug fix gets a targeted unit test proving the bug existed (fails without fix, passes with fix). Tag all tests [sync][correctness], [resource][limit], or [coro][safety] as appropriate.
- **D-17:** Full TSAN run is the acceptance gate for CORO-01. All existing tests must also pass under ASAN/UBSAN.

### Claude's Discretion
- Implementation order within each category (bugs can be fixed in any order within sync/resource/coro groupings)
- Exact error message text for subscription limit rejection
- Whether to add debug logging for each fix (recommended: yes, consistent with Phase 98 pattern)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Sync State
- `db/peer/blob_push_manager.h` — pending_fetches_ declaration and BlobPushManager interface
- `db/peer/blob_push_manager.cpp` — BlobFetch/BlobNotify handlers, pending cleanup

### Sync Protocol
- `db/sync/sync_protocol.cpp` — collect_namespace_hashes, get_blobs_by_hashes, ingest_blobs
- `db/peer/sync_orchestrator.cpp` — Phase A/B/C orchestration, run_sync_with_peer

### Resource Limits
- `db/peer/message_dispatcher.cpp` — Subscribe handler, quota checks
- `db/peer/connection_manager.cpp` — bootstrap detection, connection acceptance
- `db/storage/storage.cpp` — store_blob capacity check, quota enforcement
- `db/engine/engine.cpp` — ingest capacity/quota Step 0b and Step 2a

### Coroutine Safety
- `db/peer/peer_types.h` — NodeMetrics struct definition (line 59)
- `db/peer/metrics_collector.cpp` — Prometheus metrics collection
- `db/peer/message_dispatcher.cpp` — metrics increments after co_await

### Audit Source
- `.planning/phases/98-ttl-enforcement/98-CONTEXT.md` — Phase 98 decisions (D-03 std::time inline, D-04 now-once pattern) — reuse patterns here

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/peer/peer_types.h` ArrayHash32 functor — extend to ArrayHash64 for composite pending_fetches key
- Phase 98's handler-level check pattern — same approach for subscription limit check
- `wire::saturating_expiry` pattern — clean inline utility functions in codec.h

### Established Patterns
- Step 0 pattern: cheapest validation first (integer compare before expensive ops)
- Single io_context thread model: all peer/connection logic runs on one thread
- MDBX transactions provide snapshot isolation for reads

### Integration Points
- BlobPushManager::pending_fetches_ — key type change affects on_blob_notify, handle_blob_fetch_response, clean_pending_fetches
- Engine::ingest — capacity/quota checks need transactional upgrade
- ConnectionManager — bootstrap peer comparison logic

</code_context>

<specifics>
## Specific Ideas

No specific requirements — these are well-defined bug fixes from the v2.2.0 audit. Each requirement maps to a known defect with clear fix criteria.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 99-sync-resource-concurrency-correctness*
*Context gathered: 2026-04-09 via auto-mode*
