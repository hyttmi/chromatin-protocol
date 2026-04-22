---
phase: 126
slug: pre-shrink-audit
status: approved
nyquist_compliant: true
wave_0_complete: true
created: 2026-04-22
---

# Phase 126 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution. Fill Test Infrastructure, Sampling Rate, Per-Task Verification Map, and Wave 0 sections after planner produces PLAN.md and before execution starts.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 (existing `db/tests/net/` fixtures) |
| **Config file** | `CMakeLists.txt` — `chromatindb_tests` target |
| **Quick run command** | `cmake --build build -j$(nproc) --target chromatindb_tests && ./build/chromatindb_tests "[framing]" "[connection]"` |
| **Full suite command** | `cmake --build build -j$(nproc) --target chromatindb_tests && ./build/chromatindb_tests` |
| **Estimated runtime** | Quick: ~5 s · Full: ~30 s |

Per memory note `feedback_delegate_tests_to_user.md`: orchestrator does NOT execute these commands during planning or verification. Listed here for reference only; the user runs them.

---

## Sampling Rate

- **After every task commit:** User may run the quick command locally if they want early feedback. Not mandatory.
- **After every plan wave:** User runs the full suite.
- **Before `/gsd-verify-work`:** Full suite must be green.
- **Max feedback latency:** ~30 s (full suite).

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Threat Ref | Secure Behavior | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|------------|-----------------|-----------|-------------------|-------------|--------|
| T1 — node-side static_assert + TRANSPORT_ENVELOPE_MARGIN | 126-01 | 1 | AUDIT-02 | T-126-01 | MAX_FRAME_SIZE cannot be shrunk below 2 * STREAMING_THRESHOLD without breaking the build | Compile-time | `grep -q 'static_assert(MAX_FRAME_SIZE >= 2 \* STREAMING_THRESHOLD' db/net/framing.h && grep -q 'constexpr size_t TRANSPORT_ENVELOPE_MARGIN = 64' db/net/framing.h` | db/net/framing.h | ✅ green |
| T2 — node-side runtime assertion at enqueue_send waist | 126-01 | 1 | AUDIT-02 | T-126-02 | Any non-chunked send > STREAMING_THRESHOLD + envelope trips assert() in debug | Runtime (debug) | `grep -q 'assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN' db/net/connection.cpp && grep -q 'non-chunked send exceeds sub-frame size' db/net/connection.cpp` | db/net/connection.cpp | ✅ green |
| T3 — CLI-side file-scope STREAMING_THRESHOLD + static_assert | 126-01 | 1 | AUDIT-02 | T-126-01 | Symmetric compile-time invariant on the CLI side | Compile-time | `[ "$(grep -c '^static constexpr size_t STREAMING_THRESHOLD' cli/src/connection.cpp)" = "1" ] && grep -q 'static_assert(MAX_FRAME_SIZE >= 2 \* STREAMING_THRESHOLD' cli/src/connection.cpp` | cli/src/connection.cpp | ✅ green |
| T4 — CLI-side runtime assertion in Connection::send | 126-01 | 1 | AUDIT-02 | T-126-03 | Any CLI non-chunked send > STREAMING_THRESHOLD + envelope trips assert() in debug | Runtime (debug) | `grep -q 'assert(transport_bytes.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN' cli/src/connection.cpp && grep -Eq '^static constexpr size_t TRANSPORT_ENVELOPE_MARGIN = 64' cli/src/connection.cpp` | cli/src/connection.cpp | ✅ green |
| T5 — round-trip Catch2 TEST_CASEs on both sides of boundary | 126-01 | 1 | AUDIT-02 | T-126-04 | STREAMING_THRESHOLD - 1 payload round-trips through non-chunked path; STREAMING_THRESHOLD + 1 payload auto-chunks and reassembles byte-exact | Integration (loopback) | `grep -qE 'TEST_CASE\("streaming invariant: payload just under STREAMING_THRESHOLD' db/tests/net/test_connection.cpp && grep -qE 'TEST_CASE\("streaming invariant: payload just over STREAMING_THRESHOLD' db/tests/net/test_connection.cpp && [ "$(grep -cE '\[connection\]\[streaming\]' db/tests/net/test_connection.cpp)" -ge "2" ]` | db/tests/net/test_connection.cpp | ✅ green (grep; Catch2 run user-delegated) |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

**AUDIT-01 coverage:** Delivered as a documentation artifact (send-path inventory table) in 126-SUMMARY.md §"Send-Path Inventory" per D-07. Inventory found zero bypass sites (D-09 success condition); no fix work needed.

---

## Wave 0 Requirements

- [x] No new test framework install needed — Catch2 v3 already wired via CMake
- [x] Existing test fixture at `db/tests/net/test_connection.cpp:475–542` (acceptor + initiator + on_ready) was the pattern mirrored by the two new `[connection][streaming]` TEST_CASEs; no new harness scaffolding required

*Existing infrastructure covered all phase validation requirements.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Call-site inventory completeness | AUDIT-01 | The inventory is a one-time code-reading exercise producing a documented table, not a runtime-testable behavior. Verification = human read of the audit output in the plan summary. | After execution, read `126-SUMMARY.md` §"Send-Path Inventory" and confirm every `enqueue_send` / `send_encrypted` caller is classified. Cross-check with `grep -rn 'enqueue_send\|send_encrypted' db/` returning only sites explicitly listed in the summary. |

Everything else (runtime assertion firing, static_assert holding, round-trip test passing) is automated via Catch2.

---

## Validation Sign-Off

- [x] All tasks have `<automated>` verify or Wave 0 dependencies
- [x] Sampling continuity: every task has grep-level automated verify; no 3 consecutive tasks without feedback
- [x] Wave 0 covers all MISSING references — N/A, existing infra sufficient
- [x] No watch-mode flags — confirmed, `chromatindb_tests` runs once and exits
- [x] Feedback latency < 30 s — confirmed (grep checks are instant; user-delegated Catch2 run is ~5 s quick / ~30 s full)
- [x] `nyquist_compliant: true` set in frontmatter

**Approval:** approved (executed and verified 2026-04-22)
