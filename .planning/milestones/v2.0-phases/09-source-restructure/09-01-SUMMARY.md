---
phase: 09-source-restructure
plan: 01
subsystem: infra
tags: [cmake, directory-structure, includes, refactoring]

# Dependency graph
requires: []
provides:
  - "db/ directory layout with all 40 source files"
  - "CMakeLists.txt with db/ paths and project-root include dir"
  - "All #include directives using db/ prefix in source and test files"
affects: [09-source-restructure]

# Tech tracking
tech-stack:
  added: []
  patterns: ["db/ prefix for all project-internal includes", "project root as CMake include directory"]

key-files:
  created: ["db/ (40 files moved from src/)"]
  modified: ["CMakeLists.txt", "db/**/*.cpp", "db/**/*.h", "tests/**/*.cpp"]

key-decisions:
  - "Include root is project root (CMAKE_CURRENT_SOURCE_DIR) so includes resolve as db/module/file.h"

patterns-established:
  - "db/ prefix: all project-internal #include directives use db/ prefix (e.g., db/crypto/hash.h)"
  - "Project root include: CMake include_directories points to project root, not a subdirectory"

requirements-completed: [STRUCT-01]

# Metrics
duration: 3min
completed: 2026-03-06
---

# Phase 9 Plan 01: Source Restructure Summary

**Moved 40 source files from src/ to db/, updated CMakeLists.txt paths and all 123 #include directives to use db/ prefix**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-06T03:08:34Z
- **Completed:** 2026-03-06T03:11:35Z
- **Tasks:** 2
- **Files modified:** 59 (1 CMakeLists.txt + 40 source + 18 test files)

## Accomplishments
- Moved entire src/ directory to db/ using git mv (preserves full git history for all 40 files)
- Updated CMakeLists.txt: all 18 library source paths, daemon binary path, FlatBuffers generated dir, include root changed to project root
- Updated 123 #include directives across 51 files (33 source + 18 test) to use db/ prefix

## Task Commits

Each task was committed atomically:

1. **Task 1: Move src/ to db/ and update CMakeLists.txt** - `f0a1635` (refactor)
2. **Task 2: Update all #include directives to use db/ prefix** - `98200ab` (refactor)

## Files Created/Modified
- `CMakeLists.txt` - All source paths changed from src/ to db/, include root changed to project root, FlatBuffers generated dir to db/wire/
- `db/**/*.cpp` (21 files) - Internal #include directives updated to use db/ prefix
- `db/**/*.h` (19 files) - Internal #include directives updated to use db/ prefix
- `tests/**/*.cpp` (18 files) - Internal #include directives updated to use db/ prefix

## Decisions Made
- Include root set to project root (CMAKE_CURRENT_SOURCE_DIR) rather than db/ subdirectory, so includes resolve as `#include "db/crypto/hash.h"` -- this makes the db/ prefix explicit in all includes, clearly distinguishing project code from external dependencies.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Committed uncommitted v1.0 baseline files**
- **Found during:** Pre-Task 1 preparation
- **Issue:** Working tree had uncommitted v1.0 changes (sync protocol, peer manager, daemon entry point, transport schema) that would block git mv
- **Fix:** Committed all pending v1.0 changes as baseline commit before starting restructure
- **Files modified:** 17 files (src/main.cpp, src/sync/*, src/peer/*, src/net/server.*, tests/*, schemas/transport.fbs, CMakeLists.txt)
- **Verification:** git status clean after commit, git mv succeeded
- **Committed in:** d91ae30 (baseline commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Necessary to unblock git mv. No scope creep.

## Issues Encountered
None -- both tasks executed cleanly.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Directory structure is in place for Plan 02 (namespace rename chromatin:: to chromatindb::)
- Project will NOT compile yet -- namespace rename in Plan 02 is required first
- All FlatBuffers generated headers need regeneration in Plan 02

---
*Phase: 09-source-restructure*
*Completed: 2026-03-06*
