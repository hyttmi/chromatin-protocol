---
phase: 98-ttl-enforcement
plan: 02
subsystem: database
tags: [ttl, expiry, query-filtering, handler-enforcement, blob-fetch]

# Dependency graph
requires:
  - phase: 98-ttl-enforcement
    plan: 01
    provides: saturating_expiry() and is_blob_expired() in wire namespace
provides:
  - All 7 query handlers filter expired blobs
  - BlobFetch returns not-found for expired blobs
  - BlobNotify re-fetches if local copy expired
  - on_blob_ingested suppresses notifications for expired blobs
affects: [98-03, ttl-enforcement]

# Tech tracking
tech-stack:
  added: []
  patterns: [handler-level expiry filtering with wire::is_blob_expired, has_blob->get_blob upgrade for expiry awareness]

key-files:
  created: []
  modified:
    - db/peer/message_dispatcher.cpp
    - db/peer/blob_push_manager.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "ExistsRequest and BatchExistsRequest upgraded from has_blob to get_blob for expiry awareness"
  - "ListRequest filters per-ref via get_blob + is_blob_expired (capped at 100 lookups)"
  - "BlobNotify receiver re-fetches if local copy is expired (D-17)"
  - "on_blob_ingested Step 0 suppression uses pre-computed expiry_time parameter"
  - "Tests use store.store_blob() directly to bypass engine already-expired rejection"

patterns-established:
  - "Every query handler MUST check wire::is_blob_expired after get_blob, before response encoding"
  - "Multi-blob handlers compute now once at handler start for consistent snapshot"

requirements-completed: [TTL-01, TTL-02]

# Metrics
duration: 65min
completed: 2026-04-08
---

# Phase 98 Plan 02: Handler TTL Enforcement Summary

**Expiry checks in all 7 query handlers + MetadataRequest + BlobFetch + BlobNotify + notification fan-out, with 8 handler tests proving each path rejects expired blobs**

## Performance

- **Duration:** 65 min
- **Started:** 2026-04-08T17:51:19Z
- **Completed:** 2026-04-08T18:57:17Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Added `wire::is_blob_expired` checks to all 7 query handlers in message_dispatcher.cpp (Read, List, Exists, BatchExists, BatchRead, TimeRange, Metadata)
- BlobFetch handler returns not-found (0x01) for expired blobs
- BlobNotify receiver upgraded from `has_blob` to `get_blob + is_blob_expired`, allowing re-fetch of expired local copies
- `on_blob_ingested` suppresses BlobNotify and Notification fan-out for already-expired blobs (Step 0 pattern)
- StatsRequest and NamespaceStatsRequest intentionally unchanged (report storage reality)
- 8 new handler TTL enforcement tests covering Read, Exists, BatchExists, BatchRead, List, TimeRange, Metadata, and BlobFetch
- All 76 peer tests pass (506 assertions), all 22 TTL tests pass (81 assertions)

## Task Commits

Each task was committed atomically:

1. **Task 1: Expiry checks in all query handlers + BlobFetch + BlobNotify + notification suppression** - `a05dc40` (feat)
2. **Task 2: Handler TTL enforcement tests** - `ecaff6c` (test)

## Files Created/Modified
- `db/peer/message_dispatcher.cpp` - Added wire::is_blob_expired checks to 7 query handlers (ReadRequest, ListRequest, ExistsRequest, MetadataRequest, BatchExistsRequest, BatchReadRequest, TimeRangeRequest); upgraded ExistsRequest and BatchExistsRequest from has_blob to get_blob
- `db/peer/blob_push_manager.cpp` - Added BlobFetch expiry check, upgraded BlobNotify has_blob to get_blob for expiry-aware re-fetch, added on_blob_ingested Step 0 expiry suppression
- `db/tests/peer/test_peer_manager.cpp` - Added 8 new TEST_CASEs with [ttl] tag proving each handler rejects expired blobs

## Decisions Made
- ExistsRequest and BatchExistsRequest upgraded from `storage_.has_blob()` to `storage_.get_blob()` + `wire::is_blob_expired()` -- slightly more expensive but required for expiry awareness
- ListRequest expiry filtering requires per-ref `get_blob()` lookup since BlobRef contains only hash+seq_num (no timestamp/ttl) -- capped at 100 results per handler limit
- Multi-blob handlers (BatchExistsRequest, BatchReadRequest, TimeRangeRequest, ListRequest) compute `now` once at handler start for consistent snapshot within a single response (D-04)
- BlobNotify receiver proceeds to fetch if local blob is expired (D-17), preventing stale expired copies from blocking re-acquisition
- on_blob_ingested uses pre-computed `expiry_time` parameter for Step 0 suppression -- zero extra computation
- Tests use `store.store_blob()` directly to bypass engine's already-expired ingest rejection (from Plan 01), allowing insertion of test blobs that are expired at query time

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Engine rejects already-expired blob ingest (test adaptation)**
- **Found during:** Task 2 (TDD RED phase)
- **Issue:** Plan 01 added engine-level rejection of already-expired blobs. Test blobs with `timestamp = now - 1000, ttl = 100` are correctly rejected by `eng.ingest()`.
- **Fix:** Tests use `store.store_blob()` directly to bypass engine validation, simulating a blob that was valid at ingest time but has since expired.
- **Files modified:** db/tests/peer/test_peer_manager.cpp
- **Verification:** All 8 TTL handler tests pass, all 76 peer tests pass
- **Committed in:** ecaff6c (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (test adaptation for Plan 01's already-expired rejection)
**Impact on plan:** Minimal. The storage-level bypass is the correct test approach -- it simulates blobs that expire between ingest and query, which is the real-world scenario these handler checks protect against.

## Issues Encountered
- Build system stale state: shared build directory between worktrees had stale dependency artifacts (liboqs, Catch2, asio). Resolved by cleaning dependency build directories and reconfiguring. Used worktree-local build directory for correct source compilation.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None.

## Next Phase Readiness
- All query handlers, BlobFetch, BlobNotify, and notification fan-out now enforce expiry
- Plan 03 (sync path filtering) can proceed -- sync_orchestrator expiry checks are the remaining coverage gap
- Zero `has_blob` calls remain in message_dispatcher.cpp or blob_push_manager.cpp for query paths

## Self-Check: PASSED

- FOUND: 98-02-SUMMARY.md
- FOUND: a05dc40 (Task 1 commit)
- FOUND: ecaff6c (Task 2 commit)
- FOUND: db/peer/message_dispatcher.cpp
- FOUND: db/peer/blob_push_manager.cpp
- FOUND: db/tests/peer/test_peer_manager.cpp

---
*Phase: 98-ttl-enforcement*
*Completed: 2026-04-08*
