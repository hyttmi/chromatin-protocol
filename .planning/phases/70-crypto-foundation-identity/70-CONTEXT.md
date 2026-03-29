# Phase 70: Crypto Foundation & Identity - Context

**Gathered:** 2026-03-29
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK crypto primitives that produce byte-identical output to the C++ node, client identity management (generate/load/save ML-DSA-87 keypairs), project scaffolding (pyproject.toml, FlatBuffers codegen, exception hierarchy), and cross-language test vector validation. No networking, no transport, no connection logic.

</domain>

<decisions>
## Implementation Decisions

### Key file format & storage
- **D-01:** Raw binary key file format -- identical to C++ node (.key = 4896 bytes ML-DSA-87 secret, .pub = 2592 bytes public). Keys interoperable between SDK and node/relay without conversion.
- **D-02:** SSH-style sibling convention -- path points to .key file, .pub is derived sibling (carried forward from Phase 58 D-07/D-08).
- **D-03:** User-specified path only -- no default location. User passes key_path to Identity constructor. Matches node/relay pattern.
- **D-04:** In-memory identities supported -- Identity.generate() creates ephemeral keypair, Identity.load(path) loads from disk, Identity.generate_and_save(path) does both. Enables testing and throwaway clients.

### Python crypto libraries
- **D-05:** PQ crypto: liboqs-python (official binding, same underlying C library as node)
- **D-06:** Symmetric crypto: PyNaCl (libsodium binding, same underlying C library as node) -- ChaCha20-Poly1305 IETF + HKDF-SHA256
- **D-07:** SHA3-256: hashlib (stdlib) -- hashlib.sha3_256(), zero additional dependencies
- **D-08:** FlatBuffers: official flatbuffers pip package + flatc --python code generation

### SDK API surface & naming
- **D-09:** Package name: chromatindb (`pip install chromatindb`, `import chromatindb`)
- **D-10:** Flat module structure: chromatindb.crypto, chromatindb.identity, chromatindb.wire, chromatindb.exceptions, chromatindb.generated/
- **D-11:** Async-first with sync wrapper. Phase 70 crypto ops are naturally synchronous; async transport starts Phase 71.
- **D-12:** snake_case naming (PEP 8) -- identity.generate(), crypto.sha3_256(), crypto.build_signing_input()
- **D-13:** Exception hierarchy: ChromatinError (base) > CryptoError (SignatureError, DecryptionError, KeyError), IdentityError (KeyFileError, NamespaceError), WireError (DecodeError), ProtocolError (Phase 71+: HandshakeError, ConnectionError)
- **D-14:** Python 3.10+ minimum (match/case, union types, ParamSpec)
- **D-15:** Full type hints on all public APIs, no runtime checking (mypy/pyright catches it)
- **D-16:** Google-style docstrings on all public APIs (Args/Returns/Raises sections)

### Test vector strategy
- **D-17:** Extract test vectors from C++ test suite -- hardcode known input/output pairs in Python tests
- **D-18:** Add a small standalone C++ test vector generator binary to the repo -- prints JSON vectors for hash, sign, aead, kdf, signing_input. Commit output to sdk/python/tests/vectors/. Reusable for future SDKs.
- **D-19:** Test framework: pytest

### FlatBuffers codegen workflow
- **D-20:** Commit generated Python code to sdk/python/chromatindb/generated/. Users don't need flatc installed. Regenerate when schemas change.
- **D-21:** Single source of truth: flatc reads from db/schemas/transport.fbs and db/schemas/blob.fbs. No schema duplication.

### Dev tooling
- **D-22:** Linter/formatter: ruff (single tool for linting + formatting)
- **D-23:** Type checker: mypy, configured in pyproject.toml
- **D-24:** Dependencies in pyproject.toml use compatible release pins (~=): liboqs-python~=0.11.0, pynacl~=1.5.0, flatbuffers~=24.3

### Directory layout
- **D-25:** Flat layout: sdk/python/chromatindb/ (no src/ indirection). pyproject.toml at sdk/python/. Tests at sdk/python/tests/.
- **D-26:** Future SDKs follow same pattern: sdk/rust/, sdk/c++/, sdk/js/, sdk/c/

### Claude's Discretion
- Internal module organization within crypto.py (single file vs split when it grows)
- FlatBuffers generated code directory structure under generated/
- pytest configuration details (conftest.py, fixtures)
- ruff rule selection and configuration
- Test vector generator binary location and build integration
- __init__.py re-exports (what's in the public API surface)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Crypto primitives (SDK must match these exactly)
- `db/crypto/signing.h` -- ML-DSA-87 Signer class: key sizes (pub=2592, sec=4896, sig<=4627), generate/sign/verify API
- `db/crypto/hash.h` -- SHA3-256: sha3_256() returns 32-byte digest
- `db/crypto/aead.h` -- ChaCha20-Poly1305 IETF: KEY=32, NONCE=12, TAG=16, encrypt/decrypt API
- `db/crypto/kdf.h` -- HKDF-SHA256: extract/expand/derive API, PRK_SIZE=32
- `db/crypto/kem.h` -- ML-KEM-1024: key sizes (pub=1568, sec=3168, ct=1568, ss=32), encaps/decaps API

### Canonical signing input (SDK must replicate)
- `db/wire/codec.h` -- build_signing_input() declaration
- `db/wire/codec.cpp` -- build_signing_input() implementation: SHA3-256(namespace || data || ttl_le32 || timestamp_le64), incremental hashing

### Identity & key file format
- `db/identity/identity.h` -- NodeIdentity: generate/load/save, namespace = SHA3-256(pubkey)
- `db/identity/identity.cpp` -- Key file I/O: raw binary .pub (2592B) + .key (4896B), size validation

### Wire format schemas
- `db/schemas/transport.fbs` -- TransportMessage table: type (TransportMsgType enum), payload ([ubyte]), request_id (uint32)
- `db/schemas/blob.fbs` -- Blob table schema

### Known protocol quirks (SDK must follow C++ source, not docs)
- Empty HKDF salt (C++ uses empty, PROTOCOL.md incorrectly says SHA3-256(pubkeys)) -- fix docs in Phase 74
- Mixed endianness: BE for framing, LE for auth payload pubkey_size and canonical signing input (ttl/timestamp)
- AEAD nonce starts at 1 after PQ handshake (nonce 0 consumed by auth exchange)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/schemas/transport.fbs` and `db/schemas/blob.fbs`: FlatBuffers schemas, single source of truth for wire format
- `db/crypto/` headers: definitive reference for key sizes, algorithm parameters, API contracts
- `db/wire/codec.cpp` build_signing_input(): canonical signing implementation to replicate exactly
- `db/identity/identity.cpp`: key file format and namespace derivation reference

### Established Patterns
- Canonical signing: SHA3-256(namespace || data || ttl_le32 || timestamp_le64) -- SDK must match byte-for-byte
- Raw binary key files with size validation on load (2592 for pub, 4896 for sec)
- Namespace derivation: SHA3-256(public_key) -- 32-byte result
- TransportMessage FlatBuffer envelope: type + payload + request_id

### Integration Points
- Test vector generator binary links chromatindb_lib for access to all crypto primitives
- FlatBuffers Python codegen reads from db/schemas/ (shared with C++)
- Key files are format-compatible between SDK, node, and relay

</code_context>

<specifics>
## Specific Ideas

- Key interoperability is a hard requirement (success criterion #1) -- a keypair generated by the SDK must be loadable by the C++ node and vice versa
- FlatBuffers cross-language determinism is NOT guaranteed -- SDK must use server-returned blob_hash, never compute its own from FlatBuffer encoding
- Test vector generator is a repo-level tool, reusable by all future SDKs (Rust, C++, JS, C)

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 70-crypto-foundation-identity*
*Context gathered: 2026-03-29*
