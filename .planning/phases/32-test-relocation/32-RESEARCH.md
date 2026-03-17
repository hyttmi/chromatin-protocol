# Phase 32: Test Relocation - Research

**Researched:** 2026-03-17
**Domain:** CMake project restructuring / test co-location
**Confidence:** HIGH

## Summary

This phase is a mechanical relocation of 20 test source files from the top-level `tests/` directory into `db/tests/`, making db/ a fully self-contained CMake component. The primary technical concern is correctly wiring CMake so that Catch2 dependency ownership transfers from the top-level to db/, the `include(Catch)` module remains resolvable, and `catch_discover_tests()` continues to register all tests with CTest.

All 20 test files use `db/`-prefixed includes (e.g., `#include "db/crypto/hash.h"`) which resolve via `target_include_directories(chromatindb_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)`. Since db/tests/ is one level deeper than the old tests/ directory but the include path is set on the target (not relative), no include path changes are needed in any test file.

The only file outside CMakeLists.txt files that references `tests/` is... nothing. The Dockerfile builds with `-DBUILD_TESTING=OFF` and does not copy the tests directory. No CI configs, scripts, Dockerfiles, or other config files reference the tests/ path.

**Primary recommendation:** Execute as a single atomic operation: git mv all 20 files, rewrite both CMakeLists.txt files, verify the build and test count match.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Test executable (`chromatindb_tests`) defined in db/CMakeLists.txt, not in the top-level CMakeLists.txt
- Guarded Catch2 FetchContent added to db/CMakeLists.txt (`if(NOT TARGET Catch2::Catch2WithMain)`)
- Tests guarded behind `BUILD_TESTING` (standard CMake variable, no custom option)
- `catch_discover_tests()` used for per-test CTest granularity (same as current)
- Executable name stays `chromatindb_tests`
- Standalone build support: db/CMakeLists.txt includes `include(CTest)` behind `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)` guard
- Top-level keeps `include(CTest)` (sets BUILD_TESTING for subdirectories)
- Entire `if(BUILD_TESTING)...endif()` block removed from top-level (no placeholder, no comment)
- Catch2 FetchContent_Declare removed from top-level (no Catch2 mention at all)
- Catch2 removed from `FetchContent_MakeAvailable()` call
- Use `git mv` to preserve file history/rename tracking
- Top-level `tests/` directory deleted entirely after relocation (no .gitkeep, no README)
- All references to `tests/` paths across the project audited and fixed (CMake, Dockerfile, scripts, configs)
- db/tests/ mirrors db/ source subdirectory structure: db/tests/crypto/, db/tests/net/, db/tests/engine/, etc.
- test_daemon.cpp placed at db/tests/ root (not in a subdirectory)
- Include paths inside test files unchanged (already use db/-prefixed includes)

### Claude's Discretion
- Exact ordering of files in the CMake `add_executable()` source list
- Whether to alphabetize or group by module in the CMake file

### Deferred Ideas (OUT OF SCOPE)
None.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| CLEAN-01 | Tests relocated into db/ directory with CTest discovery preserved (284 tests before = 284 tests after) | All 20 test files identified, target directory structure mapped, CMake wiring pattern verified. NOTE: actual current test count is 313 (not 284 from v0.6.0 stats) -- planner must use actual count at execution time. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| CMake | 3.20+ | Build system | Already in use, minimum version set in project |
| Catch2 | v3.7.1 | Test framework | Already in use, provides `catch_discover_tests()` for CTest integration |
| CTest | (bundled with CMake) | Test runner/discovery | Already in use via `include(CTest)` |

### Supporting
No new libraries needed. This is a pure restructuring phase.

### Alternatives Considered
None -- all decisions are locked.

## Architecture Patterns

### Current Structure (Before)
```
chromatin-protocol/
  CMakeLists.txt          # top-level: owns Catch2 FetchContent, test target, CTest
  db/
    CMakeLists.txt        # library target only (chromatindb_lib)
    crypto/
    net/
    ...
  tests/                  # separate top-level directory
    crypto/test_hash.cpp
    crypto/test_signing.cpp
    ...
    test_daemon.cpp
```

### Target Structure (After)
```
chromatin-protocol/
  CMakeLists.txt          # top-level: no Catch2, no test target, keeps include(CTest)
  db/
    CMakeLists.txt        # library + test target + Catch2 FetchContent
    crypto/
    net/
    ...
    tests/                # co-located tests
      crypto/test_hash.cpp
      crypto/test_signing.cpp
      crypto/test_kem.cpp
      crypto/test_aead.cpp
      crypto/test_kdf.cpp
      crypto/test_master_key.cpp
      wire/test_codec.cpp
      config/test_config.cpp
      identity/test_identity.cpp
      storage/test_storage.cpp
      engine/test_engine.cpp
      net/test_framing.cpp
      net/test_protocol.cpp
      net/test_handshake.cpp
      net/test_connection.cpp
      net/test_server.cpp
      sync/test_sync_protocol.cpp
      peer/test_peer_manager.cpp
      acl/test_access_control.cpp
      test_daemon.cpp
```

### Pattern 1: Guarded FetchContent for Catch2
**What:** Add Catch2 FetchContent to db/CMakeLists.txt using the same guard pattern as all other dependencies.
**When to use:** When db/ is built as a subdirectory (top-level already fetched Catch2 -- guard skips). When db/ is built standalone (guard triggers, Catch2 fetched).
**Example:**
```cmake
# In db/CMakeLists.txt, inside if(BUILD_TESTING) block:
if(NOT TARGET Catch2::Catch2WithMain)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(Catch2)
endif()
```

**CRITICAL:** After `FetchContent_MakeAvailable(Catch2)`, the Catch2 CMakeLists.txt adds `extras/` to `CMAKE_MODULE_PATH` and propagates to parent scope. This makes `include(Catch)` resolvable. The `include(Catch)` call MUST come after the Catch2 FetchContent block.

### Pattern 2: BUILD_TESTING Guard with Standalone Detection
**What:** Tests are only built when BUILD_TESTING is ON. Standalone build includes CTest automatically.
**Example:**
```cmake
# At top of db/CMakeLists.txt test section:
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(CTest)
endif()

if(BUILD_TESTING)
  # Catch2 FetchContent (guarded)
  # add_executable(chromatindb_tests ...)
  # target_link_libraries(...)
  # include(Catch)
  # catch_discover_tests(chromatindb_tests)
endif()
```

### Pattern 3: Top-level FetchContent_MakeAvailable Cleanup
**What:** Remove Catch2 from the top-level FetchContent_MakeAvailable call.
**Current top-level line:**
```cmake
FetchContent_MakeAvailable(liboqs sodium flatbuffers Catch2 spdlog json libmdbx asiocmake)
```
**After:**
```cmake
FetchContent_MakeAvailable(liboqs sodium flatbuffers spdlog json libmdbx asiocmake)
```
Also remove the `FetchContent_Declare(Catch2 ...)` block entirely from the top-level.

### Anti-Patterns to Avoid
- **Moving Catch2 guard outside BUILD_TESTING:** Catch2 should only be fetched when tests are being built. The guard must be inside the `if(BUILD_TESTING)` block.
- **Forgetting include(Catch) after FetchContent:** The Catch.cmake module is provided by Catch2. If `include(Catch)` is called before Catch2 is available, CMake will fail.
- **Using relative paths in add_executable:** Test file paths in `add_executable()` should be relative to `CMAKE_CURRENT_SOURCE_DIR` (i.e., db/), so they should be `tests/crypto/test_hash.cpp` etc.
- **Leaving stale FetchContent_Declare in top-level:** If Catch2 FetchContent_Declare remains in the top-level but is not in the MakeAvailable call, it's dead code. Remove it entirely.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Test discovery | Manual CTest add_test() calls | `catch_discover_tests()` | Automatically registers all TEST_CASEs; handles SECTION nesting |
| Module path setup | Manual CMAKE_MODULE_PATH manipulation | `FetchContent_MakeAvailable(Catch2)` | Catch2 sets up its own module path when fetched |

## Common Pitfalls

### Pitfall 1: CMAKE_MODULE_PATH Not Set When include(Catch) Runs
**What goes wrong:** `include(Catch)` fails with "Could not find module Catch" because Catch2 hasn't been fetched yet.
**Why it happens:** The `include(Catch)` call appears before the Catch2 FetchContent block, or the FetchContent guard skips fetching but the top-level also didn't fetch it.
**How to avoid:** Place `include(Catch)` AFTER the Catch2 guarded FetchContent block. When built as subdirectory of top-level, the top-level's `FetchContent_MakeAvailable` runs first (top-level CMakeLists.txt is processed before `add_subdirectory(db)`), but since we are REMOVING Catch2 from top-level, db/ MUST handle it. The key insight: when the top-level removes Catch2 from its MakeAvailable, the only place Catch2 gets fetched is inside db/'s `if(BUILD_TESTING)` block. This is correct behavior.
**Warning signs:** CMake configure step fails with "include could not find requested file: Catch".

### Pitfall 2: Test Count Mismatch Due to Stale Build Directory
**What goes wrong:** After relocation, `ctest -N` shows 0 tests or a wrong count because the build directory has stale CMake cache entries pointing to old test file paths.
**Why it happens:** CMake caches file paths. Moving files without reconfiguring leaves stale references.
**How to avoid:** After the git mv + CMakeLists.txt changes, do a clean reconfigure: delete `build/` and run `cmake -S . -B build` fresh. Alternatively, `cmake -S . -B build` from scratch. Do NOT rely on incremental reconfigure for a structural change like this.
**Warning signs:** Build errors about missing source files, or test count is wrong.

### Pitfall 3: git mv Target Directory Must Exist
**What goes wrong:** `git mv tests/crypto/test_hash.cpp db/tests/crypto/test_hash.cpp` fails because `db/tests/crypto/` doesn't exist.
**Why it happens:** git mv does not create intermediate directories.
**How to avoid:** Create the target directory structure first: `mkdir -p db/tests/{crypto,wire,config,identity,storage,engine,net,sync,peer,acl}` before running git mv commands.
**Warning signs:** git mv errors about destination not existing.

### Pitfall 4: Top-Level FetchContent_Declare Without MakeAvailable
**What goes wrong:** Catch2 FetchContent_Declare left in top-level as dead code after removing it from MakeAvailable.
**Why it happens:** Only removing from the MakeAvailable call but forgetting the Declare block.
**How to avoid:** Remove BOTH the FetchContent_Declare(Catch2 ...) block AND the reference in FetchContent_MakeAvailable() from the top-level CMakeLists.txt.

### Pitfall 5: Incorrect Test Count Expectation
**What goes wrong:** Verification fails because the expected count (284) doesn't match reality.
**Why it happens:** The 284 count is from v0.6.0 project stats. The actual current count is 313 (verified via `ctest -N` on the current build). Tests were added during v0.6.0 development after that stat was recorded.
**How to avoid:** Record the actual test count BEFORE relocation (`ctest -N | grep "Total Tests:"`), then verify the SAME count AFTER relocation. Do not hardcode 284.
**Warning signs:** Count doesn't match expected value.

## Code Examples

### Complete db/CMakeLists.txt Test Section (to append)
```cmake
# =============================================================================
# Standalone build support
# =============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(CTest)
endif()

# =============================================================================
# Test target
# =============================================================================
if(BUILD_TESTING)
  # -- Catch2 v3.7.1 (testing framework)
  if(NOT TARGET Catch2::Catch2WithMain)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG        v3.7.1
      GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
  endif()

  add_executable(chromatindb_tests
    tests/crypto/test_hash.cpp
    tests/crypto/test_signing.cpp
    tests/crypto/test_kem.cpp
    tests/crypto/test_aead.cpp
    tests/crypto/test_kdf.cpp
    tests/crypto/test_master_key.cpp
    tests/wire/test_codec.cpp
    tests/config/test_config.cpp
    tests/identity/test_identity.cpp
    tests/storage/test_storage.cpp
    tests/engine/test_engine.cpp
    tests/net/test_framing.cpp
    tests/net/test_protocol.cpp
    tests/net/test_handshake.cpp
    tests/net/test_connection.cpp
    tests/net/test_server.cpp
    tests/sync/test_sync_protocol.cpp
    tests/peer/test_peer_manager.cpp
    tests/acl/test_access_control.cpp
    tests/test_daemon.cpp
  )

  target_link_libraries(chromatindb_tests PRIVATE
    chromatindb_lib
    Catch2::Catch2WithMain
  )

  include(Catch)
  catch_discover_tests(chromatindb_tests)
endif()
```

### Top-Level CMakeLists.txt Changes
```cmake
# REMOVE these lines entirely:
#   FetchContent_Declare(Catch2 ...)  (4 lines)
#   The entire if(BUILD_TESTING)...endif() block (lines ~155-178)

# CHANGE this line:
FetchContent_MakeAvailable(liboqs sodium flatbuffers Catch2 spdlog json libmdbx asiocmake)
# TO:
FetchContent_MakeAvailable(liboqs sodium flatbuffers spdlog json libmdbx asiocmake)

# KEEP these lines (unchanged):
include(CTest)  # remains -- sets BUILD_TESTING for subdirectories
add_subdirectory(db)  # remains -- db/ now owns tests
```

### git mv Sequence
```bash
# Create target directories
mkdir -p db/tests/{crypto,wire,config,identity,storage,engine,net,sync,peer,acl}

# Move files (preserving git history)
git mv tests/crypto/test_hash.cpp db/tests/crypto/
git mv tests/crypto/test_signing.cpp db/tests/crypto/
git mv tests/crypto/test_kem.cpp db/tests/crypto/
git mv tests/crypto/test_aead.cpp db/tests/crypto/
git mv tests/crypto/test_kdf.cpp db/tests/crypto/
git mv tests/crypto/test_master_key.cpp db/tests/crypto/
git mv tests/wire/test_codec.cpp db/tests/wire/
git mv tests/config/test_config.cpp db/tests/config/
git mv tests/identity/test_identity.cpp db/tests/identity/
git mv tests/storage/test_storage.cpp db/tests/storage/
git mv tests/engine/test_engine.cpp db/tests/engine/
git mv tests/net/test_framing.cpp db/tests/net/
git mv tests/net/test_protocol.cpp db/tests/net/
git mv tests/net/test_handshake.cpp db/tests/net/
git mv tests/net/test_connection.cpp db/tests/net/
git mv tests/net/test_server.cpp db/tests/net/
git mv tests/sync/test_sync_protocol.cpp db/tests/sync/
git mv tests/peer/test_peer_manager.cpp db/tests/peer/
git mv tests/acl/test_access_control.cpp db/tests/acl/
git mv tests/test_daemon.cpp db/tests/

# Remove empty tests/ directory (git rm handles this after all files moved)
# git will automatically remove the empty directory from tracking
```

## Inventory

### Complete File Inventory (20 test files)
| # | Current Path | Target Path |
|---|-------------|-------------|
| 1 | tests/crypto/test_hash.cpp | db/tests/crypto/test_hash.cpp |
| 2 | tests/crypto/test_signing.cpp | db/tests/crypto/test_signing.cpp |
| 3 | tests/crypto/test_kem.cpp | db/tests/crypto/test_kem.cpp |
| 4 | tests/crypto/test_aead.cpp | db/tests/crypto/test_aead.cpp |
| 5 | tests/crypto/test_kdf.cpp | db/tests/crypto/test_kdf.cpp |
| 6 | tests/crypto/test_master_key.cpp | db/tests/crypto/test_master_key.cpp |
| 7 | tests/wire/test_codec.cpp | db/tests/wire/test_codec.cpp |
| 8 | tests/config/test_config.cpp | db/tests/config/test_config.cpp |
| 9 | tests/identity/test_identity.cpp | db/tests/identity/test_identity.cpp |
| 10 | tests/storage/test_storage.cpp | db/tests/storage/test_storage.cpp |
| 11 | tests/engine/test_engine.cpp | db/tests/engine/test_engine.cpp |
| 12 | tests/net/test_framing.cpp | db/tests/net/test_framing.cpp |
| 13 | tests/net/test_protocol.cpp | db/tests/net/test_protocol.cpp |
| 14 | tests/net/test_handshake.cpp | db/tests/net/test_handshake.cpp |
| 15 | tests/net/test_connection.cpp | db/tests/net/test_connection.cpp |
| 16 | tests/net/test_server.cpp | db/tests/net/test_server.cpp |
| 17 | tests/sync/test_sync_protocol.cpp | db/tests/sync/test_sync_protocol.cpp |
| 18 | tests/peer/test_peer_manager.cpp | db/tests/peer/test_peer_manager.cpp |
| 19 | tests/acl/test_access_control.cpp | db/tests/acl/test_access_control.cpp |
| 20 | tests/test_daemon.cpp | db/tests/test_daemon.cpp |

### Include Path Verification
All 20 test files use `db/`-prefixed includes (e.g., `#include "db/crypto/hash.h"`). These resolve via chromatindb_lib's PUBLIC include directory (`$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>`), which exposes the parent of db/ as an include root. Since the test executable links chromatindb_lib, this include path propagates automatically. No test file needs editing.

### External References Audit
| File/Config | References tests/? | Action Needed |
|-------------|-------------------|---------------|
| CMakeLists.txt (top-level) | YES -- test target, Catch2 FetchContent | Rewrite (remove test block, remove Catch2) |
| db/CMakeLists.txt | NO | Add test section |
| Dockerfile | NO (uses -DBUILD_TESTING=OFF) | None |
| deploy/*.sh | NO | None |
| deploy/*.yml | NO | None |
| deploy/configs/*.json | NO | None |
| .gitignore | NO | None |
| .clang-format | Does not exist | None |
| .github/ | Does not exist | None |

## State of the Art

This phase uses well-established CMake patterns. No version-sensitive concerns.

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Separate top-level test dir | Co-located tests in component dir | CMake best practice (long-standing) | Better component isolation, enables standalone builds |
| CTest add_test() manual | catch_discover_tests() auto | Catch2 v3 (2022) | Automatic per-TEST_CASE registration |

## Open Questions

1. **Actual test count at execution time**
   - What we know: Current build reports 313 tests via `ctest -N`. The requirement CLEAN-01 says 284. The 284 number is from v0.6.0 project memory stats and is stale.
   - What's unclear: Whether the requirement should be updated to 313 or the 284 is intentionally aspirational.
   - Recommendation: Use actual count. Record `ctest -N` count BEFORE relocation, verify exact same count AFTER. Update CLEAN-01 acceptance criteria if needed. The spirit of the requirement is "no tests lost during relocation" -- the absolute number is less important than the before/after match.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | None (Catch2 is header-only + CMake module) |
| Quick run command | `cd build && ctest -N \| grep "Total Tests:"` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CLEAN-01 | All tests discoverable after relocation | smoke | `cd build && ctest -N \| grep "Total Tests:"` (compare before/after) | N/A -- verification is count comparison |
| CLEAN-01 | All tests pass after relocation | integration | `cd build && ctest --output-on-failure` | N/A -- existing tests, just relocated |
| CLEAN-01 | No test files remain in top-level tests/ | smoke | `ls tests/ 2>/dev/null && echo FAIL \|\| echo PASS` | N/A -- filesystem check |
| CLEAN-01 | Build succeeds from clean configure | smoke | `rm -rf build && cmake -S . -B build && cmake --build build` | N/A -- build verification |

### Sampling Rate
- **Per task commit:** `cd build && cmake -S .. -B . && cmake --build . && ctest -N | grep "Total Tests:"`
- **Per wave merge:** `cd build && ctest --output-on-failure` (full suite)
- **Phase gate:** Full suite green + test count match + no files in tests/

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. No new test files, config, or fixtures needed. This phase moves existing tests, not creates new ones.

## Sources

### Primary (HIGH confidence)
- Direct filesystem inspection of all 20 test files, both CMakeLists.txt files, Dockerfile, deploy/ directory
- `ctest -N` output from current build (313 tests)
- Catch2 CMakeLists.txt inspection (CMAKE_MODULE_PATH propagation verified)

### Secondary (MEDIUM confidence)
- CMake FetchContent documentation (well-established pattern, used by all 8 existing dependencies in db/CMakeLists.txt)

### Tertiary (LOW confidence)
None -- all findings are from direct codebase inspection.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all patterns already in codebase
- Architecture: HIGH - straightforward directory move with well-understood CMake patterns
- Pitfalls: HIGH - verified through direct inspection of CMake module path propagation and include resolution

**Research date:** 2026-03-17
**Valid until:** Indefinite (structural CMake patterns, no version sensitivity)
