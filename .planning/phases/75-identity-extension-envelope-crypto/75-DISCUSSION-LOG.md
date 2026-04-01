# Phase 75: Identity Extension & Envelope Crypto - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-01
**Phase:** 75-identity-extension-envelope-crypto
**Areas discussed:** DEK wrap nonce, decryption errors, envelope constants, identity KEM files, AEAD AD scope, recipient lookup, KEM constructor, test vectors, stanza ordering, envelope API, recipient cap, liboqs validation, DEK wrap AD, duplicate recipients, decrypt API, return type, HKDF salt, KEM storage, version semantics, generate determinism, KEM file format, sender inclusion, test organization

---

## DEK Wrap Nonce Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Zero nonce | KEM shared secret unique per encap, KEK always unique. Saves 12 bytes/stanza. age/CMS-KEM pattern | ✓ |
| Random 12-byte wrap nonce | Explicit nonce per stanza. Belt-and-suspenders but 12 extra bytes per recipient | |

**User's choice:** Zero nonce
**Notes:** Safe because KEK is unique per encapsulation. Code comment required.

---

## Decryption Error Behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Typed exceptions | DecryptionError, NotARecipientError, MalformedEnvelopeError under CryptoError | ✓ |
| Return None | Follow aead_decrypt pattern. Loses failure reason | |
| Mixed: exceptions for structure, None for AEAD | Raise for structure/stanza, None for AEAD tag | |

**User's choice:** Typed exceptions
**Notes:** Matches existing identity.py error pattern (KeyFileError, SignatureError)

---

## Envelope Magic Bytes

| Option | Description | Selected |
|--------|-------------|----------|
| 0x43454E56 'CENV' | Chromatin ENVelope. Mnemonic prefix | ✓ |
| 0x454E5631 'ENV1' | Version in magic, redundant with version byte | |
| 0xCDB0E000 | Binary prefix, less readable | |

**User's choice:** CENV

---

## Cipher Suite Byte

| Option | Description | Selected |
|--------|-------------|----------|
| 0x01 | Suite 1 = ML-KEM-1024 + ChaCha20-Poly1305 + HKDF-SHA256. 0x00 reserved | ✓ |
| 0x00 | Start at zero. Can't sentinel for invalid | |

**User's choice:** 0x01

---

## Identity KEM File Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Hard fail on missing .kem | load() requires 4 files. No backward compat | ✓ |
| Soft fail: signing-only if .kem missing | Flexible but contradicts prior decision | |

**User's choice:** Hard fail
**Notes:** from_public_key() stays for verify-only peers

---

## AEAD Associated Data Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Full header including stanzas | AD = everything before ciphertext. Prevents stanza substitution | ✓ |
| Fixed header only (20 bytes) | Stanzas not authenticated. MITM could swap stanzas | |

**User's choice:** Full header including stanzas

---

## Recipient Lookup Key

| Option | Description | Selected |
|--------|-------------|----------|
| SHA3-256(kem_pk) | Consistent with namespace = SHA3-256(signing_pk) | ✓ |
| First 32 bytes of kem_pk | Leaks partial key material | |

**User's choice:** SHA3-256(kem_pk)

---

## KEM Public Key Constructor

| Option | Description | Selected |
|--------|-------------|----------|
| Add from_public_keys() in Phase 75 | _envelope.py uses Identity objects. Phase 76 uses immediately | ✓ |
| Defer to Phase 76 | _envelope.py takes raw bytes | |

**User's choice:** Add now

---

## Cross-SDK Test Vectors

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, Python generator script | JSON vectors in tools/envelope_test_vectors.py | ✓ |
| No, defer to Phase 78 | Documentation artifact | |
| You decide | Claude's discretion | |

**User's choice:** Yes, Python generator script

---

## Stanza Ordering

| Option | Description | Selected |
|--------|-------------|----------|
| Sorted by kem_pk_hash | Deterministic output, O(log N) binary search | ✓ |
| Insertion order | Simpler, O(N) linear scan | |

**User's choice:** Sorted

---

## Envelope API: Self-Encrypt

| Option | Description | Selected |
|--------|-------------|----------|
| Support in Phase 75 | Empty recipients = encrypt to sender. _envelope.py feature-complete | ✓ |
| Require at least 1 recipient | Phase 77 handles self-encrypt | |

**User's choice:** Support in Phase 75

---

## Recipient Count Limit

| Option | Description | Selected |
|--------|-------------|----------|
| Cap at 256 | ~412 KiB header max | ✓ |
| Cap at 1000 | ~1.6 MiB header | |
| No cap | Trust caller, uint16 limit | |

**User's choice:** 256

---

## liboqs Key Validation

| Option | Description | Selected |
|--------|-------------|----------|
| Size check + trust liboqs | 1568-byte check, code comment on FIPS 203 gap | ✓ |
| Size check + manual modulus check | Parse ML-KEM internals | |

**User's choice:** Size check + trust liboqs

---

## DEK Wrapping AD (Circular Dependency Resolution)

| Option | Description | Selected |
|--------|-------------|----------|
| Partial header: exclude wrapped_dek | AD = fixed prefix + kem_pk_hash + kem_ct per stanza. No circular dep | ✓ |
| Fall back to kem_pk_hash only | Simpler, data AEAD authenticates everything anyway | |

**User's choice:** Partial header

---

## Duplicate Recipient Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Silently deduplicate | Dedup by kem_pk_hash before encryption | ✓ |
| Raise ValueError | Treat as caller bug | |

**User's choice:** Silently deduplicate

---

## envelope_decrypt() Input

| Option | Description | Selected |
|--------|-------------|----------|
| bytes + Identity | Consistent with encrypt API. Identity provides KEM key + hash | ✓ |
| bytes + raw kem_sk + kem_pk | Lower-level, more flexible | |

**User's choice:** bytes + Identity

---

## Return Type from Decrypt

| Option | Description | Selected |
|--------|-------------|----------|
| Just bytes | Simple. Optional envelope_parse() for metadata | ✓ |
| DecryptedEnvelope dataclass | Richer but heavier for common case | |

**User's choice:** Just bytes

---

## HKDF Salt for KEK Derivation

| Option | Description | Selected |
|--------|-------------|----------|
| Empty salt | Consistent with transport + DARE. IKM is full-entropy KEM shared secret | ✓ |
| Salt = kem_pk_hash | Extra binding but breaks consistency | |

**User's choice:** Empty salt
**Notes:** User asked why transport uses empty salt. Explanation: ML-KEM shared secret has full 256-bit entropy, salt adds nothing per RFC 5869. Domain separation via info label. All three HKDF domains (transport, DARE, envelope) consistently use empty salt.

---

## KEM Key Storage in Identity

| Option | Description | Selected |
|--------|-------------|----------|
| oqs.KeyEncapsulation object | Mirrors oqs.Signature for signing. Object holds SK internally | ✓ |
| Raw bytes + lazy construction | Store bytes, construct on-demand | |

**User's choice:** oqs.KeyEncapsulation object

---

## Version Byte Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| MalformedEnvelopeError | Reuse existing exception with clear message | ✓ |
| New UnsupportedVersionError | Distinct subclass of MalformedEnvelopeError | |

**User's choice:** MalformedEnvelopeError

---

## Identity.generate() Determinism

| Option | Description | Selected |
|--------|-------------|----------|
| No seed parameter | System randomness only. Tests validate properties not bytes | ✓ |
| Optional seed for tests | Seeded keygen for deterministic tests | |

**User's choice:** No seed parameter

---

## KEM Key File Format

| Option | Description | Selected |
|--------|-------------|----------|
| Raw binary, same convention | .kem=3168B, .kpub=1568B. Constants in identity.py | ✓ |
| Different suffix convention | .enc/.enc.pub | |

**User's choice:** Raw binary, same convention

---

## Sender Auto-Inclusion

| Option | Description | Selected |
|--------|-------------|----------|
| Auto-include sender | Prevents lockout. Dedup handles doubles | ✓ |
| Caller decides | More control but easy lockout | |

**User's choice:** Auto-include sender

---

## Test File Organization

| Option | Description | Selected |
|--------|-------------|----------|
| New test_envelope.py + extend test_identity.py | One-file-per-module convention | ✓ |
| Single test_envelope.py | All Phase 75 tests in one file | |

**User's choice:** New test_envelope.py + extend test_identity.py

---

## Claude's Discretion

- Internal _envelope.py module structure and helper functions
- Exact test case selection and assertion patterns
- Whether envelope_parse() is a separate function or a classmethod
- Code comment wording

## Deferred Ideas

None -- discussion stayed within phase scope
