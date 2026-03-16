---
phase: 30-benchmark-scenarios
plan: 01
subsystem: tooling
tags: [benchmark, bash, docker-stats, loadgen, ingest, sync-latency, multi-hop, sigusr1]

# Dependency graph
requires:
  - phase: 28-load-generator
    provides: chromatindb_loadgen binary with JSON output
  - phase: 29-multi-node-topology
    provides: Docker Compose 3-node chain topology
provides:
  - deploy/run-benchmark.sh orchestration script with utility functions
  - Ingest throughput scenario at 3 blob sizes (1K, 100K, 1M)
  - Sync latency + multi-hop propagation scenario with SIGUSR1 polling
  - Docker stats resource profiling (pre/post snapshots)
  - JSON results output to deploy/results/
affects: [30-02-PLAN, 31-report-generation]

# Tech tracking
tech-stack:
  added: []
  patterns: [scenario-runner, sigusr1-blob-polling, docker-stats-json-capture, convergence-wait-loop]

key-files:
  created: [deploy/run-benchmark.sh]
  modified: []

key-decisions:
  - "SIGUSR1 + log grep for blob count polling (no HTTP API needed)"
  - "Convergence timing starts from loadgen completion, not scenario start"
  - "Sequential scenarios with full topology reset between each for clean state"
  - "docker compose -p chromatindb for predictable network naming"

patterns-established:
  - "Scenario runner: reset_topology -> capture_stats pre -> run workload -> capture_stats post -> write JSON"
  - "SIGUSR1 polling with 1s sleep for spdlog flush + docker log latency"
  - "wait_for_convergence with nanosecond timing and configurable timeout"

requirements-completed: [PERF-01, PERF-02, PERF-03, OBS-01]

# Metrics
duration: 2min
completed: 2026-03-16
---

# Phase 30 Plan 01: Benchmark Scenarios Summary

**Benchmark orchestration script with ingest throughput (3 sizes), sync/multi-hop latency measurement via SIGUSR1 polling, and docker stats resource profiling**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-16T15:16:25Z
- **Completed:** 2026-03-16T15:18:07Z
- **Tasks:** 2 (1 auto + 1 auto-approved checkpoint)
- **Files modified:** 1

## Accomplishments
- Created deploy/run-benchmark.sh (299 lines) with 10 functions: 8 utilities + 2 scenario functions
- Ingest scenario runs loadgen at 3 blob sizes (1024, 102400, 1048576 bytes) with per-size JSON results including blobs/sec, MiB/sec, p50/p95/p99 latency
- Sync scenario measures 1-hop (node2) and 2-hop (node3) convergence times in milliseconds using SIGUSR1 blob count polling
- Docker stats snapshots captured before and after each scenario (CPU, memory, disk I/O, network I/O per container)
- Script framework ready for Plan 30-02 to add late-joiner and trusted-vs-PQ scenarios

## Task Commits

Each task was committed atomically:

1. **Task 1: Create run-benchmark.sh with utility functions and ingest + sync scenarios** - `ba96356` (feat)
2. **Task 2: Verify benchmark script core scenarios** - auto-approved (checkpoint:human-verify)

## Files Created/Modified
- `deploy/run-benchmark.sh` - Benchmark orchestration script (299 lines): configuration, 8 utility functions (log, check_deps, build_image, reset_topology, capture_stats, get_blob_count, wait_for_convergence, run_loadgen), 2 scenario functions (run_scenario_ingest, run_scenario_sync), main with --skip-build flag parsing

## Decisions Made
- SIGUSR1 + docker logs grep for blob count polling: uses existing observability mechanism, no code changes needed
- Convergence timing starts from loadgen completion (not scenario start) to measure actual replication delay
- Full topology reset (down -v + up -d) between scenarios for clean state (named volumes persist otherwise)
- docker compose -p chromatindb for predictable project/network naming (chromatindb_chromatindb-net)
- Fallback healthcheck polling for docker compose versions without --wait support

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Script framework ready for Plan 30-02 to add run_scenario_latejoin and run_scenario_trusted_vs_pq functions
- All utility functions (reset_topology, capture_stats, wait_for_convergence, run_loadgen) are reusable by new scenarios
- deploy/results/ directory will be created automatically on first run

---
*Phase: 30-benchmark-scenarios*
*Completed: 2026-03-16*
