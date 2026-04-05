# Phase 89: SDK Multi-Relay Failover - Research

**Researched:** 2026-04-05
**Domain:** Python SDK connection management / multi-endpoint failover
**Confidence:** HIGH

## Summary

Phase 89 adds multi-relay failover to the Python SDK. The core change replaces the single `(host, port)` connection target with an ordered list of relay addresses. On connection failure (both initial and during auto-reconnect), the SDK cycles through the relay list before applying circuit-breaker backoff between full cycles.

This is a contained modification to 2 source files (`client.py`, `_reconnect.py`) plus their tests. The existing auto-reconnect infrastructure from Phase 84 provides the backoff, state machine, and callback patterns -- this phase extends them to support relay rotation. No new dependencies, no protocol changes, no C++ modifications.

**Primary recommendation:** Modify `connect()` to accept `relays: list[tuple[str, int]]`, replace `_host`/`_port` with `_relays` list + `_relay_index` integer, restructure `_reconnect_loop()` to cycle through relays within each backoff cycle, extend `on_reconnect` callback signature with relay info, and update all existing tests and docs that call `connect(host, port, ...)`.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Replace `host`/`port` params with `relays: list[tuple[str, int]]`. Single-relay usage: `relays=[("host", 4201)]`. No backward compat shim -- pre-MVP, clean break.
- **D-02:** `relays` is a required parameter. First relay in the list is the preferred/primary relay. List order defines priority.
- **D-03:** Ordered rotation through the relay list. On connection failure, try the next relay in list order. After the last relay, wrap back to the first.
- **D-04:** Circuit-breaker after full cycle: reuse existing jittered exponential backoff (1s base, 30s cap) from `_reconnect.py:backoff_delay()`. Attempt counter tracks full cycles through the relay list, not individual relay attempts. Same pattern as Phase 84 single-relay reconnect.
- **D-05:** Individual relay attempts within a cycle have no inter-attempt delay (try next relay immediately after failure). The backoff only applies between full cycles.
- **D-06:** No health tracking or failure memory. Always start from the top of the relay list each cycle. Simple, predictable, no stale state.
- **D-07:** Extend existing `on_reconnect` callback signature to include relay info: `on_reconnect(attempt_count, downtime_seconds, relay_host, relay_port)`. No new callback type.
- **D-08:** Expose a `current_relay` property on ChromatinClient returning `(host, port)` tuple.

### Claude's Discretion
- Internal relay index tracking (simple integer index into the list)
- Whether `_do_connect` takes relay params or reads from internal state
- Jitter amount on per-relay connect timeout within a cycle
- Test strategy: mock multiple relays, test rotation and circuit-breaker
- Whether to add a `connected_relay` field to the on_reconnect callback info or just pass positional args
- Log messages for relay rotation events

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| SDK-01 | SDK connect() accepts a list of relay addresses and rotates to next on connection failure | D-01/D-02: `relays` parameter replaces `host`/`port`. D-03: ordered rotation. D-06: no health tracking. Architecture pattern: `_relays` list + `_relay_index`. |
| SDK-02 | SDK auto-reconnect tries next relay in the list when current relay is unreachable | D-03/D-04/D-05: cycle through list, backoff between full cycles, no inter-attempt delay within cycle. Reuse `backoff_delay()` from `_reconnect.py`. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| asyncio | stdlib | Async connection management | Already used throughout SDK |
| pytest | 9.0.2 | Test framework | Already installed in SDK venv |
| unittest.mock | stdlib | Mock relay connections | Already used in test_reconnect.py |

### Supporting
No new dependencies. All changes use existing stdlib and project infrastructure.

## Architecture Patterns

### Current State (to be modified)

```
ChromatinClient
  _host: str          -> becomes _relays: list[tuple[str, int]]
  _port: int          -> becomes _relay_index: int
  connect(host, port) -> connect(relays=[(host, port)])
```

### Target State

```python
class ChromatinClient:
    _relays: list[tuple[str, int]]   # Ordered relay list (D-02)
    _relay_index: int                 # Current position in relay list

    @classmethod
    def connect(cls, relays: list[tuple[str, int]], identity: Identity, ...) -> ChromatinClient:
        ...

    @property
    def current_relay(self) -> tuple[str, int]:  # D-08
        return self._relays[self._relay_index]
```

### Pattern 1: Relay Rotation in Reconnect Loop
**What:** The `_reconnect_loop()` restructured to cycle through relays, with backoff between full cycles.
**When to use:** On any connection loss detected by `_connection_monitor()`.

The reconnect loop becomes a nested structure:
- Outer loop: full cycles through the relay list (tracks `cycle_count` for backoff)
- Inner loop: iterate through each relay in order (no delay between relays)
- Backoff applied at the START of each new full cycle (not between individual relay attempts)

```python
async def _reconnect_loop(self) -> None:
    cycle_count = 0
    disconnect_time = time.monotonic()
    try:
        while self._state == ConnectionState.DISCONNECTED:
            cycle_count += 1

            # Backoff between full cycles (D-04)
            if cycle_count > 1:
                delay = backoff_delay(cycle_count - 1, base=1.0, cap=30.0)
                log.debug("relay cycle %d, backoff %.2fs", cycle_count, delay)
                await asyncio.sleep(delay)

            if self._state == ConnectionState.CLOSING:
                return

            # Try each relay in order (D-03, D-05: no inter-attempt delay)
            for i, (host, port) in enumerate(self._relays):
                if self._state == ConnectionState.CLOSING:
                    return

                self._relay_index = i
                self._state = ConnectionState.CONNECTING
                try:
                    await self._do_connect(host, port)
                    # Success
                    self._state = ConnectionState.CONNECTED
                    self._connected_event.set()
                    log.info(
                        "reconnected to %s:%d after %d cycles (%.1fs downtime)",
                        host, port, cycle_count,
                        time.monotonic() - disconnect_time,
                    )
                    await self._restore_subscriptions()
                    await invoke_callback(
                        self._on_reconnect,
                        cycle_count,
                        time.monotonic() - disconnect_time,
                        host,
                        port,
                    )
                    # Restart monitor, return
                    ...
                    return
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    log.debug("relay %s:%d failed: %s", host, port, exc)
                    self._state = ConnectionState.DISCONNECTED

            # Full cycle exhausted, loop back to top for backoff
    except asyncio.CancelledError:
        return
    finally:
        self._reconnect_task = None
```

### Pattern 2: _do_connect with Relay Parameters
**What:** `_do_connect()` takes `(host, port)` as parameters instead of reading `self._host`/`self._port`.
**Why:** The reconnect loop decides which relay to try. Clean separation of concerns.

```python
async def _do_connect(self, host: str, port: int) -> None:
    """Establish TCP + PQ handshake to specific relay."""
    if self._transport is not None:
        try:
            await self._transport.stop()
        except Exception:
            pass
        self._transport = None

    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(host, port, limit=4 * 1024 * 1024),
        timeout=self._timeout,
    )
    # ... TCP_NODELAY, handshake, create transport ...
```

### Pattern 3: Initial connect() in __aenter__
**What:** `__aenter__` also cycles through the relay list for initial connection (SDK-01).
**Why:** First connection failure on relay[0] should try relay[1], etc. Same rotation logic. But NO backoff on initial connect -- if all relays fail, raise immediately (matches Phase 84: "Initial connect failure raises immediately").

```python
async def __aenter__(self) -> ChromatinClient:
    # Try each relay in order for initial connection (D-03)
    last_exc: Exception | None = None
    for i, (host, port) in enumerate(self._relays):
        self._relay_index = i
        try:
            # TCP connect + handshake
            ...
            return self
        except (HandshakeError, asyncio.TimeoutError, OSError) as exc:
            last_exc = exc
            log.debug("initial connect to %s:%d failed: %s", host, port, exc)
            continue

    # All relays failed -- raise last error (no auto-reconnect for initial)
    raise last_exc  # or wrap in HandshakeError
```

### Pattern 4: OnReconnect Callback Signature Extension
**What:** Extend `OnReconnect` from `(int, float)` to `(int, float, str, int)`.
**Why:** D-07 requires `on_reconnect(attempt_count, downtime_seconds, relay_host, relay_port)`.

```python
# _reconnect.py
OnReconnect = Callable[[int, float, str, int], Awaitable[None] | None]
# on_reconnect(cycle_count, downtime_seconds, relay_host, relay_port)
```

Note: `invoke_callback` already accepts `*args` so it handles the extended signature without changes.

### Anti-Patterns to Avoid
- **Storing health/failure state per relay:** D-06 explicitly says no health tracking. Always start from the top of the list each cycle.
- **Adding delay between individual relay attempts:** D-05 says try next relay immediately after failure. Only backoff between full cycles.
- **Backward compat shim for old connect(host, port):** D-01 says clean break, pre-MVP.
- **Separate connect timeout per relay:** Use the single `timeout` parameter for each relay attempt. Total initial connect time is bounded by `timeout * len(relays)`.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Backoff calculation | Custom delay formula | Existing `backoff_delay()` in `_reconnect.py` | D-04: reuse directly. Already tested (6 tests). |
| Connection state machine | New state types | Existing `ConnectionState` enum | 4 states sufficient. No new states needed for multi-relay. |
| Safe callback dispatch | Try/except in reconnect loop | Existing `invoke_callback()` | Already handles sync/async/exception. Just pass extra args. |

## Common Pitfalls

### Pitfall 1: Attempt counter semantics
**What goes wrong:** Counting individual relay attempts instead of full cycles for backoff. With 3 relays and per-attempt backoff, the delay escalates 3x faster than intended.
**Why it happens:** The existing `_reconnect_loop` increments `attempt` per individual try.
**How to avoid:** D-04 is explicit: "Attempt counter tracks full cycles through the relay list, not individual relay attempts." Rename the counter to `cycle_count` to make this clear.
**Warning signs:** Backoff delay growing too fast in tests; reaching cap in fewer cycles than expected.

### Pitfall 2: Initial connect not rotating
**What goes wrong:** `__aenter__` only tries the first relay and raises on failure, even though relay[1] might be reachable.
**Why it happens:** Existing `__aenter__` was built for single relay.
**How to avoid:** Apply the same rotation in `__aenter__`. If all relays fail, raise the last exception. No backoff on initial connect (raise immediately after exhausting list).
**Warning signs:** Initial connect raises even though secondary relay is up.

### Pitfall 3: on_reconnect callback backward incompatibility
**What goes wrong:** Existing callbacks that accept `(attempt_count, downtime_seconds)` break when called with 4 args.
**Why it happens:** Extending positional args is a breaking signature change.
**How to avoid:** D-01 says pre-MVP, clean break. All existing test callbacks need updating. However, `invoke_callback` already catches exceptions, so a signature mismatch would be caught -- but the callback would silently fail. Update ALL test callbacks AND document the new signature.
**Warning signs:** on_reconnect callbacks silently swallowed by invoke_callback exception handler.

### Pitfall 4: _relay_index drift during reconnect
**What goes wrong:** `current_relay` property returns wrong relay if `_relay_index` is updated mid-reconnect while user code reads it.
**Why it happens:** `_relay_index` mutates during the reconnect loop's relay iteration.
**How to avoid:** `_relay_index` is only meaningful when `_state == CONNECTED`. The `current_relay` property should be documented as reflecting the relay the client last connected to (or is currently attempting). Since the event loop is single-threaded, there's no true race condition, but the semantic should be clear.
**Warning signs:** `current_relay` returning a relay the client never successfully connected to.

### Pitfall 5: Forgetting to update all connect() call sites
**What goes wrong:** Tests or docs still use `connect(host, port, identity)` which now requires `connect(relays=[(host, port)], identity=identity)`.
**Why it happens:** 40+ call sites across test_reconnect.py, test_client.py, test_integration.py, README.md, getting-started.md.
**How to avoid:** Systematic grep for `ChromatinClient.connect(` and update every occurrence. The new signature with `relays` as first positional arg will cause immediate TypeError on old calls.
**Warning signs:** Test failures from TypeError on connect().

## Code Examples

### Full connect() classmethod signature change

```python
@classmethod
def connect(
    cls,
    relays: list[tuple[str, int]],
    identity: Identity,
    *,
    timeout: float = 10.0,
    auto_reconnect: bool = True,
    on_disconnect: OnDisconnect | None = None,
    on_reconnect: OnReconnect | None = None,
) -> ChromatinClient:
    """Create a connection context manager.

    Args:
        relays: Ordered list of (host, port) relay addresses (SDK-01).
            First relay is preferred/primary. List order defines priority.
        identity: Client ML-DSA-87 identity.
        timeout: Per-relay handshake timeout in seconds (default 10s).
        auto_reconnect: If True, transparently reconnect on connection loss.
        on_disconnect: Callback invoked when connection is lost.
        on_reconnect: Callback invoked after successful reconnect,
            receives (cycle_count, downtime_seconds, relay_host, relay_port).

    Usage:
        async with ChromatinClient.connect(
            [("relay1.example.com", 4201), ("relay2.example.com", 4201)],
            identity,
        ) as conn:
            await conn.ping()
    """
    if not relays:
        raise ValueError("relays must be a non-empty list of (host, port) tuples")
    client = cls.__new__(cls)
    client._relays = list(relays)
    client._relay_index = 0
    client._identity = identity
    # ... rest of init ...
    return client
```

### current_relay property

```python
@property
def current_relay(self) -> tuple[str, int]:
    """Currently connected (or last attempted) relay address (D-08)."""
    return self._relays[self._relay_index]
```

### Test pattern: mock multi-relay rotation

```python
async def test_relay_rotation_on_failure(self) -> None:
    """When first relay fails, try second relay (SDK-01)."""
    identity = Identity.generate()
    client = ChromatinClient.connect(
        relays=[("relay1", 4201), ("relay2", 4202), ("relay3", 4203)],
        identity=identity,
        auto_reconnect=True,
    )
    # Inject connected state
    # ...

    connect_hosts = []

    async def mock_do_connect(host, port):
        connect_hosts.append((host, port))
        if host == "relay1":
            raise ConnectionError("relay1 down")
        if host == "relay2":
            raise ConnectionError("relay2 down")
        # relay3 succeeds
        new_transport = MagicMock()
        new_transport.closed = False
        # ... mock setup ...
        client._transport = new_transport

    with patch.object(client, "_do_connect", side_effect=mock_do_connect):
        with patch("chromatindb.client.backoff_delay", return_value=0):
            client._on_connection_lost()
            await asyncio.wait_for(client._connected_event.wait(), timeout=2.0)
            await asyncio.sleep(0.05)

    assert connect_hosts == [("relay1", 4201), ("relay2", 4202), ("relay3", 4203)]
    assert client.current_relay == ("relay3", 4203)
```

## Affected Files Inventory

### Source files to modify
| File | Changes |
|------|---------|
| `sdk/python/chromatindb/client.py` | `connect()` signature, `__aenter__`, `_reconnect_loop`, `_do_connect`, `__init__`, new `current_relay` property, remove `_host`/`_port` |
| `sdk/python/chromatindb/_reconnect.py` | `OnReconnect` type alias: `Callable[[int, float, str, int], ...]` |

### Test files to modify
| File | Changes |
|------|---------|
| `sdk/python/tests/test_reconnect.py` | All `connect(host, port, ...)` -> `connect(relays=[(host, port)], ...)`, on_reconnect callbacks accept 4 args, new multi-relay tests |
| `sdk/python/tests/test_client.py` | `connect("127.0.0.1", 9999, identity)` -> `connect(relays=[("127.0.0.1", 9999)], identity=identity)` |
| `sdk/python/tests/test_integration.py` | All ~25 `connect(RELAY_HOST, RELAY_PORT, identity)` calls -> `connect(relays=[(RELAY_HOST, RELAY_PORT)], identity=identity)` |

### Documentation files to update
| File | Changes |
|------|---------|
| `sdk/python/chromatindb/client.py` | Module docstring, connect() docstring |
| `sdk/python/README.md` | Usage examples |
| `sdk/python/docs/getting-started.md` | ~12 connect() call sites |

### Call site count
- `test_reconnect.py`: 9 connect() calls
- `test_client.py`: 1 connect() call
- `test_integration.py`: ~25 connect() calls
- `README.md`: 2 connect() calls
- `getting-started.md`: ~12 connect() calls
- `client.py` docstrings: 2 connect() examples
- **Total: ~51 call sites to update**

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest 9.0.2 |
| Config file | `sdk/python/pyproject.toml` (pytest section) |
| Quick run command | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q` |
| Full suite command | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/ -x -q --ignore=sdk/python/tests/test_integration.py` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SDK-01 | connect() accepts relay list, rotates on initial failure | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "multi_relay or relay_list or initial"` | New tests needed (Wave 0) |
| SDK-01 | current_relay property returns correct relay | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "current_relay"` | New tests needed (Wave 0) |
| SDK-02 | auto-reconnect cycles through relay list | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "rotation or cycle"` | New tests needed (Wave 0) |
| SDK-02 | circuit-breaker backoff between full cycles | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "circuit_breaker or backoff_cycle"` | New tests needed (Wave 0) |
| SDK-02 | on_reconnect callback receives relay info | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "reconnect_callback_relay"` | New tests needed (Wave 0) |

### Sampling Rate
- **Per task commit:** `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q`
- **Per wave merge:** `sdk/python/.venv/bin/python -m pytest sdk/python/tests/ -x -q --ignore=sdk/python/tests/test_integration.py`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] New test class `TestMultiRelayConnect` in `test_reconnect.py` -- covers SDK-01 (relay list, initial rotation, current_relay)
- [ ] New test class `TestMultiRelayReconnect` in `test_reconnect.py` -- covers SDK-02 (rotation on failure, circuit-breaker backoff, no inter-attempt delay)
- [ ] Updated `TestReconnectLoop`, `TestCallbacks` etc. for new connect() signature and 4-arg on_reconnect
- [ ] No new framework install needed -- pytest 9.0.2 already available

## Open Questions

1. **Attempt counter semantic in on_reconnect callback**
   - What we know: D-04 says "attempt counter tracks full cycles." D-07 says `on_reconnect(attempt_count, ...)`.
   - What's unclear: Is `attempt_count` now the cycle count (1 = first full cycle through all relays) or the traditional attempt count (1 = first retry regardless)?
   - Recommendation: Use cycle count, since D-04 explicitly redefines the counter. The existing single-relay behavior is unchanged (1 relay = 1 attempt per cycle). Document this clearly in the callback docstring.

2. **Initial connect jitter**
   - What we know: D-05 says no delay between relay attempts within a cycle.
   - What's unclear: Does this also apply to initial connect in `__aenter__`? (Presumably yes -- try all relays fast, fail fast.)
   - Recommendation: Yes, no delay. Initial connect should cycle through all relays immediately. Total worst-case wait is `timeout * len(relays)`.

## Sources

### Primary (HIGH confidence)
- `sdk/python/chromatindb/client.py` -- Current implementation, 1068 lines, all connect/reconnect logic
- `sdk/python/chromatindb/_reconnect.py` -- ConnectionState, backoff_delay, OnReconnect type, invoke_callback
- `sdk/python/tests/test_reconnect.py` -- 31 existing reconnect tests, established mock patterns
- `sdk/python/tests/test_integration.py` -- 25 connect() call sites against live relay
- `.planning/phases/89-sdk-multi-relay-failover/89-CONTEXT.md` -- All 8 locked decisions
- `.planning/phases/84-sdk-auto-reconnect/84-CONTEXT.md` -- Original auto-reconnect design

### Secondary (MEDIUM confidence)
- None needed -- all research is against existing codebase

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing stdlib
- Architecture: HIGH -- straightforward extension of proven Phase 84 patterns, all code inspected
- Pitfalls: HIGH -- derived from direct code inspection of 51 call sites and reconnect loop logic

**Research date:** 2026-04-05
**Valid until:** 2026-05-05 (stable -- internal SDK, no external dependency changes)
