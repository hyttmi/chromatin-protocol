---
phase: 55-runtime-compaction
plan: 01
subsystem: database
tags: [mdbx, compaction, timer, sighup, config]

# Dependency graph
requires:
  - phase: 54-operational-hardening
    provides: timer-cancel pattern for SIGHUP-reloadable intervals, expiry_scan_loop precedent
provides:
  - compaction_interval_hours config field (default 6h, 0=disabled)
  - Storage::compact() method with CompactResult metrics
  - PeerManager compaction_loop() coroutine with SIGHUP hot-reload
  - SIGUSR1 metrics: last_compaction_time, compaction_count
affects: [56-local-access]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "open_env() helper in Storage::Impl for reopen after compaction"
    - "File-level compaction: env.copy(compactify=true) + close + rename + reopen"

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/main.cpp
    - db/tests/config/test_config.cpp
    - db/tests/storage/test_storage.cpp

key-decisions:
  - "Compaction swaps mdbx.dat inside data_dir (directory layout, not file-as-path)"
  - "Factored open_env() helper in Impl for constructor + compact() reuse"
  - "Test uses 200 blobs with 10KB payloads to exceed 1 MiB minimum geometry"

patterns-established:
  - "Storage::Impl::open_env() helper: reusable env open logic for compaction reopen"
  - "data_dir_ stored in Impl for compact() file operations"

requirements-completed: [COMP-01]

# Metrics
duration: 10min
completed: 2026-03-22
---

# Phase 55 Plan 01: Runtime Compaction Summary

**Automatic mdbx compaction on configurable timer (default 6h) with live copy, file swap, SIGHUP reload, and SIGUSR1 metrics**

## Performance

- **Duration:** 10 min
- **Started:** 2026-03-22T12:48:54Z
- **Completed:** 2026-03-22T12:59:32Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- Config field compaction_interval_hours with default 6, 0=disabled, JSON parse, validation, known_keys
- Storage::compact() creates live compacted copy via env.copy(compactify=true), closes env, swaps mdbx.dat, reopens with open_env() helper
- CompactResult struct reports before/after bytes, duration, success flag
- PeerManager compaction_loop() fires on schedule, single info log line with MiB reduction percentage
- SIGHUP reloads interval and restarts timer, including disabled-to-enabled transitions
- SIGUSR1 dump includes last_compaction_time and compaction_count
- Startup log shows compaction interval or disabled state
- 9 config tests + 4 storage compact tests, 205 total config+storage tests passing

## Task Commits

Each task was committed atomically:

1. **Task 1: Config field + Storage::compact()** - `a7a7c9f` (test) + `91d5b02` (feat) [TDD: red then green]
2. **Task 2: PeerManager timer + SIGHUP + SIGUSR1** - `03ff9ee` (feat)

## Files Created/Modified
- `db/config/config.h` - Added compaction_interval_hours field (default 6)
- `db/config/config.cpp` - JSON parsing, known_keys, validation
- `db/storage/storage.h` - CompactResult struct, compact() method declaration
- `db/storage/storage.cpp` - Impl refactored with open_env()/data_dir_, compact() implementation
- `db/peer/peer_manager.h` - compaction_loop(), timer pointer, interval/metrics members
- `db/peer/peer_manager.cpp` - compaction_loop coroutine, cancel_all_timers, SIGHUP reload, SIGUSR1 dump
- `db/main.cpp` - Startup log for compaction interval
- `db/tests/config/test_config.cpp` - 9 compaction config tests
- `db/tests/storage/test_storage.cpp` - 4 storage compact tests (empty, data, deletion, post-compact ops)

## Decisions Made
- data_dir is a directory containing mdbx.dat (not a file), so compaction swaps the mdbx.dat file inside the directory rather than the directory itself
- Factored Impl constructor into open_env() helper to avoid code duplication between constructor and compact() reopen
- Test for file size reduction uses 200 blobs with 10KB payloads to push past 1 MiB minimum mdbx geometry

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed data_dir file layout assumption**
- **Found during:** Task 1 (Storage::compact() implementation)
- **Issue:** Plan assumed data_dir might be a file (mdbx with use_subdirectory=false). Actual filesystem shows data_dir is a directory containing mdbx.dat and mdbx.lck
- **Fix:** Changed compact() to swap mdbx.dat inside the data_dir directory instead of renaming the path itself
- **Files modified:** db/storage/storage.cpp
- **Verification:** All 4 storage compact tests pass including post-compaction reads/writes
- **Committed in:** 91d5b02

**2. [Rule 1 - Bug] Fixed test data volume for compaction size reduction**
- **Found during:** Task 1 (Storage compact test verification)
- **Issue:** Initial test used 50 small blobs (~350KB total), stayed within mdbx 1 MiB minimum geometry, so compaction could not reduce size
- **Fix:** Changed to 200 blobs with 10KB payloads (~3.6 MB) to exceed minimum geometry, enabling measurable size reduction after deletion
- **Files modified:** db/tests/storage/test_storage.cpp
- **Verification:** Test now correctly verifies after_bytes < before_bytes
- **Committed in:** 91d5b02

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both fixes necessary for correct implementation. No scope creep.

## Issues Encountered
None beyond the deviations documented above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Runtime compaction complete, ready for Phase 56 (Local Access / UDS interface)
- All 205 config+storage tests passing, build clean under ASAN

---
*Phase: 55-runtime-compaction*
*Completed: 2026-03-22*
