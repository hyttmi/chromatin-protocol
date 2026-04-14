---
phase: 114-relay-thread-pool-offload
plan: 01
subsystem: relay
tags: [asio, thread-pool, coroutine, offload, c++20]

# Dependency graph
requires:
  - phase: 111-single-threaded-rewrite
    provides: "single-threaded event loop model, util::offload() template in thread_pool.h"
provides:
  - "offload_if_large() conditional offload helper with 64KB threshold"
  - "thread pool reference injected into DataHandlers, QueryHandlerDeps, UdsMultiplexer"
  - "relay_main.cpp wiring of offload_pool to all three components"
affects: [114-02, relay-performance]

# Tech tracking
tech-stack:
  added: []
  patterns: ["conditional offload: inline < 64KB, pool > 64KB with transfer-back"]

key-files:
  created:
    - "relay/util/offload_if_large.h"
    - "relay/tests/test_offload_if_large.cpp"
  modified:
    - "relay/http/handlers_data.h"
    - "relay/http/handlers_data.cpp"
    - "relay/http/handlers_query.h"
    - "relay/http/handlers_query.cpp"
    - "relay/core/uds_multiplexer.h"
    - "relay/core/uds_multiplexer.cpp"
    - "relay/relay_main.cpp"
    - "relay/tests/CMakeLists.txt"

key-decisions:
  - "offload_if_large() hardcodes OFFLOAD_THRESHOLD=65536 (not parameterized) for simplicity"
  - "pool added as last constructor parameter to all three components for minimal API disruption"
  - "QueryHandlerDeps uses pointer (thread_pool*) not reference for aggregate init compatibility"

patterns-established:
  - "Conditional offload pattern: co_await offload_if_large(pool_, ioc_, size, fn)"
  - "Transfer-back encapsulated inside helper -- callers never forget asio::post(ioc)"

requirements-completed: [OFF-01, OFF-02]

# Metrics
duration: 9min
completed: 2026-04-14
---

# Phase 114 Plan 01: Offload Infrastructure Summary

**offload_if_large() coroutine helper with 64KB threshold, thread pool injected into DataHandlers/QueryHandlerDeps/UdsMultiplexer via constructor, 6 unit tests**

## Performance

- **Duration:** 9 min
- **Started:** 2026-04-14T10:11:27Z
- **Completed:** 2026-04-14T10:21:11Z
- **Tasks:** 2
- **Files modified:** 10

## Accomplishments
- Created relay/util/offload_if_large.h with conditional offload (inline below 64KB, thread pool above)
- Injected asio::thread_pool& into DataHandlers, QueryHandlerDeps, and UdsMultiplexer constructors
- Wired offload_pool from relay_main.cpp to all three components
- 6 unit tests verifying threshold boundary, inline/offload paths, transfer-back, and return value propagation
- Zero test regressions: all 209 existing relay tests + 6 new tests pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Create offload_if_large.h + wire pool** - `966fe793` (feat)
2. **Task 2: Unit tests for offload threshold** - `95634f34` (test)

## Files Created/Modified
- `relay/util/offload_if_large.h` - Conditional offload coroutine template with OFFLOAD_THRESHOLD=65536
- `relay/http/handlers_data.h` - Added pool parameter to DataHandlers constructor + pool_ member
- `relay/http/handlers_data.cpp` - Updated constructor, added offload_if_large.h include
- `relay/http/handlers_query.h` - Added pool field to QueryHandlerDeps struct
- `relay/http/handlers_query.cpp` - Updated forward_query signature + all 12 call sites + lambda captures
- `relay/core/uds_multiplexer.h` - Added pool parameter to UdsMultiplexer constructor + pool_ member
- `relay/core/uds_multiplexer.cpp` - Updated constructor, added offload_if_large.h include
- `relay/relay_main.cpp` - Passes offload_pool to UdsMultiplexer, DataHandlers, QueryHandlerDeps
- `relay/tests/test_offload_if_large.cpp` - 6 test cases, 9 assertions for threshold behavior
- `relay/tests/CMakeLists.txt` - Added test_offload_if_large executable

## Decisions Made
- offload_if_large() uses a hardcoded threshold constant (not a parameter) since D-07 mandates the same 64KB threshold everywhere
- QueryHandlerDeps uses `asio::thread_pool*` (pointer) rather than reference to maintain aggregate initialization compatibility
- pool_ placed as last constructor parameter in all three components to minimize diff and preserve existing call patterns

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- FetchContent network fetch for Catch2 failed when building from worktree -- resolved by sharing FetchContent cache via `-DFETCHCONTENT_BASE_DIR` pointing to main repo's build/_deps

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Infrastructure complete: offload_if_large.h, pool references, and unit tests all in place
- Plan 02 can now wrap json_to_binary, binary_to_json, aead_encrypt, and aead_decrypt call sites using pool_ members

## Self-Check: PASSED

All 10 files verified present. Both commit hashes (966fe793, 95634f34) verified in git log.

---
*Phase: 114-relay-thread-pool-offload*
*Completed: 2026-04-14*
