---
phase: 88
slug: relay-resilience
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-05
---

# Phase 88 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++ unit tests) + Docker integration tests |
| **Config file** | `db/tests/CMakeLists.txt` (Catch2), `tests/docker/` (integration) |
| **Quick run command** | `cd build && ctest --test-dir db/tests -R relay -j4 --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds (unit), ~120 seconds (integration) |

---

## Sampling Rate

- **After every task commit:** Run quick relay tests
- **After every plan wave:** Run full test suite
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 88-01-01 | 01 | 1 | FILT-03 | unit | `ctest -R relay_subscription` | ❌ W0 | ⬜ pending |
| 88-01-02 | 01 | 1 | RELAY-01 | unit | `ctest -R relay_reconnect` | ❌ W0 | ⬜ pending |
| 88-01-03 | 01 | 1 | RELAY-02 | unit | `ctest -R relay_replay` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/test_relay_session.cpp` — unit tests for subscription tracking, notification filtering, reconnection state machine
- [ ] Test fixtures for creating mock connections and simulating UDS disconnect

*If none: "Existing infrastructure covers all phase requirements."*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Node restart resilience | RELAY-01 | Requires actual node process restart | Start relay+node, connect client, kill node, verify client stays connected, restart node, verify subscriptions replay |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
