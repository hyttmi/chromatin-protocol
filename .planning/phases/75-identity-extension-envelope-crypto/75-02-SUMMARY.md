---
phase: 75-identity-extension-envelope-crypto
plan: 02
subsystem: crypto
tags: [ml-kem-1024, envelope-encryption, kem-then-wrap, chacha20-poly1305, hkdf, aead]

# Dependency graph
requires:
  - phase: 75-identity-extension-envelope-crypto
    plan: 01
    provides: Identity with ML-KEM-1024 keypair, NotARecipientError, MalformedEnvelopeError
provides:
  - envelope_encrypt() for multi-recipient PQ envelope creation
  - envelope_decrypt() with binary search stanza lookup and KEM decapsulation
  - envelope_parse() for header metadata without decryption
  - Cross-SDK test vectors (envelope_vectors.json)
  - Frozen binary envelope format [magic:4][version:1][suite:1][count:2 BE][nonce:12][N x stanza:1648][ciphertext+tag]
affects: [76-directory-service, 77-group-encryption, client-helpers, cross-sdk-interop]

# Tech tracking
tech-stack:
  added: [oqs.KeyEncapsulation.encap_secret/decap_secret for envelope KEM ops]
  patterns: [KEM-then-Wrap envelope pattern, two-pass AD construction, sorted stanzas with binary search]

key-files:
  created:
    - sdk/python/chromatindb/_envelope.py
    - sdk/python/tests/test_envelope.py
    - sdk/python/tests/vectors/envelope_vectors.json
    - tools/envelope_test_vectors.py
  modified:
    - sdk/python/chromatindb/__init__.py

key-decisions:
  - "Two-pass AD construction: Pass 1 encapsulates and collects (pk_hash, kem_ct, kem_ss), Pass 2 wraps DEK with wrap_ad built from partial header + all pk_hash+kem_ct pairs"
  - "Zero nonce for DEK wrapping is safe because KEM shared secret (and thus HKDF-derived KEK) is unique per encapsulation"
  - "Stanzas sorted by pk_hash for O(log N) binary search during decryption (bisect.bisect_left)"
  - "Full header (including all stanzas) used as AEAD AD for data encryption, preventing stanza substitution attacks"

patterns-established:
  - "Envelope binary format v1: CENV magic, version 0x01, suite 0x01, sorted stanzas"
  - "KEM-then-Wrap: random DEK encrypted once, per-recipient KEM encap -> HKDF KEK -> AEAD-wrap DEK"
  - "Cross-SDK test vector generator pattern: Python script generates JSON with hex-encoded keys and envelopes"

requirements-completed: [ENV-01, ENV-02, ENV-03, ENV-04, ENV-05, ENV-06]

# Metrics
duration: 4min
completed: 2026-04-01
---

# Phase 75 Plan 02: Envelope Encryption Summary

**PQ multi-recipient envelope encryption with KEM-then-Wrap pattern, sorted stanzas, AEAD AD binding, and 31 comprehensive tests**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-01T15:49:33Z
- **Completed:** 2026-04-01T15:54:24Z
- **Tasks:** 2
- **Files created:** 4
- **Files modified:** 1

## Accomplishments
- envelope_encrypt() creates versioned binary envelopes with sorted per-recipient stanzas using KEM-then-Wrap
- envelope_decrypt() finds stanza by O(log N) binary search, KEM decapsulates, HKDF derives KEK, AEAD unwraps DEK, decrypts payload
- envelope_parse() extracts metadata (version, suite, recipient_count) without decryption
- Sender auto-included as recipient (cannot lock yourself out), dedup by pk_hash
- Full header as AEAD AD prevents stanza substitution attacks across envelopes
- 31 tests covering roundtrip, multi-recipient, format byte-level validation, error handling, stanza substitution attack, cross-SDK vectors
- Test vector generator produces reproducible JSON vectors for cross-SDK interop validation

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement _envelope.py with envelope_encrypt, envelope_decrypt, and envelope_parse** - `dacbf5b` (feat)
2. **Task 2: Comprehensive envelope tests and cross-SDK test vector generator** - `f736ac4` (test)

## Files Created/Modified
- `sdk/python/chromatindb/_envelope.py` - PQ envelope encrypt/decrypt/parse with KEM-then-Wrap pattern (284 lines)
- `sdk/python/chromatindb/__init__.py` - Re-exports envelope_encrypt, envelope_decrypt, envelope_parse
- `sdk/python/tests/test_envelope.py` - 31 test functions covering all envelope behaviors
- `sdk/python/tests/vectors/envelope_vectors.json` - 3 cross-SDK test vectors with hex key material
- `tools/envelope_test_vectors.py` - Python script to regenerate envelope test vectors

## Decisions Made
- Two-pass AD construction: first pass collects KEM encapsulation results, second pass wraps DEK with AD built from partial header + all (pk_hash + kem_ct) pairs -- ensures wrap AD is identical for all recipients
- Zero nonce safe for DEK wrapping because each KEM encapsulation produces a unique shared secret, so each HKDF-derived KEK is unique and never reused
- Stanzas sorted by pk_hash (ascending bytes) enabling O(log N) binary search via bisect.bisect_left during decryption
- Full header (fixed header + all stanzas) used as AEAD AD for data encryption -- any modification to any stanza causes data decryption failure

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed test_decrypt_tampered_header byte offset**
- **Found during:** Task 2 (test execution)
- **Issue:** Test flipped a bit in the pk_hash area (offset 25) which corrupted stanza lookup rather than causing AEAD failure, raising NotARecipientError instead of DecryptionError
- **Fix:** Changed tamper target to wrapped_dek area (offset 1621) which correctly triggers AEAD authentication failure
- **Files modified:** sdk/python/tests/test_envelope.py
- **Verification:** Test now correctly validates that header tampering causes DecryptionError
- **Committed in:** f736ac4 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix)
**Impact on plan:** Minor test offset correction. No scope creep.

## Known Stubs

None - all functionality is fully wired.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Envelope format frozen -- directory service (Phase 76) and group encryption (Phase 77) can build on it
- Identity with dual keypairs + envelope encrypt/decrypt ready for higher-level APIs
- Full test suite green (411 tests, zero regressions)
- Cross-SDK test vectors available for future C++/Rust/JS SDK validation

## Self-Check: PASSED

- All 5 files exist on disk
- Both task commits (dacbf5b, f736ac4) found in git log
- SUMMARY.md created at expected path

---
*Phase: 75-identity-extension-envelope-crypto*
*Completed: 2026-04-01*
