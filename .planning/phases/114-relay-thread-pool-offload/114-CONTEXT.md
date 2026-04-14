# Phase 114: Relay Thread Pool Offload - Context

**Gathered:** 2026-04-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix event loop starvation by offloading CPU-heavy work to the existing thread pool. The event loop stays single-threaded for I/O coordination. One client's large blob operation must not block other clients. The offload infrastructure (`relay/util/thread_pool.h`) and single-threaded model already exist from Phase 111.

</domain>

<decisions>
## Implementation Decisions

### Offload Scope
- **D-01:** Offload the full translation pipeline: `json_to_binary()` and `binary_to_json()` calls. This covers JSON parse/serialize, base64 encode/decode, FlatBuffer build, and hex decode in one offload boundary. Reverses Phase 111 D-03/D-05 which kept these inline.
- **D-02:** Offload UDS AEAD encrypt/decrypt (`send_encrypted`, `recv_encrypted` in uds_multiplexer.cpp). ChaCha20-Poly1305 is linear in payload size and stalls the event loop for large blobs.
- **D-03:** ML-DSA-87 verify offload (Phase 111 D-03) is already implemented in http_router.cpp and stays as-is.

### Offload Granularity
- **D-04:** Each `json_to_binary()`, `binary_to_json()`, `aead_encrypt()`, and `aead_decrypt()` call gets its own `co_await offload(pool, ...)`. Pattern: offload -> transfer back to event loop -> continue. No wrapping of entire request handlers.
- **D-05:** The pattern applies identically in HTTP handlers (handlers_query.cpp, handlers_data.cpp) and the UDS notification path (uds_multiplexer.cpp).

### Size Threshold
- **D-06:** Only offload when input payload exceeds 64 KB. Small requests (sub-KB queries, stats, exists) stay inline on the event loop to avoid unnecessary thread hop overhead (~5-10us per hop).
- **D-07:** Same 64 KB threshold applies to both translation calls and AEAD calls. Consistent behavior across all offload sites.
- **D-08:** Threshold check uses the raw input size: HTTP body size for `json_to_binary()`, binary payload size for `binary_to_json()`, plaintext size for `aead_encrypt()`, ciphertext size for `aead_decrypt()`.

### UDS AEAD Safety
- **D-09:** AEAD offload is safe because UDS send and recv are already serialized: send_encrypted is called from the send queue drain coroutine (one at a time), recv_encrypted is called from the single read_loop coroutine. No concurrent AEAD operations on the same direction, so sequential nonce counters remain correct.
- **D-10:** Counter increment (`send_counter_++`, `recv_counter_++`) happens on the event loop thread BEFORE the offload call. The offloaded lambda captures the counter value by copy. This preserves ordering.

### Claude's Discretion
- Whether to create a helper wrapper (e.g., `offload_if_large(pool, threshold, size, fn)`) or use inline if/else at each call site
- How to thread the `asio::thread_pool&` reference to all call sites that need it (constructor injection, global, or passed through handlers)
- Plan decomposition: how many plans to split this into
- Test strategy: unit tests for threshold behavior, integration test for large payload non-blocking

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Concurrency model (Phase 111 established)
- `.planning/phases/111-single-threaded-rewrite/111-CONTEXT.md` -- Single-threaded event loop + thread pool model. D-01 through D-11. Phase 114 extends D-03/D-05.

### Offload infrastructure
- `relay/util/thread_pool.h` -- `offload()` template. Posts work to asio::thread_pool, resumes on pool thread. Callers MUST `co_await asio::post(ioc, asio::use_awaitable)` to transfer back before touching shared state.
- `relay/http/http_router.cpp` lines 259-310 -- Existing ML-DSA-87 offload pattern (reference implementation for how to use offload + transfer back).

### HTTP handler hot paths (offload sites)
- `relay/http/handlers_query.cpp` lines 117-174 -- `forward_query()`: json_to_binary (line 127), binary_to_json (line 166). Main query pipeline.
- `relay/http/handlers_data.cpp` -- Data write (line 111+), read (line 208+), delete (line 241+), batch read (line 313+). All call json_to_binary/binary_to_json.

### UDS multiplexer (AEAD offload sites)
- `relay/core/uds_multiplexer.cpp` lines 440-455 -- `send_encrypted()` and `recv_encrypted()`. AEAD encrypt/decrypt inline.
- `relay/core/uds_multiplexer.cpp` lines 461-525 -- `read_loop()`: calls recv_encrypted, then binary_to_json for notification fan-out (lines 523, 562, 603).

### Translation layer
- `relay/translate/translator.cpp` -- `json_to_binary()` (line 231) and `binary_to_json()` (line 824). These are the functions being offloaded.
- `relay/util/base64.cpp` -- `base64_encode()` / `base64_decode()` called within translator. Heavy for large blobs.
- `relay/wire/blob_codec.cpp` -- FlatBuffer build/decode called within translator for Data/Delete/ReadResponse types.

### AEAD implementation
- `relay/wire/aead.cpp` -- `aead_encrypt()` (line 45), `aead_decrypt()` (line 82). ChaCha20-Poly1305 via OpenSSL EVP.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/util/thread_pool.h`: `offload()` template already exists and is proven. Used by ML-DSA-87 verify in http_router.cpp.
- Transfer-back pattern established: `co_await asio::post(ioc, asio::use_awaitable)` after offload.

### Established Patterns
- Single event loop thread: all shared state (session map, request router, subscription tracker, UDS multiplexer) accessed without synchronization from the event loop thread.
- `offload()` resumes on the pool thread. Must transfer back before touching any shared state.
- `send_encrypted`/`recv_encrypted` are already coroutines with `asio::awaitable` return type -- adding `co_await offload()` inside them is mechanical.

### Integration Points
- `forward_query()` in handlers_query.cpp is the main query pipeline -- needs offload wrapping around json_to_binary and binary_to_json.
- handlers_data.cpp has 4 handler functions (write, read, delete, batch_read) each calling translate functions -- all need the same treatment.
- uds_multiplexer.cpp notification fan-out path calls binary_to_json at 3 sites (lines 523, 562, 603) -- all need conditional offload.
- `asio::thread_pool&` reference needs to reach all handler classes and UdsMultiplexer. Currently only http_router.cpp has it.

</code_context>

<specifics>
## Specific Ideas

- The 64 KB threshold means the common case (small queries, stats, exists, list) stays fast with zero overhead. Only blob-carrying operations (Data, Delete, Read, BatchRead) with large payloads hit the offload path.
- Counter increment for AEAD must happen BEFORE offload to maintain ordering. The offloaded lambda receives the counter value, not a reference to the counter.
- The offload pattern is identical everywhere: `if (size > 64KB) { result = co_await offload(pool, [&]{ return fn(...); }); co_await asio::post(ioc, use_awaitable); } else { result = fn(...); }`. A helper wrapper could deduplicate this.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 114-relay-thread-pool-offload*
*Context gathered: 2026-04-14*
