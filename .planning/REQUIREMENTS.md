# Requirements: chromatindb v2.1.0 Compression, Filtering & Observability

**Defined:** 2026-04-05
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v2.1.0 Requirements

Requirements for wire compression, notification filtering, relay/SDK resilience, operational improvements, and documentation refresh. Each maps to roadmap phases.

### Compression

- [ ] **COMP-01**: Node compresses sync/data message payloads with Brotli before AEAD encryption (flag byte per message)
- [ ] **COMP-02**: Node skips compression for payloads under 256 bytes or already-encrypted envelope data
- [ ] **COMP-03**: Node enforces decompressed output cap at MAX_BLOB_DATA_SIZE to prevent decompression bombs
- [ ] **COMP-04**: SDK compresses outbound and decompresses inbound payloads matching node behavior

### Notification Filtering

- [x] **FILT-01**: Peers exchange sync_namespaces after handshake via SyncNamespaceAnnounce message
- [x] **FILT-02**: Node only sends BlobNotify to peers whose announced namespaces include the blob's namespace
- [ ] **FILT-03**: Relay tracks per-client subscription namespaces and only forwards matching Notification messages

### Relay Resilience

- [ ] **RELAY-01**: Relay auto-reconnects to node UDS with jittered backoff when connection is lost
- [ ] **RELAY-02**: Relay replays client subscriptions to node after successful UDS reconnect

### SDK Resilience

- [ ] **SDK-01**: SDK connect() accepts a list of relay addresses and rotates to next on connection failure
- [ ] **SDK-02**: SDK auto-reconnect tries next relay in the list when current relay is unreachable

### Operational

- [ ] **OPS-01**: max_peers is reloadable via SIGHUP without node restart
- [ ] **OPS-02**: Node exposes Prometheus-compatible HTTP /metrics endpoint (localhost-only default, opt-in via config)
- [ ] **OPS-03**: /metrics endpoint exposes all existing metrics (peers, blobs, sync, storage, connections)

### Documentation

- [ ] **DOC-01**: PROTOCOL.md updated with compression frame format, SyncNamespaceAnnounce, and /metrics
- [ ] **DOC-02**: README.md updated with compression, filtering, and observability features
- [ ] **DOC-03**: SDK README updated with multi-relay failover API and Brotli support
- [ ] **DOC-04**: Getting-started tutorial updated with metrics and relay resilience

## Future Requirements

Deferred to future release. Tracked but not in current roadmap.

- Brotli quality level tunable via config (currently hardcoded)
- Compression negotiation in handshake (capability bit)
- Relay connection pooling to multiple nodes
- /metrics authentication (API key or mTLS)

## Out of Scope

- Compression of AEAD-encrypted envelope blobs (ciphertext is incompressible by design)
- prometheus-cpp library (conflicts with single-threaded Asio model)
- allowed_client_keys / allowed_peer_keys hot reload (already implemented)

## Traceability

| Req ID | Phase | Status |
|--------|-------|--------|
| COMP-01 | Phase 87 | Pending |
| COMP-02 | Phase 87 | Pending |
| COMP-03 | Phase 87 | Pending |
| COMP-04 | Phase 87 | Pending |
| FILT-01 | Phase 86 | Complete |
| FILT-02 | Phase 86 | Complete |
| FILT-03 | Phase 88 | Pending |
| RELAY-01 | Phase 88 | Pending |
| RELAY-02 | Phase 88 | Pending |
| SDK-01 | Phase 89 | Pending |
| SDK-02 | Phase 89 | Pending |
| OPS-01 | Phase 86 | Pending |
| OPS-02 | Phase 90 | Pending |
| OPS-03 | Phase 90 | Pending |
| DOC-01 | Phase 90 | Pending |
| DOC-02 | Phase 90 | Pending |
| DOC-03 | Phase 90 | Pending |
| DOC-04 | Phase 90 | Pending |

**Coverage:**
- v2.1.0 requirements: 18 total
- Mapped to phases: 18/18
- Unmapped: 0

---
