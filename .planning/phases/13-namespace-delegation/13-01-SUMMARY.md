---
phase: 13-namespace-delegation
plan: 01
subsystem: database
tags: [delegation, wire-codec, mdbx, index, pq-crypto]

requires:
  - phase: 12-blob-deletion
    provides: tombstone pattern (magic-prefix codec, storage operations)
provides:
  - Delegation wire codec (DELEGATION_MAGIC, is_delegation, extract_delegate_pubkey, make_delegation_data)
  - delegation_map sub-database in Storage with O(1) indexed lookup
  - has_valid_delegation() public API for delegate verification
  - Delegation blob creation via existing ingest pipeline
affects: [13-namespace-delegation]

tech-stack:
  added: []
  patterns: [magic-prefix blob type identification, indexed sub-database lookup]

key-files:
  created: []
  modified:
    - db/wire/codec.h
    - db/wire/codec.cpp
    - db/storage/storage.h
    - db/storage/storage.cpp
    - tests/storage/test_storage.cpp
    - tests/engine/test_engine.cpp

key-decisions:
  - "DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E} -- mnemonic for 'delegate'"
  - "Delegation index key = [namespace:32][SHA3-256(delegate_pubkey):32] -- compact fixed-size keys instead of full 2592-byte pubkeys"
  - "Delegation index value = [delegation_blob_hash:32] -- tracks which blob created the delegation"
  - "max_maps bumped from 4 to 5 (4 needed + 1 spare) for delegation_map"
  - "Zero engine changes needed for delegation blob creation -- existing ingest pipeline handles it naturally"

patterns-established:
  - "Magic-prefix pattern reused: delegation follows tombstone's 4-byte prefix + payload pattern"
  - "Index population in store_blob / cleanup in delete_blob_data -- transparent to callers"

requirements-completed: [DELEG-01, DELEG-03]

duration: 12min
completed: 2026-03-08
---

# Plan 13-01: Delegation Wire Codec & Storage Index Summary

**Delegation blob format with 4-byte magic prefix, delegation_map sub-database with O(1) indexed lookup, and delegation blob creation via existing ingest pipeline**

## Performance

- **Duration:** 12 min
- **Started:** 2026-03-08
- **Completed:** 2026-03-08
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- Delegation wire codec following tombstone's magic-prefix pattern (DELEGATION_MAGIC, is_delegation, extract_delegate_pubkey, make_delegation_data)
- delegation_map sub-database in Storage with automatic index population on store and cleanup on delete
- has_valid_delegation() performs O(1) btree lookup by [namespace:32][delegate_pk_hash:32]
- Delegation blobs flow through existing ingest() pipeline with zero engine changes (DELEG-01)
- Delegation blobs are regular blobs retrievable via get_blob, proving sync compatibility (DELEG-03)

## Task Commits

Each task was committed atomically:

1. **Task 1: Delegation wire codec and Storage delegation index** - `98bb6a3` (feat)
2. **Task 2: Engine delegation blob creation and tests** - `aeb0fd4` (test)

## Files Created/Modified
- `db/wire/codec.h` - DELEGATION_MAGIC, DELEGATION_PUBKEY_SIZE, DELEGATION_DATA_SIZE, is_delegation(), extract_delegate_pubkey(), make_delegation_data()
- `db/wire/codec.cpp` - Delegation codec implementations
- `db/storage/storage.h` - has_valid_delegation() public method
- `db/storage/storage.cpp` - delegation_map sub-database, index population in store_blob, cleanup in delete_blob_data, has_valid_delegation() implementation
- `tests/storage/test_storage.cpp` - 9 delegation tests (codec + index)
- `tests/engine/test_engine.cpp` - 6 engine delegation blob creation tests

## Decisions Made
- DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E} -- "delegate" mnemonic, distinct from TOMBSTONE_MAGIC
- Delegation index key uses SHA3-256(delegate_pubkey) instead of full 2592-byte pubkey for compact fixed-size keys
- max_maps increased from 4 to 5 to accommodate delegation_map while maintaining a spare slot
- No engine changes needed -- delegation blobs are structurally regular blobs with specific data content

## Deviations from Plan
None - plan executed exactly as written

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Delegation codec and storage index ready for Plan 13-02
- has_valid_delegation() API ready for engine delegation bypass in ingest()
- All 231 tests pass (216 original + 15 new)

---
*Phase: 13-namespace-delegation*
*Completed: 2026-03-08*
