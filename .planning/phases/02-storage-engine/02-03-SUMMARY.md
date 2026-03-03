---
phase: 02-storage-engine
plan: 03
subsystem: storage
tags: [ttl, expiry-scan, injectable-clock]

# Dependency graph
requires:
  - phase: 02-storage-engine-02
    provides: "Sequence indexing and expiry index population"
provides:
  - "TTL expiry scanner (run_expiry_scan)"
  - "Injectable clock for deterministic testing"
  - "Complete Phase 2 storage engine"
affects: [blob-engine, sync]

# Tech tracking
tech-stack:
  added: []
  patterns: [injectable-clock, cursor-scan-with-txn-erase]

key-files:
  created: []
  modified: []

key-decisions:
  - decision: "Use txn.erase(map, key) instead of cursor.erase() for expiry deletion"
    rationale: "cursor.erase() threw MDBX_ENODATA; advancing cursor first then txn.erase by key works reliably"
  - decision: "Expiry scan deletes from blobs + expiry indexes only, leaves seq entries intact"
    rationale: "Seq gaps are expected by design; callers skip missing blobs in range queries"

patterns-established:
  - "Injectable clock pattern: Storage(path, [&]() { return fake_time; })"
  - "Cursor advance-then-delete pattern to avoid cursor invalidation"

requirements-completed: [STOR-05, STOR-06]

# Metrics
duration: ~5min (bug fix for cursor.erase MDBX_ENODATA)
completed: 2026-03-03
---

# Phase 02 Plan 03: TTL Expiry Scanner Summary

**run_expiry_scan implemented with injectable clock for deterministic testing. Expired blobs purged from blobs and expiry indexes while seq entries remain. 8 tests covering all expiry scenarios.**

## Performance

- **Duration:** ~5 min (debugging cursor.erase MDBX_ENODATA, fix applied)
- **Completed:** 2026-03-03
- **Tasks:** 1
- **Tests passing:** 8 expiry-specific tests (22 total storage tests)

## Accomplishments
- Implemented run_expiry_scan: cursor scan from first expiry entry, delete where expiry_ts <= now
- Injectable clock enables fully deterministic expiry testing (no sleep/real-time waits)
- TTL=0 blobs never have expiry index entries, never purged
- Expiry scan is idempotent (second call returns 0)
- Seq entries remain after expiry; get_blobs_by_seq handles gaps
- Mixed permanent/ephemeral blobs: only ephemeral purged

## Task Commits

Code was committed as part of Plan 02-01: `89fc0bb`
Bug fix for cursor.erase was included in the same commit.

## Tests Covering Plan 02-03

- `Storage expiry scan with no expired blobs returns 0` - clock before expiry
- `Storage expiry scan purges expired blob` - single blob, advance past TTL
- `Storage expiry scan selective purge` - 3 blobs, advance past 2
- `Storage expiry scan is idempotent` - second scan returns 0
- `Storage TTL=0 blobs are never purged` - permanent blob survives far-future clock
- `Storage mixed TTL=0 and TTL>0 expiry` - only ephemeral purged
- `Storage expiry scan on empty database returns 0` - edge case
- `Storage seq entries remain after expiry (gaps expected)` - seq gap handling

## Deviations from Plan

- **[Auto-fixed] cursor.erase() MDBX_ENODATA**: Plan specified `cursor.erase()` to delete expiry entries. This threw MDBX_ENODATA. Fixed by advancing cursor to next entry first, then using `txn.erase(impl_->expiry_map, key_data)` to delete by key.

## Issues Encountered

- **MDBX_ENODATA on cursor.erase()**: libmdbx v0.13.11's cursor.erase() behavior differs from documentation expectations. The advance-then-delete-by-key pattern is more reliable.

## Phase 2 Complete

All Phase 2 requirements satisfied:
- STOR-01: Blob storage by namespace + hash (Plan 01)
- STOR-02: Content-addressed deduplication (Plan 01)
- STOR-03: Per-namespace monotonic sequence index (Plan 02)
- STOR-04: Expiry index sorted by timestamp (Plan 02)
- STOR-05: Automatic TTL expiry pruning (Plan 03)
- STOR-06: TTL=0 permanent blobs (Plan 03)
- DAEM-04: Crash recovery via libmdbx ACID (Plan 01)

**Total: 87 tests (244 assertions) -- 65 Phase 1 + 22 Phase 2, all passing.**

## Self-Check: PASSED

- FOUND: src/storage/storage.cpp with run_expiry_scan implementation
- FOUND: tests/storage/test_storage.cpp with expiry tests
- VERIFIED: 22 storage tests pass (8 basic + 6 seq + 8 expiry)
- VERIFIED: 87 total tests pass (no regressions)

---
*Phase: 02-storage-engine*
*Completed: 2026-03-03*
