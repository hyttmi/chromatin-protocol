# Requirements: chromatindb

**Defined:** 2026-03-07
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v3.0 Requirements

Requirements for v3.0 Real-time & Delegation. Each maps to roadmap phases.

### Pub/Sub Notifications

- [ ] **SUB-01**: Peer can subscribe to one or more namespaces via SUBSCRIBE message
- [ ] **SUB-02**: Node pushes NOTIFICATION (namespace + seq_num + hash + size) when blob ingested into subscribed namespace
- [ ] **SUB-03**: Peer can unsubscribe from namespaces
- [ ] **SUB-04**: Subscriptions are connection-scoped (no persistence across disconnects)
- [ ] **SUB-05**: Subscribers receive notification when a blob is deleted (tombstoned) in a subscribed namespace

### Namespace Delegation

- [ ] **DELEG-01**: Namespace owner can create a signed delegation blob granting write access to another pubkey
- [ ] **DELEG-02**: Node accepts writes from delegates after verifying a valid delegation blob exists in the namespace
- [ ] **DELEG-03**: Delegation blobs replicate like any blob via sync protocol
- [ ] **DELEG-04**: Owner can revoke delegation by deleting the delegation blob (tombstone)

### Blob Deletion

- [x] **DEL-01**: Namespace owner can delete a blob by creating a signed tombstone targeting a specific blob hash
- [x] **DEL-02**: Tombstones are permanent (TTL=0) and replicate like regular blobs
- [x] **DEL-03**: Nodes receiving a tombstone delete the target blob and retain the tombstone
- [x] **DEL-04**: Tombstones propagate via sync protocol (included in hash-list diff)

### Build & Tooling

- [x] **BUILD-01**: Strip unused algorithms from liboqs build (only ML-DSA-87, ML-KEM-1024, SHA3-256)

### Documentation & Performance

- [ ] **DOCS-01**: README.md with usage guide for interacting with running chromatindb nodes
- [ ] **PERF-01**: Performance test suite with benchmark numbers

## Future Requirements

### Layer 2 Relay

- **RELAY-01**: Application semantics layer (messages, profiles, nicknames)
- **RELAY-02**: Relay owns a namespace, writes app data as blobs
- **RELAY-03**: Client <-> relay communication via WebSocket

### Layer 3 Client

- **CLIENT-01**: Mobile/desktop app with SQLite cache
- **CLIENT-02**: Talks to relay via WebSocket

## Out of Scope

| Feature | Reason |
|---------|--------|
| Per-namespace subscription restrictions | ACL is the gate; per-namespace logic is relay/app layer |
| Delegate delete permissions | Deletion is owner-privileged; delegates write only |
| Persistent subscriptions | Connection-scoped is sufficient; relay can re-subscribe on reconnect |
| Delegation listing query | Delegation blobs are regular blobs; filter at app layer |
| Configurable per-delegation permissions | Write-only is sufficient for relay use case; YAGNI |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| DEL-01 | Phase 12 | Complete |
| DEL-02 | Phase 12 | Complete |
| DEL-03 | Phase 12 | Complete |
| DEL-04 | Phase 12 | Complete |
| DELEG-01 | Phase 13 | Pending |
| DELEG-02 | Phase 13 | Pending |
| DELEG-03 | Phase 13 | Pending |
| DELEG-04 | Phase 13 | Pending |
| SUB-01 | Phase 14 | Pending |
| SUB-02 | Phase 14 | Pending |
| SUB-03 | Phase 14 | Pending |
| SUB-04 | Phase 14 | Pending |
| SUB-05 | Phase 14 | Pending |
| BUILD-01 | Phase 12 | Complete |
| DOCS-01 | Phase 15 | Pending |
| PERF-01 | Phase 15 | Pending |

**Coverage:**
- v3.0 requirements: 16 total
- Mapped to phases: 16
- Unmapped: 0

---
*Requirements defined: 2026-03-07*
*Last updated: 2026-03-07 after roadmap revision*
