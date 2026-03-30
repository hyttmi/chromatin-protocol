# Requirements: chromatindb Python SDK

**Defined:** 2026-03-29
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v1.6.0 Requirements

Requirements for Python SDK milestone. Each maps to roadmap phases.

### Transport

- [x] **XPORT-01**: SDK generates and persists ML-DSA-87 client identity keypairs
- [x] **XPORT-02**: SDK performs ML-KEM-1024 key exchange with relay (PQ handshake initiator)
- [x] **XPORT-03**: SDK performs mutual ML-DSA-87 authentication during handshake
- [x] **XPORT-04**: SDK encrypts/decrypts all post-handshake frames with ChaCha20-Poly1305 AEAD
- [x] **XPORT-05**: SDK maintains correct per-direction AEAD nonce counters
- [x] **XPORT-06**: SDK encodes/decodes FlatBuffers TransportMessage wire format
- [x] **XPORT-07**: SDK supports connection lifecycle (connect, disconnect via Goodbye, context manager)

### Data Operations

- [x] **DATA-01**: SDK writes signed blobs (build canonical signing input, ML-DSA-87 sign, send Data message)
- [x] **DATA-02**: SDK reads blobs by namespace + hash (ReadRequest/ReadResponse)
- [x] **DATA-03**: SDK deletes blobs by owner via tombstone (Delete/DeleteAck)
- [x] **DATA-04**: SDK lists blobs in a namespace with pagination (ListRequest/ListResponse)
- [x] **DATA-05**: SDK checks blob existence without data transfer (ExistsRequest/ExistsResponse)
- [ ] **DATA-06**: SDK sends keepalive (Ping/Pong)

### Extended Queries

- [ ] **QUERY-01**: SDK queries blob metadata without payload (MetadataRequest/MetadataResponse)
- [ ] **QUERY-02**: SDK batch-checks blob existence (BatchExistsRequest/BatchExistsResponse)
- [ ] **QUERY-03**: SDK batch-reads multiple blobs (BatchReadRequest/BatchReadResponse)
- [ ] **QUERY-04**: SDK queries blobs by time range (TimeRangeRequest/TimeRangeResponse)
- [ ] **QUERY-05**: SDK lists namespaces (NamespaceListRequest/NamespaceListResponse)
- [ ] **QUERY-06**: SDK queries per-namespace stats (NamespaceStatsRequest/NamespaceStatsResponse)
- [ ] **QUERY-07**: SDK queries storage status (StorageStatusRequest/StorageStatusResponse)
- [ ] **QUERY-08**: SDK queries node info and capabilities (NodeInfoRequest/NodeInfoResponse)
- [ ] **QUERY-09**: SDK queries peer info (PeerInfoRequest/PeerInfoResponse)
- [ ] **QUERY-10**: SDK lists delegations (DelegationListRequest/DelegationListResponse)

### Pub/Sub

- [ ] **PUBSUB-01**: SDK subscribes to namespace notifications (Subscribe)
- [ ] **PUBSUB-02**: SDK unsubscribes from namespace notifications (Unsubscribe)
- [ ] **PUBSUB-03**: SDK receives and dispatches notification callbacks (Notification)

### Packaging

- [x] **PKG-01**: SDK is pip-installable (pyproject.toml, sdk/python/ layout)
- [ ] **PKG-02**: SDK includes getting started tutorial with usage examples
- [x] **PKG-03**: SDK has typed exception hierarchy mapping protocol errors

### Documentation

- [ ] **DOCS-01**: README and PROTOCOL.md updated with SDK section and HKDF discrepancy fixes

## Future Requirements

### Other Language SDKs

- **SDK-C-01**: C SDK under sdk/c/
- **SDK-CPP-01**: C++ SDK under sdk/c++/
- **SDK-RUST-01**: Rust SDK under sdk/rust/
- **SDK-JS-01**: JavaScript SDK under sdk/js/

### Tools

- **CLI-01**: CLI tool for admin operations built on top of Python SDK

## Out of Scope

| Feature | Reason |
|---------|--------|
| Connection pooling | request_id multiplexing handles concurrency over single connection |
| Async-only API | Sync wrapper needed for simple scripts and testing |
| C extensions | liboqs-python provides PQ crypto without custom native code |
| Auto-reconnect | v1 SDK — user handles reconnection |
| Thread safety | Single-connection model, user manages concurrency |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| XPORT-01 | Phase 70 | Complete |
| XPORT-02 | Phase 71 | Complete |
| XPORT-03 | Phase 71 | Complete |
| XPORT-04 | Phase 71 | Complete |
| XPORT-05 | Phase 71 | Complete |
| XPORT-06 | Phase 70 | Complete |
| XPORT-07 | Phase 71 | Complete |
| DATA-01 | Phase 72 | Complete |
| DATA-02 | Phase 72 | Complete |
| DATA-03 | Phase 72 | Complete |
| DATA-04 | Phase 72 | Complete |
| DATA-05 | Phase 72 | Complete |
| DATA-06 | Phase 72 | Pending |
| QUERY-01 | Phase 73 | Pending |
| QUERY-02 | Phase 73 | Pending |
| QUERY-03 | Phase 73 | Pending |
| QUERY-04 | Phase 73 | Pending |
| QUERY-05 | Phase 73 | Pending |
| QUERY-06 | Phase 73 | Pending |
| QUERY-07 | Phase 73 | Pending |
| QUERY-08 | Phase 73 | Pending |
| QUERY-09 | Phase 73 | Pending |
| QUERY-10 | Phase 73 | Pending |
| PUBSUB-01 | Phase 73 | Pending |
| PUBSUB-02 | Phase 73 | Pending |
| PUBSUB-03 | Phase 73 | Pending |
| PKG-01 | Phase 70 | Complete |
| PKG-02 | Phase 74 | Pending |
| PKG-03 | Phase 70 | Complete |
| DOCS-01 | Phase 74 | Pending |

**Coverage:**
- v1.6.0 requirements: 30 total
- Mapped to phases: 30
- Unmapped: 0

---
*Requirements defined: 2026-03-29*
*Last updated: 2026-03-29 after roadmap creation*
