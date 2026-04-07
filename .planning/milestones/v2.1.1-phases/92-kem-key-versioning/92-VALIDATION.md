---
phase: 92
slug: kem-key-versioning
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-07
---

# Phase 92 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 7.x |
| **Config file** | sdk/python/pyproject.toml |
| **Quick run command** | `cd sdk/python && python -m pytest tests/ -x -q --tb=short` |
| **Full suite command** | `cd sdk/python && python -m pytest tests/ -v` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python -m pytest tests/ -x -q --tb=short`
- **After every plan wave:** Run `cd sdk/python && python -m pytest tests/ -v`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 92-01-01 | 01 | 1 | KEY-01 | unit | `cd sdk/python && python -m pytest tests/test_identity.py -x -q` | ✅ | ⬜ pending |
| 92-01-02 | 01 | 1 | KEY-01 | unit | `cd sdk/python && python -m pytest tests/test_identity.py -x -q` | ✅ | ⬜ pending |
| 92-02-01 | 02 | 1 | KEY-02 | unit | `cd sdk/python && python -m pytest tests/test_directory.py -x -q` | ✅ | ⬜ pending |
| 92-02-02 | 02 | 1 | KEY-02 | unit | `cd sdk/python && python -m pytest tests/test_directory.py -x -q` | ✅ | ⬜ pending |
| 92-03-01 | 03 | 2 | KEY-03 | unit | `cd sdk/python && python -m pytest tests/test_envelope.py -x -q` | ✅ | ⬜ pending |
| 92-03-02 | 03 | 2 | KEY-03 | integration | `cd sdk/python && python -m pytest tests/ -x -q -k "kem"` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements.*

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
