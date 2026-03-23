---
phase: 58-relay-scaffolding
plan: 02
subsystem: relay
tags: [ml-dsa-87, identity, cli, relay, c++20]

# Dependency graph
requires:
  - phase: 58-relay-scaffolding
    plan: 01
    provides: "RelayConfig struct, load_relay_config, validate_relay_config, relay CMake targets"
provides:
  - "RelayIdentity class with generate/load/save/load_or_generate for ML-DSA-87 keypairs"
  - "pub_path_from_key_path SSH-style .key/.pub path convention"
  - "chromatindb_relay CLI with run/keygen/version/help subcommands"
  - "Relay binary loads config, initializes logging, generates/loads identity, logs public key hash"
  - "11 unit tests for relay identity (25 assertions)"
affects: [59-relay-protocol]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "RelayIdentity takes direct key path (not data_dir) per D-07, .pub sibling per D-08"
    - "Relay CLI mirrors node CLI structure (db/main.cpp) per D-01"
    - "load_or_generate auto-creates identity on first run per D-09"

key-files:
  created:
    - relay/identity/relay_identity.h
    - relay/identity/relay_identity.cpp
    - db/tests/relay/test_relay_identity.cpp
  modified:
    - relay/relay_main.cpp
    - relay/CMakeLists.txt
    - db/CMakeLists.txt

key-decisions:
  - "RelayIdentity uses direct key_path (not data_dir) matching SSH-style identity management"
  - "pub_path derived by replace_extension(.pub) - handles all path forms cleanly"

patterns-established:
  - "Relay identity: direct key path + .pub sibling (not directory-based like node identity)"
  - "Relay CLI: same dispatch pattern as node binary (run/keygen/version/help)"

requirements-completed: [RELAY-05]

# Metrics
duration: 5min
completed: 2026-03-23
---

# Phase 58 Plan 02: Relay Identity & Entry Point Summary

**ML-DSA-87 relay identity with SSH-style .key/.pub paths and full CLI entry point (run/keygen/version/help, 11 identity tests)**

## Performance

- **Duration:** 5 min
- **Started:** 2026-03-23T15:02:02Z
- **Completed:** 2026-03-23T15:07:00Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- RelayIdentity class with generate, save, load, load_or_generate using SSH-style .key/.pub path convention
- chromatindb_relay CLI with run (config + identity + logging), keygen (--output + --force), version, help subcommands
- 11 unit tests covering identity generation, save/load round-trip, error cases, pub_path derivation
- First run auto-generates ML-DSA-87 keypair; subsequent runs load existing and log public key hash

## Task Commits

Each task was committed atomically (TDD RED/GREEN + feat):

1. **Task 1 RED: Failing tests for RelayIdentity** - `73b017a` (test)
2. **Task 1 GREEN: Implement RelayIdentity** - `2b37e59` (feat)
3. **Task 2: Relay CLI entry point** - `e36b67d` (feat)

## Files Created/Modified
- `relay/identity/relay_identity.h` - RelayIdentity class declaration with generate/load/save/load_or_generate
- `relay/identity/relay_identity.cpp` - ML-DSA-87 keypair management, SSH-style .key/.pub path convention
- `relay/relay_main.cpp` - Full CLI with run/keygen/version/help subcommands
- `relay/CMakeLists.txt` - Added identity/relay_identity.cpp to library sources
- `db/tests/relay/test_relay_identity.cpp` - 11 test cases, 25 assertions
- `db/CMakeLists.txt` - Added test_relay_identity.cpp to test target

## Decisions Made
- RelayIdentity uses direct key_path (not data_dir) matching SSH-style identity management -- different from NodeIdentity which uses data_dir/node.key
- pub_path derived by std::filesystem::replace_extension(".pub") -- handles all path forms cleanly including deeply nested paths

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Relay binary fully operational: loads config, generates/loads identity, logs status
- Ready for Phase 59 to add PQ-authenticated protocol logic (event loop, handshake, message routing)
- All 26 relay tests pass (15 config + 11 identity)

## Self-Check: PASSED

- All 4 created/modified source files verified present
- All 3 commit hashes (73b017a, 2b37e59, e36b67d) verified in git log
- No stubs or placeholders found in relay/ directory

---
*Phase: 58-relay-scaffolding*
*Completed: 2026-03-23*
