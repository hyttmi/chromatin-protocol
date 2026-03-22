---
phase: 38-thread-pool-crypto-offload
plan: 02
subsystem: engine
tags: [asio, coroutines, crypto-offload, thread-pool, ml-dsa-87, sha3-256]

# Dependency graph
requires:
  - "38-01: thread pool infrastructure, offload() helper, pool_ reference plumbed through BlobEngine"
provides:
  - "Async BlobEngine::ingest() with two-dispatch crypto offload pattern"
  - "Async BlobEngine::delete_blob() with single-dispatch crypto offload"
  - "Async SyncProtocol::ingest_blobs() coroutine"
  - "PeerManager Data/Delete handlers as co_spawn coroutines"
  - "run_async() test helper for synchronous awaitable execution"
affects: [38-03]

# Tech tracking
tech-stack:
  added: []
  patterns: [two-dispatch-ingest, co_spawn-handler-wrapping, run_async-test-helper]

key-files:
  created: []
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/peer_manager.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/tests/engine/test_engine.cpp
    - db/tests/sync/test_sync_protocol.cpp
    - db/tests/peer/test_peer_manager.cpp
    - db/tests/test_daemon.cpp

key-decisions:
  - "Two-dispatch pattern for ingest: blob_hash first (dedup gate), build_signing_input+verify bundled second"
  - "All sha3_256 offloaded including small pubkey-to-namespace derivation (uniform model per CONTEXT.md)"
  - "PeerManager Data/Delete handlers wrapped in co_spawn (not refactored to member coroutines)"
  - "StorageFull/QuotaExceeded send_message uses co_await directly in coroutine (eliminated nested co_spawn)"
  - "run_async() test helper with temporary io_context per call (stateless, no io_context per test case)"

patterns-established:
  - "Two-dispatch ingest: cheap crypto (hash) -> dedup gate -> expensive crypto (verify) only for new blobs"
  - "co_spawn wrapping for non-coroutine callbacks that need async operations"
  - "run_async(pool, awaitable) pattern for calling coroutines from synchronous test code"

requirements-completed: [PERF-06, PERF-07, PERF-08]

# Metrics
duration: 17min
completed: 2026-03-19
---

# Phase 38 Plan 02: Engine Crypto Offload Summary

**BlobEngine::ingest() and delete_blob() converted to coroutines with SHA3-256 and ML-DSA-87 crypto offloaded to thread pool via two-dispatch pattern (duplicates pay only one pool round-trip)**

## Performance

- **Duration:** 17 min
- **Started:** 2026-03-19T07:45:27Z
- **Completed:** 2026-03-19T08:02:27Z
- **Tasks:** 2
- **Files modified:** 9 (2 engine, 2 sync, 1 peer_manager, 4 test files)

## Accomplishments
- BlobEngine::ingest() uses two-dispatch pattern: (1) sha3_256 pubkey hash offload, (2) blob_hash offload with dedup gate on event loop, (3) build_signing_input+verify bundled offload -- duplicates skip step 3 entirely
- BlobEngine::delete_blob() uses sha3_256 pubkey hash offload + single build_signing_input+verify bundled dispatch
- All callers (PeerManager Data/Delete handlers, SyncProtocol::ingest_blobs, sync responder/initiator) updated to co_await async engine
- All 370 tests pass including engine, sync, peer_manager, and daemon e2e tests through async coroutine execution

## Task Commits

Each task was committed atomically:

1. **Task 1: Convert BlobEngine to async with crypto offload** - `00c7ce1` (feat)
2. **Task 2: Update all callers and fix tests for async engine** - `72e34ca` (feat)

## Files Created/Modified
- `db/engine/engine.h` - Async signatures for ingest() and delete_blob(), added awaitable/thread_pool includes
- `db/engine/engine.cpp` - Two-dispatch ingest with crypto::offload(), single-dispatch delete_blob, co_return throughout
- `db/peer/peer_manager.cpp` - Data/Delete handlers wrapped in co_spawn, co_await engine calls, co_await sync_proto_.ingest_blobs
- `db/sync/sync_protocol.h` - ingest_blobs() now returns asio::awaitable<SyncStats>
- `db/sync/sync_protocol.cpp` - ingest_blobs() is a coroutine with co_await engine_.ingest()
- `db/tests/engine/test_engine.cpp` - run_async() helper, all 100+ engine.ingest/delete_blob calls wrapped
- `db/tests/sync/test_sync_protocol.cpp` - run_async() helper, all engine and sync ingest_blobs calls wrapped
- `db/tests/peer/test_peer_manager.cpp` - run_async() helper, all eng*.ingest/delete_blob calls wrapped
- `db/tests/test_daemon.cpp` - run_async() helper, all eng*.ingest calls wrapped

## Decisions Made
- Two-dispatch pattern: blob_hash offloaded separately from verify because dedup check (storage_.has_blob) must run on event loop between the two. Duplicates skip the expensive ML-DSA-87 verify entirely.
- Uniform sha3_256 offload model: even the small pubkey-to-namespace hash (2592 bytes) is offloaded. Per CONTEXT.md, this is a locked decision -- uniform model avoids conditional logic. If benchmarks show overhead, PERF-11 (per-size threshold) is explicitly deferred beyond v1.0.0.
- PeerManager Data/Delete handlers wrapped in co_spawn rather than converting on_peer_message to a coroutine, preserving the existing non-coroutine callback interface with Server.
- StorageFull/QuotaExceeded send_message calls simplified: since the handler is already a coroutine, co_await send_message directly instead of nested co_spawn.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Updated test_peer_manager.cpp and test_daemon.cpp**
- **Found during:** Task 2 (updating callers)
- **Issue:** Plan mentioned checking these files but didn't list them in the files_modified list. Both had direct engine.ingest() and engine.delete_blob() calls that broke after the async conversion.
- **Fix:** Added run_async() helper and wrapped all engine calls in both test files.
- **Files modified:** db/tests/peer/test_peer_manager.cpp, db/tests/test_daemon.cpp
- **Verification:** Build succeeds, all 370 tests pass
- **Committed in:** 72e34ca (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Necessary fix for compilation. No scope creep -- the plan mentioned checking these files but underestimated the number of call sites.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plan 03 (Connection handshake crypto offload) can proceed independently
- The offload() helper and pool_ reference are already in Connection (from Plan 01)
- Connection::do_handshake_* has 4 Signer::verify() call sites to offload
- No blockers

## Self-Check: PASSED

- All 9 modified files exist on disk
- Both commits verified (00c7ce1, 72e34ca)
- awaitable signatures confirmed in engine.h
- offload() calls confirmed in engine.cpp (6 dispatches)
- co_await engine_ calls confirmed in peer_manager.cpp and sync_protocol.cpp
- No AEAD state (send_counter_, recv_counter_, session_keys_) in engine.cpp
- 370/370 tests pass

---
*Phase: 38-thread-pool-crypto-offload*
*Completed: 2026-03-19*
