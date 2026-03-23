---
phase: 58-relay-scaffolding
plan: 01
subsystem: relay
tags: [cmake, json-config, validation, relay, c++20]

# Dependency graph
requires:
  - phase: 57-client-protocol-extensions
    provides: "chromatindb_lib with client protocol handlers"
provides:
  - "relay/ directory with chromatindb_relay_lib static library"
  - "RelayConfig struct with 6 fields and defaults"
  - "load_relay_config with required-file semantics and JSON parsing"
  - "validate_relay_config with error-accumulating validation"
  - "chromatindb_relay binary target (placeholder main)"
  - "15 unit tests for relay config"
affects: [58-02-relay-identity-entry-point]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "relay/ as separate CMake subdirectory with own static library"
    - "uint32_t for config port fields to catch JSON out-of-range values"
    - "Error-accumulating validation pattern reused from node config"

key-files:
  created:
    - relay/config/relay_config.h
    - relay/config/relay_config.cpp
    - relay/CMakeLists.txt
    - relay/relay_main.cpp
    - db/tests/relay/test_relay_config.cpp
  modified:
    - CMakeLists.txt
    - db/CMakeLists.txt

key-decisions:
  - "bind_port as uint32_t (not uint16_t) to catch out-of-range JSON values in validation"
  - "Relay config requires file (throws on missing) unlike node config which returns defaults"

patterns-established:
  - "relay/ directory: own CMakeLists.txt, chromatindb_relay_lib links chromatindb_lib"
  - "Relay tests live in db/tests/relay/ and link chromatindb_relay_lib"

requirements-completed: [RELAY-06, RELAY-07, RELAY-08]

# Metrics
duration: 6min
completed: 2026-03-23
---

# Phase 58 Plan 01: Relay Scaffolding Summary

**Relay directory structure, CMake integration, and JSON config with error-accumulating validation (15 tests, zero new dependencies)**

## Performance

- **Duration:** 6 min
- **Started:** 2026-03-23T14:52:39Z
- **Completed:** 2026-03-23T14:58:53Z
- **Tasks:** 1
- **Files modified:** 7

## Accomplishments
- Created relay/ directory with its own CMakeLists.txt and static library target
- Implemented RelayConfig struct with all 6 fields (bind_address, bind_port, uds_path, identity_key_path, log_level, log_file) and sensible defaults
- load_relay_config enforces required config file (throws on missing/invalid JSON, per D-04)
- validate_relay_config accumulates all errors and reports them at once (per D-06)
- 15 test cases covering valid/partial load, port bounds, required fields, error accumulation, malformed JSON, missing file, log_level, uds_path constraints, bind_address
- chromatindb_relay binary compiles and exits 0 (placeholder main for Plan 02)

## Task Commits

Each task was committed atomically (TDD RED/GREEN):

1. **Task 1 RED: Failing tests for relay config** - `e6a6239` (test)
2. **Task 1 GREEN: Implement relay config parsing and validation** - `149c0ca` (feat)

## Files Created/Modified
- `relay/config/relay_config.h` - RelayConfig struct, load/validate declarations
- `relay/config/relay_config.cpp` - JSON parsing, error-accumulating validation
- `relay/CMakeLists.txt` - chromatindb_relay_lib static library target
- `relay/relay_main.cpp` - Placeholder main() for binary compilation
- `db/tests/relay/test_relay_config.cpp` - 15 test cases, 41 assertions
- `CMakeLists.txt` - add_subdirectory(relay), chromatindb_relay binary target
- `db/CMakeLists.txt` - Test file added, chromatindb_relay_lib linked to test target

## Decisions Made
- **bind_port as uint32_t**: Using uint32_t instead of uint16_t allows validate_relay_config to catch out-of-range values (e.g., 70000 from JSON) uniformly. The validation checks 1-65535 range.
- **Required config file**: Unlike the node config which returns defaults for missing files, relay config throws on missing file per D-04. This is intentional -- relay must be explicitly configured.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Changed bind_port from uint16_t to uint32_t**
- **Found during:** Task 1 RED phase
- **Issue:** Plan specified uint16_t for bind_port, but test "validate_relay_config rejects bind_port=70000" is impossible with uint16_t (value wraps to 4464)
- **Fix:** Changed to uint32_t so validation can catch out-of-range JSON values
- **Files modified:** relay/config/relay_config.h
- **Verification:** Test for bind_port=70000 works correctly, validation rejects it
- **Committed in:** e6a6239 (RED commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Minor type width change for correctness. No scope creep.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- relay/ directory and build integration complete
- chromatindb_relay_lib available for Plan 02 to build the main entry point and identity management on
- Config struct ready to be loaded and validated in relay_main.cpp
- Test infrastructure in place (db/tests/relay/)

## Self-Check: PASSED

- All 5 created files verified present
- Both commit hashes (e6a6239, 149c0ca) verified in git log

---
*Phase: 58-relay-scaffolding*
*Completed: 2026-03-23*
