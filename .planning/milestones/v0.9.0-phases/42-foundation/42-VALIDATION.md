---
phase: 42
slug: foundation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-19
---

# Phase 42 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt (lines 196-238) |
| **Quick run command** | `cd build && ctest --test-dir db -R "config\|version\|daemon" --output-on-failure` |
| **Full suite command** | `cd build && ctest --test-dir db --output-on-failure` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --test-dir db -R "config|version|daemon" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --test-dir db --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 42-01-01 | 01 | 1 | OPS-01 | unit | `cd build && ctest --test-dir db -R "version" --output-on-failure` | No -- Wave 0 | ⬜ pending |
| 42-02-01 | 02 | 1 | OPS-02 | unit | `cd build && ctest --test-dir db -R "config" --output-on-failure` | Partial | ⬜ pending |
| 42-02-02 | 02 | 1 | OPS-02 | unit | `cd build && ctest --test-dir db -R "config" --output-on-failure` | No -- Wave 0 | ⬜ pending |
| 42-03-01 | 03 | 1 | OPS-06 | integration | `cd build && ctest --test-dir db -R "daemon" --output-on-failure` | Partial | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] Extend `db/tests/config/test_config.cpp` — add range validation and type mismatch tests for OPS-02
- [ ] Add version test (in test_config.cpp or new file) — covers OPS-01 (version macro defined, non-empty)

*OPS-06 is a pure refactor — existing E2E tests in test_daemon.cpp (start/stop/sync) cover timer cancel behavior implicitly. No new test file needed.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Version string in startup log | OPS-01 | Log output format | Run node, check first log line contains "v0.9.0" |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
