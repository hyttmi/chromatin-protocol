---
phase: 95-code-deduplication
plan: 01
subsystem: database
tags: [endian, utility, deduplication, refactor, c++20]

# Dependency graph
requires: []
provides:
  - "db/util/endian.h: BE read/write helpers for u16/u32/u64 (span + pointer overloads)"
  - "db/util/blob_helpers.h: namespace/hash extraction and blob ref encoding"
  - "Zero inline BE helpers in sync_protocol.cpp, reconciliation.cpp, storage.cpp, framing.cpp"
affects: [95-02, 95-03, 96, 97, 98, 99]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Shared utility headers under db/util/ with chromatindb::util namespace"
    - "Span overloads for bounds-checked reads, pointer overloads for pre-validated paths"
    - "Test files under db/tests/util/ matching header structure"

key-files:
  created:
    - db/util/endian.h
    - db/util/blob_helpers.h
    - db/tests/util/test_endian.cpp
    - db/tests/util/test_blob_helpers.cpp
  modified:
    - db/sync/sync_protocol.cpp
    - db/sync/reconciliation.cpp
    - db/storage/storage.cpp
    - db/net/framing.cpp
    - db/CMakeLists.txt

key-decisions:
  - "Span overloads throw std::out_of_range; pointer overloads are unchecked (matching existing safety contracts)"
  - "store_u32_be/store_u64_be take destination-first argument order (consistent with memcpy convention)"
  - "framing.cpp make_nonce() BE counter replaced with store_u64_be (beyond plan scope but same pattern)"

patterns-established:
  - "Utility headers: inline-only in db/util/, namespace chromatindb::util, following hex.h pattern"
  - "Bounds checking: span overloads for external/untrusted input, pointer overloads for internal pre-validated paths"

requirements-completed: [DEDUP-01, DEDUP-04, DEDUP-05]

# Metrics
duration: 14min
completed: 2026-04-07
---

# Phase 95 Plan 01: Endian/Blob Utility Headers Summary

**Two shared utility headers (endian.h, blob_helpers.h) with 36 tests, replacing all inline BE encode/decode in sync, storage, and framing layers (net -95 lines)**

## Performance

- **Duration:** 14 min (effective work; 140 min wall time including CMake compilation)
- **Started:** 2026-04-07T13:52:57Z
- **Completed:** 2026-04-07T16:13:00Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- Created endian.h with 13 inline functions covering u16/u32/u64 BE read/write in both vector-append, fixed-buffer, span, and raw-pointer variants
- Created blob_helpers.h with 4 inline functions for namespace extraction, namespace+hash encoding, and 77-byte BlobNotify blob ref encoding
- Removed 10 duplicate file-local BE helper functions from 4 source files, replacing with chromatindb::util:: calls
- All 634 tests pass (598 existing + 36 new) with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Create endian.h and blob_helpers.h with dedicated test files** - `d5fce2b` (feat)
2. **Task 2: Replace all inline BE patterns in sync, storage, and framing files** - `62320dd` (refactor)

## Files Created/Modified
- `db/util/endian.h` - BE read/write helpers for u16/u32/u64 with span and pointer overloads
- `db/util/blob_helpers.h` - Namespace/hash extraction and blob ref encoding using endian.h
- `db/tests/util/test_endian.cpp` - 25 test cases covering round-trips, boundaries, bounds checks
- `db/tests/util/test_blob_helpers.cpp` - 11 test cases covering extraction, encoding, round-trips
- `db/sync/sync_protocol.cpp` - Replaced 4 anonymous namespace BE helpers with 12 chromatindb::util:: calls
- `db/sync/reconciliation.cpp` - Replaced 2 anonymous namespace BE helpers with 10 chromatindb::util:: calls
- `db/storage/storage.cpp` - Replaced 4 static BE helpers with 22 chromatindb::util:: calls
- `db/net/framing.cpp` - Replaced 2 inline shift patterns + make_nonce with 3 chromatindb::util:: calls
- `db/CMakeLists.txt` - Added test_endian.cpp and test_blob_helpers.cpp to chromatindb_tests

## Decisions Made
- Span overloads throw std::out_of_range for bounds violations; raw pointer overloads remain unchecked (matching existing safety contracts where callers pre-validate buffer sizes)
- store_u32_be/store_u64_be use destination-first argument order (consistent with memcpy convention, reversed from storage.cpp's encode_be_u64(val, out) pattern)
- Replaced framing.cpp make_nonce() inline BE counter encoding with store_u64_be (deviation Rule 1: same pattern, trivial improvement)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed test_blob_helpers.cpp hash fill indexing**
- **Found during:** Task 1 (TDD GREEN phase)
- **Issue:** Test filled payload bytes 32-63 using `payload[i] = i + 100` (where i=32..63) but checked with `hash[i] == i + 100` (where i=0..31), giving wrong expected values
- **Fix:** Changed fill to `payload[32 + i] = i + 100` (where i=0..31) matching the extraction check
- **Files modified:** db/tests/util/test_blob_helpers.cpp
- **Verification:** Test passes after fix
- **Committed in:** d5fce2b (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug in test)
**Impact on plan:** Trivial test authoring fix. No scope creep.

## Issues Encountered
- CMake build directory was corrupted from concurrent agent operations; cleaned and reconfigured (no code impact)

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functions are fully implemented.

## Next Phase Readiness
- endian.h and blob_helpers.h are ready for use by subsequent plans (95-02 protocol_helpers.h, 95-03 notification encoding consolidation)
- All existing tests verified green, providing a stable base for further deduplication

---
*Phase: 95-code-deduplication*
*Completed: 2026-04-07*

## Self-Check: PASSED

All files exist, all commits found, all acceptance criteria verified.
