---
phase: 82-reconcile-on-connect-safety-net
plan: 01
subsystem: config, peer
tags: [config-rename, safety-net, sighup, timer]

# Dependency graph
requires:
  - phase: 79-send-queue-push-notifications
    provides: Push-based sync as primary mechanism (safety net is backstop)
  - phase: 80-targeted-blob-fetch
    provides: BlobFetch/BlobNotify as primary sync path
provides:
  - safety_net_interval_seconds config field (default 600s, minimum 60s)
  - SIGHUP-reloadable safety-net timer interval via mutable PeerManager member
  - sync_timer_loop reads mutable member for SIGHUP reactivity
affects: [82-reconcile-on-connect-safety-net]

# Tech tracking
tech-stack:
  added: []
  patterns: [mutable-member-for-sighup-reload]

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/main.cpp
    - db/tests/config/test_config.cpp
    - db/tests/sync/test_sync_protocol.cpp
    - db/tests/peer/test_peer_manager.cpp
    - db/tests/test_daemon.cpp

key-decisions:
  - "Validation minimum is 60s (not 1s) -- safety-net shorter than 60s defeats backstop purpose"
  - "Deploy production configs set to 600s; fastsync/integration configs keep small values for fast tests"

patterns-established:
  - "Mutable member pattern for SIGHUP-reloadable timer intervals (safety_net_interval_seconds_)"

requirements-completed: [MAINT-06, MAINT-07]

# Metrics
duration: 23min
completed: 2026-04-04
---

# Phase 82 Plan 01: Safety-Net Timer Rename Summary

**Renamed sync_interval_seconds to safety_net_interval_seconds (default 600s) with SIGHUP-reloadable mutable member and minimum 60s validation**

## Performance

- **Duration:** 23 min
- **Started:** 2026-04-04T06:11:34Z
- **Completed:** 2026-04-04T06:34:30Z
- **Tasks:** 2
- **Files modified:** 31

## Accomplishments
- Renamed config field from sync_interval_seconds to safety_net_interval_seconds with 600s default
- Validation minimum raised from 1 to 60 (safety-net backstop interval must be meaningful)
- PeerManager sync_timer_loop reads mutable member (not const config ref) for SIGHUP reactivity
- All 594 tests pass (2646 assertions), zero stale references in source code

## Task Commits

Each task was committed atomically:

1. **Task 1: Rename config field and update validation** - `af0adf0` (feat)
2. **Task 2: Update PeerManager safety-net timer + SIGHUP reload + all test/config references** - `d9300d7` (feat)

## Files Created/Modified
- `db/config/config.h` - Renamed field with new default 600
- `db/config/config.cpp` - Updated parsing, known_keys, validation (min 60)
- `db/main.cpp` - Updated startup log line
- `db/peer/peer_manager.h` - Added mutable safety_net_interval_seconds_ member
- `db/peer/peer_manager.cpp` - Constructor init, timer reads mutable member, reload_config updates it
- `db/tests/config/test_config.cpp` - Updated validation tests with boundary checks (0, 59, 60)
- `db/tests/sync/test_sync_protocol.cpp` - Updated defaults and JSON parsing tests
- `db/tests/peer/test_peer_manager.cpp` - 58 references renamed
- `db/tests/test_daemon.cpp` - 7 references renamed
- 22 JSON config files in deploy/configs/ and tests/integration/configs/

## Decisions Made
- Validation minimum is 60s (not 1s) -- a safety-net interval shorter than 60s defeats the purpose of being a long-interval backstop
- Deploy production configs changed from 10s to 600s; fastsync configs keep value 2 for fast integration testing
- Integration test configs keep small values (5s) since they bypass validate_config()

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Config rename complete, sync_timer_loop now operates as safety-net backstop
- Plan 82-02 (reconcile-on-connect with cursor grace period) can proceed

## Self-Check: PASSED

- All key source files verified present
- Both task commits (af0adf0, d9300d7) verified in git log
- Zero stale sync_interval_seconds references in source code

---
*Phase: 82-reconcile-on-connect-safety-net*
*Completed: 2026-04-04*
