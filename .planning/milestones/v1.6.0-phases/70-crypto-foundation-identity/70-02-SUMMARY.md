---
phase: 70-crypto-foundation-identity
plan: 02
subsystem: testing
tags: [sha3-256, hkdf-sha256, chacha20-poly1305, ml-dsa-87, test-vectors, json, cross-language]

# Dependency graph
requires:
  - phase: none
    provides: existing chromatindb_lib crypto and wire APIs
provides:
  - C++ test vector generator binary (chromatindb_test_vectors)
  - JSON known-answer vectors for all crypto primitives (crypto_vectors.json)
  - Cross-language verification foundation for Python SDK
affects: [70-03, 71, python-sdk-crypto]

# Tech tracking
tech-stack:
  added: []
  patterns: [test-vector-generator binary pattern linking chromatindb_lib]

key-files:
  created:
    - tools/test_vector_generator.cpp
    - sdk/python/tests/vectors/crypto_vectors.json
  modified:
    - CMakeLists.txt

key-decisions:
  - "Test vector generator links chromatindb_lib (same pattern as chromatindb_verify)"
  - "JSON output to stdout with nlohmann/json, same as verify_main.cpp"
  - "All hex encoding via chromatindb::util::to_hex() for consistency"

patterns-established:
  - "SDK test vectors: C++ generates authoritative JSON, Python validates against it"
  - "tools/ binaries link chromatindb_lib for crypto access"

requirements-completed: [XPORT-01]

# Metrics
duration: 27min
completed: 2026-03-29
---

# Phase 70 Plan 02: C++ Test Vector Generator Summary

**C++ binary producing JSON known-answer vectors for SHA3-256, HKDF-SHA256, ChaCha20-Poly1305, build_signing_input, ML-DSA-87, and namespace derivation**

## Performance

- **Duration:** 27 min
- **Started:** 2026-03-29T08:12:13Z
- **Completed:** 2026-03-29T08:39:16Z
- **Tasks:** 1
- **Files modified:** 3

## Accomplishments
- C++ test vector generator binary that calls all crypto primitives through chromatindb_lib
- JSON output with 6 categories: SHA3-256 (3 vectors), HKDF-SHA256 (3 vectors including empty-salt), ChaCha20-Poly1305 (3 vectors), build_signing_input (3 vectors), ML-DSA-87 (keypair + sign + verify), namespace derivation
- SHA3-256 of empty input matches NIST reference (a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a)
- Empty-salt HKDF test case exercises the critical SDK interop path (libsodium uses zero-filled 32-byte salt per RFC 5869)
- Vectors committed to sdk/python/tests/vectors/crypto_vectors.json for Phase 70-03 consumption

## Task Commits

Each task was committed atomically:

1. **Task 1: Create C++ test vector generator and produce JSON vectors** - `4f4f0b8` (feat)

**Plan metadata:** [pending] (docs: complete plan)

## Files Created/Modified
- `tools/test_vector_generator.cpp` - C++ binary that generates JSON crypto test vectors using chromatindb_lib
- `CMakeLists.txt` - Added chromatindb_test_vectors target linked to chromatindb_lib
- `sdk/python/tests/vectors/crypto_vectors.json` - Generated JSON test vectors for all crypto primitives

## Decisions Made
- Followed existing tool pattern from chromatindb_verify (links chromatindb_lib, uses nlohmann/json, outputs JSON to stdout)
- Used chromatindb::util::to_hex() for all hex encoding (consistency with existing codebase)
- ML-DSA-87 vectors include full keypair export (pubkey 2592 bytes, seckey 4896 bytes) for SDK key format interop testing

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- CMake FetchContent git clone failed on first attempt due to parallel agent contention on git pack files. Resolved by cleaning build directory and retrying.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- crypto_vectors.json ready for Phase 70-03 (Python crypto primitives + identity tested against C++ vectors)
- Test vector generator can be re-run anytime to regenerate vectors if C++ crypto code changes

## Self-Check: PASSED

- FOUND: tools/test_vector_generator.cpp
- FOUND: sdk/python/tests/vectors/crypto_vectors.json
- FOUND: .planning/phases/70-crypto-foundation-identity/70-02-SUMMARY.md
- FOUND: commit 4f4f0b8

---
*Phase: 70-crypto-foundation-identity*
*Completed: 2026-03-29*
