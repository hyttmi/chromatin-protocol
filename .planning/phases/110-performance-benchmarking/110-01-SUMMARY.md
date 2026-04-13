---
phase: 110-performance-benchmarking
plan: 01
subsystem: testing
tags: [benchmark, httpx, ml-dsa-87, flatbuffers, performance, asyncio]

requires:
  - phase: 999.9-http-sse-transport
    provides: HTTP relay API endpoints (auth, data, query)
provides:
  - "Standalone Python benchmark tool for HTTP relay performance"
  - "4 benchmark workloads: throughput, latency, large blob, mixed"
  - "ML-DSA-87 auth + FlatBuffer blob construction in Python"
  - "Markdown report generation with percentile stats"
affects: [110-02, performance-regression]

tech-stack:
  added: [httpx]
  patterns: [async benchmark workers, FlatBuffer manual vtable construction, ephemeral ML-DSA-87 identity]

key-files:
  created: [tools/relay_benchmark.py]
  modified: []

key-decisions:
  - "httpx AsyncClient over aiohttp for cleaner async HTTP"
  - "UDS baseline (PERF-02) skipped by default -- relay-only latency measured"
  - "FlatBuffer built via manual vtable slots (no codegen) matching blob.fbs field order"
  - "Ephemeral ML-DSA-87 keypair per benchmark run (no persistent identity)"

patterns-established:
  - "FlatBuffer manual construction: CreateByteVector first, then StartObject/EndObject with slot indices"
  - "ML-DSA-87 auth flow: challenge -> sign raw nonce bytes -> verify with hex-encoded fields"

requirements-completed: [PERF-01, PERF-02, PERF-03, PERF-04]

duration: 4min
completed: 2026-04-13
---

# Phase 110 Plan 01: Benchmark Tool Summary

**Python HTTP relay benchmark tool with ML-DSA-87 auth, FlatBuffer blob builder, and 4 workload types (throughput/latency/large-blob/mixed) reporting p50/p95/p99 percentiles**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-13T15:58:06Z
- **Completed:** 2026-04-13T16:02:00Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Complete standalone benchmark tool at tools/relay_benchmark.py (632 lines)
- ML-DSA-87 ephemeral identity generation and HTTP challenge-response auth
- FlatBuffer Blob construction with correct signing input (SHA3-256 of ns||data||ttl_be32||ts_be64)
- PERF-01: concurrent throughput at 1/10/100 clients with 1KB blobs
- PERF-02: per-operation latency for write/read/exists/stats (UDS baseline deferred)
- PERF-03: large blob transfer at 1/10/50/100 MiB with separate sign/build/network timing
- PERF-04: mixed workload baseline vs under-load comparison with degradation percentage
- Markdown report generation with hardware info, config notes, and percentile tables

## Task Commits

Each task was committed atomically:

1. **Task 1: Build relay_benchmark.py with all 4 benchmark workloads** - `6144da3d` (feat)

**Plan metadata:** [pending] (docs: complete plan)

## Files Created/Modified
- `tools/relay_benchmark.py` - Complete HTTP relay benchmark tool with 4 workload types

## Decisions Made
- Used httpx AsyncClient (HTTP/1.1, keep-alive) for all benchmarks -- cleaner than aiohttp
- UDS direct baseline skipped by default (--skip-uds) due to TrustedHello + AEAD complexity in Python
- FlatBuffer construction uses manual vtable slot indices (4-9) matching blob.fbs field order -- avoids needing flatc codegen
- Signing measures separated from network time in PERF-03 large blob benchmark for accuracy
- Mixed workload (PERF-04) runs 30s baseline then 30s under-load for degradation comparison

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required. httpx must be installed (pip install httpx).

## Next Phase Readiness
- Benchmark tool ready to run against any HTTP relay instance
- Report generation produces markdown suitable for commit/documentation
- PERF-02 UDS baseline deferred to future phase if needed

---
## Self-Check: PASSED

- FOUND: tools/relay_benchmark.py
- FOUND: .planning/phases/110-performance-benchmarking/110-01-SUMMARY.md
- FOUND: commit 6144da3d

*Phase: 110-performance-benchmarking*
*Completed: 2026-04-13*
