# Phase 4: Networking - Context

**Gathered:** 2026-03-04
**Status:** Ready for planning

<domain>
## Phase Boundary

PQ-encrypted TCP transport with mutual authentication, async IO (Standalone Asio), and graceful shutdown. Node can accept inbound and make outbound TCP connections over a ChaCha20-Poly1305 encrypted, ML-DSA-87 mutually authenticated channel. No peer discovery or sync — those are Phase 5.

**Note:** TRNS-02 references AES-256-GCM but the locked stack decision is ChaCha20-Poly1305 (already implemented in `src/crypto/aead.h`). ChaCha20-Poly1305 is correct.

</domain>

<decisions>
## Implementation Decisions

### Handshake protocol
- Initiator sends ML-KEM-1024 public key first; responder encapsulates and replies with ciphertext
- Session fingerprint for mutual auth: SHA3-256(shared_secret || initiator_pubkey || responder_pubkey)
- Both nodes sign the session fingerprint with ML-DSA-87 after the encrypted channel is established
- On handshake failure (bad KEM, invalid signature, auth rejection): silent TCP close + local log — no error details sent to peer
- 10-second handshake timeout — kill connection if handshake doesn't complete

### Connection lifecycle
- Persistent connections — stay open after handshake, avoid repeated PQ handshake overhead
- Auto-reconnect on unexpected disconnect with exponential backoff (1s, 2s, 4s... capped at 60s)
- No max connection limit for now (pre-MVP, mesh will be small)
- Application-level ping/pong heartbeat (~30s interval) to detect dead connections

### Shutdown behavior
- SIGTERM/SIGINT triggers graceful shutdown with 5-second drain timeout
- Send goodbye message to connected peers before closing (peers can skip reconnect backoff)
- Second SIGTERM/SIGINT during drain = force immediate exit
- Verbose shutdown logging: log each step at info level (draining N connections, storage flushed, goodbye sent, stopped)

### Wire protocol framing
- 4-byte big-endian uint32_t length prefix, then that many bytes of encrypted payload
- FlatBuffers for all transport-level messages (handshake, ping/pong, goodbye, blob transfer) — consistent with existing blob wire format
- 16 MB max message size — reject frames exceeding this before allocating memory
- Counter-based nonces: 64-bit send counter per direction, zero overhead per frame. AEAD decryption failure self-detects desync.

### Claude's Discretion
- Exact Asio event loop architecture (single-threaded vs strand-per-connection)
- Internal buffer management and memory allocation strategy
- Specific FlatBuffer schema design for transport messages
- Ping/pong timeout threshold and dead-peer detection logic
- HKDF context strings for key derivation from shared secret

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `crypto::KEM` (src/crypto/kem.h): ML-KEM-1024 encaps/decaps — ready for key exchange
- `crypto::AEAD` (src/crypto/aead.h): ChaCha20-Poly1305 encrypt/decrypt — ready for channel encryption
- `crypto::Signer` (src/crypto/signing.h): ML-DSA-87 sign/verify — ready for mutual auth
- `crypto::KDF` (src/crypto/kdf.h): HKDF-SHA256 extract/expand — ready for deriving session keys from shared secret
- `crypto::Hash` (src/crypto/hash.h): SHA3-256 — ready for session fingerprint
- `identity::NodeIdentity` (src/identity/identity.h): Wraps signer + namespace, has load_or_generate
- `config::Config` (src/config/config.h): Already has `bind_address` ("0.0.0.0:4200") and `bootstrap_peers` fields

### Established Patterns
- RAII wrappers for crypto (KEM, Signer are move-only)
- `SecureBytes` for sensitive data (auto-zeroing)
- Flat `src/` layout (src/crypto/, src/config/, src/identity/, etc.)
- Catch2 tests in `tests/` directory
- `chromatin::` namespace throughout

### Integration Points
- New `src/net/` or `src/transport/` directory for networking code
- Config already provides bind address and peer list — networking reads these directly
- NodeIdentity provides the signing key for mutual auth
- FlatBuffers schemas in existing wire/ directory can be extended for transport messages
- BlobEngine (Phase 3) will feed blobs into the transport layer in Phase 5

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 04-networking*
*Context gathered: 2026-03-04*
