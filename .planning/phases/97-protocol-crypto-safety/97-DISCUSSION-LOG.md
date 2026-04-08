# Phase 97: Protocol & Crypto Safety - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-08
**Phase:** 97-protocol-crypto-safety
**Areas discussed:** Overflow helper design, Nonce exhaustion policy, Lightweight handshake auth, PQ pubkey binding, AEAD AD bounds, Overflow scope breadth, Test organization

---

## Overflow Helper Design

### Location

| Option | Description | Selected |
|--------|-------------|----------|
| Extend endian.h | Add safe_mul/safe_add to existing db/util/endian.h alongside BE helpers | |
| New db/util/overflow.h | Separate header for checked arithmetic | |
| Inline in each decode site | No shared helper, each function does its own check | |

**User's choice:** Extend endian.h (Recommended)
**Notes:** Keeps one header for all wire-level arithmetic. Sites already include endian.h.

### Error Mode

| Option | Description | Selected |
|--------|-------------|----------|
| Return nullopt/empty | Same error path as other malformed input | |
| Throw std::overflow_error | Matches span-overload pattern from Phase 95 | |
| Kill the connection | Treat overflow as malicious | |

**User's choice:** Return nullopt/empty (Recommended)
**Notes:** Decode functions already return optional/empty on malformed input. Consistent.

### API Style

| Option | Description | Selected |
|--------|-------------|----------|
| bool safe_mul(a, b, &result) | C-style, returns false on overflow | |
| std::optional<size_t> checked_mul(a, b) | Modern C++, returns nullopt on overflow | |
| You decide | Claude picks | |

**User's choice:** std::optional<size_t> checked_mul(a, b)
**Notes:** Composes with existing optional-returning decode functions.

### Pubkey Size Validation

| Option | Description | Selected |
|--------|-------------|----------|
| Exact match required | Reject if pubkey.size() != Signer::PUBLIC_KEY_SIZE | |
| Reasonable range check | Accept 32-8192 bytes | |
| No extra validation | Trust FlatBuffer + let sig verify fail naturally | |

**User's choice:** Exact match required (Recommended)
**Notes:** Cheap check, catches garbage before expensive sig verify.

---

## Nonce Exhaustion Policy

### Threshold

| Option | Description | Selected |
|--------|-------------|----------|
| 2^63 | Half of uint64 max, check MSB | |
| 2^48 | More conservative, ~8.9 years at 1M msg/sec | |
| UINT64_MAX - 1 | Maximum possible usage | |

**User's choice:** 2^63 (Recommended)
**Notes:** Astronomically unreachable but required for correctness.

### Implementation

| Option | Description | Selected |
|--------|-------------|----------|
| Pre-increment check | Compare before incrementing, kill if exceeded | |
| Saturating counter | Counter saturates at limit, encrypt returns failure | |
| You decide | Claude picks | |

**User's choice:** Pre-increment check (Recommended)
**Notes:** Simple, correct, zero overhead.

---

## Lightweight Handshake Auth

### Auth Level

| Option | Description | Selected |
|--------|-------------|----------|
| Challenge-response signature | Sign session fingerprint with ML-DSA-87 | |
| Mutual nonce signing | Sign other side's nonce | |
| Keep unauthenticated | Rely on UDS/IP trust | |
| Remove lightweight path | All connections use PQ handshake | |

**User's choice:** Challenge-response signature
**Notes:** Proves possession of private key. Adds ~2ms per handshake.

### Signed Data

| Option | Description | Selected |
|--------|-------------|----------|
| Session fingerprint | SHA3-256(nonce_init || nonce_resp || pubkey_init || pubkey_resp) | |
| Peer's nonce only | Simpler but weaker binding | |
| You decide | Claude picks strongest | |

**User's choice:** Session fingerprint (Recommended)
**Notes:** Same approach as PQ handshake. Consistent, strongly binding.

### Ordering

| Option | Description | Selected |
|--------|-------------|----------|
| After key derivation | Derive keys first, exchange sigs over encrypted channel | |
| Before key derivation | Exchange sigs in plaintext alongside hello | |
| You decide | Claude picks | |

**User's choice:** After key derivation (Recommended)
**Notes:** Signatures protected in transit. Matches PQ path structure.

---

## PQ Pubkey Binding

**Finding:** PQ handshake already exchanges AuthSignatures in both directions (initiator verifies responder at line 435, responder verifies initiator at line 618 in connection.cpp). CRYPTO-02 may already be satisfied.

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, CRYPTO-02 is done | Existing code already does mutual pubkey binding | |
| Verify more thoroughly first | Have researcher dig deeper into all PQ paths | |

**User's choice:** Verify more thoroughly first
**Notes:** Want researcher to confirm no edge cases before declaring done.

---

## AEAD AD Bounds (PROTO-04)

| Option | Description | Selected |
|--------|-------------|----------|
| Add max AD check anyway | Defense-in-depth, prevents future misuse | |
| Document and skip | AD is always empty, defer the check | |
| You decide | Claude picks | |

**User's choice:** Add max AD check anyway (Recommended)
**Notes:** Cheap defense-in-depth.

---

## Overflow Scope Breadth

| Option | Description | Selected |
|--------|-------------|----------|
| All protocol parsing | Any function reading sizes from wire and computing offsets | |
| Strictly the 5 named | Only the 5 functions in PROTO-01 | |
| Named 5 + framing | 5 named plus frame length parsing | |

**User's choice:** All protocol parsing (Recommended)
**Notes:** Comprehensive coverage.

---

## Test Organization

| Option | Description | Selected |
|--------|-------------|----------|
| Per-module test files | Tests distributed into existing module test files | |
| Single test_protocol_safety.cpp | One new file for all hardening tests | |
| You decide | Claude organizes as fits | |

**User's choice:** Per-module test files (Recommended)
**Notes:** Each test lives near the code it validates. Follows Phase 95 pattern.

---

## Claude's Discretion

- Exact overflow-checked helper signatures (as long as they return std::optional<size_t>)
- Max AD length constant value
- Nonce check placement (Connection::send_encrypted vs make_nonce)
- Lightweight handshake auth integration with derive_lightweight_session_keys
- Plan decomposition

## Deferred Ideas

None -- discussion stayed within phase scope.
