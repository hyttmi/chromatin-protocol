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
- **Tasks:** 3 completed (checkpoint approved)
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
3. **Task 3: Live sanitizer validation** - `c79b3e4` (fix) - APPROVED: tap 11/11, smoke 13/13, ASAN clean (shutdown leaks only)

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

- ListRequest payload was 32 bytes, node expects 44 (ns + since_seq + limit). Fixed.
- NamespaceListRequest payload was empty, node expects 36 (after_ns + limit). Fixed.
- TimeRangeRequest payload was 44 bytes, node expects 52 (ns + start_ts + end_ts + limit). Fixed.
- Node sends unsolicited SyncNamespaceAnnounce(62) after handshake — tap tool now drains it before sending requests.
- UBSAN/TSAN builds deferred to Phase 107 (more runtime coverage with full 38-type E2E suite).

## Issues Encountered
- CMake configure failed initially because relay_smoke_test.cpp didn't exist yet (tools/CMakeLists.txt references both targets). Created placeholder file to unblock Task 1 build, replaced with full implementation in Task 2.

## User Setup Required

None - no external service configuration required.

## Live Test Results

- **UDS tap:** 11/11 compound response types captured, response types match expected
- **Smoke test:** 13/13 (TCP connect, WS upgrade, auth, subscribe, 7 compound queries)
- **ASAN:** No runtime violations. Shutdown leaks only (29KB relay, 176B node — coroutine/OQS state at SIGTERM)
- **UBSAN/TSAN:** Deferred to Phase 107 for better coverage

---
*Phase: 106-bug-fixes*
*Status: Complete*
