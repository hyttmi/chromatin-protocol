---
phase: 34-sync-resumption
plan: 02
subsystem: database
tags: [sync-cursor, peer-manager, metrics, sighup, libmdbx]

# Dependency graph
requires:
  - "34-01: SyncCursor CRUD API, cursor sub-database, config fields"
provides:
  - Cursor-aware sync orchestration in run_sync_with_peer and handle_sync_as_responder
  - cursor_hits, cursor_misses, full_resyncs metrics in NodeMetrics
  - SIGHUP round counter reset for forced full resync
  - Startup cursor cleanup via pubkey_hash in PersistedPeer
  - check_full_resync helper with periodic and time-gap triggers
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: [cursor skip in Phase C only (wire protocol unchanged), mutable config members for SIGHUP-reloadable fields]

key-files:
  created: []
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/tests/peer/test_peer_manager.cpp
    - db/tests/sync/test_sync_protocol.cpp

key-decisions:
  - "Wire protocol unchanged: cursor optimization is purely local Phase C skip (not partial hash exchange)"
  - "Mutable full_resync_interval_ and cursor_stale_seconds_ members follow existing rate_limit_ pattern"
  - "pubkey_hash hex string in PersistedPeer for startup cursor cleanup cross-reference"
  - "Cursor update only after successful sync completion (Pitfall 1 from research)"

patterns-established:
  - "Cursor skip at namespace level in Phase C: either full diff or skip entirely, no partial hash exchange"
  - "Full resync check: first pass scans for full resync triggers, second pass determines per-namespace cursor hits/misses"

requirements-completed: [SYNC-01, SYNC-02, SYNC-03, SYNC-04]

# Metrics
duration: 35min
completed: 2026-03-17
---

# Phase 34 Plan 02: Cursor-Aware Sync Orchestration Summary

**Cursor-based sync skip in Phase C with periodic/time-gap full resync, SIGHUP round reset, and cursor metrics**

## Performance

- **Duration:** 35 min
- **Started:** 2026-03-17T16:21:29Z
- **Completed:** 2026-03-17T16:56:31Z
- **Tasks:** 2 (both TDD)
- **Files modified:** 4

## Accomplishments
- run_sync_with_peer skips Phase C diff+transfer for cursor-hit namespaces (seq unchanged)
- handle_sync_as_responder applies same cursor logic independently
- Full resync triggered every Nth round (default 10) and after time gap (default 3600s)
- SIGHUP resets all round counters forcing full resync on next round
- Cursor mismatch (remote seq < stored) resets per-namespace cursor only
- cursor_hits, cursor_misses, full_resyncs visible in SIGUSR1 dump and periodic metrics
- Startup cleanup removes cursors for peers no longer in peers.json (via pubkey_hash field)
- 11 new tests, 337 total tests pass

## Task Commits

Each task was committed atomically (TDD):

1. **Task 1 RED: Failing tests** - `f770622` (test)
2. **Task 1 GREEN: Cursor-aware sync orchestration** - `a4c5089` (feat)
3. **Task 2: Cursor-based sync integration tests** - `b2f61ab` (test)

## Files Created/Modified
- `db/peer/peer_manager.h` - NodeMetrics cursor counters, PersistedPeer pubkey_hash, FullResyncReason enum, check_full_resync, mutable cursor config members
- `db/peer/peer_manager.cpp` - Cursor-aware Phase B/C in both sync functions, SIGHUP round reset, startup cleanup, pubkey_hash persistence, metrics dump extension
- `db/tests/peer/test_peer_manager.cpp` - 3 new tests: cursor metrics default init, reload_config round reset, PersistedPeer pubkey_hash
- `db/tests/sync/test_sync_protocol.cpp` - 8 new tests: cursor lifecycle, miss detection, full resync triggers, mismatch handling, restart persistence, round counter reset, stale cleanup

## Decisions Made
- Wire protocol unchanged: cursor optimization is purely a local Phase C skip. Both sides still send all HashLists. This is simpler and correct -- blob transfer is the dominant cost, not hash exchange.
- Mutable `full_resync_interval_` and `cursor_stale_seconds_` members follow the existing `rate_limit_bytes_per_sec_` pattern for SIGHUP-reloadable config.
- Added `pubkey_hash` field to PersistedPeer (hex string, empty = unknown) for startup cursor cleanup cross-reference. Non-breaking addition to peers.json.
- Full resync decision uses two passes: first pass checks if ANY namespace triggers full resync (breaks early), second pass determines per-namespace cursor hits/misses.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 34 complete: all four SYNC requirements shipped
- Cursor persistence (Plan 01) + cursor-aware sync (Plan 02) = full sync resumption
- Ready for next phase (quota enforcement or performance optimization)

## Self-Check: PASSED

All 4 files verified present. All 3 commits (f770622, a4c5089, b2f61ab) verified in git log.

---
*Phase: 34-sync-resumption*
*Completed: 2026-03-17*
