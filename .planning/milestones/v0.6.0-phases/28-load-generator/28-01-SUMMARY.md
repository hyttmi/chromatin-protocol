---
phase: 28-load-generator
plan: 01
subsystem: tooling
tags: [load-generator, asio, steady-timer, latency, json, pub-sub]

# Dependency graph
requires:
  - phase: 27-container-build
    provides: Dockerfile and build infrastructure
provides:
  - chromatindb_loadgen binary for protocol-compliant load generation
  - Timer-driven scheduling with anti-coordinated-omission
  - Notification-based ACK latency measurement
  - Mixed-size workload mode (1K/100K/1M)
  - JSON statistics output
affects: [29-metrics-collector, 30-benchmark-scenarios]

# Tech tracking
tech-stack:
  added: []
  patterns: [timer-driven-scheduling, notification-ack-matching, pre-computed-send-schedule]

key-files:
  created: [loadgen/loadgen_main.cpp]
  modified: [CMakeLists.txt, Dockerfile]

key-decisions:
  - "Single-file loadgen tool reusing chromatindb_lib for full protocol compliance"
  - "Notification-based ACK (type 22) for per-blob latency instead of fire-and-forget"
  - "spdlog to stderr, JSON to stdout for clean machine-readable output"
  - "Pre-generated random data pools (one per size class) reused with unique timestamps"

patterns-established:
  - "Timer-driven scheduling: pre-compute all send times, measure latency from scheduled time"
  - "Notification matching: blob_hash hex key in unordered_map for O(1) ACK lookup"

requirements-completed: [LOAD-01, LOAD-02, LOAD-03]

# Metrics
duration: 16min
completed: 2026-03-15
---

# Phase 28 Plan 01: Load Generator Summary

**Protocol-compliant load generator with timer-driven scheduling, notification ACK latency, and mixed-size workload mode outputting JSON statistics**

## Performance

- **Duration:** 16 min
- **Started:** 2026-03-15T17:21:38Z
- **Completed:** 2026-03-15T17:37:59Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Built chromatindb_loadgen binary (651 LOC) that connects as a real protocol-compliant peer with PQ handshake
- Timer-driven fixed-rate scheduling prevents coordinated omission by measuring latency from scheduled send time
- Notification ACK matching via pub/sub gives true end-to-end per-blob latency
- Mixed-size mode distributes blobs 70/20/10 across 1 KiB, 100 KiB, 1 MiB
- JSON output with blobs/sec, MiB/sec, p50/p95/p99/min/max/mean latency, errors, size breakdown
- Dockerfile updated with all 4 required changes (COPY source, build target, strip, COPY binary)
- All 313 existing tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Create loadgen binary with full protocol-compliant load generation** - `261e970` (feat)
2. **Task 2: Update Dockerfile to build and include chromatindb_loadgen** - `d1015ba` (chore)

## Files Created/Modified
- `loadgen/loadgen_main.cpp` - Complete load generator tool (651 lines): CLI parsing, connection setup, timer-driven send loop, notification ACK matching, statistics computation, JSON output
- `CMakeLists.txt` - Added chromatindb_loadgen executable target linked to chromatindb_lib
- `Dockerfile` - Added loadgen source COPY, build target, strip, and binary COPY to runtime image

## Decisions Made
- Single-file architecture: all loadgen code in loadgen_main.cpp, no separate headers needed for a standalone tool
- Notification-based ACK via pub/sub (type 22) for per-blob latency measurement instead of pure fire-and-forget throughput
- spdlog logs to stderr, JSON stats to stdout for clean scripted consumption
- Pre-generated data pools (one buffer per size class) reused with unique timestamps to avoid memory explosion
- Used spdlog::stderr_color_mt consistent with project logging patterns

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Fixed spdlog stderr sink include path**
- **Found during:** Task 1 (loadgen build)
- **Issue:** `spdlog/sinks/stderr_color_sink.h` does not exist in spdlog; the correct header is `spdlog/sinks/stdout_color_sinks.h` which provides both stdout and stderr color sinks
- **Fix:** Changed include to `spdlog/sinks/stdout_color_sinks.h` and used `spdlog::stderr_color_mt()` factory function consistent with project patterns
- **Files modified:** loadgen/loadgen_main.cpp
- **Verification:** Build succeeded after fix
- **Committed in:** 261e970 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Minor include path correction. No scope creep.

## Issues Encountered
None beyond the auto-fixed spdlog include.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- chromatindb_loadgen binary ready for Phase 29 (Metrics Collector) and Phase 30 (Benchmark Scenarios)
- Load generator exercises the full protocol path: PQ handshake, blob signing, Data message send, Notification ACK
- JSON output format ready for automated benchmark scripting

## Self-Check: PASSED

All files exist, all commits verified, binary builds and runs.

---
*Phase: 28-load-generator*
*Completed: 2026-03-15*
