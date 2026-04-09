---
phase: 97
slug: protocol-crypto-safety
status: draft
nyquist_compliant: true
wave_0_complete: true
created: 2026-04-08
---

# Phase 97 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (v3) |
| **Config file** | `db/CMakeLists.txt` |
| **Quick run command** | `./build/db/chromatindb_tests "[endian],[auth_helpers],[aead],[codec],[sync_protocol],[reconciliation],[connection],[handshake]" --abort` |
| **Full suite command** | `./build/db/chromatindb_tests --abort` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** `cmake --build build && ./build/db/chromatindb_tests --abort`
- **After every plan wave:** Full suite green
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 97-01-01 | 01 | 1 | PROTO-01 | unit | `./build/db/chromatindb_tests "[endian]" -c "checked_mul" --abort` | extends test_endian.cpp | pending |
| 97-01-02 | 01 | 1 | PROTO-01 | unit | `./build/db/chromatindb_tests "[sync_protocol]" -c "overflow" --abort` | extends test_sync_protocol.cpp, test_reconciliation.cpp | pending |
| 97-02-01 | 02 | 1 | PROTO-02, PROTO-03 | unit | `./build/db/chromatindb_tests "[auth_helpers]" --abort && ./build/db/chromatindb_tests "[codec]" --abort` | extends test_auth_helpers.cpp, test_codec.cpp | pending |
| 97-02-02 | 02 | 1 | PROTO-04, CRYPTO-01 | unit | `./build/db/chromatindb_tests "[aead]" --abort && ./build/db/chromatindb_tests "[connection][nonce]" --abort` | extends test_aead.cpp, test_connection.cpp | pending |
| 97-02-03 | 02 | 1 | CRYPTO-02 | unit | `./build/db/chromatindb_tests "[handshake][binding]" --abort` | extends test_handshake.cpp | pending |
| 97-03-01 | 03 | 2 | CRYPTO-03 | unit | `./build/db/chromatindb_tests "[connection][lightweight]" --abort` | extends test_connection.cpp | pending |
| 97-03-02 | 03 | 2 | CRYPTO-03 | integration | `./build/db/chromatindb_tests --abort` | extends test_connection.cpp | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

None -- all tests extend existing test files. No new test files needed.

*Existing test infrastructure covers all phase requirements:*
- `db/tests/util/test_endian.cpp` -- extend with checked_mul/checked_add tests
- `db/tests/net/test_auth_helpers.cpp` -- extend with pubkey size rejection tests
- `db/tests/wire/test_codec.cpp` -- extend with FlatBuffer pubkey validation tests
- `db/tests/crypto/test_aead.cpp` -- extend with AD bounds tests
- `db/tests/net/test_connection.cpp` -- extend with nonce exhaustion + lightweight handshake tests
- `db/tests/net/test_handshake.cpp` -- extend with pubkey binding test
- `db/tests/sync/test_sync_protocol.cpp` -- extend with overflow rejection tests
- `db/tests/sync/test_reconciliation.cpp` -- extend with overflow rejection tests

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| ASAN/TSAN/UBSAN clean | All | Requires sanitizer build | `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" .. && cmake --build . && ./chromatindb_tests --abort` |

*All other phase behaviors have automated verification.*

---

## Validation Sign-Off

- [x] All tasks have `<automated>` verify or Wave 0 dependencies
- [x] Sampling continuity: no 3 consecutive tasks without automated verify
- [x] Wave 0 covers all MISSING references
- [x] No watch-mode flags
- [x] Feedback latency < 30s
- [x] `nyquist_compliant: true` set in frontmatter

**Approval:** approved
