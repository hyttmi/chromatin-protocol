# Roadmap: chromatindb v4.0.0 Relay Architecture v3

## Overview

Rewrite the relay's concurrency model from multi-threaded io_context to single-threaded event loop with thread pool offload, matching the node's proven PeerManager pattern. Remove all strand/mutex code from the failed Phase 999.10 attempt. Verify ASAN-clean under concurrent load. Benchmark throughput, latency, large blob, and mixed workload performance to establish baselines. Three phases: concurrency rewrite, ASAN verification, performance benchmarking.

## Phases

**Phase Numbering:**
- Continues from v3.1.0 (Phases 106-110, backlog 999.x)
- v4.0.0 starts at Phase 111

- [x] **Phase 111: Single-Threaded Rewrite** - Change relay to 1 io_context thread + thread pool, remove all strand/mutex code, simplify handlers (completed 2026-04-14)
- [ ] **Phase 112: ASAN Verification** - Run relay under ASAN at 1/10/100 concurrent clients, fix any issues, verify signal handling
- [ ] **Phase 113: Performance Benchmarking** - Run all 4 benchmark workloads, generate baseline report

## Phase Details

### Phase 111: Single-Threaded Rewrite
**Goal**: Relay runs on a single event loop thread with CPU-heavy work offloaded to a thread pool -- all shared state is accessed without synchronization
**Depends on**: Nothing (first phase of v4.0.0)
**Requirements**: CONC-01, CONC-02, CONC-03, CONC-04, CONC-05, VER-01
**Success Criteria** (what must be TRUE):
  1. relay_main.cpp creates exactly one thread calling ioc.run(), plus a separate thread pool for offload work
  2. All strand members and std::mutex instances are removed from relay code -- grep finds zero occurrences
  3. TLS handshake, ML-DSA-87 verify, and large JSON parse/serialize execute on the thread pool via crypto::offload() and resume on the event loop thread
  4. HTTP handlers access shared state (UdsMultiplexer, RequestRouter, SubscriptionTracker, WriteTracker, TokenStore, ResponsePromiseMap) directly without strand posting or mutex locking
  5. All existing relay unit tests compile and pass under the single-threaded model
**Plans:** 3/3 plans complete

Plans:
- [x] 111-01-PLAN.md -- Thread pool infrastructure + relay_main.cpp single-threaded rewrite
- [x] 111-02-PLAN.md -- Strip strands/mutexes from all relay components + wire ML-DSA-87 offload
- [x] 111-03-PLAN.md -- Test adaptation + full test suite verification + grep audit

### Phase 112: ASAN Verification
**Goal**: Relay is proven free of memory safety and concurrency bugs under realistic concurrent load and signal handling
**Depends on**: Phase 111
**Requirements**: VER-02, VER-03
**Success Criteria** (what must be TRUE):
  1. Relay benchmark tool runs at 1, 10, and 100 concurrent HTTP clients with zero ASAN heap-use-after-free reports
  2. Relay benchmark tool completes full auth flow (challenge + verify + bearer token) and data operations without crashes under ASAN
  3. SIGHUP config reload works correctly under single-threaded model (rate limit change observed, TLS cert swap, no crash)
  4. SIGTERM graceful shutdown drains active connections and exits cleanly under ASAN with no leak reports beyond accepted shutdown leaks
**Plans**: 1 plan

Plans:
- [ ] 112-01-PLAN.md — LSAN suppression file + ASAN test harness + run verification

### Phase 113: Performance Benchmarking
**Goal**: Relay performance baselines are measured and recorded under four workload types
**Depends on**: Phase 112
**Requirements**: PERF-01, PERF-02, PERF-03, PERF-04
**Success Criteria** (what must be TRUE):
  1. Throughput benchmark produces blobs/sec at 1, 10, and 100 concurrent HTTP clients with results recorded in a benchmark report
  2. Latency benchmark measures per-operation round-trip time (p50/p95/p99) through HTTP relay with results recorded
  3. Large blob benchmark demonstrates successful write+read at 1 MiB, 10 MiB, 50 MiB, and 100 MiB with MiB/sec throughput recorded
  4. Mixed workload benchmark runs concurrent small metadata queries alongside large blob transfers and reports small-query latency degradation under load
**Plans**: 1 plan

Plans:
- [ ] 112-01-PLAN.md — LSAN suppression file + ASAN test harness + run verification

## Progress

**Execution Order:**
Phases execute in numeric order: 111 -> 112 -> 113

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 111. Single-Threaded Rewrite | 3/3 | Complete    | 2026-04-14 |
| 112. ASAN Verification | 0/1 | Not started | - |
| 113. Performance Benchmarking | 0/TBD | Not started | - |
