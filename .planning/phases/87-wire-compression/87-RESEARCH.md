# Phase 87: SDK Envelope Compression - Research

**Researched:** 2026-04-05
**Domain:** SDK-side Brotli compression before envelope encryption
**Confidence:** HIGH

## Summary

Phase 87 is an SDK-only change that adds Brotli compression inside `encrypt_envelope()` / `decrypt_envelope()`, signaled by a new cipher suite byte (0x02). The node is completely unaware of compression -- it handles blobs opaquely. The original "Wire Compression" scope was pivoted because node-to-node traffic is already-encrypted envelope data (incompressible ciphertext), making transport-layer compression pointless.

The implementation modifies two functions in `_envelope.py`, adds the `brotli` dependency to `pyproject.toml`, updates `PROTOCOL.md` with suite=0x02 documentation, and rewrites the Phase 87 section of `ROADMAP.md` and `REQUIREMENTS.md` to reflect the SDK-only scope. A new exception type (`DecompressionError`) should be added for bomb detection. The decompression bomb protection uses a streaming `brotli.Decompressor` with incremental output size checking against `MAX_BLOB_DATA_SIZE` (100 MiB).

**Primary recommendation:** Implement compression inline in `_envelope.py` (no separate `_compression.py` module -- the logic is ~20 lines total). Use brotli 1.2.0, quality 6, 256-byte threshold, expansion fallback.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Compression happens in SDK's `encrypt_envelope()` before AEAD encryption. Decompression in `decrypt_envelope()` after AEAD decryption. Node is completely unaware.
- D-02: New cipher suite byte: `suite=0x02` = ML-KEM-ChaCha-Brotli (compressed). Existing `suite=0x01` = ML-KEM-ChaCha (uncompressed).
- D-03: `decrypt_envelope()` handles both suite=0x01 and suite=0x02 transparently. Backward-compatible decryption.
- D-04: Brotli quality 6.
- D-05: 256-byte plaintext threshold. If plaintext < 256 bytes, use suite=0x01.
- D-06: Expansion fallback. If Brotli output >= input size, fall back to suite=0x01.
- D-07: `encrypt_envelope()` compresses by default (suite=0x02). Caller can pass `compress=False` to get suite=0x01.
- D-08: Envelope-only scope. No compression for non-envelope blobs.
- D-09: Decompressed output capped at MAX_BLOB_DATA_SIZE (100 MiB). Reject before decompression completes.
- D-10: Google's `brotli` Python package. Hard dependency in pyproject.toml.
- D-11: Python generates suite=0x02 test vectors. C++ test_vector_generator not modified.
- D-12: Full rewrite of Phase 87 in ROADMAP.md. Rename to "SDK Envelope Compression".
- D-13: PROTOCOL.md updated to document suite=0x02.
- D-14: Explicitly NOT doing node-side wire compression. No Brotli in C++ build. No changes to Connection, write_frame, or transport code.

### Claude's Discretion
- Internal compression module structure (new file vs inline in `_envelope.py`)
- Test strategy: unit tests for compress/decompress, integration tests for suite=0x02 round-trip
- Whether to add a `_compression.py` module or keep it inline
- How decompression bomb detection is implemented (streaming vs buffer check)

### Deferred Ideas (OUT OF SCOPE)
- Node-side transport compression
- Compression for non-envelope blobs (`write_blob()`)
- Configurable Brotli quality
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| COMP-01 | Node compresses sync/data message payloads with Brotli before AEAD encryption (flag byte per message) | **REWRITTEN** by D-12/D-14: SDK compresses plaintext in `encrypt_envelope()` with Brotli before AEAD encryption; suite byte 0x02 signals compression. No node changes. |
| COMP-02 | Node skips compression for payloads under 256 bytes or already-encrypted envelope data | **REWRITTEN** by D-05/D-06: SDK skips compression for plaintext under 256 bytes (suite=0x01). Expansion fallback if compressed >= original. Already-encrypted detection is moot -- compression happens before encryption. |
| COMP-03 | Node enforces decompressed output cap at MAX_BLOB_DATA_SIZE to prevent decompression bombs | **REWRITTEN** by D-09: SDK enforces 100 MiB decompression cap in `decrypt_envelope()` using streaming Decompressor with incremental size check. |
| COMP-04 | SDK compresses outbound and decompresses inbound payloads matching node behavior | **REWRITTEN** by D-01/D-03: SDK encrypt/decrypt handles both suite=0x01 and suite=0x02. No node behavior to match -- node is unaware. Cross-SDK test vectors verify correctness. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| brotli | 1.2.0 | Compression/decompression | Google's official Python bindings. C extension for speed. v1.2.0 adds `Decompressor.process(output_buffer_limit=)` for streaming with size limits. Actively maintained (Nov 2025 release). |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| (existing) chromatindb SDK | 0.1.0 | Envelope encryption | `_envelope.py` is the sole modification target |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| brotli (Google) | brotlipy | brotlipy is older bindings, less maintained, no output_buffer_limit. Use Google's `brotli`. |
| brotli (Google) | zstd | Brotli chosen per CONTEXT.md decision. zstd is faster but Brotli has better ratios at comparable quality levels for typical structured data. |

**Installation:**
```bash
pip install brotli~=1.2
```

**Version verification:** brotli 1.2.0 confirmed via `pip install brotli` in test venv. Python 3.10-3.14 wheels available on PyPI. Published 2025-11-05.

## Architecture Patterns

### Modification Targets

```
sdk/python/
  chromatindb/
    _envelope.py          # MODIFY: encrypt_envelope(), decrypt_envelope(), envelope_parse()
  pyproject.toml          # MODIFY: add brotli dependency
  tests/
    test_envelope.py      # MODIFY: add suite=0x02 tests
    vectors/              # ADD: compressed envelope test vectors (optional, can generate in-test)
db/
  PROTOCOL.md             # MODIFY: add suite=0x02 documentation
.planning/
  ROADMAP.md              # MODIFY: rewrite Phase 87 section
  REQUIREMENTS.md         # MODIFY: rewrite COMP-01 through COMP-04
```

### Pattern: Suite-Byte Dispatch in Envelope Functions

**What:** The suite byte at offset 5 in the envelope header already dispatches behavior. Adding suite=0x02 is a natural extension of this dispatch.

**Current state:** Both `envelope_decrypt()` and `envelope_parse()` reject anything except `suite=0x01`. To add compression:
1. `encrypt_envelope()` compresses plaintext if conditions met, writes suite=0x02
2. `decrypt_envelope()` reads suite byte, decompresses after AEAD decrypt if suite=0x02
3. `envelope_parse()` accepts both 0x01 and 0x02

**Example (encrypt side):**
```python
# New constant
CIPHER_SUITE_ML_KEM_CHACHA_BROTLI = 0x02

# In encrypt_envelope(), after building plaintext, before AEAD:
_COMPRESS_THRESHOLD = 256

actual_suite = CIPHER_SUITE_ML_KEM_CHACHA  # default: uncompressed
data_to_encrypt = plaintext

if compress and len(plaintext) >= _COMPRESS_THRESHOLD:
    compressed = brotli.compress(plaintext, quality=6)
    if len(compressed) < len(plaintext):
        data_to_encrypt = compressed
        actual_suite = CIPHER_SUITE_ML_KEM_CHACHA_BROTLI

# Write actual_suite into header byte at offset 5
partial_header.extend(struct.pack("B", actual_suite))
# ... rest of encryption uses data_to_encrypt
```

**Example (decrypt side):**
```python
# In envelope_decrypt(), after AEAD decrypt:
suite = data[5]
if suite == CIPHER_SUITE_ML_KEM_CHACHA:
    return plaintext  # no decompression needed
elif suite == CIPHER_SUITE_ML_KEM_CHACHA_BROTLI:
    return _safe_decompress(plaintext, MAX_DECOMPRESSED_SIZE)
else:
    raise MalformedEnvelopeError(f"Unsupported cipher suite: {suite}")
```

### Pattern: Streaming Decompression with Bomb Protection

**What:** Use `brotli.Decompressor()` streaming API to decompress incrementally, checking accumulated output size after each chunk.

**Why not one-shot `brotli.decompress()`:** One-shot allocates the full output buffer before returning. A 6 KB compressed payload can decompress to 4+ GiB, causing OOM before the size check runs.

**Verified pattern:**
```python
import brotli

MAX_DECOMPRESSED_SIZE = 100 * 1024 * 1024  # 100 MiB (MAX_BLOB_DATA_SIZE)

def _safe_decompress(compressed: bytes, max_size: int) -> bytes:
    """Decompress with decompression bomb protection."""
    dec = brotli.Decompressor()
    chunks: list[bytes] = []
    total = 0

    result = dec.process(compressed)
    total += len(result)
    if total > max_size:
        raise DecompressionError(
            f"Decompressed output {total} exceeds limit {max_size}"
        )
    chunks.append(result)

    while not dec.is_finished():
        result = dec.process(b"")
        total += len(result)
        if total > max_size:
            raise DecompressionError(
                f"Decompressed output {total} exceeds limit {max_size}"
            )
        chunks.append(result)

    return b"".join(chunks)
```

**Tested behavior (brotli 1.2.0):** The `Decompressor.process()` call returns output in chunks (observed ~32 KiB per call for large data). The loop drains remaining output with `process(b"")` calls until `is_finished()` returns True. The size check triggers between chunks, preventing full allocation of bomb payloads.

### Pattern: Inline vs Separate Module

**Recommendation: Inline in `_envelope.py`.**

Rationale:
- The compression logic is ~20 lines (compress function + safe_decompress function + one constant)
- It is tightly coupled to envelope encrypt/decrypt -- never used independently
- The SDK follows single-purpose module pattern: `_hkdf.py` (key derivation), `_framing.py` (wire framing), `_envelope.py` (envelope encryption). Compression is an envelope concern, not a standalone concern.
- Adding `_compression.py` would create a module with exactly two functions that are only called from one place. Unnecessary indirection.

### Anti-Patterns to Avoid
- **One-shot decompress for untrusted data:** `brotli.decompress(data)` allocates full output. Use streaming `Decompressor` with size checks for bomb protection.
- **Compressing already-encrypted data:** Ciphertext is indistinguishable from random -- Brotli will expand it. The suite byte dispatch handles this correctly (0x01 = uncompressed path).
- **Hardcoding MAX_DECOMPRESSED_SIZE separately from C++:** Define the constant once and reference `MAX_BLOB_DATA_SIZE` from `db/net/framing.h` (100 MiB). The SDK should define its own `MAX_DECOMPRESSED_SIZE = 100 * 1024 * 1024` constant in `_envelope.py`, matching the C++ value.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Brotli compression | Custom compression | `brotli.compress(data, quality=6)` | Battle-tested C library, correct edge case handling |
| Streaming decompression | Manual buffer management | `brotli.Decompressor().process()` loop | Handles incomplete streams, interruptible between chunks |
| Compression bomb detection | Ratio-based heuristics | Streaming decompress + absolute size cap | Ratio checks are bypassable; absolute output cap is definitive |

**Key insight:** The brotli library handles all compression edge cases. The SDK's only responsibility is: (1) choosing when to compress, (2) writing the correct suite byte, and (3) capping decompression output size.

## Common Pitfalls

### Pitfall 1: Suite Byte Must Be Written Into Header BEFORE AEAD AD Computation
**What goes wrong:** If you compute the full header with suite=0x01 and then change it to 0x02, the AEAD associated data (AD) won't match during decryption.
**Why it happens:** The envelope format uses the full header as AD for both DEK wrapping and data encryption. The suite byte is part of the header.
**How to avoid:** Decide the actual suite value (0x01 or 0x02) BEFORE building the header. The current code builds `partial_header` with the suite byte early -- the compression decision must happen before that line.
**Warning signs:** AEAD decryption failures on compressed envelopes.

### Pitfall 2: Compression Happens on Plaintext, Not on Ciphertext
**What goes wrong:** Compressing after AEAD encryption achieves nothing -- ciphertext looks random.
**Why it happens:** Inserting compression at the wrong layer in the encrypt flow.
**How to avoid:** The flow is: plaintext -> [compress] -> AEAD encrypt -> envelope. Compression replaces `plaintext` with `compressed_plaintext` before the AEAD encrypt call.
**Warning signs:** Envelope size doesn't decrease after enabling compression.

### Pitfall 3: Expansion Fallback Must Use Suite 0x01, Not 0x02
**What goes wrong:** If compressed data >= original and you still write suite=0x02, the receiver will try to decompress data that was never compressed, causing a Brotli decode error.
**Why it happens:** Writing suite=0x02 regardless of whether compression actually helped.
**How to avoid:** Check `len(compressed) < len(plaintext)`. Only use suite=0x02 and the compressed data if this is true. Otherwise, fall back to suite=0x01 with original plaintext.
**Warning signs:** `brotli.error` during decryption of "compressed" envelopes that were actually random/incompressible data.

### Pitfall 4: envelope_parse() Must Accept Both Suite Values
**What goes wrong:** `envelope_parse()` currently rejects anything except suite=0x01. If not updated, parsing compressed envelopes raises `MalformedEnvelopeError`.
**Why it happens:** The current code has `if suite != CIPHER_SUITE_ML_KEM_CHACHA: raise`.
**How to avoid:** Update the suite validation in both `envelope_decrypt()` and `envelope_parse()` to accept {0x01, 0x02}.
**Warning signs:** MalformedEnvelopeError on valid suite=0x02 envelopes.

### Pitfall 5: Empty Plaintext Edge Case
**What goes wrong:** Brotli compresses empty bytes to a 1-byte output (0x06). Since 0 < 256-byte threshold, the threshold check correctly skips compression. But if threshold logic is wrong, compressing empty data and then decompressing should still work.
**Why it happens:** Edge case in threshold/expansion checks.
**How to avoid:** Empty plaintext (0 bytes) is below the 256-byte threshold, so it always uses suite=0x01. No special handling needed.
**Warning signs:** Assertion failures on empty plaintext envelope tests.

### Pitfall 6: Decompressor Not Finishing in One Call
**What goes wrong:** Calling `dec.process(compressed)` once and assuming `dec.is_finished()` is True. For large data, the Decompressor may need multiple `process(b"")` calls to drain all output.
**Why it happens:** Brotli's streaming API yields output in chunks (~32 KiB observed per call).
**How to avoid:** Always loop: `while not dec.is_finished(): result = dec.process(b"")` and accumulate chunks.
**Warning signs:** Truncated decompressed output for large payloads.

## Code Examples

### Full encrypt_envelope() Modification (Conceptual)

```python
# Source: Verified against _envelope.py lines 45-139 and brotli 1.2.0 API

import brotli

CIPHER_SUITE_ML_KEM_CHACHA_BROTLI = 0x02
_COMPRESS_THRESHOLD = 256
_BROTLI_QUALITY = 6

def envelope_encrypt(
    plaintext: bytes,
    recipients: list[Identity],
    sender: Identity,
    compress: bool = True,           # NEW parameter (D-07)
) -> bytes:
    # ... existing validation and recipient dedup (unchanged) ...

    # Determine compression and suite (NEW)
    actual_suite = CIPHER_SUITE_ML_KEM_CHACHA
    data_to_encrypt = plaintext

    if compress and len(plaintext) >= _COMPRESS_THRESHOLD:
        compressed = brotli.compress(plaintext, quality=_BROTLI_QUALITY)
        if len(compressed) < len(plaintext):
            data_to_encrypt = compressed
            actual_suite = CIPHER_SUITE_ML_KEM_CHACHA_BROTLI

    # ... existing DEK/nonce generation ...
    # ... existing KEM encapsulation ...

    # Build header with actual_suite (CHANGED: was hardcoded CIPHER_SUITE_ML_KEM_CHACHA)
    partial_header.extend(struct.pack("B", actual_suite))

    # ... rest unchanged, but encrypt data_to_encrypt instead of plaintext ...
    ciphertext = aead_encrypt(data_to_encrypt, ad=full_header, nonce=data_nonce, key=dek)
    return full_header + ciphertext
```

### Full decrypt_envelope() Modification (Conceptual)

```python
# Source: Verified against _envelope.py lines 142-241 and brotli 1.2.0 API

MAX_DECOMPRESSED_SIZE = 100 * 1024 * 1024  # 100 MiB, matches MAX_BLOB_DATA_SIZE

def envelope_decrypt(data: bytes, identity: Identity) -> bytes:
    # ... existing header parse ...

    suite = data[5]
    if suite not in (CIPHER_SUITE_ML_KEM_CHACHA, CIPHER_SUITE_ML_KEM_CHACHA_BROTLI):
        raise MalformedEnvelopeError(f"Unsupported cipher suite: {suite}")

    # ... existing stanza lookup, KEM decap, DEK unwrap, data decrypt ...
    # plaintext = aead_decrypt(...)

    # Decompress if suite=0x02 (NEW)
    if suite == CIPHER_SUITE_ML_KEM_CHACHA_BROTLI:
        plaintext = _safe_decompress(plaintext, MAX_DECOMPRESSED_SIZE)

    return plaintext
```

### Decompression Bomb Protection (Verified Pattern)

```python
# Source: Tested with brotli 1.2.0 on Python 3.14.3

import brotli

def _safe_decompress(compressed: bytes, max_size: int) -> bytes:
    """Decompress Brotli data with output size cap.

    Uses streaming Decompressor to avoid allocating full output
    before checking size. Raises DecompressionError if output
    exceeds max_size.
    """
    dec = brotli.Decompressor()
    chunks: list[bytes] = []
    total = 0

    result = dec.process(compressed)
    total += len(result)
    if total > max_size:
        raise DecompressionError(
            f"Decompressed size {total} exceeds maximum {max_size}"
        )
    chunks.append(result)

    while not dec.is_finished():
        result = dec.process(b"")
        total += len(result)
        if total > max_size:
            raise DecompressionError(
                f"Decompressed size {total} exceeds maximum {max_size}"
            )
        chunks.append(result)

    return b"".join(chunks)
```

## Compression Characteristics (Verified)

Tested with brotli 1.2.0, quality 6, on Python 3.14.3:

| Input Type | Input Size | Compressed Size | Ratio | Notes |
|-----------|-----------|----------------|-------|-------|
| Tiny data | 4 B | 8 B | 200% | Expands -- below threshold, never compressed |
| Small repetitive | 16 B | 10 B | 62.5% | Below threshold, never compressed |
| 256 B repetitive | 256 B | 10 B | 3.9% | At threshold -- compressed |
| JSON repeated | ~3 KB | ~50 B | 1.6% | Excellent for structured data |
| Random bytes | 10 KB | 10,004 B | 100.0% | Incompressible -- expansion fallback triggers |
| Large repetitive | 1 MiB | 190 B | 0.01% | Best case for repetitive data |
| All-same-byte | 10 MiB | 1,904 B | 0.02% | Extreme ratio, bomb-like |

**Speed at quality 6:** ~0.2 ms to compress 439 KiB, ~0.4 ms to decompress. Negligible compared to ML-KEM operations (~tens of ms).

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| No envelope compression | Brotli compression before AEAD (suite=0x02) | Phase 87 (now) | Reduces envelope blob size for compressible plaintext |
| Single cipher suite (0x01) | Two cipher suites (0x01 uncompressed, 0x02 Brotli) | Phase 87 (now) | Forward-compatible suite dispatch |
| brotli <1.2.0 (no output limit) | brotli 1.2.0 (output_buffer_limit in Decompressor) | 2025-11 | Enables streaming decompression bomb protection |

## Open Questions

1. **New exception type for decompression bombs**
   - What we know: `MalformedEnvelopeError` is the closest existing exception. But a decompression bomb is not a "malformed envelope" -- the envelope is valid, the decompressed content is just too large.
   - What's unclear: Whether to add `DecompressionError` as a new CryptoError subclass, or reuse an existing exception.
   - Recommendation: Add `DecompressionError(CryptoError)` to the exception hierarchy. It's a distinct failure mode. The planner should include this as a task.

2. **Test vector format for suite=0x02**
   - What we know: Existing `envelope_vectors.json` contains suite=0x01 vectors generated by the C++ test_vector_generator. Suite=0x02 vectors are Python-generated only (D-11).
   - What's unclear: Whether to add to the same file or create a new `compressed_envelope_vectors.json`.
   - Recommendation: Add to the same `envelope_vectors.json` file with `expected_suite: 2`. The test already dispatches on suite byte.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest (latest) with asyncio_mode=auto |
| Config file | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && .venv/bin/python -m pytest tests/test_envelope.py -x` |
| Full suite command | `cd sdk/python && .venv/bin/python -m pytest tests/ -x` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| COMP-01 (rewritten) | SDK compresses plaintext before AEAD with suite=0x02 | unit | `pytest tests/test_envelope.py::test_compress_encrypt_roundtrip -x` | Wave 0 |
| COMP-02 (rewritten) | Skip compression for <256 byte plaintext; expansion fallback | unit | `pytest tests/test_envelope.py::test_compress_threshold -x` | Wave 0 |
| COMP-03 (rewritten) | Decompression bomb rejected before full decompression | unit | `pytest tests/test_envelope.py::test_decompression_bomb_rejected -x` | Wave 0 |
| COMP-04 (rewritten) | Both suite=0x01 and suite=0x02 decrypt correctly | unit | `pytest tests/test_envelope.py::test_decrypt_both_suites -x` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd sdk/python && .venv/bin/python -m pytest tests/test_envelope.py -x`
- **Per wave merge:** `cd sdk/python && .venv/bin/python -m pytest tests/ -x`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] New tests in `tests/test_envelope.py` -- suite=0x02 roundtrip, threshold behavior, expansion fallback, bomb protection, backward compatibility
- [ ] brotli package must be installed in venv: `pip install brotli~=1.2`
- [ ] SDK venv has broken symlink (python3.14 -> linuxbrew path that no longer exists). May need venv recreation before tests run.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Python 3 | SDK tests | Yes | 3.14.3 | -- |
| brotli PyPI package | Compression | Not yet installed | 1.2.0 (verified available) | pip install brotli~=1.2 |
| SDK virtualenv | Test execution | Broken symlink | -- | Recreate: `python3 -m venv sdk/python/.venv` |
| pytest | Test framework | In venv (if working) | -- | pip install pytest |

**Missing dependencies with no fallback:**
- None (brotli is installable via pip)

**Missing dependencies with fallback:**
- SDK virtualenv has broken python3.14 symlink (linuxbrew path no longer exists). Fallback: recreate venv with system python3 before running tests.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/_envelope.py` -- Current envelope encryption implementation, read in full
- `sdk/python/pyproject.toml` -- Current dependency list and project config
- `sdk/python/tests/test_envelope.py` -- 450 lines of existing envelope tests
- `db/PROTOCOL.md` lines 949-1053 -- Envelope encryption specification
- `db/net/framing.h` -- MAX_BLOB_DATA_SIZE = 100 MiB constant
- Brotli 1.2.0 API tested locally: compress(), decompress(), Decompressor.process(), is_finished()

### Secondary (MEDIUM confidence)
- [PyPI brotli page](https://pypi.org/project/brotli/) -- Version 1.2.0, released 2025-11-05
- [google/brotli GitHub](https://github.com/google/brotli/blob/master/python/brotli.py) -- API signatures
- [google/brotli PR #1201](https://github.com/google/brotli/pull/1201) -- output_buffer_limit feature, merged Jan 2025

### Tertiary (LOW confidence)
- [CVE-2025-6176](https://www.miggo.io/vulnerability-database/cve/CVE-2025-6176) -- Brotli decompression bomb CVE, confirms brotli <1.2.0 has no output size validation. Validates importance of using 1.2.0+.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- brotli 1.2.0 installed and API tested locally
- Architecture: HIGH -- modification targets are small and well-understood (two functions in _envelope.py)
- Pitfalls: HIGH -- all pitfalls verified through code reading and API testing
- Compression characteristics: HIGH -- measured with actual brotli 1.2.0 on Python 3.14

**Research date:** 2026-04-05
**Valid until:** 2026-05-05 (stable domain, brotli API unlikely to change)
