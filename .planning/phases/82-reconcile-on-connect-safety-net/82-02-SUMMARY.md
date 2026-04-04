---
phase: 82-reconcile-on-connect-safety-net
plan: 02
subsystem: peer
tags: [cursor-grace-period, sync-on-connect, safety-net, mdbx-cursors]

# Dependency graph
requires:
  - phase: 82-reconcile-on-connect-safety-net
    plan: 01
    provides: safety_net_interval_seconds config, mutable member, sync_timer_loop
  - phase: 80-targeted-blob-fetch
    provides: BlobFetch/BlobNotify push-then-pull loop
provides:
  - DisconnectedPeerState tracking in PeerManager
  - Cursor grace period (5 min) for reconnecting peers
  - Stale disconnected entry cleanup with MDBX cursor deletion in safety-net cycle
  - cursor_compaction_loop awareness of grace-period peers
affects: [82-reconcile-on-connect-safety-net]

# Tech tracking
tech-stack:
  added: []
  patterns: [disconnect-timestamp-tracking, grace-period-cursor-preservation]

key-files:
  created: []
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Cursors are NOT deleted when grace period expires on reconnect -- check_full_resync() already handles stale detection via cursor_stale_seconds_"
  - "Safety-net cycle (600s) handles stale entry cleanup + MDBX cursor deletion, not a per-peer timer"
  - "cursor_compaction_loop includes grace-period peers in known set to prevent premature cursor deletion"

patterns-established:
  - "DisconnectedPeerState disconnect timestamp tracking for cursor grace period"

requirements-completed: [MAINT-04, MAINT-05]

# Metrics
duration: 38min
completed: 2026-04-04
---

# Phase 82 Plan 02: Cursor Grace Period & Sync-on-Connect Summary

**DisconnectedPeerState tracking with 5-minute cursor grace period, stale cleanup in safety-net cycle, and cursor compaction awareness**

## Performance

- **Duration:** 38 min
- **Started:** 2026-04-04T06:37:43Z
- **Completed:** 2026-04-04T07:15:50Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 3

## Accomplishments
- Added DisconnectedPeerState struct and disconnected_peers_ map to PeerManager
- on_peer_disconnected records steady_clock disconnect timestamp
- on_peer_connected checks grace period, logs cursor preservation or expiry, removes entry
- sync_timer_loop cleans stale disconnected entries after sync_all_peers and deletes MDBX cursors
- cursor_compaction_loop includes recently-disconnected peers in known set
- CURSOR_GRACE_PERIOD_MS = 300,000 ms (5 minutes)
- 3 new tests verify sync-on-connect, cursor reuse on reconnect, and multi-reconnect stability
- Full test suite: 597 tests, 2661 assertions, all passing

## Task Commits

Each task was committed atomically (TDD):

1. **Task 1 RED: Add failing tests** - `9804b75` (test)
2. **Task 1 GREEN: Implement grace period logic** - `92f5068` (feat)

## Files Created/Modified
- `db/peer/peer_manager.h` - DisconnectedPeerState struct, disconnected_peers_ map, CURSOR_GRACE_PERIOD_MS constant
- `db/peer/peer_manager.cpp` - Grace period logic in on_peer_connected, on_peer_disconnected, sync_timer_loop, cursor_compaction_loop (7 references to disconnected_peers_)
- `db/tests/peer/test_peer_manager.cpp` - 3 new tests tagged [peer-manager][safety-net]

## Decisions Made
- Cursors are NOT deleted when grace period expires on reconnect -- the existing check_full_resync() in run_sync_with_peer() already handles stale detection via cursor_stale_seconds_, so no extra cursor deletion is needed
- Safety-net cycle (600s) handles stale entry cleanup + MDBX cursor deletion rather than per-peer timers -- simpler and naturally exceeds cooldown
- cursor_compaction_loop includes grace-period peers in known set to prevent the 6-hour compaction from prematurely deleting cursors for briefly-disconnected peers

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required

None - no external service configuration required.

## Known Stubs

None - all functionality is fully wired.

## Next Phase Readiness
- Phase 82 is complete (both plans shipped)
- All MAINT-04, MAINT-05, MAINT-06, MAINT-07 requirements satisfied
- Ready for Phase 83 (Bidirectional Keepalive) or parallel phases

## Self-Check: PASSED

- All key source files verified present (peer_manager.h, peer_manager.cpp, test_peer_manager.cpp)
- Both task commits (9804b75, 92f5068) verified in git log
- 597 tests pass with 2661 assertions (zero regressions)

---
*Phase: 82-reconcile-on-connect-safety-net*
*Completed: 2026-04-04*
