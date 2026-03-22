---
phase: 17-operational-stability
plan: 01
subsystem: networking
tags: [asio, signal-handling, coroutine-cancellation, shutdown, graceful-drain]

# Dependency graph
requires:
  - phase: 16-storage-foundation
    provides: "Storage::run_expiry_scan(), used_bytes(), StorageFull signaling"
provides:
  - "Server::set_on_shutdown() pre-drain callback for PeerManager"
  - "Re-arming signal handler with clean second-signal force shutdown (no std::_Exit)"
  - "Server::exit_code() accessor (0=clean, 1=forced/timeout)"
  - "PeerManager::expiry_scan_loop() cancellable member coroutine"
  - "PeerManager::exit_code() forwarding exit code to main"
affects: [17-02, 17-03, peer-persistence, metrics]

# Tech tracking
tech-stack:
  added: []
  patterns: [pre-drain-callback, re-arming-signal-handler, timer-cancel-expiry]

key-files:
  created: []
  modified:
    - db/net/server.h
    - db/net/server.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/main.cpp

key-decisions:
  - "Timer-cancel pattern (pointer to stack timer) over cancellation_signal for expiry scan -- matches existing sync_notify pattern"
  - "on_shutdown callback registered after server_.start() to ensure signal handler is armed first"
  - "Exit code propagated Server -> PeerManager -> main.cpp via accessor chain"

patterns-established:
  - "Pre-drain callback: Server::set_on_shutdown() fires before drain starts, PeerManager saves state"
  - "Re-arming signal handler: arm_signal_handler() re-arms after first signal for second-signal handling"
  - "Expiry scan as member coroutine with timer-cancel: avoids lambda stack-use-after-return and enables immediate cancellation"

requirements-completed: [OPS-01, OPS-02]

# Metrics
duration: 4min
completed: 2026-03-10
---

# Phase 17 Plan 01: Graceful Shutdown + Cancellable Expiry Summary

**SIGTERM triggers PeerManager peer save before Server drain, re-arming signal handler replaces std::_Exit with clean force-close, and expiry scan moved to cancellable PeerManager member coroutine with exit code propagation**

## Performance

- **Duration:** 4 min
- **Started:** 2026-03-10T16:21:23Z
- **Completed:** 2026-03-10T16:26:09Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Server now has a pre-drain shutdown callback that PeerManager uses to save peers before connections close
- Second SIGTERM force-closes all connections via ioc_.stop() instead of std::_Exit(1) -- destructors run, spdlog flushes
- Expiry scan migrated from main.cpp lambda (detached, no cancel path) to PeerManager::expiry_scan_loop() member coroutine with timer-cancel pattern for immediate cancellation
- Exit code 0 (clean) / 1 (forced/timeout) propagated from Server through PeerManager to main() return value

## Task Commits

Each task was committed atomically:

1. **Task 1: Server shutdown callback + re-arming signal handler + exit code** - `3c3ded6` (feat)
2. **Task 2: PeerManager shutdown orchestration + cancellable expiry coroutine + exit code propagation** - `bfe55d1` (feat)

## Files Created/Modified
- `db/net/server.h` - Added ShutdownCallback typedef, set_on_shutdown(), exit_code(), arm_signal_handler(), on_shutdown_ and exit_code_ members
- `db/net/server.cpp` - Replaced one-shot signal handler with re-arming arm_signal_handler(), added on_shutdown_ invocation in stop() before drain, exit_code_ set on timeout/force
- `db/peer/peer_manager.h` - Added exit_code() accessor, expiry_scan_loop() declaration, expiry_timer_ pointer member
- `db/peer/peer_manager.cpp` - Registered on_shutdown callback in start(), added expiry_timer_ cancel in stop(), implemented expiry_scan_loop() with timer-cancel pattern, added exit_code() forwarding
- `db/main.cpp` - Removed expiry scanner lambda, changed return to pm.exit_code()

## Decisions Made
- Timer-cancel pattern (raw pointer to stack-allocated timer) chosen over asio::cancellation_signal for expiry scan -- matches existing sync_notify pattern already proven in this codebase, simpler with no cancellation_state management required
- on_shutdown callback registered after server_.start() to ensure signal handler is armed first, preventing a timing window where shutdown callback exists but signal handling isn't ready
- Exit code propagated via accessor chain (Server::exit_code() -> PeerManager::exit_code() -> main return) rather than out-parameter, keeping the API clean

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Server shutdown callback infrastructure ready for 17-02 (peer persistence periodic flush will use same on_shutdown pattern)
- PeerManager::expiry_scan_loop() establishes the timer-cancel pattern that 17-02 peer_flush_timer_loop and 17-03 metrics_timer_loop will follow
- All 271 existing tests pass -- no regressions

## Self-Check: PASSED

All files exist, all commits verified, summary created.

---
*Phase: 17-operational-stability*
*Completed: 2026-03-10*
