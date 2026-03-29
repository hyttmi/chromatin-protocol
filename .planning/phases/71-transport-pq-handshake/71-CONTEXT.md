# Phase 71: Transport & PQ Handshake - Context

**Gathered:** 2026-03-29
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK establishes a PQ-authenticated, AEAD-encrypted session with a live relay. Includes ML-KEM-1024 key exchange, ML-DSA-87 mutual authentication, ChaCha20-Poly1305 encrypted framing, per-direction nonce management, connection lifecycle (context manager, Goodbye), and a background reader task for multiplexed frame dispatch. No data operations (read/write/delete/list) -- those are Phase 72.

</domain>

<decisions>
## Implementation Decisions

### Connection API surface
- **D-01:** Context manager pattern -- `async with ChromatinClient.connect(host, port, identity) as conn:`. Handshake on enter, Goodbye + close on exit. No explicit connect/close alternative.
- **D-02:** Class name: `ChromatinClient`. Exported from `chromatindb` package top-level.
- **D-03:** High-level API only -- no exposed transport methods (send_frame, recv_frame). All transport internals are private.
- **D-04:** Phase 71 exposes only what works: connect, disconnect, ping/pong. No stubs for future data operations. Phase 72 adds read/write/etc.

### Async/sync model
- **D-05:** Pure asyncio streams -- `asyncio.open_connection()` for TCP. Zero networking dependencies.
- **D-06:** Phase 71 is async-only. Sync wrapper deferred to Phase 74 (packaging).
- **D-07:** Configurable handshake timeout (default 10s). Raises HandshakeError on timeout. No separate TCP connect timeout -- asyncio handles that.

### Frame reader architecture
- **D-08:** Background reader coroutine spawned after handshake. Continuously reads and decrypts frames, dispatches by request_id to pending futures, and routes notifications to a queue. Foundation for Phase 72/73 multiplexing.
- **D-09:** request_id counter auto-assigned by transport layer. Internal counter increments per request. Callers never see or manage request_id.
- **D-10:** On disconnection or frame corruption: reader cancels all pending request futures with ConnectionError, sets connection state to closed. Next user operation raises immediately.

### Integration testing
- **D-11:** Integration tests run against live KVM swarm relay (192.168.1.200:4433). No Docker compose for SDK tests.
- **D-12:** Both unit tests and integration tests. Unit tests for frame encode/decode, nonce counter logic, AEAD encrypt/decrypt (no network). Integration tests for full handshake against KVM relay.
- **D-13:** Relay address from environment variables: `CHROMATINDB_RELAY_HOST` / `CHROMATINDB_RELAY_PORT` with defaults 192.168.1.200 / 4433. Integration tests skip if relay unreachable (pytest.mark.integration).

### Claude's Discretion
- Internal module organization (transport.py vs splitting into handshake.py + framing.py + client.py)
- Notification queue implementation details (asyncio.Queue vs callback registration)
- Ping/pong implementation details and keepalive behavior
- pytest fixture structure for integration tests (conftest.py, identity fixtures)
- Internal error handling granularity (which exceptions for which failure modes)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### PQ Handshake protocol (SDK must replicate initiator side)
- `db/net/handshake.h` -- HandshakeInitiator/Responder classes, SessionKeys struct, derive_session_keys()
- `db/net/handshake.cpp` -- Full handshake implementation: KEM encaps/decaps, HKDF key derivation, auth message creation/verification
- `db/net/connection.h` -- Connection class: do_handshake_initiator_pq(), send_raw(), recv_raw(), send_encrypted(), recv_encrypted()
- `db/net/connection.cpp` -- Connection lifecycle: run(), message_loop(), close_gracefully()

### Frame format (SDK must match wire-level)
- `db/net/framing.h` -- write_frame(), read_frame(), make_nonce(), MAX_FRAME_SIZE (110 MiB)
- `db/net/framing.cpp` -- Frame encoding: [4-byte BE length][AEAD ciphertext]. Nonce: [4 zero bytes][8-byte BE counter]

### Crypto primitives (Phase 70, already implemented in SDK)
- `sdk/python/chromatindb/crypto.py` -- AEAD encrypt/decrypt, SHA3-256, build_signing_input
- `sdk/python/chromatindb/_hkdf.py` -- HKDF-SHA256 extract/expand/derive
- `sdk/python/chromatindb/identity.py` -- ML-DSA-87 Identity class, sign/verify
- `sdk/python/chromatindb/wire.py` -- TransportMsgType enum, FlatBuffers encode/decode

### Wire format schemas
- `db/schemas/transport.fbs` -- TransportMessage table with type, payload, request_id

### Known protocol quirks (carried from Phase 70)
- Empty HKDF salt in PQ handshake (C++ uses empty, PROTOCOL.md incorrectly says SHA3-256(pubkeys))
- Mixed endianness: BE for framing, LE for auth payload pubkey_size and signing input (ttl/timestamp)
- AEAD nonce starts at 1 after PQ handshake (nonce 0 consumed by auth exchange)
- KemPubkey message payload: [kem_pk:1568B][sig_pk:2592B] (concatenated, no length prefix)
- KemCiphertext message payload: [kem_ct:1568B][sig_pk:2592B] (concatenated, no length prefix)
- Auth payload: [4-byte LE pubkey_size][pubkey][signature]
- Session fingerprint: SHA3-256(shared_secret || initiator_sig_pk || responder_sig_pk)
- HKDF contexts: "chromatin-init-to-resp-v1" and "chromatin-resp-to-init-v1"

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `chromatindb.crypto.aead_encrypt/aead_decrypt` -- ChaCha20-Poly1305 IETF, ready for frame encryption
- `chromatindb._hkdf.hkdf_extract/hkdf_expand` -- HKDF-SHA256 for session key derivation
- `chromatindb.identity.Identity` -- ML-DSA-87 sign/verify for auth exchange
- `chromatindb.wire.encode_transport_message/decode_transport_message` -- FlatBuffers envelope for handshake messages
- `chromatindb.wire.TransportMsgType` -- All 58 message type constants including KemPubkey(1), KemCiphertext(2), AuthSignature(3), Ping(5), Pong(6), Goodbye(7)

### Established Patterns
- Phase 70 crypto is byte-identical to C++ -- handshake can trust these primitives
- Identity class has `.sign(message)` and `.verify(message, sig, pubkey)` -- used directly for auth exchange
- FlatBuffers encode/decode tested against C++ vectors -- wire format interop confirmed
- Exception hierarchy has HandshakeError and ConnectionError ready (defined in Phase 70 D-13)

### Integration Points
- `ChromatinClient` imports Identity, crypto, wire, _hkdf, exceptions from existing modules
- liboqs-python provides `oqs.KeyEncapsulation("ML-KEM-1024")` for ephemeral KEM operations
- Test vectors from Phase 70 validate crypto correctness; Phase 71 validates protocol correctness end-to-end

</code_context>

<specifics>
## Specific Ideas

- Handshake sequence is fixed by C++ implementation -- SDK is always the initiator (client), relay is always the responder
- SDK always uses PQ handshake (never TrustedHello) since it connects over TCP to a remote relay
- Background reader task is the architectural foundation that Phase 72/73 build on -- get this right
- KVM swarm at 192.168.1.200 is the primary test target; relay listens on port 4433

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 71-transport-pq-handshake*
*Context gathered: 2026-03-29*
