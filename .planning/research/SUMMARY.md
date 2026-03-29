# Project Research Summary

**Project:** chromatindb v1.6.0 Python SDK
**Domain:** Python client SDK for binary-protocol PQ-secure database
**Researched:** 2026-03-29
**Confidence:** HIGH

## Executive Summary

The chromatindb Python SDK wraps an existing, proven C++ binary protocol. The core challenge is not designing a new protocol but rather achieving exact byte-for-byte interoperability with the running relay and node. The three fundamental difficulties are: (1) key derivation that differs between the C++ implementation and PROTOCOL.md documentation, requiring the SDK to follow the code not the docs; (2) mixed endianness in the wire format — big-endian for framing, little-endian for auth payload and signing input fields; and (3) FlatBuffers encoding determinism, which is not guaranteed across languages and requires a deliberate strategy. All three are well-understood with clear mitigations. This is an implementation precision problem, not an unsolved design problem.

The recommended approach is a minimal 3-dependency Python package (liboqs-python, PyNaCl, flatbuffers) with stdlib handling HKDF-SHA256 and SHA3-256. The SDK is async-first using asyncio streams, with a sync wrapper for non-async callers. The architecture layers cleanly from bottom (transport framing + AEAD) to top (public ChromatinClient API), and each layer is independently testable. The build order is strictly bottoms-up — crypto primitives and wire codecs must be validated against C++ test vectors before any higher layers are written.

The primary risk is invisible interop failure: a handshake may appear to complete but derive the wrong session keys, or a blob may be written and acknowledged but be unreadable because the local hash computation differs from the server's. The mitigation is mandatory cross-language test vectors at every crypto and encoding boundary before those components are considered done. All critical pitfalls are preventable with disciplined implementation order and testing; none require architectural changes after the fact.

## Key Findings

### Recommended Stack

The SDK requires exactly 3 runtime pip dependencies plus Python stdlib. liboqs-python provides ML-KEM-1024 and ML-DSA-87 PQ primitives and is the only maintained Python wrapper for liboqs. PyNaCl provides ChaCha20-Poly1305 IETF AEAD via direct libsodium bindings — using the same underlying C library as the node eliminates any behavioral divergence. The flatbuffers Python runtime (pure Python, no native deps) handles TransportMessage and Blob encoding; code is generated with `flatc --python` from the existing `db/schemas/` files and committed to the repo so users do not need flatc installed. HKDF-SHA256 and SHA3-256 use stdlib `hmac` + `hashlib` exclusively — PyNaCl does not expose HKDF bindings, and adding the `cryptography` package would pull in OpenSSL, violating the project's no-OpenSSL constraint.

**Core technologies:**
- **liboqs-python 0.14.1**: ML-KEM-1024 + ML-DSA-87 — only maintained Python PQ wrapper, mirrors C++ liboqs API directly
- **PyNaCl 1.6.2**: ChaCha20-Poly1305 IETF AEAD via `nacl.bindings.crypto_aead_chacha20poly1305_ietf_*` — same libsodium backend as the C++ node, eliminates interop risk
- **flatbuffers 25.12.19**: Wire format encoding/decoding — pure Python, ForceDefaults(True) required for determinism; generated code committed to repo
- **hashlib + hmac (stdlib)**: SHA3-256 hashing + HKDF-SHA256 per RFC 5869 — zero dependencies, correct, testable against RFC test vectors
- **asyncio (stdlib)**: TCP networking via StreamReader/StreamWriter with `readexactly()` for frame parsing — no third-party async framework needed
- **pytest 8.x + pytest-asyncio 0.24.x**: Test framework with async support — mirrors C++ Catch2 approach
- **Python 3.11 minimum**: liboqs-python requires >=3.9; 3.11 adds significant performance improvements and is still receiving security updates

### Expected Features

The SDK has a clear MVP (table stakes) and a well-defined post-validation feature set. The dependency chain is strict: identity management is a prerequisite for the PQ handshake, which is a prerequisite for AEAD transport, which is a prerequisite for all operations. This ordering is non-negotiable and directly governs the implementation phase structure.

**Must have (table stakes — v1 launch):**
- Identity management (ML-DSA-87 keypair generate/load, namespace = SHA3-256(pubkey)) — prerequisite for everything
- PQ handshake (ML-KEM-1024 exchange + ML-DSA-87 mutual auth with relay) — session establishment
- AEAD encrypted transport (ChaCha20-Poly1305, counter nonces, length-prefixed framing) — all comms are encrypted
- FlatBuffers encode/decode (TransportMessage + Blob) — mandatory wire format
- Request-response dispatch (request_id assignment + asyncio.Future correlation table) — concurrency model
- Write blobs with canonical signing (Data + WriteAck) — primary use case
- Read blobs (ReadRequest/ReadResponse, typed Blob dataclass) — primary use case
- Delete blobs with tombstone signing (Delete + DeleteAck) — primary use case
- List blobs with auto-paginating async iterator (ListRequest/ListResponse) — primary use case
- Exists check (ExistsRequest/ExistsResponse) — lightweight membership query
- Context manager (`async with`) for connection lifecycle — every Python client expects this
- Typed exception hierarchy (ChromatinError, ConnectionError, AuthenticationError, ProtocolError, etc.)
- Keepalive (background Ping/Pong) — relay drops idle connections at 120s inactivity
- Per-operation timeout support via `asyncio.wait_for`
- Graceful disconnect (Goodbye message, cleanup pending futures)

**Should have (post-v1 validation):**
- Pub/sub notifications (async iterator yielding Notification objects for real-time blob events)
- Sync (blocking) wrapper class — scripts, notebooks, and CLI tools need this
- Batch exists/read (BatchExistsRequest, BatchReadRequest) — single round-trip for multiple blobs
- Node introspection (NodeInfoRequest, StorageStatusRequest) — monitoring + capability detection
- Namespace operations (NamespaceListRequest with auto-pagination, NamespaceStatsRequest)
- Blob metadata query (MetadataRequest — size, TTL, signer without data transfer)
- Delegation management (delegate, revoke, DelegationListRequest)
- Time range queries (TimeRangeRequest — temporal blob discovery)

**Defer (v2+):**
- Auto-reconnect with backoff — complex re-subscription state tracking; defer until real users need it
- Explicit pipeline() context manager — individual awaits already pipeline naturally via request_id multiplexing
- CLI tool — separate package, not part of the SDK

**Anti-features to avoid:**
- Connection pooling — request_id multiplexing on a single persistent connection makes it unnecessary
- ORM or schema layer — blob store is bytes-in, bytes-out; serialization is the caller's concern
- Transparent client-side data encryption — application-layer concern, not SDK concern
- Automatic retry on all errors — StorageFull and AuthenticationError are not transient; only reconnect-level failures warrant retry

### Architecture Approach

The SDK is a flat package under `sdk/python/chromatindb/` with 10 modules and no nested sub-packages. Flat layout is correct for a single-protocol, single-connection-type domain — nesting adds import friction without benefit. Generated FlatBuffers code lives in `_generated/` (private sub-package, committed to repo). Integration tests that require a live relay are in `test_interop.py` and are skipped in unit test runs.

**Major components:**
1. **transport.py** — TCP socket via asyncio streams, length-prefixed frame IO (`readexactly()`), AEAD encrypt/decrypt with counter nonces, single asyncio.Lock on the send path
2. **crypto.py** — Pure primitive functions: ML-KEM-1024, ML-DSA-87, SHA3-256, HKDF-SHA256, ChaCha20-Poly1305, nonce generation; fully testable with known vectors before any network code runs
3. **handshake.py** — PQ handshake initiator as explicit state machine (SEND_KEM_PUBKEY → RECV_KEM_CIPHERTEXT → SEND_AUTH → RECV_AUTH → COMPLETE); explicit raw vs encrypted I/O per state
4. **wire.py** — FlatBuffers TransportMessage encode/decode; ForceDefaults(True) mandatory; uses `_generated/` code from flatc
5. **messages.py** — Binary payload encoders/decoders for all 38 client-facing message types; endianness explicit per field
6. **session.py** — Connection lifecycle, request_id counter, `dict[int, asyncio.Future]` dispatch table, background recv coroutine routing by request_id and message type
7. **client.py** — Public API: connect, write, read, delete, list, subscribe, and all query methods
8. **identity.py** — ML-DSA-87 keypair: generate, load/save raw binary files (compatible with node .key/.pub format), namespace derivation

**Key patterns:**
- Serialized send path via asyncio.Lock — AEAD nonce counter cannot tolerate concurrent senders
- Background recv coroutine routes: request_id != 0 → pending Futures dict; Notification (type 21) → asyncio.Queue; Ping → immediate Pong; Goodbye → connection close
- Never compute blob_hash locally — always use server-returned hash from WriteAck (FlatBuffer encoding non-determinism risk)
- Async-first primary API with sync wrapper that runs a dedicated asyncio event loop thread

### Critical Pitfalls

1. **PROTOCOL.md/C++ implementation discrepancy** — PROTOCOL.md states PQ handshake HKDF uses `SHA3-256(pubkeys)` as salt; the C++ code (`handshake.cpp:21-28`) uses empty salt. Session fingerprint is also computed as a direct SHA3-256 hash (not HKDF expand). Implement from the C++ source, not the doc. Fix PROTOCOL.md as part of v1.6.0.

2. **Mixed wire format endianness** — Frame length prefix is big-endian; auth payload pubkey_size is little-endian; signing input ttl/timestamp fields are little-endian; FlatBuffers is internally little-endian. Write per-field encode/decode functions with explicit format strings — never assume a single convention. Validate with cross-language test vectors.

3. **AEAD nonce counter desynchronization** — Any missed or double-counted encrypt/decrypt call permanently corrupts the session. After PQ handshake, both counters start at 1 (auth exchange consumed counter 0). Serialize all sends through a single asyncio.Lock. Never retry encryption with a new nonce — close and reconnect instead.

4. **FlatBuffer blob encoding non-determinism** — Python and C++ FlatBuffer builders may produce byte-different output for the same logical Blob, causing blob_hash mismatches. Design the SDK to never compute blob_hash locally — always use the hash returned in WriteAck. Validate with cross-language byte comparison test before any write operations.

5. **liboqs version and API details** — Use NIST final algorithm names ("ML-DSA-87", "ML-KEM-1024"), not old names ("Dilithium5", "Kyber1024"). Use `sign()` not `sign_with_ctx_str()`. Note that `encap_secret()` returns `(ciphertext, shared_secret)` — tuple order is easy to swap silently. Pin liboqs version in pyproject.toml to match the C++ CMakeLists.txt.

## Implications for Roadmap

Implementation follows a strict bottom-up dependency order. Each phase must be complete and validated before the next begins — there are no parallel tracks because everything depends on the crypto and framing layer being correct. Cross-language test vectors must be established early; adding them retroactively is expensive.

### Phase 1: Project Setup and Crypto Foundation
**Rationale:** The single greatest risk is invisible interop failure at the crypto boundary. This phase eliminates that risk before writing any protocol code. liboqs version must be pinned to match the C++ build. Crypto test vectors must be captured from C++ before any higher-level code is written.
**Delivers:** pyproject.toml with pinned dependencies, `constants.py`, `exceptions.py`, `crypto.py` with verified test vectors (ML-KEM-1024, ML-DSA-87, SHA3-256, HKDF-SHA256, ChaCha20-Poly1305, nonce generation), `identity.py` with keypair generation/load/save and namespace derivation.
**Addresses:** Identity management (table stakes prerequisite), liboqs version alignment
**Avoids:** Pitfall 6 (liboqs version mismatch), algorithm name confusion, HKDF PyNaCl gap, `encap_secret()` tuple order trap

### Phase 2: Transport Layer and PQ Handshake
**Rationale:** The handshake and AEAD framing are the foundation for everything else. Validating against a real relay early surfaces any interop issues before they compound into higher layers. This is the highest-risk technical work — all three critical endianness/nonce/HKDF pitfalls manifest here.
**Delivers:** `transport.py` (TCP + length-prefixed framing + AEAD), `wire.py` (FlatBuffers TransportMessage codec with ForceDefaults(True)), `handshake.py` (explicit state machine, raw vs encrypted per state). Successfully completes a PQ handshake against a live relay.
**Uses:** liboqs-python (ML-KEM-1024 encap/decap), PyNaCl (ChaCha20-Poly1305 IETF), stdlib HKDF, asyncio streams
**Avoids:** Pitfall 1 (endianness — BE frame header, LE auth payload), Pitfall 2 (nonce desync — serialized send, counters start at 1 post-PQ-handshake), Pitfall 3 (HKDF mismatch — empty salt, not SHA3-256(pubkeys)), Pitfall 7 (TCP framing — readexactly not recv), Pitfall 8 (raw vs encrypted state machine)

### Phase 3: Core Request-Response (Write, Read, Delete, List, Exists)
**Rationale:** With handshake proven against the live relay, the session layer and primary blob operations deliver the table-stakes write/read/list loop. Blob signing and FlatBuffer determinism risks are resolved here with mandatory cross-language test vectors.
**Delivers:** `messages.py` (payload encoders/decoders for all P1 message types), `session.py` (request_id dispatch table + background recv loop), `client.py` with write/read/delete/list/exists API. First complete end-to-end blob round-trip against a running node.
**Implements:** Request-response dispatch with asyncio.Future dict, auto-paginating list iterator
**Avoids:** Pitfall 4 (signing input — LE ttl/timestamp, correct field concatenation order, cross-language test vector), Pitfall 5 (FlatBuffer hash — use server-returned WriteAck hash, never local hash), Pitfall 10 (FlatBuffer field ordering — mirror C++ codec.cpp vector creation order)

### Phase 4: Extended Query Suite and Pub/Sub
**Rationale:** With core CRUD proven, the remaining 38 message types are lower-risk additions sharing the same infrastructure. Pub/sub is the most architecturally distinct addition (notifications are server-pushed, request_id=0, routed to asyncio.Queue not pending dict) and benefits from the stable recv loop established in Phase 3. Sync wrapper requires the complete async API to be stable before it can be built.
**Delivers:** Pub/sub subscribe/unsubscribe/notification routing, batch exists/read, node info, namespace list/stats, blob metadata, delegation management, time range queries, peer info, stats. Sync wrapper class (`chromatindb.ChromatinClient` wrapping `chromatindb.aio.ChromatinClient`).
**Addresses:** All P2 features from FEATURES.md
**Avoids:** Relay message filter blocklist — SDK must never send sync/PEX/handshake message types; relay disconnects immediately on blocked type

### Phase 5: Integration Testing and Documentation Corrections
**Rationale:** Docker-based integration tests against a real relay+node are the only way to verify the full protocol stack end-to-end. The project standard (established in v1.0.0) is Docker for all E2E tests. Additionally, PROTOCOL.md has known discrepancies with the C++ implementation that must be corrected as part of v1.6.0.
**Delivers:** `test_interop.py` Docker test suite covering all phases, `examples/` directory (write_blob.py, read_blob.py, subscribe.py), PROTOCOL.md corrections (HKDF salt, KemPubkey payload format includes signing pubkey, session fingerprint computation), package release configuration.
**Research flag:** None — Docker test infrastructure already established in the repo

### Phase Ordering Rationale

- Foundation-first ordering is mandatory: AEAD framing failures produce opaque errors that look like bugs at any layer above; crypto correctness cannot be assumed and must be verified with test vectors before any protocol code is written.
- Cross-language test vectors must be captured from the C++ test suite during Phase 2 and Phase 3 — adding them retroactively after discovering discrepancies is significantly more expensive.
- Pub/sub is deferred to Phase 4 because it requires the background recv loop to distinguish notification messages from correlated responses; building this cleanly requires the request-response path to be stable first.
- Sync wrapper is deferred until Phase 4 because it wraps the complete async API — it cannot be built incrementally.
- All integration tests use Docker, not in-process test servers with shared ports (project feedback standard from v1.0.0).

### Research Flags

Phases needing deeper research during planning: None. All implementation details are resolved by direct C++ source code analysis. The research files contain exact byte layouts, function signatures, HKDF parameters, and test vector strategies — no unknowns remain at the architecture level.

Phases with standard patterns (skip research-phase):
- **Phase 1 (Setup):** Dependency management and pyproject.toml are well-documented standard Python packaging.
- **Phase 3 (Core ops):** All message payload formats documented in PROTOCOL.md + `db/wire/codec.cpp`.
- **Phase 4 (Extended queries):** All 38 message types documented; patterns established in Phase 3.
- **Phase 5 (Integration):** Docker test pattern already established in the repo.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All dependencies verified on PyPI; version compatibility confirmed; PyNaCl HKDF gap confirmed by source tree inspection of PyNaCl bindings directory (no crypto_kdf.py); liboqs-python auto-download behavior documented |
| Features | HIGH | All 38 client message types documented in PROTOCOL.md; prioritization based on established SDK patterns (redis-py, nats-py, boto3); dependency graph verified against C++ architecture |
| Architecture | HIGH | Based on direct source analysis of relay + node C++ code: handshake.cpp, connection.cpp, framing.cpp, codec.cpp, relay_session.cpp, message_filter.cpp; wire format verified at byte level |
| Pitfalls | HIGH | All critical pitfalls identified from direct source code; PROTOCOL.md discrepancy explicitly verified against handshake.cpp:21-28; endianness verified in framing.cpp (BE) and codec.cpp (LE signing input) |

**Overall confidence:** HIGH

### Gaps to Address

- **HKDF implementation choice:** STACK.md recommends stdlib hmac+hashlib for HKDF-SHA256. PITFALLS.md recommends pysodium for exact libsodium parity. Both produce RFC 5869 compliant output. Phase 1 should resolve this with a test vector comparison — if stdlib HKDF matches libsodium output for the empty-salt case (highly likely), use stdlib to avoid the pysodium dependency. If any edge case differs, use pysodium instead.

- **PROTOCOL.md discrepancy:** The PQ handshake HKDF salt and session fingerprint computation differ between PROTOCOL.md and the C++ source. Phase 5 must correct PROTOCOL.md. All implementation phases must follow the C++ source (empty salt, direct SHA3-256 for fingerprint), not the documentation.

- **FlatBuffer blob hash strategy:** The safe default is to always use server-returned WriteAck hashes and never compute blob_hash locally. If Phase 3 cross-language byte comparison succeeds (Python and C++ produce identical Blob FlatBuffer bytes), locally-computed hashes could be offered as an optimization. Treat local hash computation as a validated addition, not a starting assumption.

- **liboqs runtime installation:** liboqs-python auto-downloads and compiles the C library on first import if not pre-installed, requiring CMake and a C compiler. Production deployments need a Docker image with liboqs pre-built. Document this clearly in Phase 5; consider providing a Dockerfile or referencing the existing project Dockerfile as a base.

## Sources

### Primary (HIGH confidence)
- C++ source `db/net/handshake.cpp` — HKDF derivation (empty salt confirmed at lines 21-28), session fingerprint (direct SHA3-256), auth payload encoding (LE pubkey_size)
- C++ source `db/net/connection.cpp` — nonce format (4 zero bytes + 8-byte BE counter), counter lifecycle, counter starts at 0/1 after PQ handshake
- C++ source `db/net/framing.cpp` — frame length prefix (4-byte BE), AEAD frame structure
- C++ source `db/wire/codec.cpp` — signing input construction (LE ttl/timestamp), FlatBuffer field creation order for Blob
- C++ source `db/net/protocol.cpp` — TransportCodec FlatBuffer encoding with ForceDefaults(true)
- C++ source `relay/core/relay_session.cpp`, `relay/core/message_filter.cpp` — relay PQ handshake responder behavior, blocked message types (21 blocked types)
- `db/PROTOCOL.md` — wire format reference (HKDF salt section has confirmed discrepancy with implementation; all other formats verified correct)
- [liboqs-python GitHub](https://github.com/open-quantum-safe/liboqs-python) — KeyEncapsulation, Signature API; encap_secret() returns (ciphertext, shared_secret)
- [liboqs-python on PyPI](https://pypi.org/project/liboqs-python/) — version 0.14.1, Python >=3.9, auto-download C library behavior
- [PyNaCl on PyPI](https://pypi.org/project/PyNaCl/) + [crypto_aead.py source](https://github.com/pyca/pynacl/blob/main/src/nacl/bindings/crypto_aead.py) — IETF ChaCha20-Poly1305 confirmed; HKDF absence confirmed by reviewing bindings directory
- [flatbuffers on PyPI](https://pypi.org/project/flatbuffers/) + [FlatBuffers Python docs](https://flatbuffers.dev/languages/python/) — ForceDefaults, flatc --python usage
- [Python hashlib docs](https://docs.python.org/3/library/hashlib.html) — SHA3-256 stdlib availability since Python 3.6
- [Python hmac docs](https://docs.python.org/3/library/hmac.html) — HMAC-SHA256 for RFC 5869 HKDF implementation
- [Python asyncio streams docs](https://docs.python.org/3/library/asyncio-stream.html) — readexactly(), StreamReader/Writer

### Secondary (MEDIUM confidence)
- [Azure SDK Python Design Guidelines](https://azure.github.io/azure-sdk/python_design.html) — sync/async dual client pattern, exception hierarchy conventions
- [redis-py asyncio docs](https://redis.readthedocs.io/en/stable/examples/asyncio_examples.html) — context manager, pub/sub iteration patterns
- [nats-py client](https://github.com/nats-io/nats.py) — async-first design, reconnect with backoff, callback events
- [FlatBuffers deterministic encoding discussion](https://groups.google.com/g/flatbuffers/c/v2RkM3KB1Qw) — encoding not guaranteed deterministic across language implementations
- [libsodium HKDF docs](https://doc.libsodium.org/key_derivation/hkdf) — HKDF-SHA256 added in libsodium 1.0.19; same RFC 5869 algorithm

---
*Research completed: 2026-03-29*
*Ready for roadmap: yes*
