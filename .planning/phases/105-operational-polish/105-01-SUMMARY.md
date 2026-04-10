---
phase: 105-operational-polish
plan: 01
subsystem: observability
tags: [prometheus, rate-limiting, metrics, token-bucket, asio-http]

requires:
  - phase: 104-pub-sub-uds-resilience
    provides: SubscriptionTracker, relay pipeline, session management
provides:
  - RateLimiter header-only token bucket class
  - RelayMetrics atomic counter struct (7 counters)
  - MetricsCollector with Prometheus /metrics HTTP endpoint
  - Config fields for metrics_bind and rate_limit_messages_per_sec
  - SubscriptionTracker::namespace_count() gauge helper
affects: [105-02-PLAN, relay integration, SIGHUP reload, WsSession message handling]

tech-stack:
  added: []
  patterns: [header-only rate limiter, atomic relay metrics, Prometheus text exposition]

key-files:
  created:
    - relay/core/rate_limiter.h
    - relay/core/metrics_collector.h
    - relay/core/metrics_collector.cpp
    - relay/tests/test_rate_limiter.cpp
    - relay/tests/test_metrics_collector.cpp
  modified:
    - relay/core/subscription_tracker.h
    - relay/config/relay_config.h
    - relay/config/relay_config.cpp
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt
    - relay/tests/test_relay_config.cpp

key-decisions:
  - "Token bucket burst equals rate -- full bucket available immediately after set_rate()"
  - "RelayMetrics uses std::atomic<uint64_t> with relaxed ordering for all counters"
  - "MetricsCollector mirrors node pattern: coroutine accept loop + HTTP handler, 5s timeout, 4KB cap"
  - "Prometheus prefix is chromatindb_relay_ (distinct from node's chromatindb_)"

patterns-established:
  - "Header-only rate limiter: no .cpp needed, RateLimiter in relay::core namespace"
  - "Relay metrics: RelayMetrics struct with atomic fields, MetricsCollector as manager"
  - "Config extension pattern: add field with default, load via j.value(), validate in validate_relay_config()"

requirements-completed: [OPS-01, OPS-03, OPS-02]

duration: 28min
completed: 2026-04-10
---

# Phase 105 Plan 01: Rate Limiting, Relay Metrics, and Prometheus Endpoint Summary

**Header-only token bucket rate limiter, 7-counter atomic RelayMetrics, Prometheus /metrics HTTP endpoint, and config extensions for metrics_bind and rate_limit_messages_per_sec**

## Performance

- **Duration:** 28 min
- **Started:** 2026-04-10T08:26:35Z
- **Completed:** 2026-04-10T08:55:33Z
- **Tasks:** 3
- **Files modified:** 11

## Accomplishments
- Header-only RateLimiter with token bucket algorithm, disabled mode (rate=0), burst=rate, consecutive rejection tracking, and disconnect threshold
- RelayMetrics struct with 7 atomic counters (ws_connections_total, ws_disconnections_total, messages_received_total, messages_sent_total, auth_failures_total, rate_limited_total, errors_total)
- MetricsCollector with Prometheus /metrics HTTP endpoint (7 counters + 3 gauges: ws_connections_active, subscriptions_active, uptime_seconds), coroutine accept loop
- Config extensions: metrics_bind (empty=disabled, host:port=enabled) and rate_limit_messages_per_sec (0=disabled) with JSON loading and validation
- SubscriptionTracker::namespace_count() method for gauge computation
- 17 new test cases covering all new components, 205 total relay tests with 2378 assertions

## Task Commits

Each task was committed atomically:

1. **Task 1: RateLimiter + RelayMetrics + MetricsCollector + namespace_count()** - `a7364d7` (feat)
2. **Task 2: Config extensions for metrics_bind and rate_limit_messages_per_sec** - `4689272` (feat)
3. **Task 3: Unit tests for all new components** - `fce522e` (test)

## Files Created/Modified
- `relay/core/rate_limiter.h` - Header-only token bucket rate limiter
- `relay/core/metrics_collector.h` - RelayMetrics struct (7 atomic counters) and MetricsCollector class
- `relay/core/metrics_collector.cpp` - Prometheus HTTP endpoint, text exposition format, accept loop
- `relay/core/subscription_tracker.h` - Added namespace_count() method
- `relay/config/relay_config.h` - Added metrics_bind and rate_limit_messages_per_sec fields
- `relay/config/relay_config.cpp` - JSON loading and validation for new fields
- `relay/CMakeLists.txt` - Added core/metrics_collector.cpp to build
- `relay/tests/test_rate_limiter.cpp` - 6 test cases for token bucket behavior
- `relay/tests/test_metrics_collector.cpp` - 4 test cases for Prometheus output and atomics
- `relay/tests/test_relay_config.cpp` - 7 new test cases for config extensions
- `relay/tests/CMakeLists.txt` - Added new test files

## Decisions Made
- Token bucket burst equals rate for simplicity -- no separate burst config needed
- RelayMetrics uses std::atomic<uint64_t> with memory_order_relaxed for Prometheus reads (per D-06)
- MetricsCollector mirrors the node's accept loop pattern from db/peer/metrics_collector.cpp
- Prometheus prefix is chromatindb_relay_ to distinguish from node's chromatindb_ prefix

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all components are fully implemented and ready for Plan 02 integration wiring.

## Next Phase Readiness
- All building blocks (RateLimiter, RelayMetrics, MetricsCollector) are created and tested
- Plan 02 can wire these into the relay's WsSession message path, SIGHUP reload, and main()
- Config fields are ready for use by relay_main.cpp

## Self-Check: PASSED

All 5 created files verified, all 3 commit hashes found, all content checks passed.

---
*Phase: 105-operational-polish*
*Completed: 2026-04-10*
