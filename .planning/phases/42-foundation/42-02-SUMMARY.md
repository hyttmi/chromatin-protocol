---
phase: 42-foundation
plan: 02
subsystem: config
tags: [validation, error-handling, startup, spdlog]

# Dependency graph
requires:
  - phase: 42-foundation
    provides: "Config struct with all numeric fields"
provides:
  - "validate_config() function for startup config validation"
  - "Error accumulation pattern for multi-field validation"
  - "Type mismatch wrapping in load_config"
  - "Unknown key detection with spdlog::warn"
affects: [43-resilience, 44-observability]

# Tech tracking
tech-stack:
  added: []
  patterns: [error-accumulation-validation, type-error-wrapping, unknown-key-warning]

key-files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/main.cpp
    - db/tests/config/test_config.cpp

key-decisions:
  - "Error accumulation: collect all validation errors before throwing, so operator sees everything in one restart"
  - "Unknown keys warned (spdlog::warn), not rejected -- forward compatibility for pre-1.0"
  - "validate_config called before logging::init, uses std::cerr for error output"
  - "parse_args also wrapped in try-catch for clean type mismatch handling"

patterns-established:
  - "Error accumulation: validate_config collects errors in vector, throws single exception with all"
  - "Type error wrapping: nlohmann::json::type_error caught and rethrown as readable runtime_error"

requirements-completed: [OPS-02]

# Metrics
duration: 21min
completed: 2026-03-20
---

# Phase 42 Plan 02: Config Validation Summary

**Startup config validation with error accumulation, type mismatch wrapping, and unknown key warnings via validate_config()**

## Performance

- **Duration:** 21 min
- **Started:** 2026-03-20T03:04:53Z
- **Completed:** 2026-03-20T03:26:10Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- validate_config() validates all 11 numeric fields, log_level enum, and bind_address format
- Error accumulation: multiple invalid fields produce single exception listing ALL errors
- Type mismatches in JSON config caught with human-readable error messages
- Unknown config keys warned via spdlog but not rejected (forward compatibility)
- Node exits cleanly with code 1 and stderr message on invalid config
- 21 new tests (60 config tests total), full suite passing

## Task Commits

Each task was committed atomically:

1. **Task 1 RED: Failing tests for validate_config** - `4a54a90` (test)
2. **Task 1 GREEN: Implement validate_config** - `4627149` (feat)
3. **Task 2: Wire validate_config into startup** - `8cab902` (feat)

## Files Created/Modified
- `db/config/config.h` - Added validate_config() declaration
- `db/config/config.cpp` - Implemented validate_config(), type error wrapping, unknown key detection
- `db/main.cpp` - Wired validate_config() into cmd_run() before logging init
- `db/tests/config/test_config.cpp` - 21 new test cases for validation

## Decisions Made
- Error accumulation pattern: all validation errors collected in vector before throwing single exception
- Unknown keys produce spdlog::warn, not errors (forward compatibility for pre-1.0)
- validate_config called after parse_args but before logging::init (uses std::cerr)
- parse_args call also wrapped in try-catch for clean type mismatch handling

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Wrapped parse_args in try-catch for type mismatch handling**
- **Found during:** Task 2 (startup integration)
- **Issue:** Type mismatch in config file (e.g., "max_peers": "thirty") caused unhandled runtime_error from load_config, resulting in abort instead of clean exit
- **Fix:** Wrapped both parse_args and validate_config in single try-catch block in cmd_run()
- **Files modified:** db/main.cpp
- **Verification:** Smoke test: type mismatch config now prints readable error and exits with code 1
- **Committed in:** 8cab902 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix)
**Impact on plan:** Essential for correctness -- type mismatch errors must be caught cleanly at startup. No scope creep.

## Issues Encountered
- Build artifact corruption on first cmake build (liboqs ranlib error) -- resolved on retry, pre-existing intermittent issue
- Flaky "reload_config revokes connected peer" test -- passes on retry, timing-sensitive network test unrelated to changes

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Config validation foundation complete, ready for new config fields in later phases
- validate_config() can be extended with new field checks as config grows

## Self-Check: PASSED

All 5 files verified present. All 3 commits verified in git log.

---
*Phase: 42-foundation*
*Completed: 2026-03-20*
