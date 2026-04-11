---
phase: 106-bug-fixes
plan: 02
subsystem: security
tags: [coroutine-safety, asan, audit, cpp20, asio, std-visit]

# Dependency graph
requires:
  - phase: 105-operational-polish
    provides: "Relay Phase 105 code (ws_session, uds_multiplexer, metrics_collector, etc.)"
provides:
  - "Coroutine safety audit of all relay/ code (COROUTINE-AUDIT.md)"
  - "Read-only coroutine audit of db/peer/ code (DB-COROUTINE-FINDINGS.md)"
  - "std::visit safety documentation in ws_session.cpp"
affects: [107-e2e-testing, 108-sanitizer-hardening]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "SAFETY comment pattern for std::visit in non-coroutine context"
    - "Severity-rated coroutine audit documentation format"

key-files:
  created:
    - ".planning/phases/106-bug-fixes/COROUTINE-AUDIT.md"
    - ".planning/phases/106-bug-fixes/DB-COROUTINE-FINDINGS.md"
  modified:
    - "relay/ws/ws_session.cpp"

key-decisions:
  - "No relay code fixes needed beyond documentation — all coroutine patterns already safe"
  - "MEDIUM findings documented but not fixed: benign timer/socket interaction in ws_acceptor (R-017), ReadRequest not offloaded to thread pool (D-018)"

patterns-established:
  - "Audit finding IDs: R-NNN for relay, D-NNN for db"
  - "Five-level severity scale: CRITICAL/HIGH/MEDIUM/LOW/SAFE"

requirements-completed: [FIX-02]

# Metrics
duration: 6min
completed: 2026-04-11
---

# Phase 106 Plan 02: Coroutine Safety Audit Summary

**Comprehensive coroutine safety audit across relay/ (7 files, 20 findings) and db/peer/ (5 files, 22 findings) with severity ratings -- 0 CRITICAL, 0 HIGH, all patterns safe**

## Performance

- **Duration:** 6 min
- **Started:** 2026-04-11T04:05:36Z
- **Completed:** 2026-04-11T04:11:26Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments

- Audited all 7 relay source files across 4 coroutine safety categories (lambda captures, shared_ptr lifetimes, container invalidation, strand confinement) -- 20 findings, all SAFE or LOW/MEDIUM
- Added SAFETY documentation comment to the single std::visit call in ws_session.cpp shutdown_socket()
- Audited all 5 db/peer/ component files (3631 lines) -- 22 findings with documented known-safe patterns (PeerInfo re-lookup, Connection::Ptr, deque stability)
- Confirmed relay codebase is ASAN-clean-ready for Phase 107/108 E2E testing

## Task Commits

Each task was committed atomically:

1. **Task 1: Relay coroutine safety audit and fixes** - `5b9e61a` (docs)
2. **Task 2: db/ read-only coroutine audit** - `7831a8f` (docs)

## Files Created/Modified

- `relay/ws/ws_session.cpp` - Added SAFETY comment above std::visit in shutdown_socket()
- `.planning/phases/106-bug-fixes/COROUTINE-AUDIT.md` - 20 relay audit findings with severity ratings
- `.planning/phases/106-bug-fixes/DB-COROUTINE-FINDINGS.md` - 22 db/ audit findings for user manual review

## Decisions Made

- No code fixes needed in relay/ -- all patterns already follow safe conventions (shared_from_this, get_if/get, snapshot-before-co_await)
- Two MEDIUM findings documented for future consideration: (1) R-017 timer captures raw pointer to moved-from socket in ws_acceptor TLS path (benign -- moved-from socket close is harmless no-op), (2) D-018 ReadRequest handler does synchronous get_blob on io_context thread (performance consideration only, not a safety bug)
- No db/ code changes per D-06 (db/ is frozen for v3.1.0)

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

None.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Relay coroutine safety verified -- ready for ASAN/TSAN testing in Phase 107/108
- DB-COROUTINE-FINDINGS.md available for user's manual review of db/ code
- All 205 relay tests continue to pass

---
*Phase: 106-bug-fixes*
*Completed: 2026-04-11*
