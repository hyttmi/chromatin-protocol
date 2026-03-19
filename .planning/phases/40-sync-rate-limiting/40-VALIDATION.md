---
phase: 40
slug: sync-rate-limiting
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-19
---

# Phase 40 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (latest via FetchContent) |
| **Config file** | db/tests/peer/test_peer_manager.cpp |
| **Quick run command** | `cd build && ctest -R test_peer_manager --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && cmake --build . && ctest -R test_peer_manager --output-on-failure`
- **After every plan wave:** Run `cd build && cmake --build . && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 40-01-01 | 01 | 1 | RATE-01 | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | ❌ W0 | ⬜ pending |
| 40-01-02 | 01 | 1 | RATE-01 | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | ❌ W0 | ⬜ pending |
| 40-01-03 | 01 | 1 | RATE-02 | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | ❌ W0 | ⬜ pending |
| 40-01-04 | 01 | 1 | RATE-02 | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | ❌ W0 | ⬜ pending |
| 40-01-05 | 01 | 1 | RATE-03 | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | ❌ W0 | ⬜ pending |
| 40-01-06 | 01 | 1 | RATE-03 | regression | `cd build && ctest -R test_peer_manager --output-on-failure` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] New test cases in `db/tests/peer/test_peer_manager.cpp` — stubs for RATE-01, RATE-02, RATE-03
- No new test files needed — extend existing test file
- No new framework or fixtures needed — reuse TempDir, make_signed_blob, etc.

*Existing infrastructure covers framework and fixture requirements.*

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
