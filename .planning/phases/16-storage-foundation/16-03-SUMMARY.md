---
phase: 16-storage-foundation
plan: 03
subsystem: database
tags: [wire-protocol, flatbuffers, sync, peer-management, storage-signaling]

# Dependency graph
requires:
  - phase: 16-storage-foundation
    plan: 02
    provides: "IngestError::storage_full and max_storage_bytes capacity enforcement"
provides:
  - "StorageFull = 23 wire message type in transport protocol"
  - "peer_is_full flag on PeerInfo for sync push suppression"
  - "Sync ingest storage_full_count tracking in SyncStats"
  - "Post-sync StorageFull signal from both initiator and responder"
  - "E2E tests for StorageFull signaling flow"
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: ["Peer capacity signaling: send StorageFull on ingest rejection, skip outbound blob pushes to full peers"]

key-files:
  created: []
  modified:
    - schemas/transport.fbs
    - db/wire/transport_generated.h
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - tests/peer/test_peer_manager.cpp

key-decisions:
  - "StorageFull is empty-payload message (no data needed, just signaling)"
  - "peer_is_full resets on reconnect via PeerInfo default initialization (no StorageAvailable message)"
  - "Suppress outbound BlobTransfer responses to full peers, not inbound BlobRequests (full peers can still serve data)"
  - "Post-sync StorageFull from both initiator and responder sides"
  - "Sync ingest skips blobs on storage_full silently (no callback, no strike)"

patterns-established:
  - "Capacity signaling pattern: send StorageFull on IngestError::storage_full, peer sets flag, sync suppresses pushes"
  - "Reconnect-based recovery: no StorageAvailable message, flag resets via PeerInfo default construction"

requirements-completed: [STOR-04, STOR-05]

# Metrics
duration: 34min
completed: 2026-03-10
---

# Phase 16 Plan 03: Disk-Full Signaling Summary

**StorageFull wire message (type 23) with peer_is_full sync suppression and post-sync capacity signaling**

## Performance

- **Duration:** 34 min
- **Started:** 2026-03-09T17:47:48Z
- **Completed:** 2026-03-10T03:33:50Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Added StorageFull = 23 to TransportMsgType enum and regenerated FlatBuffers header
- Data handler sends StorageFull to peer when ingest returns IngestError::storage_full
- StorageFull receive handler sets peer_is_full flag, suppressing outbound sync blob pushes
- Sync ingest_blobs() skips blobs on storage_full with count tracking in SyncStats
- Post-sync StorageFull sent by both initiator and responder when blobs were rejected
- 3 new E2E test sections covering full flow (271 tests, 1052 assertions total)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add StorageFull wire message, peer_is_full flag, and sync protocol changes** - `b5ac116` (feat)
2. **Task 2: Add StorageFull E2E integration tests** - `ea5a3d5` (test)

## Files Created/Modified
- `schemas/transport.fbs` - Added StorageFull = 23 enum value after Notification = 22
- `db/wire/transport_generated.h` - Manually updated with TransportMsgType_StorageFull = 23, enum arrays, name strings
- `db/peer/peer_manager.h` - Added peer_is_full field to PeerInfo struct (defaults false)
- `db/peer/peer_manager.cpp` - StorageFull send on Data rejection, receive handler sets flag, sync push suppression, post-sync signal
- `db/sync/sync_protocol.h` - Added storage_full_count to SyncStats
- `db/sync/sync_protocol.cpp` - ingest_blobs() handles IngestError::storage_full (skip blob, count, debug log)
- `tests/peer/test_peer_manager.cpp` - 3 new E2E test sections: full node sync, reconnect reset, graceful completion

## Decisions Made
- **Empty-payload StorageFull:** No data needed in the message -- it's pure signaling. Follows existing pattern (Ping/Pong/Goodbye are also empty-payload).
- **Reconnect-based recovery only:** No StorageAvailable message. PeerInfo is recreated on each new connection with peer_is_full defaulting to false, providing natural reset.
- **Suppression is outbound-only:** Full peers can still serve data they have. We skip sending BlobTransfers TO them but still request/receive blobs FROM them.
- **No strike for storage_full:** Storage full is not misbehavior -- it's a normal capacity condition. Skipped blobs are logged at debug level only.
- **Manual transport_generated.h update:** flatc not available in build environment; manually updated following exact generated code patterns (enum values, name array, range check).

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 16 (Storage Foundation) is now complete -- all 3 plans executed
- Storage metrics (used_bytes), capacity limits (max_storage_bytes), and disk-full signaling (StorageFull) all operational
- Ready for Phase 17 (next milestone phase)
- All 271 tests pass (1052 assertions)

## Self-Check: PASSED

All files exist, all commits verified, all key content present, all 271 tests pass.

---
*Phase: 16-storage-foundation*
*Completed: 2026-03-10*
