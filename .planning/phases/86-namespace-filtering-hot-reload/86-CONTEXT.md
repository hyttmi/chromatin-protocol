# Phase 86: Namespace Filtering & Hot Reload - Context

**Gathered:** 2026-04-05
**Status:** Ready for planning

<domain>
## Phase Boundary

Peers only receive BlobNotify for namespaces they replicate via a new SyncNamespaceAnnounce protocol message, Phase A/B/C reconciliation is scoped to the namespace intersection, and operators can adjust max_peers via SIGHUP without restart.

</domain>

<decisions>
## Implementation Decisions

### SyncNamespaceAnnounce exchange pattern
- **D-01:** Both peers send SyncNamespaceAnnounce after successful auth exchange, before sync-on-connect. Both sides wait to receive the other's announcement before proceeding to the message loop. This guarantees the namespace intersection is known before any sync traffic flows.
- **D-02:** On SIGHUP, when sync_namespaces changes, the node re-sends SyncNamespaceAnnounce to all connected peers immediately. Existing peers update their filter in real-time.
- **D-03:** Re-announce is a passive filter update only. No re-sync triggered on re-announce. The safety-net cycle (600s) catches any gaps naturally.

### Namespace filtering scope
- **D-04:** SyncNamespaceAnnounce filters BOTH BlobNotify push notifications AND Phase A/B/C reconciliation. During reconciliation, only exchange fingerprints and blobs for namespaces in the intersection of both peers' announced sets.
- **D-05:** Intersection semantics: the effective set is the intersection of both peers' announced namespace sets. Sender doesn't waste bandwidth on namespaces the receiver doesn't replicate, and skips fingerprinting namespaces it doesn't have.
- **D-06:** BlobNotify filter by sync_namespaces (node replication config), NOT subscribed_namespaces (client state). Decided during milestone planning.

### Empty set & backward compatibility
- **D-07:** No distinction between "never announced" and "announced empty set." Both mean "replicate everything." Empty announcement = receive all BlobNotify, full reconciliation scope.
- **D-08:** All nodes upgrade together (home KVM swarm). No mixed-version backward compatibility needed. Breaking protocol change is acceptable.

### Wire format
- **D-09:** SyncNamespaceAnnounce is message type 62 in the transport FlatBuffers enum. Peer-internal, added to relay blocklist.
- **D-10:** FlatBuffers-encoded payload inside the standard transport envelope (type byte + length-prefixed FlatBuffer). Same dispatch pattern as all other messages via handle_message().
- **D-11:** FlatBuffer schema uses a vector of fixed-size structs: Namespace { hash:[uint8:32] }. Type-safe, FlatBuffers handles alignment, clean iteration.

### max_peers SIGHUP reload
- **D-12:** max_peers becomes SIGHUP-reloadable in reload_config(). When the new limit is lower than current peer count, refuse new incoming connections until count drops below the limit naturally. No active disconnection of excess peers.
- **D-13:** Log a warning when the node is over the new max_peers limit after SIGHUP.

### Claude's Discretion
- How SyncNamespaceAnnounce integrates into the on_peer_connected coroutine flow (send/recv ordering)
- PeerInfo struct additions to track per-peer announced namespaces
- How sync_protocol.cpp Phase A/B/C skips non-intersecting namespaces (likely filter in fingerprint generation)
- Relay blocklist update mechanism for type 62
- Test strategy and test structure

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Connection lifecycle & handshake
- `db/peer/peer_manager.cpp` — on_peer_connected() coroutine (handshake + sync-on-connect flow)
- `db/peer/peer_manager.h` — PeerInfo struct, PeerManager class definition
- `db/net/connection.h` — Connection class: send_message(), is_uds(), SessionKeys

### BlobNotify & notification fan-out
- `db/peer/peer_manager.cpp:3032` — on_blob_ingested(): current BlobNotify fan-out (no namespace filter yet)
- `db/peer/peer_manager.cpp:3092` — on_blob_notify(): incoming BlobNotify handler

### Sync protocol & reconciliation
- `db/sync/sync_protocol.h` — SyncProtocol class, OnBlobIngested callback
- `db/sync/sync_protocol.cpp` — Phase A/B/C reconciliation implementation

### Config & SIGHUP reload
- `db/config/config.h` — Config struct (max_peers, sync_namespaces fields)
- `db/config/config.cpp` — load_config(), validate_config()
- `db/peer/peer_manager.cpp:2745` — reload_config(): existing SIGHUP handler (does NOT reload max_peers yet)
- `db/peer/peer_manager.cpp:308` — accept_peer(): current max_peers check

### Wire format & FlatBuffers
- `db/schemas/transport.fbs` — Transport message type enum (BlobFetchResponse=61, next=62)
- `db/wire/transport_generated.h` — Generated FlatBuffers code
- `db/tests/relay/test_message_filter.cpp` — Relay blocklist test (17 peer-only types)

### Protocol documentation
- `db/PROTOCOL.md` — Wire protocol spec (BlobNotify, sync protocol, message types)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `sync_namespaces_` (std::set<array<uint8_t,32>>): Already exists on PeerManager, SIGHUP-reloadable. Can be used as source for SyncNamespaceAnnounce payload.
- `encode_notification()`: Builds 77-byte BlobNotify payload. Pattern for building SyncNamespaceAnnounce.
- `reload_config()`: Comprehensive SIGHUP handler — just needs max_peers line and re-announce call added.
- `PeerInfo` struct: Already has `subscribed_namespaces` (client subs). Needs parallel `announced_namespaces` field for peer sync filtering.

### Established Patterns
- Peer-internal messages dispatched in handle_message() switch on TransportMsgType enum
- All SIGHUP-reloadable params follow same pattern: read from new_cfg, assign to member, log
- Connection lifecycle managed in on_peer_connected() coroutine
- Relay blocklist is a set of TransportMsgType checked in is_client_allowed()

### Integration Points
- `on_peer_connected()`: Insert SyncNamespaceAnnounce send/recv after auth, before sync-on-connect
- `on_blob_ingested()`: Add namespace filter check in BlobNotify fan-out loop (check peer's announced set)
- `reload_config()`: Add max_peers reload + trigger re-announce to all peers
- `transport.fbs`: Add SyncNamespaceAnnounce=62 to enum, add NamespaceHash struct and SyncNamespaceAnnounce table
- Sync protocol Phase A/B/C: Filter namespace iteration by intersection of announced sets

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 86-namespace-filtering-hot-reload*
*Context gathered: 2026-04-05*
