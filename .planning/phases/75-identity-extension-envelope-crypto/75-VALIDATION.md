---
phase: 75
slug: identity-extension-envelope-crypto
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-01
---

# Phase 75 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | pytest (asyncio_mode=auto) |
| **Config file** | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| **Quick run command** | `cd sdk/python && python3 -m pytest tests/test_identity.py tests/test_envelope.py -x -q` |
| **Full suite command** | `cd sdk/python && python3 -m pytest -x -q` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd sdk/python && python3 -m pytest tests/test_identity.py tests/test_envelope.py -x -q`
- **After every plan wave:** Run `cd sdk/python && python3 -m pytest -x -q`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 75-01-01 | 01 | 1 | IDENT-01 | unit | `python3 -m pytest tests/test_identity.py -x -q -k "kem"` | Extend existing | ⬜ pending |
| 75-01-02 | 01 | 1 | IDENT-02 | unit | `python3 -m pytest tests/test_identity.py -x -q -k "save or load"` | Extend existing | ⬜ pending |
| 75-01-03 | 01 | 1 | IDENT-03 | unit | `python3 -m pytest tests/test_identity.py -x -q -k "kem_public"` | Extend existing | ⬜ pending |
| 75-02-01 | 02 | 1 | ENV-01 | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "encrypt"` | ❌ W0 | ⬜ pending |
| 75-02-02 | 02 | 1 | ENV-02 | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "wrap or recipient"` | ❌ W0 | ⬜ pending |
| 75-02-03 | 02 | 1 | ENV-03 | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "format or parse"` | ❌ W0 | ⬜ pending |
| 75-02-04 | 02 | 1 | ENV-04 | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "ad or tamper"` | ❌ W0 | ⬜ pending |
| 75-02-05 | 02 | 1 | ENV-05 | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "decrypt"` | ❌ W0 | ⬜ pending |
| 75-02-06 | 02 | 1 | ENV-06 | unit | `python3 -m pytest tests/test_envelope.py -x -q -k "hkdf or domain"` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `sdk/python/tests/test_envelope.py` — stubs for ENV-01 through ENV-06
- [ ] `sdk/python/tests/vectors/envelope_vectors.json` — cross-SDK test vectors
- [ ] `tools/envelope_test_vectors.py` — vector generator script

*Existing `tests/test_identity.py` covers IDENT-01 through IDENT-03 extensions.*

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
