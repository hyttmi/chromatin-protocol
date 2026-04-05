# Phase 87: SDK Envelope Compression - Context

**Gathered:** 2026-04-05
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK compresses plaintext with Brotli before envelope encryption and decompresses after decryption. New cipher suite byte (0x02) signals compressed envelopes. No node-side changes — the node handles blobs opaquely and never compresses. PROTOCOL.md documents the new suite.

**Scope pivot:** Original Phase 87 was "Wire Compression" targeting node-to-node Brotli compress-then-encrypt. Analysis revealed that node-to-node traffic is already-encrypted envelope data (incompressible ciphertext) plus hashes. Wire compression at the transport layer would save zero bandwidth. Compression is only effective before encryption, which is an SDK concern.

</domain>

<decisions>
## Implementation Decisions

### Compression placement & signaling
- **D-01:** Compression happens in the SDK's `encrypt_envelope()` before AEAD encryption. Decompression happens in `decrypt_envelope()` after AEAD decryption. The node is completely unaware of compression.
- **D-02:** New cipher suite byte: `suite=0x02` means ML-KEM-ChaCha-Brotli (compressed before encryption). Existing `suite=0x01` means ML-KEM-ChaCha (uncompressed). The suite byte in the envelope header is the sole compression signal.
- **D-03:** `decrypt_envelope()` handles both suite=0x01 and suite=0x02 transparently. Reads the suite byte and decompresses if 0x02. Backward-compatible decryption, forward-compatible encryption.

### Compression behavior
- **D-04:** Brotli quality 6. Client-side CPU is less constrained than a node; quality 6 gives meaningfully better ratio than quality 4 with acceptable CPU cost.
- **D-05:** 256-byte plaintext threshold. If plaintext < 256 bytes, use suite=0x01 (uncompressed). Brotli overhead on tiny data is net-negative.
- **D-06:** Expansion fallback. If Brotli output >= input size, fall back to suite=0x01 (uncompressed). Guarantees compression never makes things worse.
- **D-07:** `encrypt_envelope()` compresses by default (suite=0x02). Caller can pass `compress=False` to get suite=0x01. On by default, opt-out available.
- **D-08:** Envelope-only scope. Compression only in `encrypt_envelope()`/`decrypt_envelope()`. `write_blob()` sends raw data as-is. No compression for non-envelope blobs.

### Decompression bomb protection
- **D-09:** Decompressed output capped at MAX_BLOB_DATA_SIZE (100 MiB). A blob can't exceed this anyway. Reject before decompression completes — no memory exhaustion.

### SDK dependency
- **D-10:** Google's `brotli` Python package. C extension, fast, well-maintained. Hard dependency in pyproject.toml alongside liboqs-python, pynacl, flatbuffers.

### Cross-SDK test vectors
- **D-11:** Python generates suite=0x02 test vectors within the existing test suite. Python is the reference implementation for envelope compression. Future C++/Rust/JS SDKs validate against these vectors. C++ test_vector_generator not modified (node doesn't compress).

### Roadmap & requirements
- **D-12:** Full rewrite of Phase 87 in ROADMAP.md. Rename to "SDK Envelope Compression". Rewrite goal, success criteria, and requirements (COMP-01..04) to reflect SDK-only scope. Done during planning.
- **D-13:** PROTOCOL.md updated to document suite=0x02, compression semantics, and decompression bomb cap in the envelope encryption section.

### Node-side wire compression
- **D-14:** Explicitly NOT doing node-side wire compression. Node-to-node traffic is encrypted envelope data (incompressible) plus hashes. No Brotli dependency in C++ build. No changes to `Connection::send_message()`, `write_frame()`, or any transport code.

### Claude's Discretion
- Internal compression module structure in the SDK (new file vs inline in `_envelope.py`)
- Test strategy: unit tests for compress/decompress, integration tests for suite=0x02 round-trip
- Whether to add a `_compression.py` module or keep it inline
- How decompression bomb detection is implemented (streaming vs buffer check)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Envelope encryption (primary modification target)
- `sdk/python/chromatindb/_envelope.py` — encrypt_envelope(), decrypt_envelope(), ENVELOPE_MAGIC, CIPHER_SUITE_ML_KEM_CHACHA
- `sdk/python/tests/test_envelope.py` — Existing envelope test suite (extend for suite=0x02)

### Envelope format specification
- `db/PROTOCOL.md` §Envelope Encryption — Wire format, cipher suite definitions, HKDF labels. Must be updated with suite=0x02.

### SDK packaging
- `sdk/python/pyproject.toml` — dependencies list (add `brotli`), project metadata

### Cross-SDK test vectors
- `tools/test_vector_generator.cpp` — Existing C++ test vector generator (NOT modified, reference only)
- `sdk/python/tests/` — Test vector location for suite=0x02 vectors

### Framing & transport (NOT modified, context only)
- `db/net/framing.h` — MAX_BLOB_DATA_SIZE (100 MiB) constant used as decompression bomb cap
- `sdk/python/chromatindb/_framing.py` — SDK framing layer (unchanged, context for understanding the stack)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `_envelope.py`: encrypt_envelope() and decrypt_envelope() are the sole modification targets. Suite byte already dispatched — adding 0x02 is a natural extension.
- `ENVELOPE_MAGIC = b"CENV"`, `CIPHER_SUITE_ML_KEM_CHACHA = 0x01`: Constants to extend with new suite value.
- `MAX_FRAME_SIZE = 110 * 1024 * 1024` in `_framing.py`: Reference for size limits. MAX_BLOB_DATA_SIZE (100 MiB) from C++ can be mirrored as decompression cap.

### Established Patterns
- SDK crypto modules are single-purpose files: `crypto.py` (AEAD), `_hkdf.py` (key derivation), `_envelope.py` (envelope encryption). Compression could follow this pattern as `_compression.py`.
- Test suite mirrors module structure: `test_envelope.py`, `test_framing.py`, `test_handshake.py`. New tests go in `test_envelope.py` or a new `test_compression.py`.
- Dependencies are pinned with `~=` version constraints in pyproject.toml.

### Integration Points
- `encrypt_envelope()`: Insert Brotli compress after plaintext validation, before AEAD encrypt. Change suite byte to 0x02.
- `decrypt_envelope()`: After AEAD decrypt, check suite byte. If 0x02, decompress with bomb cap before returning plaintext.
- `pyproject.toml`: Add `brotli` to dependencies list.
- `db/PROTOCOL.md`: Add suite=0x02 documentation to envelope encryption section.

</code_context>

<specifics>
## Specific Ideas

- Scope pivot was driven by the realization that node-to-node traffic is always encrypted (envelope ciphertext + hashes), making transport-layer compression pointless. Compression must happen before encryption in the SDK.
- Quality 6 chosen over quality 4 because client-side CPU is less constrained than a busy node.
- The expansion fallback (compressed >= input → send uncompressed) eliminates the need for envelope-specific detection logic.

</specifics>

<deferred>
## Deferred Ideas

- **Node-side transport compression**: If a use case emerges with large unencrypted blobs flowing between nodes, revisit wire compression. Currently not justified.
- **Compression for non-envelope blobs**: `write_blob()` compression could be added later if needed, using a flag byte prefix in blob data.
- **Configurable Brotli quality**: Hardcoded at 6 for now. Could be made configurable if different workloads need different tradeoffs.

</deferred>

---

*Phase: 87-wire-compression*
*Context gathered: 2026-04-05*
