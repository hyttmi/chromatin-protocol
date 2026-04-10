---
phase: 105-operational-polish
plan: 02
subsystem: relay
tags: [rate-limiting, prometheus, metrics, graceful-shutdown, sighup, sigterm, websocket]

# Dependency graph
requires:
  - phase: 105-01
    provides: "RateLimiter, MetricsCollector, RelayMetrics struct, config extensions"
provides:
  - "Per-client rate limiting enforced in WsSession AUTHENTICATED path"
  - "7 metrics counters wired at all increment sites"
  - "Prometheus /metrics with live gauge provider"
  - "Drain-first SIGTERM graceful shutdown"
  - "SIGHUP reload for rate_limit and metrics_bind"
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Shared atomic for SIGHUP propagation to existing sessions"
    - "GaugeProvider callback for live scrape-time data"
    - "Drain-first shutdown: stop acceptor, wait, close, wait, stop"

key-files:
  modified:
    - relay/ws/ws_session.h
    - relay/ws/ws_session.cpp
    - relay/ws/ws_acceptor.h
    - relay/ws/ws_acceptor.cpp
    - relay/ws/session_manager.h
    - relay/ws/session_manager.cpp
    - relay/relay_main.cpp
    - relay/core/metrics_collector.h
    - relay/core/metrics_collector.cpp
    - relay/core/rate_limiter.h

key-decisions:
  - "Rate limit check placed after JSON parse/request_id extraction but before type allowlist, so error response includes request_id"
  - "Shared atomic<uint32_t> for rate propagation -- sessions check on each message, only call set_rate() when value changes to avoid resetting token bucket"
  - "GaugeProvider callback on MetricsCollector so scrape gets live session/subscription counts without polling"
  - "Disconnection counter only fires when sessions_.erase() actually removes an entry (guards against double-remove)"

patterns-established:
  - "Shared atomic pattern: config value -> atomic -> per-session lazy sync on message path"
  - "GaugeProvider pattern: std::function callback for lazy gauge evaluation at scrape time"

requirements-completed: [OPS-01, OPS-02, OPS-03, SESS-04]

# Metrics
duration: 18min
completed: 2026-04-10
---

# Phase 105 Plan 02: Operational Wiring Summary

**Per-client rate limiting, 7 metrics counters, Prometheus gauge provider, drain-first SIGTERM shutdown, and SIGHUP reload for rate/metrics config**

## Performance

- **Duration:** 18 min
- **Started:** 2026-04-10T09:01:47Z
- **Completed:** 2026-04-10T09:19:46Z
- **Tasks:** 2
- **Files modified:** 10

## Accomplishments
- Per-client rate limiting enforced in WsSession AUTHENTICATED path with JSON error response including request_id, sustained violations (10 consecutive) disconnect with close(4002)
- All 7 metric counters increment at correct points: connect, disconnect, msg recv, msg send, auth fail, rate limit, errors
- MetricsCollector wired in main() with GaugeProvider for live session/subscription counts at Prometheus scrape time
- SIGTERM replaced with drain-first sequence: stop acceptor -> 5s drain -> Close(1001) -> 2s close handshake -> ioc.stop()
- SIGHUP extended to reload rate_limit_messages_per_sec (via shared atomic) and metrics_bind (stop/start collector)
- All 205 relay tests pass (2378 assertions), no regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: WsSession rate limiting + metrics counter increment sites** - `d4dc5ce` (feat)
2. **Task 2: relay_main SIGHUP extension, SIGTERM drain-first, MetricsCollector startup** - `a7e0b28` (feat)

## Files Created/Modified
- `relay/core/rate_limiter.h` - Added current_rate() getter for shared atomic comparison
- `relay/core/metrics_collector.h` - Added GaugeProvider callback type and set_gauge_provider()
- `relay/core/metrics_collector.cpp` - handle_connection uses gauge_provider_ for live data at scrape
- `relay/ws/ws_session.h` - RateLimiter member, RelayMetrics pointer, shared_rate atomic pointer, disconnect threshold
- `relay/ws/ws_session.cpp` - Rate limit check in AUTHENTICATED path, metrics increments at 5 sites
- `relay/ws/ws_acceptor.h` - set_metrics() and set_shared_rate() setters, member pointers
- `relay/ws/ws_acceptor.cpp` - Pass metrics and shared_rate to WsSession::create()
- `relay/ws/session_manager.h` - set_metrics() setter, RelayMetrics pointer
- `relay/ws/session_manager.cpp` - ws_connections_total and ws_disconnections_total increments
- `relay/relay_main.cpp` - MetricsCollector construction, shared atomics, drain-first SIGTERM, SIGHUP extension

## Decisions Made
- Rate limit check placed after JSON parse and request_id extraction but before type allowlist check, so error responses include request_id per RESEARCH recommendation
- Used shared atomic<uint32_t> pattern: main thread stores new rate on SIGHUP, each session reads on each message and only calls set_rate() when value changes to avoid resetting the token bucket mid-window
- Added GaugeProvider callback to MetricsCollector rather than storing session_manager/tracker pointers -- cleaner dependency direction
- Disconnection counter only increments when sessions_.erase() actually removes an entry, preventing double-count from close() + read_loop() both calling remove_session()

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Added GaugeProvider to MetricsCollector for live gauge data**
- **Found during:** Task 2
- **Issue:** Plan mentioned adding set_gauge_provider but MetricsCollector from Plan 01 called format_prometheus(0, 0) -- gauges would always show zero
- **Fix:** Added GaugeProvider type, set_gauge_provider(), and updated handle_connection to call provider at scrape time
- **Files modified:** relay/core/metrics_collector.h, relay/core/metrics_collector.cpp
- **Verification:** Build passes, gauge callback wired in main()
- **Committed in:** a7e0b28 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 missing critical)
**Impact on plan:** Essential for Prometheus gauges to report actual data. Plan anticipated this change but it required modifying Plan 01 artifacts.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All 4 requirements (OPS-01, OPS-02, OPS-03, SESS-04) are complete
- Relay is production-ready with rate limiting, metrics, graceful shutdown, and SIGHUP reload
- Phase 105 operational polish is complete

## Self-Check: PASSED

All 10 modified files verified present. Both task commits (d4dc5ce, a7e0b28) verified in history. SUMMARY.md created.

---
*Phase: 105-operational-polish*
*Completed: 2026-04-10*
