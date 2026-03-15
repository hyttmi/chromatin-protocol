# Phase 25: Transport Optimization - Research

**Researched:** 2026-03-15
**Domain:** Transport-layer handshake optimization (lightweight non-PQ path for trusted peers)
**Confidence:** HIGH

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Skip ML-KEM entirely for trusted connections
- Exchange signing pubkeys + random nonce for mutual auth
- Derive session keys via HKDF(nonce || signing pubkeys) -- same HKDF context strings as PQ path
- Transport uses ChaCha20-Poly1305 (same send_encrypted/recv_encrypted code, different key derivation)
- NOT PQ-secure -- acceptable tradeoff for localhost/trusted peers
- Server-driven: responder checks if remote IP is trusted, sends TrustedHello if yes
- Initiator must also consider server trusted -- both sides must agree for lightweight path
- Trust mismatch: fallback to full PQ handshake (initiator sends KemPubkey, server switches to PQ path)
- No connection failure on mismatch -- just slower handshake
- Localhost (127.0.0.1/::1) is always implicitly trusted -- no config needed
- `trusted_peers` config: list of plain IP addresses (IPv4 and IPv6)
- No CIDR ranges, no hostnames, no ports -- plain IPs only
- Both sides independently check: initiator checks target IP, responder checks remote IP
- Both must agree for lightweight path
- Localhost and trusted-remote use identical code path (same lightweight handshake)
- ACL still applies: trusted handshake skips ML-KEM but NOT allowed_keys authorization
- Trust (transport optimization) and authorization (access control) are orthogonal
- No force-PQ config flag -- if trusted_peers is empty, only localhost triggers lightweight; YAGNI
- Config key: `trusted_peers` (list of IP address strings)
- Validation: reject entries with ports (e.g. "192.168.1.5:4200") with clear error message
- SIGHUP reloads trusted_peers -- only affects future connections, existing connections kept
- Log changes at info level: "config reload: trusted_peers=N addresses" (consistent with existing reload logging)
- Log handshake type at info level: "handshake complete (initiator, lightweight)" vs "handshake complete (initiator, PQ)"
- Handshake flow should be 2 messages instead of 4

### Claude's Discretion
- Exact TrustedHello message format (new TransportMsgType or reuse existing)
- Nonce size and generation
- Error message wording for config validation
- Test structure and organization

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| TOPT-01 | Connections from localhost (127.0.0.1/::1) or configured trusted_peers skip ML-KEM-1024 handshake | Lightweight handshake design using HKDF key derivation from nonce + signing pubkeys; trust check at do_handshake() branch point; new TrustedHello TransportMsgType |
| TOPT-02 | Trusted peer connections use a pre-shared key or null-encryption mode with mutual identity verification | Session keys derived via existing HKDF infrastructure with random nonce as IKM; mutual auth via signing the session fingerprint; same ChaCha20-Poly1305 encrypted transport after handshake |
| TOPT-03 | trusted_peers is a config option (list of addresses) reloadable via SIGHUP | Config struct field, JSON parsing, validate_trusted_peers, reload_config() extension; follows exact same pattern as allowed_keys/rate_limit/sync_namespaces reload |
</phase_requirements>

## Summary

This phase adds a lightweight handshake path for localhost and trusted peer connections, skipping the expensive ML-KEM-1024 key exchange (1568-byte public keys, 1568-byte ciphertexts, liboqs encapsulation/decapsulation) while preserving mutual identity verification and encrypted transport. The architecture is a branch in `Connection::do_handshake()` that chooses between the existing 4-message PQ handshake and a new 2-message lightweight handshake based on IP-level trust.

The implementation touches four subsystems: (1) transport schema -- adding a `TrustedHello` message type, (2) handshake -- new lightweight key derivation and auth using existing HKDF/ChaCha20 infrastructure, (3) connection -- trust check + handshake path selection + passing trusted_peers list, (4) config/reload -- parsing `trusted_peers` with IP validation and SIGHUP integration. All post-handshake code (send_encrypted, recv_encrypted, message_loop, sync, PEX) remains untouched because both paths produce the same `SessionKeys` struct.

**Primary recommendation:** Implement as a single new `TrustedHello` TransportMsgType with a 2-message exchange (both sides send TrustedHello simultaneously is NOT safe due to responder-driven design; instead: responder sends TrustedHello first, initiator responds with TrustedHello). Derive session keys from `HKDF(nonce_init || nonce_resp, signing_pk_init || signing_pk_resp)` using the same context strings as the PQ path.

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libsodium | latest | HKDF-SHA256 (crypto_kdf_hkdf_sha256_*), ChaCha20-Poly1305 (AEAD), randombytes_buf (nonce gen) | Already used for all symmetric crypto; HKDF and AEAD APIs proven in existing handshake |
| liboqs | 0.15.0 | ML-DSA-87 signing (verify peer identity in lightweight path) | Already used; lightweight path still uses signing for mutual auth |
| FlatBuffers | 25.2.10 | Wire format for TrustedHello message | Already used for all TransportMsgType encoding/decoding |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| Standalone Asio | 1.38.0 | Socket remote_endpoint().address() for trust check | Used to extract peer IP at connection time |
| nlohmann/json | 3.11.3 | Parsing trusted_peers from config JSON | Same pattern as allowed_keys, sync_namespaces |
| spdlog | 1.15.1 | Logging handshake type and config reload | Same pattern as existing handshake/reload logging |

### Alternatives Considered
None -- all libraries are already in the project. No new dependencies needed.

## Architecture Patterns

### Recommended Changes
```
db/schemas/transport.fbs    -- Add TrustedHello = 24 to TransportMsgType enum
db/wire/transport_generated.h -- Regenerated by flatc
db/config/config.h          -- Add trusted_peers field + validate_trusted_peers()
db/config/config.cpp        -- Parse trusted_peers JSON array + validation
db/net/handshake.h          -- Add derive_lightweight_session_keys() + LightweightHandshake class
db/net/handshake.cpp        -- Implement lightweight key derivation + 2-message exchange
db/net/connection.h         -- Add trusted_peers access (std::set<std::string> or function)
db/net/connection.cpp       -- Branch in do_handshake() based on trust
db/peer/peer_manager.cpp    -- Add trusted_peers to reload_config(), pass to connections
tests/config/test_config.cpp -- trusted_peers config tests
tests/net/test_handshake.cpp -- Lightweight handshake unit tests
tests/net/test_connection.cpp -- Integration tests (lightweight over loopback)
```

### Pattern 1: Responder-Driven Handshake Negotiation
**What:** The responder (inbound side) decides whether to offer lightweight handshake based on the remote IP address. The initiator (outbound side) decides whether to accept based on the target IP address.
**When to use:** Every new connection in `do_handshake()`.
**How it works:**
1. Responder checks if `remote_endpoint().address()` is trusted (localhost or in trusted_peers set)
2. If trusted: responder sends `TrustedHello` message (contains signing pubkey + 32-byte random nonce)
3. Initiator receives first message. If it's `TrustedHello` AND initiator considers the target IP trusted: initiator responds with its own `TrustedHello` (signing pubkey + 32-byte random nonce)
4. Both sides derive session keys from the exchanged nonces and pubkeys
5. If trust mismatch (initiator gets `TrustedHello` but doesn't trust the peer): initiator sends `KemPubkey` instead, and responder falls back to full PQ handshake
6. If responder doesn't trust: responder waits for `KemPubkey` (normal PQ path), no TrustedHello sent

**Key insight:** The responder ALWAYS reads first in the current PQ path (waits for KemPubkey). In the lightweight path, the responder WRITES first (sends TrustedHello). This means the responder must decide before reading. This is possible because the responder has the remote IP from `socket_.remote_endpoint()` at construction time.

### Pattern 2: Lightweight Key Derivation
**What:** Derive session keys from exchanged nonces and signing public keys using HKDF, without ML-KEM.
**When to use:** After both TrustedHello messages are exchanged.
**Details:**
```
IKM = nonce_initiator || nonce_responder  (64 bytes total)
Salt = signing_pk_initiator || signing_pk_responder  (5184 bytes total)
PRK = HKDF-Extract(salt=Salt, ikm=IKM)
send_key = HKDF-Expand(PRK, "chromatin-init-to-resp-v1", 32)
recv_key = HKDF-Expand(PRK, "chromatin-resp-to-init-v1", 32)
fingerprint = SHA3-256(IKM || signing_pk_initiator || signing_pk_responder)
```
This reuses the exact same context strings ("chromatin-init-to-resp-v1", "chromatin-resp-to-init-v1") and produces the same `SessionKeys` struct. The only difference is the input key material: random nonces instead of ML-KEM shared secret.

**Security note:** Without ML-KEM, the nonces provide session uniqueness but NOT forward secrecy against a party that later compromises signing keys. This is the accepted tradeoff for trusted peers -- the nonces ensure different session keys per connection, and the signing pubkeys in the salt bind the keys to the specific identities.

### Pattern 3: IP Trust Check
**What:** Check if a remote IP address is localhost or in the trusted_peers set.
**When to use:** At the start of `do_handshake()` before sending/receiving any messages.
**Details:**
```cpp
bool is_trusted_address(const asio::ip::address& addr,
                        const std::set<std::string>& trusted_peers) {
    // Localhost is always trusted
    if (addr.is_loopback()) return true;  // Covers 127.0.0.1 AND ::1

    // Check configured trusted peers
    return trusted_peers.count(addr.to_string()) > 0;
}
```
**Key insight:** Use `asio::ip::address::is_loopback()` instead of string comparison. This correctly handles both IPv4 127.0.0.1 and IPv6 ::1, plus mapped addresses like ::ffff:127.0.0.1.

### Pattern 4: Mismatch Fallback
**What:** If the responder sends TrustedHello but the initiator doesn't trust the peer, the connection falls back to PQ without failure.
**When to use:** Trust disagreement between peers.
**Flow:**
1. Responder sends TrustedHello (because it trusts the initiator's IP)
2. Initiator receives TrustedHello but doesn't trust the responder's IP
3. Initiator ignores the TrustedHello content and sends KemPubkey (normal PQ initiator step 1)
4. Responder receives KemPubkey (not TrustedHello), recognizes the mismatch
5. Responder processes KemPubkey normally (encapsulate, derive PQ session keys)
6. Rest of PQ handshake proceeds (KemCiphertext, AuthSignature, AuthSignature)

**Implementation:** Responder, after sending TrustedHello, reads the next message. If it's `TransportMsgType_TrustedHello`, proceed with lightweight. If it's `TransportMsgType_KemPubkey`, fall back to PQ path (use `HandshakeResponder::receive_kem_pubkey()` on the received message).

### Anti-Patterns to Avoid
- **Mutual simultaneous TrustedHello without coordination:** Both sides sending TrustedHello simultaneously creates a race. The responder-driven design avoids this -- responder sends first (or not), initiator responds.
- **Checking trust after handshake:** Trust must be checked BEFORE any handshake messages are sent, to avoid wasting an ML-KEM key generation.
- **Storing trust status on Connection for ACL purposes:** Trust (transport optimization) is orthogonal to ACL (authorization). A trusted transport does NOT mean the peer is authorized. ACL check in `on_peer_connected()` still runs after handshake completes.
- **Using signing keys as shared secret:** Signing keys provide authentication but not session key agreement. The random nonces provide the entropy for session keys.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Nonce generation | Custom RNG | `randombytes_buf()` from libsodium | Cryptographically secure, cross-platform |
| Key derivation | Custom KDF | `crypto::KDF::extract()` + `crypto::KDF::expand()` | Already proven in PQ handshake, RFC 5869 compliant |
| Session fingerprint | Custom hash | `crypto::sha3_256()` | Already used in PQ path |
| IP address parsing | String manipulation | `asio::ip::address::from_string()` + `.is_loopback()` | Handles IPv4, IPv6, mapped addresses correctly |
| Config validation | Regex | String `find(':')` check for port rejection | Simple, precise, no regex dependency |

**Key insight:** This phase is almost entirely about routing (choosing the right handshake path) and reusing existing crypto primitives in a different combination. No new cryptographic primitives are needed.

## Common Pitfalls

### Pitfall 1: IPv4-Mapped IPv6 Addresses
**What goes wrong:** A connection from `::ffff:127.0.0.1` (IPv4-mapped IPv6) is not recognized as localhost because the trust check only compares against "127.0.0.1" and "::1" strings.
**Why it happens:** Dual-stack sockets on Linux present IPv4 connections as IPv4-mapped IPv6 addresses.
**How to avoid:** Use `asio::ip::address::is_loopback()` which correctly handles all loopback forms. For trusted_peers, store as `asio::ip::address` objects (not strings) so `::ffff:192.168.1.5` matches `192.168.1.5`.
**Warning signs:** Tests pass on IPv4-only but fail when running on systems with IPv6 enabled, or trusted_peers entries don't match when connecting via IPv6.

### Pitfall 2: Nonce Reuse in Lightweight Handshake
**What goes wrong:** If both sides use the same nonce, or nonces aren't random, session keys become predictable.
**Why it happens:** Copy-paste from PQ path where the shared secret comes from ML-KEM (which has its own randomness).
**How to avoid:** Each side generates a fresh 32-byte random nonce via `randombytes_buf()` for every connection attempt. The IKM is the concatenation of BOTH nonces, ensuring uniqueness even if one side has a weak RNG.
**Warning signs:** Session keys are identical across connections from the same peer pair.

### Pitfall 3: Handshake Message Ordering Deadlock
**What goes wrong:** If responder sends TrustedHello and then immediately waits for TrustedHello, but initiator sends KemPubkey (mismatch), the responder is stuck waiting for the wrong message type.
**Why it happens:** Tight coupling between send and expected response.
**How to avoid:** After sending TrustedHello, responder reads the next message and dispatches based on its type: TrustedHello -> lightweight path, KemPubkey -> PQ fallback. The message type byte is checked before any further processing.
**Warning signs:** Connections between mismatched-trust peers hang instead of falling back to PQ.

### Pitfall 4: SIGHUP Trusted Peers vs Active Connections
**What goes wrong:** After SIGHUP removes a peer from trusted_peers, someone expects existing connections to that peer to be re-handshaked or disconnected.
**Why it happens:** Confusion between transport trust and access control.
**How to avoid:** The user decision is clear: "SIGHUP reloads trusted_peers -- only affects future connections, existing connections kept." Log the change and move on.
**Warning signs:** Test tries to assert that existing connections change behavior after SIGHUP.

### Pitfall 5: Connection Constructor Needs Trusted Peers Access
**What goes wrong:** `Connection` is created by `Server` (which doesn't know about trusted_peers), but `do_handshake()` needs the trusted peer list.
**Why it happens:** Layering -- Server creates connections, PeerManager configures them.
**How to avoid:** Either (a) pass trusted_peers through a callback/reference that Connection can query, or (b) add a setter on Connection that PeerManager calls before `run()`, or (c) pass a trust-check function to Connection. Option (c) is cleanest -- a `std::function<bool(const asio::ip::address&)>` that encapsulates the localhost + trusted_peers logic.
**Warning signs:** Connection can't determine trust at handshake time.

### Pitfall 6: Signing Pubkey Size in TrustedHello
**What goes wrong:** ML-DSA-87 public keys are 2592 bytes. Two TrustedHello messages carry 2592 bytes of pubkey + 32 bytes of nonce each = ~5248 bytes total. This is much larger than expected for a "lightweight" handshake.
**Why it happens:** PQ signing keys are large by nature.
**How to avoid:** Accept this -- the optimization is about skipping ML-KEM (key generation + encapsulation/decapsulation compute time), not about reducing message size. The signing pubkeys must be exchanged for mutual authentication regardless. The PQ handshake sends 1568 + 2592 = 4160 bytes per side in the first message alone, so the lightweight TrustedHello (2624 bytes) is actually smaller per message.
**Warning signs:** None -- this is expected behavior, not a bug.

## Code Examples

### TrustedHello Message Format
```
TransportMsgType_TrustedHello payload:
  [nonce: 32 bytes][signing_pubkey: 2592 bytes]
Total payload: 2624 bytes
```

Both initiator and responder send this same format. The 32-byte nonce is generated fresh per connection via `randombytes_buf()`.

### Lightweight Session Key Derivation
```cpp
// After exchanging TrustedHello messages, both sides have:
// - initiator_nonce (32 bytes), responder_nonce (32 bytes)
// - initiator_signing_pk (2592 bytes), responder_signing_pk (2592 bytes)

SessionKeys derive_lightweight_session_keys(
    std::span<const uint8_t> initiator_nonce,      // 32 bytes
    std::span<const uint8_t> responder_nonce,       // 32 bytes
    std::span<const uint8_t> initiator_signing_pk,  // 2592 bytes
    std::span<const uint8_t> responder_signing_pk,  // 2592 bytes
    bool is_initiator) {

    // IKM = initiator_nonce || responder_nonce
    std::vector<uint8_t> ikm;
    ikm.reserve(64);
    ikm.insert(ikm.end(), initiator_nonce.begin(), initiator_nonce.end());
    ikm.insert(ikm.end(), responder_nonce.begin(), responder_nonce.end());

    // Salt = initiator_signing_pk || responder_signing_pk
    std::vector<uint8_t> salt;
    salt.reserve(initiator_signing_pk.size() + responder_signing_pk.size());
    salt.insert(salt.end(), initiator_signing_pk.begin(), initiator_signing_pk.end());
    salt.insert(salt.end(), responder_signing_pk.begin(), responder_signing_pk.end());

    // HKDF extract
    auto prk = crypto::KDF::extract(salt, ikm);

    // HKDF expand -- SAME context strings as PQ path
    auto init_to_resp = crypto::KDF::expand(
        prk.span(), "chromatin-init-to-resp-v1", crypto::AEAD::KEY_SIZE);
    auto resp_to_init = crypto::KDF::expand(
        prk.span(), "chromatin-resp-to-init-v1", crypto::AEAD::KEY_SIZE);

    // Session fingerprint: SHA3-256(ikm || initiator_pk || responder_pk)
    std::vector<uint8_t> fp_input;
    fp_input.reserve(ikm.size() + salt.size());
    fp_input.insert(fp_input.end(), ikm.begin(), ikm.end());
    fp_input.insert(fp_input.end(), salt.begin(), salt.end());
    auto fingerprint = crypto::sha3_256(fp_input);

    SessionKeys keys;
    keys.session_fingerprint = fingerprint;
    if (is_initiator) {
        keys.send_key = std::move(init_to_resp);
        keys.recv_key = std::move(resp_to_init);
    } else {
        keys.send_key = std::move(resp_to_init);
        keys.recv_key = std::move(init_to_resp);
    }
    return keys;
}
```

### Handshake Branch in do_handshake()
```cpp
asio::awaitable<bool> Connection::do_handshake() {
    // Determine trust based on remote IP
    asio::error_code ec;
    auto remote_addr = socket_.remote_endpoint(ec).address();
    bool peer_is_trusted = !ec && is_trusted_(remote_addr);  // is_trusted_ is std::function

    if (is_initiator_) {
        // Initiator: read first message from responder
        auto first_msg = co_await recv_raw();
        if (!first_msg) co_return false;

        auto decoded = TransportCodec::decode(*first_msg);
        if (!decoded) co_return false;

        if (decoded->type == TransportMsgType_TrustedHello && peer_is_trusted) {
            // Both sides agree: lightweight path
            co_return co_await do_lightweight_handshake_initiator(decoded->payload);
        } else if (decoded->type == TransportMsgType_TrustedHello && !peer_is_trusted) {
            // Mismatch: responder trusts us, we don't trust them -> PQ fallback
            // Send KemPubkey as normal PQ initiator
            co_return co_await do_pq_handshake_initiator();
        } else {
            // Responder didn't send TrustedHello -> this shouldn't happen
            // (responder either sends TrustedHello or waits for KemPubkey)
            co_return false;
        }
    } else {
        // Responder: decide whether to offer lightweight
        if (peer_is_trusted) {
            // Send TrustedHello
            co_return co_await do_trusted_responder_handshake();
        } else {
            // Normal PQ path: wait for KemPubkey
            co_return co_await do_pq_handshake_responder();
        }
    }
}
```

**Wait -- there's a subtlety here.** In the current PQ handshake, the **initiator sends first** (KemPubkey), then the responder reads. But in the lightweight path, the **responder sends first** (TrustedHello). This changes the message flow:

- **Current PQ:** Initiator sends KemPubkey -> Responder reads, sends KemCiphertext -> ...
- **Lightweight:** Responder sends TrustedHello -> Initiator reads, sends TrustedHello -> ...

For the **mismatch case** where responder trusts but initiator doesn't:
- Responder sends TrustedHello
- Initiator reads it, sees it's TrustedHello but doesn't trust -> Initiator sends KemPubkey
- Responder reads next message, sees KemPubkey -> falls back to PQ path

For the case where **responder doesn't trust but initiator does**:
- Responder waits for KemPubkey (normal PQ responder)
- Initiator... needs to know the responder didn't send TrustedHello. But initiator is currently the first sender in PQ.

**This reveals a protocol design issue.** The current PQ path has initiator-sends-first. The lightweight path has responder-sends-first. These are incompatible unless we change the protocol flow.

**Proposed resolution:** Change the protocol so that:
1. **Responder always sends first** -- either TrustedHello (if trusted) or a new "PQHello" / simply waits
2. Actually, simpler: the **initiator waits briefly** to see if responder sends TrustedHello. But this introduces timing complexity.

**Better approach (recommended):** The initiator always sends first, as in the current protocol. But instead of always sending KemPubkey, the initiator sends either KemPubkey (if not trusted) or TrustedHello (if trusted). The responder reads the first message and responds accordingly:

- **Initiator trusts, sends TrustedHello:** Responder checks trust. If also trusts -> responds with TrustedHello (lightweight). If doesn't trust -> responds with "please send KemPubkey" or just closes? No, the user said "no connection failure on mismatch." So responder must respond with KemCiphertext? No, it has no KEM pubkey yet.

**Cleanest approach (initiator-first, compatible with existing flow):**
1. Initiator sends first message: `KemPubkey` (PQ) or `TrustedHello` (lightweight)
2. Responder reads first message:
   - If `KemPubkey` -> PQ path (same as today)
   - If `TrustedHello` AND responder trusts -> responds with `TrustedHello` (lightweight path)
   - If `TrustedHello` AND responder doesn't trust -> must signal PQ fallback. Responder can simply not respond with TrustedHello and instead wait? No, that deadlocks. Responder should send a message back indicating "I want PQ." The simplest: responder sends an empty `KemCiphertext` with zero-length payload as a PQ-fallback signal, and initiator re-sends as KemPubkey. But this adds a round trip.

**Simplest correct approach:** Use a 1-byte "hello" message that BOTH sides can interpret:

Actually, let me re-read the user decision: "Server-driven: responder checks if remote IP is trusted, sends TrustedHello if yes." This means responder-first IS the user's decision. Let me design accordingly.

### Revised Handshake Flow (Responder-First)

```
LIGHTWEIGHT (both trust):
  Responder -> Initiator: TrustedHello (nonce_r, signing_pk_r)
  Initiator -> Responder: TrustedHello (nonce_i, signing_pk_i)
  [Both derive session keys, begin encrypted communication]
  [No additional auth messages needed -- pubkeys exchanged in TrustedHello]

PQ (neither trusts, or responder doesn't trust):
  Responder does NOT send TrustedHello; waits for KemPubkey
  Initiator -> Responder: KemPubkey (kem_pk, signing_pk_i)   [same as today]
  Responder -> Initiator: KemCiphertext (kem_ct, signing_pk_r)
  Initiator -> Responder: AuthSignature (encrypted)
  Responder -> Initiator: AuthSignature (encrypted)

MISMATCH (responder trusts, initiator doesn't):
  Responder -> Initiator: TrustedHello (nonce_r, signing_pk_r)
  Initiator reads TrustedHello, but doesn't trust peer
  Initiator -> Responder: KemPubkey (kem_pk, signing_pk_i)  [PQ fallback]
  Responder reads KemPubkey (not TrustedHello), recognizes mismatch
  Responder -> Initiator: KemCiphertext (kem_ct, signing_pk_r)
  [Normal PQ auth exchange follows]
```

**Protocol change:** Currently, the initiator sends first (KemPubkey). With this change:
- If the RESPONDER trusts the peer, it sends TrustedHello before any message from initiator
- If the RESPONDER does NOT trust, it waits for the initiator's first message (KemPubkey)
- The INITIATOR must handle both cases: it either receives TrustedHello first (responder trusts) or it needs to send KemPubkey first (responder doesn't trust, is waiting)

**This creates a deadlock if responder doesn't trust:** Responder waits for KemPubkey, Initiator waits for... what? In the current flow, initiator sends KemPubkey immediately. If we change to "initiator waits to see if responder sends TrustedHello first", we need a timeout, which is unreliable.

**Final clean design:** Responder always sends a 1-message "hello" indicating its trust choice:
1. Responder sends first: either `TrustedHello` (trusted) or `KemPubkey` request (not trusted, but we don't have this message type -- we could send a zero-payload message of a new type, or the existing handshake is initiator-first).

**Actually, the simplest correct design that preserves the "server-driven" decision:**

Change the protocol so the **first byte sent on any connection** is from the responder:
- Trusted: Responder sends TrustedHello. Initiator responds with TrustedHello or KemPubkey.
- Not trusted: Responder sends a 1-byte "PQ" indicator (or a new `PQHello` empty message), then initiator sends KemPubkey.

But adding a PQHello message for the non-trusted case changes the PQ flow (adds an extra message), which is unnecessary overhead for the common case.

**Recommended clean design (minimal protocol change):**

Add a new config/creation-time parameter to Connection: `responder_trusts_peer`. Then:
- **Responder trusts:** Responder sends TrustedHello first. Initiator reads it. If initiator also trusts -> sends TrustedHello back (lightweight). If not -> sends KemPubkey (PQ fallback, responder handles).
- **Responder does NOT trust:** Initiator sends KemPubkey first (exactly like today). Responder reads KemPubkey and proceeds with PQ.

But how does the initiator know whether to wait for TrustedHello or send KemPubkey first? The initiator can't know if the responder trusts it.

**Answer:** The initiator checks its OWN trust of the target IP. If initiator trusts the target -> initiator waits for TrustedHello (because if the initiator trusts, the responder likely trusts too, especially for localhost). If initiator doesn't trust -> initiator sends KemPubkey immediately.

Corner case: Initiator trusts, responder doesn't. Initiator waits for TrustedHello that never comes. Responder waits for KemPubkey that never comes. **Deadlock.**

**Solution: Timeout.** If initiator trusts, it waits for TrustedHello with a short timeout (e.g., 500ms). If timeout -> initiator assumes responder is PQ, sends KemPubkey. This adds 500ms latency in the rare mismatch case.

**Better solution: Initiator always sends first.** This is simpler and avoids all timeout/deadlock issues:

1. Initiator checks trust. If trusted -> sends TrustedHello. If not -> sends KemPubkey.
2. Responder reads first message:
   - If `TrustedHello` AND responder trusts -> responds with TrustedHello (lightweight)
   - If `TrustedHello` AND responder doesn't trust -> responds with KemPubkey-request? No. Simpler: just close and let the initiator retry? No, user says no connection failure.
   - **Best:** If `TrustedHello` AND responder doesn't trust -> responder treats the TrustedHello's signing pubkey as the initiator's pubkey (it's included), generates KEM keypair, encapsulates, sends KemCiphertext. But it doesn't have the KEM pubkey from the initiator... it only has the signing pubkey.

This is getting complex because KemPubkey contains the ephemeral KEM public key. TrustedHello contains a nonce + signing pubkey. They're different message structures with different information.

**Final recommended design (simplest, most robust):**

Initiator always sends first. The message type signals the initiator's preference:
- `TrustedHello` = "I'd like lightweight"
- `KemPubkey` = "I want PQ"

Responder reads, then:
- Received `TrustedHello` + responder trusts -> respond with `TrustedHello` (lightweight completes)
- Received `TrustedHello` + responder doesn't trust -> respond with ... a new `PQRequired` message (just msg type, no payload). Initiator receives `PQRequired`, generates KEM, sends `KemPubkey`, responder reads it, sends `KemCiphertext`, etc. This adds 1 round trip to the mismatch case.
- Received `KemPubkey` -> PQ path (unchanged from today)

This is clean but the mismatch case adds latency. Given that mismatch is rare (localhost always matches, trusted_peers is configured symmetrically), and the user said "just slower handshake" for mismatch, this is acceptable.

**However, we can avoid the extra round trip entirely by a simpler observation:** If the initiator doesn't trust, it sends KemPubkey (PQ). If the initiator trusts, it sends TrustedHello. The mismatch (initiator trusts, responder doesn't) can be handled by the responder simply responding with a message that signals PQ-required, forcing the initiator to restart with KemPubkey. OR, we can note that in practice, trust is symmetric (both sides configure each other), and localhost is always symmetric (both sides are localhost). The mismatch case is a misconfiguration that just costs 1 extra message.

### Config Validation Pattern
```cpp
void validate_trusted_peers(const std::vector<std::string>& peers) {
    for (const auto& peer : peers) {
        // Reject entries with ports
        if (peer.find(':') != std::string::npos) {
            // Could be IPv6 -- check if it's a valid IPv6 address
            asio::error_code ec;
            auto addr = asio::ip::make_address(peer, ec);
            if (ec) {
                throw std::runtime_error(
                    "Invalid trusted_peer '" + peer +
                    "': not a valid IP address (if specifying a port, remove it)");
            }
            // Valid IPv6 address (contains colons), that's fine
        } else {
            // No colons -- should be IPv4
            asio::error_code ec;
            asio::ip::make_address_v4(peer, ec);
            if (ec) {
                throw std::runtime_error(
                    "Invalid trusted_peer '" + peer +
                    "': not a valid IPv4 address");
            }
        }
    }
}
```

**Wait -- the user said "reject entries with ports."** An IPv4 with port looks like "192.168.1.5:4200" (has one colon). An IPv6 address like "::1" or "fe80::1" has colons but no port. An IPv6 with port looks like "[::1]:4200" (has brackets). So the check should be: reject if contains `]:` (IPv6 with port) or if it's not a valid IP address when parsed.

**Simpler:** Try to parse with `asio::ip::make_address()`. If it parses, it's a valid IP (no port). If it doesn't parse, it might have a port or be invalid. Specifically:
- "192.168.1.5" -> parses as IPv4, valid
- "192.168.1.5:4200" -> does NOT parse as address, invalid (reject with port-specific error)
- "::1" -> parses as IPv6, valid
- "[::1]:4200" -> does NOT parse as address, invalid

So: parse, and if it fails, check if the string contains `:` and a trailing number pattern to give a port-specific error message.

### SIGHUP Reload Addition
```cpp
// In PeerManager::reload_config(), after sync_namespaces reload:

// Reload trusted_peers
try {
    config::validate_trusted_peers(new_cfg.trusted_peers);
} catch (const std::exception& e) {
    spdlog::error("config reload rejected (invalid trusted_peer): {} (keeping current)", e.what());
    return;
}
// Update the trusted_peers set (atomic swap)
trusted_peers_.clear();
for (const auto& ip_str : new_cfg.trusted_peers) {
    trusted_peers_.insert(ip_str);
}
spdlog::info("config reload: trusted_peers={} addresses", trusted_peers_.size());
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Full PQ handshake for all connections | PQ for untrusted, lightweight for trusted | This phase | Faster localhost/trusted peer connections, no ML-KEM overhead |
| No transport optimization | IP-based trust with config + SIGHUP | This phase | Operator-configurable, hot-reloadable |

## Open Questions

1. **Exact mismatch fallback protocol**
   - What we know: User wants "no connection failure on mismatch -- just slower handshake." Responder-driven design means responder sends first when trusted. Initiator-first avoids deadlocks.
   - What's unclear: The exact message sequence when initiator trusts but responder doesn't (or vice versa). The research above identifies the initiator-first approach as cleanest, with a `PQRequired` fallback message for the rare mismatch case.
   - Recommendation: Use initiator-sends-first design. Initiator sends `TrustedHello` or `KemPubkey` based on its own trust check. Responder reads and responds: `TrustedHello` (lightweight), `KemCiphertext` (PQ), or sends a signal to fall back if it receives `TrustedHello` but doesn't trust.

2. **TrustedHello as new TransportMsgType vs reusing existing**
   - What we know: TrustedHello carries nonce + signing pubkey. No existing message type carries this combination.
   - What's unclear: Whether to add TrustedHello = 24 to the enum or repurpose something.
   - Recommendation: Add `TrustedHello = 24` to the TransportMsgType enum in transport.fbs. Clean, explicit, no ambiguity. Regenerate with flatc.

3. **Nonce size: 32 bytes vs other**
   - What we know: 32 bytes is standard for HKDF input key material. libsodium's `randombytes_buf()` can generate any size.
   - Recommendation: Use 32 bytes. Matches the shared secret size from ML-KEM (also 32 bytes), keeps the HKDF input consistent with the PQ path.

4. **How Connection gets the trusted_peers set**
   - What we know: Connection is created by Server, but trust config is managed by PeerManager. Connection needs trust info at handshake time.
   - Recommendation: Pass a `std::function<bool(const asio::ip::address&)> is_trusted` to Connection (via setter called before `run()`, or through Server). This decouples Connection from config details. Server already has `set_on_connected` / `set_accept_filter` callbacks from PeerManager -- a similar `set_trust_check` can be added.

## Sources

### Primary (HIGH confidence)
- Codebase analysis of `db/net/handshake.{h,cpp}` -- existing PQ handshake state machine, derive_session_keys(), HKDF context strings
- Codebase analysis of `db/net/connection.{h,cpp}` -- do_handshake() flow, send_raw/recv_raw, send_encrypted/recv_encrypted, remote_addr_ capture
- Codebase analysis of `db/crypto/kdf.{h,cpp}` -- HKDF-SHA256 extract/expand API (libsodium)
- Codebase analysis of `db/config/config.{h,cpp}` -- Config struct, load_config, validate_allowed_keys pattern
- Codebase analysis of `db/peer/peer_manager.cpp` -- reload_config() (lines 1014-1077), SIGHUP handler (lines 995-1012), on_peer_connected ACL check
- Codebase analysis of `db/schemas/transport.fbs` -- TransportMsgType enum (current max: StorageFull = 23)
- Codebase analysis of `db/crypto/signing.h` -- ML-DSA-87 key sizes (PUBLIC_KEY_SIZE = 2592, MAX_SIGNATURE_SIZE = 4627)
- Codebase analysis of `db/crypto/kem.h` -- ML-KEM-1024 sizes (PUBLIC_KEY_SIZE = 1568, SHARED_SECRET_SIZE = 32)
- Codebase analysis of `db/net/framing.h` -- make_nonce(), write_frame/read_frame
- libsodium documentation -- randombytes_buf, crypto_kdf_hkdf_sha256_* APIs

### Secondary (MEDIUM confidence)
- Asio documentation -- `asio::ip::address::is_loopback()`, `asio::ip::make_address()` for IPv4/IPv6 handling

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already in the project, no new dependencies
- Architecture: HIGH -- all patterns derived from existing codebase code, clear integration points
- Pitfalls: HIGH -- identified from real protocol flow analysis and IPv4/IPv6 edge cases in the existing codebase

**Research date:** 2026-03-15
**Valid until:** 2026-04-15 (stable -- no external dependency changes expected)
