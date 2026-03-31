# Technology Stack: v1.7.0 Client-Side Encryption

**Project:** chromatindb Python SDK -- PQ envelope encryption, pubkey directory, group management
**Researched:** 2026-03-31
**Confidence:** HIGH

## Scope

This research covers ONLY what the v1.7.0 milestone adds to the existing Python SDK. The existing stack (liboqs-python, PyNaCl, flatbuffers, asyncio) is validated and shipped in v1.6.0 -- not re-researched here. Focus: ML-KEM-1024 for per-recipient key wrapping, ChaCha20-Poly1305 for data encryption, random key generation, serialization of the encrypted envelope format, and any new dependencies needed.

## Verdict: Zero New Dependencies

**No new pip packages are needed.** Every capability required for v1.7.0 is already available in the existing SDK dependency set or Python stdlib.

| Capability | Provider | Already in SDK? |
|------------|----------|-----------------|
| ML-KEM-1024 encapsulate/decapsulate | `liboqs-python~=0.14.0` via `oqs.KeyEncapsulation` | YES -- used in handshake + tests |
| ML-KEM-1024 keypair generation + persistence | `liboqs-python~=0.14.0` via `generate_keypair()` / `export_secret_key()` | YES -- same pattern as ML-DSA-87 in `identity.py` |
| ChaCha20-Poly1305 AEAD encrypt/decrypt | `pynacl~=1.5.0` via `nacl.bindings.crypto_aead_chacha20poly1305_ietf_*` | YES -- `crypto.py` aead_encrypt/aead_decrypt |
| HKDF-SHA256 key derivation | Stdlib `hmac` + `hashlib` via `chromatindb._hkdf` | YES -- `_hkdf.py` |
| SHA3-256 hashing | Stdlib `hashlib.sha3_256` | YES -- `crypto.py` sha3_256 |
| Cryptographic random bytes | Stdlib `secrets.token_bytes()` | NEW usage but zero-dep (stdlib since Python 3.6) |
| Encrypted envelope binary format | Stdlib `struct.pack` / `struct.unpack` | YES -- same pattern as `_codec.py` |
| FlatBuffers blob encoding | `flatbuffers~=25.12` | YES -- `_codec.py` encode_blob_payload |
| Directory/group data serialization | Stdlib `struct.pack` (binary) or `json` (stdlib) | YES -- no new dep needed |

## Existing SDK APIs to Reuse

### ML-KEM-1024: `oqs.KeyEncapsulation("ML-KEM-1024")`

The SDK already exercises the full ML-KEM-1024 API:

```python
# Encapsulation (sender side -- wrap a key for a recipient)
kem = oqs.KeyEncapsulation("ML-KEM-1024")
ciphertext, shared_secret = kem.encap_secret(recipient_public_key)
# ciphertext: 1568 bytes, shared_secret: 32 bytes

# Decapsulation (recipient side -- unwrap the key)
kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=recipient_secret_key)
shared_secret = kem.decap_secret(ciphertext)
# shared_secret: 32 bytes (identical to sender's)
```

**Already proven in the SDK:**
- `_handshake.py:128` -- `KeyEncapsulation("ML-KEM-1024")` instantiation
- `_handshake.py:148` -- `kem.decap_secret(kem_ct)` (initiator decapsulation)
- `tests/test_handshake.py:185` -- `kem.encap_secret(sdk_kem_pk)` (mock responder encapsulation)

For envelope encryption, each recipient's wrapped key = `encap_secret(recipient_kem_pk)` producing a (1568-byte ciphertext, 32-byte shared_secret) pair. The shared_secret is then used via HKDF to derive the wrapping key for the per-blob symmetric key.

### ML-KEM-1024 Key Sizes (Confirmed)

| Parameter | Size | Source |
|-----------|------|--------|
| Public key | 1568 bytes | OQS docs, confirmed in `_handshake.py KEM_PK_SIZE` |
| Secret key | 3168 bytes | OQS docs (FIPS 203) |
| Ciphertext | 1568 bytes | OQS docs, confirmed in `_handshake.py KEM_CT_SIZE` |
| Shared secret | 32 bytes | OQS docs, confirmed in `_handshake.py KEM_SS_SIZE` |

### ML-KEM-1024 Keypair Persistence

Use the same SSH-style `.key`/`.pub` pattern as ML-DSA-87 in `identity.py`:

```python
# Generate
kem = oqs.KeyEncapsulation("ML-KEM-1024")
kem_public_key = bytes(kem.generate_keypair())   # 1568 bytes
kem_secret_key = bytes(kem.export_secret_key())   # 3168 bytes

# Save
Path("identity.kem.pub").write_bytes(kem_public_key)
Path("identity.kem.key").write_bytes(kem_secret_key)

# Load
kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=loaded_secret_key)
```

**Decision: Extend the existing `Identity` class** to carry an optional ML-KEM-1024 keypair alongside the existing ML-DSA-87 keypair. File convention: `identity.key` / `identity.pub` (signing, existing) + `identity.kem.key` / `identity.kem.pub` (encryption, new).

### ChaCha20-Poly1305 AEAD: `crypto.aead_encrypt` / `crypto.aead_decrypt`

Already in `crypto.py` with full validation. For envelope encryption:

```python
from chromatindb.crypto import aead_encrypt, aead_decrypt, AEAD_KEY_SIZE, AEAD_NONCE_SIZE

# Encrypt blob data
nonce = secrets.token_bytes(AEAD_NONCE_SIZE)  # 12 random bytes
ciphertext = aead_encrypt(plaintext, ad=b"", nonce=nonce, key=data_key)

# Decrypt
plaintext = aead_decrypt(ciphertext, ad=b"", nonce=nonce, key=data_key)
```

**Note:** For envelope encryption, we use random nonces (not counters) because each blob is independently encrypted with a unique per-blob key. Counter nonces are only needed for the transport layer where the same key encrypts multiple frames.

### HKDF-SHA256: `_hkdf.hkdf_derive`

Already in `_hkdf.py`. For envelope encryption key derivation:

```python
from chromatindb._hkdf import hkdf_derive

# Derive blob data key from KEM shared secret
data_key = hkdf_derive(
    salt=b"",
    ikm=shared_secret,  # 32 bytes from KEM encap/decap
    info=b"chromatindb-envelope-v1",
    length=32,
)
```

### Cryptographic Random Bytes: `secrets.token_bytes()`

**New usage, zero new dependency.** The `secrets` module is stdlib since Python 3.6. Use for:

1. **Per-blob data encryption key (DEK):** `secrets.token_bytes(32)` -- 256-bit random key
2. **AEAD nonce for data encryption:** `secrets.token_bytes(12)` -- 96-bit random nonce

Why `secrets.token_bytes()` over `os.urandom()`: Same underlying CSPRNG, but `secrets` is the canonical Python module for cryptographic randomness (PEP 506). Makes intent explicit.

Why not `nacl.utils.random()`: Adds a runtime dependency call into libsodium for something stdlib handles identically. No interop benefit since this randomness isn't shared with the C++ node.

## Encrypted Envelope Format (Serialization)

The encrypted blob envelope is a binary format using `struct.pack`, following the same conventions as the rest of the SDK's `_codec.py` (big-endian multi-byte integers).

### Proposed Wire Format

```
[version:1]                           -- Format version (0x01)
[nonce:12]                            -- AEAD nonce for data encryption
[recipient_count:2 BE]                -- Number of recipients (uint16)
[recipients...]                       -- Per-recipient wrapped key blocks
  [kem_pk_hash:32]                    -- SHA3-256(recipient_kem_public_key) for lookup
  [kem_ciphertext:1568]               -- ML-KEM-1024 ciphertext from encap_secret()
[encrypted_data_with_tag:N]           -- ChaCha20-Poly1305 ciphertext + 16-byte tag
```

Per-recipient block size: 32 + 1568 = 1600 bytes fixed.
Total overhead per recipient: 1600 bytes.
Fixed header overhead: 1 + 12 + 2 = 15 bytes + 16 bytes AEAD tag.

### Envelope Encryption Procedure

1. Generate random DEK: `dek = secrets.token_bytes(32)`
2. Generate random nonce: `nonce = secrets.token_bytes(12)`
3. Encrypt data: `ciphertext = aead_encrypt(plaintext, ad=b"", nonce=nonce, key=dek)`
4. For each recipient:
   a. `kem = oqs.KeyEncapsulation("ML-KEM-1024")`
   b. `kem_ct, shared_secret = kem.encap_secret(recipient_kem_pk)`
   c. `wrapping_key = hkdf_derive(b"", shared_secret, b"chromatindb-envelope-v1", 32)`
   d. `wrapped_dek = aead_encrypt(dek, ad=b"", nonce=nonce, key=wrapping_key)`
5. Assemble envelope: version + nonce + recipient_count + recipient_blocks + ciphertext

Wait -- step 4d wraps the DEK. The wrapped DEK is 32 + 16 = 48 bytes (32-byte key + 16-byte AEAD tag). This changes the per-recipient block:

### Revised Per-Recipient Block

```
[kem_pk_hash:32]                    -- Recipient identifier
[kem_ciphertext:1568]               -- ML-KEM-1024 encapsulation
[wrapped_dek:48]                    -- AEAD-encrypted DEK (32 + 16 tag)
```

Per-recipient block: 32 + 1568 + 48 = 1648 bytes fixed.

**All of this is serialized with `struct.pack` -- no new serialization library needed.**

## Directory and Group Data Format

User pubkeys, group definitions, and other directory entries are stored as regular blobs in the admin's namespace. The data payload needs a type tag to distinguish entry types.

**Use a simple type-length-value (TLV) binary format with `struct.pack`:**

```
[entry_type:1]                        -- 0x01=user_pubkey, 0x02=group, 0x03=revocation
[entry_data:variable]                 -- Type-specific payload
```

User pubkey entry: `[0x01][display_name_len:2 BE][display_name:N][signing_pk:2592][kem_pk:1568]`
Group entry: `[0x02][group_name_len:2 BE][group_name:N][member_count:2 BE][member_pk_hash:32*N]`

No JSON, no MessagePack, no protobuf. Binary `struct.pack` is consistent with the entire SDK wire protocol convention and adds zero dependencies.

## What NOT to Add

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| `cryptography` package | Pulls OpenSSL + Rust compiler. Project constraint: "No OpenSSL." ML-KEM-1024 is in liboqs, ChaCha20 is in PyNaCl, HKDF is in stdlib. Zero reason to add this. | Existing deps |
| `pycryptodome` | Different C backend from libsodium. Risk of subtle AEAD interop issues. No capability we lack. | PyNaCl |
| `msgpack` / `cbor2` | Additional serialization dependency for a format we can express in ~20 lines of `struct.pack`. The entire SDK wire protocol uses `struct.pack`. Consistency > convenience. | `struct` (stdlib) |
| `json` for envelope format | JSON is text-based, wastes bytes, and can't cleanly embed binary (base64 overhead). Binary `struct.pack` is what the SDK already uses for all wire encoding. | `struct` (stdlib) |
| `nacl.utils.random()` | Calls into libsodium for randomness. `secrets.token_bytes()` uses the OS CSPRNG directly and is the canonical Python approach. No interop requirement. | `secrets.token_bytes()` (stdlib) |
| New FlatBuffers schemas | Envelope encryption is SDK-layer, not wire protocol. The node stores opaque blobs -- it never interprets the encrypted payload. FlatBuffers overhead is unjustified for a format only the SDK reads. | `struct.pack` |
| `age` encryption library | Age (actually-good-encryption) is for file encryption, not programmatic envelope crypto. Would add X25519 (non-PQ) dependency. We need ML-KEM-1024 (PQ). | Direct ML-KEM + ChaCha20 from existing deps |
| Any hybrid KEM (X25519+ML-KEM) | chromatindb is PQ-only by design. ML-KEM-1024 alone. No classical fallback needed. | `oqs.KeyEncapsulation("ML-KEM-1024")` |

## Recommended Stack (Complete for v1.7.0)

### Runtime Dependencies (unchanged from v1.6.0)

| Technology | Version | Purpose in v1.7.0 | Why |
|------------|---------|-------------------|-----|
| liboqs-python | ~=0.14.0 | ML-KEM-1024 `encap_secret()` / `decap_secret()` for per-recipient key wrapping; ML-KEM keypair generation and persistence | Already proven in SDK handshake + tests. `encap_secret(pubkey)` returns (ciphertext:1568, shared_secret:32). `decap_secret(ct)` returns shared_secret:32. |
| PyNaCl | ~=1.5.0 | ChaCha20-Poly1305 AEAD for blob data encryption and DEK wrapping | Already proven in `crypto.py`. Same `aead_encrypt`/`aead_decrypt` functions, different keys. |
| flatbuffers | ~=25.12 | FlatBuffers encoding for blob payloads written to network | Unchanged -- encrypted payload is stored inside the FlatBuffer `Blob.data` field as opaque bytes. |

### Stdlib Usage (no pip install needed)

| Module | Purpose in v1.7.0 | Notes |
|--------|--------------------|-------|
| `secrets` | `secrets.token_bytes(32)` for DEK generation, `secrets.token_bytes(12)` for AEAD nonce | NEW usage. Available since Python 3.6. Canonical for crypto randomness per PEP 506. |
| `struct` | Binary envelope format encoding/decoding | Same pattern as existing `_codec.py`. No new patterns needed. |
| `hmac` + `hashlib` | HKDF-SHA256 to derive wrapping key from KEM shared secret | Reuses existing `_hkdf.py` implementation. No changes. |
| `hashlib` | SHA3-256 for recipient key hashing (`SHA3-256(kem_pk)` as identifier) | Reuses existing `crypto.sha3_256()`. No changes. |

### Development Dependencies (unchanged from v1.6.0)

| Technology | Version | Purpose |
|------------|---------|---------|
| pytest | latest | Test framework |
| ruff | latest | Linting and formatting |
| mypy | latest | Static type checking |

## pyproject.toml Changes

**None.** The `dependencies` list stays exactly the same:

```toml
dependencies = [
    "liboqs-python~=0.14.0",
    "pynacl~=1.5.0",
    "flatbuffers~=25.12",
]
```

## Key Integration Points

### 1. Identity Extension

The existing `Identity` class in `identity.py` manages ML-DSA-87 signing keys. Extend it with an optional ML-KEM-1024 encryption keypair:

- `Identity.generate()` -- add KEM keypair generation alongside signing keypair
- `Identity.save()` -- write `.kem.key` (3168 bytes) and `.kem.pub` (1568 bytes) sibling files
- `Identity.load()` -- load KEM keys if present (backward-compatible: old identities without KEM keys still work for signing)
- `Identity.kem_public_key` -- property returning 1568-byte KEM public key (or None)
- `Identity.from_public_key()` -- accept optional `kem_public_key` parameter for verify+encrypt-to capability

### 2. Crypto Module Extension

Add to `crypto.py` (or a new `_envelope.py` internal module):

- `envelope_encrypt(plaintext, recipient_kem_pks) -> bytes` -- full envelope encryption
- `envelope_decrypt(envelope, kem_secret_key, kem_public_key) -> bytes` -- find own recipient block, decap, unwrap DEK, decrypt

Both functions compose existing primitives: `secrets.token_bytes` + `oqs.KeyEncapsulation.encap_secret` + `hkdf_derive` + `aead_encrypt`.

### 3. Client API Extension

Add to `ChromatinClient`:

- `write_encrypted(data, ttl, recipients)` -- envelope encrypt then `write_blob()`
- `read_encrypted(namespace, blob_hash)` -- `read_blob()` then envelope decrypt

These are thin wrappers composing `envelope_encrypt`/`envelope_decrypt` with the existing `write_blob`/`read_blob` methods.

### 4. Directory as Blob Convention

The pubkey directory and group definitions are stored as ordinary signed blobs in a designated namespace. The SDK provides helpers to:

- Publish own KEM public key to directory namespace
- List and fetch KEM public keys from directory namespace
- Create/update group definitions
- Resolve group to member KEM public keys

All using existing `write_blob`, `read_blob`, `list_blobs` client methods. No new wire types, no node changes.

## Confidence Assessment

| Claim | Confidence | Basis |
|-------|------------|-------|
| liboqs-python `encap_secret`/`decap_secret` API | HIGH | Used in SDK handshake tests; verified against liboqs-python source on GitHub |
| ML-KEM-1024 key sizes (pk:1568, sk:3168, ct:1568, ss:32) | HIGH | OQS docs (FIPS 203), confirmed in `_handshake.py` constants |
| PyNaCl ChaCha20-Poly1305 AEAD reuse | HIGH | Already proven byte-identical to C++ node in v1.6.0 |
| HKDF-SHA256 stdlib reuse | HIGH | Already proven byte-identical to C++ node in v1.6.0 |
| `secrets.token_bytes()` availability | HIGH | Python stdlib since 3.6; SDK requires >=3.10 |
| No new dependencies needed | HIGH | Every capability mapped to existing dep or stdlib |
| Envelope format via `struct.pack` | HIGH | Same pattern as all existing SDK wire encoding |

## Sources

- [liboqs-python GitHub](https://github.com/open-quantum-safe/liboqs-python) -- `KeyEncapsulation` class API, `encap_secret()` / `decap_secret()` / `generate_keypair()` / `export_secret_key()` methods (HIGH confidence, source code verified)
- [liboqs-python oqs.py source](https://github.com/open-quantum-safe/liboqs-python/blob/main/oqs/oqs.py) -- Full class definition with method signatures and `details` dictionary structure (HIGH confidence)
- [ML-KEM on OQS](https://openquantumsafe.org/liboqs/algorithms/kem/ml-kem.html) -- ML-KEM-1024 parameters: pk=1568, sk=3168, ct=1568, ss=32, NIST Level 5 (HIGH confidence)
- [IETF PQ KEM for JOSE draft](https://datatracker.ietf.org/doc/draft-ietf-jose-pqc-kem/) -- Envelope encryption design patterns with ML-KEM (MEDIUM confidence, draft spec, used for design reference only)
- [Python secrets module docs](https://docs.python.org/3/library/secrets.html) -- `secrets.token_bytes()`, available since Python 3.6 (HIGH confidence)
- SDK source files: `_handshake.py`, `crypto.py`, `_hkdf.py`, `identity.py`, `_codec.py`, `client.py` -- existing implementations to reuse (HIGH confidence, primary source)
- SDK test files: `tests/test_handshake.py` -- proves `encap_secret()` and `decap_secret()` already work with ML-KEM-1024 in the SDK (HIGH confidence)

---
*Stack research for: chromatindb v1.7.0 Client-Side Encryption*
*Researched: 2026-03-31*
