---
phase: 107-message-type-verification
plan: 01
subsystem: testing
tags: [websocket, smoke-test, ml-dsa-87, sha3-256, flatbuffers, binary-frames, e2e]

# Dependency graph
requires:
  - phase: 106-bug-fixes
    provides: Corrected compound decoders, relay_smoke_test foundation (13 tests), relay_uds_tap tool
provides:
  - Extended smoke test covering all 38 relay-allowed message types
  - ML-DSA-87 signed blob construction in test tool
  - Binary WS frame handling for ReadResponse/BatchReadResponse
  - Full data write/read/delete roundtrip verification
affects: [108-performance-benchmarks, 109-source-exclusion, 110-operational-endpoints]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "build_signing_input(): SHA3-256 incremental hash with LE-endian ttl/timestamp for blob signing in test tools"
    - "ws_recv_frame(): generic WS frame receiver handling text(0x01), binary(0x02), ping/pong, and close"
    - "send_recv_frame(): lambda for binary WS frame detection in request/response pairs"
    - "Notification capture pattern: receive after write_ack to collect async notification"

key-files:
  created: []
  modified:
    - tools/relay_smoke_test.cpp

key-decisions:
  - "Task 1+2 implemented atomically in single edit since all changes target one file"
  - "Notification captured by reading frame after write_ack -- single-threaded blocking approach"
  - "Goodbye sent as final test before unsubscribe since it may trigger disconnect"

patterns-established:
  - "Signed blob test pattern: build_signing_input + id.sign(digest) + make_data_message JSON"
  - "Binary frame test pattern: send_recv_frame -> check opcode -> json::parse payload"

requirements-completed: [E2E-01]

# Metrics
duration: 5min
completed: 2026-04-11
---

# Phase 107 Plan 01: Message Type Verification Summary

**Extended relay_smoke_test from 13 to 31 tests covering all 38 relay-allowed message types with ML-DSA-87 signed blob write, binary WS frame handling, notification delivery, fire-and-forget, and error paths**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-11T10:36:39Z
- **Completed:** 2026-04-11T10:41:16Z
- **Tasks:** 3 (2 auto + 1 live verification)
- **Files modified:** 5 (1 smoke test + 3 relay translator + 1 relay test)

## Accomplishments
- Full data write chain: data(8)->write_ack(30), read_request(31)->read_response(32) with data match, metadata_found, exists_found, batch_exists, batch_read, delete->delete_ack
- Binary WS frame handler (WsFrame struct + ws_recv_frame) for ReadResponse and BatchReadResponse opcode 0x02 frames
- ML-DSA-87 blob signing with SHA3-256(namespace||data||ttl_LE32||timestamp_LE64) -- LITTLE-endian for ttl/timestamp per protocol spec
- Notification(21) verification after subscribe + write, with correct namespace and hash matching
- Fire-and-forget tests (ping, pong, goodbye) proving connection stability
- Error path tests: read nonexistent (status=0), metadata not-found (found=false), blocked type (error+code), stats nonexistent namespace
- All 38 types covered: 16 request/response pairs (32 types), 2 relay-intercepted (subscribe/unsubscribe), 3 fire-and-forget (ping/pong/goodbye), 1 triggered notification
- 2 node signal types (storage_full=22, quota_exceeded=25) acknowledged as untestable without special node state

## Task Commits

1. **Task 1+2: Add binary WS frame handler, blob signing, data write chain, remaining queries, fire-and-forget, and error path tests** - `e380ced` (feat)
2. **Task 3 relay bug fixes discovered during live testing:**
   - `df8ef9a` - fix(relay): Delete(17) FlatBuffer encoding, DeleteAck(18) fields, TimeRange(57) missing end_ts, smoke test socket timeout
   - `6879431` - fix(107): tombstone format for delete (DEADBEEF + target hash)
3. **Live verification:** 31/31 PASS against live node+relay

## Files Created/Modified
- `tools/relay_smoke_test.cpp` - Extended from 609 to ~970 lines: WsFrame struct, ws_recv_frame(), build_signing_input(), make_data_message(), full data write chain, remaining query types, fire-and-forget, error paths, SO_RCVTIMEO safety, tombstone delete format
- `relay/translate/json_schema.h` - Fixed DeleteAck fields (added hash+seq_num), TimeRangeRequest (added until/end_ts)
- `relay/translate/json_schema.cpp` - Changed Delete(17) to FlatBuffer type
- `relay/translate/translator.cpp` - Handle Delete(17) same as Data(8) in json_to_binary
- `relay/tests/test_translator.cpp` - Updated Delete/TimeRange/DeleteAck tests

## Decisions Made
- Implemented Tasks 1 and 2 together in a single atomic edit since all changes target the same file and are interdependent (data write chain uses helpers defined for Task 1)
- Used incremental SHA3-256 from oqs/sha3.h (OQS_SHA3_sha3_256_inc_*) matching db/wire/codec.cpp exactly
- Notification captured immediately after write_ack frame consumption -- blocking socket approach assumes notification arrives next
- Goodbye placed as final test before unsubscribe per RESEARCH.md recommendation (may trigger disconnect)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Tasks 1 and 2 merged into single commit**
- **Found during:** Task 2 (already implemented during Task 1)
- **Issue:** All Task 2 content (remaining queries, fire-and-forget, error paths, section headers) was naturally included during the Task 1 edit since it's all in the same file and follows a continuous flow
- **Fix:** Committed all changes as Task 1; no separate Task 2 commit needed
- **Files modified:** tools/relay_smoke_test.cpp
- **Verification:** All acceptance criteria for both Task 1 and Task 2 verified
- **Committed in:** e380ced

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** No scope change. Same total code, just committed as one unit instead of two.

## Issues Encountered

### 3 Relay Translation Bugs Found and Fixed

**1. Delete(17) encoding** — Relay sent flat [ns:32][hash:32] but node expects FlatBuffer-encoded blob (calls `wire::decode_blob`). Node threw exception, sent no response, client hung forever. Fix: mark Delete as FlatBuffer type, encode same as Data(8).

**2. DeleteAck(18) decoding** — Schema had only `{status}` but node sends [hash:32][seq_num:8BE][status:1] = 41 bytes (same format as WriteAck). Fix: added hash and seq_num fields.

**3. TimeRangeRequest(57)** — Schema had `{ns, since, limit}` = 44 bytes but node requires `{ns, start_ts, end_ts, limit}` = 52 bytes. Node rejected with "too short", sent no response. Fix: added `until` field.

**4. Delete tombstone format** — Node requires blob data = `[0xDE,0xAD,0xBE,0xEF][target_hash:32]` (36 bytes), not empty data. Fix: construct proper tombstone data with magic + target hash.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- 31/31 smoke tests passing against live node+relay
- All 38 message types verified (E2E-01 complete)
- Run via `/tmp/chromatindb-test/run-smoke.sh` for regression testing
- 223 relay unit tests passing (2522 assertions)

## Self-Check: PASSED

- FOUND: tools/relay_smoke_test.cpp
- FOUND: commit e380ced
- FOUND: 107-01-SUMMARY.md

---
*Phase: 107-message-type-verification*
*Completed: 2026-04-11*
