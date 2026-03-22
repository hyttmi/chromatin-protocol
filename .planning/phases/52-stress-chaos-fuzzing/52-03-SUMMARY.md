---
phase: 52-stress-chaos-fuzzing
plan: 03
subsystem: testing
tags: [stress-test, soak-test, concurrent-ops, docker, integration-test, rss-monitoring]

# Dependency graph
requires:
  - phase: 50-dos-resilience-disaster-recovery
    provides: "Docker test patterns, helpers.sh primitives, loadgen identity-save/delete"
  - phase: 51-ttl-lifecycle-e2e-primitives
    provides: "Tombstone lifecycle, SIGUSR1 metrics, integrity scan patterns"
provides:
  - "STRESS-01 long-running soak test with RSS monitoring and convergence checks"
  - "STRESS-04 concurrent mixed operations test (ingest + delete + SIGHUP + SIGUSR1)"
  - "run-integration.sh exclusion pattern for long-running tests"
affects: [52-stress-chaos-fuzzing]

# Tech tracking
tech-stack:
  added: []
  patterns: ["RSS monitoring via docker stats parsing", "Background bash jobs for concurrent operations", "Duration flag parsing (Xh/Xm) for configurable test length", "Excluded test list in run-integration.sh"]

key-files:
  created:
    - tests/integration/test_stress01_long_running.sh
    - tests/integration/test_stress04_concurrent_ops.sh
  modified:
    - tests/integration/run-integration.sh

key-decisions:
  - "Mixed-size ingest via round-robin loadgen batches (128B/1KB/10KB/100KB/1MB at rate 10)"
  - "RSS monitoring via docker stats --no-stream parsing with KiB/MiB/GiB conversion"
  - "2x initial RSS bound with 100 MiB minimum floor for memory check"
  - "STRESS-04 delete job reuses blob hashes from identity-save ingest output"
  - "EXCLUDED_TESTS array in run-integration.sh for extensible exclusion pattern"

patterns-established:
  - "Duration flag pattern: --duration Xh/Xm for configurable long-running tests"
  - "RSS monitoring pattern: parse docker stats, log to file, verify bounded growth"
  - "Concurrent background jobs: bash functions as background processes with PID tracking"

requirements-completed: [STRESS-01, STRESS-04]

# Metrics
duration: 4min
completed: 2026-03-22
---

# Phase 52 Plan 03: Stress Tests Summary

**Long-running soak test (STRESS-01) with RSS monitoring and concurrent mixed operations test (STRESS-04) with 4-node ingest/delete/signal chaos**

## Performance

- **Duration:** 4 min
- **Started:** 2026-03-22T04:13:26Z
- **Completed:** 2026-03-22T04:17:59Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- STRESS-01: 3-node soak test with configurable duration (default 4h), mixed-size ingest at ~10 blobs/sec, RSS monitoring every 60s, convergence checks every 5 min, bounded memory verification
- STRESS-04: 4-node concurrent operations (ingest + delete + SIGHUP + SIGUSR1) for 5 minutes, post-ops restart with integrity scan, blob count convergence verification
- run-integration.sh updated to exclude stress01 from default runs via EXCLUDED_TESTS array (explicit --filter still works)

## Task Commits

Each task was committed atomically:

1. **Task 1: STRESS-01 long-running soak test** - `cdeb918` (feat)
2. **Task 2: STRESS-04 concurrent ops + run-integration.sh update** - `5f7be9c` (feat)

## Files Created/Modified
- `tests/integration/test_stress01_long_running.sh` - 3-node soak test with RSS monitoring, convergence checks, configurable --duration flag (434 lines)
- `tests/integration/test_stress04_concurrent_ops.sh` - 4-node concurrent mixed operations test with background jobs (490 lines)
- `tests/integration/run-integration.sh` - Added EXCLUDED_TESTS array to skip stress01 from default discovery

## Decisions Made
- Used round-robin loadgen batches (5 sizes cycling) instead of --mixed mode, since --mixed only provides 3 size classes (1K/100K/1M) while the plan specified 5 sizes (128B/1K/10K/100K/1M)
- RSS bound check uses 2x initial with a 100 MiB minimum floor to avoid false failures on low-initial-RSS nodes
- STRESS-04 delete job feeds captured blob hashes from identity-save ingest output, cycling through available hashes
- EXCLUDED_TESTS is an array for future extensibility (easy to add more long-running tests)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- STRESS-01 and STRESS-04 test scripts ready for execution
- STRESS-01 excluded from default test runner; invoke via `--filter stress01` or directly
- Remaining phase 52 plans (STRESS-02/03, SAN-04/05) can proceed independently

## Self-Check: PASSED

- All 3 created/modified files exist on disk
- Both task commits (cdeb918, 5f7be9c) found in git history
- test_stress01_long_running.sh: 434 lines (min 150)
- test_stress04_concurrent_ops.sh: 490 lines (min 120)
- run-integration.sh contains stress01 exclusion pattern

---
*Phase: 52-stress-chaos-fuzzing*
*Completed: 2026-03-22*
