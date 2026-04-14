# Relay Performance Benchmark Report

**Date:** 2026-04-14 12:20:11 EEST
**Hardware:** x86_64 / Linux-6.19.11-arch1-1-x86_64-with-glibc2.43
**CPUs:** 12
**Relay URL:** http://127.0.0.1:4281

**Relay config notes:** Benchmarks should run with `rate_limit_messages_per_sec=0` and `request_timeout_seconds=0` to avoid artificial limits.

## PERF-01: Throughput Benchmark

1 KB blobs, POST /blob, per concurrency level.

| Concurrency | Total Ops | Wall Time (s) | Blobs/sec | Per-Client ops/sec | p50 (ms) | p95 (ms) | p99 (ms) | Errors |
|-------------|-----------|---------------|-----------|-------------------|----------|----------|----------|--------|
| 1 | 100 | 0.29 | 340.6 | 340.6 | 2.63 | 3.16 | 5.37 | 0 |
| 10 | 1000 | 1.05 | 952.1 | 95.2 | 10.05 | 11.74 | 21.69 | 0 |
| 100 | 10000 | 11.67 | 856.6 | 8.6 | 116.39 | 125.84 | 130.55 | 0 |

## PERF-02: Per-Operation Latency

Single client, HTTP keep-alive, relay-only (UDS baseline deferred).

| Operation | Iterations | p50 (ms) | p95 (ms) | p99 (ms) | Errors |
|-----------|-----------|----------|----------|----------|--------|
| write (POST /blob) | 100 | 3.09 | 3.52 | 4.01 | 0 |
| read (GET /blob/{ns}/{hash}) | 100 | 1.44 | 1.63 | 2.21 | 0 |
| exists (GET /exists/{ns}/{hash}) | 100 | 1.19 | 1.29 | 6.08 | 0 |
| stats (GET /stats/{ns}) | 100 | 0.97 | 1.05 | 1.39 | 0 |

## PERF-03: Large Blob Transfer

Raw binary POST /blob and GET /blob, single client, HTTP keep-alive.

| Size (MiB) | Sign (s) | Build (s) | Write (s) | Write MiB/s | Read (s) | Read MiB/s | Errors |
|-----------|----------|-----------|-----------|-------------|----------|------------|--------|
| 1 | 0.00 | 0.01 | 0.08 | 13.0 | 0.07 | 13.8 | 0 |
| 10 | 0.04 | 0.09 | 0.71 | 14.1 | 0.70 | 14.2 | 0 |
| 50 | 0.22 | 0.59 | 3.63 | 13.8 | 3.71 | 13.5 | 0 |
| 100 | 0.42 | 1.13 | 7.18 | 13.9 | 7.53 | 13.3 | 0 |

## PERF-04: Mixed Workload

Baseline: 38689 small queries over 30s (no large blobs).
Under load: 1216 small queries + 60 large blob writes over 30s.

| Condition | Small Queries | Large Writes | p50 (ms) | p95 (ms) | p99 (ms) | Errors |
|-----------|--------------|-------------|----------|----------|----------|--------|
| Baseline | 38689 | 0 | 7.58 | 8.60 | 9.48 | 0 |
| Under load | 1216 | 60 | 147.78 | 501.91 | 507.12 | 0 |

**p50 degradation:** +1848.5%
**p99 degradation:** +5250.4%

---

## Environment

| Field | Value |
|-------|-------|
| Git hash | `b878737a` |
| Build type | Release |
| CPU | AMD Ryzen 5 5600U with Radeon Graphics |
| OS | Linux 6.19.11-arch1-1 x86_64 |
| Date | 2026-04-14 12:21:53 EEST |
