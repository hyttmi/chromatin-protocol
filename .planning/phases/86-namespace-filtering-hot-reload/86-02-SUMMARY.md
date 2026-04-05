---
phase: 86-namespace-filtering-hot-reload
plan: 02
subsystem: database
tags: [sighup, hot-reload, max-peers, peer-management, config]

# Dependency graph
requires:
  - phase: 82-reconcile-on-connect-safety-net
    provides: SIGHUP reload_config() pattern with safety_net_interval_seconds_
provides:
  - Mutable max_peers_ member with SIGHUP hot-reload support
  - Graceful over-limit handling (refuse new connections, no mass disconnect)
affects: [86-namespace-filtering-hot-reload, config-reload, peer-management]

# Tech tracking
tech-stack:
  added: []
  patterns: [SIGHUP-reloadable member with over-limit warning log]

key-files:
  created: []
  modified: [db/peer/peer_manager.h, db/peer/peer_manager.cpp]

key-decisions:
  - "Excess peers drain naturally on max_peers reduction (no active disconnect)"
  - "Warning log when node is over new max_peers limit after SIGHUP"

patterns-established:
  - "SIGHUP member reload with conditional over-limit warning: assign, compare, warn/info"

requirements-completed: [OPS-01]

# Metrics
duration: 56min
completed: 2026-04-05
---

# Phase 86 Plan 02: max_peers SIGHUP Hot Reload Summary

**max_peers becomes SIGHUP-reloadable via mutable member, with graceful over-limit drain and warning log**

## Performance

- **Duration:** 56 min (mostly CMake FetchContent + build in fresh worktree)
- **Started:** 2026-04-05T09:12:16Z
- **Completed:** 2026-04-05T10:08:18Z
- **Tasks:** 1
- **Files modified:** 2

## Accomplishments
- Added mutable max_peers_ member to PeerManager, initialized from config in constructor
- Replaced all config_.max_peers references with max_peers_ in should_accept_connection() and handle_peer_list_response()
- Added max_peers reload to reload_config() with over-limit warning (peers drain naturally, no mass disconnect)
- Zero references to config_.max_peers remain in peer_manager.cpp

## Task Commits

Each task was committed atomically:

1. **Task 1: Add mutable max_peers_ member and wire up SIGHUP reload** - `d42ec63` (feat)

## Files Created/Modified
- `db/peer/peer_manager.h` - Added uint32_t max_peers_ = 32 member (SIGHUP-reloadable)
- `db/peer/peer_manager.cpp` - Constructor init, should_accept_connection, handle_peer_list_response, reload_config all use max_peers_

## Decisions Made
None - followed plan as specified. Decisions D-12 and D-13 from CONTEXT.md implemented exactly.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Fresh worktree required full CMake FetchContent download + build (~20 min for dependencies, ~10 min for compilation). Not a code issue.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- max_peers is now SIGHUP-reloadable alongside other config members
- Plan 01 (namespace filtering) and Plan 03 (integration tests) can proceed independently
- All existing tests pass (peer-manager: 3 test cases, reload: 5 test cases)

## Self-Check: PASSED

- All files exist (peer_manager.h, peer_manager.cpp, 86-02-SUMMARY.md)
- Commit d42ec63 found in git log
- Zero references to config_.max_peers in peer_manager.cpp
- 7 references to max_peers_ in peer_manager.cpp (init, accept, reload x4, PEX)
- 1 "excess will drain naturally" warning line present
- 1 uint32_t max_peers_ member in peer_manager.h

---
*Phase: 86-namespace-filtering-hot-reload*
*Completed: 2026-04-05*
