---
phase: 108-live-feature-verification
plan: 01
subsystem: testing
tags: [websocket, e2e-testing, relay, ml-dsa-87, rate-limiting, sigterm, sighup]

# Dependency graph
requires:
  - phase: 107-message-type-verification
    provides: relay_smoke_test.cpp with 31 protocol correctness tests
provides:
  - "Shared test helpers header (relay_test_helpers.h) with TCP, WebSocket, auth, blob signing utilities"
  - "connect_and_auth() reusable authenticated WebSocket connection helper"
  - "ws_recv_frame_raw() close frame detection for SIGTERM verification"
  - "rewrite_config() JSON config modification for SIGHUP tests"
  - "relay_feature_test skeleton with CLI parsing and four test function stubs"
affects: [108-02-PLAN]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Header-only shared test helpers with inline functions in namespace relay_test"
    - "WsFrame.close_code field for close frame status code capture"
    - "connect_and_auth() single-call helper for authenticated WS connection setup"

key-files:
  created:
    - tools/relay_test_helpers.h
    - tools/relay_feature_test.cpp
  modified:
    - tools/relay_smoke_test.cpp
    - tools/CMakeLists.txt

key-decisions:
  - "Header-only shared helpers (inline functions) over separate library target -- simpler build, no ODR risk with two translation units"
  - "Added WsFrame.close_code field to struct for SIGTERM close frame verification (E2E-05)"
  - "connect_and_auth() copies exact WebSocket upgrade and auth logic from smoke test rather than refactoring main()"

patterns-established:
  - "Pattern: relay_test namespace with inline helpers for all relay E2E test binaries"
  - "Pattern: ws_recv_frame_raw() preserves close frames while ws_recv_frame() swallows them"
  - "Pattern: relay_feature_test CLI requires --relay-pid and --config for signal delivery and config modification"

requirements-completed: []

# Metrics
duration: 11min
completed: 2026-04-11
---

# Phase 108 Plan 01: Shared Test Infrastructure Summary

**Shared test helpers header extracted from smoke test with connect_and_auth, ws_recv_frame_raw, and relay_feature_test skeleton ready for E2E-02 through E2E-05 implementation**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-11T13:20:10Z
- **Completed:** 2026-04-11T13:31:21Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Extracted ~220 lines of TCP, WebSocket, auth, and test tracking helpers from relay_smoke_test.cpp into reusable relay_test_helpers.h header
- Added three new utility functions needed by Plan 02: connect_and_auth() for multi-client tests, ws_recv_frame_raw() for close frame detection, rewrite_config() for SIGHUP config modification
- Created relay_feature_test skeleton with full CLI parsing (--identity, --relay-pid, --config, --host, --port), PID validation, and four stub test functions called in correct order (SIGTERM last)

## Task Commits

Each task was committed atomically:

1. **Task 1: Extract shared helpers header and refactor smoke test** - `a243b5e` (feat)
2. **Task 2: Create relay_feature_test skeleton with CLI and CMake target** - `46eb20d` (feat)

## Files Created/Modified
- `tools/relay_test_helpers.h` - Header-only shared helpers: TCP, WebSocket framing, auth, blob signing, test tracking, connect_and_auth, ws_recv_frame_raw, rewrite_config
- `tools/relay_feature_test.cpp` - Feature test skeleton with CLI parsing, PID validation, four stub test functions
- `tools/relay_smoke_test.cpp` - Refactored to use shared header (removed ~220 lines of duplicated helpers)
- `tools/CMakeLists.txt` - Added relay_feature_test build target

## Decisions Made
- Used header-only pattern (inline functions in namespace relay_test) rather than a separate static library -- simpler CMake, avoids linker issues, both binaries are small
- Kept smoke test main() inline connect/upgrade/auth code as-is rather than refactoring to use connect_and_auth() -- avoids risking the 31 passing tests, connect_and_auth is for the new feature test
- Added close_code field to WsFrame struct even though ws_recv_frame() still swallows close frames -- ws_recv_frame_raw() uses it, ws_recv_frame() ignores it (backwards compatible)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Worktree build directory cannot be initialized due to FetchContent dependency fetch failures (asio-populate). Verified compilation by temporarily copying files to main repo build directory, then restoring. Both relay_smoke_test and relay_feature_test compile and link successfully.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plan 02 has full shared infrastructure ready: connect_and_auth for multi-client pub/sub, ws_recv_frame_raw for SIGTERM close frame detection, rewrite_config for SIGHUP config changes
- All four test function signatures are declared and callable -- Plan 02 implements the bodies
- relay_feature_test CLI parses all required arguments (--relay-pid, --config) for signal delivery and config modification tests

## Self-Check: PASSED

---
*Phase: 108-live-feature-verification*
*Completed: 2026-04-11*
