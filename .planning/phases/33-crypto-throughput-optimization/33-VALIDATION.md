---
phase: 33
slug: crypto-throughput-optimization
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-17
---

# Phase 33 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 3.7.1 |
| **Config file** | db/CMakeLists.txt (catch_discover_tests) |
| **Quick run command** | `cmake --build build && ctest --test-dir build --output-on-failure` |
| **Full suite command** | `cmake --build build && ctest --test-dir build --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ctest --test-dir build --output-on-failure`
- **After every plan wave:** Run `cmake --build build && ctest --test-dir build --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 33-01-01 | 01 | 1 | PERF-04 | unit | `ctest --test-dir build -R codec --output-on-failure` | Existing (rewrite needed) | ⬜ pending |
| 33-01-02 | 01 | 1 | PERF-02 | unit | `ctest --test-dir build -R signing --output-on-failure` | ✅ existing | ⬜ pending |
| 33-02-01 | 02 | 2 | PERF-01 | unit | `ctest --test-dir build -R storage --output-on-failure` | New overload needs test | ⬜ pending |
| 33-02-02 | 02 | 2 | PERF-03 | unit | `ctest --test-dir build -R engine --output-on-failure` | ✅ existing (verify short-circuit) | ⬜ pending |
| 33-02-03 | 02 | 2 | PERF-05 | unit | `ctest --test-dir build -R storage --output-on-failure` | Covered by PERF-01 test | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements. The tests themselves need updating (new `build_signing_input()` return type), but no new test files or framework changes are needed.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
