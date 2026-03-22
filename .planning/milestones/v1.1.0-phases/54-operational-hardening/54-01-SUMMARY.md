---
phase: 54-operational-hardening
plan: 01
subsystem: database
tags: [config, sync, expiry, sighup, constexpr]

# Dependency graph
requires:
  - phase: 53-release-cleanup
    provides: Clean v1.0.0 codebase baseline
provides:
  - Configurable expiry scan interval (10-3600s, default 60, SIGHUP-reloadable)
  - Shared constexpr sync rejection header with 8 reason codes and string mapping
  - All sync rejection sites use shared header for codes and human-readable strings
affects: [54-02-PLAN, peer_manager, config]

# Tech tracking
tech-stack:
  added: []
  patterns: [constexpr string_view lookup for wire protocol reason codes]

key-files:
  created: [db/peer/sync_reject.h]
  modified: [db/config/config.h, db/config/config.cpp, db/peer/peer_manager.h, db/peer/peer_manager.cpp, db/main.cpp, db/tests/config/test_config.cpp]

key-decisions:
  - "sync_reject.h in chromatindb::peer namespace with constexpr switch for zero-cost reason string lookup"
  - "expiry_scan_interval_seconds minimum 10s to prevent excessive I/O"

patterns-established:
  - "Shared constexpr header for wire protocol constants and string mappings"

requirements-completed: [OPS-01, OPS-03]

# Metrics
duration: 12min
completed: 2026-03-22
---

# Phase 54 Plan 01: Configurable Expiry Scan + Sync Rejection Codes Summary

**Configurable expiry scan interval with SIGHUP hot-reload and shared constexpr sync rejection header with 8 reason codes**

## Performance

- **Duration:** 12 min
- **Started:** 2026-03-22T10:18:37Z
- **Completed:** 2026-03-22T10:30:40Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Operators can now tune GC timing via expiry_scan_interval_seconds config (minimum 10s, default 60s)
- SIGHUP reloads the expiry scan interval and immediately restarts the timer with the new value
- All 8 sync rejection reason codes (cooldown, session_limit, byte_rate, storage_full, quota_exceeded, namespace_not_found, blob_too_large, timestamp_rejected) defined in shared constexpr header
- Receiver and sender both use sync_reject_reason_string() for consistent human-readable logging
- 10 new tests covering config validation, parsing, defaults, and reason string mapping

## Task Commits

Each task was committed atomically:

1. **Task 1: Create shared sync_reject.h header and add expiry_scan_interval_seconds config field** - `e22f1a5` (feat)
2. **Task 2: Wire up expiry scan interval in PeerManager and expand sync rejection sites** - `47e9bd3` (feat)

_Note: Task 1 was TDD (tests + implementation in single commit since all passed on first run)_

## Files Created/Modified
- `db/peer/sync_reject.h` - New shared constexpr header with 8 rejection reason codes and string mapping function
- `db/config/config.h` - Added expiry_scan_interval_seconds field to Config struct
- `db/config/config.cpp` - Added parsing, known_keys entry, and validation (>= 10)
- `db/peer/peer_manager.h` - Added expiry_scan_interval_seconds_ SIGHUP-reloadable member
- `db/peer/peer_manager.cpp` - Wired configurable interval, SIGHUP reload with timer cancel, replaced anonymous constants and if/else chain with shared header
- `db/main.cpp` - Added expiry scan interval to startup log
- `db/tests/config/test_config.cpp` - 10 new tests for config and reason string mapping

## Decisions Made
- Used constexpr switch in sync_reject_reason_string() for zero-overhead reason string lookup at compile time
- Minimum expiry scan interval set to 10 seconds to prevent excessive storage I/O from overly aggressive scanning
- Kept reason codes as bare constexpr uint8_t (not enum) for direct wire protocol compatibility

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- OPS-01 (configurable expiry scan) and OPS-03 (sync rejection reason strings) requirements satisfied
- Plan 54-02 (timestamp validation) can proceed with sync_reject.h available for SYNC_REJECT_TIMESTAMP_REJECTED

## Self-Check: PASSED

- db/peer/sync_reject.h: FOUND
- 54-01-SUMMARY.md: FOUND
- Commit e22f1a5: FOUND
- Commit 47e9bd3: FOUND

---
*Phase: 54-operational-hardening*
*Completed: 2026-03-22*
