---
phase: 57-client-protocol-extensions
plan: 02
subsystem: protocol
tags: [wire-protocol, client-api, read-request, list-request, stats-request, pagination, storage-query]

# Dependency graph
requires:
  - phase: 57-client-protocol-extensions
    plan: 01
    provides: "7 new TransportMsgType enum values (31-37), WriteAck dispatch, public effective_quota()"
provides:
  - "ReadRequest/ReadResponse handlers (type 32-33) for blob fetch by namespace+hash"
  - "ListRequest/ListResponse handlers (type 34-35) for paginated blob listing"
  - "StatsRequest/StatsResponse handlers (type 36-37) for namespace usage queries"
  - "Storage::get_blob_refs_since() for efficient seq_map-only pagination"
  - "Payload format tests verifying big-endian encoding conventions"
  - "PROTOCOL.md Client Protocol section with byte-level specs for types 31-37"
affects: [relay, client-sdk]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Client protocol handlers spawn coroutines but do NOT check sync_namespaces_ (reads serve all stored data)"
    - "ListRequest pagination: fetch limit+1 to detect has_more, cap at MAX_LIST_LIMIT=100"
    - "get_blob_refs_since reads only seq_map (no blobs_map), O(count) with minimal I/O"

key-files:
  created: []
  modified:
    - "db/storage/storage.h"
    - "db/storage/storage.cpp"
    - "db/peer/peer_manager.cpp"
    - "db/tests/net/test_protocol.cpp"
    - "db/PROTOCOL.md"

key-decisions:
  - "Client read/list/stats handlers do not filter by sync_namespaces_ -- reads serve whatever is in storage"
  - "ListRequest caps limit at 100 entries per response (MAX_LIST_LIMIT) for bounded memory"
  - "get_blob_refs_since skips zero-hash sentinels (deleted blobs) for clean pagination"

patterns-established:
  - "Client protocol handlers: validate payload size, record_strike on malformed, spawn detached coroutine"
  - "Pagination via since_seq cursor: fetch limit+1, truncate, set has_more flag"

requirements-completed: [PROTO-02, PROTO-03, PROTO-04]

# Metrics
duration: 7min
completed: 2026-03-23
---

# Phase 57 Plan 02: Client Protocol Handlers Summary

**ReadRequest, ListRequest, StatsRequest handlers with efficient seq_map pagination, payload format tests, and byte-level PROTOCOL.md documentation**

## Performance

- **Duration:** 7 min
- **Started:** 2026-03-23T03:38:05Z
- **Completed:** 2026-03-23T03:45:20Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Three client protocol handlers (ReadRequest, ListRequest, StatsRequest) dispatching in peer_manager.cpp with payload validation and strike recording
- Storage::get_blob_refs_since() for efficient seq_map-only iteration (no blobs_map reads), with zero-hash sentinel skipping
- ListRequest pagination with has_more detection (fetch limit+1), capped at 100 entries per response
- StatsRequest returns blob_count, total_bytes, and quota byte limit from storage + engine APIs
- 4 new payload format test sections verifying big-endian encoding conventions
- PROTOCOL.md updated with full Client Protocol section (WriteAck, ReadRequest/Response, ListRequest/Response, StatsRequest/Response) plus 7 new rows in Message Type Reference table

## Task Commits

Each task was committed atomically:

1. **Task 1: Add storage query method and three client protocol handlers** - `a1cf2ba` (feat)
2. **Task 2: Add payload format tests and update PROTOCOL.md** - `18bcd10` (feat)

## Files Created/Modified
- `db/storage/storage.h` - Added BlobRef struct and get_blob_refs_since() declaration
- `db/storage/storage.cpp` - Implemented get_blob_refs_since() with mdbx cursor pattern
- `db/peer/peer_manager.cpp` - Added ReadRequest, ListRequest, StatsRequest dispatch handlers
- `db/tests/net/test_protocol.cpp` - Added 4 payload format test sections (ReadRequest, ListRequest, ListResponse, StatsResponse)
- `db/PROTOCOL.md` - Added Client Protocol section with byte-level specs and 7 new message type table rows

## Decisions Made
- Client read/list/stats handlers do not check sync_namespaces_ -- read operations serve whatever is in storage (per RESEARCH.md guidance)
- ListRequest caps limit at 100 entries (MAX_LIST_LIMIT=100) to bound response size and memory usage
- get_blob_refs_since skips zero-hash sentinels left by delete_blob_data, ensuring clean pagination results

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed big-endian byte offset in StatsResponse test data**
- **Found during:** Task 2 (payload format tests)
- **Issue:** Plan specified payload[5]=0x03, payload[6]=0xE8 for blob_count=1000, but correct big-endian uint64 placement is payload[6]=0x03, payload[7]=0xE8. Similarly total_bytes offset was off by one.
- **Fix:** Corrected byte offsets to payload[6]/[7] for blob_count and payload[13] for total_bytes
- **Files modified:** db/tests/net/test_protocol.cpp
- **Verification:** Test passes with REQUIRE(blob_count == 1000)
- **Committed in:** 18bcd10 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug in test data)
**Impact on plan:** Trivial byte offset correction in test. No scope creep.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- All 7 client protocol message types (31-37) fully implemented and tested
- PROTOCOL.md provides byte-level documentation for client implementors
- Phase 57 (client-protocol-extensions) is complete -- all 4 requirements (PROTO-01 through PROTO-04) delivered
- Ready for Phase 58 (relay) which will filter these message types through the PQ-authenticated relay

## Self-Check: PASSED

- All 5 modified files exist on disk
- Commits a1cf2ba and 18bcd10 verified in git log
- SUMMARY.md created at expected path

---
*Phase: 57-client-protocol-extensions*
*Completed: 2026-03-23*
