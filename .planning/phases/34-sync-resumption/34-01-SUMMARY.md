---
phase: 34-sync-resumption
plan: 01
subsystem: database
tags: [libmdbx, sync-cursor, persistence, config]

# Dependency graph
requires: []
provides:
  - SyncCursor struct and 7-method CRUD API on Storage
  - cursor sub-database (6th mdbx map) with [peer_hash:32][ns:32] keys
  - full_resync_interval and cursor_stale_seconds config fields
affects: [34-02-sync-orchestration]

# Tech tracking
tech-stack:
  added: []
  patterns: [read-then-write for cursor batch deletion, 20-byte big-endian cursor value encoding]

key-files:
  created: []
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/config/config.h
    - db/config/config.cpp
    - db/tests/storage/test_storage.cpp
    - db/tests/config/test_config.cpp

key-decisions:
  - "Cursor value encoding: [seq_num_be:8][round_count_be:4][last_sync_ts_be:8] = 20 bytes"
  - "delete_peer_cursors uses read-then-write pattern to avoid mdbx cursor invalidation after erase"
  - "cleanup_stale_cursors delegates to list_cursor_peers + delete_peer_cursors for simplicity"

patterns-established:
  - "Cursor key pattern: [peer_hash:32][namespace:32] -- same 64-byte layout as blob keys"
  - "Big-endian encoding for all cursor value fields (consistent with existing seq/expiry patterns)"

requirements-completed: [SYNC-01, SYNC-03]

# Metrics
duration: 11min
completed: 2026-03-17
---

# Phase 34 Plan 01: Cursor Persistence Layer Summary

**SyncCursor CRUD with libmdbx persistence (6th sub-database) and full_resync_interval/cursor_stale_seconds config fields**

## Performance

- **Duration:** 11 min
- **Started:** 2026-03-17T16:06:36Z
- **Completed:** 2026-03-17T16:17:31Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 6

## Accomplishments
- SyncCursor struct with seq_num, round_count, last_sync_timestamp
- 7 public cursor methods on Storage (get/set/delete, delete_peer, reset_all, list_peers, cleanup_stale)
- cursor sub-database with max_maps bumped from 6 to 7
- Config fields: full_resync_interval (default 10), cursor_stale_seconds (default 3600)
- 18 new test cases, all 326 tests pass

## Task Commits

Each task was committed atomically (TDD):

1. **Task 1 RED: Failing tests** - `8b6d1d4` (test)
2. **Task 1 GREEN: Implementation** - `43e5e40` (feat)

## Files Created/Modified
- `db/storage/storage.h` - Added SyncCursor struct and 7 cursor CRUD method declarations
- `db/storage/storage.cpp` - cursor_map sub-database, cursor value encode/decode, all 7 CRUD implementations
- `db/config/config.h` - Added full_resync_interval and cursor_stale_seconds fields
- `db/config/config.cpp` - JSON parsing for new config fields
- `db/tests/storage/test_storage.cpp` - 13 new cursor test cases (CRUD, persistence, cleanup)
- `db/tests/config/test_config.cpp` - 5 new config test cases (defaults, parsing, omission)

## Decisions Made
- Cursor value is 20 bytes: 8 (seq_num) + 4 (round_count) + 8 (last_sync_timestamp), all big-endian
- Used read-then-write for delete_peer_cursors to avoid mdbx cursor invalidation after erase
- cleanup_stale_cursors composes list_cursor_peers + delete_peer_cursors (no separate cursor iteration)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed cursor erase causing MDBX_ENODATA**
- **Found during:** Task 1 GREEN (implementation)
- **Issue:** In-place cursor.erase() in delete_peer_cursors invalidated libmdbx cursor position, causing MDBX_ENODATA on subsequent iteration
- **Fix:** Changed to read-then-write pattern: collect keys in read txn, then erase in write txn
- **Files modified:** db/storage/storage.cpp
- **Verification:** All cursor tests pass including cleanup_stale_cursors
- **Committed in:** 43e5e40 (Task 1 GREEN commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Essential fix for correctness. No scope creep.

## Issues Encountered
None beyond the auto-fixed cursor erase issue.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plan 02 can call storage_.get_sync_cursor() / set_sync_cursor() etc. without additional storage work
- Config fields ready for sync orchestration to read full_resync_interval and cursor_stale_seconds

## Self-Check: PASSED

All 6 files verified present. Both commits (8b6d1d4, 43e5e40) verified in git log.

---
*Phase: 34-sync-resumption*
*Completed: 2026-03-17*
