# Roadmap: chromatindb

## Milestones

- v1.0 MVP -- Phases 1-8 (shipped 2026-03-05)
- v2.0 Closed Node Model -- Phases 9-11 (shipped 2026-03-07)
- v3.0 Real-time & Delegation -- Phases 12-15 (in progress)

## Phases

<details>
<summary>v1.0 MVP (Phases 1-8) -- SHIPPED 2026-03-05</summary>

- [x] Phase 1: Foundation (4/4 plans) -- completed 2026-03-03
- [x] Phase 2: Storage Engine (3/3 plans) -- completed 2026-03-03
- [x] Phase 3: Blob Engine (2/2 plans) -- completed 2026-03-03
- [x] Phase 4: Networking (3/3 plans) -- completed 2026-03-04
- [x] Phase 5: Peer System (3/3 plans) -- completed 2026-03-04
- [x] Phase 6: Complete Sync Receive Side (2/2 plans) -- completed 2026-03-05
- [x] Phase 7: Peer Discovery (2/2 plans) -- completed 2026-03-05
- [x] Phase 8: Verification & Cleanup (2/2 plans) -- completed 2026-03-05

Full details: [milestones/v1.0-ROADMAP.md](milestones/v1.0-ROADMAP.md)

</details>

<details>
<summary>v2.0 Closed Node Model (Phases 9-11) -- SHIPPED 2026-03-07</summary>

- [x] Phase 9: Source Restructure (2/2 plans) -- completed 2026-03-06
- [x] Phase 10: Access Control (3/3 plans) -- completed 2026-03-06
- [x] Phase 11: Larger Blob Support (3/3 plans) -- completed 2026-03-07

Full details: [milestones/v2.0-ROADMAP.md](milestones/v2.0-ROADMAP.md)

</details>

### v3.0 Real-time & Delegation (In Progress)

**Milestone Goal:** Make chromatindb collaborative and responsive -- pub/sub notifications for real-time awareness, namespace delegation for multi-writer support, and explicit blob deletion via tombstones.

- [x] **Phase 12: Blob Deletion** - Owner-signed tombstones that permanently delete blobs and replicate via sync + liboqs build optimization
- [x] **Phase 13: Namespace Delegation** - Signed delegation blobs granting write access to other pubkeys (completed 2026-03-08)
- [ ] **Phase 14: Pub/Sub Notifications** - Real-time SUBSCRIBE/NOTIFICATION wire messages for namespace changes
- [ ] **Phase 15: Polish & Benchmarks** - README documentation and performance test suite

## Phase Details

### Phase 12: Blob Deletion
**Goal**: Namespace owners can permanently delete blobs via signed tombstones that replicate across the network
**Depends on**: Phase 11 (v2.0 baseline)
**Requirements**: DEL-01, DEL-02, DEL-03, DEL-04, BUILD-01
**Success Criteria** (what must be TRUE):
  1. Namespace owner can submit a signed tombstone targeting a specific blob hash, and the node deletes that blob
  2. Tombstones are stored permanently (TTL=0) and survive expiry pruning
  3. A node receiving a tombstone via sync deletes the target blob locally and retains the tombstone
  4. Tombstones appear in hash-list diffs and propagate to peers that do not yet have them
  5. A node that already holds a tombstone for a blob hash rejects that blob on subsequent sync or ingest (tombstone-before-blob edge case)
**Plans**: 2/2

Plans:
- [x] 12-01: Tombstone deletion (DEL-01, DEL-02, DEL-03, DEL-04) -- completed 2026-03-07
- [x] 12-02: liboqs build optimization (BUILD-01) -- completed 2026-03-07

### Phase 13: Namespace Delegation
**Goal**: Namespace owners can grant write access to other pubkeys via signed delegation blobs, enabling multi-writer namespaces
**Depends on**: Phase 12 (tombstones required for delegation revocation)
**Requirements**: DELEG-01, DELEG-02, DELEG-03, DELEG-04
**Success Criteria** (what must be TRUE):
  1. Namespace owner can create a signed delegation blob that names a delegate pubkey, and the node stores it in the owner's namespace
  2. A delegate can write blobs to a namespace they have been delegated to, signing with their own key, and the node accepts the write after verifying a valid delegation blob exists
  3. Delegation blobs replicate to peers via the existing sync protocol like any other blob
  4. Owner can revoke a delegation by tombstoning the delegation blob, and the node rejects subsequent writes from that delegate
  5. Delegation verification on the write hot-path is efficient (indexed lookup, not storage scan)
**Plans**: 2/2

Plans:
- [x] 13-01: Delegation blob format, storage delegation index, and delegation blob creation (DELEG-01, DELEG-03) -- completed 2026-03-08
- [x] 13-02: Delegate write acceptance in ingest and revocation via tombstone (DELEG-02, DELEG-04) -- completed 2026-03-08

### Phase 14: Pub/Sub Notifications
**Goal**: Connected peers receive real-time notifications when blobs are ingested or deleted in namespaces they subscribe to
**Depends on**: Phase 12 (tombstone notifications require deletion support)
**Requirements**: SUB-01, SUB-02, SUB-03, SUB-04, SUB-05
**Success Criteria** (what must be TRUE):
  1. A connected peer can send a SUBSCRIBE message for one or more namespaces and receives a NOTIFICATION (namespace + seq_num + hash + size) whenever a blob is ingested into a subscribed namespace
  2. A connected peer can unsubscribe from namespaces and stops receiving notifications for them
  3. Subscriptions are connection-scoped -- a peer that disconnects and reconnects has no active subscriptions until it re-subscribes
  4. When a blob is deleted (tombstoned) in a subscribed namespace, subscribers receive a notification for the tombstone event
**Plans**: 2/2

Plans:
- [x] 14-01: Wire format + subscription state management (SUB-01, SUB-03, SUB-04) -- completed 2026-03-08
- [ ] 14-02: Notification dispatch + E2E tests (SUB-02, SUB-05)

### Phase 15: Polish & Benchmarks
**Goal**: Documentation and performance validation for the complete v3.0 feature set
**Depends on**: Phase 14 (all protocol features must be complete for accurate docs and benchmarks)
**Requirements**: DOCS-01, PERF-01
**Success Criteria** (what must be TRUE):
  1. README.md documents how to build, configure, and interact with chromatindb nodes (write, read, sync, subscribe, delegate, delete)
  2. Performance test suite produces concrete numbers for key operations (ingest, sync, notification latency)
  3. Benchmark results are reproducible and documented
**Plans**: TBD

Plans:
- [ ] 15-01: TBD
- [ ] 15-02: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 12 -> 13 -> 14 -> 15

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
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
| 14. Pub/Sub Notifications | 1/2 | In Progress|  | - |
| 15. Polish & Benchmarks | v3.0 | 0/? | Not started | - |
