---
phase: 98-ttl-enforcement
plan: 01
subsystem: database
tags: [ttl, expiry, overflow-safety, ingest-validation, codec]

# Dependency graph
requires:
  - phase: 96-peermanager-architecture
    provides: decomposed PeerManager components (message_dispatcher, blob_push_manager, sync_orchestrator)
provides:
  - saturating_expiry() and is_blob_expired() in wire namespace as single source of truth
  - Engine ingest validation for tombstone TTL and already-expired blobs
  - Overflow-safe expiry arithmetic across all production code paths
affects: [98-02, 98-03, ttl-enforcement, query-filtering, fetch-filtering]

# Tech tracking
tech-stack:
  added: []
  patterns: [saturating arithmetic for TTL overflow safety, Step 0d validation pattern]

key-files:
  created: []
  modified:
    - db/wire/codec.h
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/storage/storage.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/peer/blob_push_manager.cpp
    - db/peer/message_dispatcher.cpp
    - db/tests/wire/test_codec.cpp
    - db/tests/engine/test_engine.cpp
    - db/tests/sync/test_sync_protocol.cpp
    - db/tests/test_daemon.cpp

key-decisions:
  - "saturating_expiry returns 0 for ttl==0 (permanent), clamps UINT64_MAX on overflow"
  - "is_blob_expired uses <= comparison (expired at equality, per D-06)"
  - "SyncProtocol::is_blob_expired deleted, all callers migrated to wire::is_blob_expired"
  - "IngestError::invalid_ttl added for tombstone TTL validation"
  - "Already-expired check uses timestamp_rejected error (not a new error type)"

patterns-established:
  - "All expiry computation MUST use wire::saturating_expiry -- no raw timestamp+ttl arithmetic"
  - "Step 0d validation pattern: cheap integer checks after timestamp validation, before structural checks"

requirements-completed: [TTL-03]

# Metrics
duration: 79min
completed: 2026-04-08
---

# Phase 98 Plan 01: Core TTL Enforcement Summary

**Overflow-safe saturating_expiry/is_blob_expired functions in codec.h with engine ingest hardening for tombstone TTL and already-expired blob rejection**

## Performance

- **Duration:** 79 min
- **Started:** 2026-04-08T16:21:28Z
- **Completed:** 2026-04-08T17:40:03Z
- **Tasks:** 2
- **Files modified:** 12

## Accomplishments
- Added `saturating_expiry()` and `is_blob_expired()` as inline functions in `db/wire/codec.h` -- single source of truth for all expiry arithmetic
- Eliminated overflow-unsafe raw `timestamp + ttl` arithmetic from 6 call sites across storage, sync, blob_push_manager, and message_dispatcher
- Deleted `SyncProtocol::is_blob_expired` and migrated all callers to `wire::is_blob_expired`
- Engine now rejects tombstones with TTL > 0 via `IngestError::invalid_ttl`
- Engine now rejects already-expired blobs at ingest time
- 14 new test assertions across 14 new test cases covering all TTL enforcement behaviors

## Task Commits

Each task was committed atomically:

1. **Task 1: Core expiry functions in codec.h + storage/sync migration + unit tests** - `e583cf1` (feat)
2. **Task 2: Engine ingest validation -- tombstone TTL and already-expired rejection** - `2707335` (feat)
3. **Fix: daemon sync test for already-expired blob rejection** - `d205299` (fix)

## Files Created/Modified
- `db/wire/codec.h` - Added saturating_expiry() and is_blob_expired() inline functions
- `db/engine/engine.h` - Added IngestError::invalid_ttl enum value
- `db/engine/engine.cpp` - Added tombstone TTL check in delete_blob, already-expired check in ingest
- `db/storage/storage.cpp` - Replaced 2 raw expiry calculations with wire::saturating_expiry
- `db/sync/sync_protocol.h` - Deleted SyncProtocol::is_blob_expired declaration
- `db/sync/sync_protocol.cpp` - Deleted is_blob_expired implementation, migrated call site and expiry_time calculation
- `db/peer/blob_push_manager.cpp` - Replaced raw expiry calculation with wire::saturating_expiry
- `db/peer/message_dispatcher.cpp` - Replaced 2 raw expiry calculations with wire::saturating_expiry
- `db/tests/wire/test_codec.cpp` - 9 new test cases for saturating_expiry and is_blob_expired
- `db/tests/engine/test_engine.cpp` - 5 new test cases for TTL enforcement, fixed 29-day-old blob test
- `db/tests/sync/test_sync_protocol.cpp` - Migrated is_blob_expired calls to wire::is_blob_expired
- `db/tests/test_daemon.cpp` - Fixed expired-blob sync test to work with new ingest validation

## Decisions Made
- `saturating_expiry` returns 0 for ttl==0 (permanent sentinel), clamps to UINT64_MAX on overflow (effectively permanent) -- follows D-02, D-05 from research
- `is_blob_expired` uses `<=` comparison at boundary (expired at equality) per D-06
- Already-expired blob rejection uses existing `IngestError::timestamp_rejected` with detail "blob already expired" rather than a new error type
- Tombstone TTL validation uses new `IngestError::invalid_ttl` since it is a distinct semantic error

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed overflow-unsafe expiry in message_dispatcher.cpp**
- **Found during:** Task 1 (scanning for raw arithmetic after planned migrations)
- **Issue:** Two call sites in message_dispatcher.cpp used raw `static_cast<uint64_t>(blob.timestamp) + static_cast<uint64_t>(blob.ttl)` -- same overflow bug as storage.cpp
- **Fix:** Replaced both with `wire::saturating_expiry(blob.timestamp, blob.ttl)`
- **Files modified:** db/peer/message_dispatcher.cpp
- **Verification:** Build passes, no remaining raw arithmetic in production code
- **Committed in:** e583cf1 (Task 1 commit)

**2. [Rule 1 - Bug] Fixed existing test with pre-expired blob**
- **Found during:** Task 2 (full engine regression test)
- **Issue:** test_engine.cpp "29 days in past passes timestamp check" created a blob with TTL=604800 (7 days) at timestamp now-29d, making it expired 22 days ago. New already-expired check correctly rejects it.
- **Fix:** Changed TTL to 30*86400 (30 days) so blob at now-29d is not yet expired
- **Files modified:** db/tests/engine/test_engine.cpp
- **Verification:** All 76 engine tests pass
- **Committed in:** 2707335 (Task 2 commit)

**3. [Rule 1 - Bug] Fixed daemon expired-blob sync test**
- **Found during:** Overall verification (full test suite)
- **Issue:** test_daemon.cpp "expired blobs not synced" created a pre-expired blob (now-200, ttl=100) which engine now correctly rejects at ingest
- **Fix:** Changed to TTL=1 with 2-second sleep so blob is valid at ingest but expired by sync time
- **Files modified:** db/tests/test_daemon.cpp
- **Verification:** Daemon e2e test passes, expired blob correctly filtered during sync
- **Committed in:** d205299 (separate fix commit)

---

**Total deviations:** 3 auto-fixed (3 bugs)
**Impact on plan:** All auto-fixes necessary for correctness. No scope creep. The overflow bug in message_dispatcher was the same class as the planned storage.cpp fix -- just in additional call sites the plan missed. The test fixes are direct consequences of the new validation correctly catching previously-tolerated already-expired blobs.

## Issues Encountered
None -- all verification passed after auto-fixes.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- `saturating_expiry` and `is_blob_expired` are ready for Plans 02 and 03 to use in query handlers, fetch paths, and sync filtering
- Zero raw `timestamp + ttl` arithmetic remains in production code
- All existing tests (681 cases) pass with zero regressions

---
*Phase: 98-ttl-enforcement*
*Completed: 2026-04-08*
