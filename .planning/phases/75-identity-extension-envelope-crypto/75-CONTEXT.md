# Phase 75: Identity Extension & Envelope Crypto - Context

**Gathered:** 2026-04-01
**Status:** Ready for planning

<domain>
## Phase Boundary

Extend Identity with ML-KEM-1024 keypair and implement PQ envelope encrypt/decrypt with versioned binary format. Pure SDK crypto -- no network IO, no C++ node changes. New `_envelope.py` module + extended `identity.py`. Users can generate encryption-capable identities and encrypt/decrypt blob data for one or more PQ recipients.

</domain>

<decisions>
## Implementation Decisions

### Envelope Binary Format
- **D-01:** Magic bytes: `CENV` (0x43454E56) -- mnemonic prefix, follows codebase convention
- **D-02:** Version byte: `0x01` for initial format version
- **D-03:** Cipher suite byte: `0x01` = ML-KEM-1024 + ChaCha20-Poly1305 + HKDF-SHA256. `0x00` reserved for invalid/unspecified
- **D-04:** Recipient count: uint16 big-endian. Practical cap enforced at 256 recipients (~412 KiB header max)
- **D-05:** Data AEAD nonce: 12 bytes random via `secrets.token_bytes(12)` -- stored in header
- **D-06:** Per-recipient stanza: `[kem_pk_hash:32][kem_ct:1568][wrapped_dek:48]` = 1648 bytes fixed
- **D-07:** Stanzas sorted by `kem_pk_hash` for deterministic output and O(log N) binary search on decrypt
- **D-08:** Full layout: `[magic:4][version:1][suite:1][count:2 BE][nonce:12][N x stanza:1648][ciphertext+tag]`

### DEK Wrapping
- **D-09:** Zero nonce for AEAD wrapping DEK with KEK -- safe because KEM shared secret (and thus KEK) is unique per encapsulation. Code comment required explaining why
- **D-10:** Wrap AD = partial header (magic + version + suite + count + nonce + all kem_pk_hash + kem_ct pairs, EXCLUDING wrapped_dek fields). Avoids circular dependency. Data AEAD AD = full serialized header (WITH wrapped_dek)
- **D-11:** HKDF for KEK derivation: `hkdf_derive(salt=b'', ikm=shared_secret, info=b'chromatindb-envelope-kek-v1', length=32)`. Empty salt consistent with transport and DARE HKDF domains

### Envelope API
- **D-12:** `envelope_encrypt(plaintext: bytes, recipients: list[Identity], sender: Identity) -> bytes`
- **D-13:** `envelope_decrypt(data: bytes, identity: Identity) -> bytes` -- returns plain bytes only
- **D-14:** Optional `envelope_parse(data: bytes)` for metadata inspection (version, suite, recipient_count) without decryption
- **D-15:** Sender always auto-included as recipient (prevents accidental lockout). Dedup handles if already in list
- **D-16:** Empty recipients list = encrypt to sender only (self-encrypt supported in Phase 75)
- **D-17:** Duplicate recipients silently deduplicated by kem_pk_hash before encryption

### Error Handling
- **D-18:** Three new exception subclasses of `CryptoError`: `DecryptionError` (AEAD tag mismatch), `NotARecipientError` (stanza not found), `MalformedEnvelopeError` (bad magic/version/truncated)
- **D-19:** Unsupported version byte raises `MalformedEnvelopeError` (no separate UnsupportedVersionError)
- **D-20:** All decryption failures raise typed exceptions -- no None returns

### Identity Extension
- **D-21:** `generate()` always produces both ML-DSA-87 signing + ML-KEM-1024 encryption keypairs. No signing-only generation
- **D-22:** `load()` requires all 4 files: `.key` (4896B), `.pub` (2592B), `.kem` (3168B), `.kpub` (1568B). Missing any -> `KeyFileError`. No backward compat
- **D-23:** KEM key files are raw binary, same convention as signing keys. Size validation on load
- **D-24:** Constants in identity.py: `KEM_PUBLIC_KEY_SIZE = 1568`, `KEM_SECRET_KEY_SIZE = 3168`
- **D-25:** Internal storage: `oqs.KeyEncapsulation` object (mirrors `oqs.Signature` for signing). `_kem` field + `_kem_public_key` field
- **D-26:** New `from_public_keys(signing_pk: bytes, kem_pk: bytes) -> Identity` constructor for verify + encrypt-capable (no signing, no decap). Used by Phase 76 directory
- **D-27:** Existing `from_public_key(signing_pk)` stays as-is for signing-verify-only peers (no KEM)
- **D-28:** `has_kem` property, `kem_public_key` property. `kem_secret_key` NOT exposed as property (stays internal to oqs object)
- **D-29:** `generate()` uses system randomness only -- no seed parameter, no test backdoor

### KEM Public Key Validation
- **D-30:** Size check (1568 bytes) before `encap_secret()`. Trust liboqs for FIPS 203 internal validation. Code comment noting the FIPS 203 section 7.2 gap
- **D-31:** Recipient lookup: `SHA3-256(kem_pk)` -- 32-byte hash, consistent with namespace = SHA3-256(signing_pk) pattern

### Testing
- **D-32:** New `test_envelope.py` for all envelope encrypt/decrypt/format tests
- **D-33:** Extend existing `test_identity.py` with KEM keypair tests (generate, save, load, from_public_keys)
- **D-34:** Python test vector generator script (`tools/envelope_test_vectors.py`) outputting JSON for cross-SDK validation. Vectors go in `sdk/python/tests/vectors/`

### Claude's Discretion
- Internal _envelope.py module structure and helper functions
- Exact test case selection and assertion patterns
- Whether envelope_parse() is a separate function or a classmethod on a dataclass
- Code comment wording for zero-nonce and FIPS 203 gap explanations

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing SDK modules (extend)
- `sdk/python/chromatindb/identity.py` -- Current Identity class (ML-DSA-87 only), file I/O, verify-only constructor
- `sdk/python/chromatindb/crypto.py` -- AEAD encrypt/decrypt, HKDF, SHA3-256 (all needed primitives)
- `sdk/python/chromatindb/_hkdf.py` -- Pure-Python HKDF-SHA256 (RFC 5869)
- `sdk/python/chromatindb/exceptions.py` -- Exception hierarchy (CryptoError base)

### Existing SDK modules (reference patterns)
- `sdk/python/chromatindb/_codec.py` -- Binary encode/decode with struct (pattern for envelope format)
- `sdk/python/chromatindb/_handshake.py` -- ML-KEM-1024 encap/decap usage, HKDF with empty salt (lines 89-102)
- `sdk/python/chromatindb/types.py` -- Frozen dataclass conventions

### Existing tests (extend)
- `sdk/python/tests/test_identity.py` -- Current identity tests (add KEM tests here)
- `sdk/python/tests/test_crypto.py` -- Crypto primitive tests (reference pattern)

### Research (design rationale)
- `.planning/research/SUMMARY.md` -- Overall architecture, phase ordering, confidence assessment
- `.planning/research/ARCHITECTURE.md` -- Module layout, encrypted blob format, component boundaries
- `.planning/research/PITFALLS.md` -- KEM-then-Wrap pattern, nonce management, HKDF domain separation
- `.planning/research/FEATURES.md` -- Feature prioritization (must-have vs defer)
- `.planning/research/STACK.md` -- Zero new deps confirmation, liboqs API details

### Protocol
- `db/PROTOCOL.md` -- HKDF salt convention (line 91), handshake reference, HKDF label registry
- `db/crypto/master_key.h` -- DARE HKDF pattern (empty salt + domain label)
- `db/crypto/master_key.cpp` -- DARE implementation reference (line 68: empty salt)

### Cross-SDK test vectors
- `tools/test_vector_generator.cpp` -- Existing C++ test vector generator (pattern reference)

### Requirements
- `.planning/REQUIREMENTS.md` -- IDENT-01 through IDENT-03, ENV-01 through ENV-06

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `crypto.aead_encrypt()` / `aead_decrypt()`: ChaCha20-Poly1305 -- used for both DEK wrapping and data encryption
- `crypto.sha3_256()`: Recipient stanza lookup key (SHA3-256 of KEM pubkey)
- `_hkdf.hkdf_derive()`: KEK derivation from KEM shared secret
- `oqs.KeyEncapsulation('ML-KEM-1024')`: Already used in `_handshake.py` for transport KEM

### Established Patterns
- identity.py: `oqs.Signature` object held internally, raw bytes exported via `export_secret_key()`. Same pattern for `oqs.KeyEncapsulation`
- identity.py: File I/O with size validation (`len(key) != EXPECTED_SIZE -> KeyFileError`)
- identity.py: `.key`/`.pub` sibling convention via `Path.with_suffix()`
- _codec.py: `struct.pack('>H', count)` for big-endian uint16 (recipient count)
- exceptions.py: Subclass hierarchy under `ChromatinError > CryptoError`

### Integration Points
- `identity.py`: Add `_kem`, `_kem_public_key` fields to `__init__`. Extend `generate()`, `load()`, `save()`, `generate_and_save()`. Add `from_public_keys()` classmethod
- `exceptions.py`: Add `DecryptionError`, `NotARecipientError`, `MalformedEnvelopeError` under `CryptoError`
- `__init__.py`: Re-export new envelope functions and exception types
- New `_envelope.py` module: pure crypto, no imports from client.py or transport

</code_context>

<specifics>
## Specific Ideas

- KEM-then-Wrap is mandatory (ML-KEM cannot encapsulate a chosen value). Two-layer: random DEK -> per-recipient encap -> HKDF KEK -> AEAD-wrap DEK
- Zero nonce for DEK wrapping is safe because each KEM encapsulation produces a unique shared secret, making the derived KEK unique. Code comment required
- Partial header as wrap AD (excluding wrapped_dek to avoid circular dependency), full header as data AEAD AD
- Sender auto-inclusion prevents accidental data lockout -- dedup handles if sender already in recipient list
- Test vectors enable future C++/Rust/JS SDKs to validate envelope interop without a live node

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 75-identity-extension-envelope-crypto*
*Context gathered: 2026-04-01*
