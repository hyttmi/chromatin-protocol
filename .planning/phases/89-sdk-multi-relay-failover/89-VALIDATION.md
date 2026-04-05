---
phase: 89
slug: sdk-multi-relay-failover
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-05
---

# Phase 89 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 9.0.2 |
| **Config file** | `sdk/python/pyproject.toml` (pytest section) |
| **Quick run command** | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q` |
| **Full suite command** | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/ -x -q --ignore=sdk/python/tests/test_integration.py` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q`
- **After every plan wave:** Run `sdk/python/.venv/bin/python -m pytest sdk/python/tests/ -x -q --ignore=sdk/python/tests/test_integration.py`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 89-01-01 | 01 | 1 | SDK-01 | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "multi_relay or relay_list or initial"` | ❌ W0 | ⬜ pending |
| 89-01-02 | 01 | 1 | SDK-01 | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "current_relay"` | ❌ W0 | ⬜ pending |
| 89-01-03 | 01 | 1 | SDK-02 | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "rotation or cycle"` | ❌ W0 | ⬜ pending |
| 89-01-04 | 01 | 1 | SDK-02 | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "circuit_breaker or backoff_cycle"` | ❌ W0 | ⬜ pending |
| 89-01-05 | 01 | 1 | SDK-02 | unit | `sdk/python/.venv/bin/python -m pytest sdk/python/tests/test_reconnect.py -x -q -k "reconnect_callback_relay"` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] New test class `TestMultiRelayConnect` in `sdk/python/tests/test_reconnect.py` — stubs for SDK-01 (relay list, initial rotation, current_relay)
- [ ] New test class `TestMultiRelayReconnect` in `sdk/python/tests/test_reconnect.py` — stubs for SDK-02 (rotation on failure, circuit-breaker backoff, no inter-attempt delay)
- [ ] Updated `TestReconnectLoop`, `TestCallbacks` etc. for new connect() signature and 4-arg on_reconnect

*Existing infrastructure covers all phase requirements — no new framework install needed.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending