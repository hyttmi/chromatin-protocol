# Requirements: chromatindb v4.0.0 Relay Architecture v3

**Defined:** 2026-04-14
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v4.0.0 Requirements

### Concurrency Model

- [x] **CONC-01**: Relay runs a single io_context thread for all I/O (HTTP accept, connections, UDS, SSE, timers) — no multi-threaded ioc.run()
- [x] **CONC-02**: CPU-heavy operations (TLS handshake, ML-DSA-87 signature verify, large JSON parse/serialize) offload to a thread pool via crypto::offload() pattern, resuming on the event loop thread after completion
- [x] **CONC-03**: All shared data structures (UdsMultiplexer, RequestRouter, SubscriptionTracker, WriteTracker, TokenStore, ResponsePromiseMap) accessed only from the single event loop thread — no mutexes, no strands
- [x] **CONC-04**: Remove all std::mutex and asio::strand code added in Phase 999.10 — clean single-threaded design
- [x] **CONC-05**: relay_main.cpp creates one thread running ioc.run(), plus a configurable thread pool for offload work

### Verification

- [ ] **VER-01**: Relay compiles and all unit tests pass with single-threaded model
- [ ] **VER-02**: Relay runs ASAN-clean under benchmark tool at 1, 10, and 100 concurrent HTTP clients with zero heap-use-after-free or data race reports
- [ ] **VER-03**: Relay handles SIGHUP config reload and SIGTERM graceful shutdown correctly under single-threaded model

### Performance Benchmarking

- [ ] **PERF-01**: Throughput benchmark produces blobs/sec at 1, 10, and 100 concurrent HTTP clients recorded in benchmark report
- [ ] **PERF-02**: Latency benchmark measures per-operation round-trip time (p50/p95/p99) through HTTP relay recorded in benchmark report
- [ ] **PERF-03**: Large blob benchmark measures write+read throughput at 1 MiB, 10 MiB, 50 MiB, 100 MiB with MiB/sec recorded
- [ ] **PERF-04**: Mixed workload benchmark measures small-query latency degradation under concurrent large-blob load

## Out of Scope

| Feature | Reason |
|---------|--------|
| Multi-threaded io_context | Root cause of all ASAN bugs — single-threaded is the fix |
| Strand confinement | Proved insufficient (Phase 999.10 abandoned) |
| HTTP/2 | Not needed pre-MVP |
| WebSocket fallback | Deleted in Phase 999.9 |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| CONC-01 | Phase 111 | Complete |
| CONC-02 | Phase 111 | Complete |
| CONC-03 | Phase 111 | Complete |
| CONC-04 | Phase 111 | Complete |
| CONC-05 | Phase 111 | Complete |
| VER-01 | Phase 111 | Pending |
| VER-02 | Phase 112 | Pending |
| VER-03 | Phase 112 | Pending |
| PERF-01 | Phase 113 | Pending |
| PERF-02 | Phase 113 | Pending |
| PERF-03 | Phase 113 | Pending |
| PERF-04 | Phase 113 | Pending |

**Coverage:**
- v4.0.0 requirements: 12 total
- Mapped to phases: 12/12 (100%)
