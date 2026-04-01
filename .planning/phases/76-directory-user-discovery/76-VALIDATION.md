---
phase: 76
slug: directory-user-discovery
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-01
---

# Phase 76 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 8.x |
| **Config file** | sdk/python/pyproject.toml |
| **Quick run command** | `cd sdk/python && python3 -m pytest tests/test_directory.py -x -q` |
| **Full suite command** | `cd sdk/python && python3 -m pytest -x -q` |
| **Estimated runtime** | ~8 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python3 -m pytest tests/test_directory.py -x -q`
- **After every plan wave:** Run `cd sdk/python && python3 -m pytest -x -q`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 8 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 76-01-01 | 01 | 1 | DIR-01, DIR-02, DIR-03 | unit | `python3 -m pytest tests/test_directory.py -x -q` | ❌ W0 | pending |
| 76-01-02 | 01 | 1 | DIR-01, DIR-02, DIR-03 | unit | `python3 -m pytest tests/test_directory.py -x -q` | ❌ W0 | pending |
| 76-02-01 | 02 | 2 | DIR-04, DIR-05, DIR-06 | unit | `python3 -m pytest tests/test_directory.py -x -q` | ❌ W0 | pending |
| 76-02-02 | 02 | 2 | DIR-04, DIR-05, DIR-06 | unit+integration | `python3 -m pytest tests/test_directory.py -x -q` | ❌ W0 | pending |

*Status: pending*

---

## Wave 0 Requirements

- [ ] `sdk/python/tests/test_directory.py` — stubs for DIR-01 through DIR-06
- [ ] Existing `sdk/python/tests/conftest.py` covers shared fixtures (identity, tmp_dir)

*Existing test infrastructure (pytest, conftest.py fixtures) covers framework needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Cache invalidation via live pub/sub | DIR-06 | Requires live node with relay | Connect two clients, register user on one, verify cache clears on other |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 8s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
