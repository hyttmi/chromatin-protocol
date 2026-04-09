---
phase: 100
slug: cleanup-foundation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-09
---

# Phase 100 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 3.7.1 (FetchContent) |
| **Config file** | relay/tests/CMakeLists.txt (to be created in Wave 0) |
| **Quick run command** | `cd build && ctest -R relay -j1 --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R relay -j1 --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 10 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 100-01-01 | 01 | 1 | CLEAN-01 | smoke | `test ! -d relay/` (old dir gone) | N/A | ⬜ pending |
| 100-01-02 | 01 | 1 | CLEAN-02 | smoke | `test ! -d sdk/python/` | N/A | ⬜ pending |
| 100-01-03 | 01 | 1 | CLEAN-03 | smoke | `cmake -S . -B build` succeeds | N/A | ⬜ pending |
| 100-02-01 | 02 | 2 | SESS-01 | unit | `cd build && ctest -R test_session --output-on-failure` | ❌ W0 | ⬜ pending |
| 100-02-02 | 02 | 2 | SESS-02 | unit | `cd build && ctest -R test_session --output-on-failure` | ❌ W0 | ⬜ pending |
| 100-02-03 | 02 | 2 | OPS-04 | unit | `cd build && ctest -R test_relay_config --output-on-failure` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/CMakeLists.txt` — Catch2 test executable for relay tests
- [ ] `relay/tests/test_session.cpp` — stubs for SESS-01, SESS-02
- [ ] `relay/tests/test_relay_config.cpp` — stubs for OPS-04
- [ ] `relay/CMakeLists.txt` — standalone FetchContent build (Asio, spdlog, nlohmann/json, Catch2)

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Old relay/ directory deleted | CLEAN-01 | Directory deletion verified by absence | `test ! -d relay/` after cleanup (old relay dir) |
| Old sdk/python/ directory deleted | CLEAN-02 | Directory deletion verified by absence | `test ! -d sdk/python/` after cleanup |
| Stale references removed | CLEAN-03 | Build system verification | `cmake -S . -B build` succeeds, `grep -r 'chromatindb_relay_lib' db/CMakeLists.txt` returns empty |

*Note: CLEAN-01/02/03 are verified by build success and file absence — no unit test needed.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 10s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
