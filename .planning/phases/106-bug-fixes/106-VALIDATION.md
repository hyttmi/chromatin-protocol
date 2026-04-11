---
phase: 106
slug: bug-fixes
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-11
---

# Phase 106 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | relay/tests/CMakeLists.txt |
| **Quick run command** | `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure` |
| **Full suite command** | `cd build && ctest -j$(nproc) --output-on-failure` |
| **Estimated runtime** | ~10 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure`
- **After every plan wave:** Run `cd build && ctest -j$(nproc) --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 10 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 106-01-01 | 01 | 1 | FIX-01a | unit | `ctest -R "translator.*node_info" --output-on-failure` | ✅ extend | ⬜ pending |
| 106-01-02 | 01 | 1 | FIX-01b | unit | `ctest -R "translator.*stats" --output-on-failure` | ✅ update | ⬜ pending |
| 106-01-03 | 01 | 1 | FIX-01c | unit | `ctest -R "translator.*time_range" --output-on-failure` | ❌ W0 | ⬜ pending |
| 106-01-04 | 01 | 1 | FIX-01d | unit | `ctest -R "translator.*delegation" --output-on-failure` | ❌ W0 | ⬜ pending |
| 106-01-05 | 01 | 1 | FIX-01e | unit | `ctest -R "translator" --output-on-failure` | ✅ partial | ⬜ pending |
| 106-02-01 | 02 | 2 | FIX-02a | manual | Review COROUTINE-AUDIT.md | ❌ W0 | ⬜ pending |
| 106-02-02 | 02 | 2 | FIX-02b | smoke+ASAN | `ASAN_OPTIONS=detect_stack_use_after_return=1 ./build/relay_smoke_test` | ❌ W0 | ⬜ pending |
| 106-02-03 | 02 | 2 | FIX-02c | manual | Review DB-COROUTINE-FINDINGS.md | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/fixtures/` directory — binary fixture files for compound responses
- [ ] TimeRangeResponse and DelegationListResponse unit tests in test_translator.cpp
- [ ] UDS tap tool for capturing live node responses
- [ ] Smoke test program for relay E2E validation
- [ ] COROUTINE-AUDIT.md template
- [ ] DB-COROUTINE-FINDINGS.md template

*Existing infrastructure covers Catch2 framework and CMake sanitizer presets.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| All std::visit sites audited | FIX-02a | Code review — pattern correctness, not runtime behavior | Review COROUTINE-AUDIT.md for completeness |
| db/ coroutine audit documented | FIX-02c | Read-only analysis of frozen code | Review DB-COROUTINE-FINDINGS.md for coverage |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 10s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
