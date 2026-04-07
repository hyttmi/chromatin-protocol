---
phase: 92-kem-key-versioning
plan: 02
subsystem: crypto
tags: [ml-kem-1024, key-versioning, userentry, directory, pq-crypto]

# Dependency graph
requires:
  - phase: 92-01-kem-key-versioning
    provides: Identity key_version property, rotate_kem(), KEM key ring
provides:
  - UserEntry v2 binary format with key_version:4 BE field
  - kem_sig signs (kem_pk || key_version_be) per D-05
  - decode_user_entry returns 5-tuple with key_version
  - verify_user_entry takes key_version parameter
  - DirectoryEntry.key_version field
  - _populate_cache highest-version-wins per signing key (D-07)
  - resolve_recipient() returns Identity with latest KEM pubkey
affects: [92-03-kem-key-versioning]

# Tech tracking
tech-stack:
  added: []
  patterns: [highest-version-wins-cache, key-version-bound-signature]

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/_directory.py
    - sdk/python/tests/test_directory.py

key-decisions:
  - "USERENTRY_VERSION bumped to 0x02; v1 (0x01) entries rejected outright per D-04/D-11"
  - "kem_sig signs (kem_pk || key_version_be) instead of just kem_pk -- binds version to signature"
  - "Highest-version-wins uses early-continue pattern (skip if existing >= new) rather than replace-if-higher"
  - "resolve_recipient delegates to get_user and extracts identity -- simple, cache-backed"

patterns-established:
  - "Key-version-bound signature: signed data always includes key_version_be to prevent version downgrade"
  - "Highest-version-wins cache: _populate_cache keeps only the entry with max key_version per signing key"

requirements-completed: [KEY-02]

# Metrics
duration: 4min
completed: 2026-04-07
---

# Phase 92 Plan 02: UserEntry v2 Format and Directory Key Version Support Summary

**UserEntry v2 with key_version:4 BE, version-bound kem_sig, highest-version cache, and resolve_recipient() for discovering latest KEM pubkeys**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-07T02:33:00Z
- **Completed:** 2026-04-07T02:37:34Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 2

## Accomplishments
- UserEntry format bumped to v2 (0x02) with key_version:4 BE field between name and kem_sig
- kem_sig now signs (kem_pk || key_version_be) binding key version to signature, preventing version downgrade
- decode_user_entry returns 5-tuple; old v1 (0x01) entries rejected per D-04
- DirectoryEntry gains key_version field for cache-level version tracking
- _populate_cache implements highest-version-wins: per signing key, only the entry with the greatest key_version is retained
- resolve_recipient() method on Directory resolves display name to Identity with latest KEM pubkey
- 100 directory tests pass (72 existing updated + 28 new), 49 identity tests pass, zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1 (RED): Failing tests for UserEntry v2** - `cc34f5d` (test)
2. **Task 1 (GREEN): Implement UserEntry v2 format** - `1861183` (feat)

_TDD task: RED phase wrote/updated tests for 5-tuple decode, 0x02 version, key_version in verify, highest-version cache, and resolve_recipient. GREEN phase implemented all functionality._

## Files Created/Modified
- `sdk/python/chromatindb/_directory.py` - UserEntry v2 format (0x02), key_version in encode/decode/verify, DirectoryEntry.key_version, _populate_cache highest-version-wins, resolve_recipient()
- `sdk/python/tests/test_directory.py` - Updated 72 existing tests for new format, added 28 new tests for key_version codec, version-bound signatures, highest-version cache, and resolve_recipient

## Decisions Made
- USERENTRY_VERSION bumped to 0x02; old 0x01 entries silently rejected (no migration per D-04/D-11)
- kem_sig signs (kem_pk || key_version_be) -- binding the key version into the signature prevents version downgrade attacks
- Highest-version-wins uses early-continue pattern: skip entry if existing version >= new version, rather than explicit comparison. Logically equivalent, slightly cleaner control flow.
- resolve_recipient() simply delegates to get_user() and returns the identity -- the cache already guarantees highest-version entry

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

None - all functionality is fully wired and tested.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- UserEntry v2 format and directory key version support complete for 92-03 (envelope decrypt key ring fallback)
- resolve_recipient() available for callers to discover latest KEM pubkey before encryption
- _populate_cache correctly tracks key versions, ready for multi-version scenarios

## Self-Check: PASSED

- All files exist (_directory.py, test_directory.py, SUMMARY.md)
- Both commits verified (cc34f5d RED, 1861183 GREEN)
- All acceptance criteria patterns confirmed in source

---
*Phase: 92-kem-key-versioning*
*Completed: 2026-04-07*
