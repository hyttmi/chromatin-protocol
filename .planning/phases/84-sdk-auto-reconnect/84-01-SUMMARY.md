---
phase: 84-sdk-auto-reconnect
plan: 01
subsystem: sdk
tags: [asyncio, reconnect, state-machine, backoff, python]

# Dependency graph
requires:
  - phase: 83-bidirectional-keepalive
    provides: "Keepalive ping/pong for dead connection detection"
provides:
  - "ConnectionState enum (4 states) for connection lifecycle tracking"
  - "Auto-reconnect loop with jittered exponential backoff (1s base, 30s cap)"
  - "Subscription restoration after reconnect"
  - "on_disconnect/on_reconnect callback hooks"
  - "wait_connected() awaitable"
  - "Reconnect-surviving notifications() generator"
affects: [84-02-tests]

# Tech tracking
tech-stack:
  added: []
  patterns: ["State machine enum for connection lifecycle", "AWS Full Jitter backoff", "Safe callback invocation (sync+async)"]

key-files:
  created:
    - sdk/python/chromatindb/_reconnect.py
  modified:
    - sdk/python/chromatindb/client.py
    - sdk/python/chromatindb/__init__.py

key-decisions:
  - "Connection monitor polls transport.closed every 0.5s rather than hooking into transport error path"
  - "on_disconnect fires before reconnect loop starts (maximum prep time for app)"
  - "Notification queue on old transport is abandoned (not drained) on reconnect"
  - "Pending operations fail with ConnectionError during reconnect (no queuing)"

patterns-established:
  - "State machine: ConnectionState enum with DISCONNECTED/CONNECTING/CONNECTED/CLOSING"
  - "Reconnect primitives in _reconnect.py, orchestration in client.py"
  - "invoke_callback() handles sync/async callbacks safely with exception suppression"

requirements-completed: [CONN-03, CONN-04, CONN-05]

# Metrics
duration: 4min
completed: 2026-04-04
---

# Phase 84 Plan 01: SDK Auto-Reconnect Summary

**State machine, jittered backoff reconnect loop, subscription restore, and app callbacks in ChromatinClient -- zero new dependencies, zero regressions across 479 tests**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-04T17:07:46Z
- **Completed:** 2026-04-04T17:12:06Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- New `_reconnect.py` module with ConnectionState enum, backoff_delay (AWS Full Jitter), invoke_callback helper, and OnDisconnect/OnReconnect type aliases
- ChromatinClient refactored with full state machine (DISCONNECTED -> CONNECTING -> CONNECTED, CLOSING terminal), background connection monitor, reconnect loop, subscription restoration, and wait_connected() awaitable
- ConnectionState exported from chromatindb public API
- All 479 existing SDK tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Create reconnect primitives module** - `cc07a6e` (feat)
2. **Task 2: Refactor ChromatinClient with auto-reconnect** - `3779cc8` (feat)

## Files Created/Modified
- `sdk/python/chromatindb/_reconnect.py` - ConnectionState enum, backoff_delay, invoke_callback, type aliases
- `sdk/python/chromatindb/client.py` - State machine, reconnect loop, monitor, subscription restore, wait_connected, connection_state property
- `sdk/python/chromatindb/__init__.py` - Export ConnectionState in public API

## Decisions Made
- Connection monitor polls transport.closed every 0.5s -- simpler than hooking into transport error path, and detect latency is bounded
- on_disconnect fires before reconnect loop starts so app has maximum preparation time
- Old transport notification queue is abandoned on reconnect (not drained) -- app uses on_reconnect callback for catch-up
- Pending operations fail with ConnectionError during reconnect -- no queuing (matches gRPC/Redis/NATS pattern, avoids unbounded memory)
- Auto-reconnect only activates after first successful connection (initial connect failure raises immediately)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plan 02 (tests) can now be implemented: all reconnect primitives and client methods are in place
- Test patterns from research (in-memory streams, mocked open_connection/perform_handshake) can verify state transitions, callbacks, and subscription restoration

## Self-Check: PASSED

All files verified present, all commit hashes found in git log.

---
*Phase: 84-sdk-auto-reconnect*
*Completed: 2026-04-04*
