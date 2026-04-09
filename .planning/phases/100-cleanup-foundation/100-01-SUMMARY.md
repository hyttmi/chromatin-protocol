---
phase: 100-cleanup-foundation
plan: 01
subsystem: infra
tags: [cmake, cleanup, relay, sdk, build-system]

# Dependency graph
requires: []
provides:
  - "Clean repo with only db/ component, no old relay or SDK artifacts"
  - "relay_identity.h/cpp preserved at /tmp for Plan 100-02"
  - "Updated dist/ config and systemd for new relay"
affects: [100-02, relay-scaffold]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "ConnectionTestAccess friend class pattern for private test methods"

key-files:
  created: []
  modified:
    - CMakeLists.txt
    - db/CMakeLists.txt
    - .gitignore
    - dist/install.sh
    - dist/config/relay.json
    - dist/systemd/chromatindb-relay.service
    - db/PROTOCOL.md
    - db/README.md
    - db/tests/net/test_connection.cpp

key-decisions:
  - "Removed all SDK-specific wording from PROTOCOL.md and README.md, replaced with generic 'client' terminology"
  - "dist/install.sh reduced to single binary (node only) until new relay is installable"

patterns-established:
  - "ConnectionTestAccess: friend class for accessing private test-only methods on Connection"

requirements-completed: [CLEAN-01, CLEAN-02, CLEAN-03]

# Metrics
duration: 52min
completed: 2026-04-09
---

# Phase 100 Plan 01: Legacy Cleanup Summary

**Deleted old relay/ (1,500 LOC), sdk/python/ (4,600 LOC + 656 tests), and db/tests/relay/ (836 LOC); scrubbed all stale references from build system, docs, and dist files**

## Performance

- **Duration:** 52 min
- **Started:** 2026-04-09T10:38:26Z
- **Completed:** 2026-04-09T11:31:25Z
- **Tasks:** 2
- **Files modified:** 63

## Accomplishments
- Deleted old relay/ directory (10 source files, ~1,500 LOC) and sdk/python/ directory (~4,600 LOC + 656 tests)
- Removed 4 old relay test files from db/tests/relay/ (836 LOC)
- Scrubbed all stale relay and SDK references from CMakeLists.txt, db/CMakeLists.txt, .gitignore, dist/, PROTOCOL.md, and README.md
- Preserved relay_identity.h/cpp at /tmp/chromatindb-relay-identity/ for Plan 100-02
- Updated dist/config/relay.json with new relay config template (includes max_send_queue)
- Updated systemd service to use --config directly (no run subcommand)
- Repo builds and tests pass cleanly with only db/ target

## Task Commits

Each task was committed atomically:

1. **Task 1: Save relay identity, delete old relay and SDK directories, remove old relay tests** - `03044da` (chore)
2. **Task 2: Clean stale references from build system, docs, dist, and .gitignore** - `c857a14` (chore)

## Files Created/Modified
- `relay/` - Deleted (10 source files)
- `sdk/python/` - Deleted (~30 files)
- `db/tests/relay/` - Deleted (4 test files)
- `CMakeLists.txt` - Removed relay component and relay binary sections
- `db/CMakeLists.txt` - Removed relay test files and chromatindb_relay_lib linkage
- `.gitignore` - Removed sdk/python/.venv entry
- `dist/install.sh` - Updated for single binary installation
- `dist/config/relay.json` - New relay config template with max_send_queue
- `dist/systemd/chromatindb-relay.service` - Updated ExecStart (no run subcommand)
- `db/PROTOCOL.md` - Renamed "SDK Client Notes" to "Client Implementation Notes", removed Python-specific section, updated wording
- `db/README.md` - Updated SDK-specific wording to generic "client" terminology
- `db/tests/net/test_connection.cpp` - Fixed pre-existing ConnectionTestAccess friend class usage

## Decisions Made
- Removed all SDK-specific wording from PROTOCOL.md and README.md, replaced with generic "client" terminology
- dist/install.sh reduced to single binary (node only) until new relay is installable
- Kept relay.json config install and relay systemd service install in dist/ -- they deploy config for the new relay

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Fixed ConnectionTestAccess friend class usage in test_connection.cpp**
- **Found during:** Task 2 (build verification)
- **Issue:** Pre-existing bug from commit ed171d8 ("fix: final audit sweep") which made set_send_counter_for_test() private and added a friend class declaration, but test_connection.cpp was not updated to use the friend accessor. Build failed with "is private within this context" error.
- **Fix:** Defined ConnectionTestAccess class in chromatindb::net namespace in test_connection.cpp and updated the direct call to use the accessor.
- **Files modified:** db/tests/net/test_connection.cpp
- **Verification:** Build succeeds, 442+ tests pass
- **Committed in:** c857a14 (part of Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Fix was necessary for build verification to succeed. Pre-existing bug, not caused by plan changes.

## Issues Encountered
None beyond the deviation noted above.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None.

## Next Phase Readiness
- Repo is clean with only db/ component
- relay_identity.h/cpp preserved at /tmp/chromatindb-relay-identity/ for Plan 100-02 to copy into new relay tree
- dist/ config and systemd already prepared for new relay binary
- Ready for Plan 100-02: scaffold new relay directory structure

---
*Phase: 100-cleanup-foundation*
*Completed: 2026-04-09*
