---
phase: 93
slug: group-membership-revocation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-07
---

# Phase 93 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest 7.x |
| **Config file** | sdk/python/pyproject.toml |
| **Quick run command** | `cd sdk/python && python -m pytest tests/ -x -q --timeout=30` |
| **Full suite command** | `cd sdk/python && python -m pytest tests/ -q --timeout=60` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python -m pytest tests/ -x -q --timeout=30`
- **After every plan wave:** Run `cd sdk/python && python -m pytest tests/ -q --timeout=60`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 93-01-01 | 01 | 1 | GRP-01 | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -x -q -k remove_member` | ✅ | ⬜ pending |
| 93-01-02 | 01 | 1 | GRP-02 | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -x -q -k write_to_group` | ✅ | ⬜ pending |
| 93-02-01 | 02 | 2 | GRP-01, GRP-02 | integration | `cd tests && bash test_group_revocation.sh` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tests/test_group_revocation.sh` — Docker integration test script for group membership revocation E2E flow

*Existing unit test infrastructure covers all unit test requirements.*

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
