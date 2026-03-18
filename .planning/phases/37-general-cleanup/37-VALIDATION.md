---
phase: 37
slug: general-cleanup
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-18
---

# Phase 37 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt |
| **Quick run command** | `cd build && ctest --output-on-failure -j4` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && cmake .. && cmake --build . && ctest --output-on-failure`
- **After every plan wave:** Run `cd build && cmake .. && cmake --build . && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green + Docker build succeeds
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 37-01-01 | 01 | 1 | CLEAN-02 | build | `cmake --build build 2>&1 \| grep -c chromatindb_bench` (expect 0) | N/A | ⬜ pending |
| 37-01-02 | 01 | 1 | CLEAN-04 | build | `docker build -t chromatindb-test .` | N/A | ⬜ pending |
| 37-02-01 | 02 | 1 | CLEAN-03 | manual | Review README for completeness | N/A | ⬜ pending |
| 37-02-02 | 02 | 1 | CLEAN-03 | manual | Review PROTOCOL.md for accuracy | N/A | ⬜ pending |
| 37-03-01 | 03 | 1 | CLEAN-04 | build+test | `cd build && cmake .. && cmake --build . && ctest --output-on-failure` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. This phase adds no new functionality, so no new tests are needed. Regressions from accidental code deletion are caught by the existing 366 tests.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| README feature completeness | CLEAN-03 | Documentation content review | Verify all v0.7.0 features listed, no stale references |
| PROTOCOL.md accuracy | CLEAN-04 | Documentation content review | Verify message type count matches transport.fbs, all types documented |
| Config field documentation | CLEAN-03 | Documentation content review | Verify all config.h fields have README descriptions |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
