---
phase: 47-crypto-transport-verification
plan: 01
subsystem: testing
tags: [integration-tests, docker, crypto-verification, ml-dsa-87, sha3-256, flatbuffers]

# Dependency graph
requires:
  - phase: 46-sanitizers-bug-fix
    provides: "Sanitizer-clean codebase with 408+ passing tests"
provides:
  - "chromatindb_verify CLI tool (hash + sig subcommands for independent crypto verification)"
  - "Integration test harness (helpers.sh, run-integration.sh, docker-compose.test.yml)"
  - "Docker image with all three binaries (chromatindb, chromatindb_loadgen, chromatindb_verify)"
affects: [47-crypto-transport-verification, 48-sync-replication-verification, 49-acl-delegation-verification, 50-operational-verification, 51-performance-limits-verification, 52-release-hardening]

# Tech tracking
tech-stack:
  added: []
  patterns: [docker-compose-test-topology, integration-test-harness, standalone-crypto-verify-tool]

key-files:
  created:
    - tools/verify_main.cpp
    - tests/integration/helpers.sh
    - tests/integration/run-integration.sh
    - tests/integration/docker-compose.test.yml
    - tests/integration/configs/node1.json
    - tests/integration/configs/node2.json
  modified:
    - CMakeLists.txt
    - Dockerfile

key-decisions:
  - "chromatindb_verify links against chromatindb_lib (same crypto code paths as the node)"
  - "JSON output from verify tool for machine-parseable test assertions"
  - "Separate test-net Docker network and chromatindb-test project name to avoid deploy/ conflicts"
  - "sync_interval_seconds=5 in test configs for fast convergence"

patterns-established:
  - "Integration test pattern: source helpers.sh, use run_loadgen/run_verify/wait_sync primitives"
  - "Test runner pattern: run-integration.sh discovers test_*.sh, reports pass/fail summary"
  - "Verify tool pattern: pipe FlatBuffer blob bytes to chromatindb_verify hash/sig for independent crypto checks"

requirements-completed: [CRYPT-01, CRYPT-02]

# Metrics
duration: 5min
completed: 2026-03-21
---

# Phase 47 Plan 01: Test Infrastructure Summary

**chromatindb_verify CLI tool with hash/sig subcommands and Docker integration test harness with 2-node topology, shared helpers, and test runner**

## Performance

- **Duration:** 5 min
- **Started:** 2026-03-21T06:20:06Z
- **Completed:** 2026-03-21T06:25:22Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- Standalone chromatindb_verify binary with `hash` (recompute SHA3-256 signing digest + blob hash) and `sig` (verify ML-DSA-87 signature) subcommands
- Integration test harness with Docker orchestration primitives reusable across Phases 47-52
- 2-node Docker Compose test topology with health checks, bridge network, and fast sync interval
- Docker image updated to include chromatindb_verify alongside chromatindb and chromatindb_loadgen

## Task Commits

Each task was committed atomically:

1. **Task 1: Create chromatindb_verify CLI tool and update Docker build** - `bca1ac5` (feat)
2. **Task 2: Create integration test harness** - `61d25c5` (feat)

## Files Created/Modified
- `tools/verify_main.cpp` - Standalone crypto verification CLI (hash + sig subcommands)
- `tests/integration/helpers.sh` - Shared test functions (wait_healthy, get_blob_count, wait_sync, run_loadgen, run_verify, cleanup)
- `tests/integration/run-integration.sh` - Test runner with discovery, filtering, and pass/fail summary
- `tests/integration/docker-compose.test.yml` - 2-node test topology (chromatindb:test image)
- `tests/integration/configs/node1.json` - Node 1 config (no bootstrap, debug logging)
- `tests/integration/configs/node2.json` - Node 2 config (bootstraps to node1, debug logging)
- `CMakeLists.txt` - Added chromatindb_verify target
- `Dockerfile` - Added chromatindb_verify to build targets, strip, and COPY

## Decisions Made
- chromatindb_verify links against chromatindb_lib directly -- uses exact same crypto code paths as the node, no separate library dependency
- JSON output format for verify tool enables machine-parseable assertions in test scripts (jq/grep)
- Test topology uses `chromatindb-test` project name and `test-net` network to isolate from deploy/ benchmarks
- sync_interval_seconds=5 (half of deploy's 10s) for faster test convergence

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Fixed spdlog stderr sink include path**
- **Found during:** Task 1 (chromatindb_verify build)
- **Issue:** `spdlog/sinks/stderr_color_sinks.h` does not exist in spdlog 1.15.1; stderr_color_mt is in `stdout_color_sinks.h`
- **Fix:** Changed include to `spdlog/sinks/stdout_color_sinks.h`
- **Files modified:** tools/verify_main.cpp
- **Verification:** Build succeeds, binary runs correctly
- **Committed in:** bca1ac5 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Trivial include path correction. No scope change.

## Issues Encountered
None beyond the auto-fixed include path.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- chromatindb_verify binary ready for CRYPT-01 through CRYPT-06 test scripts
- Test harness ready for test_*.sh scripts in subsequent plans
- Docker image builds with all three binaries
- Phases 48-52 can source helpers.sh and use the established patterns

---
*Phase: 47-crypto-transport-verification*
*Completed: 2026-03-21*
