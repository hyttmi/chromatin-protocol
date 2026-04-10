# Phase 103: UDS Multiplexer & Protocol Translation - Context

**Gathered:** 2026-04-10
**Status:** Ready for planning

<domain>
## Phase Boundary

Relay opens a single multiplexed UDS connection to the local node, translates JSON client requests to FlatBuffers binary payloads and back using the table-driven schema from Phase 102, and routes responses to the correct client via relay-scoped request_id mapping. Large payloads (ReadResponse, BatchReadResponse) are sent as binary WebSocket frames. No subscription aggregation or notification fan-out (Phase 104), no UDS auto-reconnect with subscription replay (Phase 104), no rate limiting or graceful shutdown (Phase 105).

</domain>

<decisions>
## Implementation Decisions

### UDS Connection Lifecycle
- **D-01:** Relay connects to node using `asio::local::stream_protocol::socket` directly -- NOT via db/net/Connection. The relay cannot link against db/net/ (Phase 100 D-01: db/ will move to a separate repo). Instead, the relay implements its own lightweight UDS client that speaks the node's binary transport protocol.
- **D-02:** The UDS client performs a TrustedHello handshake. Since db/net/Connection is off-limits, the relay implements the TrustedHello exchange itself: send the relay's ML-DSA-87 pubkey hash, receive the node's pubkey hash. No PQ key exchange -- UDS is always trusted (same as node's UdsAcceptor behavior). The handshake bytes are simple and documented in PROTOCOL.md.
- **D-03:** After TrustedHello, the UDS connection is unencrypted (AEAD nonce counter never starts). Messages are length-prefixed FlatBuffer TransportMessage envelopes, same as the trusted-path wire format.
- **D-04:** Startup behavior: relay starts its WebSocket acceptor immediately. UDS connection attempt runs async. If UDS connect fails, retry with jittered backoff (1s base, 30s cap). Client requests arriving before UDS is ready receive `{"type":"error","code":"node_unavailable","message":"Node connection not ready"}` with their request_id. This is a basic retry loop, NOT the full auto-reconnect with subscription replay from Phase 104.
- **D-05:** Relay uses its ML-DSA-87 identity (from relay_identity.h) for the TrustedHello handshake. Node's UdsAcceptor creates a Connection with the relay's identity as the peer.
- **D-06:** UDS read loop receives TransportMessage FlatBuffers, decodes type + payload + request_id, and dispatches to the RequestRouter for response routing.

### Request-ID Multiplexing
- **D-07:** RequestRouter maintains a `uint32_t next_relay_rid_` counter starting at 1 (0 reserved for server-initiated messages). Wraps at UINT32_MAX. Each client request gets a unique relay-scoped request_id.
- **D-08:** Pending request map: `unordered_map<uint32_t, PendingRequest>` where PendingRequest stores `{client_session_id, client_request_id, created_timestamp}`. Response from node with relay_rid -> look up -> route JSON response to the correct WsSession with the client's original request_id restored.
- **D-09:** Fire-and-forget messages (Ping, Goodbye) do NOT go through RequestRouter. They are forwarded directly to the UDS connection without request_id rewriting. Subscribe/Unsubscribe are intercepted by Phase 104 -- for Phase 103, they are forwarded directly to the node.
- **D-10:** Client disconnect: `remove_client(session_id)` purges all pending entries for that client. Prevents stale entries accumulating.
- **D-11:** Request timeout: 60s stale entry cleanup. Periodic sweep removes entries older than 60s, sends timeout error to client if session still alive.
- **D-12:** Client requests without request_id (fire-and-forget from client perspective): relay still assigns a relay_rid for routing, but the response is forwarded without request_id field (or request_id=0, which is omitted per D-24 from Phase 102).

### Protocol Translation (JSON <-> Binary)
- **D-13:** Table-driven generic encoder/decoder using Phase 102's FieldSpec metadata. A single `json_to_binary()` function iterates the MessageSchema's FieldSpec array and encodes each field per its FieldEncoding type. A single `binary_to_json()` function does the reverse. No per-type handler functions (success criterion #3).
- **D-14:** For non-FlatBuffer message types (35 of 40): the binary payload is a flat concatenation of fields in FieldSpec order. The node's MessageDispatcher expects exactly this format (e.g., ReadRequest = 32-byte namespace + 32-byte hash, ListRequest = 32-byte namespace + 8-byte BE since_seq + 4-byte BE limit).
- **D-15:** For FlatBuffer message types (Data=8, ReadResponse=32, BatchReadResponse=54): special-case handling. These use FlatBuffers encoding for the payload. The relay needs to build/parse FlatBuffer payloads for these types. Mark them with `is_flatbuffer=true` in MessageSchema (already done in Phase 102).
- **D-16:** The FlatBuffers .fbs schema files are copied into relay/wire/ and compiled locally. The relay does NOT link against db/wire/ or db/net/. It has its own copy of the generated headers. TransportCodec (encode/decode transport envelope) is reimplemented in relay as a thin wrapper around the generated FlatBuffer code.
- **D-17:** Data message (type 8) translation: JSON `{"type":"data","namespace":"hex","hash":"hex","data":"base64","ttl":N,"timestamp":"uint64_string","pubkey":"hex","signature":"base64"}` <-> FlatBuffer BlobData. This is the write path -- client sends Data, relay encodes as FlatBuffer, sends to node. Node sends WriteAck back (non-FB type).
- **D-18:** ReadResponse (type 32) translation: node sends `[status_byte][flatbuffer_blob_data]`. Relay decodes the FlatBuffer blob, converts to JSON with base64 data field. Sent as binary WebSocket frame per PROT-04.
- **D-19:** BatchReadResponse (type 54) translation: node sends `[u32BE_count][u32BE_total_size][truncated_byte][blob1_len_u32BE][blob1_fb]...`. Relay decodes each FlatBuffer blob, builds JSON array. Sent as binary WebSocket frame per PROT-04.

### Binary WebSocket Frames (PROT-04)
- **D-20:** ReadResponse and BatchReadResponse are always sent as binary WebSocket frames (opcode 0x2), regardless of size. All other response types are sent as text frames (opcode 0x1). Simple type-based rule, no size threshold.
- **D-21:** The binary frame payload IS still JSON (with base64-encoded data fields). The binary opcode signals to clients that the payload may be large. Clients parse the same JSON format regardless of opcode.

### Translation Integration
- **D-22:** WsSession's `on_message()` AUTHENTICATED path (currently a stub at line 392) becomes the entry point. After JSON parse + type extraction + filter check (already done), the flow is: json_to_binary() -> RequestRouter::register_request() -> UdsMultiplexer::send().
- **D-23:** UDS receive path: UdsMultiplexer read loop -> decode TransportMessage -> RequestRouter::resolve_response() -> binary_to_json() -> WsSession::send_json() (text) or WsSession::send_binary() (for ReadResponse/BatchReadResponse).
- **D-24:** Translation functions live in `relay/translate/translator.h/cpp`. Stateless, pure functions. No IO, no connection state.
- **D-25:** The UDS multiplexer lives in `relay/core/uds_multiplexer.h/cpp`. The request router lives in `relay/core/request_router.h/cpp`.

### Claude's Discretion
- Internal API design for UdsMultiplexer, RequestRouter, and translator functions
- TrustedHello handshake implementation details (byte-level encoding)
- Exact error messages for edge cases (oversized payload, malformed response, etc.)
- Test organization within relay/tests/
- Whether to add a `send_binary()` method to WsSession or reuse existing write_frame with opcode parameter

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Architecture
- `.planning/research/ARCHITECTURE.md` -- Request-ID multiplexing (Pattern 1, lines 93-140), JSON<->FB translation (Pattern 4, lines 215-261), data flow diagrams (lines 264-310), UDS multiplexer lifecycle (lines 343-364), anti-patterns (lines 416-427)

### Protocol
- `db/PROTOCOL.md` -- Wire format spec, TransportMessage envelope format, TrustedHello handshake, message type enum values, field layouts for all 62 message types

### FlatBuffers Schemas
- `db/wire/transport.fbs` -- TransportMessage envelope (type + payload + request_id)
- `db/wire/blob.fbs` -- BlobData FlatBuffer (namespace, hash, data, ttl, timestamp, pubkey, signature)

### Existing Relay Code (Phase 100-102 output)
- `relay/translate/type_registry.h` -- 40-entry constexpr type mapping (json_name <-> wire_type), binary search lookup
- `relay/translate/json_schema.h` -- FieldEncoding enum (12 types), FieldSpec per field, MessageSchema per message type, is_flatbuffer flag for Data/ReadResponse/BatchReadResponse
- `relay/translate/json_schema.cpp` -- schema_for_type() and schema_for_name() lookup functions
- `relay/ws/ws_session.h` -- WsSession with SessionState, AUTHENTICATED path, send_json(), write_frame()
- `relay/ws/ws_session.cpp` -- on_message() AUTHENTICATED stub (line 392: "Phase 103 adds JSON->FlatBuffers translation + UDS forwarding")
- `relay/ws/session_manager.h` -- SessionManager with session map (needed for response routing by client_id)
- `relay/core/session.h` -- Send queue with enqueue/drain + WriteCallback
- `relay/core/message_filter.h` -- 38-type allowlist (already wired into on_message)
- `relay/identity/relay_identity.h` -- ML-DSA-87 identity with sign(), pubkey, pubkey_hash
- `relay/config/relay_config.h` -- RelayConfig with uds_path field
- `relay/relay_main.cpp` -- Main entry point (add UdsMultiplexer construction + connect here)

### Node Code (reference for wire format, NOT linked by relay)
- `db/net/protocol.h` -- TransportCodec encode/decode (reference implementation for relay's own codec)
- `db/net/framing.h` -- Frame format: 4-byte BE length prefix (relay uses this for trusted UDS without encryption)
- `db/wire/codec.h` -- encode_blob/decode_blob for FlatBuffer BlobData
- `db/peer/message_dispatcher.cpp` -- How node decodes binary payloads for each type (reference for field layouts)
- `db/net/handshake.h` -- TrustedHello handshake protocol details
- `db/util/endian.h` -- BE read/write helpers (relay implements its own)

### Requirements
- `.planning/REQUIREMENTS.md` -- MUX-01, MUX-02, PROT-01, PROT-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/translate/json_schema.h` -- Complete FieldSpec metadata for 37 non-FlatBuffer message types. This IS the translation table. Phase 103 writes the generic encoder/decoder that consumes it.
- `relay/translate/type_registry.h` -- json_name <-> wire_type mapping. Used to convert JSON "type" string to TransportMsgType enum for the transport envelope.
- `relay/core/message_filter.h` -- Already wired into WsSession::on_message(). Phase 103 code runs after the filter check.
- `relay/ws/ws_session.cpp` -- to_hex() and from_hex() utilities (file-local). May need to be promoted to a shared header for use by the translator.
- `relay/identity/relay_identity.h` -- ML-DSA-87 identity. pubkey_hash() provides the 32-byte namespace hash for TrustedHello.

### Established Patterns
- Asio coroutine-based read/write loops (ws_session.cpp read_loop, ws_acceptor.cpp accept_loop)
- Send queue: core::Session enqueue() -> drain coroutine -> WsSession::write_frame() callback
- Signal handling: SIGHUP member coroutine pattern
- Config: nlohmann::json loader with validate_relay_config()
- Testing: Catch2 unit tests in relay/tests/

### Integration Points
- `relay/ws/ws_session.cpp:392` -- "Phase 103 adds JSON->FlatBuffers translation + UDS forwarding" stub. This is where the translator + RequestRouter + UdsMultiplexer pipeline plugs in.
- `relay/relay_main.cpp` -- Construct UdsMultiplexer, start UDS connect, pass to SessionManager or WsSession factory for request forwarding.
- `relay/translate/` -- Add translator.h/cpp (generic encoder/decoder using FieldSpec)
- `relay/core/` -- Add uds_multiplexer.h/cpp, request_router.h/cpp
- `relay/wire/` -- New directory: copied .fbs files + generated headers + relay's own TransportCodec
- `relay/CMakeLists.txt` -- Add FlatBuffers FetchContent, .fbs compilation, new source files

</code_context>

<specifics>
## Specific Ideas

- The node's UDS path uses TrustedHello (no PQ crypto, no AEAD). After handshake, messages flow as `[4-byte BE length][TransportMessage FlatBuffer]` -- no encryption. The relay's UDS client is much simpler than a full Connection.
- The translation layer is the CPU-bound component. Profile json_to_binary/binary_to_json for large payloads. base64 encode/decode of blob data (up to 100 MiB) dominates. Consider streaming base64 for very large blobs.
- hex utilities (to_hex, from_hex) currently duplicated in ws_session.cpp (file-local). Phase 103 should extract these to a shared header (e.g., relay/util/hex.h) since the translator needs them too.
- The 3 FlatBuffer message types (Data, ReadResponse, BatchReadResponse) need their own encode/decode paths. The generic table-driven approach covers the other 37 types.
- For Subscribe/Unsubscribe in Phase 103: forward directly to node. Phase 104 adds aggregation. This means the node sees per-request Subscribes from the same UDS connection, which is fine -- it updates the subscription set for the relay's PeerInfo.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 103-uds-multiplexer-protocol-translation*
*Context gathered: 2026-04-10*
