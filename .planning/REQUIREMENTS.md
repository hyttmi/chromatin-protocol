# Requirements: chromatindb

**Defined:** 2026-03-24
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v1.3.0 Requirements

Requirements for v1.3.0 Protocol Concurrency & Query Foundation. Each maps to roadmap phases.

### Transport Concurrency

- [ ] **CONC-01**: Transport envelope includes a `request_id: uint32` field that clients set on requests and the node echoes on corresponding responses
- [ ] **CONC-02**: `DecodedMessage`, `TransportCodec`, `Connection::send_message`, and `MessageCallback` all carry `request_id` through the full encode/decode/dispatch pipeline
- [x] **CONC-03**: Request/response handlers for Read, List, and Stats are dispatched to the thread pool via the existing offload pattern, with responses sent back on the IO thread (AEAD nonce safety)
- [x] **CONC-04**: Cheap operations (Ping, Pong, Goodbye, Subscribe, Unsubscribe, ExistsRequest, NodeInfoRequest) execute inline on the IO thread without offload overhead
- [ ] **CONC-05**: Relay forwards `request_id` bidirectionally in both `handle_client_message` and `handle_node_message` paths

### Query Extensions

- [x] **QUERY-01**: Client can send ExistsRequest (type 37) with namespace + blob hash and receive ExistsResponse (type 38) with a boolean existence result and echoed blob hash
- [x] **QUERY-02**: `Storage` exposes a `has_blob()` key-existence check that does not read the blob value
- [ ] **QUERY-03**: Client can send NodeInfoRequest (type 39) and receive NodeInfoResponse (type 40) with version, git hash, uptime, peer count, namespace count, total blobs, storage bytes used/max, and list of supported message types
- [x] **QUERY-04**: Relay message filter allows ExistsRequest (37), ExistsResponse (38), NodeInfoRequest (39), NodeInfoResponse (40) through to the node

### Documentation

- [ ] **DOCS-01**: `db/PROTOCOL.md` documents request_id semantics, concurrent dispatch model, ExistsRequest/ExistsResponse, and NodeInfoRequest/NodeInfoResponse
- [ ] **DOCS-02**: `README.md` updated with v1.3.0 protocol capabilities
- [ ] **DOCS-03**: `db/README.md` updated with v1.3.0 changes (concurrent dispatch, new message types, request_id)

## Future Requirements

Deferred to subsequent milestone (v1.4.0+).

### Extended Query Types
- **QUERY-05**: TimeRangeRequest/TimeRangeResponse -- blobs in namespace between timestamp A and B
- **QUERY-06**: MetadataRequest/MetadataResponse -- blob metadata (size, timestamp, TTL, pubkey) without data
- **QUERY-07**: NamespaceListRequest/NamespaceListResponse -- client-facing namespace enumeration with counts
- **QUERY-08**: DelegationListRequest/DelegationListResponse -- delegates with write access to a namespace

### Operator Queries
- **OPS-01**: PeerInfoRequest/PeerInfoResponse -- connected peers, addresses, sync status
- **OPS-02**: HealthRequest/HealthResponse -- node health check (storage, peers, sync)

### Client SDK
- **SDK-01**: Python SDK for connecting to relay
- **SDK-02**: CLI tool for admin operations

## Out of Scope

| Feature | Reason |
|---------|--------|
| Full worker pool model | Current offload pattern sufficient; can evolve later |
| Connection pooling in protocol | SDK concern, not protocol |
| Batch message types (BatchExists, BatchRead) | request_id + pipelining achieves same goal without dedicated types |
| Thread pool sizing config | Uses existing asio::thread_pool; tuning is operational, not protocol |
| Backward compatibility shims | Node not deployed anywhere; clean breaking changes allowed |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| CONC-01 | Phase 61 | Pending |
| CONC-02 | Phase 61 | Pending |
| CONC-03 | Phase 62 | Complete |
| CONC-04 | Phase 62 | Complete |
| CONC-05 | Phase 61 | Pending |
| QUERY-01 | Phase 63 | Complete |
| QUERY-02 | Phase 63 | Complete |
| QUERY-03 | Phase 63 | Pending |
| QUERY-04 | Phase 63 | Complete |
| DOCS-01 | Phase 64 | Pending |
| DOCS-02 | Phase 64 | Pending |
| DOCS-03 | Phase 64 | Pending |

**Coverage:**
- v1.3.0 requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-03-24*
*Last updated: 2026-03-24 after roadmap creation*
