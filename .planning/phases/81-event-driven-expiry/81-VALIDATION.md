---
phase: 81
slug: event-driven-expiry
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-03
---

# Phase 81 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 |
| **Config file** | CMakeLists.txt (FetchContent) |
| **Quick run command** | `cd build && ctest -R "event.expiry\|earliest.expiry" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "event.expiry\|earliest.expiry\|storage" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 81-01-01 | 01 | 1 | MAINT-01 | unit | `cd build && ctest -R "storage" --output-on-failure` | ❌ W0 | ⬜ pending |
| 81-02-01 | 02 | 2 | MAINT-01 | unit | `cd build && ctest -R "event.expiry" --output-on-failure` | ❌ W0 | ⬜ pending |
| 81-02-02 | 02 | 2 | MAINT-02 | unit | `cd build && ctest -R "event.expiry" --output-on-failure` | ❌ W0 | ⬜ pending |
| 81-02-03 | 02 | 2 | MAINT-03 | unit | `cd build && ctest -R "event.expiry" --output-on-failure` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/storage/test_storage.cpp` — add `[storage][earliest-expiry]` tests for `get_earliest_expiry()`
- [ ] `db/tests/peer/test_peer_manager.cpp` or new file — event-driven expiry tests (MAINT-01, MAINT-02, MAINT-03)

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
