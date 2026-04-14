# Phase 111: Single-Threaded Rewrite - Context

**Gathered:** 2026-04-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Convert the relay from multi-threaded io_context (hardware_concurrency threads all running ioc.run()) to a single event loop thread with a thread pool for CPU-heavy offload. Remove all strand and mutex code from the failed Phase 999.10 attempt. All shared state accessed from the single event loop thread without synchronization. Existing relay unit tests must compile and pass under the new model.

</domain>

<decisions>
## Implementation Decisions

### Concurrency Model
- **D-01:** Single io_context thread for all I/O (HTTP accept, connections, UDS, SSE, timers). relay_main.cpp creates one thread calling ioc.run().
- **D-02:** Thread pool created at startup with `hardware_concurrency()` threads. No config field -- hardcoded. `resolve_worker_threads()` not needed.
- **D-03:** Only ML-DSA-87 signature verification offloads to the thread pool via `crypto::offload()`. TLS handshakes and JSON parse/serialize stay inline on the event loop.
- **D-04:** TLS handshakes run inline on the event loop via Asio's ssl::stream. No custom SSL BIO or socket transfer. ~1-2ms per handshake is acceptable at relay scale.
- **D-05:** All JSON parse/serialize stays inline on the event loop. nlohmann/json is fast enough for all relay payloads (sub-millisecond even for large ListResponse/BatchReadResponse).

### Offload Infrastructure
- **D-06:** Copy node's `db/crypto/thread_pool.h` to `relay/util/thread_pool.h` under `chromatindb::relay::util` namespace. Relay stays self-contained with no cross-layer build dependency. ~45 lines.

### Strand/Mutex Removal
- **D-07:** Remove all `asio::strand` and `asio::make_strand` from relay production code and tests. Remove `Strand` type aliases from http_router, handlers_query, handlers_data, handlers_pubsub, uds_multiplexer.
- **D-08:** Remove `tls_mutex_` from http_server.h/cpp and `acl_mutex_` from authenticator.h/cpp. Single-threaded event loop means no concurrent access.
- **D-09:** Remove `set_strand()` and strand pointer members from HttpRouter and handler classes. Functions that posted to strand now execute directly.

### SIGHUP Settings
- **D-10:** Convert `std::atomic<uint32_t>` for rate_limit_rate, request_timeout, max_blob_size to plain `uint32_t`. SIGHUP handler runs via async_signal_set coroutine on the same event loop thread -- no races possible. Documents the single-threaded invariant.

### Test Adaptation
- **D-11:** Tests go single-threaded to match production. Remove strand parameters from test helpers (`run_async_dispatch`, `register_auth_routes`). Tests use plain `asio::io_context` with `ioc.run()`.

### Claude's Discretion
- Order of refactoring within the phase (e.g., strands first vs thread pool first)
- Whether to introduce relay-specific `offload()` wrapper or use the copied template directly
- How to handle the transfer-back pattern after offload (explicit `co_await asio::post(ioc)` vs wrapper)
- Plan decomposition -- how many plans to split this into

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Concurrency model
- `.planning/REQUIREMENTS.md` -- CONC-01 through CONC-05, VER-01 define the target model
- `.planning/phases/999.10-relay-thread-safety-overhaul-for-multi-threaded-http/999.10-CONTEXT.md` -- Why strands failed, root cause analysis

### Node offload pattern (reference implementation)
- `db/crypto/thread_pool.h` -- `resolve_worker_threads()` and `offload()` template to copy to relay
- `db/engine/engine.cpp` lines 175, 231, 250, 382, 391 -- offload() usage examples with transfer-back pattern

### Relay code to modify
- `relay/relay_main.cpp` -- Main entry point: strand creation (line 215), thread pool (lines 528-543), atomics (lines 221-223)
- `relay/http/http_router.h/cpp` -- Strand type alias and set_strand()
- `relay/http/handlers_query.h/cpp` -- Strand type alias
- `relay/http/handlers_data.h/cpp` -- Strand type alias
- `relay/http/handlers_pubsub.h/cpp` -- Strand type alias
- `relay/core/uds_multiplexer.h/cpp` -- Strand type alias
- `relay/http/http_server.h/cpp` -- tls_mutex_
- `relay/core/authenticator.h/cpp` -- acl_mutex_, ML-DSA-87 verify (offload candidate)
- `relay/tests/test_http_router.cpp` -- Strand usage in test helpers

### Transport layer (unchanged, for reference)
- `.planning/phases/999.9-http-transport-for-relay-data-operations/999.9-CONTEXT.md` -- HTTP+SSE transport decisions (all kept)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/crypto/thread_pool.h`: Proven `offload()` template (45 lines). Posts work to asio::thread_pool, returns result via co_await. Copy to relay/util/.
- `db/crypto/verify_helpers.h`: `verify_with_offload()` coroutine -- shows pattern for offloading ML-DSA-87 verify and transferring back.

### Established Patterns
- Node's PeerManager: single-threaded event loop + thread pool offload. Proven ASAN/TSAN clean. This is the target pattern.
- `crypto::offload()` resumes on the thread pool thread. Callers MUST `co_await asio::post(executor, asio::use_awaitable)` to transfer back to event loop before touching shared state.
- relay_main.cpp currently: 1 io_context, 1 strand, hardware_concurrency threads calling ioc.run(). Target: 1 io_context, 0 strands, 1 thread calling ioc.run() + separate asio::thread_pool.

### Integration Points
- `relay/core/authenticator.cpp` line 65: `OQS_SIG_verify` is the only CPU-heavy operation worth offloading. Currently protected by acl_mutex_ -- will be refactored to offload() instead.
- All strand.dispatch/post/defer calls in HTTP handlers become direct calls (they're already on the event loop thread).

</code_context>

<specifics>
## Specific Ideas

- The relay's offload scope is intentionally minimal: only ML-DSA-87 verify. TLS and JSON stay inline. This keeps the codebase simple and avoids unnecessary complexity.
- Thread pool is fire-and-forget at startup: `hardware_concurrency()` threads, no runtime resizing, no config field.
- The rewrite is mechanical: remove strands/mutexes, change thread pool from "N threads running ioc" to "1 thread running ioc + separate offload pool", add offload() at the one crypto call site.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 111-single-threaded-rewrite*
*Context gathered: 2026-04-14*
