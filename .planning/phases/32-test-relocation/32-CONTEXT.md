# Phase 32: Test Relocation - Context

**Gathered:** 2026-03-17
**Status:** Ready for planning

<domain>
## Phase Boundary

Move all 20 database test source files from the top-level `tests/` directory into `db/tests/`, making db/ a fully self-contained CMake component that builds and tests itself. The top-level `tests/` directory is deleted. `ctest -N` must report exactly 284 tests after relocation.

</domain>

<decisions>
## Implementation Decisions

### Test target ownership
- Test executable (`chromatindb_tests`) defined in db/CMakeLists.txt, not in the top-level CMakeLists.txt
- Guarded Catch2 FetchContent added to db/CMakeLists.txt (`if(NOT TARGET Catch2::Catch2WithMain)`)
- Tests guarded behind `BUILD_TESTING` (standard CMake variable, no custom option)
- `catch_discover_tests()` used for per-test CTest granularity (same as current)
- Executable name stays `chromatindb_tests`
- Standalone build support: db/CMakeLists.txt includes `include(CTest)` behind `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)` guard

### Top-level CMakeLists.txt cleanup
- Top-level keeps `include(CTest)` (sets BUILD_TESTING for subdirectories)
- Entire `if(BUILD_TESTING)...endif()` block removed from top-level (no placeholder, no comment)
- Catch2 FetchContent_Declare removed from top-level (db/ owns it now)
- Catch2 removed from `FetchContent_MakeAvailable()` call

### File relocation
- Use `git mv` to preserve file history/rename tracking
- Top-level `tests/` directory deleted entirely after relocation (no .gitkeep, no README)
- All references to `tests/` paths across the project audited and fixed (CMake, Dockerfile, scripts, configs)

### Directory layout
- db/tests/ mirrors db/ source subdirectory structure: db/tests/crypto/, db/tests/net/, db/tests/engine/, etc.
- test_daemon.cpp placed at db/tests/ root (not in a subdirectory)
- Include paths inside test files unchanged (already use db/-prefixed includes like `#include "db/crypto/hash.h"`)

### Claude's Discretion
- Exact ordering of files in the CMake `add_executable()` source list
- Whether to alphabetize or group by module in the CMake file

</decisions>

<specifics>
## Specific Ideas

No specific requirements — straightforward mechanical relocation following the decisions above.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- db/CMakeLists.txt: Already has guarded FetchContent pattern for all 8 dependencies — Catch2 guard follows the same `if(NOT TARGET ...)` pattern
- Top-level CMakeLists.txt: `include(CTest)` and `include(Catch)` patterns to replicate in db/

### Established Patterns
- Guarded FetchContent: `if(NOT TARGET foo) ... FetchContent_MakeAvailable(foo) ... endif()` used for all deps in db/CMakeLists.txt
- `catch_discover_tests()` from Catch2 module for auto-registration of TEST_CASEs with CTest

### Integration Points
- Top-level `add_subdirectory(db)` already exists — no new integration needed
- `FetchContent_MakeAvailable()` call in top-level needs Catch2 removed from the list
- Dockerfile and deploy scripts may reference tests/ paths

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 32-test-relocation*
*Context gathered: 2026-03-17*
