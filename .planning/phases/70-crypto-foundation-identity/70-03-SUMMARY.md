---
phase: 70-crypto-foundation-identity
plan: 03
subsystem: crypto
tags: [sha3-256, hkdf-sha256, chacha20-poly1305, ml-dsa-87, flatbuffers, liboqs-python, pynacl]

# Dependency graph
requires:
  - phase: 70-01
    provides: SDK package skeleton, exceptions.py, FlatBuffers generated code, pyproject.toml
  - phase: 70-02
    provides: C++ test vector JSON (crypto_vectors.json) for cross-language validation
provides:
  - "SHA3-256, HKDF-SHA256, ChaCha20-Poly1305 primitives (crypto.py, _hkdf.py)"
  - "ML-DSA-87 identity management with key file I/O (identity.py)"
  - "FlatBuffers TransportMessage encode/decode (wire.py)"
  - "Canonical signing input matching C++ byte-for-byte (build_signing_input)"
  - "All crypto primitives validated against C++ test vectors"
affects: [71-transport-pq-handshake, 72-core-data-operations]

# Tech tracking
tech-stack:
  added: [hashlib.sha3_256, hmac+hashlib HKDF, nacl.bindings AEAD, liboqs-python ML-DSA-87]
  patterns: [pure-Python HKDF via stdlib (no libsodium HKDF binding needed), per-element FlatBuffer payload access (no numpy)]

key-files:
  created:
    - sdk/python/chromatindb/crypto.py
    - sdk/python/chromatindb/_hkdf.py
    - sdk/python/chromatindb/identity.py
    - sdk/python/chromatindb/wire.py
    - sdk/python/tests/test_crypto.py
    - sdk/python/tests/test_identity.py
    - sdk/python/tests/test_wire.py
  modified:
    - sdk/python/chromatindb/__init__.py
    - sdk/python/pyproject.toml

key-decisions:
  - "Pure-Python HKDF-SHA256 via stdlib hmac+hashlib -- no libsodium HKDF binding needed, byte-identical to C++"
  - "FlatBuffer payload decode via per-element Payload(j) loop -- avoids numpy dependency"
  - "Removed auto-generated .pyi stubs -- buggy numpy imports and invalid Literal types"
  - "mypy configured with follow_imports=skip for generated code, ignore_missing_imports for untyped C libs"

patterns-established:
  - "Pattern: Use test vectors from crypto_vectors.json for all crypto validation"
  - "Pattern: re-raise with 'from None' for clean exception chains in identity.py"
  - "Pattern: Decode FlatBuffer bytes via list comprehension not PayloadAsNumpy"

requirements-completed: [XPORT-01, XPORT-06]

# Metrics
duration: 7min
completed: 2026-03-29
---

# Phase 70 Plan 03: Crypto Primitives, Identity & Wire Format Summary

**All crypto primitives (SHA3-256, HKDF, AEAD, signing input) and ML-DSA-87 identity management validated byte-identical to C++ via test vectors, with FlatBuffers wire helpers**

## Performance

- **Duration:** 7 min
- **Started:** 2026-03-29T08:47:27Z
- **Completed:** 2026-03-29T08:55:21Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- All crypto primitives produce byte-identical output to C++ node for every test vector
- ML-DSA-87 identity with generate/load/save/sign/verify, cross-language signature verification passes
- FlatBuffers TransportMessage encode/decode roundtrips correctly without numpy dependency
- HKDF empty-salt case specifically verified (critical for PQ handshake in Phase 71)
- 66 total tests pass (28 crypto + 15 identity + 6 wire + 17 existing), ruff clean, mypy clean

## Task Commits

Each task was committed atomically:

1. **Task 1: Crypto primitives and HKDF** - `926a8c4` (test: failing) -> `80eab4a` (feat: implementation passes)
2. **Task 2: Identity management and wire format** - `91bce00` (test: failing) -> `7ca8da1` (feat: implementation passes)

_TDD: RED tests committed before GREEN implementation for both tasks_

## Files Created/Modified
- `sdk/python/chromatindb/crypto.py` - SHA3-256, AEAD encrypt/decrypt, canonical signing input, HKDF re-exports
- `sdk/python/chromatindb/_hkdf.py` - Pure-Python HKDF-SHA256 (RFC 5869) with empty-salt handling
- `sdk/python/chromatindb/identity.py` - ML-DSA-87 identity class (generate, load, save, sign, verify)
- `sdk/python/chromatindb/wire.py` - TransportMessage FlatBuffer encode/decode, TransportMsgType re-export
- `sdk/python/chromatindb/__init__.py` - Added re-exports for crypto, identity, wire modules
- `sdk/python/pyproject.toml` - Added mypy overrides for untyped libs and generated code
- `sdk/python/tests/test_crypto.py` - 28 tests: SHA3, HKDF, AEAD, signing input against C++ vectors
- `sdk/python/tests/test_identity.py` - 15 tests: keypair, file I/O, sign/verify, C++ vector cross-check
- `sdk/python/tests/test_wire.py` - 6 tests: encode/decode roundtrip, enum values, error handling

## Decisions Made
- Pure-Python HKDF-SHA256 via stdlib hmac+hashlib: no libsodium HKDF binding needed, byte-identical output confirmed against all C++ vectors including empty-salt case
- FlatBuffer payload decode via per-element `Payload(j)` loop instead of `PayloadAsNumpy()`: avoids numpy dependency while maintaining correctness
- Removed auto-generated `.pyi` stubs from FlatBuffers codegen: they had buggy numpy imports and invalid `Literal` type parameters that broke mypy
- mypy configured with `follow_imports = "skip"` for generated code and `ignore_missing_imports` for untyped C bindings (oqs, flatbuffers, nacl)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] FlatBuffer PayloadAsNumpy requires numpy**
- **Found during:** Task 2 (wire.py decode)
- **Issue:** `PayloadAsNumpy()` raises `NumpyRequiredForThisFeature` -- numpy not installed and should not be required
- **Fix:** Replaced with per-element `bytes([msg.Payload(j) for j in range(length)])` loop
- **Files modified:** sdk/python/chromatindb/wire.py
- **Verification:** test_wire.py all 6 tests pass including large payload
- **Committed in:** 7ca8da1

**2. [Rule 3 - Blocking] FlatBuffer GetRootAs doesn't catch invalid data**
- **Found during:** Task 2 (wire.py decode)
- **Issue:** `GetRootAs` succeeds on garbage bytes; error happens later during field access
- **Fix:** Wrapped entire decode (including field access) in try/except, not just `GetRootAs`
- **Files modified:** sdk/python/chromatindb/wire.py
- **Verification:** test_decode_invalid_buffer passes
- **Committed in:** 7ca8da1

**3. [Rule 1 - Bug] Auto-generated .pyi stubs break mypy**
- **Found during:** Task 2 (mypy check)
- **Issue:** FlatBuffers-generated `.pyi` files import numpy and use invalid `Literal` syntax
- **Fix:** Removed `.pyi` stubs; configured mypy to skip generated module
- **Files modified:** Deleted blob_generated.pyi and transport_generated.pyi; updated pyproject.toml
- **Verification:** `mypy chromatindb/` reports 0 errors
- **Committed in:** 7ca8da1

---

**Total deviations:** 3 auto-fixed (2 blocking, 1 bug)
**Impact on plan:** All auto-fixes necessary for correctness and toolchain compatibility. No scope creep.

## Issues Encountered
None beyond the auto-fixed deviations above.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all data paths are wired to real crypto implementations and test vectors.

## Next Phase Readiness
- All crypto primitives proven byte-identical to C++ -- Phase 71 can build PQ handshake on top
- Identity class ready for client authentication in the transport layer
- Wire format helpers ready for TransportMessage framing
- HKDF empty-salt behavior confirmed -- matches C++ for PQ key derivation

## Self-Check: PASSED

All 8 files verified present on disk. All 4 task commits verified in git log.

---
*Phase: 70-crypto-foundation-identity*
*Completed: 2026-03-29*
