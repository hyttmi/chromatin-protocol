---
phase: 17-operational-stability
plan: 03
subsystem: observability
tags: [metrics, sigusr1, spdlog, signal-handling, coroutines]

# Dependency graph
requires:
  - phase: 17-02
    provides: "NodeMetrics struct with counter instrumentation"
provides:
  - "SIGUSR1 on-demand metrics dump (multi-line: global, per-peer, per-namespace)"
  - "60-second periodic structured metrics log line"
  - "compute_uptime_seconds helper"
affects: [18-rate-limiting, 19-monitoring]

# Tech tracking
tech-stack:
  added: []
  patterns: ["SIGUSR1 member coroutine (same pattern as SIGHUP)", "Periodic timer coroutine for metrics output"]

key-files:
  created: []
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - tests/peer/test_peer_manager.cpp

key-decisions:
  - "Sync-ingested blobs do not increment metrics_.ingests (counter tracks direct IngestBlob messages only)"
  - "blob_count as sum of latest_seq_num is an upper-bound proxy (acceptable per research)"

patterns-established:
  - "SIGUSR1 signal handler: member coroutine pattern (sigusr1_loop), identical to sighup_loop"
  - "Structured metrics line: key=value format with MiB storage suffix"

requirements-completed: [OPS-06, OPS-07]

# Metrics
duration: 6min
completed: 2026-03-10
---

# Phase 17 Plan 03: Metrics Dump & Periodic Log Summary

**SIGUSR1 on-demand multi-line metrics dump and 60-second periodic structured metrics log via spdlog**

## Performance

- **Duration:** 6 min
- **Started:** 2026-03-10T16:38:05Z
- **Completed:** 2026-03-10T16:44:49Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- SIGUSR1 signal handler spawned as member coroutine, triggers dump_metrics with global counters, per-peer breakdown, and per-namespace stats
- Periodic 60-second metrics log line with structured key=value format: connections, blobs, storage (MiB), syncs, ingests, rejections, uptime
- Extended E2E test with rejections and peers_disconnected_total assertions

## Task Commits

Each task was committed atomically:

1. **Task 1: SIGUSR1 signal handler + metrics dump + periodic metrics log** - `15da524` (feat)
2. **Task 2: E2E test for metrics output** - `072819f` (test)

## Files Created/Modified
- `db/peer/peer_manager.h` - Added sigusr1_signal_, sigusr1_loop, dump_metrics, metrics_timer_loop, log_metrics_line, compute_uptime_seconds declarations
- `db/peer/peer_manager.cpp` - Implemented SIGUSR1 handler, dump_metrics (synchronous multi-line), metrics_timer_loop (60s), log_metrics_line (structured key=value), compute_uptime_seconds
- `tests/peer/test_peer_manager.cpp` - Extended NodeMetrics E2E test with rejections and peers_disconnected_total assertions

## Decisions Made
- Sync-ingested blobs do not increment `metrics_.ingests` -- the counter tracks only direct `IngestBlob` transport messages, not sync protocol ingestions through `sync_proto_`
- `blob_count` computed as sum of `latest_seq_num` across namespaces (upper-bound proxy, O(N namespaces) not O(N blobs))

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Removed incorrect ingests assertion in E2E test**
- **Found during:** Task 2
- **Issue:** Plan suggested testing `pm2.metrics().ingests > 0` after sync, but sync-ingested blobs go through `sync_proto_` which does not increment `metrics_.ingests` (only direct IngestBlob messages do)
- **Fix:** Removed the incorrect assertion; kept rejections and peers_disconnected_total assertions
- **Files modified:** tests/peer/test_peer_manager.cpp
- **Verification:** All 272/273 tests pass (1 pre-existing SEGFAULT in unrelated test)
- **Committed in:** 072819f (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Corrected a test assertion that would have been incorrect. No scope creep.

## Issues Encountered
- Pre-existing SEGFAULT in test #253 "PeerManager storage full signaling" (Phase 16-03 origin). Logged to deferred-items.md. Does not affect Phase 17 features.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 17 (Operational Stability) complete: graceful shutdown, peer persistence, metrics counters, SIGUSR1 dump, periodic metrics log
- Ready for Phase 18 (rate limiting) or Phase 19 (monitoring integration)
- Pre-existing SEGFAULT in storage full test should be investigated separately

---
## Self-Check: PASSED

All files exist, all commits verified.

---
*Phase: 17-operational-stability*
*Completed: 2026-03-10*
