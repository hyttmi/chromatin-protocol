# Phase 105: Operational Polish - Context

**Gathered:** 2026-04-10
**Status:** Ready for planning

<domain>
## Phase Boundary

Make the relay production-ready with four capabilities: Prometheus /metrics HTTP endpoint for observability, extension of the existing SIGHUP handler to reload rate limit configuration, per-client rate limiting to protect the node from abusive clients, and graceful SIGTERM shutdown that drains send queues before closing connections. No new message types, no new UDS behavior, no changes to authentication or translation -- purely operational concerns layered onto the existing relay.

</domain>

<decisions>
## Implementation Decisions

### Prometheus /metrics Endpoint (OPS-01)
- **D-01:** New class `MetricsCollector` in `relay/core/metrics_collector.h/cpp`. Follows the node's proven pattern from `db/peer/metrics_collector.cpp`: coroutine-based HTTP accept loop on shared `io_context`, `GET /metrics` only, 404 everything else, `text/plain; version=0.0.4`.
- **D-02:** Config field `metrics_bind` in RelayConfig (string, empty=disabled, `host:port`=enabled). Same pattern as node's `metrics_bind`. SIGHUP-reloadable (start/stop/restart).
- **D-03:** Metric prefix: `chromatindb_relay_` (distinguishes from node's `chromatindb_` prefix).
- **D-04:** Counters (_total suffix): `ws_connections_total`, `ws_disconnections_total`, `messages_received_total`, `messages_sent_total`, `auth_failures_total`, `rate_limited_total`, `errors_total`.
- **D-05:** Gauges: `ws_connections_active`, `subscriptions_active`, `uptime_seconds`.
- **D-06:** MetricsCollector holds atomic counters for all metrics. Components increment via reference (same pattern as node's `NodeMetrics` struct). No mutex -- atomics only.

### Per-Client Rate Limiting (OPS-03)
- **D-07:** Token bucket algorithm per session. Simple, well-understood, good burst tolerance. Each WsSession owns a `RateLimiter` instance.
- **D-08:** Rate limit dimension: messages/sec (not bytes/sec). Relay already has send queue cap for backpressure on outbound. Inbound message rate is the abuse vector.
- **D-09:** Enforcement point: WsSession::on_message() in AUTHENTICATED path, BEFORE translation/forwarding to UDS. Rejected messages get `{"type":"error","code":"rate_limited","message":"Rate limit exceeded"}` with the client's request_id.
- **D-10:** Config field `rate_limit_messages_per_sec` in RelayConfig (uint32_t, default 0 = disabled). SIGHUP-reloadable -- existing sessions pick up new rate on next check.
- **D-11:** On sustained violation (N consecutive rejections): disconnect client. Configurable threshold or hardcoded (Claude's discretion on threshold value).
- **D-12:** RateLimiter class in `relay/core/rate_limiter.h` -- header-only, lightweight. Token bucket with configurable rate and burst size (burst = rate, allowing 1-second burst).

### SIGHUP Config Reload Extension (OPS-02)
- **D-13:** Extend existing SIGHUP handler in `relay_main.cpp:274-311` to reload `rate_limit_messages_per_sec` and `metrics_bind`. Current handler already reloads TLS, ACL, and max_connections.
- **D-14:** Rate limit reload: update a shared atomic or notify sessions of new rate. Sessions read rate on each message arrival (no lock needed if atomic).
- **D-15:** Metrics bind reload: call `MetricsCollector::stop()` then `start()` with new bind address if changed. Same pattern as node.

### Graceful SIGTERM Shutdown (SESS-04)
- **D-16:** Replace existing SIGTERM handler in `relay_main.cpp:253-272` with drain-first sequence: (1) `acceptor.stop()` -- stop accepting new connections, (2) signal all sessions to drain their send queues, (3) after drain (or drain timeout), send Close(1001) to all sessions, (4) 2s timeout for close handshake echo, then `ioc.stop()`.
- **D-17:** Drain timeout: 5 seconds. If queues haven't drained by then, proceed to close frames anyway. Prevents indefinite hang on slow clients.
- **D-18:** UDS multiplexer: stop sending new requests during shutdown. Pending requests can complete or timeout naturally.

### Claude's Discretion
- Internal API design for MetricsCollector (method signatures, which component passes metrics struct)
- Token bucket implementation details (refill strategy, clock source)
- Exact log messages and spdlog levels for rate limiting events
- Whether to add a `/health` endpoint alongside `/metrics` (FUTURE-03 in requirements -- can include if trivial, otherwise defer)
- Sustained violation threshold for disconnect (e.g., 10 consecutive rejections)
- Test organization within relay/tests/

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Architecture
- `.planning/research/ARCHITECTURE.md` -- Relay component layout and responsibilities

### Existing Node Prometheus Pattern (reference implementation)
- `db/peer/metrics_collector.h` -- MetricsCollector class interface (start/stop, format_prometheus_metrics, metrics_accept_loop)
- `db/peer/metrics_collector.cpp` lines 130-248 -- Coroutine HTTP accept loop, GET /metrics handler, Prometheus text format output
- `db/peer/peer_types.h` -- NodeMetrics struct with atomic counters (pattern for relay metrics)
- `db/config/config.h` line 48 -- `metrics_bind` config field pattern
- `db/config/config.cpp` lines 311-327 -- `metrics_bind` validation (host:port format)

### Existing Relay Code (modification targets)
- `relay/relay_main.cpp` lines 253-311 -- SIGTERM and SIGHUP handlers (extend both)
- `relay/config/relay_config.h` -- RelayConfig struct (add metrics_bind, rate_limit fields)
- `relay/config/relay_config.cpp` -- Config loading/validation (add new fields)
- `relay/ws/ws_session.h` -- WsSession class (add rate limiter member)
- `relay/ws/ws_session.cpp` -- on_message AUTHENTICATED path (add rate limit check)
- `relay/ws/session_manager.h` -- SessionManager (for_each for shutdown drain, subscription counting)
- `relay/core/subscription_tracker.h` -- SubscriptionTracker (for active subscription gauge)

### Requirements
- `.planning/REQUIREMENTS.md` -- OPS-01, OPS-02, OPS-03, SESS-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- Node's `MetricsCollector` in `db/peer/metrics_collector.cpp`: Complete Prometheus /metrics HTTP endpoint with coroutine accept loop, text exposition format, start/stop lifecycle. Near-identical pattern for relay.
- Node's `NodeMetrics` struct in `db/peer/peer_types.h`: Atomic counter struct pattern. Relay needs its own `RelayMetrics`.
- Existing `SIGHUP` handler in `relay_main.cpp:274-311`: Already reloads TLS, ACL, max_connections. Just needs extension for rate_limit and metrics_bind.
- Existing `SIGTERM` handler in `relay_main.cpp:253-272`: Already sends Close(1001) + 2s timer. Needs drain-first sequence.

### Established Patterns
- Coroutine-based accept loops (WsAcceptor, UdsMultiplexer) -- same pattern for metrics HTTP listener
- Atomic counters for metrics (node pattern) -- no mutexes for metric increments
- Config reload via SIGHUP with re-read + apply + re-arm pattern
- Component-per-concern: Authenticator, MessageFilter, RequestRouter, SubscriptionTracker -- MetricsCollector and RateLimiter follow this

### Integration Points
- `relay_main.cpp`: MetricsCollector construction, SIGHUP reload extension, SIGTERM drain sequence
- `WsSession::on_message()`: Rate limiter check in AUTHENTICATED path
- `SessionManager`: Subscription count for gauge, drain coordination for shutdown
- `RelayConfig`: New fields (metrics_bind, rate_limit_messages_per_sec)

</code_context>

<specifics>
## Specific Ideas

No specific requirements -- node's MetricsCollector is the reference implementation to mirror for the relay.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 105-operational-polish*
*Context gathered: 2026-04-10*
