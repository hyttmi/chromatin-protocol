---
phase: 72
slug: core-data-operations
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-29
---

# Phase 72 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest + pytest-asyncio |
| **Config file** | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && python3 -m pytest tests/test_types.py tests/test_codec.py -x` |
| **Full suite command** | `cd sdk/python && python3 -m pytest tests/ -x` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python3 -m pytest tests/test_types.py tests/test_codec.py tests/test_client_ops.py -x`
- **After every plan wave:** Run `cd sdk/python && python3 -m pytest tests/ -x`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 72-01-01 | 01 | 1 | DATA-01 | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_blob tests/test_codec.py::test_decode_write_ack tests/test_client_ops.py::test_write_blob -x` | No -- Wave 0 | pending |
| 72-01-02 | 01 | 1 | DATA-02 | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_read_request tests/test_codec.py::test_decode_read_response tests/test_client_ops.py::test_read_blob -x` | No -- Wave 0 | pending |
| 72-01-03 | 01 | 1 | DATA-03 | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_tombstone tests/test_codec.py::test_decode_delete_ack tests/test_client_ops.py::test_delete_blob -x` | No -- Wave 0 | pending |
| 72-01-04 | 01 | 1 | DATA-04 | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_list_request tests/test_codec.py::test_decode_list_response tests/test_client_ops.py::test_list_blobs -x` | No -- Wave 0 | pending |
| 72-01-05 | 01 | 1 | DATA-05 | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_exists_request tests/test_codec.py::test_decode_exists_response tests/test_client_ops.py::test_exists -x` | No -- Wave 0 | pending |
| 72-01-06 | 01 | 1 | DATA-06 | integration | `python3 -m pytest tests/test_integration.py::test_ping_pong -x` | Yes -- already passing | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

- [ ] `tests/test_types.py` -- covers WriteResult, ReadResult, DeleteResult, BlobRef, ListPage dataclasses
- [ ] `tests/test_codec.py` -- covers encode/decode for all 5 message payload pairs + blob FlatBuffer
- [ ] `tests/test_client_ops.py` -- covers ChromatinClient.write_blob/read_blob/delete_blob/list_blobs/exists (mock transport)
- [ ] `tests/test_integration.py` additions -- covers DATA-01 through DATA-05 against live KVM relay

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Relay forwards StorageFull/QuotaExceeded | DATA-01 | Requires filling node storage | Fill node storage, attempt write, verify ProtocolError raised |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
