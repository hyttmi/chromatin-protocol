---
phase: 57
slug: client-protocol-extensions
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-23
---

# Phase 57 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt (test target: chromatindb_tests) |
| **Quick run command** | `cd build && ctest -R "protocol\|peer_manager\|engine\|storage" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "protocol\|peer_manager" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 57-01-01 | 01 | 1 | PROTO-01 | unit | `ctest -R test_protocol` | TBD | pending |
| 57-01-02 | 01 | 1 | PROTO-02 | unit | `ctest -R test_protocol` | TBD | pending |
| 57-01-03 | 01 | 1 | PROTO-03 | unit | `ctest -R test_protocol` | TBD | pending |
| 57-01-04 | 01 | 1 | PROTO-04 | unit | `ctest -R test_protocol` | TBD | pending |

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. Catch2, the test binary, and the engine/storage test fixtures are already in place.

---

## Manual-Only Verifications

All phase behaviors have automated verification.

---

## Validation Sign-Off

- [ ] All tasks have automated verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
