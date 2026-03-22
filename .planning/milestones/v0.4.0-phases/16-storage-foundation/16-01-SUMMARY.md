---
phase: 16-storage-foundation
plan: 01
subsystem: database
tags: [mdbx, tombstone, indexing, storage, sub-database]

# Dependency graph
requires:
  - phase: 12-tombstone-deletion
    provides: "has_tombstone_for(), wire::is_tombstone(), wire::extract_tombstone_target(), tombstone codec"
  - phase: 13-delegation
    provides: "delegation_map pattern (indexed sub-database with compound key)"
provides:
  - "tombstone_map sub-database for O(1) tombstone existence checks"
  - "used_bytes() method returning mdbx database file size"
  - "One-time startup migration for existing tombstone blobs"
affects: [16-02-storage-limits, 16-03-disk-full-signaling]

# Tech tracking
tech-stack:
  added: []
  patterns: ["tombstone_map indexed sub-database following delegation_map pattern", "startup migration with batched write transactions"]

key-files:
  created: []
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - tests/storage/test_storage.cpp

key-decisions:
  - "Forward-compatible migration: one-time startup scan populates tombstone_map from existing tombstone blobs in blobs_map, batched in groups of 1000"
  - "used_bytes() uses env.get_info().mi_geo.current (authoritative mdbx file size, O(1), no drift on crash recovery)"
  - "tombstone_map value is empty (existence check only) -- no need to store the tombstone blob hash"

patterns-established:
  - "Indexed sub-database pattern: compound key [namespace:32][hash:32], empty value for existence checks, same-transaction writes in store_blob()"
  - "Startup migration pattern: read-only scan with batched write transactions to avoid oversized transactions"

requirements-completed: [STOR-01]

# Metrics
duration: 27min
completed: 2026-03-09
---

# Phase 16 Plan 01: Tombstone Index Summary

**O(1) tombstone lookup via dedicated mdbx sub-database (tombstone_map) replacing O(n) namespace scan, plus used_bytes() for capacity tracking**

## Performance

- **Duration:** 27 min
- **Started:** 2026-03-09T15:27:47Z
- **Completed:** 2026-03-09T15:54:48Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Replaced O(n) cursor scan in has_tombstone_for() with O(1) indexed lookup via tombstone_map sub-database
- Added tombstone_map as 5th mdbx sub-database with atomic index writes in store_blob() and cleanup in delete_blob_data()
- Added used_bytes() method returning authoritative database file size via env.get_info().mi_geo.current
- Implemented one-time startup migration that populates tombstone_map from existing tombstone blobs (batched in groups of 1000)
- 7 new test cases covering O(1) lookup, cleanup, multi-key tracking, cross-namespace isolation, and used_bytes

## Task Commits

Each task was committed atomically:

1. **Task 1: Add tombstone_map sub-database, O(1) lookup, and used_bytes** - `ed60bc8` (test: RED) / `3c50f97` (feat: GREEN)
2. **Task 2: Add tombstone index and used_bytes tests** - `60cf3e3` (test: additional coverage)

**Plan metadata:** (pending)

_Note: TDD tasks have RED then GREEN commits_

## Files Created/Modified
- `db/storage/storage.h` - Added used_bytes() declaration, updated docstring to 5 sub-databases, updated has_tombstone_for() documentation to O(1)
- `db/storage/storage.cpp` - Added tombstone_map DBI, tombstone indexing in store_blob(), O(1) has_tombstone_for(), tombstone cleanup in delete_blob_data(), used_bytes() implementation, startup migration
- `tests/storage/test_storage.cpp` - 7 new test cases: O(1) lookup, false-for-nonexistent, cleanup on delete, multi-key tracking, cross-namespace isolation, used_bytes after store, used_bytes on empty

## Decisions Made
- **Startup migration over forward-only indexing:** Full migration at startup ensures O(1) guarantee is unconditional for all existing tombstones, not just newly stored ones. Batched in groups of 1000 to avoid oversized transactions.
- **used_bytes() via mi_geo.current:** Authoritative mdbx file size, O(1), no drift on crash recovery. An in-memory counter would need initialization and could drift.
- **Empty tombstone_map value:** Only existence checking is needed; storing the tombstone blob hash would waste space for no benefit.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- tombstone_map and used_bytes() are ready for Plan 16-02 (storage capacity limits)
- used_bytes() provides the capacity check input for Step 0 enforcement
- All 53 storage tests pass (146 assertions)

## Self-Check: PASSED

All files exist, all commits verified, all tests pass.

---
*Phase: 16-storage-foundation*
*Completed: 2026-03-09*
