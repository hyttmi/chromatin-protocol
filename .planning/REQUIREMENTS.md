# Requirements: chromatindb

**Defined:** 2026-03-15
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v0.6.0 Requirements

Requirements for real-world validation milestone. Each maps to roadmap phases.

### Container Infrastructure

- [x] **DOCK-01**: Multi-stage Dockerfile produces chromatindb binary in debian:bookworm-slim runtime image with Release build
- [x] **DOCK-02**: docker-compose topology runs 3-5 nodes in chain connectivity with health checks, named volumes, and per-node configs

### Load Generation

- [x] **LOAD-01**: chromatindb_loadgen C++ binary connects as protocol-compliant peer, performs PQ handshake, sends signed blobs
- [x] **LOAD-02**: Load generator supports configurable blob count, sizes, and write rate with timer-driven scheduling (no coordinated omission)
- [x] **LOAD-03**: Mixed-size workload mode distributes blobs across small/medium/large sizes
- [x] **LOAD-04**: run-benchmark.sh script builds images, starts topology, runs all scenarios, collects metrics, generates report

### Performance Measurement

- [x] **PERF-01**: Ingest throughput measured at multiple blob sizes (blobs/sec, MiB/sec, p50/p95/p99 latency)
- [x] **PERF-02**: Sync/replication latency measured (wall-clock time from write on node A to availability on node B)
- [x] **PERF-03**: Multi-hop propagation time measured (A->B->C chain, no direct A-C link)
- [x] **PERF-04**: Late-joiner catch-up time measured (new node joins after data loaded, measures convergence)
- [x] **PERF-05**: Trusted vs PQ handshake comparison run with same workload under both modes

### Observability

- [x] **OBS-01**: Resource profiling captures CPU, memory, disk I/O per container via docker stats during benchmark runs
- [x] **OBS-02**: Benchmark results report generated as structured markdown with hardware specs, topology, per-scenario tables, and analysis
- [x] **OBS-03**: Machine-readable JSON results output for each scenario alongside markdown report

## Future Requirements

### Network Resilience

- **NETW-01**: Network condition simulation via tc (latency, bandwidth constraints)
- **NETW-02**: Chaos engineering / fault injection (node crashes, partitions)

### Advanced Benchmarks

- **ADVB-01**: Pub/sub notification latency (end-to-end, across nodes)
- **ADVB-02**: Concurrent writers to same namespace via delegation
- **ADVB-03**: Dataset scale test (~10 GB full replication across all nodes)
- **ADVB-04**: CI benchmark pipeline for automated regression detection

### Deployment

- **DEPL-01**: Cross-platform Docker images (ARM64 + x86_64)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Grafana/Prometheus monitoring stack | Over-engineering for one-shot benchmark; docker stats + bash sufficient |
| YCSB integration | Java-based, cannot speak PQ-encrypted binary protocol |
| HTTP/REST benchmark endpoint | Violates project constraint (no HTTP API) |
| Kubernetes orchestration | Overkill for 3-5 node validation on single host |
| Custom benchmark framework | Existing chromatindb_bench covers microbenchmarks; scenario scripts sufficient |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| DOCK-01 | Phase 27 | Complete |
| DOCK-02 | Phase 29 | Complete |
| LOAD-01 | Phase 28 | Complete |
| LOAD-02 | Phase 28 | Complete |
| LOAD-03 | Phase 28 | Complete |
| LOAD-04 | Phase 30 | Complete |
| PERF-01 | Phase 30 | Complete |
| PERF-02 | Phase 30 | Complete |
| PERF-03 | Phase 30 | Complete |
| PERF-04 | Phase 30 | Complete |
| PERF-05 | Phase 30 | Complete |
| OBS-01 | Phase 30 | Complete |
| OBS-02 | Phase 31 | Complete |
| OBS-03 | Phase 31 | Complete |

**Coverage:**
- v0.6.0 requirements: 14 total
- Mapped to phases: 14
- Unmapped: 0

---
*Requirements defined: 2026-03-15*
*Last updated: 2026-03-15 after plan-phase revision (DOCK-01 scoped to chromatindb-only for Phase 27)*
