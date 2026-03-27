# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- ✅ **v0.4.0 Production Readiness** — Phases 16-21 (shipped 2026-03-13)
- ✅ **v0.5.0 Hardening & Flexibility** — Phases 22-26 (shipped 2026-03-15)
- ✅ **v0.6.0 Real-World Validation** — Phases 27-31 (shipped 2026-03-16)
- ✅ **v0.7.0 Production Readiness** — Phases 32-37 (shipped 2026-03-18)
- ✅ **v0.8.0 Protocol Scalability** — Phases 38-41 (shipped 2026-03-19)
- ✅ **v0.9.0 Connection Resilience & Hardening** — Phases 42-45 (shipped 2026-03-20)
- ✅ **v1.0.0 Database Layer Done** — Phases 46-52 (shipped 2026-03-22)
- ✅ **v1.1.0 Operational Polish & Local Access** — Phases 53-56 (shipped 2026-03-22)
- ✅ **v1.2.0 Relay & Client Protocol** — Phases 57-60 (shipped 2026-03-23)
- ✅ **v1.3.0 Protocol Concurrency & Query Foundation** — Phases 61-64 (shipped 2026-03-26)
- [ ] **v1.4.0 Extended Query Suite** — Phases 65-67 (in progress)

## Phases

<details>
<summary>v1.0 MVP (Phases 1-8) — SHIPPED 2026-03-05</summary>

Full details: [milestones/v1.0-ROADMAP.md](milestones/v1.0-ROADMAP.md)

</details>

<details>
<summary>v2.0 Closed Node Model (Phases 9-11) — SHIPPED 2026-03-07</summary>

Full details: [milestones/v2.0-ROADMAP.md](milestones/v2.0-ROADMAP.md)

</details>

<details>
<summary>v3.0 Real-time & Delegation (Phases 12-15) — SHIPPED 2026-03-08</summary>

Full details: [milestones/v3.0-ROADMAP.md](milestones/v3.0-ROADMAP.md)

</details>

<details>
<summary>v0.4.0 Production Readiness (Phases 16-21) — SHIPPED 2026-03-13</summary>

Full details: [milestones/v0.4.0-ROADMAP.md](milestones/v0.4.0-ROADMAP.md)

</details>

<details>
<summary>v0.5.0 Hardening & Flexibility (Phases 22-26) — SHIPPED 2026-03-15</summary>

Full details: [milestones/v0.5.0-ROADMAP.md](milestones/v0.5.0-ROADMAP.md)

</details>

<details>
<summary>v0.6.0 Real-World Validation (Phases 27-31) — SHIPPED 2026-03-16</summary>

Full details: [milestones/v0.6.0-ROADMAP.md](milestones/v0.6.0-ROADMAP.md)

</details>

<details>
<summary>v0.7.0 Production Readiness (Phases 32-37) — SHIPPED 2026-03-18</summary>

Full details: [milestones/v0.7.0-ROADMAP.md](milestones/v0.7.0-ROADMAP.md)

</details>

<details>
<summary>v0.8.0 Protocol Scalability (Phases 38-41) — SHIPPED 2026-03-19</summary>

Full details: [milestones/v0.8.0-ROADMAP.md](milestones/v0.8.0-ROADMAP.md)

</details>

<details>
<summary>v0.9.0 Connection Resilience & Hardening (Phases 42-45) — SHIPPED 2026-03-20</summary>

Full details: [milestones/v0.9.0-ROADMAP.md](milestones/v0.9.0-ROADMAP.md)

</details>

<details>
<summary>v1.0.0 Database Layer Done (Phases 46-52) — SHIPPED 2026-03-22</summary>

Full details: [milestones/v1.0.0-ROADMAP.md](milestones/v1.0.0-ROADMAP.md)

</details>

<details>
<summary>v1.1.0 Operational Polish & Local Access (Phases 53-56) — SHIPPED 2026-03-22</summary>

Full details: [milestones/v1.1.0-ROADMAP.md](milestones/v1.1.0-ROADMAP.md)

</details>

<details>
<summary>v1.2.0 Relay & Client Protocol (Phases 57-60) — SHIPPED 2026-03-23</summary>

Full details: [milestones/v1.2.0-ROADMAP.md](milestones/v1.2.0-ROADMAP.md)

</details>

<details>
<summary>v1.3.0 Protocol Concurrency & Query Foundation (Phases 61-64) — SHIPPED 2026-03-26</summary>

- [x] Phase 61: Transport Foundation (3/3 plans) — completed 2026-03-25
- [x] Phase 62: Concurrent Dispatch (1/1 plan) — completed 2026-03-25
- [x] Phase 63: Query Extensions (2/2 plans) — completed 2026-03-25
- [x] Phase 64: Documentation (2/2 plans) — completed 2026-03-26

Full details: [milestones/v1.3.0-ROADMAP.md](milestones/v1.3.0-ROADMAP.md)

</details>

### v1.4.0 Extended Query Suite (In Progress)

**Milestone Goal:** Add 10 new query/response message type pairs (20 enum values, types 41-60) expanding the client-facing API with health, namespace inspection, metadata, batch operations, delegations, peer topology, and time-range queries.

- [x] **Phase 65: Node-Level Queries** - NamespaceList, StorageStatus, NamespaceStats (HealthRequest cut -- NodeInfo suffices) (completed 2026-03-26)
- [x] **Phase 66: Blob-Level Queries** - MetadataRequest, BatchExists, DelegationList with minor new Storage methods (completed 2026-03-26)
- [ ] **Phase 67: Batch/Range Queries & Integration** - BatchRead, PeerInfo, TimeRange + relay filter + NodeInfo update + PROTOCOL.md

## Phase Details

### Phase 65: Node-Level Queries
**Goal**: Operators and clients can enumerate namespaces, query storage status, and query per-namespace statistics
**Depends on**: Phase 64 (v1.3.0 -- coroutine-IO dispatch, request_id, NodeInfo pattern)
**Requirements**: QUERY-05 (dropped), QUERY-06, QUERY-07, QUERY-08
**Success Criteria** (what must be TRUE):
  1. ~~HealthRequest~~ -- CUT (NodeInfoResponse already serves as health check)
  2. Client can list all namespaces stored on the node with pagination (after_namespace cursor + limit), receiving namespace hashes and counts
  3. Client can query node-level storage status including used bytes, quota headroom, and tombstone counts in a single request
  4. Client can query per-namespace statistics (blob count, total bytes, delegation count, quota usage) for any namespace
  5. All three new request/response types pass through relay message filter and work over both TCP (via relay) and UDS paths
**Plans**: 2 plans

Plans:
- [x] 65-01-PLAN.md — Schema types 41-46, Storage methods (count_tombstones, count_delegations), relay filter update
- [x] 65-02-PLAN.md — NamespaceList, StorageStatus, NamespaceStats handlers with integration tests

### Phase 66: Blob-Level Queries
**Goal**: Clients can inspect individual blob metadata, check batch existence, and list delegations without transferring payload data
**Depends on**: Phase 65
**Requirements**: QUERY-10, QUERY-11, QUERY-12
**Success Criteria** (what must be TRUE):
  1. Client can fetch blob metadata (size, timestamp, TTL, signer pubkey) for a specific blob without transferring the payload data
  2. Client can check existence of multiple blob hashes (up to 256) in a single BatchExistsRequest, receiving a per-hash boolean result
  3. Client can list all active delegations for a namespace, receiving delegate pubkeys and delegation blob hashes
  4. All three new request/response types pass through relay message filter and work over both TCP and UDS paths
**Plans**: 2 plans

Plans:
- [x] 66-01-PLAN.md — Schema types 47-52, Storage::list_delegations(), relay filter update (32 types)
- [x] 66-02-PLAN.md — MetadataRequest, BatchExistsRequest, DelegationListRequest handlers with integration tests

### Phase 67: Batch/Range Queries & Integration
**Goal**: Clients can batch-fetch blobs, query peers, query by time range, and all v1.4.0 types are fully integrated across relay, NodeInfo, and documentation
**Depends on**: Phase 66
**Requirements**: QUERY-09, QUERY-13, QUERY-14, INTEG-01, INTEG-02, INTEG-03, INTEG-04
**Success Criteria** (what must be TRUE):
  1. Client can fetch multiple blobs in a single BatchReadRequest with cumulative size cap and partial-result flag when the cap is reached
  2. Client can query peer connection information via PeerInfoRequest with trust-gated response (full detail for trusted/UDS, reduced for untrusted)
  3. Client can query blobs in a namespace within a timestamp range via TimeRangeRequest with a result limit
  4. NodeInfoResponse supported_types includes all new v1.4.0 message types (types 41-58)
  5. PROTOCOL.md documents wire format for all 10 new request/response pairs added in this milestone
**Plans**: 3 plans

Plans:
- [ ] 67-01-PLAN.md — Schema types 53-58, relay filter update (38 types), NodeInfoResponse supported[] update
- [ ] 67-02-PLAN.md — BatchReadRequest, PeerInfoRequest, TimeRangeRequest handlers with integration tests
- [ ] 67-03-PLAN.md — PROTOCOL.md v1.4.0 documentation, requirements completion

## Progress

**Execution Order:** 65 -> 66 -> 67

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 65. Node-Level Queries | 2/2 | Complete    | 2026-03-26 |
| 66. Blob-Level Queries | 2/2 | Complete    | 2026-03-26 |
| 67. Batch/Range Queries & Integration | 0/3 | Not started | - |
