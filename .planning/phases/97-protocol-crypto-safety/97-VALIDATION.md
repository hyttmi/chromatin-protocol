---
phase: 97
slug: protocol-crypto-safety
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-08
---

# Phase 97 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (v3) |
| **Config file** | `db/tests/CMakeLists.txt` |
| **Quick run command** | `cd build && ctest --output-on-failure -R "test_" -j4` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --output-on-failure -R "test_" -j4`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 97-01-01 | 01 | 1 | PROTO-01 | unit | `ctest -R test_checked_arithmetic` | ❌ W0 | ⬜ pending |
| 97-01-02 | 01 | 1 | PROTO-02 | unit | `ctest -R test_auth_helpers` | ✅ | ⬜ pending |
| 97-02-01 | 02 | 1 | PROTO-03 | unit | `ctest -R test_nonce_exhaustion` | ❌ W0 | ⬜ pending |
| 97-02-02 | 02 | 1 | PROTO-04 | unit | `ctest -R test_protocol_decode` | ❌ W0 | ⬜ pending |
| 97-03-01 | 03 | 2 | CRYPTO-01 | unit | `ctest -R test_handshake` | ✅ | ⬜ pending |
| 97-03-02 | 03 | 2 | CRYPTO-02 | unit | `ctest -R test_handshake` | ✅ | ⬜ pending |
| 97-03-03 | 03 | 2 | CRYPTO-03 | unit | `ctest -R test_lightweight_handshake` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/test_checked_arithmetic.cpp` — tests for overflow-checked arithmetic helpers (PROTO-01)
- [ ] `db/tests/test_nonce_exhaustion.cpp` — tests for AEAD nonce limit enforcement (PROTO-03)
- [ ] `db/tests/test_protocol_decode.cpp` — tests for protocol decode validation (PROTO-04)
- [ ] `db/tests/test_lightweight_handshake.cpp` — tests for lightweight handshake identity binding (CRYPTO-03)

*Existing infrastructure covers auth_helpers and handshake tests.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| ASAN/TSAN/UBSAN clean | All | Requires sanitizer build | `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" .. && cmake --build . && ctest` |

*All other phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
