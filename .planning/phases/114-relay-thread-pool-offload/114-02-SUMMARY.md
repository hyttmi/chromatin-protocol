---
phase: 114-relay-thread-pool-offload
plan: 02
subsystem: relay
tags: [asio, thread-pool, coroutine, offload, aead, translation, c++20]

# Dependency graph
requires:
  - phase: 114-relay-thread-pool-offload
    plan: 01
    provides: "offload_if_large() helper, pool_ references in DataHandlers/QueryHandlerDeps/UdsMultiplexer"
provides:
  - "All 11 CPU-heavy call sites wrapped with conditional offload_if_large()"
  - "AEAD encrypt/decrypt with counter-by-value capture before offload"
  - "Notification/broadcast pre-translation in read_loop() coroutine"
  - "route_broadcast_pretranslated and handle_notification_pretranslated methods"
affects: [relay-performance, relay-large-blob-handling]

# Tech tracking
tech-stack:
  added: []
  patterns: ["counter-by-value before AEAD offload (D-10)", "pre-translate in coroutine then dispatch to sync handler"]

key-files:
  created: []
  modified:
    - "relay/http/handlers_query.cpp"
    - "relay/http/handlers_data.cpp"
    - "relay/core/uds_multiplexer.h"
    - "relay/core/uds_multiplexer.cpp"

key-decisions:
  - "Query JSON json_to_binary uses size=0 (always inline) since query payloads are sub-KB"
  - "AEAD counter increment happens on event loop BEFORE offload lambda to prevent race"
  - "Notification/broadcast pre-translated in read_loop() coroutine since route_response/handle_notification are sync void"
  - "route_response() simplified to client-response-only path after read_loop() handles request_id==0"
  - "Original handle_notification() kept for potential non-offloaded callers"

patterns-established:
  - "Counter-by-value pattern: auto counter = counter_++; then capture counter in lambda"
  - "Pre-translate in coroutine, dispatch to sync handler for fan-out"

requirements-completed: [OFF-03, OFF-04]

# Metrics
duration: 10min
completed: 2026-04-14
---

# Phase 114 Plan 02: Call Site Wrapping Summary

**All 11 translation + AEAD call sites wrapped with offload_if_large(), AEAD counters captured by value, notification pre-translation in read_loop() coroutine**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-14T10:23:28Z
- **Completed:** 2026-04-14T10:33:47Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Wrapped 6 HTTP handler translation call sites (2 in handlers_query.cpp, 4 in handlers_data.cpp) with offload_if_large()
- Wrapped 2 AEAD encrypt/decrypt call sites with counter-by-value capture before offload
- Lifted 3 notification/broadcast binary_to_json calls from sync handlers into read_loop() coroutine with offload_if_large()
- Added route_broadcast_pretranslated() and handle_notification_pretranslated() methods for pre-translated dispatch
- Simplified route_response() to client-response-only path
- Zero test regressions: all 209 relay tests pass (2485 assertions)

## Task Commits

Each task was committed atomically:

1. **Task 1: Wrap HTTP handler translation calls with offload_if_large()** - `ab79df2d` (feat)
2. **Task 2: Wrap UDS AEAD + refactor notification translation into read_loop()** - `1fa5c93c` (feat)

## Files Created/Modified
- `relay/http/handlers_query.cpp` - forward_query() json_to_binary (size=0, always inline) and binary_to_json (payload.size()) wrapped
- `relay/http/handlers_data.cpp` - WriteAck, DeleteAck, BatchReadRequest, BatchReadResponse translation calls wrapped
- `relay/core/uds_multiplexer.h` - Added route_broadcast_pretranslated() and handle_notification_pretranslated() declarations, nlohmann/json_fwd.hpp include
- `relay/core/uds_multiplexer.cpp` - AEAD counter-by-value, read_loop() pre-translation, new pretranslated methods, route_response() simplified

## Decisions Made
- Query JSON json_to_binary uses size=0 as threshold input (per RESEARCH D-08: query payloads are always sub-KB, no need for .dump().size() allocation)
- AEAD counters captured by value on event loop thread before offload (D-10: prevents counter race between event loop and pool thread)
- Notification/broadcast paths pre-translate in read_loop() because route_response() and handle_notification() are synchronous void -- cannot co_await inside them
- route_response() request_id==0 block removed entirely (read_loop handles server-initiated messages directly)
- Original handle_notification() retained alongside handle_notification_pretranslated() to avoid dead code concerns

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Initial build failed with Error 127 when sharing FetchContent deps from main repo build dir -- resolved by clean cmake configure with local _deps directory

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- All 11 offload sites wrapped -- event loop starvation eliminated for large payloads
- Small payloads (below 64 KB) execute inline with zero thread hop overhead
- Ready for Phase 114 Plan 03 (verification/benchmarking) if planned

## Self-Check: PASSED

All 4 modified files verified present. Both commit hashes (ab79df2d, 1fa5c93c) verified in git log.

---
*Phase: 114-relay-thread-pool-offload*
*Completed: 2026-04-14*
