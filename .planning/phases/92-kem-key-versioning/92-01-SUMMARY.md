---
phase: 92-kem-key-versioning
plan: 01
subsystem: crypto
tags: [ml-kem-1024, key-rotation, key-ring, identity, pq-crypto]

# Dependency graph
requires:
  - phase: 78-envelope-encryption
    provides: ML-KEM-1024 keypair in Identity class, envelope encrypt/decrypt
provides:
  - KEM key ring in Identity with versioned keypairs
  - rotate_kem() for offline key rotation
  - Numbered file persistence (.kem.{N} / .kpub.{N})
  - _build_kem_ring_map() for O(1) envelope decrypt key lookup
  - key_version property for directory registration
  - Lazy migration of pre-rotation identities (D-03)
affects: [92-02-kem-key-versioning, 92-03-kem-key-versioning]

# Tech tracking
tech-stack:
  added: []
  patterns: [versioned-key-ring, lazy-migration, numbered-file-persistence]

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/identity.py
    - sdk/python/tests/test_identity.py

key-decisions:
  - "Key ring stored as list of (version, pk_bytes, kem_obj_or_none) tuples, sorted by version ascending"
  - "has_kem checks ring length > 0 instead of _kem_public_key is not None (behavioral equivalence)"
  - "Numbered files use pathlib suffix pattern: .kem.0, .kem.1 -- glob discovers via stem.kem.[0-9]* with isdigit() filter"

patterns-established:
  - "Versioned key ring: _kem_ring list holds all historical KEM keypairs"
  - "Lazy migration D-03: pre-rotation load() builds ring in memory, first rotate_kem() writes v0 files"
  - "Ring map pattern: _build_kem_ring_map() returns {sha3_256(pk): kem_obj} for O(1) stanza matching"

requirements-completed: [KEY-01]

# Metrics
duration: 4min
completed: 2026-04-07
---

# Phase 92 Plan 01: KEM Key Ring and Rotation Summary

**ML-KEM-1024 key ring with rotate_kem(), numbered file persistence, lazy pre-rotation migration, and pk_hash ring map for envelope decrypt**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-07T02:25:18Z
- **Completed:** 2026-04-07T02:29:14Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 2

## Accomplishments
- Identity class now supports a KEM key ring holding all historical ML-KEM-1024 keypairs
- rotate_kem(key_path) generates new keypair, retains old in ring, writes numbered files to disk
- load() discovers numbered KEM files via glob and reconstructs full ring ordered by version
- Pre-rotation identities (no numbered files) treated as version 0 with lazy migration on first rotation
- _build_kem_ring_map() provides {sha3_256(pk): kem_obj} dict for O(1) envelope stanza matching
- All 523 SDK tests pass (49 identity tests: 29 existing + 20 new, zero regressions)

## Task Commits

Each task was committed atomically:

1. **Task 1 (RED): Failing tests for key ring** - `1f6295e` (test)
2. **Task 1 (GREEN): Implement key ring, rotation, persistence** - `e4463fb` (feat)

_TDD task: RED phase wrote 20 failing tests, GREEN phase implemented all functionality._

## Files Created/Modified
- `sdk/python/chromatindb/identity.py` - Added _kem_ring, rotate_kem(), key_version, _build_kem_ring_map(), numbered file save/load, lazy migration
- `sdk/python/tests/test_identity.py` - 20 new tests covering ring basics, rotation, file persistence, lazy migration, ring map, backward compat

## Decisions Made
- Key ring internal structure: list of (version, pk_bytes, kem_obj_or_none) tuples sorted by version ascending -- simple, ordered, serializable
- has_kem property now checks ring length instead of _kem_public_key is not None -- behavioral equivalence, cleaner with ring
- File discovery uses pathlib glob with isdigit() suffix filter to avoid matching non-version files

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

None - all functionality is fully wired and tested.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Key ring and rotation foundation complete for 92-02 (UserEntry format with key_version)
- _build_kem_ring_map() ready for 92-03 (envelope decrypt key ring fallback)
- key_version property ready for directory register() to include in UserEntry

## Self-Check: PASSED

- All files exist (identity.py, test_identity.py, SUMMARY.md)
- Both commits verified (1f6295e RED, e4463fb GREEN)
- All acceptance criteria patterns confirmed in source

---
*Phase: 92-kem-key-versioning*
*Completed: 2026-04-07*
