---
phase: 88-relay-resilience
plan: 02
subsystem: relay
tags: [asio, coroutines, reconnect, backoff, uds, state-machine]

# Dependency graph
requires:
  - phase: 88-relay-resilience plan 01
    provides: per-client subscription tracking (NamespaceSet, Subscribe/Unsubscribe interception, Notification filtering)
provides:
  - Three-state session lifecycle (ACTIVE/RECONNECTING/DEAD)
  - UDS auto-reconnect with jittered exponential backoff
  - Subscription replay after successful reconnect
  - Message drop during RECONNECTING/replay
affects: [relay-integration-tests, sdk-reconnect-transparency]

# Tech tracking
tech-stack:
  added: []
  patterns: [three-state lifecycle state machine, jittered exponential backoff, subscription replay on reconnect, wire_node_handlers extraction for reuse]

key-files:
  created: []
  modified:
    - relay/core/relay_session.h
    - relay/core/relay_session.cpp
    - db/tests/relay/test_relay_session.cpp

key-decisions:
  - "wire_node_handlers extracted from start() on_ready for reuse in reconnect on_ready"
  - "replay_pending_ flag blocks all forwarding during subscription replay, not just Subscribe messages"
  - "state_ = DEAD set in teardown for consistent lifecycle regardless of entry path"

patterns-established:
  - "Three-state lifecycle: ACTIVE/RECONNECTING/DEAD governs message routing and reconnection"
  - "Jittered exponential backoff: full jitter uniform [0, min(cap, base*2^attempt)] with overflow protection"
  - "Subscription replay on reconnect: single batch encode_namespace_list before resuming ACTIVE"

requirements-completed: [RELAY-01, RELAY-02]

# Metrics
duration: 36min
completed: 2026-04-05
---

# Phase 88 Plan 02: UDS Auto-Reconnect Summary

**Three-state relay session lifecycle with jittered backoff UDS reconnection and subscription replay after reconnect**

## Performance

- **Duration:** 36 min
- **Started:** 2026-04-05T16:32:35Z
- **Completed:** 2026-04-05T17:08:05Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Implemented three-state lifecycle (ACTIVE/RECONNECTING/DEAD) replacing instant teardown on UDS loss
- Added reconnect_loop coroutine with jittered exponential backoff (1s base, 30s cap, 10 max attempts)
- Added subscription replay after successful UDS reconnect via encode_namespace_list batch
- Added replay_pending_ flag to block forwarding during subscription replay
- Extracted wire_node_handlers() for reuse between initial start() and reconnect on_ready
- Added 5 new unit tests for state machine, backoff bounds, and replay logic

## Task Commits

Each task was committed atomically:

1. **Task 1: Add three-state lifecycle and UDS reconnect loop** - `1738a52` (feat)
2. **Task 2: Unit tests for state machine, backoff bounds, and replay logic** - `c30e994` (test)

## Files Created/Modified
- `relay/core/relay_session.h` - SessionState enum, reconnect constants, jittered_backoff, reconnect_loop, wire_node_handlers declarations, state_/replay_pending_/rng_ members
- `relay/core/relay_session.cpp` - reconnect_loop coroutine, jittered_backoff implementation, wire_node_handlers extraction, handle_node_close state machine, state gate in handle_client_message, state_ = DEAD in teardown
- `db/tests/relay/test_relay_session.cpp` - 5 new test cases: SessionState enum values, reconnect constants, backoff formula bounds, subscription replay encoding, empty set replay skip

## Decisions Made
- Extracted wire_node_handlers() from start() on_ready into a reusable method, called from both initial start() and reconnect on_ready
- replay_pending_ flag blocks ALL forwarding (not just Subscribe), consistent with D-14 spec
- state_ = DEAD added to teardown() for consistent lifecycle regardless of entry path (stop, blocked message, client disconnect, reconnect exhaustion)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Build directory needed to be configured from scratch in worktree with pre-fetched dependencies (FetchContent can't clone from GitHub in isolated environment). Resolved by pointing FETCHCONTENT_SOURCE_DIR_* to main repo's cached sources.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Relay now survives node UDS disconnections with automatic reconnection
- Client TCP sessions remain open during reconnection attempts
- Subscriptions are replayed after successful reconnect
- Ready for integration testing (Docker E2E) to verify end-to-end behavior

## Self-Check: PASSED

All files exist, all commits verified.

---
*Phase: 88-relay-resilience*
*Completed: 2026-04-05*
