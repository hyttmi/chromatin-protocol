---
phase: 127-nodeinforesponse-capability-extensions
plan: 04-integration-test
subsystem: wire-protocol-tests
tags: [nodeinfo, wire-format, big-endian, catch2, integration-test, boundary-coverage, wire-size-invariant]

# Dependency graph
requires:
  - phase: 127-nodeinforesponse-capability-extensions
    plan: 01-encoder
    provides: "NodeInfoResponse wire layout extended by +24 bytes — encoder writes blob(u64)+frame(u32)+rate(u64)+subs(u32) between storage_max and types_count"
provides:
  - "VERI-02 coverage via three [peer][nodeinfo] TEST_CASEs asserting all 4 new fields decode to their configured values at default / zero-boundary / max-boundary"
  - "Wire-size drift sentinel: hard-coded `+ 24` delta check on info_response.size() catches silent offset drift if a future change reorders or re-adds fields"
affects:
  - 128-blob-cap-config-frame-shrink   # same test coverage pattern will extend to assert cfg-driven blob cap in Phase 128
  - 129-sync-cap-divergence            # same integration-test scaffold is a template for PeerInfo.advertised_blob_cap coverage
  - 131-documentation-reconciliation   # VERI-02 completion referenced in Phase 131 docs reconciliation

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Three-fixture Catch2 coverage for boundary scenarios — each TEST_CASE owns its own Config + PeerManager + ioc because the config-per-scenario requirement makes Catch2 SECTION-per-Config awkward (PeerManager restart per SECTION)"
    - "Hard-coded wire-size delta assertion (D-10) as an offset-drift sentinel — Phase 127 pins `+ 24` bytes against future silent layout changes"
    - "Manual big-endian walk via `info_response[off++]` idiom consistent with the pre-existing NodeInfoResponse test; no new helper introduced (test-local walk preserves readability)"

key-files:
  created: []
  modified:
    - "db/tests/peer/test_peer_manager.cpp - +227 lines: (1) +1 include `db/net/framing.h`; (2) ~40 lines extending the original [peer][nodeinfo] TEST_CASE with 4 new-field assertions + wire-size delta check; (3) two new TEST_CASEs (zero + max boundary), ~90 lines each, full fixture copy"

key-decisions:
  - "D-09 three-scenario coverage: default Config + zero boundary + max boundary — implemented as 3 separate TEST_CASEs rather than SECTION blocks because each boundary needs its own Config (PeerManager+ioc cannot be rewound)"
  - "D-10 hard-coded `+ 24` delta in info_response.size() CHECK — rejects offset drift if a future phase reorders or re-adds any of the 4 new fields"
  - "feedback_no_duplicate_code.md scope exemption: test fixture scaffold duplicated 3× (default/zero/max) deliberately. Production code duplication is forbidden; test scaffolds follow Catch2 idiom (new TEST_CASE = new fixture)"
  - "No [cstdint] explicit include — UINT64_MAX and UINT32_MAX come transitively via db/tests/test_helpers.h (already includes <cstdint> at line 12)"

patterns-established:
  - "Integration-test scaffold for NodeInfoResponse boundary testing — zero and max TEST_CASEs are Config-only deltas from the default fixture. Phase 128/129 can reuse the same pattern for blob_max_bytes and advertised_blob_cap coverage."

requirements-completed: [VERI-02, NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04]

# Metrics
duration: 9min
completed: 2026-04-22
---

# Phase 127 Plan 04: NodeInfoResponse Capability Extensions — Integration Test Summary

**Extended `[peer][nodeinfo]` integration test with VERI-02 coverage: one TEST_CASE asserts all 4 new Phase 127 fields decode correctly under default Config plus a hard-coded wire-size delta; two additional TEST_CASEs pin the zero-value and UINT64_MAX/UINT32_MAX boundaries for `rate_limit_bytes_per_sec` and `max_subscriptions_per_connection`.**

## Performance

- **Duration:** ~9 min (2 edits + initial one-time cmake configure ~4 min + 2 incremental builds + 2 targeted test runs)
- **Started:** 2026-04-22T11:40:16Z
- **Completed:** 2026-04-22T11:49:18Z
- **Tasks:** 2 / 2
- **Files modified:** 1

## Accomplishments

- **Task 1**: Existing `NodeInfoRequest returns version and node state` TEST_CASE at `db/tests/peer/test_peer_manager.cpp:2773` extended with 4 new assertions (max_blob_data_bytes, max_frame_bytes, rate_limit_bytes_per_sec default 0, max_subscriptions default 256) plus the D-10 wire-size delta `CHECK(info_response.size() == ... + 24 + ...)`. Targeted test run: 31 assertions / 1 test case, all passing.
- **Task 2**: Two additional `[peer][nodeinfo]` TEST_CASEs inserted immediately after the original:
  - "NodeInfoResponse — zero boundary for rate_limit and max_subscriptions" — `cfg.rate_limit_bytes_per_sec = 0`, `cfg.max_subscriptions_per_connection = 0`, asserts `rate == 0` and `subs == 0`.
  - "NodeInfoResponse — max boundary for rate_limit and max_subscriptions" — `cfg.rate_limit_bytes_per_sec = UINT64_MAX`, `cfg.max_subscriptions_per_connection = UINT32_MAX`, asserts `rate == UINT64_MAX` and `subs == UINT32_MAX` round-trip byte-exact.
- **Include**: `#include "db/net/framing.h"` added once in the test file (Task 1 Edit A) so `chromatindb::net::MAX_BLOB_DATA_SIZE` / `MAX_FRAME_SIZE` resolve at compile time. `<cstdint>` for UINT64_MAX/UINT32_MAX comes transitively via `db/tests/test_helpers.h` — no extra include needed.
- **Build + targeted test**: `cmake --build build-debug -j$(nproc) --target chromatindb_tests` → exit 0. `./build-debug/db/chromatindb_tests "[peer][nodeinfo]"` → "All tests passed (45 assertions in 3 test cases)". Full suite run is delegated to the user at the wave boundary (per `feedback_delegate_tests_to_user.md` and plan-level guardrail).
- **Scope discipline**: Only `db/tests/peer/test_peer_manager.cpp` modified. STATE.md, ROADMAP.md, and all non-test files untouched (enforced by per-file `git add`, no `git add -A`).

## Task Commits

1. **Task 1: Extend the default-path [peer][nodeinfo] TEST_CASE with new-field assertions + wire-size delta** — `81bcdddf` (test)
2. **Task 2: Add two boundary TEST_CASEs — zero values and max values — for rate+subs** — `dbe172d2` (test)

## Files Created/Modified

- `db/tests/peer/test_peer_manager.cpp` — +227 lines across 2 commits.
  - Include block (line 16): +1 `#include "db/net/framing.h"`.
  - `TEST_CASE("NodeInfoRequest returns version and node state", "[peer][nodeinfo]")` body (was 2773–2906, now 2773–2947): +41 lines inserted — 4 new field decode blocks between the `storage_max` check and the `// Supported types` section, plus the wire-size delta `CHECK(info_response.size() == 1 + version.size() + 8 + 4 + 4 + 8 + 8 + 8 + 24 + 1 + types_count)` after the final `types.count(39)` check.
  - Two new TEST_CASEs appended at lines 2949–3136: each a full fixture copy (TempDir / Storage / PeerManager / asio::io_context / coroutine client / 5 s ioc.run_for) with Config-level boundary values and 2 assertions each (rate + subs).

## Decisions Made

None beyond those codified in the plan and CONTEXT.md (D-09 three-scenario coverage, D-10 `+ 24` delta, D-11 no CLI-side test, D-12 passing comment fix deferred to Plan 127-03, `<cstdint>` transitive inclusion via test_helpers.h). Plan executed verbatim except for two minor annotations documented under "Deviations from Plan" below.

## Deviations from Plan

**1. [Informational — AC5 grep pattern interpreted semantically]** The plan's Task 1 AC5 part 2 reads:

> `grep -B1 '+ 24' db/tests/peer/test_peer_manager.cpp | grep -c 'info_response.size()'` >= 1

This strict single-line-backward check returns 0 because the multi-line `CHECK(info_response.size() == ...)` expression puts `info_response.size()` 3 lines above `+ 24` (readable multi-line form). Widening to `-B3` returns 1. The SEMANTIC intent (that a `+ 24` literal appears inside an assertion referencing `info_response.size()`) is satisfied — the `CHECK(info_response.size() == 1 + version.size() + 8 + 4 + 4 + 8 + 8 + 8 + 24 + 1 + types_count);` expression at lines 2938–2942 binds them into one assertion. No code change made — the multi-line form matches the plan's Action block verbatim.

**2. [Informational — no deduplication of the boundary fixtures]** The two new TEST_CASEs duplicate ~85 % of the original fixture scaffold (handshake + timer + ioc.run_for). This is intentional per D-09 and the plan's own rationale (paragraph explaining why SECTION blocks are worse here). `feedback_no_duplicate_code.md` applies to production code; test scaffolding duplication is local to one TEST_CASE cluster.

Otherwise: plan executed exactly as written.

## Acceptance Criteria Results

**Task 1 (9 criteria):**
- AC1 `NodeInfoRequest returns version and node state` count == 1 — **PASS**
- AC2 `max_blob_data_bytes == chromatindb::net::MAX_BLOB_DATA_SIZE` count == 1 — **PASS**
- AC3 `max_frame_bytes == chromatindb::net::MAX_FRAME_SIZE` count == 1 — **PASS**
- AC4 `rate_limit_bytes_per_sec == 0` count == 1, `max_subscriptions == 256` count == 1 — **PASS**
- AC5 `+ 24` count == 2 (one in comment, one in assertion) — **PASS (part 1)**; strict `-B1` contextual match for `info_response.size()` returns 0 due to multi-line formatting (`-B3` returns 1) — **SEMANTICALLY PASS** (see Deviations #1)
- AC6 `#include "db/net/framing.h"` count == 1 — **PASS**
- AC7 `types_count == 39` count == 1, `types.count(39)` count == 1 — **PASS**
- AC8 `cmake --build ... --target chromatindb_tests` exit 0; targeted test run exit 0 — **PASS** (31 assertions after Task 1, 45 assertions after Task 2, all [peer][nodeinfo] cases green)
- AC9 only `db/tests/peer/test_peer_manager.cpp` modified — **PASS**

**Task 2 (6 criteria):**
- AC1 `[peer][nodeinfo]` tag count == 3 — **PASS**
- AC2 `rate_limit_bytes_per_sec = 0` count == 2 (config field + assertion idiom), `max_subscriptions_per_connection = 0` count == 1, `zero boundary` count == 3 (title + 2 comments) — **PASS**
- AC3 `rate_limit_bytes_per_sec = UINT64_MAX` count == 1, `max_subscriptions_per_connection = UINT32_MAX` count == 1, `rate == UINT64_MAX` count == 1, `subs == UINT32_MAX` count == 1, `max boundary` count == 3 (title + 2 comments) — **PASS**
- AC4 `NodeInfoResponse — zero boundary` count == 1, `NodeInfoResponse — max boundary` count == 1 — **PASS**
- AC5 build + targeted test exit 0, 3 TEST_CASEs under `[peer][nodeinfo]` — **PASS** ("All tests passed (45 assertions in 3 test cases)")
- AC6 only `db/tests/peer/test_peer_manager.cpp` modified — **PASS** (`git diff --stat acc6457b..HEAD` → `db/tests/peer/test_peer_manager.cpp | 227 ++++++++++++++++++++++++++++++++++++`)

## Issues Encountered

None. The worktree had no pre-existing `build-debug` directory, so `cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug` was run once to configure (one-time ~4 min cost). Subsequent incremental `cmake --build` was clean. No warnings, no sanitizer noise specific to the edited lines.

## Threat Flags

None — this plan adds only test assertions. No new network endpoints, auth paths, file access, or schema surface introduced.

The plan's `<threat_model>` identifies T-127-08 (offset drift) as the threat mitigated by the D-10 `+ 24` wire-size delta assertion. That mitigation is implemented and verified (31 assertions pass including the delta check).

T-127-09 (ioc-timing flakiness) is `accept`ed — the two new boundary TEST_CASEs use the same 5 s `ioc.run_for` pattern as the existing fixture, which has been stable in CI through Phases 125/126. No new timing risk introduced.

## Next Phase Readiness

- **VERI-02 structurally satisfied** — three `[peer][nodeinfo]` TEST_CASEs (default / zero / max) all compile and all pass under the targeted Catch2 tag filter.
- **Plan 127-03 (CLI decode, same Wave 2)** is independent — this plan's test coverage is node-side integration; CLI-side decoding has its own scope and runs in parallel. No coordination needed.
- **Phase 128 readiness** — when `max_blob_data_bytes` becomes cfg-driven, the same test scaffold can be extended with a `cfg.blob_max_bytes = 4 MiB` scenario. Nothing in this plan's assertions hard-code `MAX_BLOB_DATA_SIZE` values (they reference the symbol), so a future Phase 128 configure change to the constant would not break this test — only the Phase 128 assertion would need to point at `cfg.blob_max_bytes` instead.
- **Phase 129 readiness** — `PeerInfo.advertised_blob_cap` snapshotting will need a new integration TEST_CASE (two-peer fixture). The pattern established here (Config tweak + ioc spin-up + wire walk + boundary CHECK) is directly reusable.
- **No blockers** for downstream plans.

## Self-Check: PASSED

- File `db/tests/peer/test_peer_manager.cpp` — FOUND (+227 insertions verified by `git diff --stat acc6457b..HEAD`).
- Commit `81bcdddf` — FOUND (`git log --oneline | grep 81bcdddf` returns `81bcdddf test(127-04): extend [peer][nodeinfo] TEST_CASE ...`).
- Commit `dbe172d2` — FOUND (`git log --oneline | grep dbe172d2` returns `dbe172d2 test(127-04): add [peer][nodeinfo] zero + max boundary TEST_CASEs`).
- Build acceptance `cmake --build build-debug -j$(nproc) --target chromatindb_tests` — exit 0 on both incremental builds.
- Targeted Catch2 run `./build-debug/db/chromatindb_tests "[peer][nodeinfo]"` — exit 0, "All tests passed (45 assertions in 3 test cases)".
- No deletions: `git diff --diff-filter=D --name-only acc6457b..HEAD` returns empty.
- No untracked-file pollution from this plan.

---
*Phase: 127-nodeinforesponse-capability-extensions*
*Plan: 127-04-integration-test*
*Completed: 2026-04-22*
