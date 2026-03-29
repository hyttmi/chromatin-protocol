# Requirements: chromatindb Python SDK

**Defined:** 2026-03-29
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v1.6.0 Requirements

Requirements for Python SDK milestone. Each maps to roadmap phases.

### Transport

- [ ] **XPORT-01**: SDK generates and persists ML-DSA-87 client identity keypairs
- [ ] **XPORT-02**: SDK performs ML-KEM-1024 key exchange with relay (PQ handshake initiator)
- [ ] **XPORT-03**: SDK performs mutual ML-DSA-87 authentication during handshake
- [ ] **XPORT-04**: SDK encrypts/decrypts all post-handshake frames with ChaCha20-Poly1305 AEAD
- [ ] **XPORT-05**: SDK maintains correct per-direction AEAD nonce counters
- [ ] **XPORT-06**: SDK encodes/decodes FlatBuffers TransportMessage wire format
- [ ] **XPORT-07**: SDK supports connection lifecycle (connect, disconnect via Goodbye, context manager)

### Data Operations

- [ ] **DATA-01**: SDK writes signed blobs (build canonical signing input, ML-DSA-87 sign, send Data message)
- [ ] **DATA-02**: SDK reads blobs by namespace + hash (ReadRequest/ReadResponse)
- [ ] **DATA-03**: SDK deletes blobs by owner via tombstone (Delete/DeleteAck)
- [ ] **DATA-04**: SDK lists blobs in a namespace with pagination (ListRequest/ListResponse)
- [ ] **DATA-05**: SDK checks blob existence without data transfer (ExistsRequest/ExistsResponse)
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

- [ ] **PKG-01**: SDK is pip-installable (pyproject.toml, sdk/python/ layout)
- [ ] **PKG-02**: SDK includes getting started tutorial with usage examples
- [ ] **PKG-03**: SDK has typed exception hierarchy mapping protocol errors

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
| XPORT-01 | - | Pending |
| XPORT-02 | - | Pending |
| XPORT-03 | - | Pending |
| XPORT-04 | - | Pending |
| XPORT-05 | - | Pending |
| XPORT-06 | - | Pending |
| XPORT-07 | - | Pending |
| DATA-01 | - | Pending |
| DATA-02 | - | Pending |
| DATA-03 | - | Pending |
| DATA-04 | - | Pending |
| DATA-05 | - | Pending |
| DATA-06 | - | Pending |
| QUERY-01 | - | Pending |
| QUERY-02 | - | Pending |
| QUERY-03 | - | Pending |
| QUERY-04 | - | Pending |
| QUERY-05 | - | Pending |
| QUERY-06 | - | Pending |
| QUERY-07 | - | Pending |
| QUERY-08 | - | Pending |
| QUERY-09 | - | Pending |
| QUERY-10 | - | Pending |
| PUBSUB-01 | - | Pending |
| PUBSUB-02 | - | Pending |
| PUBSUB-03 | - | Pending |
| PKG-01 | - | Pending |
| PKG-02 | - | Pending |
| PKG-03 | - | Pending |
| DOCS-01 | - | Pending |

**Coverage:**
- v1.6.0 requirements: 30 total
- Mapped to phases: 0
- Unmapped: 30

---
*Requirements defined: 2026-03-29*
*Last updated: 2026-03-29 after initial definition*
