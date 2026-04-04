---
phase: 83
slug: bidirectional-keepalive
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-04
---

# Phase 83 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 |
| **Config file** | CMakeLists.txt (FetchContent) |
| **Quick run command** | `cd build && ctest -R "keepalive\|connection" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "keepalive\|connection\|peer" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 83-01-01 | 01 | 1 | CONN-01 | unit | `cd build && ctest -R "keepalive" --output-on-failure` | ❌ W0 | ⬜ pending |
| 83-01-02 | 01 | 1 | CONN-02 | unit | `cd build && ctest -R "keepalive" --output-on-failure` | ❌ W0 | ⬜ pending |

---

## Wave 0 Requirements

- [ ] `db/tests/peer/test_peer_manager.cpp` — new test cases for keepalive Ping sends, silence disconnect

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
