---
phase: 87
slug: wire-compression
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-05
---

# Phase 87 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest (latest) with asyncio_mode=auto |
| **Config file** | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && .venv/bin/python -m pytest tests/test_envelope.py -x` |
| **Full suite command** | `cd sdk/python && .venv/bin/python -m pytest tests/ -x` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && .venv/bin/python -m pytest tests/test_envelope.py -x`
- **After every plan wave:** Run `cd sdk/python && .venv/bin/python -m pytest tests/ -x`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 87-01-01 | 01 | 1 | COMP-01 | unit | `pytest tests/test_envelope.py::test_compress_encrypt_roundtrip -x` | ❌ W0 | ⬜ pending |
| 87-01-02 | 01 | 1 | COMP-02 | unit | `pytest tests/test_envelope.py::test_compress_threshold -x` | ❌ W0 | ⬜ pending |
| 87-01-03 | 01 | 1 | COMP-03 | unit | `pytest tests/test_envelope.py::test_decompression_bomb_rejected -x` | ❌ W0 | ⬜ pending |
| 87-01-04 | 01 | 1 | COMP-04 | unit | `pytest tests/test_envelope.py::test_decrypt_both_suites -x` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] New tests in `sdk/python/tests/test_envelope.py` — suite=0x02 roundtrip, threshold behavior, expansion fallback, bomb protection, backward compatibility
- [ ] `brotli` package installed in venv: `pip install brotli~=1.2`
- [ ] SDK venv may need recreation (broken python3.14 symlink). Fallback: `python3 -m venv sdk/python/.venv`

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
