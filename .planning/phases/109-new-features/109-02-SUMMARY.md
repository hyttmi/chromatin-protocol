---
phase: 109-new-features
plan: 02
subsystem: relay
tags: [health-check, blob-size-limit, http, sighup, prometheus, config]

# Dependency graph
requires:
  - phase: 105-operational-polish
    provides: MetricsCollector HTTP acceptor, RateLimiter, per-session atomic pattern
provides:
  - Configurable max_blob_size_bytes with SIGHUP reload
  - GET /health JSON endpoint on MetricsCollector HTTP acceptor
  - Blob size enforcement before translation in WsSession
affects: [109-03-PLAN, relay-operations, load-balancer-integration]

# Tech tracking
tech-stack:
  added: []
  patterns: [HealthProvider callback for HTTP health checks, shared atomic for SIGHUP-reloadable blob size limits]

key-files:
  created: []
  modified:
    - relay/config/relay_config.h
    - relay/config/relay_config.cpp
    - relay/core/metrics_collector.h
    - relay/core/metrics_collector.cpp
    - relay/ws/ws_session.h
    - relay/ws/ws_session.cpp
    - relay/ws/ws_acceptor.h
    - relay/ws/ws_acceptor.cpp
    - relay/relay_main.cpp
    - relay/tests/test_relay_config.cpp
    - relay/tests/test_metrics_collector.cpp

key-decisions:
  - "Blob size check uses base64 upper-bound estimate (b64_len * 3 / 4) to avoid decoding -- fast rejection"
  - "Health endpoint uses raw string JSON concatenation, not nlohmann::json -- minimal overhead for 3 fixed fields"
  - "blob_too_large error code is relay-only, not added to shared error_codes.h"

patterns-established:
  - "HealthProvider callback: std::function<bool()> injected into MetricsCollector for /health status"
  - "Shared atomic propagation chain: config -> relay_main atomic -> WsAcceptor setter -> WsSession constructor"

requirements-completed: [FEAT-02, FEAT-03]

# Metrics
duration: 9min
completed: 2026-04-13
---

# Phase 109 Plan 02: Blob Size Limit + Health Endpoint Summary

**Relay-side blob size enforcement (max_blob_size_bytes) with SIGHUP reload and JSON /health endpoint returning UDS connectivity status**

## Performance

- **Duration:** 9 min
- **Started:** 2026-04-13T02:31:13Z
- **Completed:** 2026-04-13T02:39:53Z
- **Tasks:** 2/2
- **Files modified:** 11

## Accomplishments
- Data(8) messages exceeding max_blob_size_bytes are rejected with blob_too_large JSON error BEFORE translation
- GET /health returns 200 {status:healthy, relay:ok, node:connected} when UDS up, 503 {status:degraded, relay:ok, node:disconnected} when down
- max_blob_size_bytes is SIGHUP-reloadable via shared atomic, default 0 (no limit)
- All 241 relay tests pass (2596 assertions) with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Blob size limit config + WsSession enforcement** - `8302852f` (feat)
2. **Task 2: Health endpoint + SIGHUP wiring + relay_main integration** - `80c85389` (feat)

## Files Created/Modified
- `relay/config/relay_config.h` - Added max_blob_size_bytes config field (uint32_t, default 0)
- `relay/config/relay_config.cpp` - Added JSON loading for max_blob_size_bytes
- `relay/core/metrics_collector.h` - Added HealthProvider type alias and set_health_provider method
- `relay/core/metrics_collector.cpp` - Added GET /health HTTP handler with 200/503 JSON responses
- `relay/ws/ws_session.h` - Added max_blob_size atomic pointer parameter and member
- `relay/ws/ws_session.cpp` - Added blob size check before json_to_binary translation
- `relay/ws/ws_acceptor.h` - Added set_max_blob_size method and member
- `relay/ws/ws_acceptor.cpp` - Pass max_blob_size_ to WsSession::create in both TLS and plain paths
- `relay/relay_main.cpp` - Wired max_blob_size atomic, health provider, SIGHUP reload, startup logging
- `relay/tests/test_relay_config.cpp` - Added 2 tests for max_blob_size_bytes config
- `relay/tests/test_metrics_collector.cpp` - Added health provider API test

## Decisions Made
- Blob size check uses base64 upper-bound estimate (b64_len * 3 / 4) to avoid decoding the payload -- fast O(1) rejection before memory allocation
- Health endpoint uses raw string concatenation for JSON body (not nlohmann::json) to avoid adding include for 3 fixed fields
- blob_too_large error code is relay-only, not added to shared error_codes.h since it's a relay enforcement concept

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all features are fully wired and functional.

## Next Phase Readiness
- Blob size limit and health endpoint are production-ready
- FEAT-02 and FEAT-03 requirements satisfied
- Plan 03 (source exclusion) can proceed independently

---
*Phase: 109-new-features*
*Completed: 2026-04-13*
