# Phase 89: SDK Multi-Relay Failover - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-05
**Phase:** 89-sdk-multi-relay-failover
**Areas discussed:** connect() API shape, Relay rotation strategy, Relay health tracking, on_relay_switch callback

---

## connect() API shape

| Option | Description | Selected |
|--------|-------------|----------|
| List of tuples | `relays=[(host1, port1), (host2, port2)]` -- simple, Pythonic, mirrors existing params. Single-relay still works. | :heavy_check_mark: |
| Keep host/port + add fallbacks | `connect(host, port, fallback_relays=[...])` -- backward-compatible, primary explicit | |
| Relay config object | `connect(relays=RelayConfig([...]))` -- typed object with per-relay options. More extensible but heavier. | |

**User's choice:** List of tuples
**Notes:** Clean break from current `host`/`port` params. Pre-MVP, no backward compat needed.

---

## Relay rotation strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Ordered rotation | Try relays in list order, wrap around. Circuit-breaker backoff between full cycles. | :heavy_check_mark: |
| Random rotation | Pick random relay each attempt. Distributes load but less predictable. | |
| Sticky with fallback | Always try to return to primary. Only use others while primary is down. | |

**User's choice:** Ordered rotation
**Notes:** User controls priority by list position. Predictable behavior.

### Circuit-breaker shape

| Option | Description | Selected |
|--------|-------------|----------|
| Reuse existing backoff | Jittered exponential (1s base, 30s cap) between full cycles. Same as Phase 84 pattern. | :heavy_check_mark: |
| Flat pause | Fixed delay between cycles. Simpler but less adaptive. | |
| Escalating with max retries | Backoff between cycles, give up after N full cycles. | |

**User's choice:** Reuse existing backoff
**Notes:** Attempt counter tracks full cycles, not individual relay attempts.

---

## Relay health tracking

| Option | Description | Selected |
|--------|-------------|----------|
| No tracking | Always start from top of list each cycle. No stale state. | :heavy_check_mark: |
| Cooldown timer | Mark relay as "cold" for N seconds after failure, skip cold relays. | |
| Failure counter | Track consecutive failures per relay, deprioritize high-failure relays. | |

**User's choice:** No tracking
**Notes:** Simple, predictable. A relay down seconds ago might be back.

---

## on_relay_switch callback

| Option | Description | Selected |
|--------|-------------|----------|
| Extend on_reconnect | Add (relay_host, relay_port) to existing on_reconnect signature. | :heavy_check_mark: |
| New on_relay_switch callback | Separate callback, fires only on relay change. | |
| No notification | App doesn't need to know. Keep on_reconnect as-is. | |

**User's choice:** Extend on_reconnect
**Notes:** No new callback type. App can compare to previous relay to detect a switch.

---

## Claude's Discretion

- Internal relay index tracking
- _do_connect parameter passing
- Per-relay connect timeout jitter
- Test strategy details
- Log messages for relay rotation

## Deferred Ideas

None -- discussion stayed within phase scope.
