---
phase: 82
slug: reconcile-on-connect-safety-net
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-04
---

# Phase 82 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 |
| **Config file** | CMakeLists.txt (FetchContent) |
| **Quick run command** | `cd build && ctest -R "peer\|config" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "peer\|config" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 82-01-01 | 01 | 1 | MAINT-07 | unit | `cd build && ctest -R "config" --output-on-failure` | ✅ existing | ⬜ pending |
| 82-02-01 | 02 | 2 | MAINT-04 | unit | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |
| 82-02-02 | 02 | 2 | MAINT-05 | integration | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |
| 82-02-03 | 02 | 2 | MAINT-06 | unit | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/peer/test_peer_manager.cpp` — new test cases for cursor grace period, safety-net interval, reconnect cursor reuse
- [ ] `db/tests/config/test_config.cpp` — tests for safety_net_interval_seconds validation

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
