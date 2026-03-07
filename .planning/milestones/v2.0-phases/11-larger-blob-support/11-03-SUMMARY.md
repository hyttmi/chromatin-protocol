---
phase: 11-larger-blob-support
plan: 03
subsystem: peer, sync, wire
tags: [one-blob-at-a-time, batch-requests, timeout, memory-bounded-sync]

requires:
  - phase: 11-larger-blob-support
    provides: MAX_BLOB_DATA_SIZE constant, index-only hash reads
provides:
  - One-blob-at-a-time sync transfer keeping memory bounded
  - Batched BlobRequests capped at 64 hashes per message
  - Per-blob transfer timeout (120s) with graceful skip
  - encode_single_blob_transfer for efficient single-blob wire encoding
  - FlatBufferBuilder sized proportional to blob data
affects: []

tech-stack:
  added: []
  patterns:
    - "One-blob-at-a-time transfer: each BlobTransfer carries count=1"
    - "Batched requests with bounded response loop"
    - "Adaptive timeout: 120s for blob transfers vs 30s for control messages"

key-files:
  created: []
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/wire/codec.cpp
    - tests/sync/test_sync_protocol.cpp
    - tests/peer/test_peer_manager.cpp

key-decisions:
  - "MAX_HASHES_PER_REQUEST=64 caps BlobRequest batch size"
  - "BLOB_TRANSFER_TIMEOUT=120s per-blob timeout (4x control timeout)"
  - "Timeout skips blob, no strike recorded (may be slow network)"
  - "FlatBufferBuilder pre-sized to blob.data.size() + 8192 to avoid reallocation chains"

patterns-established:
  - "One-blob-at-a-time: encode_single_blob_transfer wraps single blob in count=1 format"

requirements-completed: [BLOB-04, BLOB-06]

duration: 10min
completed: 2026-03-07
---

# Phase 11 Plan 03: One-Blob-At-A-Time Sync + Timeouts Summary

**Reworked Phase C sync to send individual BlobTransfers with batched requests (max 64 hashes) and 120s per-blob timeout, preventing memory exhaustion with 100 MiB blobs**

## Performance

- **Duration:** 10 min
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- FlatBufferBuilder initial size scales with blob data to avoid reallocation chains for large blobs
- SyncProtocol::encode_single_blob_transfer wraps a single blob in count=1 wire format
- Phase C in both initiator and responder now uses batched requests (MAX_HASHES_PER_REQUEST=64) with individual blob transfers
- Per-blob timeout of 120s (BLOB_TRANSFER_TIMEOUT) prevents false failures on large blobs while 30s control timeout remains for protocol messages
- Timeout skips the blob and continues sync (no strike -- may be slow network, not malice)

## Task Commits

Each task was committed atomically:

1. **Task 1: Scale FlatBufferBuilder initial size in encode_blob** - `0cd5289` (perf)
2. **Task 2: Add sync constants and one-blob-at-a-time transfer logic** - `8b5e468` (feat)

## Files Created/Modified
- `db/wire/codec.cpp` - FlatBufferBuilder sized to blob.data.size() + 8192
- `db/peer/peer_manager.h` - Added MAX_HASHES_PER_REQUEST and BLOB_TRANSFER_TIMEOUT constants
- `db/peer/peer_manager.cpp` - Rewrote Phase C in both run_sync_with_peer and handle_sync_as_responder
- `db/sync/sync_protocol.h` - Added encode_single_blob_transfer declaration
- `db/sync/sync_protocol.cpp` - Implemented encode_single_blob_transfer
- `tests/sync/test_sync_protocol.cpp` - Added single blob transfer round-trip test
- `tests/peer/test_peer_manager.cpp` - Added constant verification tests

## Decisions Made
- MAX_HASHES_PER_REQUEST=64: balances batch efficiency with memory usage (64 * 32 = 2 KB per request)
- BLOB_TRANSFER_TIMEOUT=120s: 4x the control message timeout, sufficient for 100 MiB over slow links
- No strike on timeout: timeouts indicate network conditions, not peer misbehavior

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All 3 plans complete. Phase 11 ready for verification.
- All 196 tests pass including integration tests (two-node sync, three-node PEX).

---
*Phase: 11-larger-blob-support*
*Completed: 2026-03-07*
