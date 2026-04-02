---
phase: 79-send-queue-push-notifications
plan: 02
subsystem: networking
tags: [asio, coroutines, send-queue, aead, nonce-serialization, connection]

# Dependency graph
requires:
  - phase: none
    provides: existing Connection class with send_message/send_encrypted
provides:
  - per-connection send queue serializing all outbound messages
  - drain coroutine as single AEAD nonce writer
  - awaitable send_message with backpressure
  - bounded queue (1024) with disconnect policy
  - Pong and Goodbye routed through queue
affects: [79-send-queue-push-notifications, 80-push-sync-loop, 82-reconcile-on-connect]

# Tech tracking
tech-stack:
  added: []
  patterns: [send-queue-drain-coroutine, timer-cancel-wakeup, awaitable-operators-concurrent-coroutines]

key-files:
  created: []
  modified:
    - db/net/connection.h
    - db/net/connection.cpp
    - db/tests/net/test_connection.cpp

key-decisions:
  - "close_gracefully() enqueues Goodbye then sets closing_ flag -- ordering prevents self-rejection"
  - "drain coroutine started via awaitable_operators (&&) alongside message_loop in run()"
  - "PendingMessage uses raw pointer to caller's stack-local timer (safe: single io_context thread)"

patterns-established:
  - "Send queue pattern: all post-handshake sends go through enqueue_send -> drain_send_queue -> send_encrypted"
  - "Timer-cancel wakeup for drain coroutine (consistent with existing sync_inbox pattern)"
  - "awaitable_operators && for concurrent coroutines with shared shutdown"

requirements-completed: [PUSH-04]

# Metrics
duration: 37min
completed: 2026-04-02
---

# Phase 79 Plan 02: Send Queue Summary

**Per-connection send queue with drain coroutine serializing all outbound messages to prevent AEAD nonce desync**

## Performance

- **Duration:** 37 min
- **Started:** 2026-04-02T11:35:53Z
- **Completed:** 2026-04-02T12:13:35Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- All outbound post-handshake messages serialize through a single drain coroutine, making AEAD nonce desync structurally impossible
- send_message() remains fully awaitable with backpressure -- callers know when their write completes
- Bounded queue at 1024 messages with disconnect policy prevents memory exhaustion from slow peers
- Pong replies and Goodbye now route through the queue (fixing existing latent race conditions)
- All 579 existing tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement per-connection send queue with drain coroutine** - `dde9610` (feat)
2. **Task 2: Full test suite regression check** - verification only, no code changes

## Files Created/Modified
- `db/net/connection.h` - Added PendingMessage struct, send_queue_ deque, send_signal_ timer, drain_send_queue/enqueue_send declarations, closing_ flag, MAX_SEND_QUEUE constant
- `db/net/connection.cpp` - Rewrote send_message to enqueue, implemented enqueue_send with awaitable backpressure, implemented drain_send_queue coroutine, fixed Pong to use send_message, fixed close_gracefully to queue Goodbye, added send_signal cancel to close(), run() uses awaitable_operators for concurrent coroutines
- `db/tests/net/test_connection.cpp` - Added 3 new tests: concurrent sends without crash, send after close returns false, Pong through send queue

## Decisions Made
- close_gracefully() manually enqueues Goodbye and sets closing_ AFTER the message enters the queue, avoiding the self-rejection bug where closing_=true would cause enqueue_send to reject the Goodbye itself
- drain coroutine launched via `co_await (message_loop() && drain_send_queue())` using awaitable_operators -- both exit cleanly when close() sets closed_ and cancels send_signal_
- PendingMessage stores raw pointers to caller's stack-local completion timer and result bool -- safe because all coroutines run on the same io_context thread (no data race)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed close_gracefully() Goodbye self-rejection**
- **Found during:** Task 1 (Implementation)
- **Issue:** Plan specified setting closing_=true before send_message(Goodbye), but enqueue_send checks closing_ at entry and would reject the Goodbye message itself, preventing graceful disconnects
- **Fix:** Changed close_gracefully() to manually construct the PendingMessage, push it to queue, then set closing_=true, then await completion -- ensuring Goodbye enters the queue before the rejection flag is set
- **Files modified:** db/net/connection.cpp
- **Verification:** "Connection goodbye sends properly" test passes
- **Committed in:** dde9610 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix)
**Impact on plan:** Essential correctness fix. The plan's suggested ordering would have broken graceful disconnects.

## Issues Encountered
None beyond the deviation documented above.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functionality is fully wired.

## Next Phase Readiness
- Send queue is the safety foundation for all subsequent push notification work
- BlobNotify fan-out (plan 79-03+) can now safely use co_spawn(detached) with send_message since all sends serialize through the queue
- All 579 tests green, no blockers

## Self-Check: PASSED

- FOUND: db/net/connection.h
- FOUND: db/net/connection.cpp
- FOUND: db/tests/net/test_connection.cpp
- FOUND: 79-02-SUMMARY.md
- FOUND: commit dde9610

---
*Phase: 79-send-queue-push-notifications*
*Completed: 2026-04-02*
