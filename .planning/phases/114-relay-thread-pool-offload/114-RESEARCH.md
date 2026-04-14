# Phase 114: Relay Thread Pool Offload - Research

**Researched:** 2026-04-14
**Domain:** Asio C++20 coroutine offload pattern, event loop starvation mitigation
**Confidence:** HIGH

## Summary

Phase 114 extends the single-threaded event loop model established in Phase 111 by offloading CPU-heavy operations (JSON translation, base64, FlatBuffer build, AEAD encrypt/decrypt) to the existing `asio::thread_pool`. The Phase 113 benchmark proved the bottleneck: mixed workload p99 latency degrades +5250% because a single 50 MiB blob operation blocks the event loop for hundreds of milliseconds, starving all other clients.

The offload infrastructure already exists (`relay/util/thread_pool.h` with the `offload()` template) and is proven in production for ML-DSA-87 verify. The work is mechanical: wrap `json_to_binary()`, `binary_to_json()`, `aead_encrypt()`, and `aead_decrypt()` calls in conditional offload (64 KB threshold), then transfer back to the event loop before touching shared state. All offload sites are coroutine contexts with `asio::awaitable` return types, so adding `co_await offload(...)` is syntactically straightforward.

**Primary recommendation:** Create an `offload_if_large()` helper that encapsulates the threshold check + offload + transfer-back pattern, then apply it mechanically at all call sites. Thread the `asio::thread_pool&` reference via constructor injection to `DataHandlers`, `QueryHandlerDeps`, and `UdsMultiplexer`.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Offload the full translation pipeline: `json_to_binary()` and `binary_to_json()` calls. This covers JSON parse/serialize, base64 encode/decode, FlatBuffer build, and hex decode in one offload boundary. Reverses Phase 111 D-03/D-05 which kept these inline.
- **D-02:** Offload UDS AEAD encrypt/decrypt (`send_encrypted`, `recv_encrypted` in uds_multiplexer.cpp). ChaCha20-Poly1305 is linear in payload size and stalls the event loop for large blobs.
- **D-03:** ML-DSA-87 verify offload (Phase 111 D-03) is already implemented in http_router.cpp and stays as-is.
- **D-04:** Each `json_to_binary()`, `binary_to_json()`, `aead_encrypt()`, and `aead_decrypt()` call gets its own `co_await offload(pool, ...)`. Pattern: offload -> transfer back to event loop -> continue. No wrapping of entire request handlers.
- **D-05:** The pattern applies identically in HTTP handlers (handlers_query.cpp, handlers_data.cpp) and the UDS notification path (uds_multiplexer.cpp).
- **D-06:** Only offload when input payload exceeds 64 KB. Small requests stay inline to avoid thread hop overhead (~5-10us per hop).
- **D-07:** Same 64 KB threshold applies to both translation calls and AEAD calls. Consistent behavior across all offload sites.
- **D-08:** Threshold check uses the raw input size: HTTP body size for `json_to_binary()`, binary payload size for `binary_to_json()`, plaintext size for `aead_encrypt()`, ciphertext size for `aead_decrypt()`.
- **D-09:** AEAD offload is safe because UDS send and recv are already serialized: send_encrypted is called from the send queue drain coroutine (one at a time), recv_encrypted is called from the single read_loop coroutine.
- **D-10:** Counter increment (`send_counter_++`, `recv_counter_++`) happens on the event loop thread BEFORE the offload call. The offloaded lambda captures the counter value by copy. This preserves ordering.

### Claude's Discretion
- Whether to create a helper wrapper (e.g., `offload_if_large(pool, threshold, size, fn)`) or use inline if/else at each call site
- How to thread the `asio::thread_pool&` reference to all call sites that need it (constructor injection, global, or passed through handlers)
- Plan decomposition: how many plans to split this into
- Test strategy: unit tests for threshold behavior, integration test for large payload non-blocking

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest (FetchContent) | `asio::thread_pool`, `asio::post`, `asio::use_awaitable`, `co_await` | Already in use; `offload()` template built on it |
| OpenSSL 3.3+ | system | `EVP_chacha20_poly1305()` for AEAD encrypt/decrypt | Already in use for all relay crypto |
| nlohmann/json | latest (FetchContent) | JSON parse/serialize in translator | Already in use; calls being offloaded |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| Catch2 | v3.7.1 (FetchContent) | Unit tests for threshold logic, offload behavior | All new tests |
| spdlog | latest (FetchContent) | Debug logging for offload path selection | Existing usage |

No new dependencies are needed. All work uses the existing stack.

## Architecture Patterns

### Current Offload Infrastructure

The `relay/util/thread_pool.h` `offload()` template is the building block:

```cpp
// Source: relay/util/thread_pool.h (existing, verified)
template <typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload(asio::thread_pool& pool, F&& fn) {
    using R = std::invoke_result_t<F>;
    co_return co_await async_initiate<decltype(asio::use_awaitable),
                                      void(std::error_code, R)>(
        [&pool](auto handler, auto f) {
            asio::post(pool, [h = std::move(handler), f = std::move(f)]() mutable {
                auto result = f();
                std::move(h)(std::error_code{}, std::move(result));
            });
        },
        asio::use_awaitable, std::forward<F>(fn));
}
```

### Pattern: Proven ML-DSA-87 Offload (Reference)

```cpp
// Source: relay/http/http_router.cpp lines 306-310 (existing, verified)
auto result = co_await relay::util::offload(pool, [&]() {
    return authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);
});
// CRITICAL: Transfer back to event loop before accessing shared state.
co_await asio::post(ioc, asio::use_awaitable);
```

### Recommended Pattern: Conditional Offload Helper

Create `relay/util/offload_if_large.h` to deduplicate the threshold + offload + transfer-back logic:

```cpp
// Recommended new helper
template <typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload_if_large(
    asio::thread_pool& pool,
    asio::io_context& ioc,
    size_t payload_size,
    size_t threshold,
    F&& fn) {
    using R = std::invoke_result_t<F>;
    if (payload_size > threshold) {
        auto result = co_await offload(pool, std::forward<F>(fn));
        co_await asio::post(ioc, asio::use_awaitable);
        co_return result;
    }
    co_return fn();
}
```

This encapsulates D-04 through D-08 in a single call. Each call site becomes:

```cpp
auto result = co_await util::offload_if_large(pool_, ioc_, body.size(),
    OFFLOAD_THRESHOLD, [&] { return translate::json_to_binary(query_json); });
```

### Offload Site Inventory

All call sites requiring modification, with their current locations:

**HTTP Translation (handlers_query.cpp):**
1. `forward_query()` line 127: `json_to_binary(query_json)` -- threshold on input JSON size
2. `forward_query()` line 166: `binary_to_json(response->type, payload)` -- threshold on payload size

**HTTP Translation (handlers_data.cpp):**
3. `handle_blob_write()` line 155: `binary_to_json(result->type, payload)` -- WriteAck (41 bytes, always inline)
4. `handle_batch_read()` line 324: `json_to_binary(translate_input)` -- threshold on JSON body size
5. `handle_batch_read()` line 354: `binary_to_json(result->type, payload)` -- threshold on response payload size
6. `handle_blob_delete()` line 279: `binary_to_json(result->type, payload)` -- DeleteAck (41 bytes, always inline)

**UDS Multiplexer (uds_multiplexer.cpp):**
7. `send_encrypted()` line 441: `aead_encrypt(plaintext, send_key_, send_counter_++)` -- threshold on plaintext size
8. `recv_encrypted()` line 449: `aead_decrypt(*raw, recv_key_, recv_counter_++)` -- threshold on ciphertext size
9. `route_response()` line 523: `binary_to_json(type, payload)` -- StorageFull/QuotaExceeded broadcast
10. `route_response()` line 562: `binary_to_json(type, payload)` -- WS path (legacy) translation
11. `handle_notification()` line 603: `binary_to_json(type, payload)` -- Notification fan-out

**Key observation:** Sites 9-11 are in non-coroutine functions (`route_response` and `handle_notification` are plain `void` functions, not `asio::awaitable`). They cannot use `co_await` directly. These need refactoring to become coroutines, or the offload must be done at a higher level (e.g., in `read_loop()` after `recv_encrypted()` returns the decoded payload but before `route_response()` is called).

### Dependency Injection Strategy

The `asio::thread_pool&` reference currently only reaches `http_router.cpp` (for ML-DSA-87 offload). It must be threaded to:

1. **DataHandlers** -- add `asio::thread_pool& pool_` member, extend constructor
2. **QueryHandlerDeps** -- add `asio::thread_pool* pool` field
3. **UdsMultiplexer** -- add `asio::thread_pool& pool_` member, extend constructor
4. **relay_main.cpp** -- pass `offload_pool` to all three during construction

All other components (PubSubHandlers, etc.) do not call translation or AEAD functions, so they need no changes.

### UDS AEAD Offload Detail

Per D-09 and D-10, the counter increment must happen BEFORE offload:

```cpp
// send_encrypted: SAFE because drain_send_queue serializes sends
asio::awaitable<bool> UdsMultiplexer::send_encrypted(std::span<const uint8_t> plaintext) {
    auto counter = send_counter_++;  // Increment on event loop BEFORE offload
    auto ct = co_await util::offload_if_large(pool_, ioc_, plaintext.size(),
        OFFLOAD_THRESHOLD, [&, counter] {
            return wire::aead_encrypt(plaintext, send_key_, counter);
        });
    co_return co_await send_raw(ct);
}

// recv_encrypted: SAFE because read_loop is the only caller
asio::awaitable<std::optional<std::vector<uint8_t>>> UdsMultiplexer::recv_encrypted() {
    auto raw = co_await recv_raw();
    if (!raw) co_return std::nullopt;
    
    auto counter = recv_counter_++;  // Increment on event loop BEFORE offload
    auto pt = co_await util::offload_if_large(pool_, ioc_, raw->size(),
        OFFLOAD_THRESHOLD, [&, counter]() -> std::optional<std::vector<uint8_t>> {
            return wire::aead_decrypt(*raw, recv_key_, counter);
        });
    if (!pt) {
        spdlog::error("UDS AEAD decrypt failed at recv_counter={}", counter);
        co_return std::nullopt;
    }
    co_return pt;
}
```

### Non-Coroutine Translation Sites (Critical Design Decision)

`route_response()` and `handle_notification()` are synchronous `void` methods called from `read_loop()` (which IS a coroutine). Three options:

**Option A (Recommended):** Move translation into `read_loop()` before calling `route_response()`. The read_loop already has the decoded type and payload. Add conditional offload of `binary_to_json()` in the read_loop for notification/broadcast paths, then pass the translated JSON to `route_response()` / `handle_notification()`.

**Option B:** Convert `route_response()` and `handle_notification()` to coroutines. This is a larger refactor and changes their call interface.

**Option C:** Use `std::async` or `std::future` inside the synchronous functions. This violates the established offload pattern and introduces complexity.

Option A is cleanest: it keeps route_response/handle_notification synchronous, avoids changing public interfaces, and concentrates the offload logic in the single read_loop coroutine where it belongs.

### Anti-Patterns to Avoid
- **Offloading entire request handlers:** D-04 explicitly says per-call offload, not whole-handler wrapping. This preserves the ability to access shared state between offloaded operations.
- **Capturing mutable references across co_await:** After `co_await offload(...)`, the coroutine resumes on the pool thread. All shared state access must happen AFTER `co_await asio::post(ioc, use_awaitable)` transfers back.
- **Forgetting transfer-back:** Every `co_await offload(pool, ...)` MUST be followed by `co_await asio::post(ioc, asio::use_awaitable)` before any shared state access. The `offload_if_large()` helper encapsulates this.
- **Referencing counters by reference in offloaded lambda:** AEAD counters must be captured by value (copy before offload), not by reference. Reference capture would read the counter on the pool thread after another operation might have incremented it.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Thread pool | Custom thread pool | `asio::thread_pool` | Already in use, integrates with `asio::post` and `co_await` |
| Offload primitive | Manual `std::async`/`std::future` | `relay::util::offload()` template | Already proven, type-safe, integrates with Asio coroutines |
| Transfer-back | Manual executor dispatch | `co_await asio::post(ioc, asio::use_awaitable)` | Standard Asio pattern, guaranteed to resume on event loop |

**Key insight:** All building blocks exist. This phase is purely about wiring existing infrastructure to additional call sites with a size threshold.

## Common Pitfalls

### Pitfall 1: Forgetting Transfer-Back After Offload
**What goes wrong:** Coroutine resumes on pool thread after `co_await offload(...)` and accesses shared state (session map, request router, etc.) causing data races.
**Why it happens:** The `offload()` template resumes on the pool thread by design (documented in thread_pool.h comments).
**How to avoid:** Use the `offload_if_large()` helper which encapsulates the transfer-back. Never access shared state between `co_await offload(...)` and `co_await asio::post(ioc, use_awaitable)`.
**Warning signs:** TSAN reports, intermittent crashes, corrupted shared state.

### Pitfall 2: AEAD Counter Race
**What goes wrong:** Counter is read inside the offloaded lambda by reference while the event loop thread has already incremented it for the next operation.
**Why it happens:** UDS drain_send_queue processes messages sequentially but with co_await suspension points. If counter is captured by reference, the pool thread reads a stale or advanced value.
**How to avoid:** D-10: increment counter on event loop, capture by value in lambda. `auto counter = send_counter_++; co_await offload(pool_, [counter, ...]{...});`
**Warning signs:** AEAD decrypt failures, "decrypt failed" log messages, UDS disconnections.

### Pitfall 3: Lambda Lifetime with Large Captures
**What goes wrong:** Offloaded lambda captures references to local variables that go out of scope before the pool thread executes.
**Why it happens:** `offload()` posts to the thread pool asynchronously. If the coroutine is cancelled or the scope exits, captured references dangle.
**How to avoid:** For translation calls, the JSON object and payload vector are typically local variables in the coroutine frame -- they survive across co_await. But verify that nothing captures a reference to a temporary. Prefer capturing by value for small data, by reference only for coroutine-frame-owned data.
**Warning signs:** ASAN heap-use-after-free, segfaults in pool thread.

### Pitfall 4: Threshold Mismatch Between Request and Response
**What goes wrong:** A small request (e.g., ReadRequest = 64 bytes, always inline) triggers a large response (ReadResponse with 50 MiB blob). If only the request path is offloaded, the response translation still blocks the event loop.
**Why it happens:** Threshold is per-call based on input size (D-08), so each direction is evaluated independently. This is correct behavior -- just needs to be applied at all sites.
**How to avoid:** Ensure BOTH `json_to_binary()` AND `binary_to_json()` calls have threshold checks at every site. Don't skip one because "the request is always small."
**Warning signs:** Event loop starvation on read-heavy workloads with large blobs.

### Pitfall 5: Non-Coroutine Translation Sites
**What goes wrong:** `route_response()` and `handle_notification()` are void functions, not coroutines. Attempting to add `co_await` inside them fails to compile.
**Why it happens:** These were designed as synchronous dispatch functions called from the read_loop coroutine.
**How to avoid:** Option A: lift translation into `read_loop()` before calling these functions. The read_loop is a coroutine and can offload there.
**Warning signs:** Compilation errors.

## Code Examples

### Example 1: Conditional Offload Helper (New File)

```cpp
// relay/util/offload_if_large.h
#pragma once

#include "relay/util/thread_pool.h"
#include <asio.hpp>
#include <cstddef>

namespace chromatindb::relay::util {

/// Offload threshold for CPU-heavy work (64 KB per D-06).
constexpr size_t OFFLOAD_THRESHOLD = 65536;

/// Conditionally offload a callable to the thread pool if payload exceeds threshold.
/// If offloaded, transfers back to the io_context event loop before returning.
/// If inline, executes directly on the event loop thread.
template <typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload_if_large(
    asio::thread_pool& pool,
    asio::io_context& ioc,
    size_t payload_size,
    F&& fn) {
    if (payload_size > OFFLOAD_THRESHOLD) {
        auto result = co_await offload(pool, std::forward<F>(fn));
        co_await asio::post(ioc, asio::use_awaitable);
        co_return result;
    }
    co_return fn();
}

}  // namespace chromatindb::relay::util
```

### Example 2: forward_query() with Offload (Modified)

```cpp
// handlers_query.cpp forward_query() -- modified lines 127, 166
asio::awaitable<HttpResponse> forward_query(
    const nlohmann::json& query_json,
    uint64_t session_id,
    core::UdsMultiplexer& uds_mux,
    core::RequestRouter& router,
    ResponsePromiseMap& promises,
    asio::io_context& ioc,
    asio::thread_pool& pool,        // NEW parameter
    const uint32_t* request_timeout) {

    // 1. Translate JSON -> binary (offload if large).
    auto json_str = query_json.dump();  // Measure size from JSON string
    auto result = co_await util::offload_if_large(pool, ioc, json_str.size(),
        [&] { return translate::json_to_binary(query_json); });
    // ... (unchanged: register request, send, await) ...

    // 7. Translate response binary -> JSON (offload if large).
    auto response_json = co_await util::offload_if_large(pool, ioc,
        response->payload.size(),
        [&] { return translate::binary_to_json(response->type,
            std::span<const uint8_t>(response->payload)); });
    // ...
}
```

### Example 3: AEAD Offload with Counter-by-Value (Modified)

```cpp
// uds_multiplexer.cpp send_encrypted() -- per D-09, D-10
asio::awaitable<bool> UdsMultiplexer::send_encrypted(std::span<const uint8_t> plaintext) {
    auto counter = send_counter_++;  // D-10: increment BEFORE offload, copy
    auto ct = co_await util::offload_if_large(pool_, ioc_, plaintext.size(),
        [plaintext, key = std::span<const uint8_t>(send_key_), counter] {
            return wire::aead_encrypt(plaintext, key, counter);
        });
    co_return co_await send_raw(ct);
}
```

### Example 4: read_loop() with Pre-Route Translation Offload

```cpp
// uds_multiplexer.cpp read_loop() -- lift translation before route_response
asio::awaitable<void> UdsMultiplexer::read_loop() {
    while (connected_) {
        auto msg = co_await recv_encrypted();
        if (!msg) { /* ... disconnect handling ... */ co_return; }

        auto decoded = wire::TransportCodec::decode(*msg);
        if (!decoded) { continue; }

        // For request_id==0 (notifications/broadcasts): pre-translate if large
        if (decoded->request_id == 0 && decoded->payload.size() > util::OFFLOAD_THRESHOLD) {
            auto json_opt = co_await util::offload_if_large(pool_, ioc_,
                decoded->payload.size(),
                [type = static_cast<uint8_t>(decoded->type),
                 payload = std::span<const uint8_t>(decoded->payload)] {
                    return translate::binary_to_json(type, payload);
                });
            // Pass pre-translated JSON to route_response variant
            route_response_pretranslated(static_cast<uint8_t>(decoded->type),
                std::move(decoded->payload), decoded->request_id, std::move(json_opt));
        } else {
            route_response(static_cast<uint8_t>(decoded->type),
                std::move(decoded->payload), decoded->request_id);
        }
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| JSON parse/translate inline on event loop (Phase 111 D-05) | Conditional offload >64KB to thread pool (Phase 114 D-01) | Phase 114 | Eliminates event loop starvation for large blobs |
| AEAD inline on event loop | Conditional offload >64KB to thread pool (Phase 114 D-02) | Phase 114 | AEAD for 50 MiB blob no longer blocks all clients |
| All translation always inline | Size-gated: inline for small, offload for large | Phase 114 | Zero overhead for small queries, parallel execution for large |

**Phase 113 baseline metrics (to beat):**
- Mixed workload p99: 507ms (baseline 9.48ms) -- +5250% degradation
- Large blob write 50 MiB: 3.63s (blocks event loop entire time)
- Large blob read 50 MiB: 3.71s (blocks event loop entire time)

## Open Questions

1. **read_loop() notification path refactoring**
   - What we know: `route_response()` and `handle_notification()` are sync void functions that call `binary_to_json()`. They cannot use `co_await`.
   - What's unclear: Best way to integrate offload without a large refactor. Option A (lift into read_loop) is recommended but requires passing pre-translated JSON through the routing logic.
   - Recommendation: Implement Option A. Add a `route_response_pretranslated()` overload that accepts optional pre-translated JSON. For request_id!=0 (HTTP response path), translation happens in the HTTP handler coroutines, not in route_response, so no change needed there (the ResponsePromise path just passes raw bytes).

2. **forward_query() input size measurement**
   - What we know: D-08 says "HTTP body size for json_to_binary()". But `forward_query()` receives a `const nlohmann::json&`, not raw bytes.
   - What's unclear: Whether to measure `query_json.dump().size()` (allocates a string) or use a proxy like number of fields.
   - Recommendation: For query handlers, the JSON is always small (list request, stats request, etc. are all sub-KB). The threshold will never be hit. Use `0` as the size (always inline) for `forward_query` json_to_binary calls. Only `binary_to_json` on the response path can be large (e.g., large ListResponse or BatchReadResponse). For `handlers_data.cpp`, the HTTP body size IS known (it's the `body.size()` parameter).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | `relay/tests/CMakeLists.txt` |
| Quick run command | `cd build && cmake --build . --target chromatindb_relay_tests && relay/tests/chromatindb_relay_tests` |
| Full suite command | `cd build && cmake --build . && relay/tests/chromatindb_relay_tests && ctest --test-dir relay/tests` |

### Phase Requirements -> Test Map

Phase 114 has no formal requirement IDs (noted as TBD in phase description). Test coverage maps to the locked decisions:

| Decision | Behavior | Test Type | Automated Command | File Exists? |
|----------|----------|-----------|-------------------|-------------|
| D-01 | json_to_binary/binary_to_json offloaded for large payloads | unit | `chromatindb_relay_tests "[offload]"` | Wave 0 |
| D-02 | AEAD encrypt/decrypt offloaded for large payloads | unit | `chromatindb_relay_tests "[offload][aead]"` | Wave 0 |
| D-06/D-07 | 64 KB threshold: small payloads stay inline | unit | `chromatindb_relay_tests "[offload][threshold]"` | Wave 0 |
| D-10 | AEAD counter captured by value before offload | unit | `chromatindb_relay_tests "[offload][aead][counter]"` | Wave 0 |
| D-04/D-05 | All call sites use consistent offload pattern | build | `cmake --build . --target chromatindb_relay_tests` | Compile check |
| Mixed workload | Large blob ops don't starve small queries | integration | `python3 tools/relay_benchmark.py --mixed-workload` | Existing |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . --target chromatindb_relay_tests && relay/tests/chromatindb_relay_tests`
- **Per wave merge:** Full relay test suite + manual smoke test with large blob
- **Phase gate:** Full suite green + benchmark comparison with Phase 113 baseline

### Wave 0 Gaps
- [ ] `relay/tests/test_offload_if_large.cpp` -- unit tests for threshold helper (inline vs offload behavior, transfer-back correctness)
- [ ] Existing AEAD tests in `test_aead.cpp` cover encrypt/decrypt correctness; no new file needed for AEAD logic itself
- [ ] Existing translator tests in `test_translator.cpp` cover translation correctness; offload tests verify the wrapping, not the translation logic

## Sources

### Primary (HIGH confidence)
- `relay/util/thread_pool.h` -- verified `offload()` template implementation, 36 lines
- `relay/http/http_router.cpp` lines 259-310 -- verified ML-DSA-87 offload reference pattern
- `relay/http/handlers_query.cpp` lines 117-174 -- verified `forward_query()` translation call sites
- `relay/http/handlers_data.cpp` -- verified all 4 handler functions and their translation call sites
- `relay/core/uds_multiplexer.cpp` lines 440-617 -- verified AEAD and notification translation call sites
- `relay/core/uds_multiplexer.h` -- verified class interface and member layout
- `relay/wire/aead.cpp` -- verified ChaCha20-Poly1305 implementation via OpenSSL EVP
- `relay/relay_main.cpp` lines 215-338 -- verified thread pool creation and component wiring
- `tools/benchmark_report.md` -- Phase 113 performance baseline (verified)

### Secondary (MEDIUM confidence)
- `.planning/phases/111-single-threaded-rewrite/111-CONTEXT.md` -- Phase 111 decisions (D-03/D-05 being reversed by Phase 114)

### Tertiary (LOW confidence)
None.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all existing infrastructure verified in source
- Architecture: HIGH - offload pattern proven in production (ML-DSA-87), mechanical extension to new sites
- Pitfalls: HIGH - AEAD counter safety (D-09/D-10) and non-coroutine sites are real hazards, both documented with mitigations

**Research date:** 2026-04-14
**Valid until:** 2026-05-14 (stable -- no external dependency changes expected)
