---
phase: 54-operational-hardening
plan: 02
subsystem: engine
tags: [timestamp, validation, protocol, sync-reject, step-0]

# Dependency graph
requires:
  - phase: 54-01
    provides: sync_reject.h with SYNC_REJECT_TIMESTAMP_REJECTED constant
provides:
  - Step 0c timestamp validation in BlobEngine (ingest + delete_blob)
  - IngestError::timestamp_rejected enum value
  - PROTOCOL.md documentation of all 8 sync rejection codes
  - Timestamp Validation section in PROTOCOL.md
affects: [55-runtime-compaction, 56-local-access]

# Tech tracking
tech-stack:
  added: []
  patterns: [Step 0c timestamp validation before crypto]

key-files:
  created: []
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/peer_manager.cpp
    - db/sync/sync_protocol.cpp
    - db/PROTOCOL.md
    - db/tests/engine/test_engine.cpp
    - db/tests/sync/test_sync_protocol.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Timestamp validation at Step 0c (after size/capacity, before structural/crypto checks)"
  - "Sync-path timestamp rejection: debug log and skip blob, no session abort"
  - "Direct-write timestamp rejection: debug log, no strike (receiver's decision)"
  - "TS_AUTO sentinel (UINT64_MAX) in test helpers for auto-current-time defaults"

patterns-established:
  - "Step 0c: Timestamp validation as third cheapest check (integer compare)"
  - "TS_AUTO sentinel pattern for test helpers needing valid timestamps"

requirements-completed: [OPS-02, DOCS-03]

# Metrics
duration: 24min
completed: 2026-03-22
---

# Phase 54 Plan 02: Timestamp Validation Summary

**Step 0c timestamp validation rejecting blobs >1hr future or >30d past before any crypto, with 8-code sync rejection protocol documented**

## Performance

- **Duration:** 24 min
- **Started:** 2026-03-22T10:33:33Z
- **Completed:** 2026-03-22T10:57:51Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- Timestamp validation in both ingest() and delete_blob() as Step 0c (cheapest integer compare before any crypto)
- 8 new engine tests covering boundary conditions for timestamp rejection
- Sync-path and direct-write timestamp rejection handling (debug log, skip, no abort)
- PROTOCOL.md expanded: all 8 SyncRejected reason codes documented, new Timestamp Validation section

## Task Commits

Each task was committed atomically:

1. **Task 1: Add timestamp validation to BlobEngine ingest and delete paths** - `ac00988` (feat)
2. **Task 2: Add sync-path timestamp rejection and update PROTOCOL.md** - `49984f4` (feat)

## Files Created/Modified
- `db/engine/engine.h` - Added IngestError::timestamp_rejected enum value
- `db/engine/engine.cpp` - Step 0c timestamp validation in ingest() and delete_blob()
- `db/peer/peer_manager.cpp` - Handle timestamp_rejected in Data message handler (debug log, no strike)
- `db/sync/sync_protocol.cpp` - Handle timestamp_rejected in sync ingest path (debug log, skip)
- `db/PROTOCOL.md` - Expanded SyncRejected to 8 codes, added Timestamp Validation section
- `db/tests/engine/test_engine.cpp` - 8 new timestamp tests, updated all test helpers to use TS_AUTO
- `db/tests/sync/test_sync_protocol.cpp` - Updated test helpers and timestamps for validation compatibility
- `db/tests/peer/test_peer_manager.cpp` - Updated test helpers for validation compatibility

## Decisions Made
- Timestamp validation placed at Step 0c: after size check (Step 0) and capacity check (Step 0b), before structural checks (Step 1). This keeps the cheapest-first ordering while adding the new validation.
- Sync-path timestamp rejections logged at debug level and blob skipped -- a single malformed timestamp does not abort the entire sync session.
- Direct-write (Data message) timestamp rejections logged at debug level with no strike -- timestamp rejection is a receiver-side decision, not necessarily a malicious peer.
- Test helpers updated with TS_AUTO sentinel (UINT64_MAX) to auto-use current system time, avoiding hardcoded timestamps that fall outside the 30-day validation window.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Updated all test helpers to use current timestamps**
- **Found during:** Task 1 (GREEN phase)
- **Issue:** Existing test helpers used hardcoded timestamps (1000000000, 2000, 5000, etc.) that are far outside the 30-day validation window, causing 57 existing tests to fail
- **Fix:** Introduced TS_AUTO sentinel pattern (UINT64_MAX) for test helpers; updated all 3 test files (engine, sync, peer) to use current_timestamp() defaults
- **Files modified:** db/tests/engine/test_engine.cpp, db/tests/sync/test_sync_protocol.cpp, db/tests/peer/test_peer_manager.cpp
- **Verification:** All 71 engine tests, 28 sync protocol tests pass
- **Committed in:** ac00988 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix)
**Impact on plan:** Necessary to maintain test suite compatibility with new timestamp validation. No scope creep.

## Issues Encountered
- Pre-existing bug in `SyncProtocol::is_blob_expired()`: divides timestamp by 1000000 treating it as microseconds when timestamps are actually seconds. This causes false expiry with real timestamps. Not fixed (out of scope) -- documented in deferred-items.md. One pre-existing test ("SyncProtocol tracks quota_exceeded_count") was already failing before this plan's changes.
- Pre-existing networking test flakiness (SIGSEGV in PEX closed mode test) -- port conflicts in test infrastructure, documented known issue.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Phase 54 (Operational Hardening) complete -- both plans executed
- Engine now validates timestamps before crypto work
- Protocol documentation up to date with all rejection codes
- Ready for Phase 55 (Runtime Compaction)

## Self-Check: PASSED

All files verified present. All commits verified in git log.

---
*Phase: 54-operational-hardening*
*Completed: 2026-03-22*
