---
phase: 99-sync-resource-concurrency-correctness
plan: 02
subsystem: database
tags: [resource-limits, storage, subscription, quota, bootstrap, mdbx, toctou]

# Dependency graph
requires:
  - phase: 96-peermanager-architecture
    provides: decomposed PeerManager with MessageDispatcher, ConnectionManager, Storage components
provides:
  - Per-connection subscription limit with configurable max (default 256)
  - Atomic capacity/quota check inside store_blob write transaction (eliminates TOCTOU)
  - Fixed bootstrap peer detection using full host:port endpoint
  - Fixed quota rebuild iterator that clears all entries correctly
affects: [99-sync-resource-concurrency-correctness]

# Tech tracking
tech-stack:
  added: []
  patterns: [atomic-check-in-write-transaction, erase-restart-from-first-iterator-pattern]

key-files:
  created: []
  modified:
    - db/peer/message_dispatcher.h
    - db/peer/message_dispatcher.cpp
    - db/peer/connection_manager.cpp
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/engine/engine.cpp
    - db/config/config.h
    - db/config/config.cpp
    - db/peer/peer_manager.cpp
    - db/tests/storage/test_storage.cpp
    - db/tests/engine/test_engine.cpp

key-decisions:
  - "Subscription limit sends QuotaExceeded rejection (reuse existing message type, no new wire type)"
  - "Atomic capacity/quota check via new 6-param store_blob overload; 3-param delegates with zeros"
  - "Engine Steps 0b/2a retained as fast-reject optimizations before expensive crypto"
  - "Quota rebuild uses erase-current-restart-from-first pattern (safe for MDBX cursor invalidation)"

patterns-established:
  - "Atomic-in-transaction: resource limit checks inside MDBX write transaction for TOCTOU safety"
  - "Fast-reject + authoritative: cheap pre-checks before crypto, authoritative recheck in transaction"

requirements-completed: [RES-01, RES-02, RES-03, RES-04]

# Metrics
duration: 43min
completed: 2026-04-09
---

# Phase 99 Plan 02: Resource Limit Enforcement Summary

**Four resource limit bugs fixed: subscription limit (256/conn with QuotaExceeded rejection), full endpoint bootstrap detection, atomic TOCTOU-safe capacity/quota in store_blob, and quota rebuild iterator**

## Performance

- **Duration:** 43 min
- **Started:** 2026-04-09T02:37:31Z
- **Completed:** 2026-04-09T03:20:31Z
- **Tasks:** 2
- **Files modified:** 11

## Accomplishments
- Per-connection subscription limit (default 256) with QuotaExceeded rejection message, configurable and SIGHUP-reloadable
- Bootstrap peer detection now compares full host:port endpoint instead of just host
- store_blob performs atomic capacity/quota check inside MDBX write transaction, eliminating TOCTOU race
- Quota rebuild clear loop correctly erases all entries using erase-restart-from-first pattern
- 5 new tests: 4 storage [resource] + 1 engine [resource], all passing

## Task Commits

Each task was committed atomically:

1. **Task 1: Fix subscription limit (RES-01), bootstrap detection (RES-02), and quota rebuild iterator (RES-04)** - `7ec3178` (fix)
2. **Task 2: Atomic capacity/quota check in store_blob (RES-03) with tests** - `650ddf6` (fix)

## Files Created/Modified
- `db/config/config.h` - Added max_subscriptions_per_connection field (default 256)
- `db/config/config.cpp` - JSON parsing and known_keys for new config field
- `db/peer/message_dispatcher.h` - max_subscriptions_ member and set_max_subscriptions() setter
- `db/peer/message_dispatcher.cpp` - Subscribe handler limit check with QuotaExceeded rejection via co_spawn
- `db/peer/connection_manager.cpp` - Bootstrap detection using full endpoint string comparison
- `db/peer/peer_manager.cpp` - Passes subscription limit to dispatcher on init and SIGHUP reload
- `db/storage/storage.h` - CapacityExceeded/QuotaExceeded status values, new 6-param store_blob overload
- `db/storage/storage.cpp` - Atomic capacity/quota checks inside write txn, fixed quota rebuild iterator
- `db/engine/engine.cpp` - Passes limits to store_blob at Step 4, handles new status codes
- `db/tests/storage/test_storage.cpp` - 4 new [resource] tests for atomic checks and rebuild
- `db/tests/engine/test_engine.cpp` - 1 new [resource] test for end-to-end capacity rejection

## Decisions Made
- Reused existing QuotaExceeded message type for subscription limit rejection (D-08) -- no new wire types needed
- 3-param store_blob delegates to 6-param with zeros for backward compatibility
- Engine Steps 0b (capacity) and 2a (quota) retained as fast-reject optimizations; store_blob is the authoritative check
- Quota rebuild uses erase-current + restart-from-first pattern instead of erase-returns-next (safer for MDBX cursor semantics)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- All four resource limit bugs fixed with targeted tests
- 177 storage/engine tests pass (1032 assertions)
- Ready for plan 03 (coroutine counter safety)

## Self-Check: PASSED

All 11 modified files verified on disk. Both task commits (7ec3178, 650ddf6) verified in git log. SUMMARY.md exists.

---
*Phase: 99-sync-resource-concurrency-correctness*
*Completed: 2026-04-09*
