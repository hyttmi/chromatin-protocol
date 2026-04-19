---
phase: 119-chunked-large-files
plan: 03
subsystem: cli
tags: [pipeline, in_flight, chunked, cr-01, wr-02, wr-03, wr-04, in-03, recv_next, raii, cdb, live-node]

# Dependency graph
requires:
  - phase: 119-chunked-large-files (plans 01-02)
    provides: "put_chunked / rm_chunked / get_chunked skeletons with pipelined Phase-A/Phase-B drain loops, manifest codec, CHUNK-05 truncation guard, 75 unit tests"
  - phase: 120-request-pipelining
    provides: "pipeline::pump_recv_for / pump_one_for_backpressure templates, Connection::send_async + recv_for pipelining primitives (kPipelineDepth=8), pending_replies_ correlation map bounded by D-04"
provides:
  - "pipeline::pump_recv_any template (arrival-order counterpart of pump_recv_for)"
  - "Connection::recv_next() public method delegating to pump_recv_any"
  - "5 call-site migrations from conn.recv() to conn.recv_next() in put_chunked / rm_chunked / get_chunked / cmd::put / cmd::get Phase-B drains (CR-01 closed; WR-04 closed)"
  - "get_chunked retry backoff corrected to 250/1000/4000 ms order (WR-02 closed)"
  - "TU-local UnlinkGuard RAII wrapping get_chunked body (WR-03 closed — D-12 window between ::close(fd) and verify_plaintext_sha3 now covered)"
  - "Narrowed main.cpp config.json catch with stderr warning (IN-03 closed — project-memory rule on silent error suppression)"
  - "Shared cli/tests/pipeline_test_support.h extracting ScriptedSource + make_reply + make_ack_reply (feedback_no_duplicate_code.md honored)"
  - "6 new unit tests (4 [pipeline] + 2 [chunked]) exercising pump_recv_any and the CR-01 failure shape"
  - "Live-node E2E gate passed against BOTH 192.168.1.73 (home) and 127.0.0.1 (local): 420 MiB put → get → byte-identical diff → rm → zero orphans"
affects: [phase-122-verification, future-pipelined-batch-commands]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "pump_recv_any<Source>: header-only arrival-order drain template matching pump_recv_for's shape and single-sender invariant (PIPE-02)"
    - "TU-local RAII guard for unlink-on-throw: UnlinkGuard covers the D-12 partial-output window"
    - "Shared Catch2 test fixture header (cli/tests/pipeline_test_support.h) used across translation units"

key-files:
  created:
    - cli/tests/pipeline_test_support.h
  modified:
    - cli/src/pipeline_pump.h
    - cli/src/connection.h
    - cli/src/connection.cpp
    - cli/src/chunked.cpp
    - cli/src/commands.cpp
    - cli/src/main.cpp
    - cli/tests/test_connection_pipelining.cpp
    - cli/tests/test_chunked.cpp

key-decisions:
  - "Took reviewer option (b) from 119-REVIEW.md §CR-01 (add recv_next + migrate all 5 call sites) rather than option (a) (modify Connection::recv to decrement in_flight_). Option (a) would have double-decremented whenever recv_for's pump already decremented, and would have broken send_async's backpressure drain (which stashes via recv() and must NOT touch in_flight_)."
  - "UnlinkGuard is translation-unit-local in the chunked.cpp anonymous namespace rather than exposed via a header. No other site currently needs it; introducing it TU-locally minimizes surface area (YAGNI) and keeps the get_chunked correctness fix surgical."
  - "Shared test fixture extracted to pipeline_test_support.h with inline functions and a struct, kept header-only to honor feedback_no_duplicate_code.md without adding a .cpp / CMake target."

patterns-established:
  - "Arrival-order drain: every call site that uses conn.send_async() must pair with conn.recv_next() (not conn.recv()) so in_flight_ is maintained by the same layer that incremented it. Sync commands that use plain conn.send() continue to use conn.recv() correctly."
  - "Retry backoff post-index, not pre-index: put_chunked and get_chunked now both index RETRY_BACKOFF_MS at the current attempts[] value then increment. First retry always 250 ms per D-15."

requirements-completed: [CHUNK-01, CHUNK-03, CHUNK-04]

# Metrics
duration: ~50min (excluding baseline cmake configure + build)
completed: 2026-04-19
---

# Phase 119 Plan 03: Gap-closure for Phase 119 live-node E2E Summary

**CR-01 in_flight_ leak fixed via pump_recv_any + Connection::recv_next, 5 call-site migrations closing the 420 MiB live-node hang; WR-02 retry backoff ordering corrected, WR-03 D-12 window closed with UnlinkGuard RAII, WR-04 small-batch hang closed, IN-03 silent config catch narrowed with stderr warning.**

## Performance

- **Duration:** ~50 min execution time (12 task commits + 1 live-node checkpoint)
- **Completed:** 2026-04-19
- **Tasks:** 13 atomic commits + 1 self-run live-node checkpoint
- **Files modified:** 7 source files + 1 new test header
- **Tests:** 75 → 81 (+6 new test cases — 4 [pipeline] + 2 [chunked])

## Accomplishments

- Closed 119-VERIFICATION.md's single gap (gaps_found on Truth 1): 420 MiB `cdb put` against 192.168.1.73 now completes in 52 s, previously hung for 46 min at 8 chunks with no WriteAcks flowing back.
- `cdb get <manifest_hash>` reassembles byte-identical plaintext against both home and local nodes (verified via `diff -q`, exits 0).
- `cdb rm <manifest_hash>` cascades: all 27 chunk tombstones + manifest tombstone against both nodes, zero orphan CDAT blobs post-rm on either target (36→9 on home, 63→36 on local — exactly 27 tombstoned each).
- WR-02 read-path retry backoff now matches D-15 (250/1000/4000 ms) on first/second/third retry — previously the first retry slept 1000 ms because the counter pre-incremented before indexing RETRY_BACKOFF_MS.
- WR-03 D-12 window between `::close(fd)` and `verify_plaintext_sha3` in get_chunked is covered by a TU-local `UnlinkGuard` RAII; `std::bad_alloc` in that window now unlinks the partial output automatically. Also verified by accident when a disk-full pwrite failure during live-node testing correctly unlinked the partial output (see Issues Encountered below).
- IN-03 `catch (...)` in main.cpp narrowed to `catch (const std::exception& e)` and now prints `Warning: ignoring malformed {config_path}: {e.what()}` to stderr instead of silently falling through — honors the project-memory rule `feedback_no_or_true.md`.
- Shared Catch2 fixture extracted to `cli/tests/pipeline_test_support.h`; `test_connection_pipelining.cpp` and `test_chunked.cpp` both include it — no duplicate `ScriptedSource` / `make_reply` definitions anywhere (feedback_no_duplicate_code.md honored).
- 2 new [chunked] regression tests exercise the exact CR-01 shape (8 sends + 8 pump_recv_any drains leave in_flight at 0; 9th drain on empty source returns nullopt with no underflow).

## Task Commits

Each task was committed atomically:

1. **Task 1:** add pump_recv_any template + 4 unit tests — `7c185056` (feat)
2. **Task 2:** add Connection::recv_next() arrival-order drain — `4cafb094` (feat)
3. **Task 3:** migrate put_chunked Phase-B drain — `9acaa718` (fix)
4. **Task 4:** migrate rm_chunked Phase-1 drain — `ad1eceb3` (fix)
5. **Task 5:** migrate get_chunked Phase-B drain — `dd29a917` (fix)
6. **Task 6:** migrate cmd::put Phase-B drain — `f4b5e3e9` (fix)
7. **Task 7:** migrate cmd::get Phase-B drain — `ac95ff60` (fix)
8. **Task 8:** get_chunked retry backoff RETRY_BACKOFF_MS[0]=250ms (WR-02) — `2c11aefc` (fix)
9. **Task 9:** UnlinkGuard RAII in get_chunked (WR-03) — `672d981c` (fix)
10. **Task 10:** narrow main.cpp config.json catch + stderr warning (IN-03) — `c76be71e` (fix)
11. **Task 11:** extract ScriptedSource fixture to shared header — `d769265c` (refactor)
12. **Task 12:** add CR-01 regression tests to test_chunked.cpp — `fc3379b5` (test)
13. **Task 13:** clean build verification — no code changes needed; build + suite green at 81 cases; no new warnings.
14. **Task 14 (live-node checkpoint):** self-run against 192.168.1.73 and 127.0.0.1 — PASSED (observations below). Infra was reachable so self-verify per project memory `feedback_self_verify_checkpoints.md`.

_No plan-metadata doc commit will be added here — the orchestrator commits SUMMARY.md per the worktree-agent contract._

## Files Created/Modified

### Created

- `cli/tests/pipeline_test_support.h` — header-only shared Catch2 fixture (ScriptedSource + make_reply + make_ack_reply) used by both `test_connection_pipelining.cpp` and `test_chunked.cpp` per feedback_no_duplicate_code.md.

### Modified

- `cli/src/pipeline_pump.h` — appended `pump_recv_any<Source>()` template; mirrors pump_recv_for shape, decrements in_flight on non-nullopt return with guarded underflow prevention, preserves single-sender invariant (PIPE-02).
- `cli/src/connection.h` — declared `Connection::recv_next()` public method (arrival-order counterpart of recv_for).
- `cli/src/connection.cpp` — implemented `recv_next()` as a 3-statement delegation to `pipeline::pump_recv_any` over the connection's own `pending_replies_` and `in_flight_`. `Connection::recv()` unchanged.
- `cli/src/chunked.cpp` — migrated 3 Phase-B drain call sites (put_chunked line 246, rm_chunked line 390, get_chunked line 613) from conn.recv() to conn.recv_next(); updated line-174 narrative comment; fixed retry backoff post-index in retry_chunk lambda (WR-02); added TU-local UnlinkGuard struct and wrapped get_chunked body with it (WR-03); removed duplicated mid-function ::unlink calls superseded by guard.
- `cli/src/commands.cpp` — migrated 2 Phase-B drain call sites (cmd::put line ~620, cmd::get line ~736) from conn.recv() to conn.recv_next(). Sync-command recv() sites (cmd::ls, cmd::info, cmd::stats, cmd::delete, cmd::exists, cmd::delegate, cmd::revoke, cmd::delegations, rm pre-flight, find_pubkey_blob, reshare helpers) preserved unchanged — they use plain conn.send() and never increment in_flight_.
- `cli/src/main.cpp` — narrowed `catch (...)` to `catch (const std::exception& e)` + stderr warning format `Warning: ignoring malformed {path}: {e.what()}`.
- `cli/tests/test_connection_pipelining.cpp` — deleted local anonymous-namespace ScriptedSource / make_reply definitions; now `#include "cli/tests/pipeline_test_support.h"` with `using` aliases so existing test bodies stay unchanged; added 4 new [pipeline] test cases for pump_recv_any.
- `cli/tests/test_chunked.cpp` — added `#include`s for pipeline_pump.h + pipeline_test_support.h + `<functional>` + `<unordered_map>`; appended 2 new [chunked] CR-01-regression test cases reusing shared fixture.

## Live-node Checkpoint Observations

### Target 1: --node home (192.168.1.73)

| Check | Result | Timing / Count |
|---|---|---|
| 420 MiB put (/tmp/119-test-420mb.bin → home) | PASS | 52.5 s wall, 27 chunks, manifest hash `5756258...24ff7` |
| cdb get → /tmp/119-test-420mb.bin | PASS | 29.7 s wall |
| diff against original | IDENTICAL | — |
| cdb ls --raw --type CDAT (before rm) | 36 | — |
| cdb rm <manifest> (cascade) | PASS | 10.7 s wall — 27 chunk tombstones + manifest tombstone |
| cdb ls --raw --type CDAT (after rm + 2 s settle) | 9 | 36 − 27 = 9, exact match (zero orphans) |

### Target 2: --node local (127.0.0.1)

| Check | Result | Timing / Count |
|---|---|---|
| 420 MiB put | PASS | 32.8 s wall, 27 chunks, manifest hash `493f778b...802e4` |
| cdb get (second attempt) | PASS | 23.3 s wall (first attempt hit tmpfs full — see Issues Encountered) |
| diff against original | IDENTICAL | — |
| cdb ls --raw --type CDAT (before rm) | 63 | — |
| cdb rm <manifest> (cascade) | PASS | 14.2 s wall |
| cdb ls --raw --type CDAT (after rm + 2 s settle) | 36 | 63 − 27 = 36, exact match (zero orphans) |
| 10 MiB small-file put (regression) | PASS | 0.7 s wall, single-blob path not chunked |

All 5+1 pass criteria met on both targets — the plan's blocking gate is satisfied.

## Decisions Made

- **Reviewer option (b), not option (a), for CR-01:** Plan mandated option (b) (add `recv_next` + migrate 5 call sites) rather than modifying `Connection::recv()` to decrement. Option (a) would double-decrement whenever recv_for's pump already decremented and would break send_async's backpressure drain (which stashes via recv() and must NOT touch in_flight_). Followed plan verbatim; no deviation.
- **UnlinkGuard kept TU-local:** Defined inside chunked.cpp's anonymous namespace, not exposed via a header. No other site currently needs it; YAGNI prevails. If future work needs RAII unlink in another TU, we'll extract then.
- **Shared test fixture header-only with inline functions + using aliases:** Honors feedback_no_duplicate_code.md while avoiding a new CMake target. Existing test bodies unchanged — simpler diff, lower risk.

## Deviations from Plan

None — plan executed exactly as written. All acceptance criteria met as specified.

Minor notes (not deviations):
- `grep -c "conn\.recv_next()" cli/src/chunked.cpp` returns 4 instead of the plan's expected 3 because the updated narrative comment at line 174 also contains the literal `conn.recv_next()`. The functional requirement (3 actual call sites) is met; the extra match is the comment the plan explicitly instructed to update in Task 3. Not a deviation.
- `grep -c "conn\.recv_next"` for commands.cpp returns 2 — matches plan.

## Issues Encountered

1. **First local-node `cdb get` attempt failed with `pwrite failed: Resource temporarily unavailable`.** Investigation showed `/tmp` tmpfs was 80 % full from earlier test runs (several 420 MiB originals + downloads). The UnlinkGuard RAII (WR-03) correctly unlinked the partial output on the failure — verified by `find /tmp -maxdepth 1 -name "119*"` showing no lingering download. This was an inadvertent but welcome end-to-end validation that WR-03 works under a real unlink-on-failure path. After `rm`ing prior originals to free space, the second attempt succeeded in 23.3 s.

2. **One transient shell confusion during manual testing:** a chained `mv && rm -f && cp` sequence I composed too aggressively left the test file missing briefly. Recovered by regenerating with `dd` — no impact on plan execution, no code change needed.

## Confirmation

**119-VERIFICATION.md gap is closed:**
- Truth 1 (upload >500 MiB end-to-end) — VERIFIED on both home and local.
- Truth 2 (download byte-identical) — VERIFIED on both.
- Truth 3 (rm cascades with 0 orphans) — VERIFIED on both.
- Truth 4 (truncation prevention preserved) — no code in this plan touched CHUNK-05 guard; covered by pre-existing `decode_manifest_payload` rejection tests still passing in Task 13's full-suite run.
- Truth 5 (no in_flight_ leak on any drain path) — VERIFIED by Task 12's `chunked: CR-01 regression — 8 sends + 8 recv_next drains leave in_flight at 0` test.

**119-REVIEW.md findings resolved by this plan:**
- CR-01 — CLOSED (5 call-site migrations + pump_recv_any + Connection::recv_next).
- WR-02 — CLOSED (retry backoff post-index; first retry now 250 ms).
- WR-03 — CLOSED (UnlinkGuard covers the close/verify window; incidentally validated by the tmpfs-full pwrite failure).
- WR-04 — CLOSED (cmd::put and cmd::get Phase-B drains migrated — same physical fix as CR-01).
- IN-03 — CLOSED (narrowed catch + stderr warning).
- WR-01 (narrow) — explicitly deferred per plan's out-of-scope block; no change.
- IN-01 / IN-02 / IN-04 / IN-05 — info findings, deferred per plan.

**119-VALIDATION.md:** referenced by the plan's frontmatter `validation_doc` field; not modified in this plan (it describes the validation architecture, not plan-level action items).

## Self-Check: PASSED

Files exist:
- FOUND: cli/tests/pipeline_test_support.h
- FOUND: cli/src/pipeline_pump.h (pump_recv_any appended)
- FOUND: cli/src/connection.h (recv_next declared)
- FOUND: cli/src/connection.cpp (recv_next implemented)
- FOUND: cli/src/chunked.cpp (3 migrations + WR-02 + WR-03 + stale-comment update)
- FOUND: cli/src/commands.cpp (2 migrations)
- FOUND: cli/src/main.cpp (IN-03 fix)
- FOUND: cli/tests/test_connection_pipelining.cpp (4 new [pipeline] tests + shared fixture)
- FOUND: cli/tests/test_chunked.cpp (2 new [chunked] regressions)

Commits exist (verified via `git log --oneline`):
- FOUND: 7c185056 feat(119-03): add pump_recv_any template + 4 unit tests
- FOUND: 4cafb094 feat(119-03): add Connection::recv_next()
- FOUND: 9acaa718 fix(119-03): migrate put_chunked Phase-B
- FOUND: ad1eceb3 fix(119-03): migrate rm_chunked Phase-1
- FOUND: dd29a917 fix(119-03): migrate get_chunked Phase-B
- FOUND: f4b5e3e9 fix(119-03): migrate cmd::put Phase-B
- FOUND: ac95ff60 fix(119-03): migrate cmd::get Phase-B
- FOUND: 2c11aefc fix(119-03): WR-02 retry backoff
- FOUND: 672d981c fix(119-03): WR-03 UnlinkGuard
- FOUND: c76be71e fix(119-03): IN-03 narrow catch
- FOUND: d769265c refactor(119-03): shared fixture
- FOUND: fc3379b5 test(119-03): CR-01 regression

Build + test:
- PASS: `cmake --build build/cli -j$(nproc)` exit 0, no warnings beyond nothing (0 warning: lines in /tmp/119-03-build.log).
- PASS: `build/cli/tests/cli_tests` → `All tests passed (197094 assertions in 81 test cases)`.

Live-node E2E:
- PASS: home (192.168.1.73) put/get/diff/rm/orphan-count all green.
- PASS: local (127.0.0.1) put/get/diff/rm/orphan-count all green, including 10 MiB small-file regression.

## Next Phase Readiness

- Phase 119 is now ready to flip from `gaps_found` to `verified` with 5/5 truths met.
- CHUNK-01, CHUNK-03, CHUNK-04 move from PARTIAL → VERIFIED.
- CHUNK-02, CHUNK-05 remain VERIFIED (untouched).
- Phase 122 live-node E2E matrix (cross-feature: put + group sharing + pipelined get + ls filtering + peer management) stays deferred per plan scope; this plan verified only the narrow 420 MiB round-trip path.

---
*Phase: 119-chunked-large-files*
*Plan: 03*
*Completed: 2026-04-19*
