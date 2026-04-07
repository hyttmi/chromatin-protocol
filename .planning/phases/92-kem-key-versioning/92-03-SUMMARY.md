---
phase: 92-kem-key-versioning
plan: 03
subsystem: encryption
tags: [ml-kem-1024, envelope, key-ring, rotation, decryption]

# Dependency graph
requires:
  - phase: 92-01
    provides: Identity._build_kem_ring_map(), rotate_kem(), key_version, _kem_ring
provides:
  - envelope_decrypt with key ring fallback via _build_kem_ring_map
  - Decryption of data encrypted under any historical KEM key after rotation
affects: [92-04-documentation, sdk-encryption, groups]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Key ring map pattern: build pk_hash->kem_obj dict from ring for O(1) stanza matching"
    - "Removed bisect dependency from envelope decrypt (replaced with ring map scan)"

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/_envelope.py
    - sdk/python/tests/test_envelope.py

key-decisions:
  - "Ring map scan replaces binary search: O(N) stanza scan with O(1) dict lookup per stanza, correct for multi-key rings"
  - "Two-tier guard: has_kem check for signing-only identities, ring_map emptiness check for public-only identities"

patterns-established:
  - "Key ring decrypt: _build_kem_ring_map() returns {sha3_256(pk): kem_obj} for all ring entries with secret keys"

requirements-completed: [KEY-03]

# Metrics
duration: 2min
completed: 2026-04-07
---

# Phase 92 Plan 03: Envelope Key Ring Decrypt Summary

**envelope_decrypt uses Identity key ring map for multi-key decryption after KEM rotation -- old and new keys both work**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-07T02:32:42Z
- **Completed:** 2026-04-07T02:34:42Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 2

## Accomplishments
- Replaced single-key bisect lookup with _build_kem_ring_map() ring scan in envelope_decrypt
- Removed bisect import (no longer needed anywhere in _envelope.py)
- Added 5 new tests covering key ring decrypt scenarios (52 total envelope tests)
- Full cross-module test suite green (190 tests: identity + directory + envelope)

## Task Commits

Each task was committed atomically:

1. **Task 1 RED: Failing key ring decrypt tests** - `2e3ceee` (test)
2. **Task 1 GREEN: Key ring map decrypt implementation** - `b62afe7` (feat)

## Files Created/Modified
- `sdk/python/chromatindb/_envelope.py` - Replaced bisect binary search with key ring map lookup in envelope_decrypt; removed bisect import
- `sdk/python/tests/test_envelope.py` - Added 5 tests: decrypt-after-rotation (old key), decrypt-after-rotation (new key), decrypt-after-two-rotations (all versions), non-recipient after rotation, public-only identity rejection

## Decisions Made
- Ring map scan (O(N) stanzas with O(1) dict lookup) replaces binary search -- correct for multi-key rings where the matching key may not be the latest
- Two-tier guard retained: `has_kem` for signing-only identities (no KEM at all), `not ring_map` for public-only identities (KEM pubkey but no secret)
- envelope_encrypt unchanged -- already uses `recipient.kem_public_key` which returns the latest ring key

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Envelope encryption fully supports key ring decryption
- Identity rotation + directory versioning (92-02, parallel) + envelope decrypt (this plan) complete the KEY-01/02/03 chain
- Phase 94 (documentation) can proceed to document the full key rotation workflow

## Self-Check: PASSED

- FOUND: sdk/python/chromatindb/_envelope.py
- FOUND: sdk/python/tests/test_envelope.py
- FOUND: .planning/phases/92-kem-key-versioning/92-03-SUMMARY.md
- FOUND: 2e3ceee (RED commit)
- FOUND: b62afe7 (GREEN commit)

---
*Phase: 92-kem-key-versioning*
*Completed: 2026-04-07*
