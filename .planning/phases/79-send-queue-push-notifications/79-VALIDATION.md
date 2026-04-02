---
phase: 79
slug: send-queue-push-notifications
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-02
---

# Phase 79 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 |
| **Config file** | db/CMakeLists.txt (catch_discover_tests) |
| **Quick run command** | `cd build && ctest -R "peer\|connection\|message_filter\|codec" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "peer\|connection\|message_filter\|codec" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 79-01-01 | 01 | 1 | PUSH-04 | unit | `cd build && ctest -R "connection" --output-on-failure` | ❌ W0 | ⬜ pending |
| 79-01-02 | 01 | 1 | PUSH-04 | unit | `cd build && ctest -R "connection" --output-on-failure` | ❌ W0 | ⬜ pending |
| 79-02-01 | 02 | 1 | WIRE-01 | unit | `cd build && ctest -R "codec" --output-on-failure` | ❌ W0 | ⬜ pending |
| 79-02-02 | 02 | 1 | WIRE-04 | unit | `cd build && ctest -R "message_filter" --output-on-failure` | ✅ needs new case | ⬜ pending |
| 79-03-01 | 03 | 2 | PUSH-01 | unit | `cd build && ctest -R "peer" --output-on-failure` | ✅ needs new cases | ⬜ pending |
| 79-03-02 | 03 | 2 | PUSH-02 | unit | `cd build && ctest -R "peer" --output-on-failure` | ✅ encode_notification tested | ⬜ pending |
| 79-03-03 | 03 | 2 | PUSH-07 | unit | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |
| 79-03-04 | 03 | 2 | PUSH-03 | unit | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |
| 79-03-05 | 03 | 2 | PUSH-08 | unit | `cd build && ctest -R "peer" --output-on-failure` | ✅ implicit via peers_ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/net/test_connection.cpp` — new test cases for send queue serialization, queue full disconnect, drain on close
- [ ] `db/tests/peer/test_peer_manager.cpp` — new test cases for BlobNotify fan-out, source exclusion, storm suppression, unified callback
- [ ] `db/tests/relay/test_message_filter.cpp` — new test case for BlobNotify type 59 blocked

*Existing infrastructure covers framework and fixtures; only new test cases needed.*

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
