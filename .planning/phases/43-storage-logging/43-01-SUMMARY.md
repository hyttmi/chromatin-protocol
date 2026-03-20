---
phase: 43-storage-logging
plan: 01
subsystem: logging
tags: [spdlog, rotating-file-sink, json-logging, config-validation]

# Dependency graph
requires:
  - phase: 42-foundation
    provides: validate_config() with error accumulation, known_keys warning pattern
provides:
  - Multi-sink logging with rotating file support
  - JSON structured log output option
  - Four new config fields (log_file, log_max_size_mb, log_max_files, log_format)
  - Fixed get_logger() using shared sinks vector
affects: [43-02, 44-metrics, 45-docs]

# Tech tracking
tech-stack:
  added: [rotating_file_sink_mt]
  patterns: [shared-sinks-vector, multi-sink-logger-factory]

key-files:
  created: []
  modified:
    - db/logging/logging.h
    - db/logging/logging.cpp
    - db/config/config.h
    - db/config/config.cpp
    - db/main.cpp
    - db/tests/config/test_config.cpp

key-decisions:
  - "Shared sinks vector instead of per-logger sink creation -- all loggers write to same console+file"
  - "Same formatter on all sinks (no mixed text/json between console and file)"
  - "Graceful fallback on file open failure: warn to stderr, continue console-only"
  - "Removed std::call_once -- init() called once at startup, call_once prevented re-init with new params"

patterns-established:
  - "Shared sinks pattern: all loggers created via get_logger() inherit sinks from init()"
  - "JSON log format: machine-parseable structured output for production monitoring"

requirements-completed: [OPS-04, OPS-05]

# Metrics
duration: 12min
completed: 2026-03-20
---

# Phase 43 Plan 01: File Logging & JSON Format Summary

**Multi-sink rotating file logging with JSON structured output using spdlog rotating_file_sink_mt**

## Performance

- **Duration:** 12 min
- **Started:** 2026-03-20T04:26:33Z
- **Completed:** 2026-03-20T04:38:35Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- Extended logging::init() with file path, rotation size/count, and format parameters
- Fixed get_logger() to use shared sinks vector (previously created console-only loggers)
- Added four config fields with validation (log_file, log_max_size_mb, log_max_files, log_format)
- Verified JSON output end-to-end: node emits valid JSON to both console and rotating file simultaneously

## Task Commits

Each task was committed atomically:

1. **Task 1: Config fields + validation for logging** - `6a171ca` (feat, TDD)
2. **Task 2: Multi-sink logging with file rotation and JSON format** - `4556f88` (feat)

## Files Created/Modified
- `db/config/config.h` - Four new Config struct fields with defaults
- `db/config/config.cpp` - Parsing (j.value), known_keys, validate_config rules
- `db/logging/logging.h` - Extended init() signature with file/format params
- `db/logging/logging.cpp` - Multi-sink setup, shared sinks vector, JSON pattern, fixed get_logger
- `db/main.cpp` - Pass new config fields to logging::init()
- `db/tests/config/test_config.cpp` - 12 new test cases for logging config

## Decisions Made
- Shared sinks vector at module level: all loggers created via get_logger() automatically inherit console+file sinks from init()
- Same formatter applied to all sinks (text or JSON) -- no mixed formats between console and file, avoids confusion in log aggregation
- Graceful fallback on file open failure: catches spdlog_ex, warns to stderr via fmt::print, continues with console-only logging
- Removed std::call_once guard: init() is only called once at startup, and call_once prevented re-initialization with new parameters

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Full test suite hangs on networking tests (pre-existing, unrelated to this plan). Verified correctness by running targeted test suites: [config] (95 tests), [storage] (81 tests), [logging] (12 tests), [engine], [identity] -- 242 test cases total, all passing.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Logging infrastructure complete, ready for metrics completion (plan 43-02)
- All named loggers throughout codebase will automatically write to file when log_file is configured
- No blockers or concerns

---
*Phase: 43-storage-logging*
*Completed: 2026-03-20*
