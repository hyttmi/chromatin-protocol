---
phase: 01-foundation
plan: 04
subsystem: config
tags: [ttl, protocol-constants, config]

# Dependency graph
requires:
  - phase: 01-foundation-03
    provides: "Config system with JSON loading and CLI overrides"
provides:
  - "BLOB_TTL_SECONDS constexpr protocol constant (604800s / 7 days)"
  - "Config struct without user-configurable TTL"
affects: [wire, blob-engine, storage]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Protocol constants as constexpr above Config struct"]

key-files:
  created: []
  modified:
    - src/config/config.h
    - src/config/config.cpp
    - tests/config/test_config.cpp

key-decisions:
  - "TTL is a protocol invariant, not a user preference -- enforced at compile time"

patterns-established:
  - "Protocol constants: use constexpr in namespace scope above Config struct"

requirements-completed: [DAEM-01]

# Metrics
duration: 2min
completed: 2026-03-03
---

# Phase 1 Plan 4: TTL Protocol Constant Summary

**TTL enforced as compile-time protocol constant (BLOB_TTL_SECONDS = 604800), removed from user-configurable Config struct**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-03T12:22:27Z
- **Completed:** 2026-03-03T12:24:33Z
- **Tasks:** 1
- **Files modified:** 3

## Accomplishments
- Converted TTL from user-configurable Config field to constexpr protocol constant
- Removed all default_ttl references from config loading and parsing
- Added dedicated test verifying BLOB_TTL_SECONDS is a protocol invariant
- All 65 tests pass (64 original + 1 new protocol constant test)

## Task Commits

Each task was committed atomically:

1. **Task 1: Make TTL a protocol constant and remove from Config** - `1d62df6` (feat)

## Files Created/Modified
- `src/config/config.h` - Added constexpr BLOB_TTL_SECONDS, removed default_ttl from Config struct
- `src/config/config.cpp` - Removed default_ttl JSON parsing line
- `tests/config/test_config.cpp` - Removed TTL-override assertions, added protocol constant test

## Decisions Made
- TTL is a protocol invariant enforced at compile time, not a runtime preference. Any code attempting `cfg.default_ttl` will fail to compile, making accidental override impossible.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 1 foundation fully complete (all 4 plans executed)
- Protocol constants pattern established for future invariants
- Ready for Phase 2 (storage layer)

## Self-Check: PASSED

- FOUND: src/config/config.h
- FOUND: src/config/config.cpp
- FOUND: tests/config/test_config.cpp
- FOUND: .planning/phases/01-foundation/01-04-SUMMARY.md
- FOUND: commit 1d62df6

---
*Phase: 01-foundation*
*Completed: 2026-03-03*
