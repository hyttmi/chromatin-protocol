# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- ✅ **v0.4.0 Production Readiness** — Phases 16-21 (shipped 2026-03-13)
- ✅ **v0.5.0 Hardening & Flexibility** — Phases 22-26 (shipped 2026-03-15)
- 🚧 **v0.6.0 Real-World Validation** — Phases 27-31 (in progress)

## Phases

<details>
<summary>✅ v1.0 MVP (Phases 1-8) — SHIPPED 2026-03-05</summary>

- [x] Phase 1: Foundation (4/4 plans) — completed 2026-03-03
- [x] Phase 2: Storage Engine (3/3 plans) — completed 2026-03-03
- [x] Phase 3: Blob Engine (2/2 plans) — completed 2026-03-03
- [x] Phase 4: Networking (3/3 plans) — completed 2026-03-04
- [x] Phase 5: Peer System (3/3 plans) — completed 2026-03-04
- [x] Phase 6: Complete Sync Receive Side (2/2 plans) — completed 2026-03-05
- [x] Phase 7: Peer Discovery (2/2 plans) — completed 2026-03-05
- [x] Phase 8: Verification & Cleanup (2/2 plans) — completed 2026-03-05

Full details: [milestones/v1.0-ROADMAP.md](milestones/v1.0-ROADMAP.md)

</details>

<details>
<summary>✅ v2.0 Closed Node Model (Phases 9-11) — SHIPPED 2026-03-07</summary>

- [x] Phase 9: Source Restructure (2/2 plans) — completed 2026-03-06
- [x] Phase 10: Access Control (3/3 plans) — completed 2026-03-06
- [x] Phase 11: Larger Blob Support (3/3 plans) — completed 2026-03-07

Full details: [milestones/v2.0-ROADMAP.md](milestones/v2.0-ROADMAP.md)

</details>

<details>
<summary>✅ v3.0 Real-time & Delegation (Phases 12-15) — SHIPPED 2026-03-08</summary>

- [x] Phase 12: Blob Deletion (2/2 plans) — completed 2026-03-07
- [x] Phase 13: Namespace Delegation (2/2 plans) — completed 2026-03-08
- [x] Phase 14: Pub/Sub Notifications (2/2 plans) — completed 2026-03-08
- [x] Phase 15: Polish & Benchmarks (2/2 plans) — completed 2026-03-08

Full details: [milestones/v3.0-ROADMAP.md](milestones/v3.0-ROADMAP.md)

</details>

<details>
<summary>✅ v0.4.0 Production Readiness (Phases 16-21) — SHIPPED 2026-03-13</summary>

- [x] Phase 16: Storage Foundation (3/3 plans) — completed 2026-03-10
- [x] Phase 17: Operational Stability (3/3 plans) — completed 2026-03-10
- [x] Phase 18: Abuse Prevention & Topology (3/3 plans) — completed 2026-03-12
- [x] Phase 19: Documentation & Release (2/2 plans) — completed 2026-03-12
- [x] Phase 20: Metrics Completeness & Consistency (1/1 plans) — completed 2026-03-13
- [x] Phase 21: Test 260 SEGFAULT Fix (1/1 plans) — completed 2026-03-13

Full details: [milestones/v0.4.0-ROADMAP.md](milestones/v0.4.0-ROADMAP.md)

</details>

<details>
<summary>✅ v0.5.0 Hardening & Flexibility (Phases 22-26) — SHIPPED 2026-03-15</summary>

- [x] Phase 22: Build Restructure (1/1 plans) — completed 2026-03-14
- [x] Phase 23: TTL Flexibility (1/1 plans) — completed 2026-03-14
- [x] Phase 24: Encryption at Rest (1/1 plans) — completed 2026-03-14
- [x] Phase 25: Transport Optimization (2/2 plans) — completed 2026-03-15
- [x] Phase 26: Documentation & Release (1/1 plans) — completed 2026-03-15

Full details: [milestones/v0.5.0-ROADMAP.md](milestones/v0.5.0-ROADMAP.md)

</details>

### v0.6.0 Real-World Validation (In Progress)

**Milestone Goal:** Run chromatindb in Docker, measure real-world performance, and validate sync behavior at scale.

- [x] **Phase 27: Container Build** — Multi-stage Dockerfile producing Release binaries in debian:bookworm-slim (completed 2026-03-15)
- [x] **Phase 28: Load Generator** — Protocol-compliant C++ load generation tool with configurable workloads (completed 2026-03-15)
- [x] **Phase 29: Multi-Node Topology** — Docker Compose 3-5 node chain with health checks and named volumes (completed 2026-03-16)
- [ ] **Phase 30: Benchmark Scenarios** — Run all performance scenarios with resource profiling
- [ ] **Phase 31: Report Generation** — Structured benchmark results with analysis and automation

## Phase Details

### Phase 27: Container Build
**Goal**: chromatindb builds and runs in a Docker container with reproducible Release builds
**Depends on**: Nothing (first phase of v0.6.0)
**Requirements**: DOCK-01
**Success Criteria** (what must be TRUE):
  1. `docker build` produces a working image with the `chromatindb` binary (Phase 28 will update the Dockerfile to add `chromatindb_loadgen`)
  2. The container starts, listens on the configured port, and accepts connections from a host-based peer
  3. The image uses debian:bookworm-slim runtime (not the full build image) and builds with CMAKE_BUILD_TYPE=Release
**Plans**: 1 plan

Plans:
- [ ] 27-01-PLAN.md — Multi-stage Dockerfile, .dockerignore, and version bump to 0.6.0

### Phase 28: Load Generator
**Goal**: A standalone C++ tool can generate sustained signed-blob traffic against any chromatindb node
**Depends on**: Phase 27
**Requirements**: LOAD-01, LOAD-02, LOAD-03
**Success Criteria** (what must be TRUE):
  1. `chromatindb_loadgen` connects to a running node, performs PQ handshake, and sends signed blobs that the node accepts and stores
  2. Load generator uses timer-driven fixed-rate scheduling (not response-driven) to prevent coordinated omission
  3. Mixed-size workload mode distributes blobs across small (1 KiB), medium (100 KiB), and large (1+ MiB) sizes
  4. Per-blob ACK latency and summary statistics (blobs/sec, MiB/sec, p50/p95/p99) are emitted as JSON to stdout
**Plans**: 1 plan

Plans:
- [ ] 28-01-PLAN.md — Complete loadgen binary with timer-driven scheduling, mixed sizes, and JSON stats

### Phase 29: Multi-Node Topology
**Goal**: A multi-node chromatindb network runs in Docker Compose with correct connectivity and sync
**Depends on**: Phase 27
**Requirements**: DOCK-02
**Success Criteria** (what must be TRUE):
  1. `docker compose up` starts 3-5 nodes that connect, handshake, and begin syncing
  2. Nodes use named volumes for libmdbx storage (not container filesystem)
  3. A blob written to node1 replicates to all other nodes through the peer chain
  4. A late-joiner node can be started after the initial topology and catches up on existing data
**Plans**: 1 plan

Plans:
- [ ] 29-01-PLAN.md — Docker Compose 3-node chain topology with late-joiner profile and per-node configs

### Phase 30: Benchmark Scenarios
**Goal**: All core performance scenarios are measured with resource profiling
**Depends on**: Phase 28, Phase 29
**Requirements**: PERF-01, PERF-02, PERF-03, PERF-04, PERF-05, OBS-01, LOAD-04
**Success Criteria** (what must be TRUE):
  1. Ingest throughput is measured at multiple blob sizes with blobs/sec, MiB/sec, and p50/p95/p99 latency
  2. Sync latency (write on node A to availability on node B) and multi-hop propagation time (A to B to C) are measured as wall-clock times
  3. Late-joiner catch-up time is measured (new node joins after data loaded, time to full convergence)
  4. Trusted vs PQ handshake overhead is compared with the same workload under both modes
  5. CPU, memory, and disk I/O per container are captured via docker stats during each scenario run
**Plans**: 2 plans

Plans:
- [ ] 30-01-PLAN.md — Benchmark script framework with ingest throughput and sync/multi-hop latency scenarios
- [ ] 30-02-PLAN.md — Late-joiner catch-up and trusted-vs-PQ comparison scenarios

### Phase 31: Report Generation
**Goal**: Benchmark results are aggregated into a structured, reproducible report
**Depends on**: Phase 30
**Requirements**: OBS-02, OBS-03
**Success Criteria** (what must be TRUE):
  1. A markdown report is generated with hardware specs, topology description, per-scenario results tables, and analysis
  2. Machine-readable JSON results are output alongside the markdown report for each scenario
  3. `run-benchmark.sh` automates the full pipeline: build images, start topology, run all scenarios, collect metrics, generate report
**Plans**: TBD

Plans:
- [ ] 31-01: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 27 -> 28 -> 29 -> 30 -> 31

| Phase | Milestone | Plans | Status | Completed |
|-------|-----------|-------|--------|-----------|
| 1. Foundation | v1.0 | 4/4 | Complete | 2026-03-03 |
| 2. Storage Engine | v1.0 | 3/3 | Complete | 2026-03-03 |
| 3. Blob Engine | v1.0 | 2/2 | Complete | 2026-03-03 |
| 4. Networking | v1.0 | 3/3 | Complete | 2026-03-04 |
| 5. Peer System | v1.0 | 3/3 | Complete | 2026-03-04 |
| 6. Complete Sync Receive Side | v1.0 | 2/2 | Complete | 2026-03-05 |
| 7. Peer Discovery | v1.0 | 2/2 | Complete | 2026-03-05 |
| 8. Verification & Cleanup | v1.0 | 2/2 | Complete | 2026-03-05 |
| 9. Source Restructure | v2.0 | 2/2 | Complete | 2026-03-06 |
| 10. Access Control | v2.0 | 3/3 | Complete | 2026-03-06 |
| 11. Larger Blob Support | v2.0 | 3/3 | Complete | 2026-03-07 |
| 12. Blob Deletion | v3.0 | 2/2 | Complete | 2026-03-07 |
| 13. Namespace Delegation | v3.0 | 2/2 | Complete | 2026-03-08 |
| 14. Pub/Sub Notifications | v3.0 | 2/2 | Complete | 2026-03-08 |
| 15. Polish & Benchmarks | v3.0 | 2/2 | Complete | 2026-03-08 |
| 16. Storage Foundation | v0.4.0 | 3/3 | Complete | 2026-03-10 |
| 17. Operational Stability | v0.4.0 | 3/3 | Complete | 2026-03-10 |
| 18. Abuse Prevention & Topology | v0.4.0 | 3/3 | Complete | 2026-03-12 |
| 19. Documentation & Release | v0.4.0 | 2/2 | Complete | 2026-03-12 |
| 20. Metrics Completeness & Consistency | v0.4.0 | 1/1 | Complete | 2026-03-13 |
| 21. Test 260 SEGFAULT Fix | v0.4.0 | 1/1 | Complete | 2026-03-13 |
| 22. Build Restructure | v0.5.0 | 1/1 | Complete | 2026-03-14 |
| 23. TTL Flexibility | v0.5.0 | 1/1 | Complete | 2026-03-14 |
| 24. Encryption at Rest | v0.5.0 | 1/1 | Complete | 2026-03-14 |
| 25. Transport Optimization | v0.5.0 | 2/2 | Complete | 2026-03-15 |
| 26. Documentation & Release | v0.5.0 | 1/1 | Complete | 2026-03-15 |
| 27. Container Build | v0.6.0 | 1/1 | Complete | 2026-03-15 |
| 28. Load Generator | v0.6.0 | 1/1 | Complete | 2026-03-15 |
| 29. Multi-Node Topology | v0.6.0 | 1/1 | Complete | 2026-03-16 |
| 30. Benchmark Scenarios | v0.6.0 | 0/2 | Not started | - |
| 31. Report Generation | v0.6.0 | 0/? | Not started | - |
