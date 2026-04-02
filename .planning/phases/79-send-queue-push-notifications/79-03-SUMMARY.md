---
phase: 79-send-queue-push-notifications
plan: 03
subsystem: networking
tags: [asio, coroutines, push-notifications, blob-notify, fan-out]

requires:
  - phase: 79-01
    provides: BlobNotify=59 wire type in FlatBuffers schema and relay filter
  - phase: 79-02
    provides: Per-connection send queue serializing all outbound messages

provides:
  - Unified on_blob_ingested fan-out in PeerManager replacing triple call sites
  - Engine ingest/delete_blob accept optional source Connection for fan-out exclusion
  - SyncProtocol passes source connection through to callback
  - BlobNotify (type 59) dispatched to all TCP peers on every successful ingest
  - Source exclusion prevents notification back to syncing peer

affects: [80-push-driven-sync-loop, 82-reconcile-on-connect]

tech-stack:
  added: []
  patterns: [unified-fan-out-callback, source-exclusion-on-dispatch]

key-files:
  created: []
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Source parameter on ingest stored in IngestResult rather than via separate engine callback"
  - "notify_subscribers removed entirely, replaced by on_blob_ingested with BlobNotify + Notification dispatch"
  - "Client writes (Data/Delete handlers) pass nullptr as source to notify ALL peers"

patterns-established:
  - "Unified fan-out: on_blob_ingested handles both BlobNotify to TCP peers and Notification to subscribers"
  - "Source exclusion: sync-received blobs skip BlobNotify to the originating peer"
  - "UDS skip: BlobNotify only to TCP peers, UDS connections are clients"

requirements-completed: [PUSH-01, PUSH-02, PUSH-03, PUSH-07, PUSH-08]

duration: 42min
completed: 2026-04-02
---

# Phase 79 Plan 03: Unified BlobNotify Fan-out Summary

**Unified on_blob_ingested replaces triple notify_subscribers call sites with BlobNotify (type 59) to TCP peers and Notification (type 21) to subscribers, with source exclusion preventing notification storms**

## Performance

- **Duration:** 42 min
- **Started:** 2026-04-02T12:17:50Z
- **Completed:** 2026-04-02T12:59:45Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Engine ingest() and delete_blob() accept optional Connection::Ptr source for downstream fan-out
- SyncProtocol ingest_blobs() passes source through to engine and OnBlobIngested callback
- Unified on_blob_ingested method dispatches BlobNotify (type 59) to all TCP peers except source, and Notification (type 21) to subscribed clients
- All three former notify_subscribers call sites (Data handler, Delete handler, sync callback) consolidated into single unified fan-out
- Source exclusion prevents syncing peer from receiving BlobNotify for blobs it sent
- UDS connections (clients) excluded from BlobNotify dispatch
- 3 new integration tests: sync fan-out, tombstone fan-out, three-node source exclusion

## Task Commits

Each task was committed atomically:

1. **Task 1: Modify engine ingest signature and sync protocol source passthrough** - `37e4fe2` (feat)
2. **Task 2: Unified PeerManager fan-out with BlobNotify and source exclusion** - `32a2ef9` (feat)

## Files Created/Modified
- `db/engine/engine.h` - Added source parameter to ingest/delete_blob, source field in IngestResult
- `db/engine/engine.cpp` - ingest/delete_blob store source in result on all success paths
- `db/sync/sync_protocol.h` - OnBlobIngested callback includes source, ingest_blobs takes source
- `db/sync/sync_protocol.cpp` - Passes source to engine_.ingest() and callback
- `db/peer/peer_manager.h` - on_blob_ingested declaration replacing notify_subscribers
- `db/peer/peer_manager.cpp` - Unified fan-out implementation, all call sites updated, sync callers pass conn
- `db/tests/peer/test_peer_manager.cpp` - 3 BlobNotify integration tests

## Decisions Made
- Source parameter stored in IngestResult rather than implementing a separate engine callback -- simpler, avoids additional callback registration and keeps the source tied to the result
- Client writes (Data/Delete handlers) pass nullptr as source so ALL peers receive BlobNotify -- clients connect via relay/UDS, not as peers, so no source exclusion needed
- notify_subscribers removed entirely (0 references remaining) rather than keeping as deprecated wrapper

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Updated PeerManager constructor callback signature during Task 1**
- **Found during:** Task 1 (engine/sync signature changes)
- **Issue:** Changing OnBlobIngested callback signature in sync_protocol.h broke the PeerManager constructor lambda (missing source parameter)
- **Fix:** Updated the lambda to accept the source parameter (initially ignored, fully wired in Task 2)
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** Build succeeded, all tests passed
- **Committed in:** 37e4fe2 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Necessary to keep build green between tasks. No scope creep.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- BlobNotify fan-out is now active on every blob ingest
- Phase 80 (push-driven sync loop) can now receive BlobNotify and trigger targeted fetch
- All 579+ existing tests pass alongside 3 new BlobNotify tests

## Self-Check: PASSED

All 7 modified files verified present. Both task commits (37e4fe2, 32a2ef9) verified in git log. No stubs detected.

---
*Phase: 79-send-queue-push-notifications*
*Completed: 2026-04-02*
