# Phase 111: Single-Threaded Rewrite - Research

**Researched:** 2026-04-14
**Domain:** Asio concurrency model, C++20 coroutine threading, strand/mutex removal
**Confidence:** HIGH

## Summary

Phase 111 converts the relay from a multi-threaded io_context (hardware_concurrency threads calling ioc.run()) to a single event loop thread with a separate thread pool for CPU-heavy offload. This eliminates all strand and mutex synchronization that was added during the failed Phase 999.10 strand confinement attempt. The refactoring is mechanical: all shared state (RequestRouter, ResponsePromiseMap, SubscriptionTracker, WriteTracker, TokenStore, UdsMultiplexer) lives on the single event loop thread and is accessed without synchronization.

The only CPU-heavy operation worth offloading is ML-DSA-87 signature verification in Authenticator::verify(). TLS handshakes and JSON parse/serialize stay inline on the event loop (per user decisions D-03, D-04, D-05). The thread pool is created at startup using the node's proven `offload()` template, copied to `relay/util/thread_pool.h` under the relay namespace.

**Primary recommendation:** Execute this as a bottom-up refactoring: (1) copy thread_pool.h to relay, (2) convert relay_main.cpp threading model, (3) strip strands from UdsMultiplexer/handlers/router/tests, (4) strip mutexes from authenticator/http_server, (5) convert atomics to plain types, (6) wire offload() at the one ML-DSA-87 call site, (7) verify all tests compile and pass.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Single io_context thread for all I/O (HTTP accept, connections, UDS, SSE, timers). relay_main.cpp creates one thread calling ioc.run().
- **D-02:** Thread pool created at startup with `hardware_concurrency()` threads. No config field -- hardcoded. `resolve_worker_threads()` not needed.
- **D-03:** Only ML-DSA-87 signature verification offloads to the thread pool via `crypto::offload()`. TLS handshakes and JSON parse/serialize stay inline on the event loop.
- **D-04:** TLS handshakes run inline on the event loop via Asio's ssl::stream. No custom SSL BIO or socket transfer. ~1-2ms per handshake is acceptable at relay scale.
- **D-05:** All JSON parse/serialize stays inline on the event loop. nlohmann/json is fast enough for all relay payloads (sub-millisecond even for large ListResponse/BatchReadResponse).
- **D-06:** Copy node's `db/crypto/thread_pool.h` to `relay/util/thread_pool.h` under `chromatindb::relay::util` namespace. Relay stays self-contained with no cross-layer build dependency. ~45 lines.
- **D-07:** Remove all `asio::strand` and `asio::make_strand` from relay production code and tests. Remove `Strand` type aliases from http_router, handlers_query, handlers_data, handlers_pubsub, uds_multiplexer.
- **D-08:** Remove `tls_mutex_` from http_server.h/cpp and `acl_mutex_` from authenticator.h/cpp. Single-threaded event loop means no concurrent access.
- **D-09:** Remove `set_strand()` and strand pointer members from HttpRouter and handler classes. Functions that posted to strand now execute directly.
- **D-10:** Convert `std::atomic<uint32_t>` for rate_limit_rate, request_timeout, max_blob_size to plain `uint32_t`. SIGHUP handler runs via async_signal_set coroutine on the same event loop thread -- no races possible. Documents the single-threaded invariant.
- **D-11:** Tests go single-threaded to match production. Remove strand parameters from test helpers (`run_async_dispatch`, `register_auth_routes`). Tests use plain `asio::io_context` with `ioc.run()`.

### Claude's Discretion
- Order of refactoring within the phase (e.g., strands first vs thread pool first)
- Whether to introduce relay-specific `offload()` wrapper or use the copied template directly
- How to handle the transfer-back pattern after offload (explicit `co_await asio::post(ioc)` vs wrapper)
- Plan decomposition -- how many plans to split this into

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CONC-01 | Single io_context thread for all I/O | relay_main.cpp threading model change: 1 ioc thread + separate thread_pool |
| CONC-02 | CPU-heavy operations offload to thread pool | Only ML-DSA-87 verify offloads (D-03). offload() template from node. Transfer-back pattern documented. |
| CONC-03 | All shared data structures accessed only from single event loop thread -- no mutexes, no strands | 6 shared structures identified, 26 files with strand/mutex/atomic references inventoried |
| CONC-04 | Remove all std::mutex and asio::strand code from Phase 999.10 | Complete inventory below: 2 mutexes, 1 global strand, 6+ Strand type aliases, ~15 strand posting sites |
| CONC-05 | relay_main.cpp creates one thread running ioc.run(), plus thread pool for offload | Current: N threads calling ioc.run(). Target: 1 thread + asio::thread_pool |
| VER-01 | All existing relay unit tests compile and pass | 21 test files, 223 tests. Strand usage in test_http_router.cpp needs adaptation. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | Event loop, thread_pool, coroutines | Already in relay CMakeLists.txt |
| liboqs | latest | ML-DSA-87 verify (the one offload target) | Already linked |
| System OpenSSL | 3.3+ | TLS transport (inline, not offloaded) | Already linked |

### Supporting
No new libraries needed. The `offload()` template is a ~45 line header copied from db/crypto/thread_pool.h.

## Architecture Patterns

### Current Threading Model (BEFORE)
```
relay_main.cpp:
  asio::io_context ioc;
  auto strand = asio::make_strand(ioc);     // global strand
  // ...wire everything to strand...
  
  auto thread_count = hardware_concurrency();
  std::vector<std::thread> threads;
  for (unsigned i = 1; i < thread_count; ++i) {
    threads.emplace_back([&ioc]() { ioc.run(); });
  }
  ioc.run();  // Main thread also runs
```

### Target Threading Model (AFTER)
```
relay_main.cpp:
  asio::io_context ioc;
  asio::thread_pool offload_pool(std::thread::hardware_concurrency());
  // ...wire everything to ioc directly...
  
  ioc.run();  // Single thread, blocks
```

### Pattern 1: offload() for CPU-Heavy Work
**What:** Post CPU-heavy work to thread pool, resume coroutine on pool thread, then transfer back to event loop.
**When to use:** Only for ML-DSA-87 signature verification (~2-5ms per verify).
**Example:**
```cpp
// Source: db/crypto/thread_pool.h (proven pattern)
// In authenticator.cpp verify path:
auto result = co_await relay::util::offload(pool, [&]() {
    return authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);
});
// CRITICAL: After offload(), we're on the pool thread.
// Must transfer back before touching any shared state.
co_await asio::post(ioc, asio::use_awaitable);
// Now safe to access TokenStore, ChallengeStore, etc.
```

### Pattern 2: Direct State Access (No Strand)
**What:** All shared state accessed directly without synchronization.
**When to use:** All handlers, UDS multiplexer, idle reaper, signal handlers.
**Example:**
```cpp
// BEFORE (strand posting):
co_await asio::post(strand_, asio::use_awaitable);
auto* session = token_store.lookup(token);

// AFTER (direct, single-threaded):
auto* session = token_store.lookup(token);
```

### Pattern 3: ResponsePromise Executor
**What:** ResponsePromise timer uses ioc executor directly instead of strand executor.
**When to use:** All create_promise() calls.
**Example:**
```cpp
// BEFORE:
auto promise = promises_.create_promise(relay_rid, strand_);

// AFTER:
auto promise = promises_.create_promise(relay_rid, ioc_.get_executor());
```

### Anti-Patterns to Avoid
- **Keeping any strand "just in case":** The entire point is elimination. Zero strands.
- **Leaving atomics where plain types suffice:** SIGHUP runs on the event loop thread. No atomic needed for rate_limit_rate, request_timeout, max_blob_size.
- **Forgetting transfer-back after offload():** The offload() template resumes on the pool thread. Callers MUST `co_await asio::post(ioc, asio::use_awaitable)` before touching shared state. Missing this creates the exact race conditions we're eliminating.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Thread pool offload | Custom post/resume logic | Copy `db/crypto/thread_pool.h` offload() template | Proven, ASAN-clean, ~45 lines |

## Complete Inventory of Changes

### File-by-File Change Map

This is the authoritative list of every file that needs modification, organized by change category.

#### 1. relay_main.cpp (Lines 214-543)
| Current | Target | Lines |
|---------|--------|-------|
| `auto strand = asio::make_strand(ioc)` | Delete | L215 |
| `std::atomic<uint32_t> rate_limit_rate` | `uint32_t rate_limit_rate` | L221 |
| `std::atomic<uint32_t> request_timeout` | `uint32_t request_timeout` | L222 |
| `std::atomic<uint32_t> max_blob_size` | `uint32_t max_blob_size` | L223 |
| `register_auth_routes(router, auth, store, strand)` | Remove strand param | L275 |
| `router.set_strand(&strand)` | Delete | L281 |
| `UdsMultiplexer(ioc, strand, ...)` | Remove strand param | L316 |
| `DataHandlers(..., strand)` | Remove strand param | L331-333 |
| `QueryHandlerDeps{..., &strand}` | Remove strand field | L337-338 |
| `PubSubHandlers(..., strand)` | Remove strand param | L343-344 |
| `asio::co_spawn(strand, idle_reaper(), ...)` | `asio::co_spawn(ioc, idle_reaper(), ...)` | L434 |
| `asio::steady_timer timer(strand)` | `asio::steady_timer timer(ioc)` | L400 |
| `rate_limit_rate.store(...)` | `rate_limit_rate = ...` | L493 |
| `max_blob_size.store(...)` | `max_blob_size = ...` | L500 |
| `request_timeout.store(...)` | `request_timeout = ...` | L507 |
| `static std::atomic<uint64_t> notification_counter` | `static uint64_t notification_counter` | L297 |
| Thread pool: N threads calling ioc.run() | `asio::thread_pool pool(hw); ... ioc.run();` | L528-543 |

#### 2. relay/http/http_router.h
| Current | Target |
|---------|--------|
| `using Strand = asio::strand<...>` | Delete |
| `void set_strand(Strand* s)` | Delete |
| `Strand* strand_ = nullptr` | Delete |
| `register_auth_routes(... Strand& strand)` | Remove strand param |

#### 3. relay/http/http_router.cpp
| Current | Target |
|---------|--------|
| `check_auth(... Strand& strand)` async function | Remove strand param, delete `co_await asio::post(strand, ...)` line |
| `register_auth_routes(... Strand& strand)` | Remove strand param, remove all `co_await asio::post(strand, ...)` from lambdas |
| `if (strand_) { co_await check_auth(..., *strand_) }` in dispatch_async | Call check_auth_sync directly (or inline auth check) |

#### 4. relay/http/handlers_data.h
| Current | Target |
|---------|--------|
| `using Strand = asio::strand<...>` | Delete |
| `Strand& strand_` member | Delete |
| `DataHandlers(... Strand& strand)` | Remove strand param |
| `const std::atomic<uint32_t>& max_blob_size_` | `const uint32_t& max_blob_size_` |
| `const std::atomic<uint32_t>& request_timeout_` | `const uint32_t& request_timeout_` |

#### 5. relay/http/handlers_data.cpp
| Current | Target |
|---------|--------|
| All `co_await asio::post(strand_, ...)` calls | Delete (4 occurrences: write, read, delete, batch_read) |
| `promises_.create_promise(relay_rid, strand_)` | `promises_.create_promise(relay_rid, ioc_executor)` -- needs ioc reference |
| `max_blob_size_.load(...)` | Direct read (plain uint32_t&) |
| `request_timeout_.load(...)` | Direct read |

#### 6. relay/http/handlers_query.h
| Current | Target |
|---------|--------|
| `using Strand = asio::strand<...>` | Delete |
| `Strand* strand = nullptr` in QueryHandlerDeps | Delete |

#### 7. relay/http/handlers_query.cpp
| Current | Target |
|---------|--------|
| `forward_query(... Strand& strand)` | Remove strand param |
| `co_await asio::post(strand, ...)` | Delete |
| `promises.create_promise(relay_rid, strand)` | Use ioc executor |
| All route lambdas capturing `strand_ptr` | Remove strand_ptr capture |

#### 8. relay/http/handlers_pubsub.h
| Current | Target |
|---------|--------|
| `using Strand = asio::strand<...>` | Delete |
| `Strand& strand_` member | Delete |
| `PubSubHandlers(... Strand& strand)` | Remove strand param |

#### 9. relay/http/handlers_pubsub.cpp
| Current | Target |
|---------|--------|
| All `co_await asio::post(strand_, ...)` calls | Delete (3 occurrences: subscribe, unsubscribe, events_auth) |

#### 10. relay/core/uds_multiplexer.h
| Current | Target |
|---------|--------|
| `using Strand = asio::strand<...>` | Delete |
| `Strand& strand_` member | Delete |
| `UdsMultiplexer(... Strand& strand, ...)` | Remove strand param |
| `const std::atomic<uint32_t>* request_timeout_` | `const uint32_t* request_timeout_` |

#### 11. relay/core/uds_multiplexer.cpp
| Current | Target |
|---------|--------|
| `socket_(strand_)` | `socket_(ioc_)` |
| `asio::co_spawn(strand_, ...)` (5 occurrences) | `asio::co_spawn(ioc_, ...)` |
| `asio::post(strand_, [this, msg]() { ... })` in send() | Direct inline execution (we're already on the event loop) |
| `asio::steady_timer timer(strand_)` (2 occurrences) | `asio::steady_timer timer(ioc_)` |
| `request_timeout_->load(...)` | Direct dereference `*request_timeout_` |
| `asio::local::stream_protocol::socket(strand_)` | `asio::local::stream_protocol::socket(ioc_)` |

#### 12. relay/http/http_server.h
| Current | Target |
|---------|--------|
| `#include <mutex>` | Delete |
| `mutable std::mutex tls_mutex_` | Delete |
| `std::atomic<uint32_t> max_connections_` | `uint32_t max_connections_` |
| `std::atomic<uint32_t> active_connections_{0}` | `uint32_t active_connections_{0}` |

#### 13. relay/http/http_server.cpp
| Current | Target |
|---------|--------|
| All `std::lock_guard<std::mutex> lock(tls_mutex_)` (3 occurrences) | Delete |
| `active_connections_.load(...)` | Direct read |
| `max_connections_.load(...)` | Direct read |
| `max_connections_.store(...)` | Direct assignment |

#### 14. relay/core/authenticator.h
| Current | Target |
|---------|--------|
| `#include <mutex>` | Delete |
| `mutable std::mutex acl_mutex_` | Delete |

#### 15. relay/core/authenticator.cpp
| Current | Target |
|---------|--------|
| `std::lock_guard<std::mutex> lock(acl_mutex_)` (3 occurrences) | Delete |

#### 16. relay/http/http_connection.h
| Current | Target |
|---------|--------|
| `std::atomic<uint32_t>& active_connections_` | `uint32_t& active_connections_` |
| ConnectionGuard uses `std::atomic<uint32_t>&` | Use `uint32_t&` with plain ++/-- |

#### 17. relay/http/http_connection.cpp
| Current | Target |
|---------|--------|
| No functional changes beyond header type change | Constructor param type changes |

#### 18. relay/core/metrics_collector.h
| Current | Target |
|---------|--------|
| 8 `std::atomic<uint64_t>` in RelayMetrics | 8 plain `uint64_t` |
| `MetricsCollector(const std::atomic<bool>& stopping)` | `MetricsCollector(const bool& stopping)` |
| `const std::atomic<bool>& stopping_` | `const bool& stopping_` |

#### 19. relay/core/metrics_collector.cpp
| Current | Target |
|---------|--------|
| `metrics_.*.load(std::memory_order_relaxed)` (8 occurrences) | Direct read |

#### 20. relay/http/response_promise.h
| Current | Target |
|---------|--------|
| Comments about strand serialization | Update to single-threaded invariant comments |

#### 21. relay/http/token_store.h
| Current | Target |
|---------|--------|
| "Access serialized via strand" comment | Update to single-threaded comment |

#### 22. relay/core/subscription_tracker.h
| Current | Target |
|---------|--------|
| "Access serialized via strand" comment | Update to single-threaded comment |

#### 23. relay/core/request_router.h
| Current | Target |
|---------|--------|
| "Access serialized via strand" comment | Update to single-threaded comment |

#### 24. relay/core/write_tracker.h
| Current | Target |
|---------|--------|
| "Access serialized via strand" comment | Update to single-threaded comment |

#### 25. relay/tests/test_http_router.cpp
| Current | Target |
|---------|--------|
| `using Strand = asio::strand<...>` | Delete |
| `run_async_dispatch` creates strand, calls `router.set_strand` | Remove strand, use plain ioc |
| 5 test cases create strand for `register_auth_routes` | Remove strand param |

#### 26. relay/tests/test_metrics_collector.cpp
| Current | Target |
|---------|--------|
| `std::atomic<bool> stopping{false}` (7 occurrences) | `bool stopping = false` |

### New File

#### relay/util/thread_pool.h
Copy of `db/crypto/thread_pool.h` with namespace changed from `chromatindb::crypto` to `chromatindb::relay::util`. Remove `resolve_worker_threads()` (D-02 says not needed). Keep only the `offload()` template.

### Offload Wiring (The One Call Site)

The ML-DSA-87 verify call in `relay/http/http_router.cpp` (register_auth_routes, POST /auth/verify handler, line 357) is the only place that needs offload(). Current code:

```cpp
// Current (on strand, blocking event loop):
auto result = authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);
```

Target:

```cpp
// Offload to thread pool, then transfer back:
auto result = co_await relay::util::offload(pool, [&]() {
    return authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);
});
// Transfer back to event loop before accessing ChallengeStore/TokenStore:
co_await asio::post(ioc, asio::use_awaitable);
```

**Important:** The `offload()` template needs a reference to the `asio::thread_pool`. This must be passed through to register_auth_routes(). The cleanest approach: replace the strand parameter with a thread_pool reference.

### UdsMultiplexer::send() Simplification

Current `send()` posts to strand to serialize access. In single-threaded model, all callers are already on the event loop thread, so send() becomes direct:

```cpp
// BEFORE:
bool UdsMultiplexer::send(std::vector<uint8_t> transport_msg) {
    if (!connected_) return false;
    asio::post(strand_, [this, msg = std::move(transport_msg)]() mutable {
        send_queue_.push_back(std::move(msg));
        if (!draining_) {
            draining_ = true;
            asio::co_spawn(strand_, [self_ptr]() -> asio::awaitable<void> {
                co_await self_ptr->drain_send_queue();
            }, asio::detached);
        }
    });
    return true;
}

// AFTER:
bool UdsMultiplexer::send(std::vector<uint8_t> transport_msg) {
    if (!connected_) return false;
    send_queue_.push_back(std::move(transport_msg));
    if (!draining_) {
        draining_ = true;
        auto self_ptr = this;
        asio::co_spawn(ioc_, [self_ptr]() -> asio::awaitable<void> {
            co_await self_ptr->drain_send_queue();
        }, asio::detached);
    }
    return true;
}
```

### Executor Propagation for ResponsePromise

DataHandlers and forward_query() currently create promises with `strand_` as executor. After removing strands, they need the ioc executor. Two options:

**Option A (recommended):** Store `asio::io_context&` reference in DataHandlers and QueryHandlerDeps. Use `ioc_.get_executor()` for promise creation.

**Option B:** Store `asio::any_io_executor` captured at construction time.

Option A is simpler and matches how UdsMultiplexer already stores `ioc_`.

### active_connections_ Atomic Decision

The `active_connections_` counter in HttpServer is incremented/decremented by ConnectionGuard in HttpConnection::handle(). In single-threaded model, there's only one thread, so atomic is unnecessary. Convert to plain `uint32_t` with plain ++/--.

## Common Pitfalls

### Pitfall 1: Forgetting Transfer-Back After offload()
**What goes wrong:** After `co_await offload(pool, fn)`, the coroutine resumes on the thread pool thread, not the event loop. Accessing shared state (TokenStore, ChallengeStore) from the pool thread creates a data race.
**Why it happens:** The `offload()` template uses `asio::post(pool, ...)` which means the completion handler runs on the pool's executor.
**How to avoid:** Always `co_await asio::post(ioc, asio::use_awaitable)` immediately after offload() before touching any shared state. The node codebase has this pattern at 6+ call sites -- copy it exactly.
**Warning signs:** TSAN reports after Phase 112 benchmarking.

### Pitfall 2: UDS send() Race During Transition
**What goes wrong:** If send() is refactored to be direct but some code path still calls it from a thread pool thread (e.g., after offload() without transfer-back), you get a race on send_queue_.
**Why it happens:** send() was previously self-serializing via strand post. Removing that means callers must guarantee they're on the event loop.
**How to avoid:** Audit every call site of UdsMultiplexer::send(). All are in handlers that should be on the event loop thread. The offload() call site in auth verify must transfer back before any code that eventually calls send().
**Warning signs:** Queue corruption, double-drain.

### Pitfall 3: Broken ResponsePromise Timer After Removing Strand
**What goes wrong:** ResponsePromise creates a timer with the strand executor. After removing strands, the timer must use the ioc executor. If the executor is wrong, `co_await promise->wait()` may never wake up or resume on the wrong context.
**Why it happens:** The executor determines which thread runs the completion handler. Wrong executor = wrong thread = potential race or deadlock.
**How to avoid:** Pass `ioc.get_executor()` (or `ioc_.get_executor()`) to create_promise() instead of `strand_`.
**Warning signs:** Hanging coroutines, test timeouts.

### Pitfall 4: Leaving Dead Strand References in Comments/Docs
**What goes wrong:** Comments like "Access serialized via strand" remain in token_store.h, request_router.h, etc. Misleading for future development.
**Why it happens:** Pure oversight during mechanical refactoring.
**How to avoid:** grep for "strand" in all relay .h/.cpp files after refactoring. Update comments to reflect single-threaded invariant.
**Warning signs:** Grep finds matches after phase completion.

### Pitfall 5: MetricsCollector stopping_ Bool Reference Lifetime
**What goes wrong:** MetricsCollector takes `const std::atomic<bool>& stopping_`. Converting to `const bool&` is safe if the bool outlives the collector (it does -- it's in relay_main's stack frame). But if anyone passes a temporary bool, it's dangling.
**Why it happens:** Reference semantics don't enforce lifetime.
**How to avoid:** Keep the same pattern (reference to relay_main local variable). The lifetime is identical.
**Warning signs:** Use-after-free in shutdown path.

### Pitfall 6: Notification Counter Static Atomic
**What goes wrong:** relay_main.cpp has `static std::atomic<uint64_t> notification_counter` in the send_json lambda. This is a static local -- it persists across calls. Converting to plain uint64_t is safe in single-threaded model.
**Why it happens:** Easy to miss static locals during grep.
**How to avoid:** Include it in the refactoring checklist.

## Code Examples

### offload() Template (to copy to relay/util/thread_pool.h)
```cpp
// Source: db/crypto/thread_pool.h
// Copy with namespace change to chromatindb::relay::util

#pragma once

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>

#include <concepts>
#include <system_error>
#include <type_traits>
#include <utility>

namespace chromatindb::relay::util {

/// Dispatch a callable to the thread pool and co_await its result.
/// NOTE: The coroutine resumes on the thread_pool thread after co_await.
/// Callers MUST co_await asio::post(ioc, asio::use_awaitable) to transfer
/// back to the event loop before touching shared state.
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

}  // namespace chromatindb::relay::util
```

### relay_main.cpp Target Threading Model
```cpp
// Target: single thread + offload pool
asio::io_context ioc;
asio::thread_pool offload_pool(
    std::thread::hardware_concurrency() > 0
        ? std::thread::hardware_concurrency()
        : 2);

// ... wire everything to ioc directly, no strand ...

spdlog::info("  event_loop: 1 thread");
spdlog::info("  offload_pool: {} threads", 
    std::thread::hardware_concurrency() > 0
        ? std::thread::hardware_concurrency()
        : 2);

ioc.run();  // Blocks on single thread
```

### Auth Verify with offload() and Transfer-Back
```cpp
// In register_auth_routes(), POST /auth/verify handler:
// After ChallengeStore::consume (on event loop):

// Offload ML-DSA-87 verification to thread pool
auto result = co_await relay::util::offload(pool, [&]() {
    return authenticator.verify(*challenge_bytes, *pubkey_bytes, *sig_bytes);
});

// CRITICAL: Transfer back to event loop before TokenStore access
co_await asio::post(ioc, asio::use_awaitable);

if (!result.success) {
    co_return HttpResponse::error(401, "auth_failed", result.error_code);
}

// Now safe: on event loop thread, TokenStore access is serialized
auto token = token_store.create_session(...);
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Multi-threaded ioc.run() + strand | Single-threaded ioc + thread pool offload | Phase 111 (this phase) | Eliminates all races |
| std::mutex for shared state | No synchronization needed | Phase 111 | Simpler code, no deadlock risk |
| Atomic SIGHUP settings | Plain variables | Phase 111 | SIGHUP runs on event loop thread |

**Deprecated/outdated:**
- Phase 999.10's strand confinement approach: proved insufficient, abandoned
- Multi-threaded ioc.run() pattern: root cause of all ASAN race reports

## Open Questions

1. **ioc reference propagation to auth handler**
   - What we know: register_auth_routes() currently takes `Strand& strand`. It needs `asio::thread_pool& pool` and `asio::io_context& ioc` for offload + transfer-back.
   - What's unclear: Whether to add two parameters or bundle them in a struct.
   - Recommendation: Add both parameters directly. register_auth_routes(router, authenticator, token_store, pool, ioc). Simple and explicit.

2. **DataHandlers ioc reference**
   - What we know: DataHandlers needs ioc executor for ResponsePromise creation (replacing strand_).
   - What's unclear: Whether to store ioc& or executor.
   - Recommendation: Store `asio::io_context& ioc_` directly, matching UdsMultiplexer pattern.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt |
| Quick run command | `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir relay/tests --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CONC-01 | Single ioc thread | build verification | Build succeeds with single-thread main | N/A (relay_main.cpp change) |
| CONC-02 | ML-DSA-87 offload | unit | Existing test_authenticator tests pass | Exists (test_authenticator.cpp) |
| CONC-03 | No mutex/strand on shared state | grep audit | `grep -r 'strand\|std::mutex' relay/ --include='*.h' --include='*.cpp'` finds 0 hits | N/A (verification command) |
| CONC-04 | All strand/mutex code removed | grep audit | Same as CONC-03 | N/A |
| CONC-05 | relay_main creates 1 ioc thread + pool | code review | Inspect relay_main.cpp | N/A |
| VER-01 | All existing tests pass | unit | `cd build && ctest --test-dir relay/tests --output-on-failure` | Exists (21 test files) |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . --target chromatindb_relay_tests && ctest --test-dir relay/tests -j$(nproc) --output-on-failure`
- **Per wave merge:** Full relay test suite
- **Phase gate:** Full suite green + grep audit for zero strand/mutex/atomic occurrences

### Wave 0 Gaps
- None -- existing test infrastructure covers all phase requirements. No new test files needed. The changes are removing synchronization primitives, not adding new functionality.

## Recommended Plan Decomposition

Based on the change inventory, this phase decomposes naturally into 3 plans:

**Plan 1: Thread Pool Infrastructure + relay_main.cpp Threading Model**
- Create `relay/util/thread_pool.h` (copy + namespace change)
- Convert relay_main.cpp: single ioc.run() thread + asio::thread_pool
- Convert relay_main.cpp atomics to plain types (rate_limit_rate, request_timeout, max_blob_size, notification_counter)
- Remove strand creation and strand wiring from relay_main.cpp
- Convert SIGHUP handler to use plain assignment instead of atomic store
- Update stopping flag handling (keep atomic -- shutdown signal can be from signal handler)

**Plan 2: Strip Strands/Mutexes from All Relay Components**
- Remove Strand type aliases, strand_ members, strand params from: http_router.h/cpp, handlers_data.h/cpp, handlers_query.h/cpp, handlers_pubsub.h/cpp, uds_multiplexer.h/cpp
- Remove tls_mutex_ from http_server.h/cpp
- Remove acl_mutex_ from authenticator.h/cpp
- Remove all `co_await asio::post(strand_, ...)` calls from handler code
- Simplify UdsMultiplexer::send() to direct inline execution
- Convert HttpServer/HttpConnection active_connections_ and max_connections_ to plain uint32_t
- Convert RelayMetrics atomics to plain uint64_t
- Update "serialized via strand" comments in token_store.h, request_router.h, subscription_tracker.h, write_tracker.h, response_promise.h
- Wire ioc references to DataHandlers, QueryHandlerDeps, PubSubHandlers for ResponsePromise executor

**Plan 3: ML-DSA-87 Offload + Test Adaptation + Verification**
- Wire offload() at the one call site in http_router.cpp (auth verify) with transfer-back
- Update register_auth_routes() signature to take pool + ioc instead of strand
- Update test_http_router.cpp: remove strand from run_async_dispatch and auth route tests
- Update test_metrics_collector.cpp: convert atomic<bool> stopping to plain bool
- Verify all 21 test files compile and pass
- Run grep audit confirming zero strand/mutex occurrences in relay production code

**Note on `stopping` atomic:** The `std::atomic<bool> stopping` in relay_main.cpp and MetricsCollector should remain atomic. The SIGTERM signal handler writes it, and async code reads it. Signal handlers may execute asynchronously relative to the event loop. However, since we use `async_signal_set`, the handler runs on the ioc thread, so it could also be plain bool. The CONTEXT.md only mentions converting rate_limit_rate/request_timeout/max_blob_size (D-10). Keep `stopping` as atomic for safety unless the planner decides otherwise.

## Sources

### Primary (HIGH confidence)
- `db/crypto/thread_pool.h` -- offload() template source (47 lines, verified)
- `db/crypto/verify_helpers.h` -- verify_with_offload() pattern example
- `db/engine/engine.cpp` lines 175, 231, 250, 382, 391 -- offload() usage with transfer-back
- `db/peer/message_dispatcher.cpp` line 339, 1305 -- `co_await asio::post(ioc_, asio::use_awaitable)` transfer-back pattern
- All 26 relay source files with strand/mutex/atomic (inventoried above)

### Secondary (MEDIUM confidence)
- Phase 999.10 CONTEXT.md -- root cause analysis of why strands failed

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, copying proven internal code
- Architecture: HIGH - single-threaded model is identical to node's PeerManager pattern (proven ASAN/TSAN clean)
- Pitfalls: HIGH - all pitfalls derived from direct code inspection of the relay codebase and node's offload pattern
- Change inventory: HIGH - exhaustive grep + file-by-file analysis of all 26 affected files

**Research date:** 2026-04-14
**Valid until:** 2026-05-14 (stable -- no external dependency changes)
