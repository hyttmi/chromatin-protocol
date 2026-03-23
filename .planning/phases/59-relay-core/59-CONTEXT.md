# Phase 59: Relay Core - Context

**Gathered:** 2026-03-23
**Status:** Ready for planning

<domain>
## Phase Boundary

PQ handshake responder, UDS forwarding, message type filter, bidirectional relay. A client can connect to the relay via PQ-encrypted channel and interact with the chromatindb node -- the relay authenticates, filters, and forwards. No rate limiting, no connection pooling, no payload inspection.

</domain>

<decisions>
## Implementation Decisions

### Blocked message response
- **D-01:** When a client sends a blocked message type (peer-only operations), log a warning with type name + client pubkey hash, then disconnect the client immediately
- **D-02:** Rationale: a client sending SyncRequest/PeerListRequest is either buggy or probing -- disconnect sends a clear signal and prevents repeat attempts; logging gives operators visibility

### Node unavailability behavior
- **D-03:** If UDS connect to the node fails when a new client arrives, refuse the client (close TCP connection)
- **D-04:** If UDS connection drops mid-session, disconnect the client immediately -- no reconnect attempts, no queuing
- **D-05:** Relay is a thin pipe; masking node failures creates confusion. Clients should know instantly so they can retry or failover to another relay.

### Session observability
- **D-06:** `info` level: client connected (pubkey hash, remote IP), client disconnected (reason: graceful/error/blocked)
- **D-07:** `warn` level: blocked message type from client (type name, pubkey hash) -- security-relevant event
- **D-08:** No per-message logging, no message counters. Relay is a dumb pipe; message-level observability is the node's job. Keep relay logs minimal and security-focused.

### Claude's Discretion
- Connection architecture (reuse Connection class directly or wrap)
- Coroutine structure for bidirectional relay loop
- How RelayIdentity interfaces with Connection's NodeIdentity dependency
- Graceful shutdown wiring (SIGINT/SIGTERM)
- Internal error handling and cleanup order

</decisions>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches

</specifics>

<canonical_refs>
## Canonical References

### Relay scaffolding (Phase 58)
- `relay/relay_main.cpp` -- Entry point, config loading, identity init (relay loop goes here)
- `relay/config/relay_config.h` -- RelayConfig struct (bind_address, bind_port, uds_path, identity_key_path, log_level, log_file)
- `relay/identity/relay_identity.h` -- RelayIdentity (ML-DSA-87 keypair, load/generate/save)

### Connection & networking
- `db/net/connection.h` -- Connection class: PQ handshake responder, UDS initiator, send_message/recv, message callbacks
- `db/net/uds_acceptor.h` -- UdsAcceptor: UDS listen + accept loop pattern (reference for TCP acceptor)
- `db/net/protocol.h` -- TransportCodec: encode/decode, DecodedMessage struct
- `db/net/framing.h` -- Length-prefixed framing
- `db/net/handshake.h` -- PQ handshake + TrustedHello implementation

### Wire format
- `db/wire/transport_generated.h` -- TransportMsgType enum (37 types, type IDs 0-37)

### Message type filter (RELAY-03)
- `.planning/REQUIREMENTS.md` §RELAY-03 -- Exact allow/block lists for message type filtering

### Prior context
- `.planning/phases/58-relay-scaffolding/58-CONTEXT.md` -- Phase 58 decisions (config, identity, CLI)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Connection` class: full PQ handshake responder (`create_inbound`) and UDS initiator (`create_uds_outbound`) -- both roles needed by relay
- `TransportCodec::decode()`: extracts message type without full payload parse -- relay can read type byte for filtering
- `UdsAcceptor`: accept loop pattern reusable as reference for relay's TCP acceptor
- `chromatindb_lib`: all crypto, identity, networking, wire format linked by relay already

### Established Patterns
- `Connection::on_message` callback: receives (conn, type, payload) -- type field is what relay filters on
- `Connection::on_close` callback: fires on disconnect -- triggers UDS teardown
- `Connection::on_ready` callback: fires after handshake, before message loop -- where relay opens UDS connection

### Integration Points
- `relay_main.cpp` cmd_run(): currently loads config + identity and exits -- relay loop goes here
- Relay needs TCP acceptor (similar to Server's accept loop) + per-client UDS connection
- RelayIdentity must be compatible with Connection's NodeIdentity reference (same underlying key types)

</code_context>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 59-relay-core*
*Context gathered: 2026-03-23*
