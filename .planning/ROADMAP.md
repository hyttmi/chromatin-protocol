# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- ✅ **v0.4.0 Production Readiness** — Phases 16-21 (shipped 2026-03-13)
- ✅ **v0.5.0 Hardening & Flexibility** — Phases 22-26 (shipped 2026-03-15)
- ✅ **v0.6.0 Real-World Validation** — Phases 27-31 (shipped 2026-03-16)
- [ ] **v0.7.0 Production Readiness** — Phases 32-37 (in progress)

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

<details>
<summary>✅ v0.6.0 Real-World Validation (Phases 27-31) — SHIPPED 2026-03-16</summary>

- [x] Phase 27: Container Build (1/1 plans) — completed 2026-03-15
- [x] Phase 28: Load Generator (1/1 plans) — completed 2026-03-15
- [x] Phase 29: Multi-Node Topology (1/1 plans) — completed 2026-03-16
- [x] Phase 30: Benchmark Scenarios (2/2 plans) — completed 2026-03-16
- [x] Phase 31: Report Generation (1/1 plans) — completed 2026-03-16

Full details: [milestones/v0.6.0-ROADMAP.md](milestones/v0.6.0-ROADMAP.md)

</details>

### v0.7.0 Production Readiness (In Progress)

**Milestone Goal:** Cleanup, performance optimization, and production-readiness features -- move tests into db/, fix large blob crypto bottleneck, add sync resumption with per-peer cursors, implement namespace quotas, benchmark deletion, and sweep stale artifacts.

- [ ] **Phase 32: Test Relocation** - Move all database tests into db/ for component self-containment
- [ ] **Phase 33: Crypto Throughput Optimization** - Eliminate redundant hashing and allocation in the ingest/verify hot path
- [ ] **Phase 34: Sync Resumption** - Per-peer per-namespace cursors transform sync from O(total) to O(new)
- [ ] **Phase 35: Namespace Quotas** - Per-namespace byte and blob count limits enforced at ingest
- [ ] **Phase 36: Deletion Benchmarks** - Tombstone creation, propagation, and GC performance in Docker suite
- [ ] **Phase 37: General Cleanup** - Remove stale artifacts, update documentation, sweep dead code

## Phase Details

### Phase 32: Test Relocation
**Goal**: db/ is a fully self-contained CMake component with all its tests co-located
**Depends on**: Nothing (first phase of v0.7.0)
**Requirements**: CLEAN-01
**Success Criteria** (what must be TRUE):
  1. All database test source files live under db/tests/ (no test files for db/ remain in the top-level test directory)
  2. `ctest -N` reports exactly 284 tests after relocation (same count as before)
  3. `cmake --build .` from the top-level project builds and discovers all tests without manual path fixups
**Plans**: TBD

### Phase 33: Crypto Throughput Optimization
**Goal**: Large blob (1 MiB) ingest and sync verification throughput measurably improved by eliminating redundant work in the hot path
**Depends on**: Phase 32
**Requirements**: PERF-01, PERF-02, PERF-03
**Success Criteria** (what must be TRUE):
  1. Blob content hash is computed once during ingest and passed through to storage (no redundant SHA3-256 + FlatBuffer re-encode)
  2. OQS_SIG context for ML-DSA-87 is allocated once (static or thread_local) and reused across all verify calls
  3. Sync-received blobs that already exist locally skip signature verification entirely (has_blob check before crypto)
  4. All 284+ tests pass with no regressions
**Plans**: TBD

### Phase 34: Sync Resumption
**Goal**: Sync rounds between peers exchange only hashes for blobs added since the last successful sync, transforming cost from O(total_blobs) to O(new_blobs)
**Depends on**: Phase 33
**Requirements**: SYNC-01, SYNC-02, SYNC-03, SYNC-04
**Success Criteria** (what must be TRUE):
  1. After a full sync round completes, a subsequent round with no new blobs exchanges zero hashes for unchanged namespaces
  2. Sync cursors survive node restart (stored in libmdbx sub-database) and resume where they left off
  3. A periodic full hash-diff resync triggers every Nth round (configurable, default 10) to catch any cursor drift
  4. Cursor-based sync produces identical final state as full hash-list diff sync (no missing or extra blobs)
**Plans**: TBD

### Phase 35: Namespace Quotas
**Goal**: Node operators can limit per-namespace resource usage with byte and blob count caps enforced atomically at ingest
**Depends on**: Phase 33
**Requirements**: QUOTA-01, QUOTA-02, QUOTA-03, QUOTA-04
**Success Criteria** (what must be TRUE):
  1. A blob write that would exceed the namespace byte limit is rejected before storage, and the writer receives a clear quota-exceeded error
  2. A blob write that would exceed the namespace blob count limit is rejected before storage, and the writer receives a clear quota-exceeded error
  3. Namespace usage (bytes + count) is tracked in a materialized aggregate that is O(1) on the write path (no full-namespace scan)
  4. Quota configuration is reloadable via SIGHUP without node restart
**Plans**: TBD

### Phase 36: Deletion Benchmarks
**Goal**: Tombstone creation, sync propagation, and garbage collection performance are measured and baselined in the existing Docker benchmark suite
**Depends on**: Phase 33
**Requirements**: BENCH-01, BENCH-02, BENCH-03
**Success Criteria** (what must be TRUE):
  1. Docker benchmark suite includes a scenario that creates tombstones and reports creation throughput (blobs/sec)
  2. Multi-node tombstone sync propagation latency is measured and reported (time from creation to presence on all nodes)
  3. Tombstone GC/expiry performance under load is measured (time to reclaim storage after tombstone TTL expires)
**Plans**: TBD

### Phase 37: General Cleanup
**Goal**: Stale artifacts from previous milestones are removed, documentation reflects current state, and the codebase is clean for the next milestone
**Depends on**: Phase 32, Phase 33, Phase 34, Phase 35, Phase 36
**Requirements**: CLEAN-02, CLEAN-03, CLEAN-04
**Success Criteria** (what must be TRUE):
  1. The old standalone benchmark binary (chromatindb_bench) is removed from the CMake build and its source files deleted
  2. db/README.md documents all current features (including v0.7.0 additions) with no references to removed benchmarks or outdated data
  3. No stale artifacts remain (dead code paths, leftover files from previous milestones, unreferenced CMake targets)
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 32 -> 33 -> 34 -> 35 -> 36 -> 37
Note: Phases 34, 35, 36 all depend on 33 but not on each other. Phase 37 depends on all prior phases.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 32. Test Relocation | 0/TBD | Not started | - |
| 33. Crypto Throughput Optimization | 0/TBD | Not started | - |
| 34. Sync Resumption | 0/TBD | Not started | - |
| 35. Namespace Quotas | 0/TBD | Not started | - |
| 36. Deletion Benchmarks | 0/TBD | Not started | - |
| 37. General Cleanup | 0/TBD | Not started | - |
