---
phase: 44
slug: network-resilience
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-20
---

# Phase 44 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt (FetchContent) |
| **Quick run command** | `cd build && ctest --test-dir . -R "server\|peer_manager\|config" --output-on-failure` |
| **Full suite command** | `cd build && ctest --test-dir . --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && cmake --build . && ctest --test-dir . -R "server\|peer_manager\|config" --output-on-failure`
- **After every plan wave:** Run `cd build && cmake --build . && ctest --test-dir . --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 44-01-xx | 01 | 1 | CONN-01 | unit | `ctest -R "test_server" --output-on-failure` | Exists (needs new cases) | ⬜ pending |
| 44-01-xx | 01 | 1 | CONN-02 | unit | `ctest -R "test_server\|test_peer_manager" --output-on-failure` | Exists (needs new cases) | ⬜ pending |
| 44-02-xx | 02 | 1 | CONN-03 | unit | `ctest -R "test_peer_manager" --output-on-failure` | Exists (needs new cases) | ⬜ pending |
| 44-02-xx | 02 | 1 | CONN-03 | unit | `ctest -R "test_config" --output-on-failure` | Exists (needs new cases) | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. Tests for CONN-01/02/03 will be added alongside implementation as new TEST_CASE sections in existing test files:
- `db/tests/net/test_server.cpp` -- reconnect jitter, ACL suppression (Server layer)
- `db/tests/peer/test_peer_manager.cpp` -- inactivity timeout, ACL signal (PeerManager layer)
- `db/tests/config/test_config.cpp` -- inactivity_timeout_seconds validation

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Reconnect under real network partition | CONN-01 | Requires Docker multi-node topology | Docker Compose: disconnect node, verify reconnect in logs |
| ACL suppression over extended time | CONN-02 | 600s backoff requires long-running test | Docker: set ACL rejection, verify 600s backoff in logs, SIGHUP reset |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
