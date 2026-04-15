---
phase: 115-chunked-streaming-for-large-blobs
plan: 01
subsystem: networking
tags: [chunked-streaming, uds, backpressure, framing, aead]

# Dependency graph
requires:
  - phase: 114-relay-thread-pool-offload
    provides: offload_if_large() infrastructure, single-threaded relay model
provides:
  - UDS chunked sub-frame protocol constants and header encode/decode helpers (relay/core/chunked_stream.h)
  - ChunkQueue backpressure queue primitive (bounded at 4 chunks)
  - MAX_BLOB_DATA_SIZE raised to 500 MiB in node framing
  - MAX_BODY_SIZE raised to 510 MiB in relay HTTP layer
  - UdsMultiplexer send_chunked/recv_chunked_reassemble method declarations
  - Node Connection chunked send/receive support (transparent for large payloads)
affects: [115-02-PLAN, 115-03-PLAN, 115-04-PLAN]

# Tech tracking
tech-stack:
  added: []
  patterns: [chunked-sub-frame-protocol, atomic-chunked-drain, asio-steady-timer-backpressure]

key-files:
  created:
    - relay/core/chunked_stream.h
    - relay/tests/test_chunked_stream.cpp
  modified:
    - db/net/framing.h
    - db/net/connection.h
    - db/net/connection.cpp
    - relay/http/http_connection.h
    - relay/core/uds_multiplexer.h
    - relay/tests/CMakeLists.txt
    - db/tests/net/test_framing.cpp

key-decisions:
  - "Chunked send in node Connection uses atomic drain via PendingMessage::is_chunked flag -- prevents message interleaving (Pitfall 1)"
  - "MAX_FRAME_SIZE kept at 110 MiB (per-frame limit) while MAX_BLOB_DATA_SIZE raised to 500 MiB -- chunked sub-frames are 1 MiB each"
  - "ChunkQueue uses asio::steady_timer signal pattern (same as SseWriter and drain_send_queue) for async backpressure"

patterns-established:
  - "Chunked sub-frame protocol: [flags:0x01][type:1][request_id:4BE][total_size:8BE] header + 1 MiB data sub-frames + zero-length sentinel"
  - "Atomic chunked drain: PendingMessage::is_chunked flag tells drain_send_queue to send header+chunks+sentinel without interleaving"
  - "STREAMING_THRESHOLD == CHUNK_SIZE == 1 MiB: consistent across node and relay"

requirements-completed: [CHUNK-01, CHUNK-02, CHUNK-03, CHUNK-04, CHUNK-08]

# Metrics
duration: 108min
completed: 2026-04-15
---

# Phase 115 Plan 01: UDS Chunked Sub-Frame Protocol Summary

**UDS chunked sub-frame protocol with 14-byte header, 500 MiB size limit, ChunkQueue backpressure primitive, and node-side transparent chunked send/receive**

## Performance

- **Duration:** 108 min
- **Started:** 2026-04-15T02:12:38Z
- **Completed:** 2026-04-15T04:00:26Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- Defined UDS chunked sub-frame protocol: flags byte 0x01 header (14 bytes) + 1 MiB data sub-frames + zero-length sentinel
- Created ChunkQueue backpressure queue (bounded at 4 chunks = 4 MiB) using asio::steady_timer signal pattern
- Raised MAX_BLOB_DATA_SIZE from 100 MiB to 500 MiB (node) and MAX_BODY_SIZE from 110 MiB to 510 MiB (relay)
- Node Connection transparently sends/receives chunked sub-frames for payloads >= 1 MiB
- Atomic chunked drain prevents message interleaving (Pitfall 1 from research)
- 11 unit tests (36 assertions) for chunked_stream.h all pass
- All 209 existing relay tests pass, 698/699 node tests pass (1 pre-existing failure)

## Task Commits

Each task was committed atomically:

1. **Task 1: Define chunked sub-frame protocol + size limits + ChunkQueue** - `0acb21ad` (feat)
2. **Task 2: Node-side chunked reassembly in Connection::message_loop()** - `e44d7dfc` (feat)

## Files Created/Modified
- `relay/core/chunked_stream.h` - Chunked protocol constants, header encode/decode, ChunkQueue backpressure queue
- `relay/tests/test_chunked_stream.cpp` - 11 unit tests for header encode/decode and ChunkQueue
- `relay/tests/CMakeLists.txt` - Added test_chunked_stream target
- `relay/core/uds_multiplexer.h` - Added send_chunked/recv_chunked_reassemble declarations
- `relay/http/http_connection.h` - MAX_BODY_SIZE raised to 510 MiB
- `db/net/framing.h` - MAX_BLOB_DATA_SIZE raised to 500 MiB, added STREAMING_THRESHOLD
- `db/net/connection.h` - Added ReassembledChunked struct, recv_chunked, send_message_chunked, PendingMessage::is_chunked
- `db/net/connection.cpp` - Chunked detection in message_loop, recv_chunked reassembly, send_message_chunked with atomic drain
- `db/tests/net/test_framing.cpp` - Updated framing constant tests for 500 MiB and STREAMING_THRESHOLD

## Decisions Made
- **Atomic chunked drain:** Instead of enqueueing individual sub-frames (which could interleave with other messages), `send_message_chunked` enqueues a single PendingMessage with `is_chunked=true`. The `drain_send_queue` sends all sub-frames atomically within one drain iteration. This prevents AEAD nonce desync (Pitfall 1).
- **MAX_FRAME_SIZE unchanged:** Per-frame limit stays at 110 MiB. Chunked sub-frames are 1 MiB each, well within this. Only MAX_BLOB_DATA_SIZE (logical blob size) increases to 500 MiB.
- **ChunkQueue::close_queue() naming:** Named `close_queue()` instead of `close()` to avoid confusion with socket close() methods commonly in scope.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Updated test_framing.cpp for new MAX_BLOB_DATA_SIZE**
- **Found during:** Task 2 (node build verification)
- **Issue:** Existing tests checked `MAX_BLOB_DATA_SIZE == 100 MiB` and `MAX_FRAME_SIZE > MAX_BLOB_DATA_SIZE`
- **Fix:** Updated to check 500 MiB and STREAMING_THRESHOLD instead
- **Files modified:** db/tests/net/test_framing.cpp
- **Verification:** All 7 framing test cases pass (87 assertions)
- **Committed in:** e44d7dfc (Task 2 commit)

**2. [Rule 1 - Bug] Atomic chunked drain to prevent message interleaving**
- **Found during:** Task 2 (analyzing send_message_chunked correctness)
- **Issue:** Plan's initial approach of calling enqueue_send per sub-frame could interleave chunks with other messages on multiplexed UDS connections
- **Fix:** Added PendingMessage::is_chunked flag; drain_send_queue handles chunked messages atomically (header + all chunks + sentinel in one drain iteration)
- **Files modified:** db/net/connection.h, db/net/connection.cpp
- **Verification:** Node builds and all tests pass
- **Committed in:** e44d7dfc (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (2 Rule 1 bugs)
**Impact on plan:** Both auto-fixes necessary for correctness. The test update was mechanical, the atomic drain fix prevents a real nonce desync bug. No scope creep.

## Issues Encountered
- **Build configuration:** Relay standalone CMake failed due to liboqs FetchContent. Used root CMakeLists.txt unified build instead. No code impact.
- **Pre-existing test failure:** `test_peer_manager.cpp:2740` expects 38 supported types but node reports 39. Not related to Phase 115 changes. Logged to deferred-items.md.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all code is fully functional, no placeholders.

## Next Phase Readiness
- chunked_stream.h ready for Plan 02 (relay UDS multiplexer chunked send/recv implementation)
- UdsMultiplexer::send_chunked/recv_chunked_reassemble declared but not yet implemented (Plan 02)
- Node Connection transparently handles chunked messages -- relay can start sending them
- ChunkQueue primitive ready for use in Plan 02/03 streaming pipelines

---
*Phase: 115-chunked-streaming-for-large-blobs*
*Completed: 2026-04-15*
