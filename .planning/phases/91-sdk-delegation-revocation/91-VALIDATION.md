---
phase: 91
slug: sdk-delegation-revocation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-06
---

# Phase 91 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 9.0.2 + pytest-asyncio |
| **Config file** | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py -x -v` |
| **Full suite command** | `cd sdk/python && .venv/bin/python -m pytest tests/ -x -v --ignore=tests/test_integration.py` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py -x -v`
- **After every plan wave:** Run `cd sdk/python && .venv/bin/python -m pytest tests/ -x -v --ignore=tests/test_integration.py`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 91-01-01 | 01 | 1 | REV-01 | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestRevokeDelegation -x` | ❌ W0 | ⬜ pending |
| 91-01-02 | 01 | 1 | REV-01 | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestListDelegates -x` | ❌ W0 | ⬜ pending |
| 91-01-03 | 01 | 1 | REV-01 | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestRevokeDelegation::test_revoke_not_found -x` | ❌ W0 | ⬜ pending |
| 91-01-04 | 01 | 1 | REV-01 | unit | `cd sdk/python && .venv/bin/python -m pytest tests/test_directory.py::TestRevokeDelegation::test_revoke_non_admin_raises -x` | ❌ W0 | ⬜ pending |
| 91-02-01 | 02 | 2 | REV-02 | integration | `cd sdk/python && .venv/bin/python -m pytest tests/test_integration.py::test_delegation_revocation_propagation -x -v -m integration` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tests/test_directory.py::TestRevokeDelegation` — stubs for REV-01 revocation success, not-found, non-admin
- [ ] `tests/test_directory.py::TestListDelegates` — stubs for REV-01 list delegates success, non-admin, empty list
- [ ] `tests/test_integration.py::test_delegation_revocation_propagation` — stub for REV-02 multi-node propagation
- [ ] `make_mock_client()` update — add `delegation_list = AsyncMock()` and `delete_blob = AsyncMock()`

*Existing infrastructure covers framework and fixtures — only new test classes needed.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| KVM swarm node deployment | REV-02 | Nodes must be running latest binaries before integration test | SSH to 192.168.1.200-202, verify chromatindb processes running |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
