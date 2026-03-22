---
phase: 32-test-relocation
plan: 01
subsystem: testing
tags: [cmake, catch2, test-relocation, component-isolation]

# Dependency graph
requires: []
provides:
  - "db/ self-contained CMake component with library + test targets"
  - "All 20 test files under db/tests/ with subdirectory structure"
  - "Guarded Catch2 FetchContent inside db/CMakeLists.txt BUILD_TESTING block"
affects: [33-crypto-optimization, 34-sync-cursors, 35-quota-system, 36-config-api, 37-hardening]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Guarded FetchContent for test dependencies inside subcomponent"]

key-files:
  created:
    - "db/tests/crypto/test_hash.cpp"
    - "db/tests/crypto/test_signing.cpp"
    - "db/tests/crypto/test_kem.cpp"
    - "db/tests/crypto/test_aead.cpp"
    - "db/tests/crypto/test_kdf.cpp"
    - "db/tests/crypto/test_master_key.cpp"
    - "db/tests/wire/test_codec.cpp"
    - "db/tests/config/test_config.cpp"
    - "db/tests/identity/test_identity.cpp"
    - "db/tests/storage/test_storage.cpp"
    - "db/tests/engine/test_engine.cpp"
    - "db/tests/net/test_framing.cpp"
    - "db/tests/net/test_protocol.cpp"
    - "db/tests/net/test_handshake.cpp"
    - "db/tests/net/test_connection.cpp"
    - "db/tests/net/test_server.cpp"
    - "db/tests/sync/test_sync_protocol.cpp"
    - "db/tests/peer/test_peer_manager.cpp"
    - "db/tests/acl/test_access_control.cpp"
    - "db/tests/test_daemon.cpp"
  modified:
    - "CMakeLists.txt"
    - "db/CMakeLists.txt"

key-decisions:
  - "Catch2 FetchContent guarded with if(NOT TARGET Catch2::Catch2WithMain) inside BUILD_TESTING block"
  - "Test files alphabetized by module then filename in add_executable"
  - "include(CTest) placed in top-level CMake before add_subdirectory(db) to propagate BUILD_TESTING"

patterns-established:
  - "Subcomponent test ownership: db/CMakeLists.txt owns test target and test framework dependency"
  - "Standalone build support: if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR) enables db/ independent builds"

requirements-completed: [CLEAN-01]

# Metrics
duration: 15min
completed: 2026-03-17
---

# Phase 32 Plan 01: Test Relocation Summary

**Relocated all 20 test files from top-level tests/ to db/tests/ with guarded Catch2 FetchContent, making db/ a self-contained CMake component**

## Performance

- **Duration:** 15 min
- **Started:** 2026-03-17T03:36:55Z
- **Completed:** 2026-03-17T03:52:27Z
- **Tasks:** 2
- **Files modified:** 22

## Accomplishments
- Relocated all 20 test source files from tests/ to db/tests/ via git mv (history preserved)
- Removed Catch2 FetchContent and test target from top-level CMakeLists.txt
- Added guarded Catch2 FetchContent + test target to db/CMakeLists.txt inside BUILD_TESTING block
- Verified 313/313 tests pass after relocation with zero failures

## Task Commits

Each task was committed atomically:

1. **Task 1: Relocate test files and rewrite CMakeLists.txt files** - `6ac5cbc` (refactor)
2. **Task 2: Clean build verification and test count match** - verification only, no file changes

## Files Created/Modified
- `db/tests/crypto/test_hash.cpp` - Relocated crypto hash tests
- `db/tests/crypto/test_signing.cpp` - Relocated crypto signing tests
- `db/tests/crypto/test_kem.cpp` - Relocated KEM tests
- `db/tests/crypto/test_aead.cpp` - Relocated AEAD tests
- `db/tests/crypto/test_kdf.cpp` - Relocated KDF tests
- `db/tests/crypto/test_master_key.cpp` - Relocated master key tests
- `db/tests/wire/test_codec.cpp` - Relocated codec tests
- `db/tests/config/test_config.cpp` - Relocated config tests
- `db/tests/identity/test_identity.cpp` - Relocated identity tests
- `db/tests/storage/test_storage.cpp` - Relocated storage tests
- `db/tests/engine/test_engine.cpp` - Relocated engine tests
- `db/tests/net/test_framing.cpp` - Relocated framing tests
- `db/tests/net/test_protocol.cpp` - Relocated protocol tests
- `db/tests/net/test_handshake.cpp` - Relocated handshake tests
- `db/tests/net/test_connection.cpp` - Relocated connection tests
- `db/tests/net/test_server.cpp` - Relocated server tests
- `db/tests/sync/test_sync_protocol.cpp` - Relocated sync protocol tests
- `db/tests/peer/test_peer_manager.cpp` - Relocated peer manager tests
- `db/tests/acl/test_access_control.cpp` - Relocated access control tests
- `db/tests/test_daemon.cpp` - Relocated daemon integration test
- `CMakeLists.txt` - Removed Catch2 FetchContent and test target, added include(CTest)
- `db/CMakeLists.txt` - Added test section with guarded Catch2 and chromatindb_tests target

## Decisions Made
- Catch2 FetchContent guarded with `if(NOT TARGET Catch2::Catch2WithMain)` inside `BUILD_TESTING` block for composability
- Test files alphabetized by module then filename in `add_executable` for readability
- `include(CTest)` placed in top-level CMake before `add_subdirectory(db)` to propagate `BUILD_TESTING`
- Standalone build support added via `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)` guard

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- db/ is now a self-contained CMake component owning library, test target, and all dependencies
- Top-level tests/ directory removed; all test files live under db/tests/
- Ready for Phase 33 (crypto optimization) and all subsequent v0.7.0 phases

## Self-Check: PASSED

- FOUND: 32-01-SUMMARY.md
- FOUND: db/tests/crypto/test_hash.cpp
- FOUND: db/tests/test_daemon.cpp
- FOUND: commit 6ac5cbc
- FOUND: tests/ directory removed

---
*Phase: 32-test-relocation*
*Completed: 2026-03-17*
