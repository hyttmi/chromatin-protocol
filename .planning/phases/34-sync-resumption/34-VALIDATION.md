---
phase: 34
slug: sync-resumption
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-17
---

# Phase 34 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (latest via FetchContent) |
| **Config file** | db/CMakeLists.txt (BUILD_TESTING block) |
| **Quick run command** | `cd build && ctest -R "sync\|storage" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "sync\|storage" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 34-01-01 | 01 | 1 | SYNC-01 | unit | `cd build && ctest -R test_storage --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-01-02 | 01 | 1 | SYNC-01 | unit | `cd build && ctest -R test_storage --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-01-03 | 01 | 1 | SYNC-03 | unit | `cd build && ctest -R test_storage --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-02-01 | 02 | 1 | SYNC-02 | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-02-02 | 02 | 1 | SYNC-02 | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-02-03 | 02 | 1 | SYNC-04 | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-02-04 | 02 | 1 | SYNC-04 | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | ❌ W0 | ⬜ pending |
| 34-02-05 | 02 | 1 | SYNC-04 | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/storage/test_storage.cpp` — new test cases for cursor CRUD, persistence, cleanup scan
- [ ] `db/tests/sync/test_sync_protocol.cpp` — new test cases for cursor-based sync, skip logic, full resync triggers
- [ ] `db/tests/storage/test_storage.cpp` — `get_hashes_since_seq` method tests

*Existing test infrastructure (Catch2, CMake BUILD_TESTING) covers framework needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| SIGHUP resets round counters | SYNC-04 | Signal handling requires running process | Start node, send SIGHUP, verify next sync is full via logs |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
