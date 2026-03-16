---
phase: 31-report-generation
plan: 01
subsystem: tooling
tags: [benchmark, bash, jq, report-generation, markdown, json-aggregation]

# Dependency graph
requires:
  - phase: 30-benchmark-scenarios/02
    provides: run-benchmark.sh with 5 scenarios producing per-scenario JSON results
provides:
  - collect_hardware_info() capturing system specs to hardware-info.json
  - generate_report() producing structured REPORT.md with 11 sections
  - benchmark-summary.json aggregating all scenario results into one file
  - --report-only flag for regenerating reports without re-running benchmarks
  - Graceful [not run] markers for missing scenario data
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: [heredoc-markdown-templating, jq-arithmetic-for-derived-metrics, graceful-missing-file-handling, resource-table-extraction-from-docker-stats]

key-files:
  created: [.gitignore]
  modified: [deploy/run-benchmark.sh]

key-decisions:
  - "jq for all numeric computation (rounding, overhead %, sync-interval multiples) -- no bc/awk"
  - "format_resource_table helper extracts peak CPU/memory per node from docker-stats JSON files for each scenario"
  - "Raw data section dynamically lists JSON files in results directory"

patterns-established:
  - "Report helper pattern: get_peak_stats, format_ingest_row, compute_overhead, format_resource_table as composable report-building functions"
  - "Graceful degradation: every scenario section checks file existence, falls back to [not run] or *Scenario not run.*"

requirements-completed: [OBS-02, OBS-03]

# Metrics
duration: 3min
completed: 2026-03-16
---

# Phase 31 Plan 01: Report Generation Summary

**Benchmark report generation with 11-section REPORT.md, combined benchmark-summary.json, hardware auto-detection, and --report-only flag for regeneration**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-16T15:57:28Z
- **Completed:** 2026-03-16T16:00:51Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Added collect_hardware_info() auto-detecting CPU, cores, RAM, disk type, kernel, Docker version via lscpu/free/uname/docker/lsblk
- Implemented generate_report() producing REPORT.md with all 11 sections: header with provenance, executive summary, system profile, topology diagram, parameters, ingest throughput, sync latency, late-joiner catch-up, trusted-vs-PQ comparison, caveats, raw data listing
- Built benchmark-summary.json aggregation combining all scenario results, hardware info, and parameters into one machine-readable file
- Added --report-only flag enabling report regeneration from existing JSON without re-running benchmarks
- Created .gitignore with deploy/results/ and build/ exclusions
- Verified --report-only produces valid output with [not run] markers when no scenario data exists

## Task Commits

Each task was committed atomically:

1. **Task 1: Add collect_hardware_info, --report-only flag, and .gitignore entry** - `036b1ad` (feat)
2. **Task 2: Implement generate_report() producing REPORT.md and benchmark-summary.json** - `8a0f9f8` (feat)

## Files Created/Modified
- `deploy/run-benchmark.sh` - Extended from 513 to 961 lines: added collect_hardware_info, get_peak_stats, format_ingest_row, compute_overhead, format_resource_table, generate_report functions plus --report-only flag
- `.gitignore` - Created with deploy/results/ and build/ exclusions

## Decisions Made
- Used jq exclusively for all numeric computation (rounding, percentage deltas, sync-interval multiples) -- consistent with existing script patterns, no bc/awk mixing
- format_resource_table as a reusable helper generating per-node CPU/memory tables from docker-stats files for any scenario
- Raw data section dynamically lists all JSON files in results directory rather than hardcoding filenames

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Complete benchmark pipeline: run-benchmark.sh now handles full lifecycle from build through scenarios to report generation
- --report-only enables fast iteration on report format without re-running benchmarks
- All v0.6.0 requirements (OBS-02, OBS-03) satisfied by this plan

---
*Phase: 31-report-generation*
*Completed: 2026-03-16*
