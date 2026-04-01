# Phase 75: Identity Extension & Envelope Crypto - Research

**Researched:** 2026-04-01
**Domain:** PQ envelope encryption (ML-KEM-1024 + ChaCha20-Poly1305), Identity keypair extension
**Confidence:** HIGH

## Summary

Phase 75 extends the Python SDK Identity class to generate and manage ML-KEM-1024 encryption keypairs alongside the existing ML-DSA-87 signing keypairs, and implements PQ envelope encrypt/decrypt for multi-recipient blob encryption. This is pure SDK crypto work -- no C++ node changes, no network IO in the new module. All required primitives (ML-KEM-1024 via liboqs-python, ChaCha20-Poly1305 via PyNaCl, HKDF-SHA256 via stdlib, SHA3-256 via stdlib) are already available in the SDK dependency set with zero new pip dependencies.

The envelope format is a self-describing binary structure: `[magic:4][version:1][suite:1][count:2 BE][nonce:12][N x stanza:1648][ciphertext+tag]`. Each recipient stanza contains `[kem_pk_hash:32][kem_ct:1568][wrapped_dek:48]`. The KEM-then-Wrap pattern is mandatory: a random DEK encrypts data, then each recipient's KEM shared secret derives a KEK via HKDF, and the KEK wraps the DEK via AEAD. Stanzas are sorted by `kem_pk_hash` for O(log N) binary search on decrypt.

All crypto primitives have been verified on the target machine: ML-KEM-1024 encap/decap roundtrip, HKDF key derivation, AEAD wrap/unwrap of 32-byte DEK with zero nonce, and SHA3-256 hashing all produce correct results. The full envelope encrypt/decrypt flow has been validated end-to-end using existing SDK functions.

**Primary recommendation:** Implement in three waves: (1) exceptions additions, (2) Identity extension with KEM keypair + file I/O, (3) _envelope.py with encrypt/decrypt/parse + test vectors. All using existing SDK primitives, no new dependencies.

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Magic bytes: `CENV` (0x43454E56)
- **D-02:** Version byte: `0x01`
- **D-03:** Cipher suite byte: `0x01` = ML-KEM-1024 + ChaCha20-Poly1305 + HKDF-SHA256. `0x00` reserved
- **D-04:** Recipient count: uint16 big-endian. Cap at 256 recipients
- **D-05:** Data AEAD nonce: 12 bytes random via `secrets.token_bytes(12)`
- **D-06:** Per-recipient stanza: `[kem_pk_hash:32][kem_ct:1568][wrapped_dek:48]` = 1648 bytes
- **D-07:** Stanzas sorted by `kem_pk_hash` for deterministic output and O(log N) binary search
- **D-08:** Full layout: `[magic:4][version:1][suite:1][count:2 BE][nonce:12][N x stanza:1648][ciphertext+tag]`
- **D-09:** Zero nonce for AEAD wrapping DEK with KEK (safe because KEM shared secret is unique per encap). Code comment required
- **D-10:** Wrap AD = partial header (excluding wrapped_dek fields). Data AEAD AD = full serialized header (WITH wrapped_dek)
- **D-11:** HKDF: `hkdf_derive(salt=b'', ikm=shared_secret, info=b'chromatindb-envelope-kek-v1', length=32)`
- **D-12:** `envelope_encrypt(plaintext: bytes, recipients: list[Identity], sender: Identity) -> bytes`
- **D-13:** `envelope_decrypt(data: bytes, identity: Identity) -> bytes`
- **D-14:** Optional `envelope_parse(data: bytes)` for metadata inspection
- **D-15:** Sender always auto-included as recipient (dedup if already in list)
- **D-16:** Empty recipients = encrypt to sender only
- **D-17:** Duplicate recipients silently deduplicated by kem_pk_hash
- **D-18:** Three new exceptions under CryptoError: `DecryptionError` (exists), `NotARecipientError`, `MalformedEnvelopeError`
- **D-19:** Unsupported version raises `MalformedEnvelopeError`
- **D-20:** All decryption failures raise typed exceptions -- no None returns
- **D-21:** `generate()` always produces both ML-DSA-87 + ML-KEM-1024 keypairs
- **D-22:** `load()` requires all 4 files: `.key`/`.pub`/`.kem`/`.kpub`. No backward compat
- **D-23:** KEM files are raw binary with size validation
- **D-24:** Constants: `KEM_PUBLIC_KEY_SIZE = 1568`, `KEM_SECRET_KEY_SIZE = 3168`
- **D-25:** Internal storage: `oqs.KeyEncapsulation` object. `_kem` field + `_kem_public_key` field
- **D-26:** New `from_public_keys(signing_pk: bytes, kem_pk: bytes) -> Identity` constructor
- **D-27:** Existing `from_public_key(signing_pk)` stays as-is
- **D-28:** `has_kem` property, `kem_public_key` property. `kem_secret_key` NOT exposed as property
- **D-29:** `generate()` uses system randomness only
- **D-30:** Size check (1568 bytes) before `encap_secret()`. Trust liboqs for FIPS 203 internal validation. Code comment noting gap
- **D-31:** Recipient lookup: `SHA3-256(kem_pk)` -- 32-byte hash
- **D-32:** New `test_envelope.py`
- **D-33:** Extend existing `test_identity.py` with KEM tests
- **D-34:** Python test vector generator (`tools/envelope_test_vectors.py`) outputting JSON to `sdk/python/tests/vectors/`

### Claude's Discretion
- Internal _envelope.py module structure and helper functions
- Exact test case selection and assertion patterns
- Whether envelope_parse() is a separate function or a classmethod on a dataclass
- Code comment wording for zero-nonce and FIPS 203 gap explanations

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| IDENT-01 | User can generate an identity with both ML-DSA-87 signing and ML-KEM-1024 encryption keypairs | D-21, D-25, D-29: `generate()` produces both keypairs using `oqs.KeyEncapsulation("ML-KEM-1024")`. Verified on target machine: pk=1568, sk=3168 bytes |
| IDENT-02 | User can save and load identity with .key/.pub and .kem/.kpub files | D-22, D-23, D-24: Raw binary file I/O with size validation. Same pattern as existing ML-DSA-87 file I/O in `identity.py` |
| IDENT-03 | Identity exposes KEM public key for directory publishing and encryption | D-28: `has_kem` and `kem_public_key` properties. D-26: `from_public_keys()` constructor for encrypt-to-capable identities |
| ENV-01 | SDK encrypts blob data with random per-blob DEK using ChaCha20-Poly1305 | D-05: random nonce via `secrets.token_bytes(12)`. DEK via `secrets.token_bytes(32)`. Uses existing `crypto.aead_encrypt()` |
| ENV-02 | SDK wraps DEK per-recipient via ML-KEM-1024 encapsulation + HKDF-derived KEK | D-06, D-09, D-11: KEM-then-Wrap pattern. `encap_secret()` -> HKDF KEK -> AEAD wrap DEK. Zero nonce safe per unique KEM shared secret |
| ENV-03 | Encrypted blob uses versioned binary format | D-01 through D-08: `CENV` magic, version 0x01, suite 0x01, BE uint16 count, sorted stanzas |
| ENV-04 | Envelope header authenticated as AEAD associated data | D-10: Partial header as wrap AD (no wrapped_dek), full header as data AEAD AD. Prevents stanza substitution |
| ENV-05 | Recipient can decrypt by finding stanza, decapsulating, unwrapping DEK | D-07, D-13, D-31: Binary search by `SHA3-256(kem_pk)`, `decap_secret()`, HKDF KEK, AEAD unwrap, data decrypt |
| ENV-06 | HKDF uses unique domain label "chromatindb-envelope-kek-v1" | D-11: Unique HKDF info label, empty salt, 32-byte output. Separate from transport/DARE domains |

</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs-python | ~=0.14.0 (installed: 0.14.1, liboqs 0.15.0) | ML-KEM-1024 `encap_secret()`/`decap_secret()`, keypair generation | Already in SDK since v1.6.0. Proven in handshake. Zero alternative for PQ KEM in Python |
| PyNaCl | ~=1.5.0 | ChaCha20-Poly1305 AEAD via `crypto.aead_encrypt()`/`aead_decrypt()` | Already in SDK. Wraps libsodium. Used for data encryption and DEK wrapping |
| hashlib (stdlib) | Python 3.10+ | SHA3-256 via `crypto.sha3_256()` | Already in SDK. Recipient lookup hashing |
| hmac+hashlib (stdlib) | Python 3.10+ | HKDF-SHA256 via `_hkdf.hkdf_derive()` | Already in SDK. KEK derivation from KEM shared secret |
| secrets (stdlib) | Python 3.10+ | `secrets.token_bytes(32)` for DEK, `secrets.token_bytes(12)` for nonce | Canonical Python CSPRNG (PEP 506). New usage, zero dep |
| struct (stdlib) | Python 3.10+ | Binary envelope format encode/decode | Already used throughout `_codec.py`. Consistent pattern |
| bisect (stdlib) | Python 3.10+ | O(log N) binary search on sorted stanzas for decryption | Standard for sorted sequence search |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| oqs.KeyEncapsulation | Part of liboqs-python | KEM encap/decap/keygen API | Identity generation + envelope encrypt/decrypt |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Zero new deps | `cryptography` package | Pulls OpenSSL + Rust. Project forbids OpenSSL. All primitives already available |
| `struct.pack` for format | FlatBuffers schema | Overkill for SDK-only format. Node never parses envelope. struct is consistent with codebase |
| `secrets.token_bytes()` | `os.urandom()` | Functionally identical. secrets is canonical per PEP 506 |

**Installation:** No new packages. Existing `pip install -e ".[dev]"` covers everything.

## Architecture Patterns

### New/Modified Module Layout
```
sdk/python/chromatindb/
  identity.py          MODIFY: Add ML-KEM-1024 keypair (generate, save, load, from_public_keys)
  exceptions.py        MODIFY: Add NotARecipientError, MalformedEnvelopeError
  _envelope.py         NEW: envelope_encrypt(), envelope_decrypt(), envelope_parse()
  __init__.py          MODIFY: Re-export new functions + exceptions

sdk/python/tests/
  test_identity.py     MODIFY: Add KEM keypair tests
  test_envelope.py     NEW: Envelope encrypt/decrypt/format tests

sdk/python/tests/vectors/
  envelope_vectors.json  NEW: Cross-SDK test vectors

tools/
  envelope_test_vectors.py  NEW: Python test vector generator
```

### Pattern 1: KEM-then-Wrap Envelope Encryption
**What:** Random DEK encrypts data once. Per-recipient: `encap_secret()` -> HKDF KEK -> AEAD wrap DEK. One data ciphertext, N wrapped DEKs.
**When to use:** All multi-recipient encryption.
**Example:**
```python
# Source: Verified on target machine + CONTEXT.md D-09/D-11
import secrets
import oqs
from chromatindb.crypto import aead_encrypt, sha3_256
from chromatindb._hkdf import hkdf_derive

dek = secrets.token_bytes(32)
data_nonce = secrets.token_bytes(12)
zero_nonce = b"\x00" * 12

stanzas = []
for recipient in recipients:
    kem = oqs.KeyEncapsulation("ML-KEM-1024")
    kem_ct, kem_ss = kem.encap_secret(recipient.kem_public_key)
    kem_ct, kem_ss = bytes(kem_ct), bytes(kem_ss)
    kek = hkdf_derive(salt=b"", ikm=kem_ss,
                       info=b"chromatindb-envelope-kek-v1", length=32)
    # Zero nonce is safe: KEM shared secret (and thus KEK) is unique
    # per encapsulation -- the key is never reused.
    wrapped_dek = aead_encrypt(dek, ad=wrap_ad, nonce=zero_nonce, key=kek)
    pk_hash = sha3_256(recipient.kem_public_key)
    stanzas.append((pk_hash, kem_ct, wrapped_dek))

# Sort by pk_hash for deterministic output + O(log N) search
stanzas.sort(key=lambda s: s[0])

ciphertext = aead_encrypt(plaintext, ad=full_header_ad, nonce=data_nonce, key=dek)
```

### Pattern 2: Two-Layer Associated Data
**What:** Wrap AD (for DEK wrapping) uses partial header excluding wrapped_dek. Data AD (for data AEAD) uses full serialized header including all wrapped_dek fields.
**When to use:** Envelope encrypt and decrypt.
**Why:** Wrap AD cannot include wrapped_dek (circular dependency -- you are computing wrapped_dek at that point). Data AD includes everything to bind ciphertext to complete header.
**Example:**
```python
# Source: CONTEXT.md D-10
# Wrap AD = magic + version + suite + count + nonce + all (kem_pk_hash + kem_ct)
# Data AD = full header bytes (magic through last wrapped_dek)
```

### Pattern 3: Identity Extension (Two Keypair Types in One Class)
**What:** Single Identity holds both ML-DSA-87 (signing) and ML-KEM-1024 (encryption). Internal `_kem` field as `oqs.KeyEncapsulation` object.
**When to use:** All identity operations.
**Example:**
```python
# Source: Existing identity.py pattern + CONTEXT.md D-25
class Identity:
    def __init__(self, public_key, namespace, signer=None,
                 kem_public_key=None, kem=None):
        self._public_key = public_key
        self._namespace = namespace
        self._signer = signer
        self._kem_public_key = kem_public_key  # bytes or None
        self._kem = kem  # oqs.KeyEncapsulation or None
```

### Pattern 4: Sorted Stanza Binary Search
**What:** Stanzas sorted by kem_pk_hash (32 bytes). On decrypt, compute own pk_hash, binary search the sorted list.
**When to use:** `envelope_decrypt()` recipient lookup.
**Example:**
```python
# Source: CONTEXT.md D-07, stdlib bisect
import bisect

my_hash = sha3_256(identity.kem_public_key)
# Extract sorted pk_hashes from parsed stanzas
idx = bisect.bisect_left(pk_hashes, my_hash)
if idx < len(pk_hashes) and pk_hashes[idx] == my_hash:
    # Found our stanza at index idx
    ...
else:
    raise NotARecipientError("Identity not in recipient list")
```

### Anti-Patterns to Avoid
- **Treating ML-KEM as RSA:** Cannot choose the encapsulated value. Must use KEM-then-Wrap
- **Counter nonces for data AEAD:** Use random nonces (`secrets.token_bytes(12)`). Counter pattern is transport-layer only
- **Exposing `kem_secret_key` as property:** D-28 explicitly forbids this. Internal to `oqs.KeyEncapsulation` object
- **Returning None from envelope_decrypt:** D-20 mandates typed exceptions for all failures
- **Encrypting FlatBuffer instead of data:** Node must verify signatures on cleartext FlatBuffer. Only the `data` field content is encrypted

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| KEM encapsulation/decapsulation | Custom Kyber implementation | `oqs.KeyEncapsulation("ML-KEM-1024")` | FIPS 203 compliant, battle-tested, already in deps |
| AEAD encryption | Custom ChaCha20-Poly1305 | `crypto.aead_encrypt()`/`aead_decrypt()` | Already proven byte-identical to C++ node |
| HKDF key derivation | Custom HMAC construction | `_hkdf.hkdf_derive()` | Already proven byte-identical to C++ node |
| SHA3-256 hashing | Custom hash | `crypto.sha3_256()` | stdlib hashlib, already in SDK |
| CSPRNG | Custom random | `secrets.token_bytes()` | PEP 506 canonical, OS-backed CSPRNG |
| Binary format serialization | JSON, MessagePack, protobuf | `struct.pack`/`struct.unpack` | Consistent with all existing SDK wire encoding |

**Key insight:** Every cryptographic primitive needed for envelope encryption already exists in the SDK. Phase 75 is purely composition -- assembling existing building blocks into the envelope format. No new crypto code, just new orchestration.

## Common Pitfalls

### Pitfall 1: KEM-then-Wrap Pattern Violation
**What goes wrong:** Developer tries to encrypt DEK directly with ML-KEM encap, or uses KEM shared secret as DEK directly (breaks multi-recipient).
**Why it happens:** RSA mental model where you "encrypt a chosen value to a public key."
**How to avoid:** Always: `encap_secret()` -> `hkdf_derive(ss)` -> KEK -> `aead_encrypt(dek, key=kek)`. Never pass DEK to KEM.
**Warning signs:** DEK passed as argument to `encap_secret()`, no HKDF step, data encrypted N times for N recipients.

### Pitfall 2: Nonce Reuse Between Transport and Data AEAD
**What goes wrong:** Copy-pasting counter-based nonce pattern from `_framing.py` for data encryption.
**Why it happens:** Transport AEAD uses sequential counter successfully. Looks like the same pattern.
**How to avoid:** Data AEAD always uses `secrets.token_bytes(12)` -- random nonce. Different key per blob makes counter unnecessary.
**Warning signs:** `send_counter` or incrementing int used for data nonce.

### Pitfall 3: aead_decrypt Returns None, Not Exceptions
**What goes wrong:** `envelope_decrypt()` calls `crypto.aead_decrypt()` which returns `None` on auth failure. Developer forgets to check and raises wrong error, or passes None to downstream code.
**Why it happens:** `crypto.aead_decrypt()` follows the PyNaCl convention of returning `None` on failure. But D-20 mandates typed exceptions from the envelope API.
**How to avoid:** Every `aead_decrypt()` call in `_envelope.py` must check for `None` and raise `DecryptionError` (for data AEAD failure) or re-raise as appropriate.
**Warning signs:** Unchecked `aead_decrypt()` return value, `None` propagating to caller.

### Pitfall 4: Wrap AD Circular Dependency
**What goes wrong:** Trying to include `wrapped_dek` in the wrap AD. But you are computing `wrapped_dek` at that point -- you need the AD before you can compute the wrapped value, creating a circular dependency.
**Why it happens:** D-10 specifies two different ADs and it is easy to mix them up.
**How to avoid:** Wrap AD = partial header (magic + version + suite + count + nonce + all kem_pk_hash + kem_ct). Data AD = full serialized header (includes wrapped_dek). Two distinct byte sequences.
**Warning signs:** Same AD variable used for both wrap and data AEAD.

### Pitfall 5: Forgetting Sender Auto-Include
**What goes wrong:** Sender encrypts data for recipients A and B but forgets to include themselves. They cannot decrypt their own data.
**Why it happens:** Natural to think "I am sending TO these recipients."
**How to avoid:** D-15: `envelope_encrypt()` always auto-includes sender. D-17: dedup by kem_pk_hash handles sender already in list.
**Warning signs:** Test where sender cannot decrypt their own envelope.

### Pitfall 6: KEM Key File Naming Collision
**What goes wrong:** Using `.kem.key`/`.kem.pub` (dot-separated) instead of `.kem`/`.kpub` (suffix replacement).
**Why it happens:** Research docs (ARCHITECTURE.md) suggested `.kem.key`/`.kem.pub` but CONTEXT.md D-22 locked `.kem`/`.kpub`.
**How to avoid:** Follow D-22 exactly: `Path.with_suffix(".kem")` for secret, `Path.with_suffix(".kpub")` for public. Matches existing `.key`/`.pub` suffix convention.
**Warning signs:** File extensions with double dots.

## Code Examples

Verified patterns from existing SDK source and on-machine testing:

### ML-KEM-1024 Keypair Generation (Verified)
```python
# Source: _handshake.py pattern + verified on target machine
import oqs

kem = oqs.KeyEncapsulation("ML-KEM-1024")
kem_public_key = bytes(kem.generate_keypair())   # 1568 bytes
kem_secret_key = bytes(kem.export_secret_key())   # 3168 bytes

# Reconstruct from saved secret key
kem2 = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=kem_secret_key)
```

### Encap/Decap Roundtrip (Verified)
```python
# Source: Verified on target machine 2026-04-01
kem_ct, kem_ss = kem.encap_secret(kem_public_key)
kem_ct, kem_ss = bytes(kem_ct), bytes(kem_ss)
# kem_ct: 1568 bytes, kem_ss: 32 bytes

kem_dec = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=kem_secret_key)
kem_ss2 = bytes(kem_dec.decap_secret(kem_ct))
assert kem_ss == kem_ss2  # Shared secrets match
```

### DEK Wrapping with Zero Nonce (Verified)
```python
# Source: Verified end-to-end on target machine 2026-04-01
from chromatindb.crypto import aead_encrypt, aead_decrypt
from chromatindb._hkdf import hkdf_derive

kek = hkdf_derive(salt=b"", ikm=kem_ss,
                   info=b"chromatindb-envelope-kek-v1", length=32)
zero_nonce = b"\x00" * 12
wrapped_dek = aead_encrypt(dek, ad=wrap_ad, nonce=zero_nonce, key=kek)
# wrapped_dek: 48 bytes (32-byte DEK + 16-byte AEAD tag)

unwrapped = aead_decrypt(wrapped_dek, ad=wrap_ad, nonce=zero_nonce, key=kek)
assert unwrapped == dek  # Note: returns None on failure, not exception
```

### Binary Envelope Header Serialization
```python
# Source: _codec.py pattern + CONTEXT.md D-08
import struct

ENVELOPE_MAGIC = b"CENV"  # 0x43454E56
ENVELOPE_VERSION = 0x01
CIPHER_SUITE_ML_KEM_CHACHA = 0x01

header = ENVELOPE_MAGIC
header += struct.pack("B", ENVELOPE_VERSION)    # 1 byte
header += struct.pack("B", CIPHER_SUITE_ML_KEM_CHACHA)  # 1 byte
header += struct.pack(">H", recipient_count)    # 2 bytes BE
header += data_nonce                             # 12 bytes
# Then N x 1648-byte stanzas (sorted by pk_hash)
```

### Exception Hierarchy Extension
```python
# Source: exceptions.py existing pattern + CONTEXT.md D-18
# DecryptionError already exists in exceptions.py

class NotARecipientError(CryptoError):
    """Identity not found in envelope recipient list."""

class MalformedEnvelopeError(CryptoError):
    """Envelope has invalid magic, version, or is truncated."""
```

### Identity File I/O Extension Pattern
```python
# Source: identity.py existing load() pattern + CONTEXT.md D-22
key_path = Path(key_path)
pub_path = key_path.with_suffix(".pub")
kem_path = key_path.with_suffix(".kem")
kpub_path = key_path.with_suffix(".kpub")

# Save KEM keys (same pattern as signing keys)
kem_path.write_bytes(kem.export_secret_key())  # 3168 bytes
kpub_path.write_bytes(kem_public_key)           # 1568 bytes

# Load with size validation
kem_secret = kem_path.read_bytes()
if len(kem_secret) != KEM_SECRET_KEY_SIZE:  # 3168
    raise KeyFileError(...)
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| RSA envelope (encrypt chosen DEK to pubkey) | KEM-then-Wrap (encap random SS, derive KEK, wrap DEK) | ML-KEM standardization (FIPS 203, Aug 2024) | Two-layer key wrapping is mandatory. Cannot choose encapsulated value |
| X25519 + ChaCha20 (HPKE) | ML-KEM-1024 + ChaCha20-Poly1305 | PQ migration | chromatindb is PQ-only. No classical fallback. NIST Level 5 |
| HKDF salt from pubkeys | Empty HKDF salt | Phase 74 fix | Consistent across transport, DARE, and now envelope KEK domains |

**Deprecated/outdated:**
- `.kem.key`/`.kem.pub` file extension (from ARCHITECTURE.md research): superseded by D-22 `.kem`/`.kpub` suffixes
- Backward-compatible identity loading (from ARCHITECTURE.md): superseded by D-22 requiring all 4 files

## Open Questions

1. **Wrap AD construction order**
   - What we know: D-10 says wrap AD = partial header (magic + version + suite + count + nonce + all kem_pk_hash + kem_ct pairs, EXCLUDING wrapped_dek). Stanzas are sorted by D-07.
   - What's unclear: Do we serialize ALL stanza (pk_hash + kem_ct) pairs first into the wrap AD before computing any wrapped_dek? This requires a two-pass approach: first encap all recipients to get (pk_hash, kem_ct) pairs, sort them, build wrap AD, then wrap DEK for each.
   - Recommendation: Yes, two-pass. First pass: encap all recipients, collect (pk_hash, kem_ct) tuples, sort. Build wrap AD from sorted tuples. Second pass: compute wrapped_dek for each using the shared wrap AD. This is the only correct approach given D-10.

2. **liboqs version mismatch warning**
   - What we know: Installed liboqs-python 0.14.1 warns about liboqs 0.15.0 mismatch. All KEM operations still work correctly.
   - What's unclear: Whether this mismatch will cause any subtle issues.
   - Recommendation: Document the warning. All primitives have been verified functional. Not a blocking concern.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Python 3.10+ | SDK requires-python | Yes | 3.14 | -- |
| liboqs-python | ML-KEM-1024 encap/decap | Yes | 0.14.1 (liboqs 0.15.0) | -- |
| PyNaCl | ChaCha20-Poly1305 AEAD | Yes | installed | -- |
| pytest | Test framework | Yes | installed | -- |
| secrets (stdlib) | CSPRNG for DEK/nonce | Yes | stdlib | -- |
| struct (stdlib) | Binary format | Yes | stdlib | -- |
| bisect (stdlib) | Binary search | Yes | stdlib | -- |

**Missing dependencies with no fallback:** None
**Missing dependencies with fallback:** None

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest (installed, asyncio_mode=auto) |
| Config file | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && python3 -m pytest tests/test_identity.py tests/test_envelope.py -x -q` |
| Full suite command | `cd sdk/python && python3 -m pytest -x -q` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| IDENT-01 | Generate identity with both signing + KEM keypairs | unit | `python3 -m pytest tests/test_identity.py -x -q -k "kem"` | Extend existing |
| IDENT-02 | Save/load identity with 4 files (.key/.pub/.kem/.kpub) | unit | `python3 -m pytest tests/test_identity.py -x -q -k "save or load"` | Extend existing |
| IDENT-03 | Identity exposes KEM public key via properties | unit | `python3 -m pytest tests/test_identity.py -x -q -k "kem_public"` | Extend existing |
| ENV-01 | Encrypt blob data with random DEK | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "encrypt"` | Wave 0 |
| ENV-02 | Wrap DEK per-recipient via KEM + HKDF KEK | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "wrap or recipient"` | Wave 0 |
| ENV-03 | Versioned binary format (magic, version, suite) | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "format or parse"` | Wave 0 |
| ENV-04 | Header authenticated as AEAD AD | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "ad or tamper"` | Wave 0 |
| ENV-05 | Recipient decrypts via stanza lookup + decap | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "decrypt"` | Wave 0 |
| ENV-06 | HKDF domain label "chromatindb-envelope-kek-v1" | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "hkdf or domain"` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd sdk/python && python3 -m pytest tests/test_identity.py tests/test_envelope.py -x -q`
- **Per wave merge:** `cd sdk/python && python3 -m pytest -x -q`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `sdk/python/tests/test_envelope.py` -- covers ENV-01 through ENV-06
- [ ] `sdk/python/tests/vectors/envelope_vectors.json` -- cross-SDK test vectors
- [ ] `tools/envelope_test_vectors.py` -- vector generator script

## Critical Integration Details

### aead_decrypt Returns None, Not Exceptions
The existing `crypto.aead_decrypt()` returns `None` on AEAD authentication failure (line 119-125 of crypto.py). It does NOT raise exceptions. The envelope module must check every `aead_decrypt()` return value and raise the appropriate typed exception:
- DEK unwrap failure -> `DecryptionError("Key unwrap failed")`
- Data decryption failure -> `DecryptionError("Data decryption failed")`
- Stanza not found -> `NotARecipientError`
- Bad magic/version/truncated -> `MalformedEnvelopeError`

### Two-Pass Encryption Required by AD Design
D-10 requires all `(kem_pk_hash, kem_ct)` pairs in the wrap AD. This means encryption must be two-pass:
1. **Pass 1:** For each recipient, `encap_secret()` to get `(kem_ct, kem_ss)`. Store `(pk_hash, kem_ct, kem_ss)`. Sort by `pk_hash`.
2. **Build wrap AD:** Serialize magic + version + suite + count + nonce + all sorted `(pk_hash, kem_ct)` pairs.
3. **Pass 2:** For each sorted recipient, derive KEK from stored `kem_ss`, wrap DEK using wrap AD.
4. **Build full header:** Append all `wrapped_dek` values to get complete header.
5. **Encrypt data:** `aead_encrypt(plaintext, ad=full_header, nonce=data_nonce, key=dek)`

### Identity __init__ Signature Change
Adding `kem_public_key` and `_kem` parameters to `Identity.__init__()` must use default `None` to maintain backward compatibility with existing `from_public_key()` constructor (D-27 says it stays as-is). Both new parameters are optional.

### Exception Class Additions
`DecryptionError` already exists in `exceptions.py` (line 33). Only `NotARecipientError` and `MalformedEnvelopeError` need to be added. Both subclass `CryptoError`.

### __init__.py Re-exports
New public symbols to add: `envelope_encrypt`, `envelope_decrypt`, `envelope_parse`, `NotARecipientError`, `MalformedEnvelopeError`. The existing `DecryptionError` is already re-exported.

### Stanza Size Constants
Per-recipient stanza: 32 (pk_hash) + 1568 (kem_ct) + 48 (wrapped_dek) = 1648 bytes.
Fixed header: 4 (magic) + 1 (version) + 1 (suite) + 2 (count) + 12 (nonce) = 20 bytes.
Minimum envelope size: 20 + 1648 + 16 (AEAD tag) = 1684 bytes (single recipient, empty plaintext).

### Recipient Cap
D-04: Practical cap at 256 recipients. Enforce in `envelope_encrypt()` before any crypto operations. Raise `ValueError` or `CryptoError` if exceeded.

## Sources

### Primary (HIGH confidence)
- SDK source: `identity.py`, `crypto.py`, `_hkdf.py`, `exceptions.py`, `_handshake.py`, `_codec.py` -- all read and analyzed
- On-machine verification: ML-KEM-1024 keygen, encap/decap roundtrip, HKDF KEK derivation, AEAD wrap/unwrap, full envelope flow -- all tested successfully on 2026-04-01
- CONTEXT.md decisions D-01 through D-34 -- locked by user
- `.planning/research/ARCHITECTURE.md` -- module layout and data flow
- `.planning/research/STACK.md` -- zero-new-deps confirmation, API details
- `.planning/research/PITFALLS.md` -- KEM-then-Wrap pattern, nonce management, HKDF domain separation

### Secondary (MEDIUM confidence)
- liboqs-python GitHub -- `KeyEncapsulation` API signatures
- FIPS 203 -- ML-KEM-1024 parameters (pk=1568, sk=3168, ct=1568, ss=32)

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- zero new deps, all primitives verified on target machine
- Architecture: HIGH -- module layout dictated by CONTEXT.md decisions, patterns consistent with existing SDK
- Pitfalls: HIGH -- verified against FIPS 203, existing codebase patterns, and end-to-end testing
- Integration details: HIGH -- read all source files, tested crypto flows on machine

**Research date:** 2026-04-01
**Valid until:** 2026-05-01 (stable domain, no moving targets)
