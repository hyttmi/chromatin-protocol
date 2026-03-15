---
phase: 22-build-restructure
plan: 01
subsystem: infra
tags: [cmake, fetchcontent, build-system, subdirectory-component]

# Dependency graph
requires:
  - phase: 21-test-260-segfault-fix
    provides: stable test suite (284 tests passing)
provides:
  - db/ as self-contained CMake component with project(chromatindb-core)
  - Guarded FetchContent pattern for composable subdirectories
  - Standalone buildability of db/ via cmake -S db/ -B build
affects: [23-ttl-flexibility, 24-encryption-at-rest, 25-transport-optimization]

# Tech tracking
tech-stack:
  added: []
  patterns: [guarded-fetchcontent, subdirectory-component, cmake-current-source-dir-paths]

key-files:
  created: [db/CMakeLists.txt, db/schemas/blob.fbs, db/schemas/transport.fbs]
  modified: [CMakeLists.txt]

key-decisions:
  - "No ENABLE_ASAN in db/CMakeLists.txt (YAGNI -- sanitizers are a consumer concern)"
  - "No install() rules in db/ (YAGNI -- no external consumers)"
  - "No CMAKE_BUILD_TYPE in db/ (inherited from root or set by standalone user)"

patterns-established:
  - "if(NOT TARGET) guard: wrap both FetchContent_Declare and MakeAvailable inside guard for composable subdirectories"
  - "CMAKE_CURRENT_SOURCE_DIR for all paths in subdirectory CMakeLists.txt (not CMAKE_SOURCE_DIR)"
  - "Parent dir include: target_include_directories PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..> for db/ prefix in includes"

requirements-completed: [BUILD-01]

# Metrics
duration: 14min
completed: 2026-03-14
---

# Phase 22 Plan 01: Build Restructure Summary

**Split CMakeLists.txt into self-contained db/ component with 7 guarded FetchContent deps, schema relocation, and root consuming via add_subdirectory**

## Performance

- **Duration:** 14 min
- **Started:** 2026-03-14T07:20:25Z
- **Completed:** 2026-03-14T07:34:56Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- db/ is a self-contained CMake component with project(chromatindb-core) that configures standalone
- Root CMakeLists.txt consumes db/ via add_subdirectory with zero duplicate library definitions
- All 284 tests pass with zero regressions
- Schema files relocated from schemas/ to db/schemas/ with codegen paths updated

## Task Commits

Each task was committed atomically:

1. **Task 1: Move schemas and create db/CMakeLists.txt** - `be03fea` (feat)
2. **Task 2: Rewrite root CMakeLists.txt and verify full build** - `dec9f41` (feat)

## Files Created/Modified
- `db/CMakeLists.txt` - Self-contained CMake component (182 lines): project(chromatindb-core), 7 guarded FetchContent blocks, FlatBuffers codegen, chromatindb_lib STATIC target
- `db/schemas/blob.fbs` - FlatBuffers blob schema (moved from schemas/)
- `db/schemas/transport.fbs` - FlatBuffers transport schema (moved from schemas/)
- `CMakeLists.txt` - Root build rewritten to consume db/ via add_subdirectory (235 -> 169 lines)

## Decisions Made
- No ENABLE_ASAN in db/CMakeLists.txt -- sanitizers are a consumer concern (YAGNI)
- No install() rules -- no external consumers exist (YAGNI)
- No CMAKE_BUILD_TYPE in db/ -- inherited from root or set by standalone user
- CMake minimum version 3.20 for db/ (matches root)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Build system restructured, db/ is a reusable component
- Ready for Phase 23 (TTL Flexibility) -- all source files in db/ with stable build
- Phase 25 (Transport Optimization) also unblocked since it depends only on Phase 22

---
*Phase: 22-build-restructure*
*Completed: 2026-03-14*
