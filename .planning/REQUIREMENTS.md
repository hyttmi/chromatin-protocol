# Requirements: chromatindb

**Defined:** 2026-03-07
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

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

- [ ] **DEL-01**: Namespace owner can delete a blob by creating a signed tombstone targeting a specific blob hash
- [ ] **DEL-02**: Tombstones are permanent (TTL=0) and replicate like regular blobs
- [ ] **DEL-03**: Nodes receiving a tombstone delete the target blob and retain the tombstone
- [ ] **DEL-04**: Tombstones propagate via sync protocol (included in hash-list diff)

## Future Requirements

### Layer 2 Relay

- **RELAY-01**: Application semantics layer (messages, profiles, nicknames)
- **RELAY-02**: Relay owns a namespace, writes app data as blobs
- **RELAY-03**: Client ↔ relay communication via WebSocket

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
| SUB-01 | — | Pending |
| SUB-02 | — | Pending |
| SUB-03 | — | Pending |
| SUB-04 | — | Pending |
| SUB-05 | — | Pending |
| DELEG-01 | — | Pending |
| DELEG-02 | — | Pending |
| DELEG-03 | — | Pending |
| DELEG-04 | — | Pending |
| DEL-01 | — | Pending |
| DEL-02 | — | Pending |
| DEL-03 | — | Pending |
| DEL-04 | — | Pending |

**Coverage:**
- v3.0 requirements: 13 total
- Mapped to phases: 0
- Unmapped: 13 ⚠️

---
*Requirements defined: 2026-03-07*
*Last updated: 2026-03-07 after initial definition*
