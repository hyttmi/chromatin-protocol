---
phase: 76-directory-user-discovery
plan: 02
subsystem: sdk
tags: [python, directory, pub-sub, cache, delegation, user-discovery, ml-dsa-87, ml-kem-1024]

# Dependency graph
requires:
  - phase: 76-directory-user-discovery
    plan: 01
    provides: UserEntry codec (encode/decode/verify), DirectoryEntry dataclass, DirectoryError exception, make_delegation_data helper
  - phase: 75-identity-extension-envelope-crypto
    provides: Identity.from_public_keys(), Identity.sign(), Identity.verify(), has_kem, kem_public_key
provides:
  - Directory class with admin/user modes
  - Admin delegation (write_blob with DELEGATION_MAGIC)
  - User self-registration (encode_user_entry + write_blob)
  - Cached user listing with lazy populate and O(1) secondary indexes
  - Pub/sub notification-driven cache invalidation (drain-and-requeue pattern)
  - 26 new unit tests with mocked ChromatinClient
affects: [77-group-encryption, 78-integration]

# Tech tracking
tech-stack:
  added: []
  patterns: [lazy-cache-with-dirty-flag, drain-and-requeue-notification, subscribe-before-scan, type-checking-guard-circular-import]

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/_directory.py
    - sdk/python/chromatindb/__init__.py
    - sdk/python/tests/test_directory.py

key-decisions:
  - "Drain-and-requeue pattern for notification observation (not background task) -- simpler, no task lifecycle management"
  - "TYPE_CHECKING guard for ChromatinClient import to avoid circular dependency"
  - "refresh() is synchronous (just clears state), all other Directory methods are async"

patterns-established:
  - "Lazy cache with dirty flag: populate on first read, invalidate via notification, rebuild on next access"
  - "Drain-and-requeue: read all items from asyncio.Queue, inspect, put back for other consumers"
  - "Subscribe-before-scan: subscribe to namespace before enumeration to avoid missing notifications during scan"

requirements-completed: [DIR-01, DIR-02, DIR-04, DIR-05, DIR-06]

# Metrics
duration: 5min
completed: 2026-04-01
---

# Phase 76 Plan 02: Directory Class Summary

**Directory class with admin delegation, user self-registration, cached O(1) lookups, and pub/sub invalidation via drain-and-requeue**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-01T16:31:05Z
- **Completed:** 2026-04-01T16:36:29Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Directory class with admin mode (Directory(client, admin_identity)) and user mode (Directory(client, identity, directory_namespace=ns))
- Admin delegation via delegate() writes DELEGATION_MAGIC + pubkey blob for write access grants
- User self-registration via register() encodes UserEntry with KEM cross-key binding and writes to directory namespace
- Lazy cache with O(1) secondary indexes (_by_name, _by_pubkey_hash) for instant lookups
- Subscribe-before-scan pattern (Pitfall 6) prevents missed notifications during cache population
- Drain-and-requeue notification observation (Pitfall 2) avoids stealing notifications from user code
- 26 new unit tests with AsyncMock-based ChromatinClient covering all Directory methods
- Full SDK suite: 433 tests pass, zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Directory class implementation** - `6f98319` (feat)
2. **Task 2: Directory class unit tests** - `7a68ad5` (test)

## Files Created/Modified
- `sdk/python/chromatindb/_directory.py` - Added Directory class with delegate, register, list_users, get_user, get_user_by_pubkey, refresh, _populate_cache, _check_invalidation
- `sdk/python/chromatindb/__init__.py` - Added Directory to imports and __all__
- `sdk/python/tests/test_directory.py` - Added 26 new tests (46 total) across 6 test classes: TestDirectoryInit, TestDelegate, TestRegister, TestListUsers, TestGetUser, TestCache

## Decisions Made
- Used drain-and-requeue pattern (not background asyncio.Task) for notification observation -- simpler lifecycle, no task management needed
- Used TYPE_CHECKING guard for ChromatinClient import to break circular dependency chain
- Made refresh() synchronous since it only clears in-memory state

## Deviations from Plan
None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Directory class fully operational for Phase 77's group encryption layer
- DirectoryEntry.identity objects are encrypt-capable (has_kem=True) for envelope encryption
- All public names re-exported from chromatindb package
- 433 SDK tests green, ready for Phase 77 group operations to build on top

## Self-Check: PASSED

All files exist, all commits verified.

---
*Phase: 76-directory-user-discovery*
*Completed: 2026-04-01*
