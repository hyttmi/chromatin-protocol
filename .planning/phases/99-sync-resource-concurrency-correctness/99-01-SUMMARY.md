---
phase: 99-sync-resource-concurrency-correctness
plan: 01
subsystem: database
tags: [sync, pending-fetches, composite-key, mdbx, correctness]

requires:
  - phase: 96-peermanager-architecture
    provides: BlobPushManager component with pending_fetches_ (Phase 96 decomposition)
  - phase: 80-targeted-blob-fetch
    provides: BlobNotify/BlobFetch/BlobFetchResponse protocol (Phase 80 PUSH-05/PUSH-06)
provides:
  - "64-byte composite namespace||hash key for pending_fetches_ (SYNC-02)"
  - "Unconditional pending_fetches_ cleanup on all ingest outcomes (SYNC-01)"
  - "MDBX MVCC snapshot consistency documentation for collect_namespace_hashes (SYNC-03)"
  - "ArrayHash64 functor for 64-byte array hashing"
  - "make_pending_key helper for composite key construction"
affects: [sync, blob-push, peer-manager]

tech-stack:
  added: []
  patterns:
    - "Composite key pattern for namespace-scoped dedup maps"
    - "Unconditional state cleanup before result branching"

key-files:
  created:
    - db/tests/peer/test_blob_push.cpp
  modified:
    - db/peer/peer_types.h
    - db/peer/blob_push_manager.h
    - db/peer/blob_push_manager.cpp
    - db/sync/sync_protocol.cpp
    - db/CMakeLists.txt

key-decisions:
  - "ArrayHash64 uses first 8 bytes (same pattern as ArrayHash32) -- sufficient entropy for SHA3-256 namespace||hash keys"
  - "make_pending_key is inline free function in blob_push_manager.h (not a method) for test accessibility"
  - "Not-found BlobFetchResponse entries cleaned on disconnect (no namespace+hash in wire format to build key)"
  - "Exception catch path entries also cleaned on disconnect (no decodable blob data available)"

patterns-established:
  - "Composite key: namespace_id || blob_hash for any per-blob state maps"
  - "Unconditional cleanup: erase state BEFORE branching on result status"

requirements-completed: [SYNC-01, SYNC-02, SYNC-03]

duration: 41min
completed: 2026-04-09
---

# Phase 99 Plan 01: Sync State Leak Fixes Summary

**Fix pending_fetches_ state leaks: 64-byte composite namespace||hash key (SYNC-02), unconditional cleanup on all ingest outcomes (SYNC-01), MDBX MVCC snapshot safety documented (SYNC-03)**

## Performance

- **Duration:** 41min
- **Started:** 2026-04-09T02:37:47Z
- **Completed:** 2026-04-09T03:19:43Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- Fixed pending_fetches_ state leak where rejected/failed ingest outcomes never cleaned their entries (SYNC-01)
- Changed pending_fetches_ key from 32-byte hash-only to 64-byte namespace||hash composite, preventing cross-namespace hash collisions (SYNC-02)
- Documented collect_namespace_hashes MDBX MVCC snapshot consistency as safe-by-design (SYNC-03)
- Added 8 unit tests (82 assertions) covering composite key correctness, hash functor distribution, and cleanup patterns

## Task Commits

Each task was committed atomically:

1. **Task 1: Add ArrayHash64 and make_pending_key, update pending_fetches_ type** - `aa706c1` (fix)
2. **Task 2: Unit tests for sync state fixes** - `bc5885d` (test)

## Files Created/Modified
- `db/peer/peer_types.h` - Added ArrayHash64 functor for 64-byte composite keys
- `db/peer/blob_push_manager.h` - Added make_pending_key helper, changed pending_fetches_ to 64-byte key with ArrayHash64
- `db/peer/blob_push_manager.cpp` - Composite key in on_blob_notify, unconditional erase in handle_blob_fetch_response
- `db/sync/sync_protocol.cpp` - MDBX MVCC snapshot consistency documentation comment
- `db/tests/peer/test_blob_push.cpp` - 8 tests for composite key, hash functor, cleanup patterns
- `db/CMakeLists.txt` - Added test_blob_push.cpp to test sources

## Decisions Made
- ArrayHash64 uses the same first-8-bytes pattern as ArrayHash32 -- SHA3-256 output has sufficient entropy in any 8-byte prefix
- make_pending_key is a free inline function (not a class method) so tests can call it directly without constructing BlobPushManager
- Not-found and exception paths rely on disconnect cleanup (clean_pending_fetches) since the wire format doesn't carry enough data to reconstruct the composite key
- SYNC-03 documented as safe with no code changes -- MDBX MVCC provides snapshot isolation per read transaction

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- SYNC-01, SYNC-02, SYNC-03 requirements complete
- pending_fetches_ is now correct for all ingest outcomes and namespace-safe
- Ready for 99-02 (resource exhaustion) and 99-03 (concurrency safety)

## Self-Check: PASSED

All files verified present. All commits verified in git log.

---
*Phase: 99-sync-resource-concurrency-correctness*
*Completed: 2026-04-09*
