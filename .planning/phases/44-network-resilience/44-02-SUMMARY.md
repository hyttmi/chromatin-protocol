---
phase: 44-network-resilience
plan: 02
subsystem: networking
tags: [inactivity, timeout, keepalive, dead-peer-detection, asio, coroutines]

# Dependency graph
requires:
  - phase: 44-network-resilience
    provides: cancel_all_timers pattern, timer-cancel coroutine pattern, PeerInfo struct
  - phase: 42-config-cleanup
    provides: validate_config error accumulation, known_keys set
provides:
  - Receiver-side inactivity timeout detecting and disconnecting dead peers
  - last_message_time tracking on every message for all connected peers
  - Configurable inactivity_timeout_seconds (default 120s, 0=disabled, min 30)
  - 30-second periodic sweep timer integrated into cancel_all_timers
affects: [integration-tests, v1.0.0-hardening]

# Tech tracking
tech-stack:
  added: []
  patterns: [receiver-side-inactivity-detection, top-of-handler-timestamp-update]

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/tests/config/test_config.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Receiver-side detection (not Ping sender) to avoid AEAD nonce desync"
  - "Timestamp update at very top of on_peer_message, before rate limiting, so all messages including rate-limited ones prevent false disconnects"
  - "Reuse bucket_last_refill timestamp for initial last_message_time (same steady_clock::now() call, no redundant clock read)"
  - "conn->close() not close_gracefully() for dead peers -- they cannot process goodbye messages"
  - "Collect to_close vector before iterating to avoid iterator invalidation from on_disconnect callback"

patterns-established:
  - "Top-of-handler timestamp: update tracking fields at the very top of message handlers before any dispatch or filtering"

requirements-completed: [CONN-03]

# Metrics
duration: 11min
completed: 2026-03-20
---

# Phase 44 Plan 02: Inactivity Timeout Summary

**Receiver-side inactivity detection with 30s sweep timer, configurable timeout (default 120s), and per-peer last_message_time tracking**

## Performance

- **Duration:** 11 min
- **Started:** 2026-03-20T06:31:02Z
- **Completed:** 2026-03-20T06:42:01Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- Config field inactivity_timeout_seconds with default 120s, validated as 0 (disabled) or >= 30
- PeerInfo.last_message_time tracked for all connected peers (initialized on connect, updated on every message)
- 30-second periodic sweep timer disconnects peers exceeding the timeout threshold
- Timer conditionally spawned only when enabled (non-zero config), integrated into cancel_all_timers
- 11 new tests covering config validation, JSON loading, PeerInfo defaults, connection tracking, and disabled mode

## Task Commits

Each task was committed atomically:

1. **Task 1: Add inactivity_timeout_seconds config field with validation** - `bbb0493` (feat)
2. **Task 2: Implement inactivity sweep timer and last_message_time tracking** - `bfbd2fe` (feat)

## Files Created/Modified
- `db/config/config.h` - Added inactivity_timeout_seconds field (default 120, 0=disabled, min 30)
- `db/config/config.cpp` - Config loading, known_keys entry, validate_config rule
- `db/peer/peer_manager.h` - last_message_time in PeerInfo, inactivity_timer_ member, inactivity_check_loop declaration
- `db/peer/peer_manager.cpp` - Inactivity sweep loop, last_message_time tracking in on_peer_message and on_peer_connected, cancel_all_timers update
- `db/tests/config/test_config.cpp` - 8 new tests for inactivity config field
- `db/tests/peer/test_peer_manager.cpp` - 3 new tests for inactivity peer tracking

## Decisions Made
- Receiver-side detection (not Ping sender) to avoid AEAD nonce desync. Adding a new wire message (Ping at application level) would require both sides to track nonce state. Instead, the receiver simply observes existing message traffic.
- Timestamp update placed at the very top of on_peer_message, before rate limiting check. This ensures that even rate-limited messages (which still indicate an alive peer) prevent false inactivity disconnects.
- Reused the bucket_last_refill timestamp for initial last_message_time to avoid a redundant steady_clock::now() call in on_peer_connected.
- Used conn->close() instead of close_gracefully() for inactivity disconnects. A dead peer cannot process goodbye messages, so sending one would just time out.
- Collect connections to close into a vector before closing them. The on_disconnect callback removes from peers_ deque, which would invalidate iterators during iteration.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Inactivity timeout completes Phase 44 (network resilience)
- All connection lifecycle timers now managed: reconnect (44-01), inactivity sweep (44-02)
- Ready for Phase 45 or v1.0.0 integration testing

## Self-Check: PASSED

All 6 modified files verified present. Both task commits (bbb0493, bfbd2fe) verified in git log. Build succeeds. 463/464 tests pass (excluding pre-existing PEX failure).

---
*Phase: 44-network-resilience*
*Completed: 2026-03-20*
