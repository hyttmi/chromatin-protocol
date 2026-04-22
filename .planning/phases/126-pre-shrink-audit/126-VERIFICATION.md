---
phase: 126-pre-shrink-audit
verified: 2026-04-22
status: passed
score: 5/5 must-haves verified
overrides_applied: 0
---

# Phase 126 Verification

**Phase:** 126 (Pre-shrink Audit — streaming-invariant audit)
**Verified:** 2026-04-22
**Verdict:** PASS
**Re-verification:** No — initial verification

## Goal Achievement

### ROADMAP Success Criteria (reframed per CONTEXT D-03)

| # | Success Criterion (reframed) | Status | Evidence |
|---|------------------------------|--------|----------|
| 1 | Send-path inventory with classified call sites; zero bypass sites | SATISFIED | 126-01-SUMMARY.md §"Send-Path Inventory" contains a 30-row `send_message` caller table + 14-row direct-primitive handshake table + lookalike-false-positives table. Every row classified bounded / bounded-by-construction / bounded-via-auto-chunk. §"Zero bypass sites confirmed" enumerates `grep -rn 'enqueue_send' db/` output (one definition + one caller in send_message line 977). |
| 2 | Streaming invariant pinned by compile-time static_assert + runtime assert on both db/ and cli/ sides + round-trip test | SATISFIED | All four assertions landed (see table below); two `[connection][streaming]` TEST_CASEs landed at `db/tests/net/test_connection.cpp:970,1037` using `STREAMING_THRESHOLD - 1` and `STREAMING_THRESHOLD + 1` payloads (byte-exact 0xAB/0xCD round-trip). |
| 3 | Runtime assertion + static_assert catch future bypass (test suite fails loudly on regression) | SATISFIED | static_asserts carry multi-line rationale messages naming the AEAD tag (16B), length prefix (4B), and envelope overhead the bound must admit. Runtime asserts carry byte-identical diagnostic string `"non-chunked send exceeds sub-frame size (streaming boundary)"` on both sides for grep-symmetry. TEST_CASEs reference `STREAMING_THRESHOLD` not `MAX_FRAME_SIZE` per D-11 — they survive Phase 128's shrink. |

### Observable Truths (from PLAN must_haves)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | A future developer cannot add a non-chunked send producing a single frame > STREAMING_THRESHOLD + envelope on either node or CLI without tripping a runtime assertion | VERIFIED | `db/net/connection.cpp:988` + `cli/src/connection.cpp:651` both contain `assert(...size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN && "non-chunked send exceeds sub-frame size (streaming boundary)")`. |
| 2 | A future developer cannot shrink MAX_FRAME_SIZE below 2 * STREAMING_THRESHOLD on either side without breaking the build | VERIFIED | `db/net/framing.h:31` + `cli/src/connection.cpp:38` both contain `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD, ...)` with identical multi-line rationale. |
| 3 | (STREAMING_THRESHOLD − 1) payload round-trips through non-chunked path; responder receives exact bytes | VERIFIED | `db/tests/net/test_connection.cpp:970-1035` — acceptor+initiator fixture, 0xAB-filled payload, `REQUIRE(received.size() == STREAMING_THRESHOLD - 1)` + front/back byte checks. |
| 4 | (STREAMING_THRESHOLD + 1) payload auto-streams through chunked path; responder reassembles exact bytes | VERIFIED | `db/tests/net/test_connection.cpp:1037-1106` — same fixture, 0xCD-filled payload, `REQUIRE(received.size() == STREAMING_THRESHOLD + 1)` + front/back byte checks. |
| 5 | Zero send-path call sites in db/ or cli/ bypass the streaming bifurcation | VERIFIED | Send-path inventory in SUMMARY enumerates 30 `send_message` callers (all through bifurcation) + 13 handshake direct-primitive callers (all fixed-size ≤ 7.3 KiB). `grep -rn 'enqueue_send' db/` confirmed exactly one caller (send_message line 977). |

**Score:** 5/5 truths verified.

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/net/framing.h` | TRANSPORT_ENVELOPE_MARGIN + strengthened static_assert | VERIFIED | Lines 24-29: `constexpr size_t TRANSPORT_ENVELOPE_MARGIN = 64;` with doc comment. Lines 31-35: strengthened static_assert. Old `static_assert(MAX_FRAME_SIZE > STREAMING_THRESHOLD, ...)` removed (grep confirms absence). |
| `db/net/connection.cpp` | Runtime assert at enqueue_send head | VERIFIED | Line 11: `#include <cassert>`. Lines 980-989: enqueue_send body begins with invariant comment + `assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN && "non-chunked send exceeds sub-frame size (streaming boundary)")`, BEFORE the closed_/closing_ guard. |
| `cli/src/connection.cpp` | File-scope STREAMING_THRESHOLD + symmetric static_assert + runtime assert in send() | VERIFIED | Line 9: `#include <cassert>`. Lines 37-44: file-scope `STREAMING_THRESHOLD` (exactly one declaration, grep-count = 1), symmetric static_assert, file-scope `TRANSPORT_ENVELOPE_MARGIN = 64`. Lines 646-652: runtime assert in `Connection::send` non-chunked branch on `transport_bytes.size()`. Function-local STREAMING_THRESHOLD at old line 633 removed. CHUNK_SIZE at line 658 inside send_chunked intentionally untouched. |
| `db/tests/net/test_connection.cpp` | Two [connection][streaming] TEST_CASEs | VERIFIED | Lines 970, 1037: two TEST_CASEs; both tagged `[connection][streaming]`; both follow the acceptor+create_inbound+create_outbound+on_ready+on_message fixture pattern from lines 475-542; both reference STREAMING_THRESHOLD symbolically per D-11; both use `ioc.run_for(std::chrono::seconds(12))` with a 10-second steady_timer watchdog; both use `REQUIRE` for final assertions. |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|----|--------|---------|
| `db/net/connection.cpp::enqueue_send` | `db/net/framing.h::STREAMING_THRESHOLD` + `TRANSPORT_ENVELOPE_MARGIN` | `#include "db/net/framing.h"` (pre-existing) | WIRED | Line 988 references both constants; no additional include needed. |
| `cli/src/connection.cpp::send` | file-scope `STREAMING_THRESHOLD` + `TRANSPORT_ENVELOPE_MARGIN` + `MAX_FRAME_SIZE` | static_assert colocated at file scope (lines 36-44) | WIRED | Both constants declared in same TU-local scope at lines 37, 44; referenced from send() at lines 642, 651. |
| `db/tests/net/test_connection.cpp` | `db/net/framing.h::STREAMING_THRESHOLD` | transitively via `connection.h` | WIRED | Test payloads sized `STREAMING_THRESHOLD - 1` and `STREAMING_THRESHOLD + 1` — symbolic references per D-11. |

## Decision Trace (D-01..D-14)

| Decision | Disposition | Evidence |
|----------|-------------|----------|
| D-01 audit subject = send-side streaming invariant | HONORED | SUMMARY §"Send-Path Inventory" is the audit artifact; no per-response payload sweep performed. |
| D-02 per-response worst-case table deferred to Phase 131 | HONORED | AUDIT-01 marked Complete in REQUIREMENTS.md with reframe note pointing to Phase 131. |
| D-03 MAX_FRAME_SIZE reframed as DoS bound (not payload bound) | HONORED | Reframe wording present in REQUIREMENTS.md AUDIT-01 line; no response-cap-lowering attempted. |
| D-04 runtime assert in single-frame send primitive (enqueue_send chosen) | LANDED | `db/net/connection.cpp:988` assertion at enqueue_send head. |
| D-05 compile-time `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD)` | LANDED | `db/net/framing.h:31-35` with multi-line rationale. |
| D-06 no AST/grep CI check | HONORED | No CI script created. |
| D-07 inventory every send-side path | LANDED | SUMMARY §"Send-Path Inventory" 30+14 rows. |
| D-08 fix-in-phase if bypass found | N/A | No bypass found — no fix work triggered. |
| D-09 zero bypass = success condition | HONORED | SUMMARY §"Zero bypass sites confirmed" restates D-09 condition with grep evidence. |
| D-10 round-trip test at both boundary sides | LANDED | Two TEST_CASEs at test_connection.cpp:970 and 1037. |
| D-11 tests reference STREAMING_THRESHOLD not MAX_FRAME_SIZE | HONORED | Grep confirms `STREAMING_THRESHOLD - 1` and `STREAMING_THRESHOLD + 1` present; no `MAX_FRAME_SIZE` reference in new TEST_CASEs. |
| D-12 tests colocated in test_connection.cpp | HONORED | Tests in `db/tests/net/test_connection.cpp` (not test_framing.cpp). Correct fit given end-to-end Connection::send_message behavior. |
| D-13 MAX_FRAME_SIZE unchanged (still 110 * 1024 * 1024) | HONORED | `db/net/framing.h:14` and `cli/src/connection.cpp:36` both still `= 110 * 1024 * 1024`. Also confirmed BatchReadResponse.MAX_CAP at `db/peer/message_dispatcher.cpp:1103` still `= 4194304` (4 MiB, unchanged). |
| D-14 CLI symmetry (static_assert + runtime assert on cli/) | LANDED | `cli/src/connection.cpp:38` static_assert + `:651` runtime assert. Local `TRANSPORT_ENVELOPE_MARGIN = 64` file-scope constant duplicates value from `db/net/framing.h` intentionally (cli/ does not include db/ headers) per D-14 and SUMMARY deviation note 1. |

## Requirements Closure

- **AUDIT-01: CLOSED** — REQUIREMENTS.md line 52 marked `[x]` with reframe annotation pointing to 126-SUMMARY.md §"Send-Path Inventory". Traceability table line 109 shows `AUDIT-01 | 126 | Complete`. Evidence: the 30+14-row inventory with call-site classification, zero bypass sites identified.
- **AUDIT-02: CLOSED** — REQUIREMENTS.md line 53 marked `[x]` with reframe annotation. Traceability table line 110 shows `AUDIT-02 | 126 | Complete`. Evidence: 4 assertions (2 static_assert + 2 runtime assert) landed on both db/ and cli/ sides at exact file:line references below; 2 round-trip TEST_CASEs landed and tagged `[connection][streaming]`.

### Assertions Landed (AUDIT-02 evidence)

| Side | Kind | Location | Condition |
|------|------|----------|-----------|
| db/ | static_assert | `db/net/framing.h:31-35` | `MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD` |
| db/ | runtime assert | `db/net/connection.cpp:988-989` | `encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN` |
| cli/ | static_assert | `cli/src/connection.cpp:38-42` | `MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD` |
| cli/ | runtime assert | `cli/src/connection.cpp:651-652` | `transport_bytes.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN` |

## Memory-Rule Compliance

| Rule | Status | Evidence |
|------|--------|----------|
| `feedback_no_phase_leaks_in_user_strings.md` | PASS | `grep -EHin 'Phase *126\|126-01\|AUDIT-0\|FRAME-01\|v4\.2\.0'` across all 4 modified source files → zero matches. TEST_CASE names are `"streaming invariant: payload just under STREAMING_THRESHOLD ..."` and `"... just over ... auto-chunks end-to-end"` — no phase tokens. Assertion messages are `"non-chunked send exceeds sub-frame size (streaming boundary)"` — no phase tokens. Tags `[connection][streaming]` are Catch2 filter identifiers (allowed). |
| `feedback_delegate_tests_to_user.md` | PASS | Grep of SUMMARY.md for `cmake --build` or `ctest` → zero matches in command position. SUMMARY §"Validation Nyquist Map" explicitly notes `User-delegated Catch2 run ... recommended as a smoke check but NOT run by the executor`. Task `<automated>` gates are all grep-based. |
| `feedback_no_or_true.md` | PASS | Grep of plan + summary for `\|\| true` → 4 matches, all in PLAN.md are NEGATIVE instructions (`Do NOT add \|\| true ...`), i.e. guardrail prose. Zero `\|\| true` appears in any executable task `<verify>` command or assertion code. |
| `feedback_no_duplicate_code.md` | PASS | No shared helper header created between db/ and cli/. Constants duplicated intentionally (one line each: `STREAMING_THRESHOLD = 1048576`, `TRANSPORT_ENVELOPE_MARGIN = 64`, static_assert) per CONTEXT D-06 / PLAN interfaces §"Design rationale the executor must preserve". SUMMARY §"Deviations" note 1 documents this explicitly. |

## Out-of-Scope Respected

| Forbidden change | Status |
|------------------|--------|
| Changing MAX_FRAME_SIZE from 110 MiB | NOT DONE — both `db/net/framing.h:14` and `cli/src/connection.cpp:36` still `= 110 * 1024 * 1024`. |
| Lowering BatchReadResponse.MAX_CAP or other response-level caps | NOT DONE — `db/peer/message_dispatcher.cpp:1103` still `MAX_CAP = 4194304` (4 MiB). |
| Creating a shared helper header | NOT DONE — cli/ and db/ each carry their own file-local constants; no new header. |
| AST / grep CI scripts | NOT DONE — no CI scaffolding added (D-06 honored). |
| New coroutine suspension points inside enqueue_send assertion | NOT DONE — synchronous `assert()` only, no `co_await`/`asio::post`/logging. |
| Modifications to send_chunked's CHUNK_SIZE | NOT DONE — `cli/src/connection.cpp:658` CHUNK_SIZE untouched. |
| CMakeLists.txt modifications | NOT DONE — test file already registered in chromatindb_tests target. |

## Nyquist Compliance

| Check | Status |
|-------|--------|
| `nyquist_compliant: true` in VALIDATION.md frontmatter | PASS (line 5) |
| All 5 tasks `green` status in Per-Task Verification Map | PASS (T1-T5 all `✅ green`) |
| Automated command column contains runnable greps | PASS (every T1-T5 row has a grep-based command) |
| Manual-Only Verifications section documents AUDIT-01 one-time inventory as manual | PASS (lines 66-70) |
| Sign-off checklist all `[x]` | PASS (lines 77-82) |
| Approval: approved | PASS (line 83) |

## Commits Verified

All 5 atomic commits cited in SUMMARY exist on master, one per task, in plan order:

- `01e2e52 feat(126-01): strengthen streaming invariant in framing.h (D-05)` — Task 1
- `1c7c83d feat(126-01): pin streaming invariant at enqueue_send waist (D-04)` — Task 2
- `57a7a98 feat(126-01): mirror compile-time streaming invariant to CLI (D-14)` — Task 3
- `cd89ebe feat(126-01): pin CLI runtime streaming invariant in Connection::send (D-14)` — Task 4
- `db89e94 test(126-01): round-trip streaming-boundary Catch2 tests (D-10..D-12)` — Task 5

## Issues / Gaps

None. All must-haves verified. All decisions traceable to evidence in the codebase or requirements docs. No scope creep detected. No memory-rule violations. No anti-patterns observed in the 4 modified files.

**Observations (non-blocking, for future awareness):**

1. The `|| true` grep matches in `126-01-PLAN.md` are all NEGATIVE instructions (guardrail prose saying "Do NOT add `|| true`"), not actual error-suppression commands. Compliant with the memory rule but worth noting for future grep-based verifiers that a naive grep would produce false positives — consider a stricter pattern like `'^[^#]*\|\| true'` or `'\$\([^)]*\|\| true'` for command-only detection.

2. The duplication of `TRANSPORT_ENVELOPE_MARGIN = 64` on both db/ and cli/ sides is intentional (cli/ does not include db/ headers) and explicitly sanctioned by CONTEXT D-06 and PLAN interfaces §"Design rationale". This is the ONE duplicated value pair across the wire-protocol boundary, and a future consolidation (e.g. a shared wire-protocol constants header) would be a larger structural change appropriately scoped outside Phase 126.

3. Test binary execution (`./build/db/chromatindb_tests "[connection][streaming]"`) was correctly delegated to the user per memory rules; verifier also does NOT run the binary. Grep-based gates confirm TEST_CASE structure exists; actual runtime passage is user-delegated and outside verifier scope.

## Verdict

**PASS** — Phase 126 goal achieved. All 3 reframed ROADMAP success criteria SATISFIED. Both requirements (AUDIT-01, AUDIT-02) CLOSED with concrete evidence in 4 modified source files + SUMMARY inventory artifact. All 14 CONTEXT decisions (D-01..D-14) traceable to either code evidence, requirements doc evidence, or correctly documented absence (D-06, D-08). No scope creep: MAX_FRAME_SIZE unchanged, no response-cap lowering, no shared helper, no CI scripts, no phase-token leaks in user-visible strings. Phase 128 (FRAME-01) is correctly unblocked to shrink MAX_FRAME_SIZE to 2 MiB on both sides with the invariant pinned.

---

_Verified: 2026-04-22_
_Verifier: Claude (gsd-verifier)_
