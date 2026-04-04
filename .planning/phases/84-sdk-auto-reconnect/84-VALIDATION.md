---
phase: 84
slug: sdk-auto-reconnect
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-04
---

# Phase 84 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest + pytest-asyncio |
| **Config file** | sdk/python/pyproject.toml |
| **Quick run command** | `cd sdk/python && python -m pytest tests/test_reconnect.py -v` |
| **Full suite command** | `cd sdk/python && python -m pytest tests/ -v` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python -m pytest tests/test_reconnect.py -v`
- **After every plan wave:** Run `cd sdk/python && python -m pytest tests/ -v`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 84-01-01 | 01 | 1 | CONN-03 | unit | `cd sdk/python && python -m pytest tests/test_reconnect.py -v` | ❌ W0 | ⬜ pending |
| 84-01-02 | 01 | 1 | CONN-04 | unit | `cd sdk/python && python -m pytest tests/test_reconnect.py -v` | ❌ W0 | ⬜ pending |
| 84-01-03 | 01 | 1 | CONN-05 | unit | `cd sdk/python && python -m pytest tests/test_reconnect.py -v` | ❌ W0 | ⬜ pending |

---

## Wave 0 Requirements

- [ ] `sdk/python/tests/test_reconnect.py` — new test file for reconnect behavior

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
