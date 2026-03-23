---
phase: 60-codebase-deduplication-audit
plan: 01
subsystem: infra
tags: [c++20, header-only, hex-encoding, deduplication, refactor]

# Dependency graph
requires: []
provides:
  - "db/util/hex.h: shared header-only hex encoding/decoding utility in chromatindb::util namespace"
  - "All 6 production files consolidated to use shared hex functions"
affects: [60-02 (test file hex deduplication uses same shared header)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Shared utility headers in db/util/ namespace chromatindb::util"
    - "using-declarations in anonymous namespace for unqualified access to shared utilities"

key-files:
  created:
    - db/util/hex.h
  modified:
    - db/main.cpp
    - db/peer/peer_manager.cpp
    - relay/relay_main.cpp
    - relay/core/relay_session.cpp
    - loadgen/loadgen_main.cpp
    - tools/verify_main.cpp

key-decisions:
  - "Header-only utility with inline functions to avoid ODR violations across translation units"
  - "from_hex_safe() non-throwing wrapper in verify_main.cpp preserves CLI error handling pattern"
  - "Explicit max_len=8 added to truncated logging calls in peer_manager.cpp (previously implicit default)"

patterns-established:
  - "db/util/ directory for shared utility headers"
  - "chromatindb::util namespace for cross-module utilities"

requirements-completed: [DEDUP-01, DEDUP-03, DEDUP-04]

# Metrics
duration: 16min
completed: 2026-03-23
---

# Phase 60 Plan 01: Production Hex Deduplication Summary

**Shared db/util/hex.h header replacing 11 duplicated to_hex/from_hex copies across 6 production files with 4 unified overloads (to_hex, to_hex truncating, from_hex, from_hex_fixed)**

## Performance

- **Duration:** 16 min
- **Started:** 2026-03-23T16:26:47Z
- **Completed:** 2026-03-23T16:42:50Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Created db/util/hex.h with 4 functions: to_hex(span), to_hex(span, max_bytes), from_hex(string), from_hex_fixed<N>(string)
- Removed 11 anonymous-namespace hex function copies from 6 production files (net -124 lines)
- All 514 non-E2E tests pass; 13 pre-existing E2E failures unchanged

## Task Commits

Each task was committed atomically:

1. **Task 1: Create db/util/hex.h shared header** - `b80eb31` (feat)
2. **Task 2: Replace all production to_hex/from_hex copies** - `96bd5e7` (refactor)

## Files Created/Modified
- `db/util/hex.h` - Shared hex encoding/decoding utility (header-only, namespace chromatindb::util)
- `db/main.cpp` - Replaced anonymous-namespace to_hex with shared header using-declaration
- `db/peer/peer_manager.cpp` - Replaced truncating to_hex with shared header, added explicit max_len=8 to 10 call sites
- `relay/relay_main.cpp` - Replaced anonymous-namespace to_hex with shared header using-declaration
- `relay/core/relay_session.cpp` - Replaced anonymous-namespace to_hex with shared header, removed empty anonymous namespace
- `loadgen/loadgen_main.cpp` - Replaced 4 hex functions (to_hex x2, from_hex, from_hex_bytes) with shared from_hex_fixed<32> and from_hex
- `tools/verify_main.cpp` - Replaced 3 hex functions with shared header plus from_hex_safe() non-throwing wrapper

## Decisions Made
- Header-only with all functions marked `inline` to avoid ODR violations (chromatindb_tests links multiple TUs)
- Added `from_hex_safe()` non-throwing wrapper in verify_main.cpp because the CLI tool checks `empty()` returns instead of catching exceptions, preserving the existing error-handling pattern
- Made default `max_len=8` explicit in peer_manager.cpp calls rather than adding a default parameter to the shared header (shared header has no opinion about truncation defaults)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Non-throwing from_hex in verify_main.cpp**
- **Found during:** Task 2 (verify_main.cpp replacement)
- **Issue:** verify_main.cpp's local from_hex returned empty vector on invalid input (non-throwing). Shared from_hex throws std::invalid_argument. Direct replacement would change error behavior.
- **Fix:** Added from_hex_safe() local wrapper that catches std::invalid_argument and returns empty vector, preserving existing CLI error handling pattern.
- **Files modified:** tools/verify_main.cpp
- **Verification:** Build succeeds, all tests pass
- **Committed in:** 96bd5e7 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug prevention)
**Impact on plan:** Essential for correctness -- changed error semantics would break verify tool's CLI error handling.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- db/util/hex.h available for Plan 60-02 (test file hex deduplication)
- Test files still contain their own hex function copies -- Plan 60-02 will consolidate those

## Self-Check: PASSED

All 7 files verified present. Both commits (b80eb31, 96bd5e7) verified in git log.

---
*Phase: 60-codebase-deduplication-audit*
*Completed: 2026-03-23*
