---
phase: 75-identity-extension-envelope-crypto
verified: 2026-04-01T19:00:00Z
status: passed
score: 12/12 must-haves verified
re_verification: false
---

# Phase 75: Identity Extension & Envelope Crypto Verification Report

**Phase Goal:** Users can generate encryption-capable identities and encrypt/decrypt blob data for one or more PQ recipients
**Verified:** 2026-04-01T19:00:00Z
**Status:** passed
**Re-verification:** No -- initial verification

## Goal Achievement

### Observable Truths

| #  | Truth                                                                                 | Status     | Evidence                                                              |
|----|---------------------------------------------------------------------------------------|------------|-----------------------------------------------------------------------|
| 1  | Identity.generate() produces both ML-DSA-87 signing and ML-KEM-1024 encryption keypairs | VERIFIED | generate() calls oqs.Signature("ML-DSA-87") + oqs.KeyEncapsulation("ML-KEM-1024"), smoke check passes |
| 2  | Identity can save/load 4 key files (.key/.pub/.kem/.kpub) and round-trip correctly    | VERIFIED   | save() writes 4 files; load() reads and size-validates all 4; 4 roundtrip tests pass |
| 3  | Identity exposes kem_public_key property and has_kem property                         | VERIFIED   | Both properties implemented, has_kem checks _kem_public_key is not None, kem_public_key raises KeyFileError when absent |
| 4  | from_public_keys(signing_pk, kem_pk) creates an encrypt-capable verify-only identity  | VERIFIED   | Classmethod exists, validates both key sizes, sets has_kem=True, can_sign=False |
| 5  | Existing signing-only code paths (from_public_key, sign, verify) are unbroken         | VERIFIED   | Full 411-test suite passes with no regressions |
| 6  | envelope_encrypt() encrypts arbitrary data for multiple PQ recipients and returns a self-describing binary envelope | VERIFIED | CENV magic, version, suite, sorted stanzas, AEAD ciphertext verified in tests and smoke checks |
| 7  | envelope_decrypt() finds recipient stanza by KEM pubkey hash, decapsulates, unwraps DEK, and returns original plaintext | VERIFIED | Binary search via bisect.bisect_left, KEM decap, HKDF KEK, AEAD unwrap all implemented and tested |
| 8  | Sender is always auto-included as recipient (cannot lock yourself out)                | VERIFIED   | Sender prepended to all_recipients list before dedup; test_sender_auto_included passes |
| 9  | Non-recipient identity raises NotARecipientError when attempting decrypt              | VERIFIED   | Binary search miss raises NotARecipientError; behavioral spot-check passes |
| 10 | Tampered header or ciphertext raises DecryptionError (AEAD tag verification fails)    | VERIFIED   | aead_decrypt returns None on failure, code checks and raises DecryptionError; spot-check passes |
| 11 | Malformed envelope (bad magic, wrong version, truncated) raises MalformedEnvelopeError | VERIFIED  | All three error paths implemented and tested; spot-check confirms bad magic raises correctly |
| 12 | envelope_parse() returns metadata (version, suite, recipient_count) without decrypting | VERIFIED  | Separate function stops at fixed header; test_parse_returns_metadata passes |

**Score:** 12/12 truths verified

### Required Artifacts

| Artifact                                              | Expected                                       | Status     | Details                                         |
|-------------------------------------------------------|------------------------------------------------|------------|-------------------------------------------------|
| `sdk/python/chromatindb/exceptions.py`                | NotARecipientError and MalformedEnvelopeError  | VERIFIED   | Both classes present under CryptoError (lines 39-44) |
| `sdk/python/chromatindb/identity.py`                  | ML-KEM-1024 keypair generation, file I/O, KEM properties | VERIFIED | 314 lines, KEM_PUBLIC_KEY_SIZE=1568, all methods present |
| `sdk/python/tests/test_identity.py`                   | KEM keypair tests for generate, save, load, from_public_keys | VERIFIED | 298 lines, 29 test functions |
| `sdk/python/chromatindb/_envelope.py`                 | PQ envelope encrypt/decrypt/parse with KEM-then-Wrap | VERIFIED | 283 lines, all three functions exported |
| `sdk/python/tests/test_envelope.py`                   | Comprehensive envelope encrypt/decrypt/format tests | VERIFIED | 450 lines, 31 test functions |
| `sdk/python/tests/vectors/envelope_vectors.json`      | Cross-SDK test vectors for envelope format interop | VERIFIED | Valid JSON, 3 vectors with all required fields |
| `tools/envelope_test_vectors.py`                      | Python script to regenerate test vectors       | VERIFIED   | 114 lines at /home/mika/dev/chromatin-protocol/tools/envelope_test_vectors.py |

### Key Link Verification

| From                                       | To                                     | Via                                      | Status   | Details                                           |
|--------------------------------------------|----------------------------------------|------------------------------------------|----------|---------------------------------------------------|
| `sdk/python/chromatindb/identity.py`       | `oqs.KeyEncapsulation`                 | KEM keypair generation in generate()     | WIRED    | `oqs.KeyEncapsulation("ML-KEM-1024")` at line 65 |
| `sdk/python/chromatindb/identity.py`       | `sdk/python/chromatindb/exceptions.py` | KeyFileError on missing/invalid KEM files | WIRED   | 14+ raise KeyFileError sites for KEM paths |
| `sdk/python/chromatindb/_envelope.py`      | `sdk/python/chromatindb/crypto.py`     | aead_encrypt/aead_decrypt for DEK wrapping | WIRED  | `from chromatindb.crypto import AEAD_TAG_SIZE, aead_decrypt, aead_encrypt, sha3_256` |
| `sdk/python/chromatindb/_envelope.py`      | `sdk/python/chromatindb/_hkdf.py`      | hkdf_derive for KEK derivation           | WIRED    | `from chromatindb._hkdf import hkdf_derive` + used at lines 122 and 220 |
| `sdk/python/chromatindb/_envelope.py`      | `sdk/python/chromatindb/identity.py`   | Identity.kem_public_key for recipient lookup | WIRED | `identity.kem_public_key` used at line 204; `r.kem_public_key` used in encrypt loop |
| `sdk/python/chromatindb/_envelope.py`      | `sdk/python/chromatindb/exceptions.py` | Raises DecryptionError, NotARecipientError, MalformedEnvelopeError | WIRED | All three raised at multiple sites (lines 165-240) |
| `sdk/python/tests/test_envelope.py`        | `sdk/python/tests/vectors/envelope_vectors.json` | load_vectors for cross-SDK format validation | WIRED | `load_vectors("envelope_vectors.json")` at line 40 |
| `sdk/python/chromatindb/__init__.py`       | `sdk/python/chromatindb/_envelope.py`  | Re-exports envelope_encrypt/decrypt/parse | WIRED  | All three in imports and __all__ |
| `sdk/python/chromatindb/__init__.py`       | `sdk/python/chromatindb/identity.py`   | Re-exports KEM_PUBLIC_KEY_SIZE, KEM_SECRET_KEY_SIZE | WIRED | Both in import at line 56 and __all__ |
| `sdk/python/chromatindb/__init__.py`       | `sdk/python/chromatindb/exceptions.py` | Re-exports MalformedEnvelopeError, NotARecipientError | WIRED | Both in import at lines 49-50 and __all__ |

### Data-Flow Trace (Level 4)

Not applicable. No dynamic data rendering -- all artifacts are cryptographic primitives and utilities, not components that render from a data source. Data flows directly through function parameters and return values, which are verified by the test suite.

### Behavioral Spot-Checks

| Behavior                                      | Command                                     | Result                                   | Status  |
|-----------------------------------------------|---------------------------------------------|------------------------------------------|---------|
| Non-recipient raises NotARecipientError        | Python spot-check (inline)                  | NotARecipientError raised correctly       | PASS    |
| Tampered ciphertext raises DecryptionError     | Python spot-check (inline)                  | DecryptionError raised correctly          | PASS    |
| Bad magic raises MalformedEnvelopeError        | Python spot-check (inline)                  | MalformedEnvelopeError raised correctly   | PASS    |
| Sender auto-included and can decrypt           | Python spot-check (inline)                  | Plaintext recovered correctly             | PASS    |
| HKDF domain label is correct                   | Python spot-check (inline)                  | _HKDF_LABEL == b"chromatindb-envelope-kek-v1" | PASS |
| Full test suite                                | pytest -x -q (411 tests)                    | 411 passed, 0 failed                      | PASS    |

### Requirements Coverage

| Requirement | Source Plan | Description                                                                       | Status    | Evidence                                                                   |
|-------------|-------------|-----------------------------------------------------------------------------------|-----------|----------------------------------------------------------------------------|
| IDENT-01    | 75-01-PLAN  | User can generate identity with both ML-DSA-87 and ML-KEM-1024 keypairs           | SATISFIED | Identity.generate() creates both; test_generate_kem_keys passes            |
| IDENT-02    | 75-01-PLAN  | User can save and load identity with .key/.pub/.kem/.kpub files                   | SATISFIED | save() writes 4 files, load() reads/validates all 4; roundtrip test passes |
| IDENT-03    | 75-01-PLAN  | Identity exposes KEM public key for directory publishing and encryption            | SATISFIED | kem_public_key and has_kem properties; from_public_keys() constructor      |
| ENV-01      | 75-02-PLAN  | SDK encrypts blob data with random per-blob DEK using ChaCha20-Poly1305           | SATISFIED | dek = secrets.token_bytes(32); aead_encrypt(plaintext, ad=full_header, nonce=data_nonce, key=dek) |
| ENV-02      | 75-02-PLAN  | SDK wraps DEK per-recipient via ML-KEM-1024 encapsulation + HKDF-derived KEK     | SATISFIED | kem.encap_secret(); hkdf_derive(salt=b"", ikm=kem_ss, info=_HKDF_LABEL); aead_encrypt(dek, ..., key=kek) |
| ENV-03      | 75-02-PLAN  | Encrypted blob uses versioned binary format (magic, version, suite, stanzas, ciphertext) | SATISFIED | CENV + 0x01 + 0x01 + BE count + nonce + stanzas + ciphertext; byte-level format tests pass |
| ENV-04      | 75-02-PLAN  | Envelope header is authenticated as AEAD associated data (prevents stanza substitution) | SATISFIED | full_header used as AD for data encryption; test_stanza_substitution_fails passes |
| ENV-05      | 75-02-PLAN  | Recipient can decrypt by finding stanza, decapsulating, unwrapping DEK           | SATISFIED | bisect_left stanza lookup + identity._kem.decap_secret() + HKDF + aead_decrypt |
| ENV-06      | 75-02-PLAN  | HKDF uses unique domain label "chromatindb-envelope-kek-v1"                      | SATISFIED | _HKDF_LABEL = b"chromatindb-envelope-kek-v1" at line 41; test_hkdf_domain_label_in_source passes |

**All 9 phase requirements satisfied. No orphaned requirements.**

REQUIREMENTS.md traceability table marks all 9 requirements (IDENT-01 through ENV-06) as Complete/Phase 75.

### Anti-Patterns Found

None. Scan of identity.py and _envelope.py found:
- No TODO/FIXME/PLACEHOLDER comments
- No empty implementations (return null/return {}/return [])
- No stub handlers
- The zero-nonce for DEK wrapping is intentional and documented with an inline comment explaining safety (unique KEK per encapsulation makes nonce reuse safe)

### Human Verification Required

None. All must-haves are fully verifiable programmatically. The cryptographic correctness is validated by:
1. The 411-test suite (60 tests for identity + envelope specifically)
2. Cross-SDK test vectors in envelope_vectors.json (3 vectors with known key material and expected plaintext)
3. Behavioral spot-checks confirming correct exception types

### Gaps Summary

No gaps found. All phase requirements are satisfied, all artifacts exist and are substantive, all key links are wired, and the full test suite passes cleanly.

---

_Verified: 2026-04-01T19:00:00Z_
_Verifier: Claude (gsd-verifier)_
