---
phase: 91-sdk-delegation-revocation
plan: 01
subsystem: sdk
tags: [python, directory, delegation, revocation, acl]

# Dependency graph
requires:
  - phase: 78-client-encryption
    provides: Directory class with delegate() method and DelegationEntry/DelegationList types
provides:
  - revoke_delegation() method on Directory class
  - list_delegates() method on Directory class
  - DelegationNotFoundError exception class
  - 8 unit tests for revocation and delegate listing
affects: [91-02, sdk-docs, protocol-docs]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Admin-guard pattern: raise DirectoryError before any async work"
    - "pk_hash lookup via delegation_list then delete_blob(delegation_blob_hash)"

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/exceptions.py
    - sdk/python/chromatindb/_directory.py
    - sdk/python/chromatindb/__init__.py
    - sdk/python/tests/test_directory.py

key-decisions:
  - "revoke_delegation uses delegation_list + delete_blob (not direct tombstone write) for correctness"
  - "DelegationNotFoundError subclasses DirectoryError (not ProtocolError) since it is a directory-level semantic error"

patterns-established:
  - "Delegation revocation: sha3_256(pubkey) lookup in delegation_list, tombstone via delete_blob(delegation_blob_hash)"

requirements-completed: [REV-01]

# Metrics
duration: 2min
completed: 2026-04-06
---

# Phase 91 Plan 01: Delegation Revocation & Listing Summary

**Directory.revoke_delegation() and list_delegates() with admin guards, pk_hash lookup, and DelegationNotFoundError exception**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-06T07:51:48Z
- **Completed:** 2026-04-06T07:53:58Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- DelegationNotFoundError exception class added as DirectoryError subclass
- Directory.revoke_delegation() implements sha3_256(pubkey) -> delegation_list lookup -> delete_blob(delegation_blob_hash) flow with admin guard
- Directory.list_delegates() wraps client.delegation_list(directory_namespace) with admin guard
- 8 new unit tests covering success, error, guard, multi-delegate, and edge cases
- All 518 non-integration tests pass (89 in test_directory.py: 81 existing + 8 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement DelegationNotFoundError, revoke_delegation(), and list_delegates()** - `786cc63` (feat)
2. **Task 2: Unit tests for revoke_delegation() and list_delegates()** - `54473d7` (test)

## Files Created/Modified
- `sdk/python/chromatindb/exceptions.py` - Added DelegationNotFoundError(DirectoryError)
- `sdk/python/chromatindb/_directory.py` - Added revoke_delegation() and list_delegates() methods with DelegationNotFoundError/DelegationEntry/DeleteResult imports
- `sdk/python/chromatindb/__init__.py` - Export DelegationNotFoundError in import block and __all__
- `sdk/python/tests/test_directory.py` - TestRevokeDelegation (5 tests) and TestListDelegates (3 tests), updated make_mock_client with delegation_list/delete_blob AsyncMock

## Decisions Made
- revoke_delegation uses delegation_list + delete_blob (not direct tombstone write) for correctness -- matches the proven node-side pattern
- DelegationNotFoundError subclasses DirectoryError (not ProtocolError) since it is a directory-level semantic error, not a wire/protocol error

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

None - all methods fully wired to client API calls.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- revoke_delegation() and list_delegates() ready for integration testing in plan 02
- DelegationNotFoundError exported and importable from top-level chromatindb package
- All existing tests remain green -- no regressions

## Self-Check: PASSED

All created/modified files verified present. Both task commits (786cc63, 54473d7) verified in git log.

---
*Phase: 91-sdk-delegation-revocation*
*Completed: 2026-04-06*
