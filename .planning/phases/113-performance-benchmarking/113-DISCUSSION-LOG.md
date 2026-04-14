# Phase 113: Performance Benchmarking - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-14
**Phase:** 113-performance-benchmarking
**Areas discussed:** Test environment setup, Benchmark parameters, Report format and storage, Automation script

---

## Test Environment Setup

| Option | Description | Selected |
|--------|-------------|----------|
| New benchmark script | Create tools/relay_perf_test.sh that builds Release, starts node+relay, runs relay_benchmark.py | Yes |
| Extend ASAN test script | Add --perf mode to relay_asan_test.sh | |
| Manual invocation | Document manual steps, no script | |

**User's choice:** New benchmark script
**Notes:** Follows same pattern as relay_asan_test.sh but optimized for performance.

### Build Type

| Option | Description | Selected |
|--------|-------------|----------|
| Release | Full optimizations, no debug overhead | Yes |
| RelWithDebInfo | Optimized with debug symbols | |
| Both | Release for numbers, RelWithDebInfo for profiling | |

**User's choice:** Release
**Notes:** Production-representative numbers.

---

## Benchmark Parameters

| Option | Description | Selected |
|--------|-------------|----------|
| Use defaults | 100 iterations, 5 warmup, concurrency 1/10/100, blobs 1/10/50/100 MiB, 30s mixed | Yes |
| Reduce large blobs | Skip 100 MiB, run 1/10/50 MiB only | |
| Custom | User-specified parameters | |

**User's choice:** Use defaults

### Workloads

| Option | Description | Selected |
|--------|-------------|----------|
| Run all 4 | PERF-01 through PERF-04 | Yes |
| Skip mixed workload | Skip PERF-04 | |
| Let me choose | Pick specific workloads | |

**User's choice:** Run all 4

---

## Report Format and Storage

| Option | Description | Selected |
|--------|-------------|----------|
| tools/benchmark_report.md | Default location, stays with tooling | Yes |
| .planning/phases/113-*/ | Phase directory, archived with milestone | |
| Both | Primary in tools/, copy in phase dir | |

**User's choice:** tools/benchmark_report.md

### Metadata

| Option | Description | Selected |
|--------|-------------|----------|
| Standard | Git hash, build type, CPU model, OS, date | Yes |
| Minimal | Git hash and date only | |
| Extended | Standard + compiler, CMake flags, kernel, memory | |

**User's choice:** Standard

---

## Automation Script

| Option | Description | Selected |
|--------|-------------|----------|
| Run only | Run benchmarks and produce report, no thresholds | Yes |
| Run + validate | Check results against minimum thresholds | |
| You decide | Claude's discretion | |

**User's choice:** Run only
**Notes:** This is a baseline measurement phase, not a regression gate.

---

## Claude's Discretion

- Script structure and error handling details
- Whether to add warmup/cooldown between workloads
- Node/relay config tuning for benchmark

## Deferred Ideas

None
