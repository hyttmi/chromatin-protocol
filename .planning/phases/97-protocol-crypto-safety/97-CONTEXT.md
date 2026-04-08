# Phase 97: Protocol & Crypto Safety - Context

**Gathered:** 2026-04-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Harden all protocol parsing paths to reject malformed input before processing, and enforce identity binding in all cryptographic handshake paths. This is defensive hardening -- no new features, no new wire types, no behavioral changes beyond rejecting bad input and authenticating peers.

Requirements: PROTO-01, PROTO-02, PROTO-03, PROTO-04, CRYPTO-01, CRYPTO-02, CRYPTO-03

</domain>

<decisions>
## Implementation Decisions

### Overflow-Checked Arithmetic (PROTO-01)
- **D-01:** Overflow-checked helpers (`checked_mul`, `checked_add`) added to existing `db/util/endian.h`. Same header, same namespace (`chromatindb::util`). No new header.
- **D-02:** Return type: `std::optional<size_t>` -- returns `std::nullopt` on overflow. Composes with existing optional-returning decode functions.
- **D-03:** Overflow in protocol parsing causes the decode function to return nullopt/empty (same error path as other malformed input). No throws, no connection kills -- callers already handle nullopt.
- **D-04:** Scope covers ALL protocol parsing integer arithmetic (not just the 5 functions named in PROTO-01). Any function that reads sizes from the wire and computes offsets/allocations must use checked arithmetic.

### Auth Payload & Pubkey Validation (PROTO-02, PROTO-03)
- **D-05:** `decode_auth_payload` in `db/net/auth_helpers.h` must enforce exact pubkey size: `pk_size != Signer::PUBLIC_KEY_SIZE` returns nullopt. Cheap check before expensive sig verify.
- **D-06:** FlatBuffer decode in `db/wire/codec.cpp` must validate `pubkey.size() == Signer::PUBLIC_KEY_SIZE` after FlatBuffer verification, before passing to signature verification.

### AEAD Associated Data Bounds (PROTO-04)
- **D-07:** Add a `max_ad_length` constant and size check in AEAD encrypt/decrypt functions. Reject if AD exceeds bound. Defense-in-depth -- current code always passes empty AD, but prevents future misuse.

### AEAD Nonce Exhaustion (CRYPTO-01)
- **D-08:** Kill threshold at 2^63 (half of uint64 max). Check MSB -- if set, kill connection.
- **D-09:** Pre-increment check: before incrementing counter, compare against limit. If exceeded, close connection cleanly. Zero overhead (one comparison per frame).

### PQ Handshake Pubkey Binding (CRYPTO-02)
- **D-10:** Current code already exchanges AuthSignatures in both directions (initiator at line 435, responder at line 618 in connection.cpp). CRYPTO-02 may already be satisfied. Researcher should verify all PQ paths thoroughly to confirm no edge cases are missed before declaring done.

### Lightweight Handshake Authentication (CRYPTO-03)
- **D-11:** Add challenge-response signature to the lightweight (trusted) handshake. Each side signs the session fingerprint: `SHA3-256(nonce_init || nonce_resp || pubkey_init || pubkey_resp)`. Same approach as PQ handshake's AuthSignature -- consistent across both paths.
- **D-12:** Signature exchange happens AFTER session key derivation. Signatures sent over the encrypted channel (derive keys from nonces first, then exchange encrypted AuthSignature messages). Matches PQ path structure.

### Test Organization
- **D-13:** Per-module test files. Overflow helper tests in `test_endian.cpp`. Reject/kill path tests distributed into existing test files for each module (test_sync_protocol.cpp, test_connection.cpp, etc.). Each test lives near the code it validates. Follows Phase 95 pattern.
- **D-14:** All new validation paths must have tests that trigger the reject/kill codepath, passing under ASAN/TSAN/UBSAN.

### Claude's Discretion
- Exact overflow-checked helper signatures (as long as they return `std::optional<size_t>`)
- Max AD length constant value (reasonable bound, e.g., 64 KiB)
- Whether nonce check lives in Connection::send_encrypted or make_nonce
- How lightweight handshake auth integrates with existing derive_lightweight_session_keys flow
- Plan decomposition (how many plans, what ordering)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Protocol Parsing (overflow targets)
- `db/sync/sync_protocol.cpp` -- decode_namespace_list (line 149: `count * 40` overflow), decode_blob_request (line 194: `count * 32` overflow), decode_blob_transfer, encode_namespace_list
- `db/sync/reconciliation.cpp` -- integer arithmetic in reconciliation decode paths
- `db/wire/codec.cpp` -- FlatBuffer decode + pubkey size validation gap (line 32-57)
- `db/net/framing.cpp` -- frame length parsing (outermost wire boundary)
- `db/net/connection.cpp` -- handshake paths, nonce counters (lines 182-183)

### Crypto & Handshake
- `db/net/connection.h` -- send_counter_/recv_counter_ (lines 182-183), nonce exhaustion target
- `db/net/connection.cpp` -- PQ handshake initiator (lines 382-445), PQ responder (lines 580-639), lightweight initiator (lines 252-376), lightweight responder (lines 452-493)
- `db/net/auth_helpers.h` -- decode_auth_payload (line 43-58), missing exact pubkey size check
- `db/net/framing.cpp` -- make_nonce function (lines 10-16)
- `db/crypto/aead.cpp` -- encrypt/decrypt with empty AD (lines 35, 72), AD bounds check target
- `db/crypto/signing.h` -- Signer::PUBLIC_KEY_SIZE constant

### Phase 95 Utilities (reuse/extend)
- `db/util/endian.h` -- existing BE helpers, extend with checked_mul/checked_add
- `db/crypto/verify_helpers.h` -- verify_with_offload (used in handshake auth)
- `db/net/auth_helpers.h` -- encode/decode_auth_payload (reuse in lightweight handshake)

### Requirements
- `.planning/REQUIREMENTS.md` -- PROTO-01 through PROTO-04, CRYPTO-01 through CRYPTO-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/util/endian.h` -- existing span-checked read helpers (`read_u32_be(span)` throws on underflow). Extend with `checked_mul`/`checked_add`.
- `db/net/auth_helpers.h` -- encode/decode_auth_payload already shared. Reuse for lightweight handshake AuthSignature.
- `db/crypto/verify_helpers.h` -- `verify_with_offload(pool, data, sig, pk)` coroutine. Reuse for lightweight handshake sig verification.
- `db/tests/test_helpers.h` -- TempDir, make_signed_blob, listening_address for test setup.

### Established Patterns
- Span overloads throw `std::out_of_range`; pointer overloads unchecked (Phase 95)
- Decode functions return `std::optional` or empty containers on malformed input
- `co_await crypto::offload(pool_, lambda)` for CPU-heavy crypto work
- Auth payload uses LE encoding for pubkey_size (protocol-defined, never change)
- Session fingerprint = SHA3-256(nonce_init || nonce_resp || pubkey_init || pubkey_resp)

### Integration Points
- 5+ decode functions in sync_protocol.cpp need overflow-checked arithmetic
- decode_auth_payload needs exact pubkey size check (all 4+ handshake call sites benefit)
- AEAD encrypt/decrypt need AD bounds check (db/crypto/aead.cpp)
- Connection send/recv paths need nonce pre-increment check
- Lightweight handshake paths need AuthSignature exchange after key derivation

</code_context>

<specifics>
## Specific Ideas

- Nonce check: compare MSB (if counter >= (1ULL << 63)) is the cheapest possible check
- Lightweight handshake should mirror PQ handshake structure: derive keys, then exchange encrypted AuthSignature
- CRYPTO-02 may already be done -- researcher must verify all PQ paths before planning

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 97-protocol-crypto-safety*
*Context gathered: 2026-04-08*
