---
phase: 32
slug: test-relocation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-17
---

# Phase 32 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | None (Catch2 is header-only + CMake module) |
| **Quick run command** | `cd build && ctest -N \| grep "Total Tests:"` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~120 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && cmake -S .. -B . && cmake --build . && ctest -N | grep "Total Tests:"`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green + test count 313 + no files in tests/
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 32-01-01 | 01 | 1 | CLEAN-01 | smoke | `ctest -N \| grep "Total Tests:"` (compare before/after) | N/A | ⬜ pending |
| 32-01-02 | 01 | 1 | CLEAN-01 | integration | `ctest --output-on-failure` | N/A | ⬜ pending |
| 32-01-03 | 01 | 1 | CLEAN-01 | smoke | `ls tests/ 2>/dev/null && echo FAIL \|\| echo PASS` | N/A | ⬜ pending |
| 32-01-04 | 01 | 1 | CLEAN-01 | smoke | `cmake -S . -B build && cmake --build build` | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements. No new test files, config, or fixtures needed. This phase moves existing tests, not creates new ones.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
