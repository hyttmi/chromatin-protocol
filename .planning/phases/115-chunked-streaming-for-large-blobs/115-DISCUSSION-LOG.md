# Phase 115: Chunked Streaming for Large Blobs - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-14
**Phase:** 115-chunked-streaming-for-large-blobs
**Areas discussed:** Streaming scope, UDS wire framing, AEAD chunking, HTTP chunked encoding

---

## Streaming Scope

### Direction

| Option | Description | Selected |
|--------|-------------|----------|
| Both directions | Stream uploads (HTTP→UDS) and downloads (UDS→HTTP). Fixes both write and read path buffering. | ✓ |
| Downloads only | Stream UDS→HTTP only. Most impactful — read path has 5-6 copies. | |
| Uploads only | Stream HTTP→UDS only. Less impactful since write responses are small. | |

**User's choice:** Both directions (Recommended)
**Notes:** None

### Threshold

| Option | Description | Selected |
|--------|-------------|----------|
| 1 MiB (match chunk size) | Aligns with Phase 999.8's 1 MiB chunk size. Small blobs handled inline. | ✓ |
| 64 KiB (match offload threshold) | Aligns with Phase 114's OFFLOAD_THRESHOLD. More aggressive streaming. | |
| Always stream | No threshold. All blob I/O uses chunked path. | |

**User's choice:** 1 MiB (matches chunk size)
**Notes:** None

### Max Size

| Option | Description | Selected |
|--------|-------------|----------|
| Keep 100 MiB limit | Same as node's MAX_BLOB_DATA_SIZE. Streaming improves memory, not max size. | |
| Raise to 1 GiB | Streaming enables larger blobs. But node also needs limit raised. | |
| You decide | Claude picks. | |

**User's choice:** Other — raise to 500 MiB
**Notes:** "raise to 500MiB, we can raise in node too as it supports chunking also"

### Backpressure

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, bounded buffer | Fixed-size chunk queue. Producer pauses when full. | ✓ |
| No, fire-and-forget | Producer writes as fast as possible. Risks unbounded memory. | |
| You decide | Claude picks. | |

**User's choice:** Yes, bounded buffer (Recommended)
**Notes:** None

---

## UDS Wire Framing

### Framing Mode

| Option | Description | Selected |
|--------|-------------|----------|
| Chunked sub-frames | Add chunked framing mode within logical messages. Small messages use single-frame. | ✓ |
| Multiple independent frames | Break into separate UDS frames with sequence numbers. | |
| Keep full-frame, change relay only | Don't change UDS format. Relay buffers full blob at UDS boundary. | |

**User's choice:** Chunked sub-frames (Recommended)
**Notes:** None

### Chunk Size

| Option | Description | Selected |
|--------|-------------|----------|
| 1 MiB (match blob chunking) | Same as Phase 999.8's database chunk size. Consistent across stack. | ✓ |
| 64 KiB (match AEAD offload) | Smaller, more granular streaming. | |
| 256 KiB (compromise) | Balance between overhead and granularity. | |

**User's choice:** 1 MiB (match blob chunking)
**Notes:** None

### Signaling Mechanism

| Option | Description | Selected |
|--------|-------------|----------|
| Flag in frame header | Extend 4-byte length header with flags byte. | |
| TrustedHello capability | Add capability field to handshake. | |
| Magic length sentinel | Special length value signals chunked mode. | |

**User's choice:** Other — Claude's discretion
**Notes:** "do what is best and remember that it's still pre MVP on all, db, relay etc. even if i said we don't need to touch db it should be touched for this change"

---

## AEAD Chunking

### Authentication Mode

| Option | Description | Selected |
|--------|-------------|----------|
| Per-chunk AEAD | Each 1 MiB sub-frame gets own ChaCha20-Poly1305 with sequential nonces. | ✓ |
| Encrypt-then-chunk | Encrypt entire payload, then split. Must reassemble before auth. | |
| No per-chunk AEAD | Skip AEAD for UDS entirely. Rely on UDS permissions. | |

**User's choice:** Per-chunk AEAD (Recommended)
**Notes:** None

### Nonce Space

| Option | Description | Selected |
|--------|-------------|----------|
| Shared counter | Single monotonic counter for all frames. Each chunk consumes one nonce. | ✓ |
| Separate counter for chunks | Non-chunked and chunked use different counters. | |
| You decide | Claude picks. | |

**User's choice:** Shared counter (Recommended)
**Notes:** None

---

## HTTP Chunked Encoding

### Download Response

| Option | Description | Selected |
|--------|-------------|----------|
| Chunked Transfer-Encoding | Stream chunks as they arrive from UDS. No Content-Length needed upfront. | ✓ |
| Content-Length with scatter-gather | Keep Content-Length, use Asio scatter-gather to avoid copy. | |
| You decide | Claude picks. | |

**User's choice:** Chunked Transfer-Encoding (Recommended)
**Notes:** None

### Upload Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Stream Content-Length body in chunks | Client sends Content-Length. Relay reads in 1 MiB chunks and forwards to UDS incrementally. | ✓ |
| Accept chunked uploads too | Support both Content-Length and Transfer-Encoding: chunked for uploads. | |
| You decide | Claude picks. | |

**User's choice:** Stream Content-Length body in chunks
**Notes:** None

### Serialize Refactor

| Option | Description | Selected |
|--------|-------------|----------|
| Blob endpoints only | Keep serialize() for JSON responses. Add streaming for blob handlers only. | |
| Refactor all responses | Replace serialize() with scatter-gather writes everywhere. | ✓ |
| You decide | Claude picks. | |

**User's choice:** Refactor all responses
**Notes:** None

---

## Claude's Discretion

- UDS chunked framing signaling mechanism (pre-MVP, pick simplest)
- Exact bounded queue size for backpressure
- Whether TransportCodec needs changes or can be bypassed for raw blob data
- Thread pool interaction for per-chunk AEAD
- Error handling for partial chunk delivery

## Deferred Ideas

- Client-side chunked transfer encoding for uploads
- HTTP/2 streaming
- Zero-copy splice/sendfile
- Streaming FlatBuffer parsing
