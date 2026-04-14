# Phase 115: Chunked Streaming for Large Blobs - Context

**Gathered:** 2026-04-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Eliminate full-blob buffering in the relay by implementing chunked streaming I/O for large blobs. Both upload (HTTP→UDS) and download (UDS→HTTP) paths stream in 1 MiB chunks. Blobs under 1 MiB continue to use the existing full-buffer path. Raise MAX_BLOB_DATA_SIZE from 100 MiB to 500 MiB in both node and relay. Refactor HttpResponse to use scatter-gather writes for all responses. Changes touch both relay and node (UDS framing, AEAD per-chunk).

</domain>

<decisions>
## Implementation Decisions

### Streaming Scope
- **D-01:** Both directions streamed: upload (HTTP→UDS) and download (UDS→HTTP). All full-blob buffering eliminated for blobs >= 1 MiB.
- **D-02:** Streaming threshold: 1 MiB (1,048,576 bytes). Matches Phase 999.8's database chunk size. Blobs under 1 MiB handled inline with existing full-buffer path.
- **D-03:** MAX_BLOB_DATA_SIZE raised from 100 MiB to 500 MiB in both node (`db/net/framing.h`) and relay (`relay/http/http_connection.cpp` MAX_BODY_SIZE, `relay/core/uds_multiplexer.h` MAX_FRAME_SIZE). Pre-MVP — both sides can change freely.
- **D-04:** Bounded backpressure: fixed-size chunk queue between producer and consumer. Producer pauses when queue is full. Prevents OOM from fast producer / slow consumer.

### UDS Wire Framing
- **D-05:** Chunked sub-frame mode for UDS. Large payloads broken into 1 MiB sub-frames within a single logical message. Small messages (< 1 MiB) use existing single-frame format.
- **D-06:** 1 MiB UDS sub-frame chunk size. One UDS sub-frame per database chunk. Consistent across the stack.
- **D-07:** Both node and relay are modified for chunked UDS framing. Pre-MVP — no backward compatibility constraints.

### AEAD Chunking
- **D-08:** Per-chunk AEAD authentication. Each 1 MiB sub-frame gets its own ChaCha20-Poly1305 encrypt/decrypt with sequential nonces. Each chunk independently authenticated — corrupt chunk identified immediately.
- **D-09:** Shared monotonic nonce counter for all frames (both chunked and non-chunked). Each chunk consumes one nonce from the same counter. Simple, no nonce reuse risk.

### HTTP Transport
- **D-10:** HTTP chunked transfer encoding for blob responses (downloads). Relay streams chunks as they arrive from UDS. No Content-Length needed upfront for blob responses.
- **D-11:** Upload streaming via Content-Length body chunking. Client sends Content-Length header. Relay reads body in 1 MiB chunks from socket and forwards each to UDS incrementally. No client-side chunked encoding required.
- **D-12:** HttpResponse::serialize() refactored for ALL responses to use scatter-gather writes (Asio buffer sequences). Eliminates the header+body concatenation into single std::string.

### Claude's Discretion
- UDS chunked framing signaling mechanism (flags byte, capability negotiation, or sentinel — pre-MVP, pick simplest)
- Exact bounded queue size for backpressure (e.g., 4-8 chunks in flight)
- Whether to add a streaming FlatBuffer encode/decode path or bypass FlatBuffer for the blob data portion of large messages
- Error handling for partial chunk delivery (connection drop mid-stream)
- Whether TransportCodec needs changes or can be bypassed for raw blob data
- Thread pool interaction: whether per-chunk AEAD should use offload_if_large or always run inline (1 MiB chunks are above the 64 KiB threshold)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Relay HTTP layer (streaming target)
- `relay/http/http_connection.cpp` — Body reading (read_body()), response serialization, MAX_BODY_SIZE
- `relay/http/http_response.h` — HttpResponse::serialize(), binary() — full-buffer bottleneck
- `relay/http/handlers_data.cpp` — Blob write/read/delete/batch-read handlers
- `relay/http/handlers_data.h` — Handler declarations, max_blob_size wiring

### Relay UDS layer (chunked framing target)
- `relay/core/uds_multiplexer.h` — MAX_FRAME_SIZE, send(), send_queue_
- `relay/core/uds_multiplexer.cpp` — send_encrypted/recv_encrypted, drain_send_queue, recv_raw, read_loop
- `relay/wire/transport_codec.cpp` — FlatBuffer encode/decode (copies entire payload)
- `relay/http/response_promise.h` — ResponsePromise/ResponsePromiseMap (whole-payload delivery)

### Relay offload infrastructure
- `relay/util/offload_if_large.h` — offload_if_large() + OFFLOAD_THRESHOLD (64 KiB)
- `relay/util/thread_pool.h` — offload() primitive

### Node framing (must change)
- `db/net/framing.h` — MAX_BLOB_DATA_SIZE (100 MiB → 500 MiB), MAX_FRAME_SIZE
- `db/wire/codec.h` — BlobData struct, encode_blob()

### Node AEAD (per-chunk model)
- `relay/wire/aead.h` — AEAD encrypt/decrypt, nonce counter

### Prior phase context
- `.planning/phases/999.8-database-layer-chunking-for-large-files/999.8-CONTEXT.md` — Database-layer chunking decisions (1 MiB chunks, CHNK manifests)
- `.planning/phases/999.9-http-transport-for-relay-data-operations/999.9-CONTEXT.md` — HTTP transport decisions (raw binary blobs, SSE notifications)

### Config
- `relay/config/relay_config.h` — max_blob_size_bytes, request_timeout_seconds

</canonical_refs>

<code_context>
## Existing Code Insights

### Full-Blob Buffering Points (what must change)
1. **HttpConnection::read_body()** — accumulates entire HTTP body in vector<uint8_t> before handler
2. **TransportCodec::encode()** — copies entire payload into FlatBuffer builder
3. **UdsMultiplexer::recv_raw()** — allocates vector<uint8_t>(length) for entire frame
4. **HttpResponse::serialize()** — concatenates headers + body into single std::string
5. **ResponsePromise** — delivers complete payload as single vector<uint8_t>

### Write path copies (current): body→TransportCodec→send_queue→AEAD ciphertext = 3-4 full copies
### Read path copies (current): recv_raw→AEAD decrypt→TransportCodec decode→ResponsePromise→handler slice→HttpResponse→serialize = 5-6 full copies

### Reusable Assets
- `offload_if_large()` — per-chunk AEAD can reuse this (1 MiB > 64 KiB threshold)
- `drain_send_queue()` coroutine — serializes outbound UDS writes, can be extended for chunked send
- Asio scatter-gather: `asio::buffer_sequence` for multi-buffer async_write already available

### Integration Points
- `relay_main.cpp` — wires max_blob_size into DataHandlers
- `handlers_data.cpp` — write/read handlers need streaming variants
- Node's framing.h and connection code — must support chunked sub-frames on UDS

</code_context>

<specifics>
## Specific Ideas

- The streaming threshold (1 MiB) and UDS chunk size (1 MiB) deliberately match the database chunk size from Phase 999.8, creating consistency across the entire stack
- Per-chunk AEAD with shared nonce counter means a 500 MiB blob consumes ~500 nonces — well within ChaCha20-Poly1305's 2^32 nonce space per session
- Backpressure queue prevents OOM: a slow HTTP client receiving a 500 MiB blob won't cause the relay to buffer all 500 MiB from UDS
- Small blobs (< 1 MiB) are untouched — the existing path stays fast for the common case

</specifics>

<deferred>
## Deferred Ideas

- Client-side chunked transfer encoding for uploads (Transfer-Encoding: chunked) — not needed pre-MVP, Content-Length streaming sufficient
- HTTP/2 streaming with multiplexed frames — future optimization
- Zero-copy splice/sendfile between UDS and TCP sockets — OS-level optimization, complex
- Streaming FlatBuffer parsing — may not be needed if raw blob data bypasses FlatBuffer

</deferred>

---

*Phase: 115-chunked-streaming-for-large-blobs*
*Context gathered: 2026-04-14*
