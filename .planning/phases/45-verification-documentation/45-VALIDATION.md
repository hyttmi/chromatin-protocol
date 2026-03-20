---
phase: 45
slug: verification-documentation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-20
---

# Phase 45 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 (latest via FetchContent) + bash (Docker scripts) |
| **Config file** | CMakeLists.txt (test targets defined inline) |
| **Quick run command** | `cd build && ctest -R "delegation.*quota" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~45 seconds (unit tests) + ~120 seconds (crash test script) |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --output-on-failure`
- **After every plan wave:** Run full suite + `bash deploy/test-crash-recovery.sh`
- **Before `/gsd:verify-work`:** Full suite must be green + crash test passes
- **Max feedback latency:** 45 seconds (unit tests), 120 seconds (crash test)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 45-01-01 | 01 | 1 | STOR-04 | Docker integration | `bash deploy/test-crash-recovery.sh` | ❌ W0 | ⬜ pending |
| 45-01-02 | 01 | 1 | STOR-05 | unit | `cd build && ctest -R "delegation.*quota" --output-on-failure` | ❌ W0 | ⬜ pending |
| 45-02-01 | 02 | 1 | DOCS-01 | manual review | N/A (prose) | manual-only | ⬜ pending |
| 45-02-02 | 02 | 1 | DOCS-02 | manual review | N/A (prose) | manual-only | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `deploy/test-crash-recovery.sh` — covers STOR-04 crash recovery verification
- [ ] New delegation quota test cases in `db/tests/engine/test_engine.cpp` — covers STOR-05

*Existing infrastructure covers Catch2 framework and Docker setup.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| README accuracy | DOCS-01 | Prose content correctness requires human review | Verify all v0.9.0 config fields documented, features described, scenarios added |
| Protocol doc accuracy | DOCS-02 | Wire protocol documentation correctness requires human review | Verify SyncRejected(30) in table, rate limiting + inactivity sections present |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
