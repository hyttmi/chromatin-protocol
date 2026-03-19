# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- ✅ **v0.4.0 Production Readiness** — Phases 16-21 (shipped 2026-03-13)
- ✅ **v0.5.0 Hardening & Flexibility** — Phases 22-26 (shipped 2026-03-15)
- ✅ **v0.6.0 Real-World Validation** — Phases 27-31 (shipped 2026-03-16)
- ✅ **v0.7.0 Production Readiness** — Phases 32-37 (shipped 2026-03-18)
- [ ] **v0.8.0 Protocol Scalability** — Phases 38-41 (in progress)

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

### v0.8.0 Protocol Scalability (In Progress)

**Milestone Goal:** Fix the fundamental sync protocol scaling flaw (O(N) hash list exchange breaks at ~3.4M blobs per namespace), harden against sync-based abuse, and offload CPU-bound crypto to worker threads. The sync protocol must scale honestly before claiming production readiness.

- [x] **Phase 38: Thread Pool Crypto Offload** - ML-DSA-87 verify and SHA3-256 hash dispatched to asio::thread_pool, freeing the event loop (completed 2026-03-19)
- [x] **Phase 39: Negentropy Set Reconciliation** - Replace O(N) hash list exchange with O(differences) custom XOR-fingerprint reconciliation protocol per namespace (completed 2026-03-19)
- [ ] **Phase 40: Sync Rate Limiting** - Metered sync initiation, byte-rate accounting, and concurrent session limits per peer
- [ ] **Phase 41: Benchmark Validation** - Docker benchmark confirms O(diff) scaling improvement and no regression

## Phase Details

### Phase 38: Thread Pool Crypto Offload
**Goal**: The event loop never blocks on ML-DSA-87 signature verification or SHA3-256 content hashing -- these CPU-bound operations run on worker threads while the event loop continues processing I/O
**Depends on**: Nothing (first phase of v0.8.0)
**Requirements**: PERF-06, PERF-07, PERF-08, PERF-09
**Success Criteria** (what must be TRUE):
  1. A blob ingest or sync receive that triggers ML-DSA-87 verification does not block the event loop -- other connections continue processing during the verify
  2. A blob ingest or sync receive that triggers SHA3-256 hashing does not block the event loop -- other connections continue processing during the hash
  3. Connection-scoped AEAD state (ChaCha20-Poly1305 nonce counters) is never accessed from a thread pool worker -- only stateless crypto ops are offloaded
  4. Thread pool worker count is configurable via config JSON and defaults to std::thread::hardware_concurrency()
  5. All existing tests pass with thread pool enabled (no concurrency regressions)
**Plans**: 3 plans

Plans:
- [x] 38-01-PLAN.md — Config, thread pool lifecycle, offload helper, plumbing pool ref through object graph
- [x] 38-02-PLAN.md — BlobEngine async conversion with two-dispatch crypto offload, caller updates
- [x] 38-03-PLAN.md — Connection handshake Signer::verify offload to thread pool

### Phase 39: Negentropy Set Reconciliation
**Goal**: Namespace sync uses custom XOR-fingerprint range-based set reconciliation instead of full hash list exchange, making sync cost proportional to differences (O(diff)) not total blobs (O(N)), and eliminating the ~3.4M blob MAX_FRAME_SIZE cliff
**Depends on**: Phase 38
**Requirements**: SYNC-06, SYNC-07, SYNC-08, SYNC-09
**Success Criteria** (what must be TRUE):
  1. Custom reconciliation module built with no external dependency (XOR-fingerprint range-based algorithm in ~400-500 lines of owned code)
  2. When two nodes sync a namespace with N blobs and D differences, wire traffic scales with D not N -- a namespace with 1M blobs and 10 new blobs does not exchange 32 MB of hashes
  3. Sync cursors from v0.7.0 still skip unchanged namespaces entirely -- reconciliation only runs for namespaces where the cursor indicates new data
  4. Reconciliation wire messages carry a version byte so future protocol changes can coexist with older peers
**Plans**: 2 plans

Plans:
- [x] 39-01-PLAN.md — Reconciliation module (XOR fingerprint algorithm, encode/decode, wire protocol update, unit tests)
- [x] 39-02-PLAN.md — Sync flow integration (replace Phase B in initiator/responder, message routing, PROTOCOL.md)

### Phase 40: Sync Rate Limiting
**Goal**: Sync requests are metered per peer to prevent resource exhaustion via repeated sync initiation, closing the abuse vector where sync messages bypass all existing rate limiting
**Depends on**: Phase 39
**Requirements**: RATE-01, RATE-02, RATE-03
**Success Criteria** (what must be TRUE):
  1. A peer that initiates sync more frequently than the configured cooldown is rejected with a rate-limit response -- the node does not begin reconciliation
  2. Sync message bytes (including reconciliation rounds) count against the existing per-peer byte-rate token bucket -- a peer cannot bypass bandwidth limits via sync traffic
  3. A peer cannot open more than the configured maximum number of concurrent sync sessions -- excess sync requests are rejected
**Plans**: TBD

### Phase 41: Benchmark Validation
**Goal**: The Docker benchmark suite confirms that set reconciliation delivers O(diff) sync scaling, thread pool offload improves large-blob throughput, and neither change causes regression for small namespaces
**Depends on**: Phase 38, Phase 39, Phase 40
**Requirements**: SYNC-10
**Success Criteria** (what must be TRUE):
  1. A benchmark scenario with a large namespace (1000+ blobs) and few new blobs (10) demonstrates sync wire traffic and time proportional to differences, not total namespace size
  2. 1 MiB blob ingest/sync throughput is measurably improved over the v0.6.0 baseline (15.3 blobs/sec) with the improvement percentage quantified in the report
  3. Small namespace sync (under 100 blobs) shows no regression from reconciliation or thread pool overhead (within 5% of baseline or better)
**Plans**: TBD

## Progress

**Execution Order:**
Phases 38 -> 39 -> 40 -> 41
Note: Phase 38 (thread pool) is protocol-agnostic and executes first. Phase 39 (reconciliation) is the largest change. Phase 40 (rate limiting) benefits from reconciliation being in place. Phase 41 (benchmarks) validates the full stack.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 38. Thread Pool Crypto Offload | 3/3 | Complete    | 2026-03-19 |
| 39. Set Reconciliation | 2/2 | Complete    | 2026-03-19 |
| 40. Sync Rate Limiting | 0/TBD | Not started | - |
| 41. Benchmark Validation | 0/TBD | Not started | - |
