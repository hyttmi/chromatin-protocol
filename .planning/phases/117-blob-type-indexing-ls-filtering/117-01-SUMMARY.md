---
phase: 117-blob-type-indexing-ls-filtering
plan: 01
subsystem: database
tags: [mdbx, wire-protocol, blob-type, indexing, storage, schema-migration]

requires:
  - phase: 95-node-hardening
    provides: "overflow-checked arithmetic utilities in db/util/endian.h"
provides:
  - "BlobRef.blob_type field populated on store and retrieval"
  - "36-byte seq_map values [hash:32][type:4] with schema v2 migration"
  - "extract_blob_type helper in codec.h"
  - "ListRequest flags (include_all, type_filter) at offset 44-49"
  - "44-byte ListResponse entries with type prefix"
  - "Type-based tombstone/delegation filtering (no full blob load)"
affects: [117-02-cli-ls-filtering, future-type-aware-queries]

tech-stack:
  added: []
  patterns:
    - "4-byte type prefix extraction from blob data on ingest"
    - "Schema migration with version stamping in meta_map"
    - "Length-based optional field detection in wire protocol"

key-files:
  created: []
  modified:
    - db/wire/codec.h
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/peer/message_dispatcher.cpp
    - db/PROTOCOL.md
    - db/tests/storage/test_storage.cpp

key-decisions:
  - "Zero-type migration: pre-existing seq_map entries get {0,0,0,0} type prefix because blob_key_ is not yet derived at migration time"
  - "ListRequest flags use length-based detection (44 bytes = no flags, 45+ = flags present) for backward compatibility"

patterns-established:
  - "seq_map values are 36 bytes: [hash:32][type:4]. Code must use >= 32 length checks, not == 32"
  - "Type filtering uses raw 4-byte memcmp against magic constants, no enum or validation"

requirements-completed: [TYPE-01, TYPE-02, TYPE-03, TYPE-04]

duration: 57min
completed: 2026-04-16
---

# Phase 117 Plan 01: Blob Type Indexing Summary

**Node-side 4-byte blob type indexing on ingest with 44-byte ListResponse entries, optional ListRequest flags, and type-based tombstone/delegation filtering**

## Performance

- **Duration:** 57 min
- **Started:** 2026-04-16T07:40:24Z
- **Completed:** 2026-04-16T08:37:48Z
- **Tasks:** 3
- **Files modified:** 6

## Accomplishments
- Storage layer extracts and indexes first 4 bytes of blob data as type prefix on every store_blob and store_blobs_atomic call
- ListResponse now serves 44-byte entries (hash:32 + seq:8BE + type:4) with type field visible to clients
- ListRequest accepts optional flags byte for include_all (bit 0) and type_filter (bit 1 + 4 bytes)
- Tombstone/delegation filtering in ListRequest and TimeRangeRequest uses BlobRef.blob_type prefix instead of loading full blob data
- Schema v1-to-v2 migration extends existing 32-byte seq_map entries to 36 bytes
- 5 new unit tests covering type population, short blob handling, extensibility, atomic store, and hash retrieval

## Task Commits

Each task was committed atomically:

1. **Task 1: Storage layer type indexing + extract_blob_type helper + schema migration** - `85efdfed` (feat)
2. **Task 2: ListRequest flags parsing + ListResponse type field + type-based filtering optimization** - `5adf4354` (feat)
3. **Task 3: Unit tests for type indexing, migration, and extensibility** - `61a55740` (test)

## Files Created/Modified
- `db/wire/codec.h` - Added extract_blob_type inline helper and cstring include
- `db/storage/storage.h` - Added blob_type field to BlobRef struct
- `db/storage/storage.cpp` - Schema v2 migration, 36-byte seq_map values in store_blob/store_blobs_atomic, type population in get_blob_refs_since, >= 32 length checks in all seq_map readers
- `db/peer/message_dispatcher.cpp` - ListRequest flags parsing, 44-byte ListResponse entries, type-based tombstone/delegation filtering in both ListRequest and TimeRangeRequest handlers
- `db/PROTOCOL.md` - Updated ListRequest (44-49 bytes) and ListResponse (44-byte entries) wire format documentation
- `db/tests/storage/test_storage.cpp` - 5 new test cases for type indexing

## Decisions Made
- **Zero-type migration:** Pre-existing seq_map entries get `{0,0,0,0}` type prefix during schema v1->v2 migration. The `blob_key_` for decryption is not yet derived when `open_env()` runs, making full blob data extraction impossible at migration time. Zero-typed blobs will not match type filters but will be listed normally. New blobs always get correct types on ingest.
- **36-byte zero sentinel:** delete_blob_data now writes 36-byte zero sentinels (instead of 32-byte) to maintain consistent value sizes in seq_map.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed get_blobs_by_seq value length check**
- **Found during:** Task 1 (storage layer changes)
- **Issue:** `get_blobs_by_seq` at line 900 had `val_data.length() != 32` which would break with 36-byte values, causing 7 test failures
- **Fix:** Changed to `val_data.length() < 32`
- **Files modified:** db/storage/storage.cpp
- **Verification:** All 100 storage tests pass
- **Committed in:** 85efdfed (Task 1 commit)

**2. [Rule 1 - Bug] Fixed store_blob duplicate detection length check**
- **Found during:** Task 1 (storage layer changes)
- **Issue:** Duplicate blob detection in store_blob scanned seq_map with `v.length() == 32` which would miss 36-byte values, causing duplicate detection to fail
- **Fix:** Changed to `v.length() >= 32`
- **Files modified:** db/storage/storage.cpp
- **Committed in:** 85efdfed (Task 1 commit)

**3. [Rule 1 - Bug] Fixed store_blobs_atomic duplicate detection length check**
- **Found during:** Task 1 (storage layer changes)
- **Issue:** Same pattern as #2 but in store_blobs_atomic
- **Fix:** Changed to `v.length() >= 32`
- **Files modified:** db/storage/storage.cpp
- **Committed in:** 85efdfed (Task 1 commit)

**4. [Rule 1 - Bug] Fixed delete_blob_data seq_map scan and sentinel size**
- **Found during:** Task 1 (storage layer changes)
- **Issue:** delete_blob_data scanned with `v.length() == 32` and wrote 32-byte zero sentinel, both incompatible with 36-byte values
- **Fix:** Changed to `v.length() >= 32` and write 36-byte zero sentinel
- **Files modified:** db/storage/storage.cpp
- **Committed in:** 85efdfed (Task 1 commit)

---

**Total deviations:** 4 auto-fixed (4 Rule 1 bugs)
**Impact on plan:** All fixes were necessary - every seq_map reader in storage.cpp had hardcoded `== 32` or `!= 32` value length checks that would break with 36-byte values. The plan listed Sites A-F but missed these 4 additional sites.

## Issues Encountered
- **Build infrastructure:** Git worktree environment caused FetchContent failures (git operations fail because `.git` is a file, not a directory). Resolved by providing pre-fetched source directories via `-DFETCHCONTENT_SOURCE_DIR_*` cmake flags from the main repo's build cache and manually cloning Catch2.
- **Pre-existing flaky tests:** 1-3 peer manager / daemon sync tests fail intermittently with SIGSEGV or timing-dependent assertions. These are pre-existing and unrelated to this plan's changes. All 105 storage tests, 27 codec tests, and 247 storage/codec/engine tests pass.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- BlobRef.blob_type field ready for CLI ls filtering in Plan 02
- 44-byte ListResponse entries provide type prefix to clients
- ListRequest flags enable type_filter for server-side filtering
- Wire format fully documented in PROTOCOL.md

---
*Phase: 117-blob-type-indexing-ls-filtering*
*Completed: 2026-04-16*
