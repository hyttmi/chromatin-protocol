---
phase: 77
slug: groups-encrypted-client-helpers
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-02
---

# Phase 77 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 9.0.2 |
| **Config file** | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && pytest tests/test_directory.py tests/test_client.py -x -q` |
| **Full suite command** | `cd sdk/python && pytest tests/ -x -q` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && pytest tests/test_directory.py tests/test_client.py -x -q`
- **After every plan wave:** Run `cd sdk/python && pytest tests/ -x -q`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 77-01-01 | 01 | 1 | GRP-01 | unit | `pytest tests/test_directory.py -x -q -k "create_group"` | Extend existing | ⬜ pending |
| 77-01-02 | 01 | 1 | GRP-02 | unit | `pytest tests/test_directory.py -x -q -k "add_member or remove_member"` | Extend existing | ⬜ pending |
| 77-01-03 | 01 | 1 | GRP-03 | unit | `pytest tests/test_directory.py -x -q -k "list_groups or get_group"` | Extend existing | ⬜ pending |
| 77-01-04 | 01 | 1 | GRP-04 | unit | `pytest tests/test_client.py -x -q -k "write_to_group"` | Extend existing | ⬜ pending |
| 77-02-01 | 02 | 1 | CLI-01 | unit | `pytest tests/test_client.py -x -q -k "write_encrypted"` | Extend existing | ⬜ pending |
| 77-02-02 | 02 | 1 | CLI-02 | unit | `pytest tests/test_client.py -x -q -k "read_encrypted"` | Extend existing | ⬜ pending |
| 77-02-03 | 02 | 1 | CLI-03 | unit | `pytest tests/test_client.py -x -q -k "write_to_group"` | Extend existing | ⬜ pending |
| 77-02-04 | 02 | 1 | CLI-04 | unit | `pytest tests/test_client.py -x -q -k "write_encrypted_self"` | Extend existing | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. Test files exist; tests will be appended. pytest config, conftest.py fixtures, and asyncio_mode="auto" are all in place.

---

## Manual-Only Verifications

All phase behaviors have automated verification.

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
