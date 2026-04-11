---
phase: 106-bug-fixes
plan: 03
subsystem: testing
tags: [uds, websocket, smoke-test, sanitizer, asan, tsan, ubsan, ml-dsa-87]

# Dependency graph
requires:
  - phase: 106-bug-fixes (plans 01, 02)
    provides: fixed compound decoders and audited coroutine patterns
provides:
  - UDS tap tool for capturing binary responses from live node
  - WebSocket smoke test for end-to-end relay validation
  - Binary fixture directory structure
  - Sanitizer validation tooling (ASAN/UBSAN/TSAN)
affects: [107-message-type-verification, 108-live-feature-verification, 110-performance-benchmarking]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Standalone blocking-socket tools (no Asio) for deterministic sanitizer testing"
    - "Client-side masked WS frames for RFC 6455 compliance"

key-files:
  created:
    - tools/relay_uds_tap.cpp
    - tools/relay_smoke_test.cpp
    - tools/CMakeLists.txt
    - relay/tests/fixtures/.gitkeep
  modified:
    - CMakeLists.txt

key-decisions:
  - "Blocking POSIX sockets for tools instead of Asio -- simpler, deterministic, no coroutine complexity for sanitizer runs"
  - "Skip Data(8) write in smoke test -- FlatBuffer encoding requires signed blob, covered by UDS tap instead"
  - "Binary fixture files are gitignored (environment-dependent per D-01) -- directory tracked via .gitkeep"

patterns-established:
  - "tools/ directory for relay diagnostic/testing tools linked against chromatindb_relay_lib"
  - "Client-side WebSocket: manual frame construction with RAND_bytes masking"

requirements-completed: []

# Metrics
duration: 10min
completed: 2026-04-11
---

# Phase 106 Plan 03: UDS Tap Tool + WebSocket Smoke Test Summary

**Blocking-socket UDS tap tool and WebSocket smoke test for capturing binary fixtures and validating relay under ASAN/UBSAN/TSAN sanitizers**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-11T04:21:24Z
- **Completed:** 2026-04-11T04:31:26Z
- **Tasks:** 2 completed, 1 checkpoint pending (human-verify)
- **Files modified:** 5

## Accomplishments
- UDS tap tool connects via TrustedHello + AEAD handshake, sends all 11 compound request types, saves raw binary response payloads to files
- WebSocket smoke test completes ML-DSA-87 challenge-response auth and exercises subscribe + 7 compound query paths with JSON response validation
- Both tools build and link against chromatindb_relay_lib, reusing wire/AEAD/identity code (no hand-rolled implementations)
- Sanitizer run commands documented at top of smoke test for ASAN, UBSAN, TSAN

## Task Commits

Each task was committed atomically:

1. **Task 1: UDS tap tool and binary fixture capture** - `67190b3` (feat)
2. **Task 2: WebSocket smoke test** - `c43de1b` (feat)
3. **Task 3: Live sanitizer validation** - CHECKPOINT PENDING (human-verify)

## Files Created/Modified
- `tools/relay_uds_tap.cpp` - Standalone UDS tap: TrustedHello + AEAD handshake, 11 compound request captures
- `tools/relay_smoke_test.cpp` - WebSocket smoke test: auth handshake, subscribe, 7 compound query validations
- `tools/CMakeLists.txt` - Build targets for both tools linked to chromatindb_relay_lib
- `relay/tests/fixtures/.gitkeep` - Binary fixture directory placeholder
- `CMakeLists.txt` - Added add_subdirectory(tools) for relay tool builds

## Decisions Made
- Used blocking POSIX sockets instead of Asio for tools -- deterministic behavior simplifies sanitizer debugging
- Skipped Data(8) write path in smoke test because it requires a properly signed blob (FlatBuffer encoding with ML-DSA-87 signature). The UDS tap tool covers binary response capture for all compound types.
- Binary fixture files are not committed (environment-dependent). Directory tracked via .gitkeep.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- CMake configure failed initially because relay_smoke_test.cpp didn't exist yet (tools/CMakeLists.txt references both targets). Created placeholder file to unblock Task 1 build, replaced with full implementation in Task 2.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Task 3 (live sanitizer validation) is a checkpoint requiring user to:
  1. Start chromatindb node and relay
  2. Run UDS tap tool to capture binary fixtures
  3. Run smoke test against live relay
  4. Build and test under ASAN, UBSAN, TSAN
- Both tools are built and ready for the user to execute
- After Task 3 approval, Phase 106 is complete and Phase 107 can begin

---
*Phase: 106-bug-fixes*
*Status: Checkpoint pending (Task 3 human-verify)*
