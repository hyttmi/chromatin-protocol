---
phase: 35
slug: namespace-quotas
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-18
---

# Phase 35 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt (BUILD_TESTING block) |
| **Quick run command** | `cd build && ctest --test-dir . -R "quota\|Quota" --output-on-failure` |
| **Full suite command** | `cd build && ctest --test-dir . --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --test-dir . -R "quota\|Quota" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --test-dir . --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 35-01-01 | 01 | 1 | QUOTA-03 | unit | `ctest -R "quota" --output-on-failure` | No -- Wave 0 | pending |
| 35-01-02 | 01 | 1 | QUOTA-04 | unit | `ctest -R "config.*quota\|quota.*config" --output-on-failure` | No -- Wave 0 | pending |
| 35-02-01 | 02 | 2 | QUOTA-01/02 | unit | `ctest -R "engine.*quota\|quota.*engine" --output-on-failure` | No -- Wave 0 | pending |
| 35-02-02 | 02 | 2 | QUOTA-04 | unit | `ctest -R "quota\|Quota" --output-on-failure` | No -- Wave 0 | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

- Existing infrastructure covers all phase requirements
- Tests added inline with implementation (existing project pattern)
- New tests go into existing files: `test_storage.cpp`, `test_engine.cpp`, `test_config.cpp`, `test_peer_manager.cpp`

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| SIGHUP reload updates quota in-flight | QUOTA-04 | Requires signal delivery | Send SIGHUP to running node, verify logs show quota reload |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
