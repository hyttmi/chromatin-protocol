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
- 🚧 **v1.3.0 Protocol Concurrency & Query Foundation** — Phases 61-64 (in progress)

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

- [x] Phase 57: Client Protocol Extensions (2/2 plans) — completed 2026-03-23
- [x] Phase 58: Relay Scaffolding & Identity (2/2 plans) — completed 2026-03-23
- [x] Phase 59: Relay Core (2/2 plans) — completed 2026-03-23
- [x] Phase 60: Codebase Deduplication Audit (2/2 plans) — completed 2026-03-23

Full details: [milestones/v1.2.0-ROADMAP.md](milestones/v1.2.0-ROADMAP.md)

</details>

### v1.3.0 Protocol Concurrency & Query Foundation (In Progress)

**Milestone Goal:** Clients can send concurrent requests with correlation IDs, check blob existence without data transfer, and discover node capabilities -- establishing the concurrent dispatch foundation for all future query types.

- [ ] **Phase 61: Transport Foundation** - request_id in wire format, codec pipeline, relay forwarding
- [x] **Phase 62: Concurrent Dispatch** - Thread pool offload for heavy ops, inline for cheap ops (completed 2026-03-25)
- [x] **Phase 63: Query Extensions** - ExistsRequest/ExistsResponse, NodeInfoRequest/NodeInfoResponse (completed 2026-03-25)
- [ ] **Phase 64: Documentation** - PROTOCOL.md, README.md, db/README.md updated for v1.3.0

## Phase Details

### Phase 61: Transport Foundation
**Goal**: Every request/response message carries a client-assigned correlation ID through the full encode/decode/dispatch/relay pipeline
**Depends on**: Phase 60 (v1.2.0 complete)
**Requirements**: CONC-01, CONC-02, CONC-05
**Success Criteria** (what must be TRUE):
  1. A client can set a request_id on any request message and receive the same request_id echoed on the corresponding response
  2. The relay forwards request_id bidirectionally without modification -- a client connected through the relay sees the same correlation behavior as a direct connection
  3. Non-request/response operations (sync, PEX, pub/sub, lifecycle) function unchanged with request_id defaulting to 0
  4. All existing tests pass with the updated MessageCallback and send_message signatures
**Plans**: 3 plans
- [ ] 61-01-PLAN.md — Schema and Codec Foundation (Wave 1)
- [ ] 61-02-PLAN.md — Pipeline Plumbing & Relay Forwarding (Wave 2)
- [ ] 61-03-PLAN.md — PeerManager Dispatch & Echoing (Wave 3)

### Phase 62: Concurrent Dispatch
**Goal**: Multiple in-flight client requests execute concurrently without blocking each other, while maintaining AEAD nonce safety on the IO thread
**Depends on**: Phase 61
**Requirements**: CONC-03, CONC-04
**Success Criteria** (what must be TRUE):
  1. A client can send multiple ReadRequest/ListRequest/StatsRequest messages without waiting for responses, and each response arrives with the correct request_id regardless of completion order
  2. Cheap operations (Ping, Pong, Goodbye, Subscribe, Unsubscribe) execute inline on the IO thread without thread pool dispatch overhead
  3. All send_message calls happen on the IO thread (AEAD nonce safety verified -- no TSAN findings)
**Plans**: 1 plan
Plans:
- [x] 62-01-PLAN.md — IO-thread safety fix and concurrent dispatch verification

### Phase 63: Query Extensions
**Goal**: Clients can check blob existence without data transfer and discover node capabilities, version, and storage state
**Depends on**: Phase 62
**Requirements**: QUERY-01, QUERY-02, QUERY-03, QUERY-04
**Success Criteria** (what must be TRUE):
  1. A client can send ExistsRequest with namespace + blob hash and receive ExistsResponse with a boolean result, without any blob data being transferred
  2. A client can send NodeInfoRequest and receive NodeInfoResponse containing version, git hash, uptime, peer count, namespace count, total blobs, storage bytes, and a list of supported message types
  3. The relay allows ExistsRequest/ExistsResponse and NodeInfoRequest/NodeInfoResponse through its message filter
  4. Storage exposes a key-existence check (has_blob) that does not read the blob value
**Plans**: 2 plans
Plans:
- [x] 63-01-PLAN.md — ExistsRequest/ExistsResponse handler, relay filter, tests (Wave 1)
- [x] 63-02-PLAN.md — NodeInfoRequest/NodeInfoResponse handler, relay filter, tests (Wave 2)

### Phase 64: Documentation
**Goal**: Protocol documentation and READMEs reflect the full v1.3.0 concurrency model, new message types, and request_id semantics
**Depends on**: Phase 63
**Requirements**: DOCS-01, DOCS-02, DOCS-03
**Success Criteria** (what must be TRUE):
  1. db/PROTOCOL.md documents request_id semantics (client-assigned, node-echoed, per-connection scope), the concurrent dispatch model (inline vs offloaded categories), and both new message type pairs (ExistsRequest/ExistsResponse, NodeInfoRequest/NodeInfoResponse)
  2. README.md describes v1.3.0 capabilities (concurrent requests, blob existence check, node info query)
  3. db/README.md documents the concurrent dispatch model, new message types, and request_id field
**Plans**: 2 plans
Plans:
- [ ] 64-01-PLAN.md — PROTOCOL.md: request_id semantics, ExistsRequest/ExistsResponse, NodeInfoRequest/NodeInfoResponse wire format
- [ ] 64-02-PLAN.md — README.md version bump, db/README.md features, dispatch model, counts

## Progress

**Execution Order:** 61 -> 62 -> 63 -> 64

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 61. Transport Foundation | 3/3 | Complete | 2026-03-25 |
| 62. Concurrent Dispatch | 1/1 | Complete    | 2026-03-25 |
| 63. Query Extensions | 2/2 | Complete    | 2026-03-25 |
| 64. Documentation | 0/2 | Not started | - |
