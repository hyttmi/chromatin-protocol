---
phase: 104
slug: pub-sub-uds-resilience
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-10
---

# Phase 104 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | relay/tests/CMakeLists.txt |
| **Quick run command** | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |
| **Full suite command** | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ./build/relay/tests/chromatindb_relay_tests`
- **After every plan wave:** Run `cmake --build build && ./build/relay/tests/chromatindb_relay_tests`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 104-01-01 | 01 | 1 | MUX-03 | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_tracker]" -x` | Wave 0 | pending |
| 104-01-02 | 01 | 1 | MUX-03 | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_tracker][cap]" -x` | Wave 0 | pending |
| 104-01-03 | 01 | 1 | MUX-03 | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_tracker][cleanup]" -x` | Wave 0 | pending |
| 104-01-04 | 01 | 1 | MUX-04 | unit | `./build/relay/tests/chromatindb_relay_tests "[notification]" -x` | Wave 0 | pending |
| 104-01-05 | 01 | 1 | MUX-04 | unit | `./build/relay/tests/chromatindb_relay_tests "[broadcast]" -x` | Wave 0 | pending |
| 104-02-01 | 02 | 2 | MUX-05 | unit | `./build/relay/tests/chromatindb_relay_tests "[uds_reconnect]" -x` | Wave 0 | pending |
| 104-02-02 | 02 | 2 | MUX-06 | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_replay]" -x` | Wave 0 | pending |
| 104-02-03 | 02 | 2 | MUX-07 | unit | `./build/relay/tests/chromatindb_relay_tests "[bulk_fail]" -x` | Wave 0 | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/test_subscription_tracker.cpp` -- stubs for MUX-03 (reference counting, cap, cleanup), MUX-04 (notification routing, broadcast), MUX-05 (reconnect contract), MUX-06 (subscription replay)
- [ ] `relay/tests/test_request_router.cpp` -- stubs for MUX-07 (bulk-fail)

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| UDS reconnect under real node disconnect | MUX-05 | Requires running node process | Start node + relay, kill node, verify relay reconnects and replays subscriptions |

---

## Validation Sign-Off

- [ ] All tasks have automated verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
