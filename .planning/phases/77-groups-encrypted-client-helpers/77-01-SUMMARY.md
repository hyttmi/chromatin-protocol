---
phase: 77-groups-encrypted-client-helpers
plan: 01
subsystem: sdk
tags: [python, groups, directory, binary-codec, frozen-dataclass]

# Dependency graph
requires:
  - phase: 76-directory-user-discovery
    provides: Directory class, UserEntry codec, DirectoryEntry, cache/invalidation pattern
provides:
  - GroupEntry frozen dataclass with name, members, blob_hash, timestamp fields
  - GRPE binary codec (encode_group_entry/decode_group_entry)
  - Directory.create_group, add_member, remove_member, list_groups, get_group methods
  - Extended _populate_cache for GRPE blobs with latest-timestamp-wins
affects: [77-02, encrypted-client-helpers, write_to_group]

# Tech tracking
tech-stack:
  added: []
  patterns: [GRPE binary codec, latest-timestamp-wins group resolution, admin-gated mutation]

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/_directory.py
    - sdk/python/tests/test_directory.py

key-decisions:
  - "GroupEntry placed in _directory.py alongside DirectoryEntry -- same module, same pattern"
  - "GRPE codec follows UENT pattern: magic + version + length-prefixed fields + big-endian integers"
  - "Members stored as SHA3-256(signing_pk) hashes (32 bytes each) for compactness"
  - "latest-timestamp-wins for group updates -- no tombstone dance needed"
  - "_populate_cache extended with fallthrough logic: try UENT first, then GRPE, skip unknown"

patterns-established:
  - "GRPE binary format: [GRPE:4][ver:1][name_len:2 BE][name:N][member_count:2 BE][N x hash:32]"
  - "Admin-gated group mutation: all write operations check self._is_admin before proceeding"
  - "Read-modify-write pattern for add_member/remove_member: get_group -> modify -> encode -> write_blob"

requirements-completed: [GRP-01, GRP-02, GRP-03, GRP-04]

# Metrics
duration: 6min
completed: 2026-04-02
---

# Phase 77 Plan 01: Group Management Summary

**Named group CRUD with GRPE binary codec, 5 Directory methods, and cache extension with latest-timestamp-wins resolution**

## Performance

- **Duration:** 6 min
- **Started:** 2026-04-02T02:39:50Z
- **Completed:** 2026-04-02T02:45:28Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- GRPE binary codec (encode_group_entry/decode_group_entry) with safe parsing and validation
- GroupEntry frozen dataclass with name, members (32-byte hashes), blob_hash, timestamp
- 5 Directory group methods: create_group, add_member, remove_member, list_groups, get_group
- Extended _populate_cache to decode GRPE blobs alongside UENT, with latest-timestamp-wins
- 35 new tests (17 codec + 18 directory group methods), all 81 tests pass

## Task Commits

Each task was committed atomically:

1. **Task 1: GroupEntry dataclass, GRPE codec, Directory group methods** - `9fd66b2` (test: RED), `dbf9b0a` (feat: GREEN)
2. **Task 2: Group codec and Directory group method unit tests** - `923f81b` (test)

## Files Created/Modified
- `sdk/python/chromatindb/_directory.py` - Added GroupEntry dataclass, GRPE constants, encode/decode_group_entry, 5 Directory methods, extended _populate_cache and refresh
- `sdk/python/tests/test_directory.py` - Added TestGroupEntryCodec (17 tests) and TestDirectoryGroups (18 tests)

## Decisions Made
- GroupEntry placed in _directory.py alongside DirectoryEntry (same module, same pattern)
- GRPE codec follows the UENT pattern: magic + version + length-prefixed UTF-8 name + big-endian member count + 32-byte member hashes
- _populate_cache uses fallthrough: try decode_user_entry first, if None try decode_group_entry, if None skip -- single namespace scan handles both types

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- GroupEntry and Directory group methods ready for Plan 02 (write_to_group, write_encrypted, read_encrypted)
- write_to_group can resolve group name to member KEM pubkeys via directory.get_group() + directory.get_user_by_pubkey()
- No blockers

## Self-Check: PASSED

- FOUND: sdk/python/chromatindb/_directory.py
- FOUND: sdk/python/tests/test_directory.py
- FOUND: .planning/phases/77-groups-encrypted-client-helpers/77-01-SUMMARY.md
- FOUND: commit 9fd66b2 (test RED)
- FOUND: commit dbf9b0a (feat GREEN)
- FOUND: commit 923f81b (test comprehensive)

---
*Phase: 77-groups-encrypted-client-helpers*
*Completed: 2026-04-02*
