---
phase: 115-chunked-streaming-for-large-blobs
plan: 02
subsystem: networking
tags: [chunked-streaming, uds, aead, relay, multiplexer, variant]

# Dependency graph
requires:
  - phase: 115-01
    provides: chunked_stream.h (encode/decode helpers, CHUNKED_BEGIN, CHUNK_SIZE), send_chunked/recv_chunked_reassemble declarations in uds_multiplexer.h
provides:
  - UdsMultiplexer::send_chunked() -- atomic chunked send over encrypted UDS
  - UdsMultiplexer::send_chunked_msg() -- public API for queueing chunked sends
  - UdsMultiplexer::recv_chunked_reassemble() -- chunked receive with size validation
  - Chunked frame detection in read_loop() (CHUNKED_BEGIN 0x01 prefix)
  - ChunkedSendJob/SendItem variant for drain queue atomicity
affects: [115-03-PLAN, 115-04-PLAN]

# Tech tracking
tech-stack:
  added: []
  patterns: [send-queue-variant-dispatch, chunked-frame-detection-in-read-loop]

key-files:
  created:
    - relay/tests/test_uds_chunked.cpp
  modified:
    - relay/core/uds_multiplexer.h
    - relay/core/uds_multiplexer.cpp
    - relay/tests/CMakeLists.txt

key-decisions:
  - "SendItem variant (std::variant<vector<uint8_t>, ChunkedSendJob>) replaces raw deque<vector<uint8_t>> for send queue -- enables atomic chunked send without separate entry point"
  - "CHUNKED_BEGIN (0x01) detection before TransportCodec::decode() -- FlatBuffer messages never start with 0x01 (root table offset is always >= 4)"
  - "Chunked reassembly failure triggers full disconnect cleanup (same as recv_encrypted failure) -- partial chunked state cannot be recovered"

patterns-established:
  - "Send queue variant dispatch: drain_send_queue uses std::get_if to handle raw vs chunked items, keeping the entire chunked sequence atomic"
  - "Chunked frame detection: first byte 0x01 check before TransportCodec::decode() in read_loop()"

requirements-completed: [CHUNK-01, CHUNK-02]

# Metrics
duration: 13min
completed: 2026-04-15
---

# Phase 115 Plan 02: Relay UDS Chunked Send/Recv Summary

**Chunked send/recv in UdsMultiplexer with SendItem variant dispatch, per-chunk AEAD, and atomic drain queue integration**

## Performance

- **Duration:** 13 min
- **Started:** 2026-04-15T04:04:34Z
- **Completed:** 2026-04-15T04:17:45Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Implemented send_chunked() with header + 1 MiB data chunks + zero-length sentinel, all through per-chunk AEAD via existing send_encrypted()
- Implemented recv_chunked_reassemble() with 500 MiB size validation and sentinel loop termination
- Added CHUNKED_BEGIN (0x01) detection in read_loop() for transparent chunked vs non-chunked dispatch
- ChunkedSendJob/SendItem variant ensures chunked sequences are atomic in drain_send_queue (no interleaving)
- 5 new unit tests (63 assertions) covering protocol simulation with AEAD, nonce counter consumption, and variant construction
- All 209 existing relay tests pass (2485 assertions), plus 11 chunked_stream tests (36 assertions)

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement send_chunked() in UdsMultiplexer** - `ec177e56` (feat)
2. **Task 2: Implement recv_chunked_reassemble() + chunked detection + tests** - `baaf8b44` (test)

## Files Created/Modified
- `relay/core/uds_multiplexer.h` - Added ChunkedSendJob struct, SendItem variant, send_chunked_msg() public API, updated send_queue_ type
- `relay/core/uds_multiplexer.cpp` - Implemented send_chunked(), recv_chunked_reassemble(), send_chunked_msg(), updated drain_send_queue() for variant dispatch, added CHUNKED_BEGIN detection in read_loop()
- `relay/tests/test_uds_chunked.cpp` - 5 TEST_CASEs: flag detection, header round-trip, AEAD protocol simulation, nonce counter verification, SendItem variant construction
- `relay/tests/CMakeLists.txt` - Added test_uds_chunked target

## Decisions Made
- **SendItem variant over separate entry point:** Using `std::variant<std::vector<uint8_t>, ChunkedSendJob>` for send queue items allows chunked sends to go through the same drain_send_queue() mechanism as regular sends, ensuring atomicity without a separate code path.
- **CHUNKED_BEGIN detection position:** Checking `(*msg)[0] == 0x01` before TransportCodec::decode() is safe because FlatBuffer messages always start with a root table offset (little-endian uint32) whose first byte is >= 4 for any non-trivial message.
- **Chunked failure = disconnect:** When recv_chunked_reassemble() fails mid-sequence, the AEAD nonce counters are desynchronized. Full disconnect cleanup (same as recv_encrypted failure) is the only safe recovery.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- **Catch2 macro template comma:** `std::holds_alternative<Type>()` inside REQUIRE() macro causes parse errors due to comma in template arguments. Fixed by extracting to `bool` variables before assertion.
- **CMake ccache configuration:** Initial build failed with error 127 (ccache not found). Reconfigured without `CMAKE_CXX_COMPILER_LAUNCHER`.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all code is fully functional, no placeholders.

## Next Phase Readiness
- UdsMultiplexer fully supports chunked send and receive for large payloads
- send_chunked_msg() ready for Plan 03 HTTP streaming handlers to use
- recv_chunked_reassemble() ready for Plan 03 HTTP response streaming
- Plan 03 can now wire HTTP upload streaming -> send_chunked_msg() and recv response -> HTTP chunked transfer encoding

---
*Phase: 115-chunked-streaming-for-large-blobs*
*Completed: 2026-04-15*
