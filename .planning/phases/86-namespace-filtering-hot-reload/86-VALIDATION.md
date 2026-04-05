---
phase: 86
slug: namespace-filtering-hot-reload
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-05
---

# Phase 86 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt (FetchContent, catch_discover_tests) |
| **Quick run command** | `cd build && ctest -R "peer_manager\|message_filter" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~45 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "peer_manager|message_filter" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 60 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 86-01-01 | 01 | 1 | FILT-01 | unit | `cd build && ctest -R "peer_manager.*announce" --output-on-failure` | ❌ W0 | ⬜ pending |
| 86-01-02 | 01 | 1 | FILT-01 | unit | `cd build && ctest -R "message_filter" --output-on-failure` | ✅ Extend | ⬜ pending |
| 86-01-03 | 01 | 1 | FILT-02 | unit | `cd build && ctest -R "peer_manager.*filter" --output-on-failure` | ❌ W0 | ⬜ pending |
| 86-01-04 | 01 | 1 | FILT-02 | unit | `cd build && ctest -R "peer_manager.*empty" --output-on-failure` | ❌ W0 | ⬜ pending |
| 86-02-01 | 02 | 2 | OPS-01 | integration | Docker compose test script | ❌ W0 | ⬜ pending |
| 86-02-02 | 02 | 2 | OPS-01 | integration | Docker compose test script | ❌ W0 | ⬜ pending |
| 86-03-01 | 03 | 3 | FILT-01+02 | integration | Docker compose test script | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/peer/test_namespace_announce.cpp` — stubs for FILT-01, FILT-02 (unit tests for announce exchange, filtering logic)
- [ ] `tests/integration/test_filt01_namespace_filtering.sh` — Docker E2E for namespace-filtered sync
- [ ] `tests/integration/test_ops01_max_peers_sighup.sh` — Docker E2E for max_peers hot reload
- [ ] Add new test file to `db/CMakeLists.txt` test target source list

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| SIGHUP signal delivery | OPS-01 | Requires OS signal | Docker integration test sends SIGHUP via `docker kill -s HUP` |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 60s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
