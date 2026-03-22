---
phase: 50-operations-disaster-recovery-resource-limits
plan: 01
subsystem: testing
tags: [docker, integration-tests, signals, sighup, sigusr1, sigterm, rate-limiting, metrics, graceful-shutdown]

# Dependency graph
requires:
  - phase: 49-network-resilience-reconciliation
    provides: integration test infrastructure (helpers.sh, run-integration.sh)
provides:
  - OPS-01 SIGHUP config reload integration test (rate limit hot-reload)
  - OPS-02 SIGUSR1 metrics dump integration test (15-field validation)
  - OPS-03 SIGTERM graceful shutdown integration test (mid-ingest shutdown + recovery)
affects: [50-operations-disaster-recovery-resource-limits]

# Tech tracking
tech-stack:
  added: []
  patterns: [unique-container-names-per-test, standalone-docker-run-topology]

key-files:
  created:
    - tests/integration/test_ops01_sighup_config_reload.sh
    - tests/integration/test_ops02_sigusr1_metrics.sh
    - tests/integration/test_ops03_sigterm_graceful.sh
  modified: []

key-decisions:
  - "Unique container names per test (chromatindb-ops0N-nodeN) to prevent cross-test collisions when running via --filter ops"
  - "rate_limit_bytes_per_sec=1024 (config validation minimum) with burst=2048 (must be >= rate) for tight rate limiting test"

patterns-established:
  - "Unique container naming: chromatindb-{test-prefix}-node{N} prevents collisions when multiple tests share the sequential runner"
  - "Single-node topology for metrics-only tests (OPS-02) minimizes test duration"

requirements-completed: [OPS-01, OPS-02, OPS-03]

# Metrics
duration: 10min
completed: 2026-03-21
---

# Phase 50 Plan 01: Operations Signal Tests Summary

**Docker integration tests for SIGHUP rate-limit hot-reload, SIGUSR1 15-field metrics dump, and SIGTERM graceful shutdown with mid-ingest recovery**

## Performance

- **Duration:** 10 min
- **Started:** 2026-03-21T16:54:24Z
- **Completed:** 2026-03-21T17:04:40Z
- **Tasks:** 2
- **Files created:** 3

## Accomplishments
- OPS-01: SIGHUP with rate_limit change applies immediately; flooding peer disconnected; rate_limited counter incremented
- OPS-02: SIGUSR1 outputs all 15 expected metric fields (peers, connected_total, disconnected_total, blobs, storage, syncs, ingests, rejections, rate_limited, cursor_hits, cursor_misses, full_resyncs, quota_rejections, sync_rejections, uptime) with correct values
- OPS-03: SIGTERM during active ingest exits cleanly (exit code 0), restart passes integrity scan, node accepts new connections and blobs, sync converges

## Task Commits

Each task was committed atomically:

1. **Task 1: OPS-01 SIGHUP rate-limit reload + OPS-02 SIGUSR1 metrics dump** - `2687b41` (feat)
2. **Task 2: OPS-03 SIGTERM graceful shutdown** - `3fd39e0` (feat)
3. **Fix: Unique container names** - `e0ccb63` (fix)

## Files Created/Modified
- `tests/integration/test_ops01_sighup_config_reload.sh` - 2-node SIGHUP rate-limit reload test (3-phase: unlimited ingest, SIGHUP to 1KB/s, rate-limited disconnect)
- `tests/integration/test_ops02_sigusr1_metrics.sh` - Single-node SIGUSR1 metrics validation (15 fields, value checks)
- `tests/integration/test_ops03_sigterm_graceful.sh` - 2-node SIGTERM mid-ingest shutdown (exit code, integrity scan, post-restart sync)

## Decisions Made
- Used unique container names per test (`chromatindb-ops0N-nodeN`) instead of shared `chromatindb-test-node{1,2}` to prevent collisions when tests run sequentially via `--filter ops`
- Set `rate_limit_bytes_per_sec=1024` (config validation minimum is 1024) with `rate_limit_burst=2048` (burst must be >= rate) for the tightest valid rate limit
- Single-node topology for OPS-02 (metrics-only test needs no peers) keeps test duration under 10s

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Container name collisions between OPS tests**
- **Found during:** Final verification (running all three tests together)
- **Issue:** All three tests used `chromatindb-test-node1` / `chromatindb-test-node2` container names, causing collisions when run sequentially by run-integration.sh
- **Fix:** Changed to unique names: `chromatindb-ops01-node{1,2}`, `chromatindb-ops02-node1`, `chromatindb-ops03-node{1,2}`
- **Files modified:** all three test scripts
- **Verification:** `run-integration.sh --skip-build --filter ops` passes 3/3
- **Committed in:** `e0ccb63`

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Essential fix for test isolation. No scope creep.

## Issues Encountered
None beyond the container name collision (documented above).

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All three OPS signal tests pass individually and together
- Test infrastructure (helpers.sh, run-integration.sh) unchanged
- Ready for remaining 50-02 and 50-03 plans

## Self-Check: PASSED

- All 3 test scripts exist on disk
- SUMMARY.md exists
- All 3 commits found in git log (2687b41, 3fd39e0, e0ccb63)

---
*Phase: 50-operations-disaster-recovery-resource-limits*
*Completed: 2026-03-21*
