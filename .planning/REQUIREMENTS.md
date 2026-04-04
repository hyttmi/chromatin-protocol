# Requirements: chromatindb v2.0.0 Event-Driven Architecture

**Defined:** 2026-04-02
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v2.0.0 Requirements

Requirements for event-driven sync, maintenance overhaul, connection resilience, and documentation refresh. Each maps to roadmap phases.

### Push Sync

- [x] **PUSH-01**: Node notifies all connected peers immediately when a new blob is ingested (from client write or peer sync)
- [x] **PUSH-02**: Notification contains namespace, blob hash, sequence number, size, and tombstone flag (77-byte payload)
- [x] **PUSH-03**: Notifications are suppressed during active reconciliation to prevent notification storms
- [x] **PUSH-04**: Per-connection send queue serializes all outbound messages to prevent AEAD nonce desync
- [x] **PUSH-05**: Peer receiving a BlobNotify can fetch the specific blob via targeted BlobFetch request (skip full reconciliation)
- [x] **PUSH-06**: BlobFetch is handled inline in the message loop (no sync session handshake required)
- [x] **PUSH-07**: Node does not send BlobNotify back to the peer that originated the blob (source exclusion)
- [x] **PUSH-08**: Push notifications are delivered to currently-connected peers only; disconnected peers recover via reconcile-on-connect

### Maintenance

- [x] **MAINT-01**: Expiry uses a next-expiry timer that fires at exactly the earliest blob's expiry time (O(1) via MDBX cursor)
- [x] **MAINT-02**: After processing an expired blob, timer rearms to the next earliest expiry (chain processing)
- [x] **MAINT-03**: Expiry timer rearms when a blob with an earlier expiry is ingested
- [x] **MAINT-04**: Peer cursors are compacted immediately when a peer disconnects (with 5-minute grace period for transient disconnects)
- [x] **MAINT-05**: Full reconciliation runs on peer connect/reconnect (catch-up path)
- [x] **MAINT-06**: Safety-net reconciliation runs at a long interval (default 600s) as a monitoring signal
- [x] **MAINT-07**: sync_interval_seconds config field repurposed to safety_net_interval_seconds with 600s default

### Connections

- [x] **CONN-01**: Node sends Ping to all TCP peers every 30 seconds (bidirectional keepalive)
- [x] **CONN-02**: Peer that doesn't respond within 2 missed keepalive cycles is disconnected
- [x] **CONN-03**: SDK ChromatinClient auto-reconnects on connection loss with jittered exponential backoff (1s-30s)
- [x] **CONN-04**: SDK restores pub/sub subscriptions after successful reconnect
- [x] **CONN-05**: SDK exposes a reconnection event/callback for application-level catch-up

### Wire Protocol

- [x] **WIRE-01**: New message type BlobNotify (type 59) — peer-internal push notification
- [x] **WIRE-02**: New message type BlobFetch (type 60) — targeted blob request by hash
- [x] **WIRE-03**: New message type BlobFetchResponse (type 61) — response with blob data or not-found
- [x] **WIRE-04**: Relay message filter updated to block types 59-61 (peer-internal only)

### Documentation

- [ ] **DOC-01**: PROTOCOL.md updated with push sync protocol, new message types, keepalive spec
- [ ] **DOC-02**: README.md updated with v2.0.0 sync model description
- [ ] **DOC-03**: SDK README updated with auto-reconnect API and behavior
- [ ] **DOC-04**: Getting-started tutorial updated for new connection lifecycle

## Future Requirements

Deferred to future release. Tracked but not in current roadmap.

### Batched Notifications

- **BATCH-01**: Node can batch multiple BlobNotify messages into a single wire frame for high-ingest scenarios

### Gossip Protocol

- **GOSSIP-01**: At 50+ peers, switch from direct notification to gossip-based propagation to reduce fan-out

### Notification Gap Detection

- **GAP-01**: SDK can detect and recover from notification gaps during reconnect using last_seen_seq

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Gossip-based sync propagation | Direct notification is O(32) at max_peers=32; gossip solves 100+ node problem we don't have |
| Guaranteed delivery for push notifications | TCP + AEAD already guarantees delivery or connection death; adding app-level ack is redundant |
| Backward-compatible sync protocol | Only deployed on home KVM; no production users to break |
| Conflict resolution / CRDT | Blob store is append-only with content-addressed dedup; no conflicts possible |
| Partial blob sync / delta encoding | Blobs are opaque signed units; partial sync would break signature verification |
| New dependencies (message queues, etc.) | Existing Asio + libmdbx + FlatBuffers are sufficient for everything |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| PUSH-01 | Phase 79 | Complete |
| PUSH-02 | Phase 79 | Complete |
| PUSH-03 | Phase 79 | Complete |
| PUSH-04 | Phase 79 | Complete |
| PUSH-05 | Phase 80 | Complete |
| PUSH-06 | Phase 80 | Complete |
| PUSH-07 | Phase 79 | Complete |
| PUSH-08 | Phase 79 | Complete |
| MAINT-01 | Phase 81 | Complete |
| MAINT-02 | Phase 81 | Complete |
| MAINT-03 | Phase 81 | Complete |
| MAINT-04 | Phase 82 | Complete |
| MAINT-05 | Phase 82 | Complete |
| MAINT-06 | Phase 82 | Complete |
| MAINT-07 | Phase 82 | Complete |
| CONN-01 | Phase 83 | Complete |
| CONN-02 | Phase 83 | Complete |
| CONN-03 | Phase 84 | Complete |
| CONN-04 | Phase 84 | Complete |
| CONN-05 | Phase 84 | Complete |
| WIRE-01 | Phase 79 | Complete |
| WIRE-02 | Phase 80 | Complete |
| WIRE-03 | Phase 80 | Complete |
| WIRE-04 | Phase 79 | Complete |
| DOC-01 | Phase 85 | Pending |
| DOC-02 | Phase 85 | Pending |
| DOC-03 | Phase 85 | Pending |
| DOC-04 | Phase 85 | Pending |

**Coverage:**
- v2.0.0 requirements: 28 total
- Mapped to phases: 28/28
- Unmapped: 0

---
*Requirements defined: 2026-04-02*
*Last updated: 2026-04-02 after roadmap creation*
