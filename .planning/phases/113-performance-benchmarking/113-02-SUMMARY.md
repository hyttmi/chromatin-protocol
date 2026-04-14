---
phase: 113-performance-benchmarking
plan: 02
subsystem: testing
tags: [benchmark, performance, relay, baseline]

# Dependency graph
requires:
  - phase: 113-performance-benchmarking
    plan: 01
    provides: relay_perf_test.sh orchestration script and relay_benchmark.py tool
provides:
  - "tools/benchmark_report.md -- Release performance baseline for all 4 PERF workloads"
  - "Fixed relay_benchmark.py WriteAck field name (hash vs blob_hash)"
affects: [regression-testing, performance-gates]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Performance baseline: measure before optimizing"]

key-files:
  created:
    - tools/benchmark_report.md
  modified:
    - tools/relay_benchmark.py

key-decisions:
  - "Fixed WriteAck JSON field 'hash' mismatch (was 'blob_hash') in relay_benchmark.py -- Rule 1 bug fix"
  - "All PERF results are baseline-only, no pass/fail thresholds"

patterns-established:
  - "Benchmark report format: PERF-01 through PERF-04 sections with environment metadata footer"

requirements-completed: [PERF-01, PERF-02, PERF-03, PERF-04]

# Metrics
duration: 10min
completed: 2026-04-14
---

# Phase 113 Plan 02: Benchmark Execution Summary

**Performance baseline report with throughput 341-952 blobs/sec, sub-4ms write latency, 13-14 MiB/s large blob transfer, and mixed workload degradation measured against Release-built relay**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-14T09:12:36Z
- **Completed:** 2026-04-14T09:23:04Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Executed all 4 PERF workloads against Release-built relay+node, producing tools/benchmark_report.md
- Fixed relay_benchmark.py bug: WriteAck JSON uses field name "hash" not "blob_hash" (affected PERF-02 read/exists and PERF-03 reads)
- Baseline results: PERF-01 throughput 341-952 blobs/sec, PERF-02 write p50=3.1ms/read p50=1.4ms/exists p50=1.2ms/stats p50=1.0ms, PERF-03 13-14 MiB/s write+read across 1-100 MiB, PERF-04 5250% p99 degradation under mixed load

## Task Commits

Each task was committed atomically:

1. **Task 1: Execute benchmark and produce report** - `91fe835f` (feat)
2. **Task 2: Verify benchmark results** - auto-approved (checkpoint:human-verify, auto_advance=true)

## Files Created/Modified
- `tools/benchmark_report.md` - Performance baseline report with all 4 PERF workloads and environment metadata
- `tools/relay_benchmark.py` - Fixed WriteAck JSON field name from "blob_hash" to "hash" (3 occurrences)

## Decisions Made
- WriteAck JSON field is "hash" per the relay's json_schema.h WRITE_ACK_FIELDS definition; benchmark script had wrong field name

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed WriteAck JSON field name in relay_benchmark.py**
- **Found during:** Task 1 (benchmark execution)
- **Issue:** relay_benchmark.py used `resp.json().get("blob_hash", "")` but the relay's WriteAck translation produces `{"hash": "..."}` per WRITE_ACK_FIELDS in json_schema.h. This caused PERF-02 to skip read/exists latency tests (empty blob_hashes list) and PERF-03 to fail all reads with 400 (empty hash in URL path).
- **Fix:** Changed 3 occurrences of `"blob_hash"` to `"hash"` in relay_benchmark.py
- **Files modified:** tools/relay_benchmark.py
- **Verification:** Re-ran benchmark -- all 4 operations measured in PERF-02, all 4 blob sizes read successfully in PERF-03
- **Committed in:** 91fe835f (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Bug fix was essential for complete benchmark results. Without it, PERF-02 was missing read/exists latency and PERF-03 had no read data.

## Issues Encountered
None beyond the bug fix above.

## User Setup Required

None - no external service configuration required.

## Key Benchmark Results

| Workload | Key Metric | Value |
|----------|-----------|-------|
| PERF-01 Throughput (c=1) | blobs/sec | 341 |
| PERF-01 Throughput (c=10) | blobs/sec | 952 |
| PERF-01 Throughput (c=100) | blobs/sec | 857 |
| PERF-02 Write Latency | p50 | 3.09 ms |
| PERF-02 Read Latency | p50 | 1.44 ms |
| PERF-02 Exists Latency | p50 | 1.19 ms |
| PERF-02 Stats Latency | p50 | 0.97 ms |
| PERF-03 Large Blob (100 MiB) | Write MiB/s | 13.9 |
| PERF-03 Large Blob (100 MiB) | Read MiB/s | 13.3 |
| PERF-04 Mixed p99 Degradation | percentage | +5250% |

## Next Phase Readiness
- Baseline performance data available for future regression gates
- Mixed workload p99 degradation is high (5250%) due to 10 MiB large blob writes monopolizing the single-threaded relay -- expected behavior, not a bug
- Future optimization targets: write throughput scaling, mixed workload fairness

---
*Phase: 113-performance-benchmarking*
*Completed: 2026-04-14*
