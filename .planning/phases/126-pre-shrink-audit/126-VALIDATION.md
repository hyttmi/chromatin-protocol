---
phase: 126
slug: pre-shrink-audit
status: draft
nyquist_compliant: false
wave_0_complete: false
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

*To be filled once planner produces tasks.*

| Task ID | Plan | Wave | Requirement | Threat Ref | Secure Behavior | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|------------|-----------------|-----------|-------------------|-------------|--------|
| (TBD after planner runs) | | | | | | | | | |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] No new test framework install needed — Catch2 v3 already wired via CMake
- [ ] Existing test fixture at `db/tests/net/test_connection.cpp:475–542` (acceptor + initiator + on_ready) is the pattern to mirror; no new harness scaffolding required

*Existing infrastructure covers all phase validation requirements.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Call-site inventory completeness | AUDIT-01 | The inventory is a one-time code-reading exercise producing a documented table, not a runtime-testable behavior. Verification = human read of the audit output in the plan summary. | After execution, read `126-SUMMARY.md` §"Send-Path Inventory" and confirm every `enqueue_send` / `send_encrypted` caller is classified. Cross-check with `grep -rn 'enqueue_send\|send_encrypted' db/` returning only sites explicitly listed in the summary. |

Everything else (runtime assertion firing, static_assert holding, round-trip test passing) is automated via Catch2.

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies (to confirm post-planner)
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify (to confirm post-planner)
- [ ] Wave 0 covers all MISSING references — N/A, existing infra sufficient
- [ ] No watch-mode flags — confirmed, `chromatindb_tests` runs once and exits
- [ ] Feedback latency < 30 s — confirmed
- [ ] `nyquist_compliant: true` set in frontmatter — pending post-planner review

**Approval:** pending
