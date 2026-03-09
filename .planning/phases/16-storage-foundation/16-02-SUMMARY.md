---
phase: 16-storage-foundation
plan: 02
subsystem: database
tags: [mdbx, capacity, config, ingest, storage-limits]

# Dependency graph
requires:
  - phase: 16-storage-foundation
    plan: 01
    provides: "used_bytes() method for capacity checking"
provides:
  - "max_storage_bytes config field with JSON parsing"
  - "IngestError::storage_full enum value"
  - "Step 0b capacity check in BlobEngine::ingest() before crypto ops"
  - "Tombstone exemption from capacity check"
affects: [16-03-disk-full-signaling]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Step 0b capacity gate pattern: cheapest check (int compare) before expensive ops (crypto)"]

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/main.cpp
    - tests/config/test_config.cpp
    - tests/engine/test_engine.cpp

key-decisions:
  - "Step 0b placement: capacity check after oversized_blob (Step 0a) but before structural/namespace/signature checks"
  - "Tombstone exemption: tombstones bypass capacity check because they free space (delete target blob) and are always small (36 bytes)"
  - "max_storage_bytes=0 means unlimited (backward-compatible default, no check performed)"

patterns-established:
  - "Step 0b capacity gate: check storage_.used_bytes() >= max_storage_bytes_ before any crypto operations"
  - "Config field with default 0 = disabled/unlimited pattern"

requirements-completed: [STOR-02, STOR-03]

# Metrics
duration: 25min
completed: 2026-03-09
---

# Phase 16 Plan 02: Storage Capacity Limits Summary

**Configurable max_storage_bytes with Step 0b capacity gate rejecting IngestError::storage_full before crypto, tombstone-exempt**

## Performance

- **Duration:** 25 min
- **Started:** 2026-03-09T16:04:38Z
- **Completed:** 2026-03-09T17:40:08Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Added max_storage_bytes config field (default 0 = unlimited) with JSON parsing
- Added IngestError::storage_full enum value for capacity rejection
- Implemented Step 0b capacity check in BlobEngine::ingest() before any crypto operations
- Tombstone blobs exempt from capacity check (they free space by deleting target)
- BlobEngine constructor accepts max_storage_bytes, main.cpp passes config value
- 3 config tests + 5 engine capacity tests (270 total tests, 1042 assertions)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add max_storage_bytes config field and IngestError::storage_full** - `533b5d3` (feat)
2. **Task 2: Add config and engine capacity tests** - `783b82e` (test)

_Note: TDD approach -- tests written alongside implementation in Task 1, edge-case tests added in Task 2_

## Files Created/Modified
- `db/config/config.h` - Added max_storage_bytes field to Config struct (default 0)
- `db/config/config.cpp` - Added JSON parsing for max_storage_bytes using j.value() pattern
- `db/engine/engine.h` - Added IngestError::storage_full, BlobEngine constructor with max_storage_bytes param, private max_storage_bytes_ member
- `db/engine/engine.cpp` - Added Step 0b capacity check in ingest(), updated constructor
- `db/main.cpp` - Pass config.max_storage_bytes to BlobEngine constructor
- `tests/config/test_config.cpp` - 3 new tests: JSON parsing, default value, Config struct default
- `tests/engine/test_engine.cpp` - 5 new tests: over capacity rejection, tombstone exemption, unlimited mode, under capacity, delete_blob when over capacity

## Decisions Made
- **Step 0b placement:** Capacity check comes after the existing Step 0 oversized_blob check but before all other validation (structural, namespace, signature). This maximizes the number of rejected blobs that skip expensive crypto.
- **Tombstone exemption:** Tombstones are always small (36 bytes) and they free space by deleting the target blob. Blocking tombstones when storage is full would prevent cleanup.
- **No capacity check in delete_blob():** The delete_blob() method creates tombstones, which are inherently exempt. No code change needed.
- **Default parameter in constructor:** `BlobEngine(store, max_storage_bytes = 0)` preserves backward compatibility -- all existing call sites and tests continue to work unchanged.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Storage capacity enforcement is complete and testable
- Ready for Plan 16-03 (disk-full signaling / graceful degradation)
- All 270 tests pass (1042 assertions)

## Self-Check: PASSED

All files exist, all commits verified, all tests pass.

---
*Phase: 16-storage-foundation*
*Completed: 2026-03-09*
