---
phase: 75-identity-extension-envelope-crypto
plan: 01
subsystem: crypto
tags: [ml-kem-1024, ml-dsa-87, identity, pq-crypto, liboqs, envelope-encryption]

# Dependency graph
requires:
  - phase: 70-crypto-foundation-identity
    provides: Identity class with ML-DSA-87 signing, crypto.py, exceptions.py
provides:
  - Identity with ML-KEM-1024 encryption keypair (generate, save, load, from_public_keys)
  - KEM_PUBLIC_KEY_SIZE (1568) and KEM_SECRET_KEY_SIZE (3168) constants
  - has_kem and kem_public_key properties on Identity
  - NotARecipientError and MalformedEnvelopeError exception classes
affects: [75-02-envelope-crypto, envelope-encryption, directory-service, group-encryption]

# Tech tracking
tech-stack:
  added: [oqs.KeyEncapsulation ML-KEM-1024]
  patterns: [dual-keypair identity (signing + encryption), 4-file key persistence]

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/identity.py
    - sdk/python/chromatindb/exceptions.py
    - sdk/python/chromatindb/__init__.py
    - sdk/python/tests/test_identity.py

key-decisions:
  - "Identity.generate() always produces both ML-DSA-87 + ML-KEM-1024 keypairs"
  - "save() requires KEM keypair (rejects signing-only identities)"
  - "from_public_key() unchanged (signing-verify only), from_public_keys() for dual-key"
  - "KEM secret key never exposed as property (only internal _kem for decap)"

patterns-established:
  - "Dual-keypair identity: signing (ML-DSA-87) + encryption (ML-KEM-1024) always generated together"
  - "4-file key persistence: .key/.pub (signing) + .kem/.kpub (encryption)"
  - "from_public_keys() for encrypt-capable verify-only peers"

requirements-completed: [IDENT-01, IDENT-02, IDENT-03]

# Metrics
duration: 4min
completed: 2026-04-01
---

# Phase 75 Plan 01: Identity Extension + Envelope Exceptions Summary

**ML-KEM-1024 encryption keypair added to SDK Identity with 4-file persistence, from_public_keys() constructor, and NotARecipientError/MalformedEnvelopeError exceptions**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-01T15:42:01Z
- **Completed:** 2026-04-01T15:45:45Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Identity.generate() now produces both ML-DSA-87 signing and ML-KEM-1024 encryption keypairs
- 4-file key persistence (.key/.pub/.kem/.kpub) with size validation on load
- from_public_keys(signing_pk, kem_pk) creates encrypt-capable verify-only identity for peer encryption
- NotARecipientError and MalformedEnvelopeError exception classes ready for envelope module
- 14 new KEM tests, full suite (356 tests) passes with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Add envelope exception classes and extend Identity with KEM keypair** - `b1f601a` (feat)
2. **Task 2: Test KEM keypair generation, file I/O, and constructors** - `3580236` (test)

## Files Created/Modified
- `sdk/python/chromatindb/identity.py` - Extended Identity with ML-KEM-1024 keypair, save/load 4 files, from_public_keys(), has_kem/kem_public_key properties
- `sdk/python/chromatindb/exceptions.py` - Added NotARecipientError and MalformedEnvelopeError under CryptoError
- `sdk/python/chromatindb/__init__.py` - Re-exports for new exception classes and KEM constants
- `sdk/python/tests/test_identity.py` - 14 new KEM tests (total 29 identity tests)

## Decisions Made
- Identity.generate() always produces both keypairs (no opt-out) per D-21/D-29
- save() rejects identities without KEM keypair since generate() always produces one
- from_public_key(signing_pk) unchanged for backward compatibility per D-27
- KEM secret key not exposed as property per D-28 (only internal _kem for decapsulation)

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

None - all functionality is fully wired.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Identity with dual keypairs ready for envelope encryption module (75-02)
- Exception classes ready for envelope seal/open error paths
- All existing tests pass, no regressions

## Self-Check: PASSED

- All 4 modified files exist on disk
- Both task commits (b1f601a, 3580236) found in git log
- SUMMARY.md created at expected path

---
*Phase: 75-identity-extension-envelope-crypto*
*Completed: 2026-04-01*
