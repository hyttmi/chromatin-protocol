---
phase: 81-event-driven-expiry
plan: 02
subsystem: database
tags: [asio, coroutines, timer, expiry, event-driven, steady_timer]

# Dependency graph
requires:
  - phase: 81-event-driven-expiry/81-01
    provides: Storage::get_earliest_expiry() for timer rearm queries
provides:
  - Event-driven expiry_scan_loop replacing periodic 60s scan
  - OnBlobIngested callback with expiry_time parameter for timer rearm
  - PeerManager unit tests for event-driven expiry behavior
affects: [82-reconcile-on-connect, 83-cursor-cleanup, 85-documentation-refresh]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Event-driven timer: wall-clock target -> steady_timer duration with underflow guard"
    - "Coroutine lifecycle flag (expiry_loop_running_) prevents double co_spawn"
    - "Storage cursor as source of truth for next expiry (no in-memory queue)"

key-files:
  created:
    - db/tests/peer/test_event_expiry.cpp
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/config/config.h
    - db/CMakeLists.txt

key-decisions:
  - "Move on_blob_ingested to public API for test access (Rule 3: tests need to trigger rearm)"
  - "Storage injectable clock for deterministic expiry tests with real Asio timers"
  - "expiry_scan_interval_seconds deprecated but kept for config file compatibility"

patterns-established:
  - "Event-driven timer pattern: target -> duration -> fire -> scan -> rearm from storage"
  - "Callback signature extension: add parameter, update all call sites, compute at each site"

requirements-completed: [MAINT-01, MAINT-02, MAINT-03]

# Metrics
duration: 88min
completed: 2026-04-03
---

# Phase 81 Plan 02: Event-Driven Expiry Timer Summary

**Replaced periodic 60s expiry scan with event-driven timer that fires at exact blob expiry time, chain-rearms via storage cursor, and rearms on shorter-TTL ingest**

## Performance

- **Duration:** 88 min
- **Started:** 2026-04-03T14:17:33Z
- **Completed:** 2026-04-03T15:45:23Z
- **Tasks:** 3
- **Files modified:** 7

## Accomplishments
- OnBlobIngested callback extended with expiry_time parameter across all 6 call sites (sync, delete, data, blob-fetch-response, lambda wiring)
- expiry_scan_loop() rewritten from periodic 60s interval to event-driven: sleeps until exact next-expiry wall-clock time, chain-rearms via get_earliest_expiry()
- Ingest-triggered rearm in on_blob_ingested() cancels/spawns timer when shorter-TTL blob arrives
- 5 unit tests covering timer-fires-at-expiry (MAINT-01), chain-rearm (MAINT-02), ingest-rearm (MAINT-03), empty-storage, TTL-0-only edge cases
- Startup queries get_earliest_expiry() for initial timer arm; no timer when no expiring blobs exist

## Task Commits

Each task was committed atomically:

1. **Task 1: Update callback signatures and all call sites** - `e2a0e6d` (feat)
2. **Task 2: Rewrite expiry_scan_loop and startup to event-driven timing** - `af39d08` (feat)
3. **Task 3: Create PeerManager event-driven expiry unit tests** - `aeb0547` (test)

## Files Created/Modified
- `db/sync/sync_protocol.h` - OnBlobIngested typedef with uint64_t expiry_time parameter
- `db/sync/sync_protocol.cpp` - Compute and pass expiry_time at sync ingest callback
- `db/peer/peer_manager.h` - on_blob_ingested moved to public, next_expiry_target_ and expiry_loop_running_ members added
- `db/peer/peer_manager.cpp` - Rewritten expiry_scan_loop (event-driven), updated start() (initial arm), updated on_blob_ingested (rearm logic), updated all 4 direct call sites
- `db/config/config.h` - Deprecated expiry_scan_interval_seconds comment
- `db/tests/peer/test_event_expiry.cpp` - 5 [event-expiry] test cases
- `db/CMakeLists.txt` - Registered test_event_expiry.cpp in test sources

## Decisions Made
- Moved on_blob_ingested from private to public in PeerManager to enable test access for ingest-rearm test (MAINT-03). Logically a notification API, not an internal detail.
- Used real Asio timers with injectable Storage clock for deterministic tests. Storage clock controls when run_expiry_scan considers blobs expired; Asio steady_timer controls coroutine wake timing.
- Kept expiry_scan_interval_seconds_ member variable despite deprecation -- still loaded from config and written by SIGHUP reload. Removing would break compilation for no benefit.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Moved on_blob_ingested to public API**
- **Found during:** Task 3 (unit test creation)
- **Issue:** on_blob_ingested was in the private section of PeerManager, but Test 3 (ingest rearm) needs to call it directly to simulate peer-replicated blob arrival
- **Fix:** Moved declaration from private to public section in peer_manager.h
- **Files modified:** db/peer/peer_manager.h
- **Verification:** Tests compile and pass, no other code changes needed
- **Committed in:** aeb0547 (Task 3 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Essential for test coverage. Method is logically a public notification API already called from sync callback. No scope creep.

## Issues Encountered
- Build required cmake reconfiguration without ccache (not available in worktree environment)
- Peer tests take >60s due to network operations, verified via storage/sync/event-expiry suites instead

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Event-driven expiry system complete and tested
- Phase 81 plans 01 + 02 together deliver D-01 through D-06 and MAINT-01 through MAINT-03
- Ready for Phase 82 (reconcile-on-connect) and Phase 83 (cursor cleanup) which can proceed independently

## Self-Check: PASSED

- All 8 files verified present
- All 3 commit hashes verified in git log
- 5 event-expiry tests pass, 96 storage tests pass, 214 combined tests pass

---
*Phase: 81-event-driven-expiry*
*Completed: 2026-04-03*
