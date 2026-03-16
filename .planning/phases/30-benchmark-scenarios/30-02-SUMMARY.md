---
phase: 30-benchmark-scenarios
plan: 02
subsystem: tooling
tags: [benchmark, bash, docker-compose, late-joiner, trusted-peers, handshake-comparison, convergence]

# Dependency graph
requires:
  - phase: 30-benchmark-scenarios/01
    provides: run-benchmark.sh framework with utility functions and ingest/sync scenarios
  - phase: 29-multi-node-topology
    provides: Docker Compose 3-node chain + node4 latejoin profile
provides:
  - Late-joiner catch-up scenario measuring node4 convergence time (PERF-04)
  - Trusted vs PQ handshake comparison scenario with full restart (PERF-05)
  - Complete 5-scenario benchmark pipeline (LOAD-04)
  - docker-compose.trusted.yml override for trusted-mode configs
  - generate_trusted_configs helper for runtime IP resolution
affects: [31-report-generation]

# Tech tracking
tech-stack:
  added: []
  patterns: [runtime-ip-resolution-via-docker-inspect, compose-override-for-config-swap, full-restart-for-fair-comparison]

key-files:
  created: [deploy/docker-compose.trusted.yml]
  modified: [deploy/run-benchmark.sh]

key-decisions:
  - "Full compose down/up between PQ and trusted runs for fair handshake comparison (fresh connections required)"
  - "Runtime IP resolution via docker inspect for trusted_peers (Docker DNS names not accepted)"
  - "Heredoc for JSON config generation (cleaner than sed/jq on existing configs)"
  - "Node3 convergence gate before starting node4 (ensures data fully replicated before late-joiner test)"

patterns-established:
  - "Compose override pattern: docker-compose.trusted.yml mounts alternate configs at same container paths"
  - "Runtime IP resolution: docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' for trusted_peers"
  - "Late-joiner pattern: load data -> converge existing nodes -> start new node -> time catch-up"

requirements-completed: [PERF-04, PERF-05, LOAD-04]

# Metrics
duration: 2min
completed: 2026-03-16
---

# Phase 30 Plan 02: Benchmark Scenarios Summary

**Late-joiner catch-up timing and PQ-vs-trusted handshake comparison scenarios completing the 5-scenario benchmark pipeline**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-16T15:21:47Z
- **Completed:** 2026-03-16T15:24:41Z
- **Tasks:** 2 (1 auto + 1 auto-approved checkpoint)
- **Files modified:** 2

## Accomplishments
- Added run_scenario_latejoin() measuring catch-up time from node4 healthy to full convergence with 300s timeout (PERF-04)
- Added run_scenario_trusted_vs_pq() comparing PQ vs trusted handshake with identical workload, full compose restart between modes (PERF-05)
- Created generate_trusted_configs() helper resolving container IPs at runtime via docker inspect for trusted_peers
- Created deploy/docker-compose.trusted.yml compose override mounting trusted configs at same container paths
- Wired all 5 scenarios into main with final cleanup and result file count summary (LOAD-04)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add late-joiner and trusted-vs-PQ scenarios to benchmark script** - `49e7ba4` (feat)
2. **Task 2: Verify complete benchmark suite** - auto-approved (checkpoint:human-verify)

## Files Created/Modified
- `deploy/run-benchmark.sh` - Updated from 299 to 513 lines: added generate_trusted_configs helper, run_scenario_latejoin (PERF-04), run_scenario_trusted_vs_pq (PERF-05), updated main with all 5 scenarios + final cleanup
- `deploy/docker-compose.trusted.yml` - Compose override mounting node*-trusted.json configs at /config/node*.json paths with data volume re-specification

## Decisions Made
- Full compose down/up between PQ and trusted runs: SIGHUP can reload trusted_peers at runtime, but existing connections keep their handshake type. Fresh handshakes require fresh containers.
- Runtime IP resolution via docker inspect for trusted_peers: Docker DNS names not accepted by trusted_peers config.
- Heredoc for JSON config generation: cleaner than sed/jq manipulation of existing configs.
- Node3 convergence gate before starting node4: ensures full chain has data before measuring late-joiner catch-up.
- Healthcheck polling fallback for trusted mode startup (same pattern as reset_topology).

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Complete 5-scenario benchmark suite ready for execution
- All results output as JSON to deploy/results/ for report generation (Phase 31)
- Expected result files: scenario-ingest-{1k,100k,1m}.json, scenario-sync-latency.json, scenario-latejoin.json, scenario-trusted-vs-pq.json, docker-stats-*.json

---
*Phase: 30-benchmark-scenarios*
*Completed: 2026-03-16*
