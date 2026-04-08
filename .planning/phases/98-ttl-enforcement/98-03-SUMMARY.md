---
phase: 98-ttl-enforcement
plan: 03
subsystem: database
tags: [ttl, expiry, sync-filtering, protocol-docs, documentation]

# Dependency graph
requires:
  - phase: 98-ttl-enforcement
    plan: 01
    provides: wire::saturating_expiry and wire::is_blob_expired inline functions in codec.h
provides:
  - Sync path expiry filtering in collect_namespace_hashes, get_blobs_by_hashes, and Phase C transfer
  - Comprehensive TTL Enforcement documentation in PROTOCOL.md
  - Prominent tombstone TTL=0 requirement in README.md
affects: [ttl-enforcement, sync-protocol, protocol-documentation]

# Tech tracking
tech-stack:
  added: []
  patterns: [sync-time expiry filtering using injectable clock, defense-in-depth expiry at all exit points]

key-files:
  created: []
  modified:
    - db/sync/sync_protocol.cpp
    - db/peer/sync_orchestrator.cpp
    - db/PROTOCOL.md
    - db/README.md
    - db/tests/sync/test_sync_protocol.cpp

key-decisions:
  - "collect_namespace_hashes loads blob data to filter expired -- trades O(n) blob reads for correctness"
  - "SyncOrchestrator uses std::time(nullptr) for expiry checks (no injectable clock in orchestrator context)"
  - "PROTOCOL.md TTL Enforcement section placed between Storing a Blob and Sync Protocol for discoverability"
  - "README.md TTL Enforcement section placed between Wire Protocol and Scenarios for prominence"

patterns-established:
  - "All sync exit points filter expired blobs before sending -- defense-in-depth with query and fetch paths"

requirements-completed: [TTL-01, TTL-02, TTL-03]

# Metrics
duration: 30min
completed: 2026-04-08
---

# Phase 98 Plan 03: Sync Filtering & Documentation Summary

**Sync paths filter expired blobs in hash collection, blob retrieval, and Phase C transfer; PROTOCOL.md and README.md document all TTL enforcement guarantees**

## Performance

- **Duration:** 30 min
- **Started:** 2026-04-08T17:52:02Z
- **Completed:** 2026-04-08T18:22:02Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- collect_namespace_hashes now loads blobs and filters expired via wire::is_blob_expired before returning hash list
- get_blobs_by_hashes filters expired blobs from retrieval results
- SyncOrchestrator checks expiry at all 4 engine_.get_blob call sites before sending in Phase C
- PROTOCOL.md gains comprehensive TTL Enforcement section with expiry arithmetic, handler table, and all path filtering
- README.md gains prominent TTL Enforcement section with "Tombstones MUST have TTL = 0" requirement

## Task Commits

Each task was committed atomically:

1. **Task 1: Sync path expiry filtering + tests** - `8d07c8b` (test, RED), `4b1c125` (feat, GREEN)
2. **Task 2: Documentation -- PROTOCOL.md and README.md TTL enforcement sections** - `6a125c1` (docs)

## Files Created/Modified
- `db/sync/sync_protocol.cpp` - Added expiry filtering in collect_namespace_hashes and get_blobs_by_hashes
- `db/peer/sync_orchestrator.cpp` - Added wire::is_blob_expired checks at all 4 Phase C get_blob call sites
- `db/tests/sync/test_sync_protocol.cpp` - 3 new test cases with [sync][ttl] tags, updated existing test expectations
- `db/PROTOCOL.md` - New TTL Enforcement section with expiry arithmetic, ingest validation, query/fetch/sync filtering, notification suppression
- `db/README.md` - New TTL Enforcement section, updated Writer-Controlled TTL feature entry

## Decisions Made
- collect_namespace_hashes trades O(n) blob reads for correctness -- expired blobs must not appear in hash sets sent to peers
- SyncOrchestrator uses std::time(nullptr) since it lacks injectable clock access; SyncProtocol methods use clock_() for testability
- PROTOCOL.md section placed between blob storage and sync for logical flow
- README.md features Writer-Controlled TTL entry corrected to match tombstone TTL=0 enforcement reality

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Updated existing test expecting expired blobs in hash collection**
- **Found during:** Task 1 (TDD GREEN phase)
- **Issue:** "sync skips expired blobs" test expected collect_namespace_hashes to return 2 (including expired), but new behavior filters them
- **Fix:** Updated REQUIRE(hashes.size() == 2) to REQUIRE(hashes.size() == 1) with corrected comment
- **Files modified:** db/tests/sync/test_sync_protocol.cpp
- **Verification:** All 37 sync tests pass (207 assertions)
- **Committed in:** 4b1c125 (Task 1 GREEN commit)

**2. [Rule 1 - Bug] Fixed incorrect tombstone TTL claim in README.md**
- **Found during:** Task 2 (README documentation)
- **Issue:** Writer-Controlled TTL entry claimed "time-limited tombstones (TTL>0) are garbage-collected" -- this contradicts the tombstone TTL=0 enforcement added in Plan 01
- **Fix:** Updated to "Tombstones MUST have TTL=0 (permanent) -- the node rejects tombstones with non-zero TTL at ingest"
- **Files modified:** db/README.md
- **Verification:** README now consistent with PROTOCOL.md and engine behavior
- **Committed in:** 6a125c1 (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both fixes necessary for correctness -- test expectation matched old behavior, README contradicted actual enforcement. No scope creep.

## Issues Encountered
None -- all verification passed after auto-fixes.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All sync paths now filter expired blobs at every exit point
- TTL enforcement is documented comprehensively in PROTOCOL.md and README.md
- Phase 98 TTL enforcement is now complete across all paths: ingest, query, fetch, sync, and documentation
- All 37 sync tests pass with zero regressions

## Self-Check: PASSED

All 6 files verified present. All 3 commit hashes verified in git log.

---
*Phase: 98-ttl-enforcement*
*Completed: 2026-04-08*
