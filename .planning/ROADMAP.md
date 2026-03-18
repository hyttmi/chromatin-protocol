# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- ✅ **v0.4.0 Production Readiness** — Phases 16-21 (shipped 2026-03-13)
- ✅ **v0.5.0 Hardening & Flexibility** — Phases 22-26 (shipped 2026-03-15)
- ✅ **v0.6.0 Real-World Validation** — Phases 27-31 (shipped 2026-03-16)
- ✅ **v0.7.0 Production Readiness** — Phases 32-37 (shipped 2026-03-18)
- [ ] **v1.0.0 Performance & Production Readiness** — Phases 38-41 (in progress)

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

<details>
<summary>✅ v0.7.0 Production Readiness (Phases 32-37) — SHIPPED 2026-03-18</summary>

- [x] Phase 32: Test Relocation (1/1 plans) — completed 2026-03-17
- [x] Phase 33: Crypto Throughput Optimization (2/2 plans) — completed 2026-03-17
- [x] Phase 34: Sync Resumption (3/3 plans) — completed 2026-03-18
- [x] Phase 35: Namespace Quotas (2/2 plans) — completed 2026-03-18
- [x] Phase 36: Deletion Benchmarks (2/2 plans) — completed 2026-03-18
- [x] Phase 37: General Cleanup (2/2 plans) — completed 2026-03-18

Full details: [milestones/v0.7.0-ROADMAP.md](milestones/v0.7.0-ROADMAP.md)

</details>

### v1.0.0 Performance & Production Readiness (In Progress)

**Milestone Goal:** Thread pool crypto offload to break the large-blob CPU bottleneck, cursor compaction for stale peers, connection retry with exponential backoff, and benchmark validation confirming throughput improvements. This is the "database layer is done" release.

- [ ] **Phase 38: Thread Pool Crypto Offload** - ML-DSA-87 verify and SHA3-256 hash dispatched to asio::thread_pool, freeing the event loop
- [ ] **Phase 39: Cursor Compaction** - Stale peer cursors automatically pruned based on configurable retention
- [ ] **Phase 40: Connection Retry** - Automatic reconnection to configured peers with exponential backoff and ACL-aware suppression
- [ ] **Phase 41: Benchmark Validation** - Full benchmark suite re-run confirming thread pool throughput gains and no small-blob regression

## Phase Details

### Phase 38: Thread Pool Crypto Offload
**Goal**: The event loop never blocks on ML-DSA-87 signature verification or SHA3-256 content hashing -- these CPU-bound operations run on worker threads while the event loop continues processing I/O
**Depends on**: Nothing (first phase of v1.0.0)
**Requirements**: PERF-06, PERF-07, PERF-08, PERF-09
**Success Criteria** (what must be TRUE):
  1. A blob ingest or sync receive that triggers ML-DSA-87 verification does not block the event loop -- other connections continue processing during the verify
  2. A blob ingest or sync receive that triggers SHA3-256 hashing does not block the event loop -- other connections continue processing during the hash
  3. Connection-scoped AEAD state (ChaCha20-Poly1305 nonce counters) is never accessed from a thread pool worker -- only stateless crypto ops are offloaded
  4. Thread pool worker count is configurable via config JSON and defaults to std::thread::hardware_concurrency()
  5. All existing tests pass with thread pool enabled (no concurrency regressions)
**Plans**: TBD

### Phase 39: Cursor Compaction
**Goal**: Stale sync cursors from peers that have not connected for a configurable retention period are automatically pruned, preventing unbounded cursor storage growth
**Depends on**: Phase 38
**Requirements**: SYNC-05
**Success Criteria** (what must be TRUE):
  1. Cursors for peers not seen within the retention period are deleted from the cursor sub-database during periodic compaction
  2. Retention period is configurable in config JSON and reloadable via SIGHUP without restart
  3. Active peer cursors are never pruned (only peers absent longer than retention period)
**Plans**: TBD

### Phase 40: Connection Retry
**Goal**: Nodes automatically maintain connectivity to their configured and bootstrap peers by reconnecting after disconnection with backoff, without wasting resources on peers that have rejected us
**Depends on**: Phase 38
**Requirements**: CONN-01, CONN-02, CONN-03
**Success Criteria** (what must be TRUE):
  1. When a configured/bootstrap peer disconnects, the node automatically attempts to reconnect without operator intervention
  2. Reconnection attempts use exponential backoff (e.g., 1s, 2s, 4s, ...) up to a configurable maximum interval
  3. A peer that sent an ACL rejection is not retried (reconnection suppressed until next config reload or restart)
  4. Successful reconnection resumes normal sync and message exchange as if freshly connected
**Plans**: TBD

### Phase 41: Benchmark Validation
**Goal**: The full Docker benchmark suite confirms that thread pool crypto offload delivers measurable throughput improvement for large blobs with no regression for small/medium blobs
**Depends on**: Phase 38
**Requirements**: BENCH-04, BENCH-05, BENCH-06
**Success Criteria** (what must be TRUE):
  1. The full 5-scenario Docker benchmark suite runs successfully with thread pool offload enabled
  2. 1 MiB blob ingest/sync throughput is measurably improved over the v0.6.0 baseline (15.3 blobs/sec) with the improvement percentage quantified in the report
  3. 1K and 100K blob throughput shows no regression from thread pool dispatch overhead (within 5% of baseline or better)
**Plans**: TBD

## Progress

**Execution Order:**
Phases 38 -> 39 -> 40 -> 41
Note: Phases 39, 40, and 41 all depend on Phase 38 but not on each other. They are sequenced for clean execution.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 38. Thread Pool Crypto Offload | 0/TBD | Not started | - |
| 39. Cursor Compaction | 0/TBD | Not started | - |
| 40. Connection Retry | 0/TBD | Not started | - |
| 41. Benchmark Validation | 0/TBD | Not started | - |
