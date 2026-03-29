# Stack Research: v1.6.0 Python SDK

**Domain:** Python client SDK for post-quantum secure database protocol
**Researched:** 2026-03-29
**Confidence:** HIGH

## Scope

This research covers ONLY what is needed for the v1.6.0 Python SDK: PQ crypto bindings (liboqs-python), symmetric crypto (PyNaCl for ChaCha20-Poly1305 AEAD), key derivation (HKDF-SHA256), hashing (SHA3-256), wire format (FlatBuffers Python runtime), networking (asyncio), and packaging (pyproject.toml). The existing C++ node/relay stack is validated and locked -- not re-researched.

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| liboqs-python | 0.14.1 | ML-KEM-1024 key encapsulation + ML-DSA-87 signing | Only maintained Python wrapper for liboqs. Provides `KeyEncapsulation("ML-KEM-1024")` and `Signature("ML-DSA-87")` classes that directly mirror the C++ liboqs usage. Auto-downloads liboqs shared library at runtime if not pre-installed. Requires Python >=3.9. |
| PyNaCl | 1.6.2 | ChaCha20-Poly1305 IETF AEAD encryption/decryption | Python binding to libsodium. Exposes `nacl.bindings.crypto_aead_chacha20poly1305_ietf_encrypt()` and `_decrypt()` at the low-level bindings layer -- exactly the IETF construction (RFC 8439) that the C++ node uses. Bundles libsodium 1.0.20 -- no separate install needed. |
| flatbuffers | 25.12.19 | FlatBuffers Python runtime for TransportMessage encode/decode | Official Google FlatBuffers Python runtime. Pure Python, no native dependencies. Builder supports `ForceDefaults(True)` for deterministic encoding matching C++ `ForceDefaults(true)`. |
| hashlib (stdlib) | Python 3.11+ | SHA3-256 hashing for namespace derivation and canonical signing input | Part of Python standard library since 3.6. `hashlib.sha3_256()` backed by OpenSSL or HACL* fallback. Zero dependencies. Used for: `SHA3-256(pubkey)` = namespace, `SHA3-256(ns||data||ttl_le32||ts_le64)` = signing digest, session fingerprint computation. |
| hmac (stdlib) | Python 3.11+ | HKDF-SHA256 extract/expand implementation | Part of Python standard library. Combined with hashlib provides RFC 5869 HKDF without any external dependency. |
| asyncio (stdlib) | Python 3.11+ | Async TCP client with `asyncio.open_connection()` streams API | Standard library async networking. `StreamReader`/`StreamWriter` pattern matches the frame-oriented protocol well. Supports `await reader.readexactly(n)` for length-prefixed frame reading. No third-party async framework needed. |
| pytest | 8.x | Test framework | Standard Python test framework. Supports fixtures, parametrize, async tests (via pytest-asyncio). Matches the project's test-first approach with Catch2 on the C++ side. |

### Supporting Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| pytest-asyncio | 0.24.x | Async test support for pytest | Always -- needed to test async connection/handshake/messaging code with `@pytest.mark.asyncio` |
| mypy | 1.x | Static type checking | Development -- the SDK should be fully typed for developer experience. Type stubs exist for PyNaCl and flatbuffers. |
| ruff | 0.8.x | Linting and formatting | Development -- replaces flake8 + black + isort in a single fast tool. |

### Development Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| flatc (compiler) | Generate Python classes from .fbs schemas | Run `flatc --python -o sdk/python/src/chromatindb/ db/schemas/transport.fbs db/schemas/blob.fbs` to generate `wire/` Python modules. The generated code depends on the `flatbuffers` pip package. Version must match the flatbuffers runtime. Use the flatc binary already built by CMake in `build/_deps/flatbuffers-src/`. |
| pip (editable install) | Development workflow | `pip install -e "sdk/python[dev]"` for editable install with dev dependencies. |

## Python Version

**Minimum: Python 3.11**

Rationale:
- liboqs-python requires >=3.9 (floor)
- Python 3.11 added ExceptionGroup (useful for connection errors), significant performance improvements (10-60% faster), and is the oldest version still receiving security updates in 2026
- Python 3.10 EOL is October 2026 -- cutting it too close
- hashlib SHA3-256 available since 3.6, asyncio streams since 3.4 -- not constraining
- Target the range 3.11-3.13 (3.14 in beta, not yet stable)

## HKDF-SHA256: Stdlib Implementation (No External Dependency)

**Critical decision: implement HKDF-SHA256 using only `hmac` + `hashlib` from stdlib.**

PyNaCl does NOT expose HKDF bindings despite bundling libsodium 1.0.20 which has `crypto_kdf_hkdf_sha256_*`. The PyNaCl bindings directory contains `crypto_aead.py`, `crypto_box.py`, `crypto_generichash.py`, etc. but no `crypto_kdf.py`. This is a confirmed gap -- checked the PyNaCl source tree on GitHub.

HKDF-SHA256 per RFC 5869 is trivially implementable in ~20 lines of Python using `hmac.new(key, msg, 'sha256')`. The C++ code uses libsodium's `crypto_kdf_hkdf_sha256_extract()` and `_expand()` which are the exact same RFC 5869 algorithm. No need to pull in the heavyweight `cryptography` package (which brings OpenSSL, Rust build toolchain, and C compilation) for 20 lines of HMAC operations.

```python
# Reference implementation -- ~20 lines, zero dependencies beyond stdlib
import hmac
import hashlib

def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    """RFC 5869 Section 2.2: HKDF-Extract"""
    if not salt:
        salt = b'\x00' * 32  # HashLen zeros per RFC 5869
    return hmac.new(salt, ikm, hashlib.sha256).digest()

def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 Section 2.3: HKDF-Expand"""
    hash_len = 32  # SHA-256 output
    n = (length + hash_len - 1) // hash_len
    okm = b''
    t = b''
    for i in range(1, n + 1):
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
    return okm[:length]
```

## Wire Protocol Integration Details

### PQ Handshake (SDK is initiator)

The SDK always acts as the handshake initiator connecting to a relay. The exact sequence from the C++ source (`db/net/handshake.cpp`):

1. **KemPubkey message (raw, unencrypted):** Generate ML-KEM-1024 ephemeral keypair. Build FlatBuffers `TransportMessage` with `type=KemPubkey(1)`, payload = `[kem_pubkey:1568B][signing_pubkey:2592B]`. Send as raw bytes (no length prefix, no encryption).

2. **Receive KemCiphertext (raw):** Parse FlatBuffers `TransportMessage`, extract payload = `[ciphertext:1568B][responder_signing_pubkey:2592B]`. Call `KeyEncapsulation.decap_secret(ciphertext)` to recover shared secret.

3. **Key derivation (MUST match C++ implementation, NOT PROTOCOL.md):**
   - HKDF Extract: `prk = hkdf_extract(salt=b'', ikm=shared_secret)` -- NOTE: empty salt, not SHA3-256(pubkeys) as PROTOCOL.md states. The C++ code passes empty salt.
   - HKDF Expand: `init_to_resp = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)`
   - HKDF Expand: `resp_to_init = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)`
   - Session fingerprint: `SHA3-256(shared_secret || initiator_signing_pk || responder_signing_pk)` -- NOTE: this is a direct SHA3-256 hash, NOT an HKDF-expand with info3 as PROTOCOL.md describes.
   - Initiator: send_key = init_to_resp, recv_key = resp_to_init.

4. **AuthSignature (encrypted):** Sign session fingerprint with ML-DSA-87. Payload = `[pubkey_size:4B LE][signing_pubkey:2592B][signature]`. Wrap in `TransportMessage(type=AuthSignature(3))`, then AEAD-encrypt as a length-prefixed frame. Nonce counter starts at 0.

5. **Receive AuthSignature (encrypted):** Decrypt with recv_key (counter 0). Verify responder's ML-DSA-87 signature over session fingerprint. Nonce counters now at 1 for both directions.

### AEAD Frame Format

After handshake, all messages are:
```
[4 bytes: big-endian uint32 ciphertext_length]
[ciphertext_length bytes: ChaCha20-Poly1305 ciphertext + 16-byte tag]
```

Nonce format: `b'\x00\x00\x00\x00' + counter.to_bytes(8, 'big')` (4 zero bytes + 8-byte BE counter). Each direction maintains its own counter.

### FlatBuffers Usage

The SDK needs to encode/decode `TransportMessage` and `Blob` tables. Two approaches:

1. **Generated code from flatc:** Run `flatc --python` on `db/schemas/transport.fbs` and `db/schemas/blob.fbs`. Produces Python classes with typed accessors. Clean but adds a code generation step.

2. **Raw FlatBuffer builder:** Use `flatbuffers.Builder` directly. For the simple 3-field TransportMessage, manual construction is ~10 lines and avoids generated code. The Blob table is also simple (6 fields).

**Recommendation:** Use flatc-generated code. The generated classes provide type safety, match the C++ approach, and the flatc binary already exists in the build tree. Commit the generated files so users don't need flatc installed.

Builder must call `ForceDefaults(True)` to match the C++ `ForceDefaults(true)` for deterministic encoding. This is critical for signing -- the signing input includes FlatBuffer-encoded data that must be identical on both sides.

### Canonical Signing Input

For blob creation (write operation):
```python
# Build signing input: SHA3-256(namespace_id || data || ttl_le32 || timestamp_le64)
import struct
signing_input = namespace_id + data + struct.pack('<I', ttl) + struct.pack('<Q', timestamp)
digest = hashlib.sha3_256(signing_input).digest()
signature = signer.sign(digest)  # ML-DSA-87
```

## Package Structure

```
sdk/python/
  pyproject.toml
  src/
    chromatindb/
      __init__.py          # Public API: Client, Identity, Blob
      client.py            # Connection, handshake, request/response
      identity.py          # ML-DSA-87 keypair management
      crypto/
        __init__.py
        aead.py            # ChaCha20-Poly1305 encrypt/decrypt
        hkdf.py            # HKDF-SHA256 (stdlib hmac + hashlib)
        kem.py             # ML-KEM-1024 wrapper
        signing.py         # ML-DSA-87 wrapper
      wire/
        __init__.py
        transport.py       # TransportMessage encode/decode (flatc-generated)
        blob.py            # Blob encode/decode (flatc-generated)
        types.py           # TransportMsgType enum (flatc-generated or manual)
        framing.py         # Length-prefixed AEAD frame read/write
      protocol/
        __init__.py
        handshake.py       # PQ handshake initiator state machine
        messages.py        # Request/response builders for all 38 message types
  tests/
    conftest.py            # Fixtures: test identity, temp keys
    test_crypto.py         # AEAD, HKDF, signing round-trips
    test_wire.py           # FlatBuffers encode/decode
    test_handshake.py      # Handshake against real relay (integration)
    test_client.py         # Full client operations (integration)
```

### pyproject.toml

```toml
[build-system]
requires = ["setuptools>=68.0"]
build-backend = "setuptools.backends._legacy:_Backend"

[project]
name = "chromatindb"
version = "0.1.0"
description = "Python SDK for chromatindb post-quantum secure database"
requires-python = ">=3.11"
dependencies = [
    "liboqs-python>=0.14.0",
    "PyNaCl>=1.5.0",
    "flatbuffers>=24.0.0",
]

[project.optional-dependencies]
dev = [
    "pytest>=8.0",
    "pytest-asyncio>=0.24",
    "mypy>=1.0",
    "ruff>=0.8",
]

[tool.setuptools.packages.find]
where = ["src"]

[tool.pytest.ini_options]
asyncio_mode = "auto"

[tool.mypy]
strict = true

[tool.ruff]
target-version = "py311"
```

## Installation

```bash
# User install
pip install sdk/python/

# Developer install (editable with dev tools)
pip install -e "sdk/python/[dev]"

# Dependencies pulled automatically:
#   liboqs-python>=0.14.0  (auto-downloads liboqs C library if not found)
#   PyNaCl>=1.5.0          (bundles libsodium, no separate install)
#   flatbuffers>=24.0.0    (pure Python, no native deps)
```

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| PyNaCl (libsodium bindings) | PyCryptodome | PyCryptodome has ChaCha20-Poly1305 and HKDF, but the C++ node uses libsodium. Using the same underlying C library via PyNaCl ensures byte-for-byte AEAD compatibility. PyCryptodome uses its own C implementations which could theoretically produce different behavior around edge cases (empty AD, nonce format). |
| PyNaCl low-level bindings | PyNaCl high-level SecretBox | SecretBox uses XSalsa20-Poly1305 (24-byte nonce), not ChaCha20-Poly1305 IETF (12-byte nonce). The node protocol uses IETF ChaCha20-Poly1305 specifically. Must use `nacl.bindings.crypto_aead_chacha20poly1305_ietf_encrypt/decrypt`. |
| stdlib hmac+hashlib for HKDF | `cryptography` package HKDF | The `cryptography` package requires OpenSSL headers, Rust compiler, and C compilation during pip install. Massive dependency for 20 lines of HMAC. Our project constraint is "No OpenSSL" -- adding it through the Python side contradicts the philosophy. stdlib HKDF is trivial, correct, and testable against RFC 5869 test vectors. |
| stdlib hmac+hashlib for HKDF | `hkdf` pip package | Unmaintained (last release 2015). Only 50 lines of code. Not worth a dependency for something we can write inline with better typing. |
| flatbuffers (Python runtime) | Manual binary struct packing | The transport.fbs and blob.fbs schemas define the canonical wire format. Using the flatbuffers runtime ensures forward compatibility when schemas evolve. Manual struct.pack would be fragile and need updating if fields are added. The runtime is pure Python, no native deps. |
| asyncio streams | trio / anyio | asyncio is stdlib, zero dependency. The SDK connects to one relay at a time -- no need for trio's structured concurrency or anyio's multi-backend abstraction. YAGNI. |
| asyncio streams | raw socket | `asyncio.open_connection()` returns `StreamReader`/`StreamWriter` which provide `readexactly()` for frame parsing and `drain()` for backpressure. Raw sockets would require reimplementing buffering. |
| pytest | unittest (stdlib) | pytest is the de facto standard for Python testing. Fixtures, parametrize, and the pytest-asyncio plugin make async tests clean. unittest's class-based approach is more verbose for the same coverage. |
| setuptools | hatchling / flit / pdm | setuptools is the most widely used build backend, works everywhere, and the project has no complex build requirements. The SDK is pure Python -- any build backend works. Setuptools is the conservative choice. |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| `cryptography` package | Pulls in OpenSSL + Rust compiler. Project constraint: "No OpenSSL." Only needed if PyNaCl lacked IETF ChaCha20-Poly1305 -- but it has it via low-level bindings. | PyNaCl for AEAD, stdlib for HKDF |
| `pycryptodome` | Different C backend from the node's libsodium. Risk of subtle interop issues. Unnecessary when PyNaCl provides exactly what we need. | PyNaCl |
| `pysha3` | Only needed for Python <3.6 which lacks hashlib.sha3_256(). We require >=3.11. | `hashlib.sha3_256()` from stdlib |
| `pynacl` high-level `SecretBox` | Uses XSalsa20-Poly1305, not the protocol's ChaCha20-Poly1305 IETF. Wrong algorithm. | `nacl.bindings.crypto_aead_chacha20poly1305_ietf_*` |
| `requests` / `httpx` | chromatindb uses raw TCP with a custom binary protocol, not HTTP. | `asyncio.open_connection()` for TCP sockets |
| `protobuf` / `msgpack` | Wire format is FlatBuffers, matching the C++ node. Using a different serialization would be incompatible. | `flatbuffers` Python runtime |
| Synchronous socket (stdlib `socket`) | Async is needed for pub/sub notifications which arrive asynchronously while the client may be sending requests. Sync would block on recv, unable to send concurrently. | asyncio streams with reader/writer |

## API Design Pattern

The SDK should expose both sync and async interfaces:

```python
# Async (primary, used with asyncio)
async with chromatindb.connect("relay.example.com", 4201, identity) as client:
    ack = await client.write(data, ttl=3600)
    blob = await client.read(namespace, blob_hash)

# Sync (convenience wrapper for scripts)
with chromatindb.connect_sync("relay.example.com", 4201, identity) as client:
    ack = client.write(data, ttl=3600)
```

The sync wrapper uses `asyncio.run()` internally. The async API is primary because pub/sub requires concurrent send/receive.

## Critical Implementation Notes

### 1. PROTOCOL.md vs C++ Source Discrepancy

The PROTOCOL.md describes the PQ handshake HKDF with `salt = SHA3-256(initiator_signing_pubkey || responder_signing_pubkey)` and session fingerprint as HKDF-expand with `info3 = "chromatin-session-fp-v1"`.

The actual C++ implementation (`db/net/handshake.cpp`) uses:
- **Empty salt** for HKDF extract (not SHA3-256 of pubkeys)
- **Session fingerprint** = `SHA3-256(shared_secret || initiator_pk || responder_pk)` (direct hash, not HKDF expand)

**The SDK MUST match the C++ implementation, not the PROTOCOL.md.** The C++ is the ground truth because the relay runs the C++ code. This discrepancy should be fixed in PROTOCOL.md as part of the v1.6.0 documentation refresh.

### 2. KemPubkey/KemCiphertext Payload Format

The PROTOCOL.md describes KemPubkey as containing only the 1568-byte KEM public key. The C++ implementation actually sends `[kem_pubkey:1568B][signing_pubkey:2592B]` in the payload. Similarly, KemCiphertext payload is `[ciphertext:1568B][signing_pubkey:2592B]`.

The SDK must use the actual C++ format: both KEM messages include the signing pubkey.

### 3. Auth Payload Format

The AuthSignature payload is: `[pubkey_size:4B little-endian uint32][signing_pubkey:2592B][signature:variable]`. This format is used by both `encode_auth_payload()` and `decode_auth_payload()` in the C++ source.

### 4. Nonce Counter Continuity

After the PQ handshake, the send counter is at 1 (one auth message sent) and the recv counter is at 1 (one auth message received). All subsequent AEAD frames continue from counter 1, not 0.

### 5. request_id for Pipelining

Every request message must include a client-chosen `request_id` (uint32). The node echoes it on the response. The SDK should use a simple incrementing counter per connection. This enables pipelining: send multiple requests without waiting for responses, match by `request_id`.

### 6. liboqs-python Runtime Dependency

liboqs-python auto-downloads and compiles the liboqs C library on first import if not found. This requires CMake and a C compiler on the user's system. For production deployments, pre-install liboqs via system packages or provide a Docker image with liboqs pre-built. The SDK documentation should note this requirement.

## Dependency Count

| Dependency | Type | Why Needed | Transitive Deps |
|------------|------|-----------|-----------------|
| liboqs-python | Runtime | ML-KEM-1024 + ML-DSA-87 PQ crypto | liboqs C library (auto-built) |
| PyNaCl | Runtime | ChaCha20-Poly1305 IETF AEAD | cffi, libsodium (bundled) |
| flatbuffers | Runtime | FlatBuffers Python runtime | None (pure Python) |
| **Total: 3 runtime deps** | | | |

Everything else (SHA3-256, HKDF-SHA256, asyncio, struct packing) comes from Python stdlib. This is the minimal possible dependency set for the protocol's requirements.

## Version Compatibility

| Package | Min Version | Max Tested | Notes |
|---------|-------------|-----------|-------|
| liboqs-python | 0.14.0 | 0.14.1 | Algorithm names "ML-KEM-1024" and "ML-DSA-87" stable since liboqs 0.10.0. Pin >=0.14.0 because earlier versions used "-ipd" suffix names. |
| PyNaCl | 1.5.0 | 1.6.2 | IETF ChaCha20-Poly1305 low-level bindings available since 1.4.0. Pin >=1.5.0 for Python 3.11+ support. |
| flatbuffers | 24.0.0 | 25.12.19 | Uses date-based versioning. ForceDefaults available for years. Pin >=24.0.0 as a reasonable floor. |
| Python | 3.11 | 3.13 | hashlib.sha3_256, asyncio streams, struct, hmac all stable across this range. |

## Sources

- [liboqs-python on PyPI](https://pypi.org/project/liboqs-python/) -- version 0.14.1, Python >=3.9, auto-download behavior (HIGH confidence)
- [liboqs-python GitHub](https://github.com/open-quantum-safe/liboqs-python) -- API: KeyEncapsulation, Signature classes, algorithm name strings (HIGH confidence)
- [ML-KEM on Open Quantum Safe](https://openquantumsafe.org/liboqs/algorithms/kem/ml-kem.html) -- ML-KEM-1024 algorithm support confirmed (HIGH confidence)
- [ML-DSA on Open Quantum Safe](https://openquantumsafe.org/liboqs/algorithms/sig/ml-dsa.html) -- ML-DSA-87 algorithm support confirmed (HIGH confidence)
- [PyNaCl on PyPI](https://pypi.org/project/PyNaCl/) -- version 1.6.2, Python >=3.8, libsodium 1.0.20 bundled (HIGH confidence)
- [PyNaCl crypto_aead.py source](https://github.com/pyca/pynacl/blob/main/src/nacl/bindings/crypto_aead.py) -- IETF ChaCha20-Poly1305 encrypt/decrypt confirmed (HIGH confidence)
- [PyNaCl bindings directory](https://github.com/pyca/pynacl/tree/main/src/nacl/bindings) -- no crypto_kdf.py, HKDF not exposed (HIGH confidence, verified source tree)
- [flatbuffers on PyPI](https://pypi.org/project/flatbuffers/) -- version 25.12.19 (HIGH confidence)
- [FlatBuffers Python docs](https://flatbuffers.dev/languages/python/) -- ForceDefaults, flatc --python usage (HIGH confidence)
- [Python hashlib docs](https://docs.python.org/3/library/hashlib.html) -- SHA3-256 available since Python 3.6 (HIGH confidence)
- [Python hmac docs](https://docs.python.org/3/library/hmac.html) -- HMAC with any hashlib algorithm (HIGH confidence)
- [Python asyncio streams docs](https://docs.python.org/3/library/asyncio-stream.html) -- open_connection, StreamReader/Writer (HIGH confidence)
- [Python packaging guide](https://packaging.python.org/en/latest/guides/writing-pyproject-toml/) -- pyproject.toml, src layout (HIGH confidence)
- [libsodium 1.0.19 release](https://github.com/jedisct1/libsodium/releases/tag/1.0.19-RELEASE) -- HKDF added in 1.0.19 (HIGH confidence)
- C++ source files: `db/net/handshake.cpp`, `db/crypto/kdf.cpp`, `db/crypto/aead.h`, `db/net/framing.h`, `db/schemas/transport.fbs`, `db/schemas/blob.fbs` -- ground truth for wire protocol (HIGH confidence, primary source)

---
*Stack research for: chromatindb v1.6.0 Python SDK*
*Researched: 2026-03-29*
