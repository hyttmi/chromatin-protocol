---
phase: 99
slug: sync-resource-concurrency-correctness
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-09
---

# Phase 99 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++20) |
| **Config file** | db/tests/CMakeLists.txt |
| **Quick run command** | `cmake --build build && cd build && ctest --output-on-failure -R "sync\|resource\|coro"` |
| **Full suite command** | `cmake --build build && cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds (targeted tests), ~120 seconds (full suite) |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ./build/db/chromatindb_tests "[sync][correctness],[resource],[coro]"`
- **After every plan wave:** Run `cmake --build build && cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green + TSAN run
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 99-01-01 | 01 | 1 | SYNC-01,SYNC-02 | unit | `ctest -R pending_fetch` | Existing | pending |
| 99-01-02 | 01 | 1 | SYNC-03 | unit | `ctest -R snapshot` | Existing | pending |
| 99-02-01 | 02 | 1 | RES-01 | unit | `ctest -R subscription` | Existing | pending |
| 99-02-02 | 02 | 1 | RES-02,RES-03,RES-04 | unit | `ctest -R resource` | Existing | pending |
| 99-03-01 | 03 | 2 | CORO-01 | tsan | `ctest -R coro` | Existing | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

*Existing test infrastructure covers all phase requirements. Catch2 + ASAN/TSAN/UBSAN already configured.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| TSAN clean run | CORO-01 | Requires TSAN build variant | Build with -fsanitize=thread, run full suite, verify 0 warnings |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
