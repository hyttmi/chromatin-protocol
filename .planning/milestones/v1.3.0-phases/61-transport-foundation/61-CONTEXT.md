# Phase 61: Transport Foundation - Context

**Gathered:** 2026-03-24
**Status:** Ready for planning

<domain>
## Phase Boundary

Every request/response message carries a client-assigned `request_id: uint32` through the full encode/decode/dispatch/relay pipeline. FlatBuffers schema updated, codec updated, Connection and MessageCallback signatures updated, relay forwards transparently. No concurrency changes (Phase 62), no new message types (Phase 63).

</domain>

<decisions>
## Implementation Decisions

### Relay forwarding
- **D-01:** `request_id` is opaque to the relay — no logging, no inspection, pure pass-through
- **D-02:** Blocked messages get immediate disconnect as today — no error response echoing `request_id` (client sending peer-only types is buggy or probing per Phase 59 D-01/D-02)
- **D-03:** Single UDS connection per client session unchanged; concurrency concerns deferred to Phase 62
- **D-04:** Relay signature updates bundled with db/ codec changes in the same plan — relay won't compile without the updated `MessageCallback` signature

### Server dispatch (on_peer_message)
- **D-05:** Single `on_peer_message` with added `request_id` parameter — no split into separate client/peer dispatchers
- **D-06:** All `send_message` calls echo the received `request_id`; peer-originated messages naturally carry 0
- **D-07:** Data/WriteAck and Delete/DeleteAck follow the same echo pattern as Read/List/Stats handlers

### Error signal correlation
- **D-08:** `StorageFull` and `QuotaExceeded` echo the `request_id` from the `Data` message that triggered them — SDK can identify which write was rejected
- **D-09:** `Notification` messages are server-initiated, always `request_id = 0`

### WriteAck/DeleteAck correlation
- **D-10:** Both `request_id` (transport-level) and blob hash (application-level) available on acks — belt and suspenders, no removal of existing hash-based matching

### Claude's Discretion
- Relay blocked-message error response strategy (decided: none, immediate disconnect)
- Relay signature update bundling (decided: single plan with db/ changes)
- Compiler warning handling for unused `request_id` in peer-only handler branches
- Exact FlatBuffers field ordering in updated TransportMessage table
- Test structure and organization for request_id round-trip verification

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<canonical_refs>
## Canonical References

### Wire format
- `db/schemas/transport.fbs` — TransportMessage table (currently: type + payload, needs request_id field)
- `db/wire/transport_generated.h` — Generated FlatBuffers code (TransportMsgType enum, 37 types)

### Codec pipeline
- `db/net/protocol.h` — DecodedMessage struct + TransportCodec encode/decode signatures
- `db/net/protocol.cpp` — Encode/decode implementation (FlatBufferBuilder with ForceDefaults)

### Connection
- `db/net/connection.h` — MessageCallback typedef (line 33-35), send_message signature (line 62-64)
- `db/net/connection.cpp` — send_message impl (line 810-814), message_loop callback invocation (line 777-779)

### Server dispatch
- `db/peer/peer_manager.h` — on_peer_message signature (line 185-187)
- `db/peer/peer_manager.cpp` — on_peer_message dispatch (line 467+), ~15 handler branches including Data (815+), Delete (631+), Read (696+), List (729+), Stats handlers

### Relay forwarding
- `relay/core/relay_session.h` — handle_client_message/handle_node_message signatures (lines 53-60)
- `relay/core/relay_session.cpp` — Forwarding lambdas in start() (lines 64-76), message handlers (lines 96-133)

### Requirements
- `.planning/REQUIREMENTS.md` §CONC-01 — request_id in transport envelope
- `.planning/REQUIREMENTS.md` §CONC-02 — Pipeline plumbing (DecodedMessage, TransportCodec, Connection, MessageCallback)
- `.planning/REQUIREMENTS.md` §CONC-05 — Relay bidirectional forwarding

### Prior phase context
- `.planning/phases/59-relay-core/59-CONTEXT.md` — Relay architecture decisions (blocked message behavior, session model, logging philosophy)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `TransportCodec::encode/decode`: Direct modification targets — add request_id parameter to encode, populate field in decode
- `Connection::send_message`: Single call site for TransportCodec::encode — signature change propagates cleanly
- `Connection::message_loop`: Single call site for MessageCallback invocation — add decoded->request_id to callback args
- `RelaySession::handle_client_message/handle_node_message`: Mechanical signature update + pass request_id to send_message

### Established Patterns
- `ForceDefaults(true)` in FlatBufferBuilder: ensures request_id=0 is explicitly serialized (deterministic encoding)
- Coroutine lambda capture in on_peer_message handlers: request_id captured same as payload — `[this, conn, request_id, payload = std::move(payload)]`
- Handshake messages use raw frames (send_raw/recv_raw), not TransportCodec — request_id only applies post-handshake

### Integration Points
- `on_peer_message` dispatch: every handler branch's send_message call gains request_id parameter
- `send_sync_rejected` helper: passes request_id=0 (peer-only, never client-originated)
- All test files using MessageCallback or send_message need signature updates
- Relay test files (test_relay_session, test_message_filter) need updated callback signatures

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 61-transport-foundation*
*Context gathered: 2026-03-24*
