---
phase: 36-deletion-benchmarks
plan: 02
subsystem: benchmark
tags: [benchmark, tombstone, deletion, gc, sync, docker, bash]

# Dependency graph
requires:
  - phase: 36-deletion-benchmarks
    provides: Loadgen --delete mode with identity persistence and blob hash output (Plan 01)
provides:
  - Three tombstone benchmark scenarios in Docker benchmark suite (creation, sync, GC)
  - Storage-based GC polling via get_storage_mib() (not seq_num high-water)
  - Tombstone sections in REPORT.md (creation table, sync metrics, GC per-node timing)
  - Tombstone scenario keys in benchmark-summary.json
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "run_loadgen_v() variant with -v volume mount and -i stdin for write-then-delete pipelines"
    - "Storage-based GC detection via SIGUSR1 storage=X.XMiB (seq_num is high-water, never decreases)"
    - "Tombstone sync convergence uses existing wait_for_convergence at original_count + tombstone_count"
    - "format_tombstone_row() omits MiB/sec column (tombstones are always 36 bytes)"

key-files:
  created: []
  modified:
    - deploy/run-benchmark.sh

key-decisions:
  - "GC polling uses storage MiB metric (not blob count seq_num which never decreases)"
  - "run_loadgen_v() adds both -v volume mount and -i stdin flag for identity + hash piping"
  - "Tombstone sync measured via wait_for_convergence at 2x blob count (blobs + tombstones get separate seq_nums)"
  - "GC scenario uses TTL=30 for both blobs and tombstones with 180s timeout for expiry scan"

patterns-established:
  - "get_storage_mib(): SIGUSR1-based actual disk usage polling for GC measurement"
  - "wait_for_gc_completion(): storage threshold polling with bc float comparison"
  - "Identity volume mount pattern: host tmpdir mapped to container /tmp/bench-id"

requirements-completed: [BENCH-01, BENCH-02, BENCH-03]

# Metrics
duration: 4min
completed: 2026-03-18
---

# Phase 36 Plan 02: Tombstone Benchmark Scenarios Summary

**Three tombstone benchmark scenarios added to Docker suite measuring creation throughput, sync propagation, and GC reclamation with storage-based polling**

## Performance

- **Duration:** 4 min
- **Started:** 2026-03-18T15:51:38Z
- **Completed:** 2026-03-18T15:55:40Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Three new run_scenario_tombstone_*() functions orchestrate write-then-delete benchmarks with identity persistence via volume mounts
- GC measurement uses storage=X.XMiB from SIGUSR1 metrics (correctly avoids seq_num high-water which never decreases)
- REPORT.md gains Tombstone Creation, Tombstone Sync Propagation, and Tombstone GC/Expiry sections with tables and resource usage
- Executive Summary has 3 new rows; benchmark-summary.json has 5 new tombstone scenario keys

## Task Commits

Each task was committed atomically:

1. **Task 1: Add tombstone scenario functions and GC polling helpers** - `e00b03d` (feat)
2. **Task 2: Add tombstone report sections and summary JSON keys** - `824af1b` (feat)

## Files Created/Modified
- `deploy/run-benchmark.sh` - Extended with run_loadgen_v(), get_storage_mib(), wait_for_gc_completion(), 3 tombstone scenario functions, format_tombstone_row(), report sections, and summary JSON keys

## Decisions Made
- GC polling uses storage MiB metric from SIGUSR1 output -- blob count (blobs=N) is latest_seq_num which is a high-water mark and never decreases, making it useless for GC detection
- run_loadgen_v() includes both -v (volume mount) and -i (stdin) flags since tombstone scenarios need identity persistence AND hash piping
- Tombstone sync convergence target is original_count + tombstone_count because tombstones get their own seq_nums
- GC scenario writes both blobs and tombstones with TTL=30, allows 180s timeout to account for 60s expiry scan interval

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Tombstone benchmarks fully integrated into Docker benchmark suite
- All 3 BENCH requirements complete: creation throughput, sync propagation, GC reclamation
- Suite ready for full benchmark run via `bash deploy/run-benchmark.sh`

---
*Phase: 36-deletion-benchmarks*
*Completed: 2026-03-18*
