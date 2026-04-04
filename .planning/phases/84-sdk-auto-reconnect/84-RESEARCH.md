# Phase 84: SDK Auto-Reconnect - Research

**Researched:** 2026-04-04
**Domain:** Python asyncio connection resilience, state machines, exponential backoff
**Confidence:** HIGH

## Summary

Phase 84 adds transparent auto-reconnect to the Python SDK's `ChromatinClient`. The current architecture uses a single `Transport` object with AEAD-encrypted frame IO tied to session keys derived during PQ handshake. When a connection drops, the entire Transport is dead (counters, keys, reader task -- all invalid). Reconnection requires a full TCP connect + PQ handshake + new Transport creation, not a "resume" of the old session.

The key insight is that reconnection is NOT transport-level recovery -- it is a full teardown-and-rebuild of the connection stack (TCP -> handshake -> Transport -> subscriptions). The `ChromatinClient` already stores all the information needed to reconnect (`_host`, `_port`, `_identity`, `_timeout`) and tracks active subscriptions (`_subscriptions: set[bytes]`). The auto-reconnect logic lives in the `ChromatinClient` layer, not in Transport.

**Primary recommendation:** Implement a state machine (enum-based) at the ChromatinClient level that manages connection lifecycle, a background reconnect loop with jittered exponential backoff (1s base, 30s cap, infinite retries), and callback hooks for app notification. No new dependencies -- hand-roll the backoff with `random.uniform()` and `min()`.

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Any read/write failure triggers auto-reconnect. When the internal message loop or a send operation gets a ConnectionError/EOF, enter reconnect mode. Covers both clean disconnects and network failures.
- **D-02:** Infinite retries with jittered exponential backoff. 1s base, 30s cap, random jitter. The app can call close() to stop. Matches daemon-style resilience.
- **D-03:** Silent re-subscribe after successful reconnect + handshake. Re-send Subscribe for all namespaces in _subscriptions. Log at debug level. If re-subscribe fails, log warning but don't disconnect -- app gets reconnect event and can handle it.
- **D-04:** Callback functions. Pass on_disconnect and on_reconnect callbacks to ChromatinClient constructor (or set_reconnect_handler()). Callbacks receive connection info (attempt count, downtime duration).
- **D-05:** State machine tracking connection lifecycle (disconnected, connecting, connected, closing). App can query connection state. close() transitions to closing state, which suppresses reconnect. Connection loss transitions to disconnected, which triggers reconnect.

### Claude's Discretion
- State machine implementation (enum, class, or string states)
- Whether on_disconnect fires before or after reconnect attempt starts
- How pending send operations are handled during reconnect (queue, raise, or drop)
- Whether the message loop task is cancelled and restarted on reconnect or paused
- Whether to expose a `wait_connected()` awaitable for callers who want to block until reconnected
- Test approach (mock server, actual relay, or protocol-level simulation)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CONN-03 | SDK ChromatinClient auto-reconnects on connection loss with jittered exponential backoff (1s-30s) | State machine + reconnect loop pattern; backoff algorithm documented in Architecture Patterns |
| CONN-04 | SDK restores pub/sub subscriptions after successful reconnect | _subscriptions set already tracked; re-subscribe pattern documented in Code Examples |
| CONN-05 | SDK exposes a reconnection event/callback for application-level catch-up | Callback API pattern with on_disconnect/on_reconnect; documented in Architecture Patterns |

</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| asyncio | stdlib (Python 3.14) | Event loop, tasks, locks, events | Already used throughout SDK |
| enum | stdlib | ConnectionState enum | Clean state machine representation |
| random | stdlib | Jitter for backoff (`random.uniform()`) | No external dep needed for simple jitter |
| time | stdlib | Monotonic clock for downtime tracking | `time.monotonic()` for duration measurement |
| logging | stdlib | Debug-level reconnect logging | Already used in SDK ecosystem |

### Supporting
No new dependencies. The project explicitly avoids adding deps when stdlib suffices. The backoff algorithm is ~10 lines of code and does not warrant a library like `tenacity` or `backoff`.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Hand-rolled backoff | `tenacity` library | Adds a dependency for 10 lines of code; project policy is zero new deps |
| Enum state machine | `transitions` library | Overkill for 4 states; enum + if/match is clearer |
| `asyncio.Event` for wait_connected | Manual polling | Event is the standard asyncio primitive for "wait until condition" |

## Architecture Patterns

### Connection State Machine

```
                    connect()
    [DISCONNECTED] ---------> [CONNECTING]
         ^                       |
         |               success |  failure
         |                       v       |
         |               [CONNECTED]     |
         |                    |          |
         |         conn loss  |          |
         +--------------------+  backoff |
         |                       +-------+
         |
    close() from ANY state
         |
         v
      [CLOSING]
```

**States:**
```python
import enum

class ConnectionState(enum.Enum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    CLOSING = "closing"
```

**Key invariants:**
- `close()` from ANY state transitions to CLOSING -- never triggers reconnect
- Connection loss from CONNECTED transitions to DISCONNECTED -- triggers reconnect loop
- CONNECTING is entered only from DISCONNECTED (backoff happens in DISCONNECTED)
- CLOSING is terminal within a session

### Pattern 1: Reconnect Loop

**What:** A background asyncio task that runs the reconnect cycle when in DISCONNECTED state.
**When to use:** Always -- spawned when connection loss is detected.

```python
async def _reconnect_loop(self) -> None:
    attempt = 0
    disconnect_time = time.monotonic()

    while self._state == ConnectionState.DISCONNECTED:
        attempt += 1
        delay = _backoff_delay(attempt, base=1.0, cap=30.0)
        await asyncio.sleep(delay)

        if self._state == ConnectionState.CLOSING:
            return

        self._state = ConnectionState.CONNECTING
        try:
            await self._do_connect()  # TCP + handshake + Transport
            self._state = ConnectionState.CONNECTED
            await self._restore_subscriptions()
            await self._fire_on_reconnect(attempt, time.monotonic() - disconnect_time)
            return
        except Exception:
            self._state = ConnectionState.DISCONNECTED
            # Continue loop for next attempt
```

### Pattern 2: Jittered Exponential Backoff

**What:** Compute delay as `min(cap, base * 2^(attempt-1))` with full jitter.
**When to use:** Between reconnect attempts.

```python
import random

def _backoff_delay(attempt: int, base: float = 1.0, cap: float = 30.0) -> float:
    """Jittered exponential backoff: uniform random in [0, min(cap, base * 2^(attempt-1))]."""
    exp = min(cap, base * (2 ** (attempt - 1)))
    return random.uniform(0, exp)
```

This implements AWS's "Full Jitter" algorithm. For attempt 1: [0, 1s], attempt 2: [0, 2s], attempt 3: [0, 4s], ..., attempt 5+: [0, 30s].

### Pattern 3: Error Detection and Reconnect Trigger

**What:** Detect connection loss in both the reader loop and send operations, then trigger reconnect.
**When to use:** On any ConnectionError/EOF in Transport.

The detection happens in two places:
1. **Reader loop** (`Transport._reader_loop`): already calls `_close_with_error()` on exception. After this, the client detects `transport.closed` and enters reconnect.
2. **Send operations**: `send_request` and `send_message` raise `ChromatinConnectionError` when transport is closed. The client catches this and enters reconnect.

The cleanest approach: wrap operations in a try/except at the ChromatinClient level that catches `ChromatinConnectionError` and triggers reconnect. For the notification iterator, it naturally loops checking `transport.closed`.

### Pattern 4: Pending Operations During Reconnect

**Recommendation:** Raise `ChromatinConnectionError` for pending operations during reconnect. Do NOT queue them.

Rationale:
- Queuing creates unbounded memory growth during long outages
- The caller knows best whether to retry (idempotent read) or not (non-idempotent write with timestamp)
- The app gets `on_reconnect` callback to trigger catch-up logic
- This matches the behavior of every serious SDK (gRPC, Redis, NATS)

### Pattern 5: Callback Notification

**What:** `on_disconnect` and `on_reconnect` callbacks passed to constructor or via setter.
**When to use:** App-level catch-up after reconnection.

```python
from typing import Callable, Awaitable

# Callback signatures
OnDisconnect = Callable[[], Awaitable[None] | None]
OnReconnect = Callable[[int, float], Awaitable[None] | None]
# on_reconnect receives (attempt_count, downtime_seconds)
```

Callbacks should be invoked safely (try/except around each, log exceptions, never let callback failure affect reconnect logic). Support both sync and async callbacks.

### Pattern 6: wait_connected() Awaitable

**Recommendation:** Expose `wait_connected()` using `asyncio.Event`.

```python
self._connected_event = asyncio.Event()

async def wait_connected(self, timeout: float | None = None) -> bool:
    """Wait until connected. Returns True if connected, False on timeout."""
    if self._state == ConnectionState.CONNECTED:
        return True
    if self._state == ConnectionState.CLOSING:
        return False
    try:
        await asyncio.wait_for(self._connected_event.wait(), timeout=timeout)
        return True
    except asyncio.TimeoutError:
        return False
```

This is lightweight, idiomatic, and avoids busy-wait polling.

### Anti-Patterns to Avoid
- **Resuming Transport state:** AEAD nonce counters and session keys are per-connection. Never try to "resume" a Transport -- always create a new one.
- **Reconnecting on HandshakeError:** If the PQ handshake fails (e.g., key rejected by node), retrying with backoff makes sense (node might restart), but if it's a persistent auth failure, the app is stuck. Log loudly.
- **Reconnecting on close():** The CLOSING state MUST suppress all reconnect attempts. This is the most common bug in reconnect implementations.
- **Blocking the event loop during backoff:** Always use `asyncio.sleep()`, never `time.sleep()`.
- **Firing callbacks synchronously in reconnect loop:** If callback is async, it must be awaited. If sync, it must be called in a way that doesn't block the event loop. Use `asyncio.iscoroutinefunction()` to dispatch correctly.

### Recommended Changes to Existing Code

**ChromatinClient.connect() classmethod:** Needs additional parameters for callbacks and auto_reconnect flag:
```python
@classmethod
def connect(
    cls,
    host: str,
    port: int,
    identity: Identity,
    *,
    timeout: float = 10.0,
    auto_reconnect: bool = True,
    on_disconnect: OnDisconnect | None = None,
    on_reconnect: OnReconnect | None = None,
) -> ChromatinClient:
```

**ChromatinClient.__aenter__:** After initial connection, spawn the connection monitor. On initial connection failure, raise immediately (auto-reconnect only activates after first successful connection).

**ChromatinClient.__aexit__:** Set state to CLOSING, cancel reconnect task if running, then clean up transport.

**Transport teardown:** The existing `Transport.stop()` method handles cancellation and cleanup. On reconnect, the old Transport is fully stopped, then a new one is created. No Transport code changes needed.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| AEAD encrypted transport | Custom crypto framing | Existing `_framing.py` + `_transport.py` | Already correct and tested; reconnect creates a new Transport instance |
| PQ handshake | Custom key exchange | Existing `_handshake.py` `perform_handshake()` | Proven correct; reuse on each reconnect |
| Subscription tracking | Custom subscription manager | Existing `_subscriptions: set[bytes]` | Already maintained per-client; just re-send on reconnect |

**Key insight:** Reconnect is orchestration of existing primitives (connect, handshake, subscribe), not new protocol work. Every building block already exists.

## Common Pitfalls

### Pitfall 1: AEAD Nonce Desync on Reconnect
**What goes wrong:** Attempting to reuse old Transport's send/recv counters after reconnect.
**Why it happens:** Developer thinks reconnect means "resume" rather than "rebuild."
**How to avoid:** Always create a fresh Transport with counters from the new handshake (send_counter=1, recv_counter=1 post-handshake).
**Warning signs:** `DecryptionError: AEAD decryption failed` immediately after reconnect.

### Pitfall 2: Race Between close() and Reconnect Loop
**What goes wrong:** `close()` is called while reconnect loop is in the middle of TCP connect or handshake. The new transport gets created, then immediately torn down, potentially leaving half-open connections.
**Why it happens:** State check is not atomic with the connect operation.
**How to avoid:** Check `self._state == ConnectionState.CLOSING` after each await point in the reconnect loop. Cancel the reconnect task from `close()`.
**Warning signs:** Resource leaks (unclosed writers), unexpected reconnect after close().

### Pitfall 3: Callback Exception Kills Reconnect
**What goes wrong:** An `on_reconnect` callback raises an exception, which propagates into the reconnect loop and kills it.
**Why it happens:** Not wrapping callback invocations in try/except.
**How to avoid:** Always wrap callback calls: `try: await cb(args) except Exception: log.warning("callback failed", exc_info=True)`.
**Warning signs:** Reconnect stops working after adding a callback.

### Pitfall 4: Notification Queue Stale Data
**What goes wrong:** Old notifications from pre-disconnect transport are mixed with post-reconnect notifications.
**Why it happens:** The notification queue is not cleared on reconnect.
**How to avoid:** Drain (clear) the notification queue when entering DISCONNECTED state. The app gets `on_reconnect` to trigger catch-up anyway.
**Warning signs:** App processes notifications that are older than the reconnect event.

### Pitfall 5: Initial Connection Failure vs. Reconnect
**What goes wrong:** Auto-reconnect activates when the very first `__aenter__` connect fails.
**Why it happens:** Not distinguishing "never connected" from "was connected, lost connection."
**How to avoid:** Auto-reconnect only activates after the first successful connection. Initial failure raises immediately so the caller knows the endpoint is unreachable.
**Warning signs:** `async with ChromatinClient.connect(...)` hangs forever instead of raising on bad host/port.

### Pitfall 6: Subscription Re-send Failure Cascades
**What goes wrong:** Re-subscribing after reconnect fails, triggering another disconnect, creating an infinite reconnect loop.
**Why it happens:** Subscribe failure raises ConnectionError, which triggers reconnect again.
**How to avoid:** Per D-03: if re-subscribe fails, log warning but don't disconnect. The app gets `on_reconnect` and can handle it.
**Warning signs:** Rapid reconnect cycling in logs.

### Pitfall 7: asyncio.Event Across Event Loops
**What goes wrong:** `wait_connected()` using an Event created on a different event loop.
**Why it happens:** Event created during `__init__` before the event loop is running.
**How to avoid:** Create the Event lazily on first use, or in `__aenter__`.
**Warning signs:** `RuntimeError: no running event loop` or `Event` never fires.

## Code Examples

### Jittered Exponential Backoff (verified pattern from AWS Architecture Blog)
```python
import random

def _backoff_delay(attempt: int, base: float = 1.0, cap: float = 30.0) -> float:
    """Full jitter: uniform random in [0, min(cap, base * 2^(attempt-1))].

    attempt=1 -> [0, 1.0]
    attempt=2 -> [0, 2.0]
    attempt=3 -> [0, 4.0]
    attempt=4 -> [0, 8.0]
    attempt=5 -> [0, 16.0]
    attempt=6 -> [0, 30.0]  (capped)
    """
    exp = min(cap, base * (2 ** (attempt - 1)))
    return random.uniform(0, exp)
```

### Safe Callback Invocation
```python
import asyncio
import logging

log = logging.getLogger(__name__)

async def _invoke_callback(callback, *args):
    """Safely invoke a sync or async callback."""
    if callback is None:
        return
    try:
        result = callback(*args)
        if asyncio.iscoroutine(result):
            await result
    except Exception:
        log.warning("reconnect callback raised", exc_info=True)
```

### Transport Teardown and Rebuild
```python
async def _teardown_transport(self) -> None:
    """Fully tear down current transport."""
    if self._transport is not None:
        try:
            await self._transport.stop()
        except Exception:
            pass
        self._transport = None

async def _do_connect(self) -> None:
    """Establish new TCP connection, perform PQ handshake, create Transport."""
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(self._host, self._port, limit=4 * 1024 * 1024),
        timeout=self._timeout,
    )
    # TCP_NODELAY
    sock = writer.transport.get_extra_info("socket")
    if sock is not None:
        import socket
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    result = await asyncio.wait_for(
        perform_handshake(reader, writer, self._identity),
        timeout=self._timeout,
    )
    send_key, recv_key, send_counter, recv_counter, _ = result

    self._transport = Transport(
        reader, writer, send_key, recv_key, send_counter, recv_counter,
    )
    self._transport.start()
```

### Subscription Restoration
```python
async def _restore_subscriptions(self) -> None:
    """Re-subscribe to all namespaces tracked in _subscriptions."""
    for ns in list(self._subscriptions):
        try:
            payload = encode_subscribe([ns])
            await self._transport.send_message(
                TransportMsgType.Subscribe, payload
            )
        except Exception:
            log.warning("failed to re-subscribe to %s", ns.hex())
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Manual reconnect by caller | Auto-reconnect with state machine | Standard since gRPC, NATS, Redis clients ~2020 | Users expect transparent recovery |
| Fixed backoff intervals | Jittered exponential backoff (AWS "Full Jitter") | AWS blog 2015, universally adopted | Prevents thundering herd |
| Polling for reconnect status | asyncio.Event-based wait_connected() | Standard asyncio pattern | Non-blocking, efficient |
| Resume session | Full session rebuild | Always for AEAD protocols | Nonce-based encryption cannot resume safely |

## Open Questions

1. **on_disconnect timing relative to reconnect start**
   - What we know: D-04 says callback receives connection info. D-01 says enter reconnect mode on failure.
   - What's unclear: Does on_disconnect fire before or after the first reconnect attempt begins?
   - Recommendation: Fire on_disconnect immediately when entering DISCONNECTED state, before any backoff delay. This gives the app maximum time to prepare for the outage.

2. **Operations in flight when connection drops**
   - What we know: Transport._close_with_error() cancels all pending futures with ConnectionError.
   - What's unclear: Should the client automatically retry the in-flight operation after reconnect?
   - Recommendation: No. Let the pending futures fail with ConnectionError. The caller can use on_reconnect to retry. Auto-retry of writes is dangerous (double-write if the server processed but response was lost).

3. **Notification iterator behavior during reconnect**
   - What we know: `notifications()` loops on `transport.closed`. During reconnect, transport is closed.
   - What's unclear: Should the iterator yield a sentinel "reconnected" event or just resume silently?
   - Recommendation: Clear the notification queue on disconnect. After reconnect, the iterator resumes naturally because `transport.closed` becomes False on the new transport. The app uses on_reconnect callback for catch-up, not the notification stream.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest 9.0.2 with asyncio mode |
| Config file | `sdk/python/pyproject.toml` (test config section) |
| Quick run command | `pytest sdk/python/tests/test_reconnect.py -x -q` |
| Full suite command | `pytest sdk/python/tests/ -x -q` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CONN-03 | Auto-reconnect with jittered backoff on connection loss | unit | `pytest sdk/python/tests/test_reconnect.py::TestAutoReconnect -x` | Wave 0 |
| CONN-03 | Backoff delay calculation (1s base, 30s cap, jitter) | unit | `pytest sdk/python/tests/test_reconnect.py::TestBackoffDelay -x` | Wave 0 |
| CONN-03 | State machine transitions (disconnected -> connecting -> connected) | unit | `pytest sdk/python/tests/test_reconnect.py::TestConnectionState -x` | Wave 0 |
| CONN-04 | Subscription restoration after reconnect | unit | `pytest sdk/python/tests/test_reconnect.py::TestSubscriptionRestore -x` | Wave 0 |
| CONN-04 | Re-subscribe failure does not trigger disconnect | unit | `pytest sdk/python/tests/test_reconnect.py::TestSubscriptionRestore -x` | Wave 0 |
| CONN-05 | on_disconnect callback fires on connection loss | unit | `pytest sdk/python/tests/test_reconnect.py::TestCallbacks -x` | Wave 0 |
| CONN-05 | on_reconnect callback fires with attempt count and downtime | unit | `pytest sdk/python/tests/test_reconnect.py::TestCallbacks -x` | Wave 0 |
| CONN-05 | close() does not trigger reconnect | unit | `pytest sdk/python/tests/test_reconnect.py::TestCloseNoReconnect -x` | Wave 0 |

### Sampling Rate
- **Per task commit:** `pytest sdk/python/tests/test_reconnect.py -x -q`
- **Per wave merge:** `pytest sdk/python/tests/ -x -q`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `sdk/python/tests/test_reconnect.py` -- covers CONN-03, CONN-04, CONN-05
- [ ] Test helper: mock server that accepts connect, handshake, then drops connection on demand

### Test Approach Recommendation

Use the existing test pattern from `test_client.py`: in-memory stream pairs with `CaptureWriter` and `feed_encrypted_frame`. This avoids needing a real relay and tests the reconnect logic in isolation.

For reconnect testing specifically:
1. Create client with in-memory transport
2. Simulate connection loss by feeding EOF to reader / setting transport.closed
3. Mock `asyncio.open_connection` and `perform_handshake` to return new stream pairs
4. Verify state transitions, callback invocations, and subscription restoration
5. Use `asyncio.Event` to synchronize test assertions with background reconnect task

This approach is consistent with the existing 656-test suite and requires no infrastructure.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/client.py` -- ChromatinClient current architecture, connect/close lifecycle
- `sdk/python/chromatindb/_transport.py` -- Transport reader loop, error propagation, send_lock
- `sdk/python/chromatindb/_handshake.py` -- PQ handshake (must be re-run on each reconnect)
- `sdk/python/chromatindb/_framing.py` -- AEAD nonce counters (prove session cannot be resumed)
- `sdk/python/chromatindb/exceptions.py` -- Exception hierarchy (ConnectionError inherits ProtocolError)
- `sdk/python/tests/test_client.py` -- Existing test patterns (in-memory streams, CaptureWriter)
- Python 3.14 asyncio documentation -- Event, Task cancellation, create_task

### Secondary (MEDIUM confidence)
- [AWS Architecture Blog: Exponential Backoff And Jitter](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/) -- Full Jitter algorithm
- [websockets library reconnect pattern](https://websockets.readthedocs.io/en/stable/reference/asyncio/client.html) -- Verified reconnect-as-infinite-iterator pattern
- [PubNub asyncio reconnection policies](https://www.pubnub.com/docs/sdks/asyncio/reconnection-policies) -- 1s min / 32s max backoff reference

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all stdlib, no version concerns
- Architecture: HIGH -- all building blocks exist in codebase, patterns are well-established in asyncio ecosystem
- Pitfalls: HIGH -- derived from direct code analysis (AEAD nonces, Transport lifecycle, exception hierarchy)

**Research date:** 2026-04-04
**Valid until:** 2026-05-04 (stable domain, no fast-moving dependencies)
