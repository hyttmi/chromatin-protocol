---
phase: 40-sync-rate-limiting
plan: 02
subsystem: peer
tags: [rate-limiting, sync, token-bucket, cooldown, session-limit]

# Dependency graph
requires:
  - phase: 40-sync-rate-limiting/01
    provides: "SyncRejected=30, config fields, PeerManager helpers, reason constants"
provides:
  - "Sync cooldown enforcement on inbound SyncRequest"
  - "Session limit enforcement on concurrent sync"
  - "Universal byte accounting for all message types"
  - "Initiator namespace-boundary budget cutoff"
  - "Responder silent budget cutoff"
  - "Initiator SyncRejected handling"
  - "Integration tests for RATE-01, RATE-02, RATE-03"
affects: [sync, peer-manager, rate-limiting]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Universal byte accounting at top of on_peer_message (before dispatch)"
    - "Cooldown check uses steady_clock ms with last_sync_initiated field"
    - "Differentiated bucket-exhausted behavior: Data/Delete disconnect, SyncRequest reject, others route"

key-files:
  created: []
  modified:
    - db/peer/peer_manager.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Universal byte accounting at top of on_peer_message, not per-type"
  - "Data/Delete disconnect preserved (Phase 18 behavior), SyncRequest rejects with SyncRejected, other sync messages route through for mid-sync cutoff at namespace boundary"
  - "Cooldown tests use closed mode (ACL) to avoid PEX 5s timeout inflating sync cycle beyond cooldown window"

patterns-established:
  - "Closed mode in tests to eliminate PEX timeout interference with sync timing"
  - "sync_cooldown_seconds=0 in existing tests that need rapid re-sync"

requirements-completed: [RATE-01, RATE-02, RATE-03]

# Metrics
duration: 48min
completed: 2026-03-19
---

# Phase 40 Plan 02: Sync Rate Limiting Enforcement Summary

**Sync cooldown, session limit, and universal byte accounting enforcement with 4 new integration tests covering RATE-01/02/03**

## Performance

- **Duration:** 48 min
- **Started:** 2026-03-19T12:30:06Z
- **Completed:** 2026-03-19T13:18:47Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Universal byte accounting at top of on_peer_message: all message types consume token bucket
- Cooldown check rejects SyncRequest within sync_cooldown_seconds with SyncRejected(0x01)
- Session limit check rejects concurrent sync with SyncRejected(0x02)
- Byte rate check rejects SyncRequest when bucket exhausted with SyncRejected(0x03)
- Data/Delete disconnect on exceed preserved (Phase 18 backward compat)
- Initiator byte budget check at namespace boundary stops new namespaces when bucket_tokens==0
- Responder silent cutoff when byte budget exhausted (initiator times out)
- Initiator handles SyncRejected with reason logging
- 4 new integration tests, 1 updated test, 408 total tests passing

## Task Commits

Each task was committed atomically:

1. **Task 1: Enforcement logic** - `b1a7f19` (feat)
2. **Task 2: Integration tests** - `c6a2673` (test)

## Files Created/Modified
- `db/peer/peer_manager.cpp` - Universal byte accounting, cooldown/session/byte-rate enforcement, initiator budget check, responder silent cutoff, SyncRejected handling, SyncRejected routing
- `db/tests/peer/test_peer_manager.cpp` - 4 new test cases (cooldown, cooldown-disabled, concurrent session, byte accounting), 1 updated test (sync traffic counted), existing tests updated with sync_cooldown_seconds=0

## Decisions Made
- Universal byte accounting placed at top of on_peer_message before any dispatch, not inline with each handler. This ensures all messages are metered uniformly and the old Data/Delete-only check could be cleanly removed.
- Differentiated response on bucket exhaustion: Data/Delete still disconnect (Phase 18), SyncRequest sends SyncRejected(byte_rate), other sync messages route through (mid-sync cutoff at namespace boundary). Control messages (Subscribe, StorageFull, etc.) also route through -- they're lightweight.
- Cooldown tests use closed mode (both nodes in each other's allowed_keys) to skip PEX exchange, which has a 5-second timeout that inflates the sync cycle beyond the cooldown window. This is a test technique, not a production change.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Existing tests failed due to default sync_cooldown_seconds=30**
- **Found during:** Task 1
- **Issue:** Three existing tests (tombstone notification, tombstone propagation, namespace filter) needed multiple sync rounds with short intervals (2-3s) but the new default 30s cooldown rejected subsequent SyncRequests
- **Fix:** Added `sync_cooldown_seconds = 0` to all affected test configs to disable cooldown for tests that need rapid re-sync
- **Files modified:** db/tests/peer/test_peer_manager.cpp
- **Verification:** All 3 previously failing tests pass
- **Committed in:** b1a7f19 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Necessary fix for backward compatibility of existing tests with new cooldown enforcement.

## Issues Encountered
- PEX exchange after sync has a 5-second timeout that inflates the sync cycle from ~3s to ~8s, making cooldown testing unreliable in open mode. Resolved by using closed mode in cooldown tests (which skips PEX). This is a pre-existing timing characteristic, not a bug introduced by this plan.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Phase 40 sync rate limiting is complete (both plans executed)
- All 12 v0.8.0 requirements addressed across phases 38-40
- 408 tests passing, no regressions

---
*Phase: 40-sync-rate-limiting*
*Completed: 2026-03-19*
