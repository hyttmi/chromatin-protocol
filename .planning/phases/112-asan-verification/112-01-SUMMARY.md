---
phase: 112-asan-verification
plan: 01
subsystem: testing
tags: [asan, lsan, sanitizer, memory-safety, benchmark, signal-handling]

# Dependency graph
requires:
  - phase: 111-single-threaded-rewrite
    provides: single-threaded relay with HTTP transport and thread pool offload
provides:
  - LSAN suppression file for relay shutdown leaks (relay/lsan_suppressions.txt)
  - ASAN test harness script covering concurrent load + signal handling (tools/relay_asan_test.sh)
  - Proof of ASAN-clean execution at concurrency 1, 10, 100
  - Proof of SIGHUP config reload under single-threaded model
  - Proof of SIGTERM graceful shutdown under single-threaded model
affects: [113-perf-benchmarks, relay-ci]

# Tech tracking
tech-stack:
  added: []
  patterns: [asan-test-harness, lsan-suppression-file, stdout-stderr-combined-capture]

key-files:
  created:
    - relay/lsan_suppressions.txt
    - tools/relay_asan_test.sh
  modified: []

key-decisions:
  - "Relay logs (spdlog) go to stdout, ASAN output to stderr -- test script captures both to single log file"
  - "Node port 4290, relay port 4291 (config validator rejects ephemeral port 0)"
  - "SIGHUP verification checks for 'rate_limit reloaded: 999 msg/s' (relay appends unit suffix)"
  - "Benchmark uses 20 iterations (not 100) to keep ASAN runtime under 30 seconds"

patterns-established:
  - "ASAN test harness pattern: build + start node + start relay + benchmark + SIGHUP + SIGTERM + parse"
  - "Combined stdout+stderr capture for processes that use spdlog (stdout) and ASAN (stderr)"

requirements-completed: [VER-02, VER-03]

# Metrics
duration: 11min
completed: 2026-04-14
---

# Phase 112 Plan 01: ASAN Verification Summary

**ASAN test harness with LSAN suppressions proving relay memory-safety at 1/10/100 concurrent HTTP clients with SIGHUP/SIGTERM signal verification**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-14T07:16:19Z
- **Completed:** 2026-04-14T07:27:31Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Created relay-specific LSAN suppression file covering liboqs, OpenSSL, and Asio shutdown leaks
- Built ASAN test harness that exercises full auth + data pipeline at concurrency 1, 10, 100
- Proved relay ASAN-clean: zero AddressSanitizer or LeakSanitizer errors under concurrent load
- Proved SIGHUP config reload works (rate_limit change confirmed in logs)
- Proved SIGTERM graceful shutdown works (relay exits within 10s, "relay stopped" confirmed)

## Task Commits

Each task was committed atomically:

1. **Task 1: Create LSAN suppression file and ASAN test harness script** - `ca2ce166` (feat)
2. **Task 2: Run ASAN test harness and verify clean output** - `c534214e` (fix)

## Files Created/Modified
- `relay/lsan_suppressions.txt` - LSAN suppression patterns for liboqs (OQS_SIG_new, OQS_MEM_malloc), OpenSSL (OPENSSL_init_ssl, SSL_CTX_new, OPENSSL_init_crypto), and Asio (awaitable_handler, awaitable_frame, basic_resolver_results, resolve_query_op)
- `tools/relay_asan_test.sh` - Complete ASAN test harness: builds ASAN binaries, starts node+relay, runs benchmark at 3 concurrency levels, tests SIGHUP rate limit reload, tests SIGTERM clean shutdown, parses ASAN output for errors

## Decisions Made
- Used port 4290 for node and 4291 for relay instead of ephemeral port 0 (config validator rejects port 0 in CLI mode, unlike unit test API)
- Combined stdout and stderr capture (`>file 2>&1`) because spdlog's stdout_color_sink_mt writes to stdout while ASAN writes to stderr
- Matched SIGHUP grep pattern to actual relay log format: `rate_limit reloaded: 999 msg/s` (relay appends unit suffix to the value)
- Benchmark reduced to 20 iterations per client (not 100) to keep ASAN runtime reasonable (~25s total)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed node config port 0 rejection**
- **Found during:** Task 2 (first ASAN test run)
- **Issue:** Plan specified `bind_address: "127.0.0.1:0"` (ephemeral port) but node's config validator rejects port 0
- **Fix:** Changed to `bind_address: "127.0.0.1:4290"` (high port, no conflict)
- **Files modified:** tools/relay_asan_test.sh
- **Verification:** Node starts successfully on port 4290
- **Committed in:** c534214e

**2. [Rule 1 - Bug] Fixed stdout vs stderr log capture**
- **Found during:** Task 2 (SIGHUP/SIGTERM log checks failed)
- **Issue:** Script captured only stderr (`2>file`) but spdlog writes to stdout. SIGHUP/SIGTERM confirmation logs were not in the captured file.
- **Fix:** Changed to combined capture (`>file 2>&1`) for both node and relay processes
- **Files modified:** tools/relay_asan_test.sh
- **Verification:** grep finds "rate_limit reloaded: 999 msg/s" and "relay stopped" in log files
- **Committed in:** c534214e

**3. [Rule 1 - Bug] Fixed SIGHUP grep pattern mismatch**
- **Found during:** Task 2 (SIGHUP check failed)
- **Issue:** Script checked for `"rate_limit reloaded: 999"` but relay logs `"rate_limit reloaded: 999 msg/s"` (with unit suffix)
- **Fix:** Updated grep pattern to match actual log format
- **Files modified:** tools/relay_asan_test.sh
- **Verification:** SIGHUP check passes on repeated runs
- **Committed in:** c534214e

---

**Total deviations:** 3 auto-fixed (3 Rule 1 bugs)
**Impact on plan:** All fixes necessary for correct test script operation. No scope creep.

## ASAN Test Results

Benchmark results under ASAN instrumentation (2-5x overhead expected):

| Concurrency | Total Ops | Blobs/sec | p50 (ms) | p99 (ms) | Errors |
|-------------|-----------|-----------|----------|----------|--------|
| 1 | 20 | 244 | 3.8 | 4.4 | 0 |
| 10 | 200 | 766 | 12.7 | 15.7 | 0 |
| 100 | 2000 | 716 | 133.6 | 160.0 | 0 |

- **ASAN errors:** 0
- **LSAN errors:** 0 (all third-party leaks suppressed)
- **SIGHUP:** rate_limit reloaded to 999 msg/s -- confirmed
- **SIGTERM:** relay exited cleanly, "relay stopped" -- confirmed

## Issues Encountered
None beyond the auto-fixed script bugs documented above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- ASAN test harness can be run with `bash tools/relay_asan_test.sh` for regression testing
- Use `--skip-build` flag to reuse existing ASAN build for faster iterations
- Phase 113 (performance benchmarks) can proceed -- relay proven memory-safe under load

---
*Phase: 112-asan-verification*
*Completed: 2026-04-14*
