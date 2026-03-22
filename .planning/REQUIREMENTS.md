# Requirements: chromatindb

**Defined:** 2026-03-22
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v1.2.0 Requirements

Requirements for v1.2.0 Relay & Client Protocol. Each maps to roadmap phases.

### Protocol Extensions

- [ ] **PROTO-01**: Node sends WriteAck (type 25) back to client after successful blob ingest, containing blob hash and seq_num
- [ ] **PROTO-02**: Client can fetch a specific blob by namespace + hash via ReadRequest (type 26) / ReadResponse (type 27)
- [ ] **PROTO-03**: Client can list blobs in a namespace with pagination via ListRequest (type 28) / ListResponse (type 29), using since_seq cursor + limit
- [ ] **PROTO-04**: Client can query namespace usage (blob count, total bytes, quota remaining) via StatsRequest (type 30) / StatsResponse (type 31)

### Relay

- [ ] **RELAY-01**: Relay accepts TCP connections and performs PQ handshake as responder (ML-KEM-1024 + ML-DSA-87), using its own identity keypair
- [ ] **RELAY-02**: Relay connects to chromatindb node via UDS with TrustedHello, one UDS connection per client session
- [ ] **RELAY-03**: Relay filters messages by type -- allows client operations (Data, WriteAck, Delete, DeleteAck, ReadRequest, ReadResponse, ListRequest, ListResponse, StatsRequest, StatsResponse, Subscribe, Unsubscribe, Notification, Ping, Pong, Goodbye), blocks peer operations (Sync*, Reconcile*, PeerList*, NamespaceList, BlobRequest, BlobTransfer, StorageFull, TrustedHello), default-deny on unknown types
- [ ] **RELAY-04**: Relay forwards allowed messages bidirectionally -- client<->node -- without parsing payloads (type field only)
- [ ] **RELAY-05**: Relay has its own ML-DSA-87 identity keypair, generated on first run or loaded from configured path
- [ ] **RELAY-06**: Relay config via JSON file (bind_address, bind_port, uds_path, identity_key_path, log_level, log_file)
- [ ] **RELAY-07**: Relay lives in `relay/` directory with own CMakeLists.txt, links chromatindb_lib, zero new dependencies
- [ ] **RELAY-08**: Relay binary `chromatindb_relay` builds alongside `chromatindb` from root CMakeLists.txt

## Previous Milestone (v1.1.0 -- Complete)

### Compaction
- [x] **COMP-01**: Node operator can trigger runtime mdbx compaction automatically for long-running nodes

### Local Access
- [x] **UDS-01**: Local process can read/write blobs via Unix Domain Socket without TCP+PQ overhead

### Operational Hardening
- [x] **OPS-01**: Node operator can configure expiry scan interval via config field (replacing hardcoded 60s)
- [x] **OPS-02**: Node rejects blobs with timestamps too far in the future or past on ingest
- [x] **OPS-03**: SyncRejected messages include human-readable reason strings for operator debugging

### Release & Cleanup
- [x] **REL-01**: Git repository has a v1.0.0 release tag on the shipped commit
- [x] **REL-02**: Stale bash tests (deploy/test-crash-recovery.sh) and design docs (db/TESTS.md) removed
- [x] **REL-03**: Stale .planning/milestones/v1.0.0-* deferred docs cleaned up
- [x] **REL-04**: CMake project version bumped to 1.1.0

### Documentation
- [x] **DOCS-01**: db/README.md reflects v1.0.0 state (sanitizers, 469 tests, Docker integration, stress/chaos/fuzz)
- [x] **DOCS-02**: README.md aligned with v1.0.0 shipped state
- [x] **DOCS-03**: db/PROTOCOL.md updated with sync reject reason strings

## Future Requirements

### Client SDK (v1.3.0+)
- **SDK-01**: Python SDK for connecting to relay, writing/reading blobs
- **SDK-02**: CLI tool for admin operations

## Out of Scope

| Feature | Reason |
|---------|--------|
| HTTP/REST API | Binary protocol over PQ-encrypted channel only -- no web protocol attack surface |
| OpenSSL / TLS | Relay uses same PQ handshake as node -- no OpenSSL dependency |
| Connection pooling / multiplexing | YAGNI -- one client = one UDS connection |
| Rate limiting at relay | Node already enforces rate limits |
| Authentication at relay | Node verifies blob signatures -- relay is a pass-through |
| Relay payload inspection | Relay reads type field only, never parses message content |
| Per-blob encryption keys | Single HKDF-derived key per node is sufficient |
| Chunked/streaming blob transfer | Only necessary at 1+ GiB |
| NAT traversal | Server daemon assumes reachable address |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| PROTO-01 | Phase 57 | Pending |
| PROTO-02 | Phase 57 | Pending |
| PROTO-03 | Phase 57 | Pending |
| PROTO-04 | Phase 57 | Pending |
| RELAY-01 | Phase 59 | Pending |
| RELAY-02 | Phase 59 | Pending |
| RELAY-03 | Phase 59 | Pending |
| RELAY-04 | Phase 59 | Pending |
| RELAY-05 | Phase 58 | Pending |
| RELAY-06 | Phase 58 | Pending |
| RELAY-07 | Phase 58 | Pending |
| RELAY-08 | Phase 58 | Pending |

**Coverage:**
- v1.2.0 requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-03-22*
*Last updated: 2026-03-22 after roadmap creation*
