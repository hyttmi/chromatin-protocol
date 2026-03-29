---
phase: 70
slug: crypto-foundation-identity
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-29
---

# Phase 70 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 8.x |
| **Config file** | sdk/python/pyproject.toml (or "none — Wave 0 installs") |
| **Quick run command** | `cd sdk/python && python -m pytest tests/ -x -q` |
| **Full suite command** | `cd sdk/python && python -m pytest tests/ -v` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python -m pytest tests/ -x -q`
- **After every plan wave:** Run `cd sdk/python && python -m pytest tests/ -v`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 70-01-01 | 01 | 1 | PKG-01 | unit | `python -m pytest tests/ -x -q` | ❌ W0 | ⬜ pending |
| 70-02-01 | 02 | 1 | XPORT-01 | unit | `python -m pytest tests/test_crypto.py -v` | ❌ W0 | ⬜ pending |
| 70-03-01 | 03 | 1 | XPORT-06 | unit | `python -m pytest tests/test_identity.py -v` | ❌ W0 | ⬜ pending |
| 70-04-01 | 04 | 2 | PKG-03 | unit | `python -m pytest tests/test_vectors.py -v` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `sdk/python/tests/conftest.py` — shared fixtures
- [ ] `sdk/python/tests/test_crypto.py` — stubs for XPORT-01 crypto primitives
- [ ] `sdk/python/tests/test_identity.py` — stubs for XPORT-06 identity management
- [ ] `sdk/python/tests/test_vectors.py` — stubs for PKG-03 cross-language vectors
- [ ] pytest + dependencies installed in venv

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Key interop with C++ node | XPORT-01 | Requires running C++ node binary | Generate keypair with SDK, load with C++ node; reverse direction |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
