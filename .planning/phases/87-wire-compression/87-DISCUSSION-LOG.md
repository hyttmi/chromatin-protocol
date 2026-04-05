# Phase 87: SDK Envelope Compression - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-05
**Phase:** 87-wire-compression (pivoted to SDK Envelope Compression)
**Areas discussed:** Compression layer placement, Brotli quality & config, Envelope blob detection, SDK brotli dependency, Small plaintext threshold, Cross-SDK test vectors, ROADMAP.md rewrite scope

---

## Scope Pivot Discovery

During discussion of "Envelope blob detection," the user questioned whether wire compression was useful at all given that blob data is always encrypted client-side. Analysis confirmed:
- Node-to-node traffic is encrypted envelope data (incompressible) + hashes (high-entropy)
- Wire compression at the transport layer would save zero bandwidth
- Compression is only effective before encryption, which is an SDK concern

**User's decision:** Repurpose Phase 87 from node wire compression to SDK pre-encrypt envelope compression.

---

## Compression Layer Placement

| Option | Description | Selected |
|--------|-------------|----------|
| Compress full FlatBuffer | TransportCodec::encode() → [flag][brotli(flatbuffer)] → AEAD encrypt. Clean transparent layer. | (original, superseded) |
| Compress inner payload only | Compress payload before FlatBuffer encoding. Requires schema change. | |
| Flag outside AEAD | Flag byte unencrypted before AEAD frame. Leaks metadata. | |

**User's choice:** Compress full FlatBuffer (before scope pivot)
**Notes:** After scope pivot, compression moved entirely to SDK `encrypt_envelope()`.

## Threshold Application

| Option | Description | Selected |
|--------|-------------|----------|
| FlatBuffer-encoded size | Check after TransportCodec::encode(). Threshold matches reality. | (original, superseded) |
| Raw payload size | Check before FlatBuffer encoding. | |

**User's choice:** FlatBuffer-encoded size (before scope pivot)

## Expansion Fallback

| Option | Description | Selected |
|--------|-------------|----------|
| Always fall back | If brotli output >= input, send uncompressed. Never makes things worse. | ✓ |
| Send compressed anyway | Always send compressed once past threshold. | |

**User's choice:** Always fall back

## Code Organization

| Option | Description | Selected |
|--------|-------------|----------|
| Standalone utility | New db/net/compression.h with compress()/decompress(). Testable in isolation. | (original, superseded) |
| Inline in Connection | Logic in send_message() and message loop. | |

**User's choice:** Standalone utility (before scope pivot to SDK)

## Message Compression Scope

| Option | Description | Selected |
|--------|-------------|----------|
| All post-handshake messages | Every message goes through compress layer. Small ones auto-skip via threshold. | (original, superseded) |
| Only sync & data types | Selective type allowlist. | |

**User's choice:** All post-handshake messages (before scope pivot)

## Brotli Quality Level

| Option | Description | Selected |
|--------|-------------|----------|
| Quality 1 (fast) | Minimal CPU, modest compression. | |
| Quality 4 (balanced) | Good speed/ratio for wire traffic. | (initially selected for node) |
| Quality 6 (better ratio) | Better ratio, more CPU. Client-side can afford it. | ✓ |
| Quality 11 (max) | Best ratio, slow. | |

**User's choice:** Quality 4 initially (for node), then Quality 6 after pivot to SDK (client CPU less constrained)

## Brotli Configuration

| Option | Description | Selected |
|--------|-------------|----------|
| Configurable + SIGHUP | Config file option, SIGHUP-reloadable. | |
| Hardcoded | Baked-in quality level. Can add config later. | ✓ |

**User's choice:** Hardcoded at quality 6

## Brotli C++ Dependency

| Option | Description | Selected |
|--------|-------------|----------|
| FetchContent | CMake FetchContent from Google repo. Consistent with project convention. | (superseded — no C++ changes) |
| System package | Require libbrotli-dev. | |

**User's choice:** FetchContent (before scope pivot — no longer applicable)

## Compression Kill Switch

| Option | Description | Selected |
|--------|-------------|----------|
| No kill switch | Always active. Threshold and expansion fallback handle edge cases. YAGNI. | ✓ |
| compression_enabled config | Boolean to disable. | |

**User's choice:** No kill switch

## Envelope Blob Detection (Pivotal Discussion)

| Option | Description | Selected |
|--------|-------------|----------|
| Check CENV magic prefix | 4-byte check before compress. Skips envelope data. | |
| Never skip / expansion fallback | Brotli tries, fails on encrypted data, falls back to uncompressed. | ✓ |
| Blob metadata flag | is_encrypted field in storage. Schema change. | |

**User's choice:** Expansion fallback — but then questioned whether wire compression was useful at all, leading to the scope pivot.

## Compression Signal in Envelope

| Option | Description | Selected |
|--------|-------------|----------|
| New cipher suite byte | suite=0x02 means compressed. Clean, no format change. | ✓ |
| Flag byte in plaintext | Prepend 0x00/0x01 to plaintext before encryption. | |
| Envelope version bump | Overloads version semantics. | |

**User's choice:** New cipher suite byte (0x02)

## SDK Compression Default

| Option | Description | Selected |
|--------|-------------|----------|
| On by default, opt-out | encrypt_envelope() compresses by default. compress=False available. | ✓ |
| Off by default, opt-in | suite=0x01 default. compress=True to enable. | |
| Always compress | No option. | |

**User's choice:** On by default, opt-out available

## Backward-Compatible Decryption

| Option | Description | Selected |
|--------|-------------|----------|
| Transparent | decrypt_envelope() handles both suites. | ✓ |
| Require explicit suite | Caller specifies expected suite. | |

**User's choice:** Transparent

## Decompression Bomb Cap

| Option | Description | Selected |
|--------|-------------|----------|
| MAX_BLOB_DATA_SIZE (100 MiB) | Same as node blob cap. Protocol invariant. | ✓ |
| Configurable limit | Caller-settable max. | |

**User's choice:** MAX_BLOB_DATA_SIZE

## Envelope-Only Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Envelope only | Compression in encrypt/decrypt_envelope() only. write_blob() unchanged. | ✓ |
| Both envelope and plain | Add compression to write_blob() too. | |

**User's choice:** Envelope only

## Python Brotli Package

| Option | Description | Selected |
|--------|-------------|----------|
| brotli (Google) | C extension, fast, well-maintained, 30M+ monthly downloads. | ✓ |
| brotlicffi | CFFI-based, PyPy-friendly. | |
| brotlipy | Deprecated. | |

**User's choice:** brotli (Google)

## Dependency Type

| Option | Description | Selected |
|--------|-------------|----------|
| Hard dependency | Always required. Consistent behavior. | ✓ |
| Optional with fallback | Graceful degradation to uncompressed. | |

**User's choice:** Hard dependency

## PROTOCOL.md Update

| Option | Description | Selected |
|--------|-------------|----------|
| Update PROTOCOL.md | Document suite=0x02 in envelope section. Protocol-level change. | ✓ |
| SDK docs only | Node doesn't change, so skip PROTOCOL.md. | |

**User's choice:** Update PROTOCOL.md

## Small Plaintext Threshold

| Option | Description | Selected |
|--------|-------------|----------|
| 256-byte threshold | Same as original wire design. Skip Brotli for tiny plaintext. | ✓ |
| Always compress | No threshold. Expansion fallback handles bad cases. | |
| 128-byte threshold | Lower, more aggressive. | |

**User's choice:** 256-byte threshold

## Cross-SDK Test Vectors

| Option | Description | Selected |
|--------|-------------|----------|
| Python generates vectors | Reference implementation. Future SDKs validate against these. | ✓ |
| Extend C++ generator | Add Brotli to C++ tooling. | |
| Standalone script | Separate vector generation tool. | |

**User's choice:** Python generates vectors

## ROADMAP.md Rewrite Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Full rewrite of Phase 87 | Rename, rewrite goal/criteria/requirements. | ✓ |
| Rewrite during plan-phase | Defer to planner. | |
| Keep original, add notes | Less churn but confusing. | |

**User's choice:** Full rewrite

---

## Claude's Discretion

- Internal compression module structure in the SDK
- Test strategy and structure
- Decompression bomb implementation approach

## Deferred Ideas

- Node-side transport compression (if unencrypted blob use case emerges)
- Compression for non-envelope blobs via write_blob()
- Configurable Brotli quality
