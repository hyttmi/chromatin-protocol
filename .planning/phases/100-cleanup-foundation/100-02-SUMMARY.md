---
phase: 100-cleanup-foundation
plan: 02
subsystem: relay
tags: [websocket, session, send-queue, spdlog, liboqs, ml-dsa-87, cmake, asio, coroutines]

# Dependency graph
requires:
  - phase: 100-01
    provides: "Clean repo with old relay/ and sdk/ removed, db/ builds standalone"
provides:
  - "Compilable relay binary with --config, spdlog, identity, signal handling"
  - "Session class with bounded send queue, drain coroutine, overflow disconnect"
  - "RelayConfig JSON loader with validation"
  - "RelayIdentity using liboqs directly (no db/ deps)"
  - "relay/ directory structure with ws/ and translate/ placeholders"
affects: [101-websocket-accept, 102-json-schema, 103-translation, 104-uds-multiplexing]

# Tech tracking
tech-stack:
  added: [standalone relay CMake, relay-specific spdlog init]
  patterns: [deque-based send queue with drain coroutine, timer-cancel wakeup, overflow-disconnect]

key-files:
  created:
    - relay/CMakeLists.txt
    - relay/relay_main.cpp
    - relay/config/relay_config.h
    - relay/config/relay_config.cpp
    - relay/core/session.h
    - relay/core/session.cpp
    - relay/identity/relay_identity.h
    - relay/identity/relay_identity.cpp
    - relay/tests/CMakeLists.txt
    - relay/tests/test_session.cpp
    - relay/tests/test_relay_config.cpp
  modified:
    - CMakeLists.txt

key-decisions:
  - "Session::close() drains pending queue directly (not just via drain coroutine) to prevent hangs when drain is not running"
  - "Relay CMake uses if(NOT TARGET ...) guards for FetchContent to work both in-repo and standalone"
  - "SHA3-256 via oqs/sha3.h include (not just oqs/oqs.h)"

patterns-established:
  - "relay::core::Session: bounded deque send queue with drain coroutine and overflow disconnect"
  - "relay::config: JSON config with required/optional field separation"
  - "relay::identity: liboqs-direct ML-DSA-87 identity (no db/ coupling)"

requirements-completed: [SESS-01, SESS-02, OPS-04]

# Metrics
duration: 11min
completed: 2026-04-09
---

# Phase 100 Plan 02: Relay Scaffold Summary

**Relay v2 binary with bounded per-client Session send queue (deque + drain coroutine), JSON config, ML-DSA-87 identity via liboqs, structured spdlog logging, and SIGTERM/SIGHUP handling**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-09T11:38:45Z
- **Completed:** 2026-04-09T11:49:46Z
- **Tasks:** 1
- **Files modified:** 14

## Accomplishments
- Session class with bounded deque-based send queue, drain coroutine, and immediate overflow disconnect (SESS-01, SESS-02)
- Relay binary compiles, starts with --config, logs startup via spdlog, loads/generates ML-DSA-87 identity, handles SIGTERM/SIGHUP (OPS-04)
- 14 unit tests pass: 7 session send queue tests + 7 config loading/validation tests
- Zero db/ dependencies in relay code -- uses liboqs directly for all crypto

## Task Commits

Each task was committed atomically:

1. **Task 1: Create relay directory structure, standalone CMake, config, identity, and Session class** - `8bf1e2f` (feat)

**Plan metadata:** [pending] (docs: complete plan)

## Files Created/Modified
- `relay/CMakeLists.txt` - Standalone relay build with FetchContent for Asio, spdlog, json, liboqs
- `relay/relay_main.cpp` - Entry point: --config, spdlog init, identity load, signal handling
- `relay/config/relay_config.h` - RelayConfig struct with bind, UDS, identity, queue cap fields
- `relay/config/relay_config.cpp` - JSON config loader with required/optional field separation
- `relay/core/session.h` - Session class: bounded send queue, drain coroutine, close, is_closed
- `relay/core/session.cpp` - Deque + drain + configurable cap (default 256) + overflow disconnect
- `relay/identity/relay_identity.h` - RelayIdentity with ML-DSA-87 via liboqs (no db/ includes)
- `relay/identity/relay_identity.cpp` - OQS_SIG_* for keygen/sign, OQS_SHA3_sha3_256 for hash
- `relay/tests/test_session.cpp` - 7 tests: enqueue, FIFO, capacity, overflow, close, pending fail, configurable cap
- `relay/tests/test_relay_config.cpp` - 7 tests: valid load, missing fields, defaults, nonexistent, port validation
- `relay/tests/CMakeLists.txt` - Test executable linking relay lib + Catch2
- `relay/ws/.gitkeep` - Placeholder for Phase 101 WebSocket code
- `relay/translate/.gitkeep` - Placeholder for Phase 103 translation code
- `CMakeLists.txt` - Added add_subdirectory(relay) and chromatindb_relay binary target

## Decisions Made
- Session::close() directly cancels all pending completion timers in the queue, in addition to waking the drain coroutine. This prevents hanging when close() is called without a drain coroutine running (found during testing -- ASAN caught stack-use-after-scope).
- Relay CMake uses `if(NOT TARGET ...)` guards rather than duplicate FetchContent declarations, working both when built from root (deps already fetched by db/) and standalone.
- SHA3-256 requires explicit `#include <oqs/sha3.h>` in addition to `<oqs/oqs.h>`.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Session::close() must drain pending queue directly**
- **Found during:** Task 1 (Session implementation)
- **Issue:** ASAN detected stack-use-after-scope. When close() is called without a drain coroutine running, pending enqueue coroutines hang forever because their completion timers are never cancelled.
- **Fix:** close() now iterates the send queue and cancels all pending completion timers directly, setting result_ptr to false. Added re-entrancy guard (if closed_ return).
- **Files modified:** relay/core/session.cpp
- **Verification:** All 14 tests pass clean under ASAN
- **Committed in:** 8bf1e2f

**2. [Rule 3 - Blocking] Missing oqs/sha3.h include**
- **Found during:** Task 1 (RelayIdentity compilation)
- **Issue:** OQS_SHA3_sha3_256 not declared -- requires separate sha3.h header, not included in oqs.h
- **Fix:** Added `#include <oqs/sha3.h>` to relay_identity.cpp
- **Files modified:** relay/identity/relay_identity.cpp
- **Verification:** Compilation succeeds
- **Committed in:** 8bf1e2f

**3. [Rule 3 - Blocking] Catch2 module path not available**
- **Found during:** Task 1 (CMake configuration)
- **Issue:** `include(Catch)` failed because Catch2's CMake extras path wasn't in CMAKE_MODULE_PATH
- **Fix:** Added `list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)` before include(Catch)
- **Files modified:** relay/tests/CMakeLists.txt
- **Verification:** CMake configures and tests discover successfully
- **Committed in:** 8bf1e2f

---

**Total deviations:** 3 auto-fixed (1 bug, 2 blocking)
**Impact on plan:** All auto-fixes necessary for correctness and compilation. No scope creep.

## Issues Encountered
- RelayIdentity default constructor is private (by design), so relay_main.cpp cannot default-construct it. Fixed by scoping the identity variable inside the try block.

## User Setup Required
None - no external service configuration required.

## Known Stubs
- `relay/core/session.cpp` `do_send()` (line 80-84): Stub that appends to delivered_ deque instead of writing to WebSocket. Intentional -- replaced in Phase 101 when WebSocket transport is added.

## Next Phase Readiness
- Relay binary compiles and runs with identity, config, and signal handling
- Session send queue ready for WebSocket integration in Phase 101
- ws/ and translate/ directories ready for Phase 101 and 103 additions
- All 647 db/ unit tests + 14 relay tests pass

---
*Phase: 100-cleanup-foundation*
*Completed: 2026-04-09*
