# Phase 70: Crypto Foundation & Identity - Research

**Researched:** 2026-03-29
**Domain:** Python SDK crypto primitives, identity management, project scaffolding
**Confidence:** HIGH

## Summary

Phase 70 establishes the Python SDK's crypto foundation. The SDK must produce byte-identical output to the C++ node for SHA3-256, ChaCha20-Poly1305, HKDF-SHA256, ML-DSA-87 signing, and the canonical signing input. The critical insight is that all crypto primitives map directly to well-established Python libraries: `hashlib.sha3_256()` for hashing, `liboqs-python` for ML-DSA-87 (same underlying C library as the node), and `PyNaCl` for ChaCha20-Poly1305 AEAD. The one gap is HKDF-SHA256: PyNaCl does NOT expose libsodium's `crypto_kdf_hkdf_sha256_*` functions. HKDF must be implemented using Python's stdlib `hmac` module per RFC 5869, which produces byte-identical output since the standard is fully defined in terms of HMAC-SHA256.

The project scaffolding (pyproject.toml, exception hierarchy, FlatBuffers codegen) is straightforward. flatc 25.2.10 is available in the build tree. liboqs 0.15.0 and libsodium 1.0.21 are installed system-wide. Python 3.14 is available, exceeding the 3.10+ minimum.

**Primary recommendation:** Implement crypto.py (SHA3-256, AEAD, HKDF, signing input), identity.py (ML-DSA-87 keypair management), and wire.py (FlatBuffers encode/decode) with test vectors extracted from a purpose-built C++ generator binary. The HKDF gap must use pure-Python stdlib implementation, not a third-party library.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Raw binary key file format -- .key = 4896 bytes ML-DSA-87 secret, .pub = 2592 bytes public
- **D-02:** SSH-style sibling convention -- path points to .key file, .pub is derived sibling
- **D-03:** User-specified path only -- no default location
- **D-04:** In-memory identities supported -- Identity.generate(), Identity.load(path), Identity.generate_and_save(path)
- **D-05:** PQ crypto: liboqs-python
- **D-06:** Symmetric crypto: PyNaCl -- ChaCha20-Poly1305 IETF + HKDF-SHA256
- **D-07:** SHA3-256: hashlib (stdlib)
- **D-08:** FlatBuffers: official flatbuffers pip package + flatc --python codegen
- **D-09:** Package name: chromatindb
- **D-10:** Flat module structure: chromatindb.crypto, chromatindb.identity, chromatindb.wire, chromatindb.exceptions, chromatindb.generated/
- **D-11:** Async-first with sync wrapper (Phase 70 crypto ops are naturally synchronous)
- **D-12:** snake_case naming (PEP 8)
- **D-13:** Exception hierarchy: ChromatinError > CryptoError, IdentityError, WireError, ProtocolError
- **D-14:** Python 3.10+ minimum
- **D-15:** Full type hints, no runtime checking
- **D-16:** Google-style docstrings
- **D-17:** Extract test vectors from C++ test suite
- **D-18:** Standalone C++ test vector generator binary, outputs JSON to sdk/python/tests/vectors/
- **D-19:** Test framework: pytest
- **D-20:** Commit generated FlatBuffers Python code to sdk/python/chromatindb/generated/
- **D-21:** Single source of truth: flatc reads from db/schemas/transport.fbs and blob.fbs
- **D-22:** Linter/formatter: ruff
- **D-23:** Type checker: mypy
- **D-24:** Dependency pins: liboqs-python~=0.11.0, pynacl~=1.5.0, flatbuffers~=24.3
- **D-25:** Flat layout: sdk/python/chromatindb/, pyproject.toml at sdk/python/
- **D-26:** Future SDKs: sdk/rust/, sdk/c++/, sdk/js/, sdk/c/

### Claude's Discretion
- Internal module organization within crypto.py (single file vs split)
- FlatBuffers generated code directory structure under generated/
- pytest configuration details (conftest.py, fixtures)
- ruff rule selection and configuration
- Test vector generator binary location and build integration
- __init__.py re-exports (public API surface)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| XPORT-01 | SDK generates and persists ML-DSA-87 client identity keypairs | Identity class using liboqs-python Signature("ML-DSA-87"), raw binary file I/O matching C++ format |
| XPORT-06 | SDK encodes/decodes FlatBuffers TransportMessage wire format | flatc --python --python-typing --gen-onefile codegen from db/schemas/, builder/reader patterns documented |
| PKG-01 | SDK is pip-installable (pyproject.toml, sdk/python/ layout) | pyproject.toml structure, dependency pins, flat layout verified |
| PKG-03 | SDK has typed exception hierarchy mapping protocol errors | Exception class tree documented, maps to C++ error conditions |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs-python | 0.14.1 (PyPI latest) | ML-DSA-87 sign/verify, ML-KEM-1024 (Phase 71) | Official OQS Python binding, same C library as node (liboqs 0.15.0 system-installed) |
| PyNaCl | 1.6.2 | ChaCha20-Poly1305 IETF AEAD | Official libsodium binding, same underlying C library as node |
| flatbuffers | 25.12.19 | FlatBuffers runtime for Python | Official Google package, matches flatc 25.2.10 in build tree |
| hashlib (stdlib) | N/A | SHA3-256 hashing | Zero-dependency, NIST standard, byte-identical to liboqs OQS_SHA3_sha3_256 |
| hmac (stdlib) | N/A | HKDF-SHA256 extract/expand | RFC 5869 implementation via HMAC-SHA256, byte-identical to libsodium |
| struct (stdlib) | N/A | Little-endian integer encoding | For build_signing_input ttl/timestamp encoding |

### Supporting (Dev)
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| pytest | latest | Test framework | All SDK tests |
| ruff | latest | Linter + formatter | CI and pre-commit |
| mypy | latest | Type checking | CI, type safety validation |

### Version Pin Correction (IMPORTANT)

D-24 specifies `liboqs-python~=0.11.0` but the latest PyPI version is **0.14.1** (September 2025). The ~=0.11.0 pin would fail to install since 0.11.x is not available on PyPI (versions jump). The v0.12.0 release introduced a **breaking change**: failed operations now raise `RuntimeError` instead of returning 0. Additionally, v0.12.0 added ML-DSA context string API methods.

**Recommendation:** Use `liboqs-python~=0.14.0` to get the latest stable release. The `pynacl~=1.5.0` pin is compatible (1.6.2 satisfies ~=1.5.0 since it is >=1.5.0,<2.0.0). The `flatbuffers~=24.3` pin should be updated to `flatbuffers~=25.12` to match the flatc 25.2.10 compiler version in the build tree.

**Installation:**
```bash
cd sdk/python
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
```

## Architecture Patterns

### Recommended Project Structure
```
sdk/python/
    pyproject.toml
    chromatindb/
        __init__.py              # Public API re-exports
        crypto.py                # SHA3-256, AEAD, HKDF, build_signing_input
        identity.py              # Identity class (generate/load/save)
        wire.py                  # FlatBuffers encode/decode wrappers
        exceptions.py            # Exception hierarchy
        _hkdf.py                 # Pure-Python HKDF-SHA256 (internal)
        generated/
            __init__.py
            transport_generated.py  # flatc output
            blob_generated.py       # flatc output
    tests/
        __init__.py
        conftest.py              # Shared fixtures
        vectors/                 # JSON test vectors from C++ generator
            hash_vectors.json
            aead_vectors.json
            kdf_vectors.json
            signing_input_vectors.json
        test_crypto.py
        test_identity.py
        test_wire.py
        test_exceptions.py
tools/
    test_vector_generator.cpp    # C++ binary, links chromatindb_lib
```

### Pattern 1: HKDF-SHA256 via Python stdlib
**What:** Pure-Python implementation of RFC 5869 HKDF-SHA256 using `hmac` + `hashlib`.
**When to use:** Always for this project. PyNaCl does NOT expose libsodium's `crypto_kdf_hkdf_sha256_*` functions.
**Why byte-identical:** HKDF is defined as: Extract = HMAC-SHA256(salt, IKM), Expand = HMAC-SHA256(PRK, info || counter). Both libsodium and Python `hmac` implement HMAC-SHA256 per RFC 2104. Same input, same output.
**Example:**
```python
# Source: RFC 5869 + Python stdlib docs
import hmac
import hashlib

def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    """HKDF-SHA256 Extract: PRK = HMAC-SHA256(salt, IKM)."""
    if not salt:
        salt = b'\x00' * 32  # RFC 5869 Section 2.2
    return hmac.new(salt, ikm, hashlib.sha256).digest()

def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    """HKDF-SHA256 Expand: OKM from PRK + info."""
    hash_len = 32  # SHA-256 output
    n = (length + hash_len - 1) // hash_len
    okm = b''
    t = b''
    for i in range(1, n + 1):
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
    return okm[:length]
```

**CRITICAL NOTE on empty salt:** The C++ node passes empty salt to libsodium's extract. libsodium's implementation, per RFC 5869 Section 2.2, uses a zero-filled salt of hash length (32 bytes) when salt is empty. The Python implementation must do the same. The C++ code confirms this: `salt.empty() ? &dummy : salt.data(), salt.size()` -- it passes size 0, and libsodium internally uses the zero salt.

### Pattern 2: ML-DSA-87 Identity Management
**What:** Wrapper around liboqs-python Signature class with raw binary key file I/O.
**When to use:** All identity operations.
**Example:**
```python
# Source: liboqs-python examples/sig.py + C++ identity.cpp reference
import oqs

class Identity:
    def __init__(self, public_key: bytes, _signer: oqs.Signature | None = None):
        self._public_key = public_key
        self._signer = _signer  # None for verify-only
        self._namespace = hashlib.sha3_256(public_key).digest()

    @classmethod
    def generate(cls) -> "Identity":
        signer = oqs.Signature("ML-DSA-87")
        public_key = signer.generate_keypair()
        return cls(public_key, signer)

    @classmethod
    def load(cls, key_path: str | Path) -> "Identity":
        key_path = Path(key_path)
        pub_path = key_path.with_suffix(".pub")
        secret_key = key_path.read_bytes()
        public_key = pub_path.read_bytes()
        if len(secret_key) != 4896:
            raise KeyFileError(f"Invalid secret key size: {len(secret_key)}")
        if len(public_key) != 2592:
            raise KeyFileError(f"Invalid public key size: {len(public_key)}")
        signer = oqs.Signature("ML-DSA-87", secret_key)
        return cls(public_key, signer)
```

### Pattern 3: Canonical Signing Input
**What:** SHA3-256(namespace || data || ttl_le32 || timestamp_le64) -- must match C++ byte-for-byte.
**When to use:** Before signing any blob data.
**Example:**
```python
# Source: db/wire/codec.cpp build_signing_input()
import hashlib
import struct

def build_signing_input(
    namespace_id: bytes,  # 32 bytes
    data: bytes,
    ttl: int,             # uint32
    timestamp: int,       # uint64, seconds
) -> bytes:
    """Build canonical signing input matching C++ node exactly."""
    h = hashlib.sha3_256()
    h.update(namespace_id)
    h.update(data)
    h.update(struct.pack('<I', ttl))       # Little-endian uint32
    h.update(struct.pack('<Q', timestamp)) # Little-endian uint64
    return h.digest()
```

### Pattern 4: FlatBuffers Wire Format
**What:** Encode/decode TransportMessage using generated Python code.
**Example:**
```python
# Source: flatc --python generated code
import flatbuffers
from chromatindb.generated.transport_generated import (
    TransportMessage, TransportMsgType,
    TransportMessageStart, TransportMessageEnd,
    TransportMessageAddType, TransportMessageAddPayload,
    TransportMessageAddRequestId, TransportMessageStartPayloadVector,
)

def encode_transport_message(
    msg_type: int, payload: bytes, request_id: int = 0
) -> bytes:
    builder = flatbuffers.Builder(len(payload) + 64)
    payload_vec = builder.CreateByteVector(payload)
    TransportMessageStart(builder)
    TransportMessageAddType(builder, msg_type)
    TransportMessageAddPayload(builder, payload_vec)
    TransportMessageAddRequestId(builder, request_id)
    msg = TransportMessageEnd(builder)
    builder.Finish(msg)
    return bytes(builder.Output())

def decode_transport_message(data: bytes) -> tuple[int, bytes, int]:
    msg = TransportMessage.GetRootAs(data)
    payload = bytes(msg.PayloadAsNumpy()) if not msg.PayloadIsNone() else b''
    return msg.Type(), payload, msg.RequestId()
```

### Anti-Patterns to Avoid
- **Using `cryptography` library for HKDF:** Heavy OpenSSL dependency. Project forbids OpenSSL. Use stdlib `hmac`.
- **Computing blob_hash from Python FlatBuffer encoding:** FlatBuffers is NOT deterministic cross-language. Always use server-returned blob_hash.
- **Storing keys in PEM/DER format:** C++ node uses raw binary. SDK must match exactly -- no headers, no Base64.
- **Using liboqs-python < 0.12.0:** Breaking change at 0.12.0 (errors raise RuntimeError vs return 0).
- **Microsecond timestamps:** All timestamps are SECONDS. No /1000000 dividers.
- **Forgetting empty salt HKDF behavior:** Empty salt must produce zero-filled 32-byte salt per RFC 5869.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| ML-DSA-87 signing | Custom PQ crypto | liboqs-python `oqs.Signature("ML-DSA-87")` | Quantum-resistant, FIPS 204 compliant, same C library as node |
| ChaCha20-Poly1305 | Custom AEAD | PyNaCl `nacl.bindings.crypto_aead` | Constant-time, battle-tested, same C library as node |
| SHA3-256 | Custom hash | `hashlib.sha3_256()` | Stdlib, NIST standard, zero dependencies |
| FlatBuffers encoding | Manual binary packing | flatc generated code + `flatbuffers` runtime | Schema-driven, verified compatible with C++ |
| HKDF-SHA256 | Third-party library | Pure-Python `hmac` + `hashlib` (RFC 5869) | 15 lines of code, zero dependencies, byte-identical to libsodium |

**Key insight:** Every crypto primitive in the SDK maps to either a standard library or a library that wraps the same C library the node uses. This makes byte-identical output achievable, not aspirational.

## Common Pitfalls

### Pitfall 1: HKDF Empty Salt Semantics
**What goes wrong:** Passing `b''` as salt to HMAC directly instead of replacing with 32 zero bytes.
**Why it happens:** RFC 5869 Section 2.2 specifies "if not provided, [salt] is set to a string of HashLen zeros" but the code path where salt_len=0 needs explicit handling.
**How to avoid:** In the Python HKDF extract, always check `if not salt: salt = b'\x00' * 32`.
**Warning signs:** HKDF output differs from C++ test vector for empty-salt case.

### Pitfall 2: liboqs-python Secret Key Reconstruction
**What goes wrong:** Loading a saved .key file but not having the public key available for the Signature constructor.
**Why it happens:** `oqs.Signature("ML-DSA-87", secret_key=sk)` restores signing capability, but the public key must be loaded separately from the .pub file.
**How to avoid:** Always load both .key and .pub files. The Identity.load() method reads both and validates sizes.
**Warning signs:** `generate_keypair()` called after loading secret key, overwriting the saved key.

### Pitfall 3: FlatBuffers Python Byte Vector Access
**What goes wrong:** Using `Payload(j)` in a loop (O(n) with constant overhead per call) instead of `PayloadAsNumpy()`.
**Why it happens:** The generated API has per-element access methods that look like the natural API.
**How to avoid:** Use `bytes(msg.PayloadAsNumpy())` for bulk byte vector access. numpy dependency is pulled in by flatbuffers automatically.
**Warning signs:** Slow FlatBuffer decoding for large payloads.

### Pitfall 4: Endianness in Signing Input
**What goes wrong:** Using big-endian for ttl/timestamp in build_signing_input.
**Why it happens:** The protocol uses big-endian for framing, creating an assumption that all integers are BE.
**How to avoid:** The canonical signing input uses LITTLE-endian for ttl (uint32) and timestamp (uint64). This matches the C++ source. Use `struct.pack('<I', ttl)` and `struct.pack('<Q', timestamp)`.
**Warning signs:** Signing input hash doesn't match C++ test vector.

### Pitfall 5: Python 3.14 venv Requirement
**What goes wrong:** `pip install` fails with "externally-managed-environment" error.
**Why it happens:** Python 3.14 on Arch Linux (brew) enforces PEP 668 externally managed environments.
**How to avoid:** Always create a venv: `python3 -m venv sdk/python/.venv && source sdk/python/.venv/bin/activate`.
**Warning signs:** First `pip install` attempt fails.

### Pitfall 6: liboqs-python Version Mismatch
**What goes wrong:** D-24 specifies `~=0.11.0` but no 0.11.x exists on PyPI.
**Why it happens:** Context session used stale version information.
**How to avoid:** Pin `liboqs-python~=0.14.0` (latest stable, verified on PyPI 2025-09-02). Test vector compatibility is guaranteed since both SDK and node use the same liboqs C library (0.15.0 installed system-wide).
**Warning signs:** `pip install` resolution failure.

### Pitfall 7: ML-DSA-87 Signature Size Variability
**What goes wrong:** Assuming signature is always exactly 4627 bytes.
**Why it happens:** Header says `MAX_SIGNATURE_SIZE = 4627` which looks like a constant.
**How to avoid:** ML-DSA-87 signatures are variable length UP TO 4627 bytes. The C++ code resizes: `signature.resize(signature_len)`. Python code must accept the actual length returned by `signer.sign()`.
**Warning signs:** Hardcoded signature buffer sizes causing truncation.

## Code Examples

### pyproject.toml Structure
```toml
# Source: PEP 621 + project decisions D-09, D-14, D-22, D-23, D-24, D-25
[build-system]
requires = ["setuptools>=68.0"]
build-backend = "setuptools.backends._legacy:_Backend"

[project]
name = "chromatindb"
version = "0.1.0"
description = "Python SDK for chromatindb"
requires-python = ">=3.10"
dependencies = [
    "liboqs-python~=0.14.0",
    "pynacl~=1.5.0",
    "flatbuffers~=25.12",
]

[project.optional-dependencies]
dev = [
    "pytest",
    "ruff",
    "mypy",
]

[tool.setuptools.packages.find]
where = ["."]
include = ["chromatindb*"]

[tool.ruff]
target-version = "py310"
line-length = 88

[tool.ruff.lint]
select = ["E", "F", "W", "I", "N", "UP", "B", "A", "SIM", "TCH", "RUF"]

[tool.mypy]
python_version = "3.10"
strict = true
warn_return_any = true
warn_unused_configs = true
```

### Exception Hierarchy
```python
# Source: D-13 decisions
class ChromatinError(Exception):
    """Base exception for all chromatindb SDK errors."""

class CryptoError(ChromatinError):
    """Base for cryptographic operation errors."""

class SignatureError(CryptoError):
    """ML-DSA-87 signing or verification failed."""

class DecryptionError(CryptoError):
    """AEAD decryption or authentication failed."""

class KeyDerivationError(CryptoError):
    """HKDF key derivation failed."""

class IdentityError(ChromatinError):
    """Base for identity management errors."""

class KeyFileError(IdentityError):
    """Key file missing, corrupt, or wrong size."""

class NamespaceError(IdentityError):
    """Namespace derivation or validation failed."""

class WireError(ChromatinError):
    """Base for wire format errors."""

class DecodeError(WireError):
    """FlatBuffer decode or verification failed."""

class ProtocolError(ChromatinError):
    """Base for protocol-level errors (Phase 71+)."""
```

### ChaCha20-Poly1305 AEAD via PyNaCl
```python
# Source: PyNaCl nacl.bindings.crypto_aead API
from nacl.bindings import (
    crypto_aead_chacha20poly1305_ietf_encrypt,
    crypto_aead_chacha20poly1305_ietf_decrypt,
)

AEAD_KEY_SIZE = 32
AEAD_NONCE_SIZE = 12
AEAD_TAG_SIZE = 16

def aead_encrypt(plaintext: bytes, ad: bytes, nonce: bytes, key: bytes) -> bytes:
    """Encrypt with ChaCha20-Poly1305 IETF. Returns ciphertext + tag."""
    if len(nonce) != AEAD_NONCE_SIZE:
        raise CryptoError(f"AEAD nonce must be {AEAD_NONCE_SIZE} bytes")
    if len(key) != AEAD_KEY_SIZE:
        raise CryptoError(f"AEAD key must be {AEAD_KEY_SIZE} bytes")
    return crypto_aead_chacha20poly1305_ietf_encrypt(
        plaintext, ad if ad else None, nonce, key
    )

def aead_decrypt(ciphertext: bytes, ad: bytes, nonce: bytes, key: bytes) -> bytes | None:
    """Decrypt with ChaCha20-Poly1305 IETF. Returns None on auth failure."""
    if len(ciphertext) < AEAD_TAG_SIZE:
        return None
    try:
        return crypto_aead_chacha20poly1305_ietf_decrypt(
            ciphertext, ad if ad else None, nonce, key
        )
    except Exception:
        return None
```

### Test Vector Generator Binary (C++)
```cpp
// Source: project decisions D-17, D-18
// Location: tools/test_vector_generator.cpp
// Build: links chromatindb_lib, outputs JSON to stdout
// Usage: ./test_vector_generator > sdk/python/tests/vectors/vectors.json
//
// Outputs known-answer vectors for:
//   - SHA3-256 (empty, "chromatindb", binary patterns)
//   - HKDF-SHA256 (with salt, empty salt, different contexts)
//   - ChaCha20-Poly1305 (encrypt/decrypt with known key/nonce/plaintext/ad)
//   - build_signing_input (known namespace/data/ttl/timestamp -> expected hash)
//   - ML-DSA-87 (keypair -> sign known message -> signature + pubkey for verify)
```

### FlatBuffers Codegen Command
```bash
# flatc is available at: build/_deps/flatbuffers-build/flatc (v25.2.10)
# Source schemas: db/schemas/transport.fbs, db/schemas/blob.fbs
build/_deps/flatbuffers-build/flatc \
    --python --python-typing --gen-onefile \
    -o sdk/python/chromatindb/generated/ \
    db/schemas/transport.fbs db/schemas/blob.fbs
```

This produces:
- `transport_generated.py` + `transport_generated.pyi`
- `blob_generated.py` + `blob_generated.pyi`
- `__init__.py` (empty)

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Dilithium (pre-standard) | ML-DSA-87 (FIPS 204) | 2024 | Algorithm name in liboqs changed; use "ML-DSA-87" not "Dilithium5" |
| liboqs-python errors return 0 | Errors raise RuntimeError | v0.12.0 (Jan 2025) | Use try/except, not return code checking |
| PyNaCl pre-1.6 | PyNaCl 1.6.2 (libsodium 1.0.20) | Jan 2026 | CVE-2025-69277 fix, Python 3.14 support |
| flatbuffers multi-file output | flatc --gen-onefile | v25.x | Single file per schema, cleaner imports |

**Deprecated/outdated:**
- `oqs.Signature("Dilithium5")`: Use `oqs.Signature("ML-DSA-87")` (FIPS 204 finalized name)
- liboqs-python 0.11.x: Does not exist on PyPI. Earliest likely available is 0.12.0+.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Python 3 | Everything | Yes | 3.14.3 | -- |
| liboqs (C library) | liboqs-python | Yes | 0.15.0 (system-wide) | liboqs-python auto-builds if missing |
| libsodium | PyNaCl | Yes | 1.0.21 (system-wide) | PyNaCl bundles 1.0.20 |
| flatc | FlatBuffers codegen | Yes | 25.2.10 (build tree) | -- |
| pip/venv | Package management | Yes | 26.0 | -- |

**Missing dependencies with no fallback:** None.

**Missing dependencies with fallback:** None -- all dependencies are available.

**Note:** Python environment is externally managed (brew PEP 668). A venv MUST be created for SDK development:
```bash
python3 -m venv sdk/python/.venv
source sdk/python/.venv/bin/activate
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest (latest, installed via `pip install -e ".[dev]"`) |
| Config file | sdk/python/pyproject.toml `[tool.pytest.ini_options]` section |
| Quick run command | `cd sdk/python && python -m pytest tests/ -x -q` |
| Full suite command | `cd sdk/python && python -m pytest tests/ -v` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| XPORT-01 | Generate ML-DSA-87 keypair, save/load key files, key sizes correct | unit | `pytest tests/test_identity.py -x` | Wave 0 |
| XPORT-01 | Cross-language: C++ node can load SDK-generated keys | integration | C++ test vector generator binary validates | Wave 0 |
| XPORT-06 | Encode/decode TransportMessage FlatBuffer | unit | `pytest tests/test_wire.py -x` | Wave 0 |
| PKG-01 | pip install -e . succeeds, import chromatindb works | smoke | `pip install -e . && python -c "import chromatindb"` | Wave 0 |
| PKG-03 | Exception hierarchy correct, all exceptions importable | unit | `pytest tests/test_exceptions.py -x` | Wave 0 |
| (implicit) | SHA3-256 matches C++ test vectors | unit | `pytest tests/test_crypto.py::test_sha3_256 -x` | Wave 0 |
| (implicit) | HKDF-SHA256 matches C++ test vectors | unit | `pytest tests/test_crypto.py::test_hkdf -x` | Wave 0 |
| (implicit) | ChaCha20-Poly1305 matches C++ test vectors | unit | `pytest tests/test_crypto.py::test_aead -x` | Wave 0 |
| (implicit) | build_signing_input matches C++ test vectors | unit | `pytest tests/test_crypto.py::test_signing_input -x` | Wave 0 |
| (implicit) | Namespace derivation matches C++ | unit | `pytest tests/test_identity.py::test_namespace -x` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd sdk/python && python -m pytest tests/ -x -q`
- **Per wave merge:** `cd sdk/python && python -m pytest tests/ -v`
- **Phase gate:** Full suite green + ruff check + mypy check before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `sdk/python/tests/conftest.py` -- shared fixtures (vector loading, temp directories)
- [ ] `sdk/python/tests/vectors/` -- JSON test vectors (generated by C++ binary)
- [ ] `sdk/python/pyproject.toml` -- pytest config section
- [ ] All test files (test_crypto.py, test_identity.py, test_wire.py, test_exceptions.py)
- [ ] C++ test vector generator binary (`tools/test_vector_generator.cpp`)

## Open Questions

1. **liboqs-python version pin**
   - What we know: D-24 says ~=0.11.0, but 0.14.1 is latest on PyPI and 0.11.x likely doesn't exist
   - What's unclear: Whether user wants to pin to latest or a specific older version
   - Recommendation: Use ~=0.14.0 and note the correction. The underlying liboqs C library is 0.15.0 system-wide regardless.

2. **liboqs-python public key from loaded secret key**
   - What we know: `oqs.Signature("ML-DSA-87", secret_key=sk)` reconstructs signing, but getting the public key requires calling `generate_keypair()` which would overwrite the loaded key
   - What's unclear: Whether liboqs-python provides `export_public_key()` after loading only a secret key
   - Recommendation: Always load BOTH .key and .pub files (matching C++ pattern). Store public_key separately in Identity class. This is already the planned approach per D-01/D-02.

3. **flatbuffers version compatibility**
   - What we know: flatc is 25.2.10, D-24 pins flatbuffers~=24.3
   - What's unclear: Whether 24.x runtime is compatible with 25.x generated code
   - Recommendation: Pin `flatbuffers~=25.12` to match the compiler major version. Generated code and runtime should be version-aligned.

## Sources

### Primary (HIGH confidence)
- `/home/mika/dev/chromatin-protocol/db/crypto/signing.h` + `signing.cpp` -- ML-DSA-87 key sizes, API
- `/home/mika/dev/chromatin-protocol/db/crypto/hash.h` + `hash.cpp` -- SHA3-256 implementation
- `/home/mika/dev/chromatin-protocol/db/crypto/aead.h` + `aead.cpp` -- ChaCha20-Poly1305 implementation
- `/home/mika/dev/chromatin-protocol/db/crypto/kdf.h` + `kdf.cpp` -- HKDF-SHA256 implementation, empty salt handling
- `/home/mika/dev/chromatin-protocol/db/identity/identity.h` + `identity.cpp` -- Key file I/O, namespace derivation
- `/home/mika/dev/chromatin-protocol/db/wire/codec.h` + `codec.cpp` -- build_signing_input, canonical signing
- `/home/mika/dev/chromatin-protocol/db/schemas/transport.fbs` + `blob.fbs` -- Wire format schemas
- System verification: liboqs 0.15.0 at /usr/local/lib, libsodium 1.0.21 at /usr/lib, flatc 25.2.10 in build tree

### Secondary (MEDIUM confidence)
- [liboqs-python PyPI](https://pypi.org/project/liboqs-python/) -- Version 0.14.1, September 2025
- [PyNaCl PyPI](https://pypi.org/project/PyNaCl/) -- Version 1.6.2, January 2026
- [flatbuffers PyPI](https://pypi.org/project/flatbuffers/) -- Version 25.12.19, December 2025
- [liboqs-python sig.py example](https://github.com/open-quantum-safe/liboqs-python/blob/main/examples/sig.py) -- API usage patterns
- [PyNaCl crypto_aead.py bindings](https://github.com/pyca/pynacl/blob/main/src/nacl/bindings/crypto_aead.py) -- ChaCha20-Poly1305 IETF API
- [PyNaCl bindings directory listing](https://github.com/pyca/pynacl/tree/main/src/nacl/bindings) -- Confirmed NO crypto_kdf module exists

### Tertiary (LOW confidence)
- [liboqs-python CHANGES.md](https://github.com/open-quantum-safe/liboqs-python/blob/main/CHANGES.md) -- Version history (showed 0.12.0 as latest in changelog, but PyPI has 0.14.1)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- All libraries verified on PyPI with exact versions, system dependencies confirmed installed
- Architecture: HIGH -- C++ reference implementations read line-by-line, Python equivalents verified with stdlib
- Pitfalls: HIGH -- HKDF gap discovered and documented, version pin issues identified, empty salt semantics traced through C++ source
- Test vectors: HIGH -- C++ test files examined, test vector generator approach validated against existing test patterns

**Research date:** 2026-03-29
**Valid until:** 2026-04-28 (stable domain, 30 days)
