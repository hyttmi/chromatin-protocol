# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- **v0.4.0 Production Readiness** — Phases 16-19 (in progress)

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

### v0.4.0 Production Readiness

- [x] **Phase 16: Storage Foundation** - O(1) tombstone lookups, bounded disk usage, disk-full signaling to peers (completed 2026-03-10)
- [x] **Phase 17: Operational Stability** - Graceful shutdown, persistent peer list, runtime observability (completed 2026-03-10)
- [ ] **Phase 18: Abuse Prevention & Topology** - Per-connection rate limiting, namespace-scoped sync filtering
- [ ] **Phase 19: Documentation & Release** - Operator README, interaction samples, version 0.4.0

## Phase Details

### Phase 16: Storage Foundation
**Goal**: Node enforces storage capacity at the protocol boundary with O(1) tombstone verification
**Depends on**: Phase 15
**Requirements**: STOR-01, STOR-02, STOR-03, STOR-04, STOR-05
**Success Criteria** (what must be TRUE):
  1. Tombstone lookups complete in O(1) via indexed sub-database instead of scanning all blobs in the namespace
  2. Node stops accepting blobs when storage exceeds the configured max_storage_bytes limit
  3. Storage capacity check runs before any cryptographic operations on the ingest path
  4. Peers receiving a StorageFull message suppress sync pushes to the full node until reconnection
**Plans**: 3 plans

Plans:
- [ ] 16-01: Tombstone index (tombstone_map DBI, startup migration, O(1) has_tombstone_for)
- [ ] 16-02: Storage limits (used_bytes query, IngestError::storage_full, Step 0 capacity check in ingest)
- [ ] 16-03: Disk-full reporting (StorageFull wire message, peer_is_full flag, sync push suppression)

### Phase 17: Operational Stability
**Goal**: Node survives restarts and crashes without losing peer connections or operational visibility
**Depends on**: Phase 16
**Requirements**: OPS-01, OPS-02, OPS-03, OPS-04, OPS-05, OPS-06, OPS-07
**Success Criteria** (what must be TRUE):
  1. SIGTERM triggers a bounded shutdown that drains in-flight coroutines, saves peer list, and exits cleanly
  2. Node reconnects to previously-known peers after restart without requiring bootstrap re-discovery
  3. Operator can send SIGUSR1 to get a metrics snapshot (connections, storage used, blobs, syncs, rejections) in the log
  4. Metrics are logged automatically every 60 seconds without operator intervention
  5. Expiry scan coroutine is cancellable and does not block shutdown
**Plans**: 3 plans

Plans:
- [ ] 17-01: Graceful shutdown + expiry cancellation (Server on_shutdown callback, re-arming signal handler, cancellable expiry coroutine, exit code propagation)
- [ ] 17-02: Persistent peer list + NodeMetrics (atomic file write, 30s periodic flush, NodeMetrics struct, counter instrumentation)
- [ ] 17-03: SIGUSR1 dump + periodic metrics log (sigusr1_loop coroutine, dump_metrics, metrics_timer_loop, structured key=value output)

### Phase 18: Abuse Prevention & Topology
**Goal**: Open nodes resist write-flooding abuse and operators control which namespaces replicate
**Depends on**: Phase 17
**Requirements**: PROT-01, PROT-02, PROT-03, PROT-04, PROT-05, PROT-06
**Success Criteria** (what must be TRUE):
  1. A peer exceeding the configured write rate is disconnected via the strike system without stalling the io_context
  2. Rate limiting applies only to Data and Delete messages, not to sync BlobTransfer traffic
  3. Operator can configure sync_namespaces to restrict which namespaces the node replicates
  4. Namespace filter is applied at sync Phase A (namespace list assembly) so filtered namespace IDs are never sent to peers
  5. Empty sync_namespaces defaults to replicate-all behavior (backward compatible)
**Plans**: 2 plans

Plans:
- [ ] 18-01: Rate limiting (token bucket in PeerInfo, config fields, strike integration)
- [ ] 18-02: Namespace-scoped sync (sync_namespaces config, std::set filter at Phase A)

### Phase 19: Documentation & Release
**Goal**: Operator can deploy and interact with chromatindb using documented procedures
**Depends on**: Phase 18
**Requirements**: DOC-01, DOC-02, DOC-03, DOC-04
**Success Criteria** (what must be TRUE):
  1. db/README.md documents config schema, startup command, wire protocol overview, and deployment scenarios
  2. Interaction samples file demonstrates how to connect to and use the database programmatically
  3. version.h reports 0.4.0 and all tests pass with the new version
**Plans**: 2 plans

Plans:
- [ ] 19-01: README and interaction samples (db/README.md, usage examples)
- [ ] 19-02: Version bump (version.h to 0.4.0, test verification)

## Progress

**Execution Order:**
Phases execute in numeric order: 16 -> 17 -> 18 -> 19

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
| 16. Storage Foundation | 3/3 | Complete    | 2026-03-10 | - |
| 17. Operational Stability | 3/3 | Complete   | 2026-03-10 | - |
| 18. Abuse Prevention & Topology | v0.4.0 | 0/2 | Not started | - |
| 19. Documentation & Release | v0.4.0 | 0/2 | Not started | - |
