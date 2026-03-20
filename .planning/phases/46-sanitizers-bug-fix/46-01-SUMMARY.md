---
phase: 46-sanitizers-bug-fix
plan: 01
subsystem: infra
tags: [cmake, asan, sanitizer, coroutine, mdbx, aead]

# Dependency graph
requires:
  - phase: 45-verification-docs
    provides: "v0.9.0 codebase with 408+ Catch2 tests"
provides:
  - "SANITIZER cmake enum option (asan|tsan|ubsan)"
  - "Suppression files for third-party sanitizer findings"
  - "ASAN-clean codebase (469 tests, zero findings in db/ code)"
  - "Fixed coroutine parameter lifetime bug (const ref -> value)"
  - "Fixed AEAD nonce desync from concurrent SyncRejected writes"
  - "Fixed MDBX geometry under ASAN shadow memory"
affects: [46-02, sanitizer, testing]

# Tech tracking
tech-stack:
  added: [SANITIZER cmake option, lsan.supp]
  patterns: [coroutine-params-by-value, asan-geometry-reduction, no-detached-write-during-sync]

key-files:
  created:
    - sanitizers/asan.supp
    - sanitizers/tsan.supp
    - sanitizers/ubsan.supp
    - sanitizers/lsan.supp
  modified:
    - CMakeLists.txt
    - db/net/server.cpp
    - db/net/server.h
    - db/storage/storage.cpp
    - db/peer/peer_manager.cpp

key-decisions:
  - "Coroutine parameters must be by value, never by reference -- C++ coroutine pitfall"
  - "MDBX geometry.size_upper reduced to 1 GiB under __SANITIZE_ADDRESS__ to avoid virtual address space conflict"
  - "SyncRequest silently dropped when peer is syncing instead of spawning detached SyncRejected coroutine (avoids AEAD nonce desync)"
  - "LSAN suppressions for liboqs and asio leaks (third-party, not our code)"

patterns-established:
  - "Coroutine params by value: all coroutine function parameters MUST be by value (std::string address, not const std::string&) to avoid stack-use-after-scope"
  - "No detached writes during sync: when peer->syncing is true, never spawn detached coroutines that write to the same connection"
  - "ASAN geometry: use #if defined(__SANITIZE_ADDRESS__) to reduce mmap upper limits under ASAN"

requirements-completed: [SAN-01]

# Metrics
duration: 142min
completed: 2026-03-20
---

# Phase 46 Plan 01: CMake SANITIZER Enum + ASAN Clean Pass Summary

**SANITIZER cmake enum (asan/tsan/ubsan) replacing ENABLE_ASAN, plus three ASAN bugs fixed: coroutine ref-param lifetime, MDBX geometry under shadow memory, and AEAD nonce desync from concurrent SyncRejected writes. Full suite 469 tests pass with zero ASAN findings in db/ code.**

## Performance

- **Duration:** 142 min
- **Started:** 2026-03-20T11:08:38Z
- **Completed:** 2026-03-20T13:31:15Z
- **Tasks:** 2
- **Files modified:** 9 (4 created, 5 modified)

## Accomplishments
- Replaced ENABLE_ASAN boolean with SANITIZER enum supporting asan, tsan, and ubsan (mutually exclusive, rejects invalid values)
- Fixed stack-use-after-scope in reconnect_loop/connect_to_peer coroutines (const ref parameters to by-value)
- Fixed MDBX_TOO_LARGE under ASAN by reducing geometry.size_upper from 64 GiB to 1 GiB when ASAN is active
- Fixed AEAD nonce desync caused by send_sync_rejected spawning detached write coroutines concurrent with sync initiator writes
- Full Catch2 suite: 469 tests, 1692 assertions, zero ASAN findings in db/ code

## Task Commits

Each task was committed atomically:

1. **Task 1: Replace ENABLE_ASAN with SANITIZER enum and create suppression files** - `9561649` (feat)
2. **Task 2: ASAN build and clean test run** - `0c65bd9` (fix)

## Files Created/Modified
- `CMakeLists.txt` - SANITIZER enum option replacing ENABLE_ASAN
- `sanitizers/asan.supp` - ASAN suppression file (empty, no third-party ASAN errors found)
- `sanitizers/tsan.supp` - TSAN suppression file (placeholder for Phase 46 Plan 02)
- `sanitizers/ubsan.supp` - UBSAN suppression file (placeholder for Phase 46 Plan 02)
- `sanitizers/lsan.supp` - LSAN suppression file for liboqs and asio leaks
- `db/net/server.cpp` - Coroutine params changed from const ref to value
- `db/net/server.h` - Coroutine params changed from const ref to value
- `db/storage/storage.cpp` - ASAN-conditional geometry reduction
- `db/peer/peer_manager.cpp` - Session-limit check moved before cooldown; SyncRequest silently dropped when syncing

## Decisions Made
- **Coroutine params by value:** C++ coroutine functions must take string parameters by value, not by reference. The coroutine frame copies the reference (not the value), and the original may go out of scope while the coroutine is suspended. This is a known C++ coroutine pitfall.
- **ASAN geometry reduction:** Under ASAN, shadow memory consumes 1/8 of virtual address space. The 64 GiB `size_upper` for mdbx fails with MDBX_TOO_LARGE. Reduced to 1 GiB under `__SANITIZE_ADDRESS__`, which is more than enough for tests.
- **Silent drop instead of SyncRejected:** When `peer->syncing` is true, the sync initiator coroutine owns the connection's send path. Spawning a detached coroutine to send SyncRejected races with the initiator's writes, causing AEAD nonce desync. The fix: silently drop the incoming SyncRequest. The remote initiator times out after 5s and retries next interval.
- **LSAN suppressions:** liboqs OQS_SIG contexts and asio resolver results leak when test coroutines are cancelled early by `ioc.run_for()` timeout. These are third-party cleanup issues, not our code.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed stack-use-after-scope in coroutine parameters**
- **Found during:** Task 2 (ASAN test run)
- **Issue:** `reconnect_loop` and `connect_to_peer` took `const std::string&` parameters. When called from `connect_once` with a temporary string, the coroutine frame held a dangling reference after the caller returned.
- **Fix:** Changed both functions to take `std::string` by value in header and source.
- **Files modified:** db/net/server.h, db/net/server.cpp
- **Verification:** ASAN no longer reports stack-use-after-scope
- **Committed in:** 0c65bd9 (Task 2 commit)

**2. [Rule 1 - Bug] Fixed MDBX_TOO_LARGE under ASAN**
- **Found during:** Task 2 (ASAN test run)
- **Issue:** ASAN shadow memory (1/8 of virtual address space) conflicts with mdbx's 64 GiB `size_upper` mmap, causing 192 test failures with MDBX_TOO_LARGE.
- **Fix:** Added compile-time check for `__SANITIZE_ADDRESS__` to reduce geometry.size_upper to 1 GiB under ASAN.
- **Files modified:** db/storage/storage.cpp
- **Verification:** All 469 tests pass under ASAN (previously 192 MDBX failures)
- **Committed in:** 0c65bd9 (Task 2 commit)

**3. [Rule 1 - Bug] Fixed AEAD nonce desync from concurrent SyncRejected writes**
- **Found during:** Task 2 (ASAN test run)
- **Issue:** When both nodes initiate sync simultaneously (same-second sync_interval), the responder's `send_sync_rejected` spawned a detached coroutine that wrote to the connection concurrently with the sync initiator's writes. This corrupted the AEAD nonce sequence, causing "AEAD decrypt failed" on the other side.
- **Fix:** Moved session-limit check before cooldown check. When `peer->syncing`, silently drop the incoming SyncRequest instead of spawning a detached write. Remote initiator times out (5s) and retries.
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** All 469 tests pass including previously-flaky ACL and storage-full tests
- **Committed in:** 0c65bd9 (Task 2 commit)

---

**Total deviations:** 3 auto-fixed (3 bugs)
**Impact on plan:** All auto-fixes were real bugs discovered by ASAN. The nonce desync bug is particularly significant -- it could cause intermittent connection drops in production when sync intervals align.

## Issues Encountered
- ASAN build required ~196s for cmake configure (FetchContent downloads) and several minutes for compilation. Normal build times are significantly faster.
- LSAN detected leaks in liboqs and asio coroutine frames -- all third-party, suppressed via lsan.supp.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- SANITIZER enum ready for TSAN and UBSAN passes (Plan 46-02)
- Suppression files ready to be populated with any TSAN/UBSAN third-party findings
- Three bugs fixed that would have affected TSAN/UBSAN runs as well
- The nonce desync fix is critical for TSAN -- it would have been flagged as a data race

---
*Phase: 46-sanitizers-bug-fix*
*Completed: 2026-03-20*
