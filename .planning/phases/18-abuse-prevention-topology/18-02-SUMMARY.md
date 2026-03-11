---
phase: 18-abuse-prevention-topology
plan: 02
subsystem: peer
tags: [namespace-filtering, sync, topology, config, sighup]

# Dependency graph
requires:
  - phase: 18-abuse-prevention-topology
    provides: rate limiting infrastructure, reload_config pattern, PeerInfo token bucket
provides:
  - Configurable namespace-scoped sync filtering (sync_namespaces)
  - Namespace filter at sync Phase A (namespace list assembly) and Phase C (blob request)
  - Namespace filter at Data/Delete ingest (silent drop)
  - SIGHUP-reloadable sync_namespaces
  - hex_to_namespace conversion helper
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: [namespace filtering at sync Phase A/C and ingest, hex-to-binary namespace conversion]

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - tests/config/test_config.cpp
    - tests/peer/test_peer_manager.cpp

key-decisions:
  - "Reuse validate_allowed_keys for sync_namespaces (same 64-char hex format)"
  - "Filter at Phase C (blob request) in addition to Phase A (namespace list) to prevent requesting filtered blobs"
  - "Silent drop at Data/Delete ingest (no strike, no error) for defense in depth"
  - "Empty sync_namespaces means replicate all (default)"
  - "sync_namespaces_ as std::set<std::array<uint8_t, 32>> for O(log n) lookup"

patterns-established:
  - "Namespace filtering: applied at both send (Phase A) and receive (Phase C + ingest) for completeness"
  - "hex_to_namespace: 64-char hex to 32-byte array conversion in anonymous namespace"

requirements-completed: [PROT-04, PROT-05, PROT-06]

# Metrics
duration: 38min
completed: 2026-03-11
---

# Phase 18 Plan 02: Namespace Filtering Summary

**Configurable namespace-scoped sync filtering with sync_namespaces config, Phase A/C filtering, Data/Delete ingest drop, and SIGHUP reload**

## Performance

- **Duration:** 38 min
- **Started:** 2026-03-11T15:50:25Z
- **Completed:** 2026-03-11T16:29:06Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- sync_namespaces config field with JSON parsing and validate_allowed_keys reuse
- Namespace filter at sync Phase A (both initiator and responder) excludes filtered namespace IDs from list
- Namespace filter at sync Phase C (both initiator and responder) skips requesting blobs from filtered namespaces
- Namespace filter at Data/Delete ingest silently drops blobs for filtered namespaces
- SIGHUP reload validates and rebuilds sync_namespaces_ set
- 5 new tests: 4 config parsing + 1 E2E namespace filter verification
- Empty sync_namespaces means replicate all namespaces (no filtering)

## Task Commits

Each task was committed atomically:

1. **Task 1 (TDD): Config field + PeerManager set + SIGHUP reload** - `ae94177` (feat)
2. **Task 2 RED: Failing E2E namespace filter test** - `c64cec9` (test)
3. **Task 2 GREEN: Namespace filter at sync Phase A/C + Data/Delete ingest** - `8dd2350` (feat)

## Files Created/Modified
- `db/config/config.h` - Added sync_namespaces field to Config struct
- `db/config/config.cpp` - JSON parsing for sync_namespaces with validate_allowed_keys reuse
- `db/peer/peer_manager.h` - Added sync_namespaces_ set member to PeerManager
- `db/peer/peer_manager.cpp` - hex_to_namespace helper, constructor init, Phase A/C filter, Data/Delete filter, reload_config
- `tests/config/test_config.cpp` - 4 config tests: default empty, parse valid, missing field, invalid entries
- `tests/peer/test_peer_manager.cpp` - E2E test verifying namespace filter at sync

## Decisions Made
- Reuse validate_allowed_keys for sync_namespaces validation (same 64-char hex format, no duplication)
- Filter at Phase C (blob request) in addition to Phase A -- Phase A controls what we advertise, Phase C controls what we request from peer's advertised namespaces
- Silent drop at Data/Delete ingest for defense in depth (peer may still send us Data for filtered ns)
- sync_namespaces_ stored as std::set for O(log n) lookup during filtering
- Empty sync_namespaces means replicate all (default, no special handling needed)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Added namespace filter at sync Phase C (blob request stage)**
- **Found during:** Task 2 (E2E test failure)
- **Issue:** Plan specified filter at Phase A (namespace list) only, but peer still advertises filtered namespaces and node would request/receive those blobs during Phase C
- **Fix:** Added sync_namespaces_ check in Phase C loop (both initiator and responder) to skip requesting blobs from filtered namespaces
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** E2E test passes -- filtered namespace blobs not replicated
- **Committed in:** 8dd2350 (Task 2 GREEN commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Essential for correctness. Phase A filter alone is insufficient because peer's namespace list is unfiltered. Phase C filter prevents requesting blobs from filtered namespaces.

## Issues Encountered

None.

## User Setup Required

None -- no external service configuration required.

## Next Phase Readiness
- Phase 18 complete: rate limiting (Plan 01) and namespace filtering (Plan 02) both shipped
- All PROT requirements (PROT-01 through PROT-06) satisfied
- Ready for Phase 19 (if applicable)

## Self-Check: PASSED

All 6 files verified present. All 3 commits verified in git log.

---
*Phase: 18-abuse-prevention-topology*
*Completed: 2026-03-11*
