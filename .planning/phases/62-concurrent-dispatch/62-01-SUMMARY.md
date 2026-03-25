---
phase: 62-concurrent-dispatch
plan: 01
subsystem: networking
tags: [asio, coroutines, thread-pool, io-thread, AEAD, concurrent-dispatch]

# Dependency graph
requires:
  - phase: 61-request-id
    provides: request_id field in transport envelope for response correlation
provides:
  - IO-thread transfer in Data and Delete handlers after engine offload
  - Dispatch model documentation in on_peer_message
  - Concurrent pipelining tests (Data + ReadRequest)
affects: [62-concurrent-dispatch remaining plans, future SDK pipelining]

# Tech tracking
tech-stack:
  added: []
  patterns: [co_await asio::post(ioc_) transfer after thread pool offload]

key-files:
  created: []
  modified:
    - db/peer/peer_manager.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Client sends must be serialized within a single coroutine to avoid AEAD nonce desync"

patterns-established:
  - "IO-thread transfer: after any co_await that may resume on thread pool, add co_await asio::post(ioc_, asio::use_awaitable) before accessing IO-bound state"
  - "Dispatch model: inline for cheap ops, co_spawn for sync storage, co_spawn+offload+transfer for engine ops"

requirements-completed: [CONC-03, CONC-04]

# Metrics
duration: 40min
completed: 2026-03-25
---

# Phase 62 Plan 01: IO-Thread Transfer Summary

**IO-thread transfer for Data/Delete handlers after engine offload, preventing AEAD nonce desync and shared state races on concurrent requests**

## Performance

- **Duration:** 40 min
- **Started:** 2026-03-25T15:25:36Z
- **Completed:** 2026-03-25T16:06:14Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Fixed latent thread safety issue where Data and Delete handlers called send_message, notify_subscribers, metrics_, and record_strike from the thread pool thread after engine_.ingest()/delete_blob() resumed the coroutine on a pool thread
- Added dispatch model documentation classifying all message types by their execution pattern (inline, coroutine, coroutine+offload)
- Added concurrent pipelining tests proving both Data and ReadRequest pipelining works correctly with request_id correlation

## Task Commits

Each task was committed atomically:

1. **Task 1: Add IO-thread transfer to Data and Delete handlers** - `4294f0c` (feat)
2. **Task 2: Add concurrent pipelining tests and verify all tests pass** - `b4924d7` (test)

## Files Created/Modified
- `db/peer/peer_manager.cpp` - Added co_await asio::post(ioc_) after engine_.ingest() and engine_.delete_blob(), added dispatch model comment block
- `db/tests/peer/test_peer_manager.cpp` - Added two concurrent pipelining E2E tests (Data with request_ids 42/99, ReadRequest with request_ids 11/22)

## Decisions Made
- Client-side sends must be serialized within a single coroutine (not separate co_spawn calls) to prevent AEAD nonce desync from concurrent send_counter_ access

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed test client-side AEAD nonce desync**
- **Found during:** Task 2 (concurrent pipelining tests)
- **Issue:** Initial test implementation used two separate co_spawn calls for sending messages, which caused client-side AEAD nonce desync (two concurrent coroutines both calling send_message with interleaved send_counter_ increments)
- **Fix:** Refactored both tests to send messages sequentially within a single coroutine
- **Files modified:** db/tests/peer/test_peer_manager.cpp
- **Verification:** Both tests pass with correct request_id correlation
- **Committed in:** b4924d7 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Test-only fix. The plan's suggested pattern of separate co_spawn calls for sending would cause client-side nonce desync. Sequential sends within one coroutine is the correct pattern.

## Issues Encountered
- ASAN leak detection reports 88-176 byte leaks in liboqs OQS_SIG_new during thread pool shutdown -- known third-party issue, not a regression. Tests pass with ASAN_OPTIONS=detect_leaks=0.
- 13 pre-existing E2E test failures from port conflicts in test infrastructure (documented in STATE.md as known issue) -- not caused by this plan's changes.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- IO-thread safety established for all message handlers
- Concurrent pipelining verified for both write (Data) and read (ReadRequest) paths
- Ready for remaining 62-concurrent-dispatch plans (ExistsRequest, NodeInfo, PROTOCOL.md)

## Self-Check: PASSED

All files exist. All commits verified.

---
*Phase: 62-concurrent-dispatch*
*Completed: 2026-03-25*
