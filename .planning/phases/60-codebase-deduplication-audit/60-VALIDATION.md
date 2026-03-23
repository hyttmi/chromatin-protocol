---
phase: 60
slug: codebase-deduplication-audit
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-23
---

# Phase 60 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++ unit tests) + Docker integration tests |
| **Config file** | db/tests/CMakeLists.txt, tests/docker/ |
| **Quick run command** | `cd build && ctest --output-on-failure -j$(nproc)` |
| **Full suite command** | `cd build && ctest --output-on-failure -j$(nproc)` |
| **Estimated runtime** | ~60 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && cmake --build . && ctest --output-on-failure -j$(nproc)`
- **After every plan wave:** Run full suite
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 90 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 60-01-01 | 01 | 1 | SC-1 | build+unit | `cmake --build build && cd build && ctest --output-on-failure` | existing | pending |
| 60-01-02 | 01 | 1 | SC-2 | build+unit | `cmake --build build && cd build && ctest --output-on-failure` | existing | pending |
| 60-01-03 | 01 | 1 | SC-3 | grep audit | `grep -rn 'to_hex\|ns_to_hex\|from_hex' db/ relay/ --include='*.cpp' --include='*.h'` | n/a | pending |
| 60-01-04 | 01 | 1 | SC-4 | build+unit | `cmake --build build && cd build && ctest --output-on-failure` | existing | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements. No new test framework or test stubs needed -- this phase modifies includes and verifies existing tests still pass.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have automated verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 90s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
