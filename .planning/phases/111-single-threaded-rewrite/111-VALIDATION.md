---
phase: 111
slug: single-threaded-rewrite
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-14
---

# Phase 111 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.x |
| **Config file** | relay/tests/CMakeLists.txt |
| **Quick run command** | `cd build && ninja relay_tests && ./relay/relay_tests` |
| **Full suite command** | `cd build && ninja relay_tests && ./relay/relay_tests` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ninja relay_tests && ./relay/relay_tests`
- **After every plan wave:** Run `cd build && ninja relay_tests && ./relay/relay_tests`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 111-01-01 | 01 | 1 | CONC-01 | unit | `cd build && ninja relay_tests && ./relay/relay_tests "[thread_pool]"` | ✅ | ⬜ pending |
| 111-02-01 | 02 | 2 | CONC-02, CONC-04 | unit | `cd build && ninja relay_tests && ./relay/relay_tests` | ✅ | ⬜ pending |
| 111-02-02 | 02 | 2 | CONC-03 | grep | `grep -rn 'strand\|mutex' relay/ --include='*.h' --include='*.cpp' \| grep -v test \| grep -v RESEARCH \| wc -l` (expect 0) | ✅ | ⬜ pending |
| 111-03-01 | 03 | 3 | CONC-05 | unit | `cd build && ninja relay_tests && ./relay/relay_tests "[offload]"` | ❌ W0 | ⬜ pending |
| 111-03-02 | 03 | 3 | VER-01 | integration | `cd build && ninja relay_tests && ./relay/relay_tests` (full suite green) | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements.* Catch2 is already configured, relay_tests target exists with 222+ tests. No new test framework or fixtures needed.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Single ioc.run() thread at runtime | CONC-01 | Runtime thread count not testable in unit tests | Inspect relay_main.cpp: exactly 1 thread calling ioc.run() |

*All other phase behaviors have automated verification via grep and test suite.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
