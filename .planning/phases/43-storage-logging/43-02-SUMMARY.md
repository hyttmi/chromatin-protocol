---
phase: 43-storage-logging
plan: 02
subsystem: database
tags: [libmdbx, integrity-scan, cursor-compaction, metrics, tombstone-gc]

# Dependency graph
requires:
  - phase: 42-foundation
    provides: cancel_all_timers() consolidated timer pattern, validate_config
provides:
  - integrity_scan() startup health check for all 7 sub-databases
  - used_data_bytes() accurate storage metric (B-tree occupancy)
  - Tombstone GC root cause documented (mmap geometry vs data volume)
  - Cursor compaction timer (6h interval, prunes disconnected peers)
  - Complete metrics output (quota_rejections + sync_rejections)
affects: [44-connection-resilience, 45-docs-release]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Scoped read txn pattern: close txn before calling methods that open their own"
    - "Timer-cancel pattern extension: cursor_compaction_timer_ follows same pattern as other 5 timers"

key-files:
  created: []
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/main.cpp
    - db/tests/storage/test_storage.cpp

key-decisions:
  - "Tombstone GC is NOT a bug: used_bytes() returns mmap geometry, freed pages reused internally"
  - "used_data_bytes() uses mi_last_pgno * pagesize for accurate B-tree occupancy"
  - "Integrity scan is informational only (logs warnings, does not refuse startup)"
  - "Cursor compaction hardcoded to 6h interval (YAGNI, no config needed)"
  - "Scoped read txn in integrity_scan to avoid MDBX_BAD_RSLOT with list_namespaces"

patterns-established:
  - "Scoped txn pattern: when combining stat reads with methods that open their own txn, scope the outer txn"

requirements-completed: [STOR-01, STOR-02, STOR-03, OPS-03]

# Metrics
duration: 26min
completed: 2026-03-20
---

# Phase 43 Plan 02: Storage Health Summary

**Startup integrity scan for 7 sub-databases, cursor compaction timer, tombstone GC root cause documented, and complete metrics output with quota/sync rejection counters**

## Performance

- **Duration:** 26 min
- **Started:** 2026-03-20T04:26:25Z
- **Completed:** 2026-03-20T04:52:58Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- integrity_scan() reports entry counts for all 7 sub-databases at startup with cross-reference warnings
- Tombstone GC root cause documented: used_bytes() is mmap geometry, freed pages are reused internally (not a bug)
- used_data_bytes() provides accurate B-tree occupancy metric (mi_last_pgno * pagesize)
- Cursor compaction timer runs every 6h, pruning cursors for disconnected peers
- log_metrics_line() now emits quota_rejections and sync_rejections (both periodic and SIGUSR1 paths)

## Task Commits

Each task was committed atomically:

1. **Task 1 RED: Integrity scan tests** - `70a7e7a` (test)
2. **Task 1 GREEN: Integrity scan + used_data_bytes + GC docs** - `dfe7092` (feat)
3. **Task 2: Cursor compaction + metrics completeness** - `63f9ea9` (feat)

## Files Created/Modified
- `db/storage/storage.h` - Added integrity_scan() and used_data_bytes() declarations with documented used_bytes() behavior
- `db/storage/storage.cpp` - Implemented integrity_scan() with O(1) stat reads and cross-reference checks, used_data_bytes(), documented used_bytes() comments
- `db/peer/peer_manager.h` - Added cursor_compaction_timer_ member and cursor_compaction_loop() declaration
- `db/peer/peer_manager.cpp` - Cursor compaction timer loop, extended log_metrics_line() with quota_rejections + sync_rejections, timer in cancel_all_timers()
- `db/main.cpp` - Wired storage.integrity_scan() after Storage construction, before PeerManager
- `db/tests/storage/test_storage.cpp` - 4 tests for integrity_scan(), GC entry count verification, used_data_bytes()

## Decisions Made
- Tombstone GC is NOT a bug: used_bytes() returns mi_geo.current (mmap file geometry). Freed pages are reused internally by libmdbx's B-tree GC. File only shrinks when freed space exceeds shrink_threshold (4 MiB).
- used_data_bytes() uses mi_last_pgno * pagesize for actual B-tree occupancy rather than mmap geometry.
- Integrity scan is informational only -- logs entry counts and warnings, does not refuse to start on inconsistencies.
- Cursor compaction interval hardcoded to 6 hours (YAGNI -- no config option needed).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed MDBX_BAD_RSLOT in integrity_scan()**
- **Found during:** Task 1 (integrity_scan implementation)
- **Issue:** integrity_scan() held a read transaction while calling list_namespaces(), which tries to open its own read transaction on the same thread, causing MDBX_BAD_RSLOT error
- **Fix:** Scoped the stat-reading transaction to close before calling list_namespaces()
- **Files modified:** db/storage/storage.cpp
- **Verification:** Tests pass clean with no MDBX errors
- **Committed in:** dfe7092 (part of Task 1 GREEN commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Essential fix for correctness. No scope creep.

## Issues Encountered
- Pre-existing flaky network test ("NodeMetrics counters increment during E2E flow") fails intermittently when running full suite due to port collisions, passes in isolation. Out of scope, not caused by plan changes.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Storage health features complete, ready for Phase 44 (Connection Resilience)
- integrity_scan() provides startup diagnostics for production nodes
- Cursor compaction prevents unbounded cursor growth over long uptimes
- Complete metrics visibility for operational monitoring

## Self-Check: PASSED

All 6 files verified present. All 3 commits (70a7e7a, dfe7092, 63f9ea9) verified in git log.

---
*Phase: 43-storage-logging*
*Completed: 2026-03-20*
