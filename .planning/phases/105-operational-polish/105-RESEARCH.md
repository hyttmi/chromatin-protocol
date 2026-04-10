# Phase 105: Operational Polish - Research

**Researched:** 2026-04-10
**Domain:** Prometheus metrics, token-bucket rate limiting, SIGHUP config reload, graceful SIGTERM shutdown
**Confidence:** HIGH

## Summary

Phase 105 adds four operational capabilities to the relay: a Prometheus /metrics HTTP endpoint, per-client message-rate token bucket limiting, extension of the existing SIGHUP handler for new config fields, and a drain-first graceful SIGTERM shutdown. All four are well-understood patterns already proven in the node codebase (db/peer/metrics_collector.cpp, db/peer/message_dispatcher.cpp). The relay code is structured for clean insertion -- MetricsCollector as a new component, RateLimiter as a header-only utility, SIGHUP/SIGTERM as extensions to existing relay_main.cpp handlers.

The node's MetricsCollector (368 lines, coroutine-based HTTP accept loop, Prometheus text exposition format) is the canonical reference. The relay version is simpler: no Storage dependency, no peers_ deque, no SIGUSR1 dump -- just atomic counters and HTTP. The token bucket is also proven in the node (try_consume_tokens in message_dispatcher.cpp) but the relay version is message-count based (not byte-based) and per-WsSession (not per-PeerInfo).

**Primary recommendation:** Mirror the node's MetricsCollector pattern exactly for the relay metrics endpoint. Use a header-only RateLimiter class with steady_clock for the token bucket. Extend relay_main.cpp SIGHUP/SIGTERM handlers in-place.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: New class `MetricsCollector` in `relay/core/metrics_collector.h/cpp`. Follows node's proven pattern: coroutine HTTP accept loop on shared io_context, GET /metrics only, 404 everything else, text/plain; version=0.0.4.
- D-02: Config field `metrics_bind` in RelayConfig (string, empty=disabled, host:port=enabled). SIGHUP-reloadable (start/stop/restart).
- D-03: Metric prefix: `chromatindb_relay_` (distinguishes from node's `chromatindb_` prefix).
- D-04: Counters (_total suffix): ws_connections_total, ws_disconnections_total, messages_received_total, messages_sent_total, auth_failures_total, rate_limited_total, errors_total.
- D-05: Gauges: ws_connections_active, subscriptions_active, uptime_seconds.
- D-06: MetricsCollector holds atomic counters. Components increment via reference. No mutex.
- D-07: Token bucket algorithm per session. Each WsSession owns a RateLimiter instance.
- D-08: Rate limit dimension: messages/sec (not bytes/sec).
- D-09: Enforcement point: WsSession::on_message() in AUTHENTICATED path, BEFORE translation/forwarding.
- D-10: Config field `rate_limit_messages_per_sec` in RelayConfig (uint32_t, default 0 = disabled). SIGHUP-reloadable.
- D-11: On sustained violation (N consecutive rejections): disconnect client.
- D-12: RateLimiter class in `relay/core/rate_limiter.h` -- header-only, lightweight. Token bucket with burst = rate.
- D-13: Extend existing SIGHUP handler in relay_main.cpp to reload rate_limit_messages_per_sec and metrics_bind.
- D-14: Rate limit reload: sessions read rate on each message arrival via atomic (no lock needed).
- D-15: Metrics bind reload: call MetricsCollector::stop() then start() with new bind address.
- D-16: Replace existing SIGTERM handler with drain-first: (1) stop acceptor, (2) signal sessions to drain, (3) drain or timeout, (4) Close(1001), (5) 2s close handshake timeout, (6) ioc.stop().
- D-17: Drain timeout: 5 seconds.
- D-18: UDS multiplexer: stop sending new requests during shutdown.

### Claude's Discretion
- Internal API design for MetricsCollector (method signatures, which component passes metrics struct)
- Token bucket implementation details (refill strategy, clock source)
- Exact log messages and spdlog levels for rate limiting events
- Whether to add /health alongside /metrics (FUTURE-03 -- can include if trivial, otherwise defer)
- Sustained violation threshold for disconnect (e.g., 10 consecutive rejections)
- Test organization within relay/tests/

### Deferred Ideas (OUT OF SCOPE)
- None
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| OPS-01 | Prometheus /metrics HTTP endpoint (connections, messages, errors) | Node MetricsCollector pattern, RelayMetrics struct with atomics, coroutine HTTP accept loop |
| OPS-02 | SIGHUP config reload (TLS context, connection limits, rate limits) | Existing SIGHUP handler at relay_main.cpp:274-311, extend with metrics_bind + rate_limit fields |
| OPS-03 | Per-client rate limiting (messages/sec) | Token bucket per WsSession, header-only RateLimiter, enforcement in on_message AUTHENTICATED path |
| SESS-04 | Graceful shutdown on SIGTERM (drain queues, close frames) | Existing SIGTERM at relay_main.cpp:253-272, replace with drain-first sequence using SessionManager::for_each |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest (FetchContent) | Async I/O, coroutines, timers, signal handling | Already in project, all relay networking uses it |
| spdlog | latest (FetchContent) | Structured logging | Already in project, all relay logging uses it |
| nlohmann/json | latest (FetchContent) | JSON config parsing | Already in project, relay config uses it |
| Catch2 | v3.7.1 (FetchContent) | Unit testing | Already in project, 188 existing relay tests |

No new dependencies. All four features are implemented with existing project libraries.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Hand-rolled Prometheus text | prometheus-cpp library | Adds dependency for trivial text format -- node already proves hand-rolled works |
| Token bucket | Sliding window | Token bucket is simpler, proven in node codebase, better burst tolerance |
| Atomic counters | Mutex-guarded struct | Atomics match decision D-06, simpler, faster for counter-only workload |

## Architecture Patterns

### Relay Component Layout (post-Phase 105)
```
relay/
  config/
    relay_config.h/cpp       # + metrics_bind, rate_limit_messages_per_sec fields
  core/
    authenticator.h/cpp       # (unchanged)
    message_filter.h/cpp      # (unchanged)
    metrics_collector.h/cpp   # NEW: Prometheus /metrics HTTP endpoint
    rate_limiter.h             # NEW: header-only token bucket
    request_router.h/cpp      # (unchanged)
    session.h/cpp              # (unchanged)
    subscription_tracker.h/cpp # (unchanged)
    uds_multiplexer.h/cpp      # (unchanged)
  ws/
    ws_session.h/cpp           # + RateLimiter member, on_message rate check
    ws_acceptor.h/cpp          # (unchanged)
    session_manager.h/cpp      # (unchanged)
  relay_main.cpp               # + SIGHUP extension, SIGTERM drain-first
  tests/
    test_rate_limiter.cpp      # NEW
    test_metrics_collector.cpp # NEW
    test_relay_config.cpp      # extended for new fields
```

### Pattern 1: Atomic Counter Struct (RelayMetrics)
**What:** A struct of std::atomic<uint64_t> fields for all relay metrics, passed by reference to components that increment them.
**When to use:** When multiple coroutines on the same io_context need to increment counters. The relay uses a thread pool (hardware_concurrency() threads running ioc.run()), so multiple threads can call on_message concurrently.
**Why atomics, not plain uint64_t:** The node's NodeMetrics uses plain uint64_t because all access is strand-confined (verified in Phase 99 CORO-01). The relay's WsSession callbacks run on ANY thread in the thread pool via the shared io_context, so atomics are required. D-06 explicitly mandates atomics.
**Example:**
```cpp
// relay/core/metrics_collector.h
struct RelayMetrics {
    std::atomic<uint64_t> ws_connections_total{0};
    std::atomic<uint64_t> ws_disconnections_total{0};
    std::atomic<uint64_t> messages_received_total{0};
    std::atomic<uint64_t> messages_sent_total{0};
    std::atomic<uint64_t> auth_failures_total{0};
    std::atomic<uint64_t> rate_limited_total{0};
    std::atomic<uint64_t> errors_total{0};
    // Gauges are computed at scrape time (ws_connections_active from SessionManager::count(),
    // subscriptions_active from SubscriptionTracker, uptime from steady_clock).
};
```

### Pattern 2: Header-Only Token Bucket RateLimiter
**What:** Per-session rate limiter using token bucket with steady_clock for time tracking. Consumed at the top of the AUTHENTICATED message path.
**When to use:** Every inbound message from an authenticated client.
**Example:**
```cpp
// relay/core/rate_limiter.h (header-only)
class RateLimiter {
public:
    // rate = messages/sec, burst = rate (allows 1-second burst per D-12)
    // rate == 0 means disabled
    void set_rate(uint32_t rate) {
        rate_ = rate;
        burst_ = rate;
        tokens_ = rate;  // Full bucket on rate change
        consecutive_rejects_ = 0;
    }

    // Returns true if message is allowed, false if rate exceeded
    bool try_consume() {
        if (rate_ == 0) return true;  // Disabled
        refill();
        if (tokens_ < 1.0) {
            ++consecutive_rejects_;
            return false;
        }
        tokens_ -= 1.0;
        consecutive_rejects_ = 0;
        return true;
    }

    bool should_disconnect(uint32_t threshold) const {
        return consecutive_rejects_ >= threshold;
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(tokens_ + elapsed * rate_, static_cast<double>(burst_));
        last_refill_ = now;
    }

    uint32_t rate_ = 0;
    uint32_t burst_ = 0;
    double tokens_ = 0.0;
    uint32_t consecutive_rejects_ = 0;
    std::chrono::steady_clock::time_point last_refill_ = std::chrono::steady_clock::now();
};
```

### Pattern 3: Coroutine HTTP Accept Loop (Prometheus)
**What:** Minimal HTTP server on shared io_context. Accepts TCP connections, reads GET /metrics, responds with Prometheus text format, 404 for everything else.
**When to use:** Serving /metrics endpoint.
**Source:** Direct copy from db/peer/metrics_collector.cpp lines 130-248 with relay-specific changes (prefix, counters, no Storage dependency).

### Pattern 4: Drain-First SIGTERM Shutdown
**What:** Ordered shutdown sequence: stop acceptor -> drain queues -> close frames -> timeout -> ioc.stop().
**When to use:** SIGTERM/SIGINT handler.
**Example:**
```cpp
// 1. Stop accepting new connections
acceptor.stop();

// 2. Drain existing send queues (wait up to 5s)
// Signal all sessions that shutdown is happening
auto drain_timer = std::make_shared<asio::steady_timer>(ioc);
drain_timer->expires_after(std::chrono::seconds(5));
drain_timer->async_wait([&](const asio::error_code&) {
    // 3. After drain timeout: send Close(1001) to all sessions
    session_manager.for_each([](uint64_t, const auto& session) {
        session->close(1001, "server shutting down");
    });
    // 4. Give 2s for close handshake echo
    auto close_timer = std::make_shared<asio::steady_timer>(ioc);
    close_timer->expires_after(std::chrono::seconds(2));
    close_timer->async_wait([&ioc, close_timer](const asio::error_code&) {
        ioc.stop();
    });
});
```

### Anti-Patterns to Avoid
- **Mutex for metric counters:** Use std::atomic<uint64_t> with relaxed memory ordering for counters. Mutexes add contention on hot paths.
- **Computing gauges on increment:** ws_connections_active should be computed at scrape time from SessionManager::count(), not maintained as a separate counter. This avoids double-bookkeeping bugs.
- **Rate limiter with wall clock:** Use std::chrono::steady_clock, never system_clock. System clock can jump backward (NTP adjustment).
- **Blocking in SIGTERM handler:** The lambda runs on the io_context. Do not call blocking operations. Use timers for the drain sequence.
- **Draining send queues synchronously:** The Session::drain_send_queue() coroutine is already running. The shutdown sequence just needs to wait for it to finish (or timeout), not drain manually.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Prometheus exposition format | Custom binary format | Text exposition format (hand-built, trivial) | Standard is ~20 lines of string concatenation. Node already has working code to copy. |
| HTTP server for /metrics | Full HTTP parser | Minimal read-until-\r\n\r\n + first-line check | Proven pattern from node. Only serves /metrics, no general HTTP needed. |
| Rate limiting algorithm | Custom timing code | Token bucket (refill on check) | Industry standard, proven in node codebase. Simple to implement correctly. |

## Common Pitfalls

### Pitfall 1: Thread-Safety of MetricsCollector
**What goes wrong:** Node MetricsCollector uses plain uint64_t (strand-confined). Relay has a thread pool where multiple threads run ioc.run(). Copying the node pattern verbatim creates data races.
**Why it happens:** Node has a single io_context thread for metrics. Relay has hardware_concurrency() threads.
**How to avoid:** Use std::atomic<uint64_t> for all RelayMetrics counters (D-06 mandates this). Use relaxed memory ordering -- counter monotonicity is sufficient, no ordering constraints needed.
**Warning signs:** TSAN warnings on metric increments.

### Pitfall 2: Gauge Computation vs Counter Maintenance
**What goes wrong:** Maintaining ws_connections_active as an atomic counter that's incremented on connect and decremented on disconnect. Off-by-one errors if disconnect is missed.
**Why it happens:** Tempting to track active connections as a counter for simplicity.
**How to avoid:** Compute gauges at scrape time: ws_connections_active = session_manager.count(), subscriptions_active = iterate tracker. uptime_seconds = steady_clock diff. Only counters need atomic increments.

### Pitfall 3: Rate Limiter Clock Precision
**What goes wrong:** Using integer milliseconds for token refill causes quantization artifacts at high rates. At 100 msg/sec, 1ms resolution means 0.1 tokens per ms -- works fine. But at 1 msg/sec, long gaps between messages cause large refills that might overflow.
**Why it happens:** Integer arithmetic can truncate or overflow.
**How to avoid:** Use double-precision floating point for tokens (like the example above). Cap tokens at burst size. The node uses uint64_t because it tracks bytes (large values), but message-rate values are small enough that double is cleaner.

### Pitfall 4: SIGTERM Drain Race with Session Close
**What goes wrong:** Calling close(1001) on a session that already closed due to client disconnect during drain timeout. Double-close triggers UB or assertion.
**Why it happens:** 5-second drain window is long enough for clients to disconnect naturally.
**How to avoid:** WsSession::close() already guards with `if (closing_) return;`. The for_each + close(1001) pattern is safe because close() is idempotent.

### Pitfall 5: SIGHUP Rate Limit Propagation to Existing Sessions
**What goes wrong:** Reloading rate_limit_messages_per_sec in relay_main.cpp but existing WsSession instances still use the old value.
**Why it happens:** Each WsSession owns a RateLimiter with a local copy of the rate.
**How to avoid:** Store rate_limit_messages_per_sec as a std::atomic<uint32_t> in a shared location (RelayConfig or a dedicated shared state). RateLimiter::try_consume() reads from the shared atomic on each call. Alternatively, MetricsCollector or a shared config object holds the atomic, and RateLimiter references it. Per D-14: "sessions read rate on each message arrival via atomic."

### Pitfall 6: MetricsCollector Lifecycle on SIGHUP
**What goes wrong:** Stopping metrics listener while a connection handler coroutine is still running causes use-after-free of the acceptor.
**Why it happens:** metrics_acceptor_ is closed, but an in-flight metrics_handle_connection() coroutine holds a socket accepted from it.
**How to avoid:** Closing the acceptor stops new accepts. Existing connection handler coroutines operate on moved sockets and complete naturally. The node's set_metrics_bind() pattern (stop then start) is safe because socket is moved into the handler coroutine at accept time.

### Pitfall 7: Drain Timeout Implementation
**What goes wrong:** Waiting for all Session drain coroutines to finish is complex -- there's no built-in "wait for all sessions to drain."
**Why it happens:** Session::drain_send_queue() is a fire-and-forget coroutine spawned at session start.
**How to avoid:** Don't try to detect drain completion. Just wait the 5-second timeout unconditionally. Any session that hasn't drained by then gets force-closed. The existing send queues are bounded (256 messages, per config), so drain should complete well within 5s.

## Code Examples

### RelayMetrics Struct
```cpp
// relay/core/metrics_collector.h
#include <atomic>
#include <cstdint>

namespace chromatindb::relay::core {

struct RelayMetrics {
    // Counters (monotonically increasing, _total suffix in Prometheus)
    std::atomic<uint64_t> ws_connections_total{0};
    std::atomic<uint64_t> ws_disconnections_total{0};
    std::atomic<uint64_t> messages_received_total{0};
    std::atomic<uint64_t> messages_sent_total{0};
    std::atomic<uint64_t> auth_failures_total{0};
    std::atomic<uint64_t> rate_limited_total{0};
    std::atomic<uint64_t> errors_total{0};
};

} // namespace chromatindb::relay::core
```

### MetricsCollector Class Interface
```cpp
// relay/core/metrics_collector.h
class MetricsCollector {
public:
    MetricsCollector(asio::io_context& ioc, const std::string& metrics_bind,
                     const std::atomic<bool>& stopping);

    // Lifecycle
    void start();   // Start HTTP listener if metrics_bind non-empty
    void stop();    // Stop HTTP listener

    // Config reload (SIGHUP)
    void set_metrics_bind(const std::string& bind);

    // Metrics access
    RelayMetrics& metrics() { return metrics_; }
    uint64_t uptime_seconds() const;

    // Prometheus output -- needs session_manager and tracker for gauges
    std::string format_prometheus(size_t active_connections, size_t active_subscriptions);

private:
    asio::awaitable<void> accept_loop();
    asio::awaitable<void> handle_connection(asio::ip::tcp::socket socket);

    asio::io_context& ioc_;
    const std::atomic<bool>& stopping_;
    RelayMetrics metrics_;
    std::chrono::steady_clock::time_point start_time_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::string metrics_bind_;
};
```

### Rate Limit Check in on_message
```cpp
// In WsSession::on_message(), AUTHENTICATED path, BEFORE JSON parse:
// (Actually after opcode check but before any translation work)

// Rate limit check (D-09: before translation/forwarding)
if (!rate_limiter_.try_consume()) {
    metrics_->rate_limited_total.fetch_add(1, std::memory_order_relaxed);
    nlohmann::json err = {
        {"type", "error"}, {"code", "rate_limited"},
        {"message", "Rate limit exceeded"}
    };
    // Include request_id if available (need to parse JSON first for this)
    send_json(err);
    if (rate_limiter_.should_disconnect(10)) {
        spdlog::warn("session {}: sustained rate limit violation, disconnecting", session_id_);
        close(CLOSE_PROTOCOL_ERROR, "rate limit exceeded");
    }
    co_return;
}
metrics_->messages_received_total.fetch_add(1, std::memory_order_relaxed);
```

Note on ordering: D-09 says "BEFORE translation/forwarding." The rate limit check should happen after opcode validation but can happen before JSON parsing. However, to include request_id in the error response, JSON needs to be parsed first. Trade-off: parse JSON, then rate-limit check (so request_id is available), then translation. The JSON parse is cheap compared to translation + UDS forwarding.

### SIGHUP Extension
```cpp
// Add to existing SIGHUP handler in relay_main.cpp after max_connections reload:

// Reload rate limit (D-14: sessions read from shared atomic)
rate_limit_rate.store(new_cfg.rate_limit_messages_per_sec, std::memory_order_relaxed);
spdlog::info("rate_limit reloaded: {} msg/s",
             new_cfg.rate_limit_messages_per_sec == 0
                 ? "disabled" : std::to_string(new_cfg.rate_limit_messages_per_sec));

// Reload metrics_bind (D-15)
metrics_collector.set_metrics_bind(new_cfg.metrics_bind);
```

### SIGTERM Drain-First Sequence
```cpp
// Replace existing SIGTERM handler:
term_signals.async_wait([&](const asio::error_code& ec, int sig) {
    if (ec) return;
    stopping.store(true, std::memory_order_relaxed);
    spdlog::info("received signal {}, graceful shutdown ({} sessions)",
                 sig, session_manager.count());

    // 1. Stop accepting new connections
    acceptor.stop();

    // 2. Wait up to 5s for send queues to drain (D-17)
    auto drain_timer = std::make_shared<asio::steady_timer>(ioc);
    drain_timer->expires_after(std::chrono::seconds(5));
    drain_timer->async_wait([&, drain_timer](const asio::error_code&) {
        // 3. Send Close(1001) to all remaining sessions
        spdlog::info("drain timeout, closing {} sessions", session_manager.count());
        session_manager.for_each([](uint64_t, const auto& session) {
            session->close(1001, "server shutting down");
        });

        // 4. Wait 2s for close handshake echo
        auto close_timer = std::make_shared<asio::steady_timer>(ioc);
        close_timer->expires_after(std::chrono::seconds(2));
        close_timer->async_wait([&ioc, close_timer](const asio::error_code&) {
            ioc.stop();
        });
    });
});
```

### Prometheus Text Output
```cpp
std::string MetricsCollector::format_prometheus(size_t active_connections,
                                                 size_t active_subscriptions) {
    std::string out;
    out.reserve(2048);

    // Counters
    out += "# HELP chromatindb_relay_ws_connections_total Total WebSocket connections since startup.\n"
           "# TYPE chromatindb_relay_ws_connections_total counter\n"
           "chromatindb_relay_ws_connections_total "
        + std::to_string(metrics_.ws_connections_total.load(std::memory_order_relaxed)) + "\n\n";

    // ... (similar for all 7 counters)

    // Gauges (computed at scrape time)
    out += "# HELP chromatindb_relay_ws_connections_active Current active WebSocket connections.\n"
           "# TYPE chromatindb_relay_ws_connections_active gauge\n"
           "chromatindb_relay_ws_connections_active "
        + std::to_string(active_connections) + "\n\n";

    out += "# HELP chromatindb_relay_subscriptions_active Current active namespace subscriptions.\n"
           "# TYPE chromatindb_relay_subscriptions_active gauge\n"
           "chromatindb_relay_subscriptions_active "
        + std::to_string(active_subscriptions) + "\n\n";

    out += "# HELP chromatindb_relay_uptime_seconds Relay uptime in seconds.\n"
           "# TYPE chromatindb_relay_uptime_seconds gauge\n"
           "chromatindb_relay_uptime_seconds "
        + std::to_string(uptime_seconds()) + "\n";

    return out;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Node plain uint64_t metrics | Relay std::atomic metrics | Phase 105 | Thread pool requires atomics |
| Node byte-rate token bucket | Relay message-rate token bucket | Phase 105 | Simpler for WebSocket: each frame = 1 message |
| Immediate SIGTERM (current relay) | Drain-first SIGTERM | Phase 105 | Clients see clean Close(1001) instead of TCP reset |

## Open Questions

1. **Rate limit check ordering vs request_id extraction**
   - What we know: D-09 says rate limit before translation. But the error response should include request_id (from JSON body).
   - What's unclear: Parse JSON first (to get request_id), then rate-limit, then translate? Or rate-limit first (before JSON parse) and omit request_id from error?
   - Recommendation: Parse JSON first, then rate-limit check. JSON parse is cheap (~microseconds). Translation + UDS forwarding is the expensive part. The rate limit gate protects the expensive path.

2. **Shared rate for RateLimiter propagation**
   - What we know: D-14 says "sessions read rate on each message arrival via atomic." RateLimiter is per-session.
   - What's unclear: Should RateLimiter hold a reference to a shared atomic, or should on_message read the atomic and call rate_limiter.set_rate() if changed?
   - Recommendation: RateLimiter holds a pointer/reference to the shared atomic<uint32_t>. On each try_consume(), it checks if the rate changed and updates internal state. This avoids N set_rate() calls per SIGHUP.

3. **SubscriptionTracker total subscription count for gauge**
   - What we know: D-05 requires subscriptions_active gauge. SubscriptionTracker has client_subscription_count(session_id) but no total count method.
   - What's unclear: Need to add a total_subscription_count() method or compute from subs_.size().
   - Recommendation: Add a simple `size_t namespace_count() const { return subs_.size(); }` method to SubscriptionTracker. This returns the number of distinct namespaces with at least one subscriber -- the natural gauge value.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt |
| Quick run command | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |
| Full suite command | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| OPS-01 | Prometheus /metrics endpoint returns correct text format | unit | `./build/relay/tests/chromatindb_relay_tests "[metrics_collector]"` | Wave 0 |
| OPS-01 | RelayMetrics atomic counters increment correctly | unit | `./build/relay/tests/chromatindb_relay_tests "[metrics_collector]"` | Wave 0 |
| OPS-02 | SIGHUP reloads rate_limit and metrics_bind | unit | `./build/relay/tests/chromatindb_relay_tests "[relay_config]"` | Extend existing |
| OPS-03 | Token bucket allows/rejects at correct rate | unit | `./build/relay/tests/chromatindb_relay_tests "[rate_limiter]"` | Wave 0 |
| OPS-03 | Consecutive rejection triggers disconnect | unit | `./build/relay/tests/chromatindb_relay_tests "[rate_limiter]"` | Wave 0 |
| OPS-03 | Rate=0 disables limiting | unit | `./build/relay/tests/chromatindb_relay_tests "[rate_limiter]"` | Wave 0 |
| SESS-04 | Graceful shutdown drains then closes | integration | Manual (requires running relay) | N/A |

### Sampling Rate
- **Per task commit:** `cmake --build build && ./build/relay/tests/chromatindb_relay_tests`
- **Per wave merge:** Full suite (same -- relay tests are fast)
- **Phase gate:** Full suite green before /gsd:verify-work

### Wave 0 Gaps
- [ ] `relay/tests/test_rate_limiter.cpp` -- covers OPS-03 (token bucket logic)
- [ ] `relay/tests/test_metrics_collector.cpp` -- covers OPS-01 (Prometheus format, counter increment)
- [ ] Extend `relay/tests/test_relay_config.cpp` -- covers OPS-02 (new config fields parse/validate)

## Sources

### Primary (HIGH confidence)
- `db/peer/metrics_collector.h/cpp` -- Node's MetricsCollector (direct source code, reference implementation)
- `db/peer/peer_types.h` -- NodeMetrics struct pattern
- `db/peer/message_dispatcher.cpp` -- try_consume_tokens() token bucket implementation
- `relay/relay_main.cpp` -- Existing SIGTERM (lines 253-272) and SIGHUP (lines 274-311) handlers
- `relay/ws/ws_session.h/cpp` -- WsSession AUTHENTICATED path (modification target)
- `relay/config/relay_config.h/cpp` -- RelayConfig struct (modification target)
- `relay/ws/session_manager.h` -- SessionManager for_each pattern (shutdown coordination)
- `relay/core/subscription_tracker.h` -- SubscriptionTracker interface (gauge computation)

### Secondary (MEDIUM confidence)
- Prometheus text exposition format: well-documented standard, node already implements it correctly

### Tertiary (LOW confidence)
- None -- all research is based on existing project code

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all patterns proven in codebase
- Architecture: HIGH -- direct copy of node patterns with relay-specific adaptations
- Pitfalls: HIGH -- thread-safety difference (atomics vs plain) is the main risk, well-understood

**Research date:** 2026-04-10
**Valid until:** 2026-05-10 (stable patterns, no external dependency drift)
