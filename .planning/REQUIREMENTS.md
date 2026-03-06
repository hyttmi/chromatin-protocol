# Requirements: chromatindb

**Defined:** 2026-03-05
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v2.0 Requirements

Requirements for v2.0 Closed Node Model. Each maps to roadmap phases.

### Source Restructure

- [x] **STRUCT-01**: Source files moved to `/db` directory layout with updated CMakeLists.txt
- [ ] **STRUCT-02**: C++ namespace renamed from `chromatin::` to `chromatindb::` across all source, headers, and FlatBuffers schemas
- [ ] **STRUCT-03**: All 155 existing tests pass after restructure with clean build

### Access Control

- [ ] **ACL-01**: Node operator can specify `allowed_keys` in config JSON to restrict which pubkeys can connect (non-empty = closed mode, empty = open mode)
- [ ] **ACL-02**: Unauthorized peers are rejected at connection level after handshake, before entering PeerManager state
- [ ] **ACL-03**: PEX is disabled when node is in closed mode (non-empty `allowed_keys`)
- [ ] **ACL-04**: Node operator can hot-reload `allowed_keys` via SIGHUP without restarting the daemon
- [ ] **ACL-05**: Peers whose pubkey is revoked from `allowed_keys` are disconnected on config reload

### Larger Blob Support

- [ ] **BLOB-01**: Node enforces `MAX_BLOB_DATA_SIZE` (100 MiB) as Step 0 in ingest, before signature verification
- [ ] **BLOB-02**: Transport frame size supports 100 MiB blobs plus protocol overhead
- [ ] **BLOB-03**: Sync hash collection reads hashes from seq_map index without loading blob data
- [ ] **BLOB-04**: Sync transfers blobs individually with batched requests capped by `MAX_HASHES_PER_REQUEST`
- [ ] **BLOB-05**: Transport validates declared frame length against max before allocating receive buffer
- [ ] **BLOB-06**: Sync timeout adapts to transfer size to prevent timeout on large blob exchanges

## Future Requirements

### Layer 2 (Relay)

- **RELAY-01**: Application semantics (messages, profiles, nicknames) built on top of chromatindb
- **RELAY-02**: Relay owns a namespace, writes app data as blobs
- **RELAY-03**: Profile blobs use TTL=0 (permanent), messages use 7-day TTL

### Layer 3 (Client)

- **CLIENT-01**: Mobile/desktop app with SQLite cache
- **CLIENT-02**: Talks to relay via WebSocket

## Out of Scope

| Feature | Reason |
|---------|--------|
| Per-peer read/write restrictions | Adds config complexity; YAGNI for initial closed model |
| Chunked/streaming blob transfer | Only necessary at 1+ GiB; ML-DSA-87 requires full data for signing |
| Namespace-level ACLs | Belongs in Layer 2 (Relay), not the database node |
| inotify-based config watching | SIGHUP is the correct Unix daemon convention |
| Separate `closed_mode` toggle | Implicit from non-empty `allowed_keys`; one field, zero ambiguity |
| DHT or gossip protocol | Proven unreliable in previous projects |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| STRUCT-01 | Phase 9 | Complete |
| STRUCT-02 | Phase 9 | Pending |
| STRUCT-03 | Phase 9 | Pending |
| ACL-01 | Phase 10 | Pending |
| ACL-02 | Phase 10 | Pending |
| ACL-03 | Phase 10 | Pending |
| ACL-04 | Phase 10 | Pending |
| ACL-05 | Phase 10 | Pending |
| BLOB-01 | Phase 11 | Pending |
| BLOB-02 | Phase 11 | Pending |
| BLOB-03 | Phase 11 | Pending |
| BLOB-04 | Phase 11 | Pending |
| BLOB-05 | Phase 11 | Pending |
| BLOB-06 | Phase 11 | Pending |

**Coverage:**
- v2.0 requirements: 14 total
- Mapped to phases: 14/14
- Unmapped: 0

---
*Requirements defined: 2026-03-05*
*Last updated: 2026-03-05 after roadmap creation*
