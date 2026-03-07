# Roadmap: chromatindb

## Milestones

- v1.0 MVP -- Phases 1-8 (shipped 2026-03-05)
- v2.0 Closed Node Model -- Phases 9-11 (in progress)

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

### v2.0 Closed Node Model (In Progress)

**Milestone Goal:** Transform chromatindb from an open permissionless node into a hostable secure storage service with access control and larger blob support.

- [x] **Phase 9: Source Restructure** - Move to /db layout and rename namespace to chromatindb::
- [x] **Phase 10: Access Control** - Closed node model with allowed_keys config and connection-level gating
- [ ] **Phase 11: Larger Blob Support** - Bump blob limit to 100 MiB with safe sync and transport

## Phase Details

### Phase 9: Source Restructure
**Goal**: Codebase uses the chromatindb:: namespace and /db directory layout, ready for feature work without merge conflicts
**Depends on**: Phase 8 (v1.0 complete)
**Requirements**: STRUCT-01, STRUCT-02, STRUCT-03
**Success Criteria** (what must be TRUE):
  1. All source files live under the /db directory layout with updated CMakeLists.txt
  2. Every C++ namespace reference and FlatBuffers schema uses chromatindb:: instead of chromatin::
  3. All 155 existing tests pass after a clean build (rm -rf build && cmake && make && ctest)
**Plans**: 2 plans
- [x] 09-01-PLAN.md -- Move src/ to db/ directory layout and update all paths
- [x] 09-02-PLAN.md -- Rename namespace to chromatindb:: and verify clean build

### Phase 10: Access Control
**Goal**: Node operators can restrict which pubkeys connect, creating a fully closed node that rejects unauthorized peers
**Depends on**: Phase 9
**Requirements**: ACL-01, ACL-02, ACL-03, ACL-04, ACL-05
**Success Criteria** (what must be TRUE):
  1. A node with non-empty allowed_keys in config rejects connections from pubkeys not in the list (closed mode), while an empty list allows all connections (open mode, backward compatible with v1.0)
  2. Unauthorized peers are disconnected after handshake but before entering PeerManager state -- they never see any data
  3. PEX is disabled when the node is in closed mode so it does not advertise or accept peer addresses
  4. Sending SIGHUP to the daemon reloads allowed_keys from config without restarting, and peers whose pubkey was removed are immediately disconnected
**Plans**: 3 plans
- [x] 10-01-PLAN.md -- Config extensions + AccessControl class
- [x] 10-02-PLAN.md -- PeerManager ACL integration + PEX disable
- [x] 10-03-PLAN.md -- SIGHUP reload + peer revocation

### Phase 11: Larger Blob Support
**Goal**: Nodes accept, store, and sync blobs up to 100 MiB without memory exhaustion or sync failure
**Depends on**: Phase 10
**Requirements**: BLOB-01, BLOB-02, BLOB-03, BLOB-04, BLOB-05, BLOB-06
**Success Criteria** (what must be TRUE):
  1. A 100 MiB blob can be ingested, stored, and retrieved by a single node
  2. Two nodes can sync a namespace containing 100 MiB blobs without OOM or timeout
  3. Oversized blobs (>100 MiB) are rejected at ingest before signature verification
  4. Malformed frame headers declaring huge lengths are rejected before buffer allocation
  5. Sync hash collection does not load blob data into memory (reads hashes from index only)
**Plans**: TBD

## Progress

**Execution Order:** Phase 9 -> Phase 10 -> Phase 11

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
| 11. Larger Blob Support | 1/3 | In Progress|  | - |
