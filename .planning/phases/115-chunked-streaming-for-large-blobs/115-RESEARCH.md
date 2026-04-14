# Phase 115: Chunked Streaming for Large Blobs - Research

**Researched:** 2026-04-14
**Domain:** Streaming I/O, UDS chunked framing, HTTP chunked transfer encoding, per-chunk AEAD
**Confidence:** HIGH

## Summary

Phase 115 eliminates full-blob buffering in the relay for blobs >= 1 MiB by implementing chunked streaming through the entire relay pipeline: HTTP body read -> UDS chunked send -> node processing -> UDS chunked response -> HTTP chunked response. The existing relay buffers entire blobs in memory at 5-6 copy points, which is untenable at the new 500 MiB limit. Both upload and download paths must stream in 1 MiB chunks with bounded backpressure to prevent OOM.

This phase touches both the relay (HTTP layer, UDS multiplexer, AEAD, TransportCodec, response model) and the node (framing.h MAX sizes, Connection's send/recv). Since this is pre-MVP, there are no backward compatibility constraints -- both sides change simultaneously. The core challenge is designing a UDS sub-frame protocol that both the node's Connection class and the relay's UdsMultiplexer understand, while maintaining the shared monotonic AEAD nonce counter (each chunk consumes one nonce).

**Primary recommendation:** Implement in 4 plans: (1) UDS chunked sub-frame protocol + node-side changes, (2) relay UDS multiplexer chunked send/recv + per-chunk AEAD, (3) HTTP streaming upload (read body in chunks -> forward to UDS) + download (UDS chunks -> HTTP chunked transfer encoding), (4) HttpResponse scatter-gather refactor + integration testing.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Both directions streamed: upload (HTTP->UDS) and download (UDS->HTTP). All full-blob buffering eliminated for blobs >= 1 MiB.
- **D-02:** Streaming threshold: 1 MiB (1,048,576 bytes). Matches Phase 999.8's database chunk size. Blobs under 1 MiB handled inline with existing full-buffer path.
- **D-03:** MAX_BLOB_DATA_SIZE raised from 100 MiB to 500 MiB in both node (`db/net/framing.h`) and relay (`relay/http/http_connection.cpp` MAX_BODY_SIZE, `relay/core/uds_multiplexer.h` MAX_FRAME_SIZE). Pre-MVP -- both sides can change freely.
- **D-04:** Bounded backpressure: fixed-size chunk queue between producer and consumer. Producer pauses when queue is full. Prevents OOM from fast producer / slow consumer.
- **D-05:** Chunked sub-frame mode for UDS. Large payloads broken into 1 MiB sub-frames within a single logical message. Small messages (< 1 MiB) use existing single-frame format.
- **D-06:** 1 MiB UDS sub-frame chunk size. One UDS sub-frame per database chunk. Consistent across the stack.
- **D-07:** Both node and relay are modified for chunked UDS framing. Pre-MVP -- no backward compatibility constraints.
- **D-08:** Per-chunk AEAD authentication. Each 1 MiB sub-frame gets its own ChaCha20-Poly1305 encrypt/decrypt with sequential nonces. Each chunk independently authenticated -- corrupt chunk identified immediately.
- **D-09:** Shared monotonic nonce counter for all frames (both chunked and non-chunked). Each chunk consumes one nonce from the same counter. Simple, no nonce reuse risk.
- **D-10:** HTTP chunked transfer encoding for blob responses (downloads). Relay streams chunks as they arrive from UDS. No Content-Length needed upfront for blob responses.
- **D-11:** Upload streaming via Content-Length body chunking. Client sends Content-Length header. Relay reads body in 1 MiB chunks from socket and forwards each to UDS incrementally. No client-side chunked encoding required.
- **D-12:** HttpResponse::serialize() refactored for ALL responses to use scatter-gather writes (Asio buffer sequences). Eliminates the header+body concatenation into single std::string.

### Claude's Discretion
- UDS chunked framing signaling mechanism (flags byte, capability negotiation, or sentinel -- pre-MVP, pick simplest)
- Exact bounded queue size for backpressure (e.g., 4-8 chunks in flight)
- Whether to add a streaming FlatBuffer encode/decode path or bypass FlatBuffer for the blob data portion of large messages
- Error handling for partial chunk delivery (connection drop mid-stream)
- Whether TransportCodec needs changes or can be bypassed for raw blob data
- Thread pool interaction: whether per-chunk AEAD should use offload_if_large or always run inline (1 MiB chunks are above the 64 KiB threshold)

### Deferred Ideas (OUT OF SCOPE)
- Client-side chunked transfer encoding for uploads (Transfer-Encoding: chunked) -- not needed pre-MVP, Content-Length streaming sufficient
- HTTP/2 streaming with multiplexed frames -- future optimization
- Zero-copy splice/sendfile between UDS and TCP sockets -- OS-level optimization, complex
- Streaming FlatBuffer parsing -- may not be needed if raw blob data bypasses FlatBuffer
</user_constraints>

## Architecture Patterns

### Current Data Flow (Full-Buffer -- What Must Change)

```
UPLOAD (HTTP -> Node):
  HTTP body -> read_body() [full buffer] -> handler -> TransportCodec::encode() [copy into FlatBuffer]
  -> send_queue_ [copy] -> send_encrypted() [AEAD encrypt, copy] -> UDS wire

DOWNLOAD (Node -> HTTP):
  UDS wire -> recv_encrypted() [AEAD decrypt, copy] -> recv_raw() [full buffer alloc]
  -> TransportCodec::decode() [copy] -> ResponsePromise [full payload] -> handler
  -> HttpResponse::binary() [copy] -> serialize() [header+body concat] -> async_write
```

Total copies: 3-4 on upload, 5-6 on download. At 500 MiB, this means 1.5-3 GB peak memory per concurrent large blob.

### Target Data Flow (Chunked Streaming)

```
UPLOAD (HTTP -> UDS, streamed):
  HTTP body -> read_body_chunk(1 MiB) -> TransportCodec::encode_header() [once]
  -> for each chunk: AEAD encrypt(chunk) -> send_raw(sub-frame) -> UDS wire
  Backpressure: pause HTTP read when UDS send queue has N chunks pending

DOWNLOAD (UDS -> HTTP, streamed):
  UDS wire -> for each sub-frame: recv_raw(sub-frame) -> AEAD decrypt(chunk)
  -> HTTP chunked-TE write(chunk) -> TCP wire
  Backpressure: pause UDS recv when HTTP write backpressures
```

Peak memory per concurrent large blob: ~4-8 MiB (queue depth * chunk size), not 500 MiB.

### Recommended UDS Chunked Sub-Frame Protocol

**Signaling mechanism (Claude's discretion -- recommendation: flags byte):**

Use a 1-byte flags field in a thin chunked header prepended to each sub-frame. This is the simplest approach for pre-MVP:

```
Regular frame (unchanged for small messages):
  [4B BE total_length][payload]

Chunked frame (for payloads >= 1 MiB):
  [4B BE header_length][chunked_header]     <- "begin" sub-frame
  [4B BE chunk_length][chunk_1_data]         <- data sub-frame
  [4B BE chunk_length][chunk_2_data]         <- data sub-frame
  ...
  [4B BE 0]                                  <- "end" sentinel (zero-length frame)

Chunked header format:
  [1B flags: 0x01 = CHUNKED_BEGIN]
  [1B message_type]
  [4B BE request_id]
  [8B BE total_payload_size]     <- so receiver knows expected total
  [remaining: metadata portion of TransportMessage, if any]

Each data sub-frame:
  [4B BE chunk_length][chunk_data]
  (Each goes through AEAD encrypt/decrypt independently per D-08)

End sentinel:
  [4B BE 0]  <- zero length signals end of chunked sequence
```

**Key design points:**
- The flags byte 0x01 in the first sub-frame distinguishes chunked mode from regular frames (regular frames never start with 0x01 because they contain a FlatBuffer which has a different header layout)
- Each sub-frame goes through the existing `send_raw`/`recv_raw` framing (4B length prefix) and AEAD encrypt/decrypt with sequential nonces
- The zero-length end sentinel is unambiguous (no valid frame has zero payload)
- Total payload size in the header allows the receiver to pre-validate against MAX_BLOB_DATA_SIZE before reading chunks

**Alternative considered:** A flags byte embedded in the existing 4B length header (e.g., high bit). Rejected because it halves MAX_FRAME_SIZE to 2 GiB and creates ambiguity with existing framing.

### Backpressure Queue Design

**Recommended queue depth: 4 chunks (4 MiB total).**

Rationale:
- 1 chunk is too tight -- stalls pipeline on any I/O hiccup
- 8 chunks = 8 MiB per concurrent stream -- at 100 concurrent large blob downloads, that is 800 MiB just for queues
- 4 chunks provides 1 chunk being written, 1 decrypted, 1 being read from UDS, 1 buffer -- adequate pipeline depth
- At 4 MiB per stream and 100 concurrent streams: 400 MiB memory ceiling -- manageable

Implementation: `asio::steady_timer` as a signal mechanism (same pattern as SseWriter and drain_send_queue). Producer pushes chunk, if queue full -> co_await signal. Consumer pops chunk, signals producer after each pop.

### FlatBuffer Bypass for Large Blob Data

**Recommendation: Bypass TransportCodec for the blob data portion of chunked messages.**

Currently TransportCodec::encode() copies the entire payload into a FlatBufferBuilder. For chunked mode:
- The chunked header carries type + request_id (the metadata TransportCodec normally wraps)
- The chunk data sub-frames carry raw blob bytes -- no FlatBuffer wrapping
- On the receive side, the chunked header provides type + request_id, so TransportCodec::decode() is not needed for the data sub-frames

This avoids the FlatBuffer O(payload_size) copy for large blobs while keeping TransportCodec unchanged for all small messages.

### Per-Chunk AEAD Threading

**Recommendation: Always offload per-chunk AEAD to the thread pool.**

Each 1 MiB chunk exceeds the 64 KiB OFFLOAD_THRESHOLD by 16x. Using `offload_if_large()` would always trigger the offload path anyway. For consistency and to avoid stalling the event loop during streaming, all chunk AEAD operations should go through the offload path. The counter-by-value pattern from D-10 (Phase 114) applies: increment on event loop, capture by value, offload encrypt/decrypt.

### Error Handling for Partial Chunk Delivery

**Recommendation: Abort and propagate error.**

If a connection drops mid-stream during chunked transfer:
- **Upload (HTTP->UDS):** The relay has been forwarding chunks to the node. The node receives a partial message (the chunked sequence lacks the end sentinel). Node should discard partial data and the relay should NOT send a WriteAck to the HTTP client. The HTTP connection is already closed, so no explicit error response is needed.
- **Download (UDS->HTTP):** The relay is streaming HTTP chunked-TE to the client. If UDS drops mid-stream, the relay cannot send a proper error (HTTP headers already sent with 200). It closes the TCP connection abruptly -- the client detects an incomplete chunked-TE stream (no terminating `0\r\n\r\n`).

Both cases are safe because the underlying blob storage is transactional -- partial writes don't corrupt data.

### Recommended Project Structure Changes

```
relay/
  core/
    uds_multiplexer.h/cpp      # Add chunked send/recv methods
    chunked_stream.h            # NEW: ChunkedSender/ChunkedReceiver abstractions
  http/
    http_connection.h/cpp       # Add read_body_chunked(), add async_write_buffers()
    http_response.h             # Refactor serialize() to return buffer sequence
    handlers_data.cpp           # Streaming variants of blob read/write handlers
  wire/
    transport_codec.h/cpp       # Add encode_chunked_header(), decode_chunked_header()
    aead.h/cpp                  # No changes (already per-message, works per-chunk)
db/
  net/
    framing.h                   # Raise MAX_BLOB_DATA_SIZE to 500 MiB, MAX_FRAME_SIZE to 510 MiB
    connection.h/cpp            # Add chunked recv/send support in message_loop
```

### Anti-Patterns to Avoid
- **Full-buffer fallback for "simplicity":** Do not read the entire HTTP body before deciding to stream. Check Content-Length upfront and branch to streaming path if >= 1 MiB.
- **Shared mutable state across chunks:** Each chunk's AEAD counter must be captured by value before offload (Phase 114 D-10 pattern). Do not capture the counter by reference.
- **Blocking the event loop during streaming:** All AEAD operations on 1 MiB chunks MUST be offloaded. The event loop thread should only coordinate I/O, never perform crypto on chunks.
- **Unbounded queues:** Without backpressure, a fast UDS producer + slow HTTP consumer would buffer the entire blob in memory, defeating the purpose.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Buffer sequence I/O | Custom multi-buffer write loop | `asio::async_write(stream, buffer_sequence)` | Asio handles partial writes, scatter-gather natively |
| HTTP chunked-TE format | Manual hex size + CRLF concatenation | Small inline helper (3 lines) | Format is trivial but error-prone with CRLF placement |
| Backpressure signaling | Custom condition variable / mutex | `asio::steady_timer` signal pattern | Already proven in SseWriter and drain_send_queue |
| AEAD per-chunk | New AEAD mode or wrapper | Existing `wire::aead_encrypt()`/`aead_decrypt()` | Each chunk is just a regular AEAD message with the next nonce |

**Key insight:** The streaming infrastructure is composed from primitives the codebase already has -- AEAD with sequential nonces, send_raw/recv_raw framing, steady_timer signaling, offload_if_large. The novelty is in the protocol (chunked sub-frames) and the plumbing (connecting HTTP I/O to UDS I/O with backpressure), not in new cryptographic or I/O primitives.

## Common Pitfalls

### Pitfall 1: Nonce Counter Desync During Chunked Transfer
**What goes wrong:** If chunked and non-chunked messages share the nonce counter (D-09), interleaving a small message during a chunked transfer could desync sender/receiver counts.
**Why it happens:** The send queue is a deque -- if a non-chunked message (e.g., a Subscribe from another client) is queued between chunks of a large blob transfer, the receiver expects chunks but gets a different message type.
**How to avoid:** During a chunked sequence, the drain_send_queue must complete the entire chunked sequence before draining any other messages. The chunked send is atomic from the queue's perspective -- it holds the drain lock for the entire sequence. This is already the natural behavior if the chunked send is a single coroutine that writes all sub-frames sequentially.
**Warning signs:** AEAD decrypt failures immediately after a large blob transfer.

### Pitfall 2: HTTP Response Headers Already Sent
**What goes wrong:** During a streaming download, the relay sends `HTTP/1.1 200 OK` + `Transfer-Encoding: chunked` headers immediately. If the UDS read fails mid-stream, it is too late to send a 5xx error.
**Why it happens:** HTTP/1.1 does not allow changing the status code after headers are sent.
**How to avoid:** Accept this limitation. Close the connection on mid-stream failure. The incomplete chunked-TE (missing `0\r\n\r\n` terminator) signals error to the client. Document this in the API.
**Warning signs:** Client receives partial data without error indication.

### Pitfall 3: MAX_FRAME_SIZE vs MAX_BLOB_DATA_SIZE Mismatch
**What goes wrong:** Raising MAX_BLOB_DATA_SIZE to 500 MiB without raising MAX_FRAME_SIZE causes node to reject its own responses for large blobs.
**Why it happens:** MAX_FRAME_SIZE (110 MiB) was set to accommodate 100 MiB blob + overhead. With 500 MiB blobs in chunked mode, individual sub-frames are only 1 MiB, but the sentinel/header values in framing checks might still reference the old limits.
**How to avoid:** In chunked mode, MAX_FRAME_SIZE validation applies per sub-frame, not per logical message. For non-chunked messages (< 1 MiB), the existing MAX_FRAME_SIZE is still adequate. The key is: keep MAX_FRAME_SIZE at a per-frame level (a few MiB), not raise it to 500 MiB. Only MAX_BLOB_DATA_SIZE increases.
**Warning signs:** "frame exceeds maximum size" errors when trying to send/receive large blobs.

### Pitfall 4: Read Path Status Byte in Chunked Responses
**What goes wrong:** ReadResponse format is `[status:1][FlatBuffer blob data...]`. In the current path, the handler checks the status byte before returning. In chunked mode, the first sub-frame from the node contains the status byte -- the relay must read and check it before streaming to the client.
**Why it happens:** The chunked download starts sending HTTP 200 + chunked-TE headers. If the first chunk reveals status != 0x01 (not found), it is too late.
**How to avoid:** For ReadResponse specifically, read the first sub-frame (or the chunked header) to extract the status byte BEFORE committing to the HTTP response. If not found, send a normal 404. Only start chunked streaming after confirming the blob exists.
**Warning signs:** Client receives empty chunked-TE body for non-existent blobs instead of 404.

### Pitfall 5: HttpResponse Scatter-Gather Breaking Existing Code
**What goes wrong:** Refactoring serialize() to return a buffer sequence instead of std::string changes the interface used by every response path (auth, health, metrics, queries, errors).
**Why it happens:** D-12 requires scatter-gather for ALL responses, not just blob responses.
**How to avoid:** Keep the existing `serialize()` -> `std::string` for non-streaming responses (they are small and the copy is negligible). Add a new `write_to()` method that writes headers + body as a buffer sequence directly to the stream. The streaming blob response uses a completely different code path (chunked-TE writer) that never touches HttpResponse at all.
**Warning signs:** Compilation errors across 20+ files, test breakage.

### Pitfall 6: Single-Threaded Event Loop Starvation During Streaming
**What goes wrong:** A single 500 MiB blob transfer monopolizes the event loop's coroutine scheduler -- other clients' requests get no attention.
**Why it happens:** The streaming coroutine yields at each chunk's I/O points (async_read_some, async_write), but AEAD offload + I/O for 500 chunks takes significant wall time.
**How to avoid:** AEAD offload moves crypto off the event loop. The remaining event-loop work per chunk is two async I/O calls (read from one socket, write to another), each of which yields to the scheduler. At 500 chunks with 2 yields each, the scheduler gets 1000 opportunities to service other coroutines. This is adequate. No special intervention needed beyond the AEAD offload.
**Warning signs:** High p99 latency for small queries during large blob transfers (this was already measured in Phase 113 benchmarks -- the current bottleneck is the event loop doing AEAD inline, which Phase 114 offloading addresses).

## Code Examples

### HTTP Chunked Transfer Encoding Format (RFC 9112 Section 7.1)

```cpp
// Format a single HTTP chunk: "{hex_size}\r\n{data}\r\n"
// Terminating chunk: "0\r\n\r\n"
std::string format_chunk_header(size_t size) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%zx\r\n", size);
    return std::string(buf, static_cast<size_t>(n));
}

static constexpr std::string_view CHUNK_TERMINATOR = "0\r\n\r\n";
static constexpr std::string_view CHUNK_SUFFIX = "\r\n";
```

### Asio Scatter-Gather Write (Buffer Sequence)

```cpp
// Write multiple buffers in a single async_write call.
// Asio handles partial writes automatically.
std::string chunk_header = format_chunk_header(chunk_data.size());
std::array<asio::const_buffer, 3> bufs = {
    asio::buffer(chunk_header),
    asio::buffer(chunk_data.data(), chunk_data.size()),
    asio::buffer(CHUNK_SUFFIX.data(), CHUNK_SUFFIX.size())
};
auto [ec, n] = co_await asio::async_write(stream, bufs, use_nothrow);
```

### UDS Chunked Send Pattern (Relay Side)

```cpp
// In UdsMultiplexer: send a large message as chunked sub-frames.
// Each sub-frame goes through AEAD with the shared nonce counter.
asio::awaitable<bool> UdsMultiplexer::send_chunked(
    uint8_t type, uint32_t request_id,
    std::span<const uint8_t> payload) {

    // 1. Send chunked header sub-frame
    std::vector<uint8_t> header;
    header.push_back(0x01);  // CHUNKED_BEGIN flag
    header.push_back(type);
    // ... request_id (4B BE), total_size (8B BE)
    if (!co_await send_encrypted(header)) co_return false;

    // 2. Send data sub-frames (1 MiB each)
    constexpr size_t CHUNK_SIZE = 1048576;
    size_t offset = 0;
    while (offset < payload.size()) {
        size_t chunk_len = std::min(CHUNK_SIZE, payload.size() - offset);
        auto chunk = payload.subspan(offset, chunk_len);
        if (!co_await send_encrypted(chunk)) co_return false;
        offset += chunk_len;
    }

    // 3. Send end sentinel (zero-length frame)
    if (!co_await send_encrypted(std::span<const uint8_t>{})) co_return false;

    co_return true;
}
```

### Backpressure Queue Pattern

```cpp
// Producer-consumer with asio::steady_timer as signal.
// Same pattern as SseWriter and drain_send_queue.
struct ChunkQueue {
    std::deque<std::vector<uint8_t>> chunks;
    asio::steady_timer signal;
    bool closed = false;
    static constexpr size_t MAX_DEPTH = 4;

    // Producer: push chunk, wait if full
    asio::awaitable<bool> push(std::vector<uint8_t> chunk) {
        while (chunks.size() >= MAX_DEPTH && !closed) {
            signal.expires_at(asio::steady_timer::time_point::max());
            auto [ec] = co_await signal.async_wait(use_nothrow);
            if (closed) co_return false;
        }
        if (closed) co_return false;
        chunks.push_back(std::move(chunk));
        signal.cancel();  // Wake consumer
        co_return true;
    }

    // Consumer: pop chunk, wait if empty
    asio::awaitable<std::optional<std::vector<uint8_t>>> pop() {
        while (chunks.empty() && !closed) {
            signal.expires_at(asio::steady_timer::time_point::max());
            auto [ec] = co_await signal.async_wait(use_nothrow);
        }
        if (chunks.empty()) co_return std::nullopt;
        auto chunk = std::move(chunks.front());
        chunks.pop_front();
        signal.cancel();  // Wake producer if it was waiting
        co_return chunk;
    }
};
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Full-blob buffer | Chunked streaming | This phase | 500 MiB blobs without OOM |
| TransportCodec for all payloads | Bypass for large blob chunks | This phase | Eliminates FlatBuffer O(n) copy |
| HttpResponse::serialize() string concat | Scatter-gather / chunked-TE | This phase | Eliminates header+body copy |
| MAX_BLOB_DATA_SIZE 100 MiB | 500 MiB | This phase | 5x larger blob support |

## Open Questions

1. **Node-side chunked receive integration**
   - What we know: The node's `Connection::message_loop()` calls `recv_encrypted() -> TransportCodec::decode()` in a tight loop. Chunked mode requires the message_loop to detect the chunked header flag and accumulate sub-frames into a complete message before dispatching to `message_cb_`.
   - What's unclear: Whether the node should reassemble the full blob before dispatching (simple but defeats streaming for the node's storage), or stream chunks directly to storage.
   - Recommendation: For pre-MVP, have the node reassemble chunks into a single payload before dispatching. The node already handles 100 MiB blobs in memory -- 500 MiB is larger but the node is the storage layer, not a proxy. True node-side streaming can come later. The relay is the bottleneck proxy that MUST stream.

2. **Exact chunked header format details**
   - What we know: Need flags byte + type + request_id + total_size at minimum.
   - What's unclear: Whether metadata beyond the TransportMessage envelope (e.g., FlatBuffer-encoded wrapper) needs to be in the header.
   - Recommendation: The chunked header replaces TransportCodec for the chunked path. It carries only: flags (1B), type (1B), request_id (4B BE), total_payload_size (8B BE) = 14 bytes. The chunk data sub-frames carry raw payload bytes. On reassembly, the receiver constructs the equivalent of a DecodedMessage struct.

3. **ReadResponse status byte timing**
   - What we know: ReadResponse has a status byte prefix. The relay needs it before starting HTTP chunked-TE.
   - What's unclear: Should the status byte be in the chunked header, or in the first data sub-frame?
   - Recommendation: Include the status byte as part of the chunked header metadata (1 extra byte). This allows the relay to check status and send 404 before committing to 200 + chunked-TE.

## Environment Availability

Step 2.6: SKIPPED (no external dependencies identified -- this phase modifies only C++ source code and CMake builds within the existing project).

## Sources

### Primary (HIGH confidence)
- Source code analysis: relay/core/uds_multiplexer.cpp, relay/http/http_connection.cpp, relay/http/handlers_data.cpp, relay/wire/transport_codec.cpp, relay/http/http_response.h, relay/http/response_promise.h, db/net/connection.cpp, db/net/framing.h/cpp, relay/wire/aead.h
- Phase 999.8 CONTEXT.md: database chunking decisions (1 MiB chunks, CHNK manifests)
- Phase 999.9 CONTEXT.md: HTTP transport decisions (raw binary blobs, SSE notifications)
- RFC 9112 Section 7.1: HTTP/1.1 chunked transfer encoding specification

### Secondary (MEDIUM confidence)
- [Asio buffer sequence documentation](http://think-async.com/Asio/asio-1.11.0/doc/asio/overview/core/buffers.html) -- scatter-gather concepts
- [MDN Transfer-Encoding reference](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Transfer-Encoding) -- chunked encoding format
- [Asio scatter-gather issue #203](https://github.com/chriskohlhoff/asio/issues/203) -- performance considerations for many buffers (not applicable at 3 buffers per chunk)

### Tertiary (LOW confidence)
- None. All recommendations are grounded in source code analysis and protocol specifications.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, all built on existing Asio + AEAD primitives
- Architecture: HIGH -- chunked framing protocol is straightforward, patterns match existing codebase (SseWriter, drain_send_queue)
- Pitfalls: HIGH -- all pitfalls identified from source code analysis of existing buffering points and framing invariants

**Research date:** 2026-04-14
**Valid until:** 2026-05-14 (stable -- internal protocol, no external dependency changes)
