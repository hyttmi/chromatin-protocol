---
phase: 71
slug: transport-pq-handshake
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-29
---

# Phase 71 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 9.0.2 |
| **Config file** | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && .venv/bin/python3 -m pytest tests/ -x -q` |
| **Full suite command** | `cd sdk/python && .venv/bin/python3 -m pytest tests/ -v` |
| **Estimated runtime** | ~5 seconds (unit), ~15 seconds (with integration) |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && .venv/bin/python3 -m pytest tests/ -x -q`
- **After every plan wave:** Run `cd sdk/python && .venv/bin/python3 -m pytest tests/ -v`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 71-01-01 | 01 | 1 | XPORT-04, XPORT-05 | unit | `pytest tests/test_framing.py -x` | ❌ W0 | ⬜ pending |
| 71-02-01 | 02 | 1 | XPORT-02, XPORT-03 | unit | `pytest tests/test_handshake.py -x` | ❌ W0 | ⬜ pending |
| 71-03-01 | 03 | 2 | XPORT-07 | unit | `pytest tests/test_client.py -x` | ❌ W0 | ⬜ pending |
| 71-04-01 | 04 | 3 | XPORT-02, XPORT-03, XPORT-04, XPORT-05, XPORT-07 | integration | `pytest tests/test_integration.py -x` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tests/test_framing.py` — stubs for XPORT-04, XPORT-05 (nonce construction, frame encode/decode, encrypt/decrypt roundtrip)
- [ ] `tests/test_handshake.py` — stubs for XPORT-02, XPORT-03 (KEM exchange, session key derivation, auth payload encode/decode)
- [ ] `tests/test_client.py` — stubs for XPORT-07 (context manager, Goodbye, disconnection handling)
- [ ] `tests/test_integration.py` — stubs for all XPORT reqs end-to-end against live relay
- [ ] Update `tests/conftest.py` — add Identity fixture, relay address fixture, integration marker
- [ ] Update `pyproject.toml` — add `markers = ["integration: requires live relay"]` to pytest config

*If none: "Existing infrastructure covers all phase requirements."*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Live relay handshake | XPORT-02, XPORT-03 | Requires KVM swarm running | Start relay on 192.168.1.200:4433, run `pytest tests/test_integration.py -v` |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
