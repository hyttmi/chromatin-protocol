# Phase 97: Protocol & Crypto Safety - Research

**Researched:** 2026-04-08
**Domain:** C++ protocol parsing hardening, AEAD nonce safety, handshake authentication
**Confidence:** HIGH

## Summary

Phase 97 is a defensive hardening phase -- no new features, no new wire types, just making existing protocol parsing and crypto handshake paths reject bad input before processing. The codebase has 7 specific requirements (PROTO-01 through PROTO-04, CRYPTO-01 through CRYPTO-03) that map cleanly to distinct code locations.

The main work areas are: (1) adding overflow-checked arithmetic helpers to `db/util/endian.h` and wiring them into all protocol decode paths, (2) adding exact pubkey size validation to `decode_auth_payload` and FlatBuffer decode in `codec.cpp`, (3) adding AD length bounds to AEAD encrypt/decrypt, (4) adding nonce exhaustion pre-check to Connection's send/recv paths, (5) verifying that PQ handshake already has pubkey binding (it does -- HandshakeResponder::verify_peer_auth checks `auth->pubkey != initiator_signing_pubkey_`), and (6) adding challenge-response authentication to the lightweight handshake.

**Primary recommendation:** Split into 3 plans: Plan 1 covers overflow helpers + protocol parsing hardening (PROTO-01 through PROTO-04), Plan 2 covers AEAD nonce exhaustion + PQ handshake verification (CRYPTO-01, CRYPTO-02), Plan 3 covers lightweight handshake authentication (CRYPTO-03). This ordering respects dependencies and keeps plan scope manageable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Overflow-checked helpers (`checked_mul`, `checked_add`) added to existing `db/util/endian.h`. Same header, same namespace (`chromatindb::util`). No new header.
- **D-02:** Return type: `std::optional<size_t>` -- returns `std::nullopt` on overflow. Composes with existing optional-returning decode functions.
- **D-03:** Overflow in protocol parsing causes the decode function to return nullopt/empty (same error path as other malformed input). No throws, no connection kills -- callers already handle nullopt.
- **D-04:** Scope covers ALL protocol parsing integer arithmetic (not just the 5 functions named in PROTO-01). Any function that reads sizes from the wire and computes offsets/allocations must use checked arithmetic.
- **D-05:** `decode_auth_payload` in `db/net/auth_helpers.h` must enforce exact pubkey size: `pk_size != Signer::PUBLIC_KEY_SIZE` returns nullopt. Cheap check before expensive sig verify.
- **D-06:** FlatBuffer decode in `db/wire/codec.cpp` must validate `pubkey.size() == Signer::PUBLIC_KEY_SIZE` after FlatBuffer verification, before passing to signature verification.
- **D-07:** Add a `max_ad_length` constant and size check in AEAD encrypt/decrypt functions. Reject if AD exceeds bound. Defense-in-depth -- current code always passes empty AD, but prevents future misuse.
- **D-08:** Kill threshold at 2^63 (half of uint64 max). Check MSB -- if set, kill connection.
- **D-09:** Pre-increment check: before incrementing counter, compare against limit. If exceeded, close connection cleanly. Zero overhead (one comparison per frame).
- **D-10:** Current code already exchanges AuthSignatures in both directions (initiator at line 435, responder at line 618 in connection.cpp). CRYPTO-02 may already be satisfied. Researcher should verify all PQ paths thoroughly to confirm no edge cases are missed before declaring done.
- **D-11:** Add challenge-response signature to the lightweight (trusted) handshake. Each side signs the session fingerprint: `SHA3-256(nonce_init || nonce_resp || pubkey_init || pubkey_resp)`. Same approach as PQ handshake's AuthSignature -- consistent across both paths.
- **D-12:** Signature exchange happens AFTER session key derivation. Signatures sent over the encrypted channel (derive keys from nonces first, then exchange encrypted AuthSignature messages). Matches PQ path structure.
- **D-13:** Per-module test files. Overflow helper tests in `test_endian.cpp`. Reject/kill path tests distributed into existing test files for each module (test_sync_protocol.cpp, test_connection.cpp, etc.). Each test lives near the code it validates. Follows Phase 95 pattern.
- **D-14:** All new validation paths must have tests that trigger the reject/kill codepath, passing under ASAN/TSAN/UBSAN.

### Claude's Discretion
- Exact overflow-checked helper signatures (as long as they return `std::optional<size_t>`)
- Max AD length constant value (reasonable bound, e.g., 64 KiB)
- Whether nonce check lives in Connection::send_encrypted or make_nonce
- How lightweight handshake auth integrates with existing derive_lightweight_session_keys flow
- Plan decomposition (how many plans, what ordering)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PROTO-01 | All integer arithmetic in protocol parsing uses overflow-checked helpers | Identified 10 specific `count * N` overflow sites across sync_protocol.cpp, reconciliation.cpp, peer_manager.cpp. Helper signatures and integration pattern documented. |
| PROTO-02 | Auth payload pubkey size validated against exact expected constant | decode_auth_payload in auth_helpers.h needs `pk_size != Signer::PUBLIC_KEY_SIZE` check at line 52 (before data extraction). 6 call sites benefit automatically. |
| PROTO-03 | FlatBuffer decode validates pubkey and data field sizes before passing to signature verification | decode_blob in codec.cpp needs `fb_blob->pubkey()->size() == Signer::PUBLIC_KEY_SIZE` check at line 44. |
| PROTO-04 | AEAD associated data length bounded | aead.cpp encrypt (line 35) and decrypt (line 72) need AD size guard. Current code always passes empty AD. |
| CRYPTO-01 | AEAD nonce counters kill connection before 2^64 | send_encrypted (line 153) and recv_encrypted (line 166) in connection.cpp need pre-increment check against 2^63. |
| CRYPTO-02 | PQ handshake initiator verifies responder pubkey binding | VERIFIED: HandshakeResponder::verify_peer_auth (handshake.cpp line 292) already checks `auth->pubkey != initiator_signing_pubkey_`. Connection.cpp PQ initiator (lines 435-440) verifies signature. All 4 PQ paths (initiator, responder, fallback-initiator, fallback-responder) have full auth exchange. |
| CRYPTO-03 | Lightweight handshake authenticates peer identity | Lightweight paths (initiator line 296-303, responder line 485-493) derive keys from nonces+pubkeys but never verify peer actually holds the signing key. Must add AuthSignature exchange after key derivation. |
</phase_requirements>

## Architecture Patterns

### Overflow-Checked Helpers

**What:** Two inline functions in `db/util/endian.h` that wrap multiplication and addition with overflow detection.

**Recommended signatures:**
```cpp
// In namespace chromatindb::util

/// Checked multiplication: returns nullopt on overflow.
inline std::optional<size_t> checked_mul(size_t a, size_t b) {
    if (a == 0 || b == 0) return 0;
    size_t result = a * b;
    if (result / a != b) return std::nullopt;
    return result;
}

/// Checked addition: returns nullopt on overflow.
inline std::optional<size_t> checked_add(size_t a, size_t b) {
    size_t result = a + b;
    if (result < a) return std::nullopt;
    return result;
}
```

**Why `size_t` not `uint64_t`:** All callers use the result for `size_t` comparisons against `payload.size()` or `vector::reserve()`. Using `size_t` avoids unnecessary casts. On 64-bit Linux, `size_t` is `uint64_t`.

**Integration pattern for decode functions:**
```cpp
// BEFORE (vulnerable):
size_t expected = 4 + count * 40;
if (payload.size() < expected) return {};

// AFTER (safe):
auto product = chromatindb::util::checked_mul(static_cast<size_t>(count), 40);
if (!product) return {};
auto expected = chromatindb::util::checked_add(4, *product);
if (!expected || payload.size() < *expected) return {};
```

### Overflow Sites Requiring Modification

**Critical decode paths (receive untrusted wire data):**

| File | Function | Line | Expression | Risk |
|------|----------|------|------------|------|
| sync_protocol.cpp | decode_namespace_list | 149 | `4 + count * 40` | uint32 * int wraps in 32-bit |
| sync_protocol.cpp | decode_blob_request | 194 | `36 + count * 32` | uint32 * int wraps in 32-bit |
| sync_protocol.cpp | decode_blob_transfer | 246 | `offset + 4`, `offset + len` | offset accumulates per-loop |
| reconciliation.cpp | decode_reconcile_ranges | 338 | `offset + entry.count * 32` | uint32 * int wraps in 32-bit |
| reconciliation.cpp | decode_reconcile_items | 385 | `HEADER_SIZE + count * 32` | uint32 * int wraps in 32-bit |
| peer_manager.cpp | decode_namespace_list | 434 | `2 + static_cast<size_t>(count) * 32` | Already casts to size_t -- safe, but should use checked_mul for consistency |

**Encode paths (use trusted local data -- lower risk but D-04 says "all"):**

| File | Function | Line | Expression |
|------|----------|------|------------|
| sync_protocol.cpp | encode_namespace_list | 133 | `4 + namespaces.size() * 40` |
| sync_protocol.cpp | encode_blob_request | 173 | `32 + 4 + hashes.size() * 32` |
| reconciliation.cpp | encode_reconcile_ranges | 270 | `36 + ranges.size() * 69` |
| reconciliation.cpp | encode_reconcile_items | 366 | `32 + 4 + items.size() * 32` |

**Response builders in message_dispatcher.cpp (use bounded local data):**

| File | Function | Line | Expression |
|------|----------|------|------------|
| message_dispatcher.cpp | handle ListResponse | 398 | `4 + count * 40 + 1` |
| message_dispatcher.cpp | handle NamespaceListResponse | 579 | `4 + 1 + entries.size() * 40` |
| message_dispatcher.cpp | handle DelegationListResponse | 814 | `4 + entry_count * 64` |
| message_dispatcher.cpp | handle TimeRangeResponse | 1067 | `5 + result_count * 48` |

**Prioritization:** Decode paths are highest priority (receive untrusted data). Encode paths and response builders are lower risk because they use data from local storage with bounded sizes, but D-04 mandates covering them too. Consider using checked_mul in encode paths only for `reserve()` calls (no behavioral change if reserve overshoots), while decode paths get strict validation.

### AEAD Nonce Exhaustion Pattern

**Current code** (connection.cpp lines 152-157):
```cpp
asio::awaitable<bool> Connection::send_encrypted(std::span<const uint8_t> plaintext) {
    auto nonce = make_nonce(send_counter_++);
    // ... encrypt and send
}
```

**Recommended change:**
```cpp
asio::awaitable<bool> Connection::send_encrypted(std::span<const uint8_t> plaintext) {
    static constexpr uint64_t NONCE_LIMIT = 1ULL << 63;
    if (send_counter_ >= NONCE_LIMIT) {
        spdlog::error("nonce exhaustion: send counter reached limit, killing connection");
        close();
        co_return false;
    }
    auto nonce = make_nonce(send_counter_++);
    // ... encrypt and send
}
```

Similarly for `recv_encrypted` with `recv_counter_`.

**Where to put the check:** In `send_encrypted` and `recv_encrypted` (not in `make_nonce`). Reason: `make_nonce` is a pure function used in framing.cpp too (write_frame/read_frame) -- adding side effects would be incorrect. Connection owns the counter lifecycle and the close() mechanism.

**Overhead:** One comparison per frame. MSB check (`counter >= (1ULL << 63)`) compiles to a single `test` instruction on x86_64.

### PQ Handshake Pubkey Binding (CRYPTO-02) - Verification

**Findings -- CRYPTO-02 is already satisfied.** All four PQ-capable paths have full bidirectional AuthSignature verification:

1. **PQ initiator** (`do_handshake_initiator_pq`, connection.cpp lines 382-445): Signs session fingerprint, sends encrypted AuthSignature. Receives and verifies responder's AuthSignature via `verify_with_offload`.

2. **PQ responder** (`do_handshake_responder_pq`, connection.cpp lines 580-639): Receives and verifies initiator's AuthSignature. Signs and sends own AuthSignature.

3. **Fallback initiator** (`do_handshake_initiator_trusted` PQ fallback, lines 306-370): Same full AuthSignature exchange when responder returns PQRequired.

4. **Fallback responder** (`do_handshake_responder_pq_fallback`, lines 500-573): Same full AuthSignature exchange.

Additionally, `HandshakeResponder::verify_peer_auth` (handshake.cpp line 292) explicitly checks `auth->pubkey != initiator_signing_pubkey_` -- verifying that the pubkey in the AuthSignature matches the one sent during KEM exchange. This is the exact "pubkey binding" check CRYPTO-02 requires.

**Recommendation:** Mark CRYPTO-02 as satisfied. Add a test that specifically validates pubkey binding by attempting to substitute a different signing key in the auth message (should fail). This documents the security invariant without changing code.

### Lightweight Handshake Authentication (CRYPTO-03)

**The vulnerability:** Current lightweight handshake (trusted path) exchanges nonces and pubkeys, derives session keys via HKDF, but never proves that either party holds the corresponding signing secret key. An attacker who intercepts the nonces and knows a target's pubkey could impersonate them.

**The fix:** After key derivation, both sides exchange encrypted AuthSignature messages -- exactly mirroring the PQ path.

**Current lightweight initiator flow (do_handshake_initiator_trusted):**
1. Generate nonce, send TrustedHello `[nonce_i:32][signing_pk:2592]`
2. Receive TrustedHello response `[nonce_r:32][signing_pk:2592]`
3. `derive_lightweight_session_keys(nonce_i, nonce_r, our_pk, peer_pk, true)`
4. Set `authenticated_ = true` -- **NO identity verification**

**Required additions after step 3:**
```
4. Sign session_fingerprint with our signing key
5. encode_auth_payload(our_pk, signature)
6. send_encrypted(TransportCodec::encode(AuthSignature, auth_payload))
7. recv_encrypted() -> decode AuthSignature
8. decode_auth_payload -> verify pubkey matches what was in TrustedHello
9. verify_with_offload(session_fingerprint, sig, pubkey)
10. Set authenticated_ = true
```

**Session fingerprint is already correct:** `derive_lightweight_session_keys` already computes `SHA3-256(nonce_init || nonce_resp || init_signing_pk || resp_signing_pk)` which matches the D-11 specification.

**Affected functions:**
- `do_handshake_initiator_trusted` (connection.cpp lines 252-376) -- add auth exchange after key derivation at line 301
- `do_handshake_responder_trusted` (connection.cpp lines 452-493) -- add auth exchange after key derivation at line 488

**Key constraint:** Initiator sends auth FIRST (matches PQ path order). Responder waits for initiator auth, verifies, then sends own auth.

**Pubkey binding for lightweight:** Must verify that the pubkey in the received AuthSignature matches the pubkey from the TrustedHello message. This prevents a MitM who relays TrustedHello but substitutes their own AuthSignature.

**Reusable code:** `encode_auth_payload`, `decode_auth_payload`, `verify_with_offload` are all available and already imported in connection.cpp.

### Auth Payload Pubkey Size Validation (PROTO-02)

**Current decode_auth_payload** (auth_helpers.h line 43-58):
```cpp
inline std::optional<AuthPayload> decode_auth_payload(std::span<const uint8_t> data) {
    if (data.size() < 4) return std::nullopt;
    uint32_t pk_size = /* LE read */;
    if (pk_size > data.size() - 4) return std::nullopt;  // bounds check
    // NO pubkey size validation against expected constant
    ...
}
```

**Required change:** Add `if (pk_size != crypto::Signer::PUBLIC_KEY_SIZE) return std::nullopt;` after the LE read and before the bounds check. This is the Step 0 pattern (cheapest check first).

**Include dependency:** auth_helpers.h will need to include `db/crypto/signing.h` for `Signer::PUBLIC_KEY_SIZE`. Check that this doesn't create a circular include. Currently auth_helpers.h has no dependency on crypto -- this adds one. Verify the include graph is clean.

### FlatBuffer Pubkey Size Validation (PROTO-03)

**Current decode_blob** (codec.cpp line 44-45):
```cpp
if (fb_blob->pubkey()) {
    result.pubkey.assign(fb_blob->pubkey()->begin(), fb_blob->pubkey()->end());
}
```

**Required change:**
```cpp
if (fb_blob->pubkey()) {
    if (fb_blob->pubkey()->size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        throw std::runtime_error("Invalid pubkey size in FlatBuffer blob");
    }
    result.pubkey.assign(fb_blob->pubkey()->begin(), fb_blob->pubkey()->end());
}
```

codec.cpp already includes `db/crypto/hash.h` but not `db/crypto/signing.h`. Will need to add this include.

### AEAD AD Length Bound (PROTO-04)

**Recommended constant:** `constexpr size_t MAX_AD_LENGTH = 65536;` (64 KiB). This is generous -- current code never uses AD at all (always empty). The bound prevents future misuse where someone accidentally passes a multi-megabyte buffer as AD.

**Location:** Add to `db/crypto/aead.h` in the `AEAD` namespace alongside `KEY_SIZE`, `NONCE_SIZE`, `TAG_SIZE`.

**Guard in encrypt and decrypt:**
```cpp
if (ad.size() > MAX_AD_LENGTH) {
    throw std::runtime_error("AEAD associated data exceeds maximum length");
}
```

This throws rather than returning nullopt because an oversized AD is a programming error (violation of an internal invariant), not a runtime decryption failure.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Overflow detection | Custom bit-manipulation | `result / a != b` division check | Simple, portable, compiler-optimized. GCC/Clang optimize this well. |
| Pubkey validation | Per-call-site size checks | Single check in decode_auth_payload | D-05: centralized, all 6 call sites benefit |
| Auth exchange in lightweight HS | Custom protocol | Reuse encode/decode_auth_payload + verify_with_offload | Already proven in PQ path |

## Common Pitfalls

### Pitfall 1: uint32_t Multiplication Wraps in 32-bit
**What goes wrong:** `count * 40` where count is `uint32_t` and `40` is `int` -- multiplication is performed in `uint32_t` (unsigned wins), wrapping at 2^32.
**Why it happens:** C++ integer promotion rules. `uint32_t * int` promotes the int to uint32_t.
**How to avoid:** Cast to `size_t` before multiplying, or use `checked_mul(static_cast<size_t>(count), 40)`.
**Warning signs:** Any `count * N` where count comes from wire data.

### Pitfall 2: Lightweight Handshake Auth Ordering
**What goes wrong:** If both sides try to send auth simultaneously, AEAD nonce ordering breaks.
**Why it happens:** Encrypted messages must be sent in counter order. Both sides sending first means both use counter=0 for their auth.
**How to avoid:** Fixed order: initiator sends auth first, responder receives then sends. Same as PQ path.
**Warning signs:** Nonce desync errors during lightweight handshake.

### Pitfall 3: Include Cycle from auth_helpers.h -> signing.h
**What goes wrong:** Adding `#include "db/crypto/signing.h"` to `auth_helpers.h` could create a cycle if signing.h transitively includes auth_helpers.h.
**Why it happens:** auth_helpers.h is in `db/net/`, signing.h is in `db/crypto/`. Cross-layer includes need checking.
**How to avoid:** Verify signing.h includes: it only includes `db/crypto/secure_bytes.h` and standard headers. No cycle possible.
**Warning signs:** Compile errors about incomplete types.

### Pitfall 4: Nonce Check on Wrong Side of Increment
**What goes wrong:** Checking `send_counter_ >= LIMIT` after incrementing means the check fires one message too late.
**Why it happens:** Post-increment (`send_counter_++`) increments first then returns old value in the expression.
**How to avoid:** Check BEFORE the post-increment: `if (send_counter_ >= LIMIT) { close(); co_return false; }` then `make_nonce(send_counter_++)`.
**Warning signs:** Off-by-one in nonce exhaustion tests.

### Pitfall 5: Pubkey Binding in Lightweight Auth
**What goes wrong:** Verifying the auth signature but not checking that the pubkey matches the one from TrustedHello allows MitM substitution.
**Why it happens:** The PQ path has this check in HandshakeResponder::verify_peer_auth (line 292) but connection.cpp's inline PQ auth code does not -- it relies on the session fingerprint binding. In the lightweight path, the same oversight could occur.
**How to avoid:** After decode_auth_payload, compare `auth->pubkey` against the pubkey extracted from TrustedHello. Reject on mismatch.
**Warning signs:** MitM test passes when it should fail.

## Code Examples

### Checked Arithmetic Integration (decode_namespace_list)
```cpp
// Source: sync_protocol.cpp decode_namespace_list
std::vector<storage::NamespaceInfo> SyncProtocol::decode_namespace_list(
    std::span<const uint8_t> payload) {
    if (payload.size() < 4) return {};

    uint32_t count = chromatindb::util::read_u32_be(payload.data());
    auto product = chromatindb::util::checked_mul(static_cast<size_t>(count), size_t{40});
    if (!product) return {};
    auto expected = chromatindb::util::checked_add(size_t{4}, *product);
    if (!expected || payload.size() < *expected) return {};

    // ... rest unchanged
}
```

### Nonce Exhaustion Check
```cpp
// Source: connection.cpp send_encrypted
asio::awaitable<bool> Connection::send_encrypted(std::span<const uint8_t> plaintext) {
    static constexpr uint64_t NONCE_LIMIT = 1ULL << 63;
    if (send_counter_ >= NONCE_LIMIT) {
        spdlog::error("nonce exhaustion on send (counter={}), killing connection {}",
                      send_counter_, remote_addr_);
        close();
        co_return false;
    }
    auto nonce = make_nonce(send_counter_++);
    std::span<const uint8_t> empty_ad{};
    auto ciphertext = crypto::AEAD::encrypt(plaintext, empty_ad, nonce, session_keys_.send_key.span());
    co_return co_await send_raw(ciphertext);
}
```

### Lightweight Handshake Auth Exchange (Initiator Side)
```cpp
// Source: connection.cpp do_handshake_initiator_trusted, after key derivation
// ... existing code through derive_lightweight_session_keys ...

// NEW: Auth exchange over encrypted channel (CRYPTO-03)
// Sign session fingerprint
auto sig = identity_.sign(session_keys_.session_fingerprint);
auto auth_payload = chromatindb::net::encode_auth_payload(identity_.public_key(), sig);
auto auth_msg = TransportCodec::encode(wire::TransportMsgType_AuthSignature, auth_payload);
if (!co_await send_encrypted(auth_msg)) {
    spdlog::warn("handshake: failed to send auth (lightweight)");
    co_return false;
}

// Receive responder's auth
auto resp_auth = co_await recv_encrypted();
if (!resp_auth) {
    spdlog::warn("handshake: failed to receive peer auth (lightweight)");
    co_return false;
}
auto resp_decoded = TransportCodec::decode(*resp_auth);
if (!resp_decoded || resp_decoded->type != wire::TransportMsgType_AuthSignature) {
    spdlog::warn("handshake: invalid auth message from peer (lightweight)");
    co_return false;
}
auto auth = chromatindb::net::decode_auth_payload(std::span{resp_decoded->payload});
if (!auth) co_return false;

// Verify pubkey matches TrustedHello
if (auth->pubkey != std::vector<uint8_t>(resp_signing_pk.begin(), resp_signing_pk.end())) {
    spdlog::warn("handshake: auth pubkey mismatch (lightweight)");
    co_return false;
}

bool valid = co_await chromatindb::crypto::verify_with_offload(
    pool_, session_keys_.session_fingerprint, auth->signature, auth->pubkey);
if (!valid) {
    spdlog::warn("handshake: peer auth signature invalid (lightweight)");
    co_return false;
}

peer_pubkey_ = std::move(auth->pubkey);
authenticated_ = true;
```

### Auth Payload Pubkey Size Validation
```cpp
// Source: auth_helpers.h decode_auth_payload
#include "db/crypto/signing.h"  // for Signer::PUBLIC_KEY_SIZE

inline std::optional<AuthPayload> decode_auth_payload(std::span<const uint8_t> data) {
    if (data.size() < 4) return std::nullopt;

    uint32_t pk_size = /* LE read */;

    // Step 0: exact pubkey size check (PROTO-02)
    if (pk_size != crypto::Signer::PUBLIC_KEY_SIZE) return std::nullopt;

    // Overflow-safe bounds check
    if (pk_size > data.size() - 4) return std::nullopt;

    // ... rest unchanged
}
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (lines 220-262) |
| Quick run command | `./build/db/chromatindb_tests "[endian]" --abort` |
| Full suite command | `./build/db/chromatindb_tests --abort` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PROTO-01 | checked_mul returns nullopt on overflow | unit | `./build/db/chromatindb_tests "[endian]" -c "checked_mul" --abort` | Extends test_endian.cpp |
| PROTO-01 | checked_add returns nullopt on overflow | unit | `./build/db/chromatindb_tests "[endian]" -c "checked_add" --abort` | Extends test_endian.cpp |
| PROTO-01 | decode_namespace_list rejects overflow count | unit | `./build/db/chromatindb_tests "[sync_protocol]" -c "overflow" --abort` | Extends test_sync_protocol.cpp |
| PROTO-01 | decode_blob_request rejects overflow count | unit | `./build/db/chromatindb_tests "[sync_protocol]" -c "overflow" --abort` | Extends test_sync_protocol.cpp |
| PROTO-01 | decode_reconcile_ranges rejects overflow count | unit | `./build/db/chromatindb_tests "[reconciliation]" -c "overflow" --abort` | Extends test_reconciliation.cpp |
| PROTO-01 | decode_reconcile_items rejects overflow count | unit | `./build/db/chromatindb_tests "[reconciliation]" -c "overflow" --abort` | Extends test_reconciliation.cpp |
| PROTO-02 | decode_auth_payload rejects wrong pubkey size | unit | `./build/db/chromatindb_tests "[auth_helpers]" -c "pubkey size" --abort` | Extends test_auth_helpers.cpp |
| PROTO-03 | decode_blob rejects wrong pubkey size | unit | `./build/db/chromatindb_tests "[codec]" -c "pubkey" --abort` | Extends test_codec.cpp |
| PROTO-04 | AEAD encrypt rejects oversized AD | unit | `./build/db/chromatindb_tests "[aead]" -c "AD" --abort` | Extends test_aead.cpp |
| PROTO-04 | AEAD decrypt rejects oversized AD | unit | `./build/db/chromatindb_tests "[aead]" -c "AD" --abort` | Extends test_aead.cpp |
| CRYPTO-01 | Connection kills on nonce exhaustion | unit | `./build/db/chromatindb_tests "[connection]" -c "nonce" --abort` | Extends test_connection.cpp |
| CRYPTO-02 | PQ handshake rejects mismatched pubkey | unit | `./build/db/chromatindb_tests "[handshake]" -c "binding" --abort` | Extends test_handshake.cpp |
| CRYPTO-03 | Lightweight handshake verifies peer identity | integration | `./build/db/chromatindb_tests "[connection][lightweight]" --abort` | Extends test_connection.cpp |

### Sampling Rate
- **Per task commit:** `cmake --build build && ./build/db/chromatindb_tests "[endian],[auth_helpers],[aead],[codec],[sync_protocol],[reconciliation],[connection],[handshake]" --abort`
- **Per wave merge:** `cmake --build build && ./build/db/chromatindb_tests --abort`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. No new test files needed, only extensions to existing files.

## Open Questions

1. **CRYPTO-02 completeness:**
   - What we know: HandshakeResponder::verify_peer_auth checks pubkey binding. Connection.cpp PQ paths verify auth signatures.
   - What's unclear: Whether the inline PQ auth in connection.cpp (used by do_handshake_initiator_pq) should ALSO check that `auth->pubkey` matches the pubkey from KemCiphertext. Currently it accepts any valid signature -- the session fingerprint binds the pubkeys indirectly since `derive_session_keys` uses both pubkeys in HKDF.
   - Recommendation: The session fingerprint binding is cryptographically sufficient (an impersonator can't sign the correct fingerprint without the correct secret key, and the fingerprint includes both pubkeys). No code change needed, but add a documenting test.

2. **decode_blob_transfer loop overflow:**
   - What we know: `offset + len` in the loop could accumulate, but both are bounded by `payload.size()` checks.
   - What's unclear: Whether `offset + 4` or `offset + len` could ever overflow size_t in practice (offset is bounded by payload.size() which is bounded by MAX_FRAME_SIZE = 110 MiB, far from SIZE_MAX).
   - Recommendation: Still add checked_add for consistency with D-04, even though the risk is theoretical.

3. **message_dispatcher.cpp response builders:**
   - What we know: These use `count * N` where count comes from local storage queries with bounded results (MAX_LIST_LIMIT, etc.).
   - What's unclear: Whether D-04's "ALL protocol parsing integer arithmetic" extends to response BUILDING (outbound) or only parsing (inbound).
   - Recommendation: Apply checked_mul to reserve() calls in response builders. If checked_mul returns nullopt, the count is absurdly large and something is wrong -- log error and skip response. Low priority compared to decode paths.

## Sources

### Primary (HIGH confidence)
- Direct code inspection of all referenced source files (connection.cpp, handshake.cpp, sync_protocol.cpp, reconciliation.cpp, auth_helpers.h, aead.cpp, codec.cpp, framing.cpp, endian.h, signing.h)
- Existing test file patterns (test_endian.cpp, test_auth_helpers.cpp, test_connection.cpp, test_aead.cpp)
- CONTEXT.md decisions from user discussion session

### Secondary (MEDIUM confidence)
- C++ integer promotion rules (ISO C++ standard, well-known behavior)
- ChaCha20-Poly1305 IETF nonce construction (RFC 8439)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new libraries, extending existing code only
- Architecture: HIGH - patterns directly observable in current codebase
- Pitfalls: HIGH - overflow sites verified by direct code inspection
- CRYPTO-02 verification: HIGH - all 4 PQ paths inspected line-by-line

**Research date:** 2026-04-08
**Valid until:** 2026-05-08 (stable - no external dependency changes)
