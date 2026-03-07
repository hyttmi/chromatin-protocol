---
phase: 11-larger-blob-support
plan: 02
subsystem: storage, sync
tags: [index-reads, seq-map, hash-collection, memory-efficiency]

requires:
  - phase: 10-access-control
    provides: storage layer, sync protocol
provides:
  - Storage::get_hashes_by_namespace for O(n) index-only hash reads
  - SyncProtocol::collect_namespace_hashes uses index-only path
affects: [11-03]

tech-stack:
  added: []
  patterns:
    - "Index-only reads from seq_map (32-byte values) without touching blobs_map"
    - "Expiry filtering deferred to receiving end (ingest_blobs) rather than sender"

key-files:
  created: []
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/peer/peer_manager.cpp
    - tests/storage/test_storage.cpp
    - tests/sync/test_sync_protocol.cpp

key-decisions:
  - "Expiry filtering removed from collect_namespace_hashes -- peers handle expired blobs at ingest time"
  - "SyncProtocol constructor now takes Storage& alongside BlobEngine&"

patterns-established:
  - "seq_map as index: cursor scan of 40-byte keys (ns:32 + seq:8) yields 32-byte hash values"

requirements-completed: [BLOB-03]

duration: 8min
completed: 2026-03-07
---

# Phase 11 Plan 02: Hash Index + Sync Optimization Summary

**Storage::get_hashes_by_namespace reads blob hashes from seq_map index without loading blob data, enabling memory-efficient sync hash collection for large blobs**

## Performance

- **Duration:** 8 min
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Storage::get_hashes_by_namespace scans seq_map index to return blob hashes in seq_num order without touching blobs_map
- SyncProtocol::collect_namespace_hashes now uses index-only reads instead of loading all blobs into memory
- Expiry filtering deferred to receiving end -- simplifies sender logic, expired blobs are harmless (ingest_blobs filters them)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add Storage::get_hashes_by_namespace** - `42f23ca` (feat)
2. **Task 2: Update SyncProtocol::collect_namespace_hashes to use index-only reads** - `8faaa16` (feat)

## Files Created/Modified
- `db/storage/storage.h` - Added get_hashes_by_namespace declaration
- `db/storage/storage.cpp` - Implemented cursor scan of seq_map for hash retrieval
- `db/sync/sync_protocol.h` - Updated constructor to take Storage& reference, updated doc comments
- `db/sync/sync_protocol.cpp` - Rewrote collect_namespace_hashes to use storage index
- `db/peer/peer_manager.cpp` - Updated SyncProtocol construction to pass storage
- `tests/storage/test_storage.cpp` - Added 6 test cases for get_hashes_by_namespace
- `tests/sync/test_sync_protocol.cpp` - Updated SyncProtocol constructors, adjusted expiry tests for new behavior

## Decisions Made
- Removed expiry filtering from collect_namespace_hashes. Expired blobs in hash lists are harmless -- the receiving peer's ingest_blobs already skips expired blobs. This simplifies the sender and avoids needing blob data at hash collection time.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Index-only hash reads established; Plan 11-03 (one-blob-at-a-time sync) can proceed
- All 194 tests pass

---
*Phase: 11-larger-blob-support*
*Completed: 2026-03-07*
