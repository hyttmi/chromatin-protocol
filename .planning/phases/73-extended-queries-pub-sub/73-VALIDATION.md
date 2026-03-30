---
phase: 73
slug: extended-queries-pub-sub
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-30
---

# Phase 73 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest + pytest-asyncio |
| **Config file** | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && python -m pytest tests/ -x --ignore=tests/test_integration.py -q` |
| **Full suite command** | `cd sdk/python && python -m pytest tests/ -v` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python -m pytest tests/ -x --ignore=tests/test_integration.py -q`
- **After every plan wave:** Run `cd sdk/python && python -m pytest tests/ -v`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 73-01-01 | 01 | 1 | QUERY-01 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k metadata -x` | extend existing | ⬜ pending |
| 73-01-02 | 01 | 1 | QUERY-02 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k batch_exists -x` | extend existing | ⬜ pending |
| 73-01-03 | 01 | 1 | QUERY-03 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k batch_read -x` | extend existing | ⬜ pending |
| 73-01-04 | 01 | 1 | QUERY-04 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k time_range -x` | extend existing | ⬜ pending |
| 73-01-05 | 01 | 1 | QUERY-05 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k namespace_list -x` | extend existing | ⬜ pending |
| 73-01-06 | 01 | 1 | QUERY-06 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k namespace_stats -x` | extend existing | ⬜ pending |
| 73-01-07 | 01 | 1 | QUERY-07 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k storage_status -x` | extend existing | ⬜ pending |
| 73-01-08 | 01 | 1 | QUERY-08 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k node_info -x` | extend existing | ⬜ pending |
| 73-01-09 | 01 | 1 | QUERY-09 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k peer_info -x` | extend existing | ⬜ pending |
| 73-01-10 | 01 | 1 | QUERY-10 | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k delegation -x` | extend existing | ⬜ pending |
| 73-02-01 | 02 | 1 | PUBSUB-01 | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -k subscribe -x` | extend existing | ⬜ pending |
| 73-02-02 | 02 | 1 | PUBSUB-02 | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -k unsubscribe -x` | extend existing | ⬜ pending |
| 73-02-03 | 02 | 1 | PUBSUB-03 | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -k notification -x` | extend existing | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. Test files exist and just need extension.

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
