---
phase: 11-larger-blob-support
plan: 01
subsystem: engine
tags: [protocol-constants, size-validation, framing, ingest]

requires:
  - phase: 10-access-control
    provides: engine ingest pipeline, framing layer
provides:
  - MAX_BLOB_DATA_SIZE constant (100 MiB) for protocol-wide size limits
  - MAX_FRAME_SIZE raised to 110 MiB for large blob transport
  - Step 0 oversized_blob rejection in BlobEngine::ingest
affects: [11-02, 11-03]

tech-stack:
  added: []
  patterns:
    - "Step 0 cheapest-first validation (integer comparison before crypto)"

key-files:
  created: []
  modified:
    - db/net/framing.h
    - db/engine/engine.h
    - db/engine/engine.cpp
    - tests/engine/test_engine.cpp
    - tests/net/test_framing.cpp

key-decisions:
  - "MAX_FRAME_SIZE set to 110 MiB (10% headroom over 100 MiB blob + overhead)"
  - "MAX_BLOB_DATA_SIZE is uint64_t to prevent overflow in downstream calculations"
  - "Size check is Step 0 in ingest -- single integer comparison before any crypto"

patterns-established:
  - "Protocol size constants co-located in framing.h with static_assert relationship"
  - "Step 0 pattern: cheapest validation first in ingest pipeline"

requirements-completed: [BLOB-01, BLOB-02, BLOB-05]

duration: 12min
completed: 2026-03-07
---

# Phase 11 Plan 01: Protocol Constants + Size Validation Summary

**MAX_BLOB_DATA_SIZE (100 MiB) and MAX_FRAME_SIZE (110 MiB) as constexpr protocol invariants with Step 0 oversized blob rejection in ingest pipeline**

## Performance

- **Duration:** 12 min
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- MAX_BLOB_DATA_SIZE (100 MiB) and MAX_FRAME_SIZE (110 MiB) defined as constexpr protocol invariants in framing.h with static_assert relationship
- IngestError::oversized_blob added as Step 0 in BlobEngine::ingest, executing before any crypto validation
- 8 new test assertions across framing and engine test suites (constant verification, boundary tests, ordering tests)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add MAX_BLOB_DATA_SIZE constant and raise MAX_FRAME_SIZE** - `3747120` (feat)
2. **Task 2: Add oversized_blob rejection as Step 0 in BlobEngine::ingest** - `a753fff` (feat)

## Files Created/Modified
- `db/net/framing.h` - Added MAX_BLOB_DATA_SIZE, raised MAX_FRAME_SIZE to 110 MiB, added static_assert
- `db/engine/engine.h` - Added oversized_blob to IngestError enum
- `db/engine/engine.cpp` - Added Step 0 size check before structural validation
- `tests/net/test_framing.cpp` - Added constant verification and 1 MiB round-trip tests
- `tests/engine/test_engine.cpp` - Added oversized blob rejection tests (5 sections)

## Decisions Made
- MAX_FRAME_SIZE set to 110 MiB (10% headroom over 100 MiB blob + ~7 KB protocol overhead)
- MAX_BLOB_DATA_SIZE uses uint64_t to prevent integer overflow in downstream size calculations
- Size check is Step 0 (single integer comparison) -- cheapest possible validation before pubkey/sig/namespace checks

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Missing include in test file**
- **Found during:** Task 2 (oversized blob tests)
- **Issue:** tests/engine/test_engine.cpp referenced chromatindb::net::MAX_BLOB_DATA_SIZE without including db/net/framing.h
- **Fix:** Added `#include "db/net/framing.h"` to test file
- **Files modified:** tests/engine/test_engine.cpp
- **Verification:** Build succeeds, all 188 tests pass
- **Committed in:** a753fff (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Trivial include fix. No scope creep.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Protocol constants established; Plan 11-02 (hash index) and Plan 11-03 (sync optimization) can proceed
- All 188 tests pass

---
*Phase: 11-larger-blob-support*
*Completed: 2026-03-07*
