# Phase 25: Transport Optimization - Context

**Gathered:** 2026-03-15
**Status:** Ready for planning

<domain>
## Phase Boundary

Lightweight handshake for localhost and trusted peer connections — skip ML-KEM-1024 overhead while keeping mutual identity verification and encrypted transport. Non-trusted remote peers continue using full PQ handshake (no regression).

</domain>

<decisions>
## Implementation Decisions

### Lightweight handshake protocol
- Skip ML-KEM entirely for trusted connections
- Exchange signing pubkeys + random nonce for mutual auth
- Derive session keys via HKDF(nonce || signing pubkeys) — same HKDF context strings as PQ path
- Transport uses ChaCha20-Poly1305 (same send_encrypted/recv_encrypted code, different key derivation)
- NOT PQ-secure — acceptable tradeoff for localhost/trusted peers

### Handshake negotiation
- Server-driven: responder checks if remote IP is trusted, sends TrustedHello if yes
- Initiator must also consider server trusted — both sides must agree for lightweight path
- Trust mismatch: fallback to full PQ handshake (initiator sends KemPubkey, server switches to PQ path)
- No connection failure on mismatch — just slower handshake

### Trust model
- Localhost (127.0.0.1/::1) is always implicitly trusted — no config needed
- `trusted_peers` config: list of plain IP addresses (IPv4 and IPv6)
- No CIDR ranges, no hostnames, no ports — plain IPs only
- Both sides independently check: initiator checks target IP, responder checks remote IP
- Both must agree for lightweight path

### Security boundary
- Localhost and trusted-remote use identical code path (same lightweight handshake)
- ACL still applies: trusted handshake skips ML-KEM but NOT allowed_keys authorization
- Trust (transport optimization) and authorization (access control) are orthogonal
- No force-PQ config flag — if trusted_peers is empty, only localhost triggers lightweight; YAGNI

### Config and SIGHUP
- Config key: `trusted_peers` (list of IP address strings)
- Validation: reject entries with ports (e.g. "192.168.1.5:4200") with clear error message
- SIGHUP reloads trusted_peers — only affects future connections, existing connections kept
- Log changes at info level: "config reload: trusted_peers=N addresses" (consistent with existing reload logging)

### Logging
- Log handshake type at info level: "handshake complete (initiator, lightweight)" vs "handshake complete (initiator, PQ)"
- Operators can verify trusted config is working from logs

### Claude's Discretion
- Exact TrustedHello message format (new TransportMsgType or reuse existing)
- Nonce size and generation
- Error message wording for config validation
- Test structure and organization

</decisions>

<specifics>
## Specific Ideas

- Handshake flow should be 2 messages instead of 4 (TrustedHello exchange + auth, vs KemPubkey/KemCiphertext/AuthSig/AuthSig)
- Session key derivation should reuse existing HKDF infrastructure and context strings
- SIGHUP reload follows exact same pattern as allowed_keys/rate_limit/sync_namespaces reload in PeerManager::reload_config()

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/net/handshake.{h,cpp}`: HandshakeInitiator/HandshakeResponder state machines — new LightweightHandshake class follows same pattern
- `db/net/connection.{h,cpp}`: do_handshake() is the branch point — check trust before choosing handshake path
- `db/crypto/kdf.h`: HKDF extract/expand already used for PQ session key derivation — reuse for lightweight key derivation
- `db/crypto/aead.h`: ChaCha20-Poly1305 encrypt/decrypt — unchanged, used by both paths
- `db/net/framing.h`: write_frame/read_frame — unchanged, used by both paths
- `db/config/config.h`: Config struct — add trusted_peers field
- `db/peer/peer_manager.cpp`: reload_config() — add trusted_peers reload (lines 1014-1097 show pattern)

### Established Patterns
- SIGHUP reload: load new config, validate, apply changes, log diff (PeerManager::reload_config)
- Config validation: validate_allowed_keys pattern — create validate_trusted_peers
- Connection captures remote_addr_ at construction — use remote_endpoint().address() for trust check
- Session keys are directional (send_key/recv_key) with HKDF context strings

### Integration Points
- Connection::do_handshake() — branch between PQ and lightweight based on trust
- Connection needs access to trusted_peers list (pass through Server or config reference)
- PeerManager::reload_config() — add trusted_peers parsing and update
- Config::load_config() — parse new trusted_peers JSON array
- wire/transport.fbs — may need new TransportMsgType for TrustedHello

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 25-transport-optimization*
*Context gathered: 2026-03-15*
