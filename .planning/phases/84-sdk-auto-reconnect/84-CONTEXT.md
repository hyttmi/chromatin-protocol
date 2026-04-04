# Phase 84: SDK Auto-Reconnect - Context

**Gathered:** 2026-04-04
**Status:** Ready for planning

<domain>
## Phase Boundary

Python SDK transparently recovers from connection loss. Jittered exponential backoff (1s base, 30s cap), infinite retries. After reconnect: restore subscriptions, notify app via callbacks. State machine tracks connection lifecycle. Intentional close() does not trigger reconnect.

</domain>

<decisions>
## Implementation Decisions

### Reconnect trigger & backoff
- **D-01:** Any read/write failure triggers auto-reconnect. When the internal message loop or a send operation gets a ConnectionError/EOF, enter reconnect mode. Covers both clean disconnects and network failures.
- **D-02:** Infinite retries with jittered exponential backoff. 1s base, 30s cap, random jitter. The app can call close() to stop. Matches daemon-style resilience.

### Subscription restore
- **D-03:** Silent re-subscribe after successful reconnect + handshake. Re-send Subscribe for all namespaces in _subscriptions. Log at debug level. If re-subscribe fails, log warning but don't disconnect — app gets reconnect event and can handle it.

### App notification API
- **D-04:** Callback functions. Pass on_disconnect and on_reconnect callbacks to ChromatinClient constructor (or set_reconnect_handler()). Callbacks receive connection info (attempt count, downtime duration).

### Intentional vs unintentional close
- **D-05:** State machine tracking connection lifecycle (disconnected, connecting, connected, closing). App can query connection state. close() transitions to closing state, which suppresses reconnect. Connection loss transitions to disconnected, which triggers reconnect.

### Claude's Discretion
- State machine implementation (enum, class, or string states)
- Whether on_disconnect fires before or after reconnect attempt starts
- How pending send operations are handled during reconnect (queue, raise, or drop)
- Whether the message loop task is cancelled and restarted on reconnect or paused
- Whether to expose a `wait_connected()` awaitable for callers who want to block until reconnected
- Test approach (mock server, actual relay, or protocol-level simulation)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### SDK client
- `sdk/python/chromatindb/client.py` — ChromatinClient class (line 82), connect(), close(), subscribe(), _subscriptions set
- `sdk/python/chromatindb/_transport.py` — Transport layer (if exists), read/write operations
- `sdk/python/chromatindb/_handshake.py` — PQ handshake implementation

### SDK tests
- `sdk/python/tests/` — Existing test patterns, fixtures, mocking approach

### Requirements
- `.planning/REQUIREMENTS.md` — CONN-03, CONN-04, CONN-05

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `_subscriptions: set[bytes]` — already tracked per-client, reusable for re-subscribe (line 90)
- `connect()` method — existing connect + handshake logic, reusable for reconnect
- `subscribe()` / `encode_subscribe()` — existing subscribe implementation

### Established Patterns
- Async context manager (`async with ChromatinClient(...) as client:`)
- asyncio-based async/await throughout
- ConnectionError/ProtocolError exception hierarchy

### Integration Points
- Message loop (where read failures are detected → trigger reconnect)
- send operations (where write failures are detected → trigger reconnect)
- close() method (set closing state to suppress reconnect)
- Constructor (accept callback parameters)

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 84-sdk-auto-reconnect*
*Context gathered: 2026-04-04*
