---
phase: 23-ttl-flexibility
plan: 01
subsystem: storage
tags: [ttl, expiry, tombstone, mdbx, blob-store]

# Dependency graph
requires:
  - phase: 16-storage-foundation
    provides: tombstone_map index and has_tombstone_for() API
provides:
  - Writer-controlled TTL (BLOB_TTL_SECONDS removed)
  - run_expiry_scan() tombstone_map cleanup for expired tombstones
affects: [24-ttl-enforcement, 25-tombstone-ttl]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Read-before-delete in expiry scan for index cleanup"

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/storage/storage.cpp
    - tests/config/test_config.cpp
    - tests/storage/test_storage.cpp

key-decisions:
  - "No new APIs needed -- existing store_blob() already creates expiry entries for TTL>0 tombstones"

patterns-established:
  - "Read-before-delete: run_expiry_scan() reads blob data before erasing from blobs_map to detect tombstones and clean secondary indexes"

requirements-completed: [TTL-01, TTL-03, TTL-04, TTL-05]

# Metrics
duration: 42min
completed: 2026-03-14
---

# Phase 23 Plan 01: Remove BLOB_TTL_SECONDS Summary

**Writer-controlled TTL replaces hardcoded BLOB_TTL_SECONDS; run_expiry_scan() now cleans tombstone_map when expired tombstones are garbage collected**

## Performance

- **Duration:** 42 min
- **Started:** 2026-03-14T15:06:31Z
- **Completed:** 2026-03-14T15:48:31Z
- **Tasks:** 1
- **Files modified:** 4

## Accomplishments
- Removed BLOB_TTL_SECONDS constexpr from config.h -- writers now control TTL via signed blob data
- Fixed run_expiry_scan() to read blob data before deleting from blobs_map, detecting tombstones and cleaning tombstone_map entries
- Added 3 tombstone expiry lifecycle tests verifying TTL>0 expiry, TTL=0 permanence, and regular blob expiry regression-free
- All 286 tests pass (284 original - 1 removed + 3 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Remove BLOB_TTL_SECONDS and fix run_expiry_scan tombstone_map cleanup** - `cc05dca` (feat)

_Note: TDD task -- RED tests were pre-written; GREEN implementation committed in single commit_

## Files Created/Modified
- `db/config/config.h` - Removed BLOB_TTL_SECONDS constexpr (writers set TTL in signed blob data)
- `db/storage/storage.cpp` - run_expiry_scan() reads blob before delete, detects tombstones, cleans tombstone_map
- `tests/config/test_config.cpp` - Removed BLOB_TTL_SECONDS assertion and dedicated test case
- `tests/storage/test_storage.cpp` - Added 3 tombstone expiry lifecycle tests

## Decisions Made
- No new APIs needed -- existing store_blob() already creates expiry entries for TTL>0 tombstones, so only the scan needed updating

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Writer-controlled TTL foundation is in place
- Ready for Phase 24 (max_ttl enforcement in BlobEngine::ingest) and Phase 25 (tombstone_ttl config)
- No blockers or concerns

## Self-Check: PASSED

- SUMMARY.md: FOUND
- Commit cc05dca: FOUND
- db/config/config.h: FOUND
- db/storage/storage.cpp: FOUND
- tests/config/test_config.cpp: FOUND
- tests/storage/test_storage.cpp: FOUND

---
*Phase: 23-ttl-flexibility*
*Completed: 2026-03-14*
