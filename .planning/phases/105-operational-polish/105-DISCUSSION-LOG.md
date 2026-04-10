# Phase 105: Operational Polish - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-10
**Phase:** 105-operational-polish
**Areas discussed:** Metrics selection, Rate limiting strategy, Shutdown drain semantics
**Mode:** --auto (all decisions auto-selected)

---

## Metrics Selection

| Option | Description | Selected |
|--------|-------------|----------|
| Relay-specific counters + gauges mirroring node pattern | chromatindb_relay_ prefix, 7 counters + 3 gauges, atomic increments | ✓ |
| Minimal counters only | Just connections and errors, no gauges | |
| Full observability with histograms | Add latency histograms for translation/auth -- complex, overkill for v3.0.0 | |

**User's choice:** [auto] Relay-specific counters + gauges mirroring node pattern (recommended default)
**Notes:** Node's MetricsCollector is the proven pattern. Relay counters: ws_connections_total, ws_disconnections_total, messages_received_total, messages_sent_total, auth_failures_total, rate_limited_total, errors_total. Gauges: ws_connections_active, subscriptions_active, uptime_seconds.

---

## Rate Limiting Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Token bucket, messages/sec, per-session | Simple algorithm, good burst tolerance, enforced at WsSession before translation | ✓ |
| Sliding window, messages/sec | More precise rate enforcement, slightly more complex | |
| Token bucket, bytes/sec | Limits bandwidth rather than message count -- redundant with send queue cap | |

**User's choice:** [auto] Token bucket, messages/sec, per-session (recommended default)
**Notes:** Messages/sec is the right dimension because inbound message rate is the abuse vector. Send queue cap already handles outbound backpressure. Token bucket is simplest and most battle-tested.

---

## Shutdown Drain Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| Stop accept, drain queues, close frames, force stop | 4-step sequence with 5s drain timeout + 2s close timeout | ✓ |
| Close frames immediately, then force stop | Current behavior -- no drain, may lose in-flight messages | |
| Drain with no timeout | Could hang indefinitely on slow clients | |

**User's choice:** [auto] Stop accept, drain queues, close frames, force stop (recommended default)
**Notes:** Existing handler already does step 1 (acceptor.stop) and step 3-4 (Close(1001) + 2s timer). Adding drain-first with 5s timeout completes SESS-04.

---

## Claude's Discretion

- Internal MetricsCollector API design
- Token bucket implementation details
- Log messages and levels for rate limiting
- Whether to include /health alongside /metrics (FUTURE-03)
- Sustained violation disconnect threshold

## Deferred Ideas

None -- discussion stayed within phase scope.
