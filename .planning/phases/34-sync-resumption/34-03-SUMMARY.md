---
phase: 34-sync-resumption
plan: 03
subsystem: database
tags: [sync-cursor, tombstone, storage, seq-num, libmdbx]

# Dependency graph
requires:
  - "34-02: Cursor-aware sync orchestration (cursor skip logic introduced the regression)"
provides:
  - Fixed tombstone propagation via sync with cursor-aware orchestration active
  - Monotonic seq_num assignment in storage (zero-hash sentinel on blob deletion)
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: [zero-hash sentinel in seq_map preserves seq_num monotonicity across deletions]

key-files:
  created: []
  modified:
    - db/storage/storage.cpp
    - db/storage/storage.h

key-decisions:
  - "Zero-hash sentinel in seq_map instead of entry deletion preserves seq_num monotonicity for cursor change detection"
  - "Fix applied at storage layer (root cause) rather than cursor logic (symptom) for correctness across all seq_num consumers"

patterns-established:
  - "seq_map entries are never deleted, only tombstoned with zero-hash sentinel to guarantee monotonic seq_num"

requirements-completed: [SYNC-01, SYNC-02, SYNC-03, SYNC-04]

# Metrics
duration: 12min
completed: 2026-03-18
---

# Phase 34 Plan 03: Tombstone Sync Regression Fix Summary

**Fixed seq_num reuse in storage that caused cursor-aware sync to skip tombstone propagation between nodes**

## Performance

- **Duration:** 12 min
- **Started:** 2026-03-18T03:01:34Z
- **Completed:** 2026-03-18T03:14:28Z
- **Tasks:** 1
- **Files modified:** 2 (storage.cpp, storage.h)

## Accomplishments
- Diagnosed and fixed root cause of 2 tombstone sync E2E test regressions (ctest #210 and #213)
- Root cause: `delete_blob_data` erased seq_map entries, allowing `next_seq_num()` to recycle the same number for tombstones replacing deleted blobs -- cursor comparison saw identical seq_num and incorrectly reported HIT
- Fix: replace seq_map hash with zero sentinel on deletion, preserving the seq_num slot so tombstone gets seq=2 instead of reusing seq=1
- All 337/337 tests pass (335 previously passing + 2 fixed regressions, 0 new regressions)

## Task Commits

Each task was committed atomically:

1. **Task 1: Diagnose and fix tombstone sync regression** - `ff6a70e` (fix)

## Files Created/Modified
- `db/storage/storage.cpp` - Zero-hash sentinel in delete_blob_data instead of seq_map entry erasure; skip sentinels in get_hashes_by_namespace
- `db/storage/storage.h` - Updated doc comment for delete_blob_data to reflect sentinel behavior

## Decisions Made
- Fixed at storage layer rather than cursor logic: the seq_num reuse was a storage correctness bug that would affect any consumer relying on monotonic seq_nums, not just cursors
- Zero-hash sentinel chosen over alternatives (high-water mark key, ghost entries with real hashes) for minimal invasiveness: existing get_blobs_by_seq already handles missing blobs gracefully, and get_hashes_by_namespace only needed a single memcmp check added

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fix applied to storage layer instead of peer_manager.cpp**
- **Found during:** Task 1 (root cause analysis)
- **Issue:** Plan listed `db/peer/peer_manager.cpp` as the file to fix, but the root cause was in `db/storage/storage.cpp` where `delete_blob_data` erased seq_map entries, enabling seq_num reuse
- **Fix:** Applied fix to storage.cpp (root cause) rather than adding workarounds in peer_manager.cpp (symptom)
- **Files modified:** db/storage/storage.cpp, db/storage/storage.h
- **Verification:** All 337 tests pass including both previously-failing tombstone tests
- **Committed in:** ff6a70e

---

**Total deviations:** 1 auto-fixed (1 bug fix in different file than planned)
**Impact on plan:** Fix is more correct and minimal than a cursor-logic workaround would have been. No scope creep.

## Issues Encountered
None -- root cause was identified from the first test run's log output (both sides reporting cursor HIT after tombstone addition, meaning seq_num was unchanged) without needing temporary debug logging.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 34 gap closure complete: all 4 SYNC requirements fully satisfied
- All 337 tests pass with zero regressions
- Cursor-based sync correctly handles all mutation types (store, delete/tombstone)
- Ready for next phase (quota enforcement or performance optimization)

---
*Phase: 34-sync-resumption*
*Completed: 2026-03-18*
