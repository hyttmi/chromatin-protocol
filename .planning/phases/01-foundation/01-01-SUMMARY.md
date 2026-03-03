---
phase: 01-foundation
plan: 01
subsystem: crypto
tags: [pq-crypto, raii, tdd]
requires: []
provides: [ml-dsa-87, ml-kem-1024, sha3-256, chacha20-poly1305, hkdf-sha256, secure-bytes]
affects: [wire, identity, transport]
tech-stack:
  added: [liboqs-0.15.0, libsodium, flatbuffers, catch2, spdlog, nlohmann-json]
  patterns: [raii-wrappers, secure-memory, tdd]
key-files:
  created:
    - CMakeLists.txt
    - src/crypto/secure_bytes.h
    - src/crypto/hash.h
    - src/crypto/hash.cpp
    - src/crypto/signing.h
    - src/crypto/signing.cpp
    - src/crypto/kem.h
    - src/crypto/kem.cpp
    - src/crypto/aead.h
    - src/crypto/aead.cpp
    - src/crypto/kdf.h
    - src/crypto/kdf.cpp
    - tests/crypto/test_hash.cpp
    - tests/crypto/test_signing.cpp
    - tests/crypto/test_kem.cpp
    - tests/crypto/test_aead.cpp
    - tests/crypto/test_kdf.cpp
  modified: []
key-decisions:
  - decision: "ML-DSA-87 secret key is 4896 bytes in liboqs 0.15.0 (not 4866 per FIPS 204 spec)"
    rationale: "liboqs implementation uses expanded key format; constant updated to match runtime"
  - decision: "HKDF-SHA256 chosen over HKDF-SHA3-256 for KDF"
    rationale: "libsodium has native HKDF-SHA256 support; SHA3 variant would require hand-rolling"
  - decision: "SHA3 header requires explicit include of oqs/sha3.h (not in oqs/oqs.h)"
    rationale: "liboqs only exposes sha3_ops.h callbacks via oqs.h; direct SHA3 function needs separate include"
requirements-completed: [CRYP-01, CRYP-02, CRYP-03, CRYP-04]
duration: "~15 min"
completed: "2026-03-03"
---

# Phase 01 Plan 01: CMake Scaffold + Crypto RAII Wrappers Summary

C++20 project scaffold with all 6 dependencies via FetchContent, plus thin RAII wrappers around liboqs and libsodium for ML-DSA-87, ML-KEM-1024, SHA3-256, ChaCha20-Poly1305, and HKDF-SHA256. 37 TDD tests (107 assertions) all passing.

## Tasks Completed

| Task | Commit | Status |
|------|--------|--------|
| 1. CMake scaffold + SecureBytes + SHA3-256 | 36516be | Done |
| 2. ML-DSA-87 signing + ML-KEM-1024 KEM wrappers | 36516be | Done |
| 3. ChaCha20-Poly1305 AEAD + HKDF-SHA256 KDF wrappers | 36516be | Done |

## Deviations from Plan

- **[Rule 1 - Bug] ML-DSA-87 secret key size**: FIPS 204 spec says 4866 bytes, liboqs 0.15.0 uses 4896 bytes (expanded key format). Updated constant to match actual runtime value.
- **[Rule 3 - Blocking] SHA3 header include**: `OQS_SHA3_sha3_256` is not declared in `<oqs/oqs.h>` -- requires explicit `<oqs/sha3.h>` include. Added to hash.cpp.
- **[Rule 3 - Blocking] liboqs build include path**: liboqs copies generated headers to `${BUILD_DIR}/include/oqs/` but doesn't expose this via its CMake target. Added explicit `target_include_directories` for the oqs target in root CMakeLists.txt.

**Total deviations:** 3 auto-fixed (1 bug, 2 blocking). **Impact:** Minor -- all constants and includes now match liboqs 0.15.0 reality.

## Issues Encountered

None -- all issues resolved during execution.

## Next

Ready for Plan 01-02 (wire codec) and Plan 01-03 (config + identity) in Wave 2.
