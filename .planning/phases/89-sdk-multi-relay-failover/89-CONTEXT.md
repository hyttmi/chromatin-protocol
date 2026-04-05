# Phase 89: SDK Multi-Relay Failover - Context

**Gathered:** 2026-04-05
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK clients can connect to multiple relays and automatically rotate to the next one when the current relay is unreachable. Three capabilities: (1) connect() accepts a list of relay addresses (SDK-01), (2) auto-reconnect cycles through the relay list on failure (SDK-02), (3) circuit-breaker backoff after exhausting the full list prevents tight-loop retry.

</domain>

<decisions>
## Implementation Decisions

### connect() API shape
- **D-01:** Replace `host`/`port` params with `relays: list[tuple[str, int]]`. Single-relay usage: `relays=[("host", 4201)]`. No backward compat shim — pre-MVP, clean break.
- **D-02:** `relays` is a required parameter. First relay in the list is the preferred/primary relay. List order defines priority.

### Relay rotation strategy
- **D-03:** Ordered rotation through the relay list. On connection failure, try the next relay in list order. After the last relay, wrap back to the first.
- **D-04:** Circuit-breaker after full cycle: reuse existing jittered exponential backoff (1s base, 30s cap) from `_reconnect.py:backoff_delay()`. Attempt counter tracks full cycles through the relay list, not individual relay attempts. Same pattern as Phase 84 single-relay reconnect.
- **D-05:** Individual relay attempts within a cycle have no inter-attempt delay (try next relay immediately after failure). The backoff only applies between full cycles.

### Relay health tracking
- **D-06:** No health tracking or failure memory. Always start from the top of the relay list each cycle. Simple, predictable, no stale state. A relay that was down seconds ago might be back.

### Relay switch notification
- **D-07:** Extend existing `on_reconnect` callback signature to include relay info: `on_reconnect(attempt_count, downtime_seconds, relay_host, relay_port)`. No new callback type. App can compare to previous relay to detect a switch.
- **D-08:** Expose a `current_relay` property on ChromatinClient returning `(host, port)` tuple so the app can always query which relay it's connected to.

### Claude's Discretion
- Internal relay index tracking (simple integer index into the list)
- Whether `_do_connect` takes relay params or reads from internal state
- Jitter amount on per-relay connect timeout within a cycle
- Test strategy: mock multiple relays, test rotation and circuit-breaker
- Whether to add a `connected_relay` field to the on_reconnect callback info or just pass positional args
- Log messages for relay rotation events

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### SDK client (primary modification targets)
- `sdk/python/chromatindb/client.py` -- ChromatinClient class, connect(), _do_connect(), _reconnect_loop(), _connection_monitor()
- `sdk/python/chromatindb/_reconnect.py` -- ConnectionState enum, backoff_delay(), OnReconnect callback type

### SDK transport and handshake
- `sdk/python/chromatindb/_transport.py` -- Transport layer
- `sdk/python/chromatindb/_handshake.py` -- PQ handshake (perform_handshake)

### SDK tests
- `sdk/python/tests/` -- Existing test patterns, reconnect test fixtures

### Prior phase context
- `.planning/phases/84-sdk-auto-reconnect/84-CONTEXT.md` -- SDK auto-reconnect pattern (backoff, state machine, callbacks)
- `.planning/phases/88-relay-resilience/88-CONTEXT.md` -- Relay-side resilience (subscription tracking, UDS reconnect)

### Requirements
- `.planning/REQUIREMENTS.md` -- SDK-01 (multi-relay connect), SDK-02 (auto-reconnect relay rotation)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `backoff_delay()` in `_reconnect.py` -- Full jitter exponential backoff, reuse directly for circuit-breaker between cycles
- `ConnectionState` enum -- DISCONNECTED/CONNECTING/CONNECTED/CLOSING state machine, no changes needed
- `_reconnect_loop()` -- Existing reconnect loop to modify (add relay cycling)
- `_do_connect()` -- Existing connect logic (TCP + PQ handshake), needs to accept relay params
- `_restore_subscriptions()` -- Already replays subscriptions after reconnect, works unchanged with multi-relay

### Established Patterns
- `_host`/`_port` stored on client instance -- will become `_relays` list + `_relay_index` integer
- `on_reconnect` callback receives `(attempt_count, downtime_seconds)` -- extend with `(relay_host, relay_port)`
- connect() classmethod creates client, sets fields, returns context manager -- same pattern, new params

### Integration Points
- `connect()` classmethod -- Change signature: `relays` replaces `host`/`port`
- `_reconnect_loop()` -- Add relay cycling within the loop, backoff between full cycles
- `_do_connect()` -- Pass current relay's (host, port) instead of reading `self._host`/`self._port`
- `on_reconnect` callback type -- Extend signature in `_reconnect.py`
- All existing tests using `connect(host, port, ...)` -- Update to `connect(relays=[(host, port)], ...)`

</code_context>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 89-sdk-multi-relay-failover*
*Context gathered: 2026-04-05*
