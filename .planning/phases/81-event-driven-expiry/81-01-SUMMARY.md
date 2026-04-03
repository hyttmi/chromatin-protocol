---
phase: 81-event-driven-expiry
plan: 01
subsystem: database
tags: [mdbx, cursor, expiry, storage, o1-query]

# Dependency graph
requires: []
provides:
  - "Storage::get_earliest_expiry() -- O(1) MDBX cursor query for earliest expiry timestamp"
affects: [81-02 (PeerManager event-driven timer uses this method)]

# Tech tracking
tech-stack:
  added: []
  patterns: [read-only MDBX cursor seek for O(1) min-key queries]

key-files:
  created: []
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/tests/storage/test_storage.cpp

key-decisions:
  - "Used read-only transaction (start_read) for const query -- no write lock contention"
  - "Returns nullopt on empty expiry_map or key length < 8 (defensive)"

patterns-established:
  - "MDBX cursor.to_first() for O(1) min-key queries on sorted sub-databases"

requirements-completed: [MAINT-01]

# Metrics
duration: 13min
completed: 2026-04-03
---

# Phase 81 Plan 01: Storage Earliest Expiry Query Summary

**O(1) MDBX cursor query for earliest expiry timestamp via Storage::get_earliest_expiry()**

## Performance

- **Duration:** 13 min
- **Started:** 2026-04-03T14:00:57Z
- **Completed:** 2026-04-03T14:13:55Z
- **Tasks:** 1
- **Files modified:** 3

## Accomplishments
- Added Storage::get_earliest_expiry() method using MDBX cursor seek to first key in expiry_map
- O(1) query leveraging big-endian expiry timestamp sort order
- 4 unit tests covering empty, single, multiple, and post-scan scenarios
- All 96 existing storage tests pass (no regression)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add Storage::get_earliest_expiry() method** - `2e264ef` (test: RED phase), `a545373` (feat: GREEN phase)

## Files Created/Modified
- `db/storage/storage.h` - Added get_earliest_expiry() declaration (const method returning std::optional<uint64_t>)
- `db/storage/storage.cpp` - Implemented get_earliest_expiry() using read-only MDBX cursor seek
- `db/tests/storage/test_storage.cpp` - Added 4 TEST_CASEs with [storage][earliest-expiry] tag

## Decisions Made
- Used read-only transaction (start_read) since this is a const query with no mutation needed
- Returns nullopt defensively when key_data.length() < 8 (malformed entry guard)
- Placed implementation between run_expiry_scan() and used_bytes() for logical grouping

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functionality is fully wired.

## Next Phase Readiness
- get_earliest_expiry() is ready for Plan 02 to use in PeerManager event-driven expiry timer
- Method is const and uses read-only transactions, safe to call from any context

## Self-Check: PASSED

All artifacts verified:
- db/storage/storage.h: FOUND
- db/storage/storage.cpp: FOUND
- db/tests/storage/test_storage.cpp: FOUND
- 81-01-SUMMARY.md: FOUND
- Commit 2e264ef (RED): FOUND
- Commit a545373 (GREEN): FOUND

---
*Phase: 81-event-driven-expiry*
*Completed: 2026-04-03*
