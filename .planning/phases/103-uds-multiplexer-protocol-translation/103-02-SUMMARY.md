---
phase: 103-uds-multiplexer-protocol-translation
plan: 02
subsystem: relay
tags: [uds, aead, hkdf, flatbuffers, json-translation, websocket-binary, request-routing, trusted-hello]

requires:
  - phase: 103-uds-multiplexer-protocol-translation
    plan: 01
    provides: "AEAD crypto, TransportCodec, RequestRouter, hex/base64/endian utilities"
provides:
  - "UdsMultiplexer with TrustedHello + HKDF + AEAD handshake (relay/core/uds_multiplexer.h)"
  - "Table-driven JSON<->binary translator for all 38 message types (relay/translate/translator.h)"
  - "FlatBuffer blob encode/decode (relay/wire/blob_codec.h)"
  - "WsSession binary WebSocket frame support (send_binary, OPCODE_BINARY)"
  - "Full request pipeline: JSON parse -> translate -> register_request -> UDS forward -> response route"
affects: [104-subscription-notification-fanout, 105-rate-limiting-graceful-shutdown]

tech-stack:
  added: []
  patterns: [binary-marker-prefix for WS opcode routing through session queue, compound-response custom decoders, table-driven field encoding/decoding]

key-files:
  created:
    - relay/core/uds_multiplexer.h
    - relay/core/uds_multiplexer.cpp
    - relay/wire/blob_codec.h
    - relay/wire/blob_codec.cpp
    - relay/translate/translator.h
    - relay/translate/translator.cpp
    - relay/tests/test_translator.cpp
  modified:
    - relay/translate/json_schema.h
    - relay/translate/json_schema.cpp
    - relay/ws/ws_session.h
    - relay/ws/ws_session.cpp
    - relay/ws/ws_acceptor.h
    - relay/ws/ws_acceptor.cpp
    - relay/relay_main.cpp
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt

key-decisions:
  - "Binary WS frame via marker-prefix: send_binary() prepends 0x02 byte, write_frame() detects it. Avoids changing Session queue signature while maintaining serialization."
  - "Compound response types (10 total) use custom decode helpers rather than flat FieldSpec -- preserves table-driven spirit for flat types while handling variable-length and status-dependent wire formats correctly."
  - "StorageStatusResponse, NamespaceStatsResponse, NodeInfoResponse marked as compound in addition to the 7 types in the plan -- their actual node wire formats don't match flat FieldSpec assumptions."

patterns-established:
  - "relay/core/ for stateful connection management (UdsMultiplexer)"
  - "relay/translate/ for stateless JSON<->binary translation functions"
  - "Compound response decode helpers as static functions in translator.cpp"
  - "WsSession accepts UdsMultiplexer* and RequestRouter* as nullable pointers for optional forwarding"

requirements-completed: [MUX-01, PROT-01, PROT-04]

duration: 20min
completed: 2026-04-10
---

# Phase 103 Plan 02: UDS Multiplexer, Protocol Translator & WsSession Integration Summary

**Single multiplexed UDS connection with TrustedHello+AEAD handshake, table-driven JSON<->binary translator for all 38 types, and full request pipeline wired through WsSession -- 161 tests (1136 assertions)**

## Performance

- **Duration:** 20 min
- **Started:** 2026-04-10T03:05:39Z
- **Completed:** 2026-04-10T03:25:00Z
- **Tasks:** 2
- **Files created:** 7
- **Files modified:** 9

## Accomplishments
- UdsMultiplexer with full TrustedHello + HKDF-SHA256 key derivation + ChaCha20-Poly1305 AEAD encrypted UDS connection to node, with jittered exponential backoff reconnect
- Table-driven translator handles all 38 message types: 18 flat via FieldSpec iteration, 10 compound via custom decode helpers, 3 FlatBuffer via blob_codec, 7 request-only
- WsSession AUTHENTICATED path: JSON parse -> type filter -> translate -> register request -> UDS forward, with fire-and-forget bypass for Ping/Pong/Goodbye
- ReadResponse and BatchReadResponse sent as binary WebSocket frames (OPCODE_BINARY)
- Schema gaps from Phase 102 fixed: ListRequest since_seq, NamespaceListRequest cursor+limit, BatchReadRequest field order, 10 compound response types
- All 161 relay tests pass (1136 assertions) -- 126 existing + 35 new translator tests

## Task Commits

Each task was committed atomically:

1. **Task 1: UdsMultiplexer with TrustedHello handshake and AEAD encryption** - `1341879` (feat)
2. **Task 2: Table-driven translator, schema fixes, and WsSession integration** - `6c0aa21` (feat)

## Files Created/Modified
- `relay/core/uds_multiplexer.h` - UdsMultiplexer class: start(), send(), is_connected(), TrustedHello handshake, AEAD crypto, response routing
- `relay/core/uds_multiplexer.cpp` - Full implementation: connect_loop, do_handshake, read_loop, drain_send_queue, cleanup_loop, route_response
- `relay/wire/blob_codec.h` - DecodedBlob struct, encode_blob(), decode_blob()
- `relay/wire/blob_codec.cpp` - FlatBuffer blob encode/decode using blob_generated.h
- `relay/translate/translator.h` - TranslateResult struct, json_to_binary(), binary_to_json(), is_binary_response()
- `relay/translate/translator.cpp` - Table-driven encoder/decoder, 10 compound response decode helpers, FlatBuffer special cases
- `relay/translate/json_schema.h` - Added is_compound to MessageSchema, fixed ListRequest/NamespaceListRequest/BatchReadRequest fields
- `relay/translate/json_schema.cpp` - Updated all 40 schema entries with is_compound flag
- `relay/ws/ws_session.h` - Added send_binary(), made send_json() public, UdsMultiplexer*/RequestRouter* members
- `relay/ws/ws_session.cpp` - AUTHENTICATED path with full forwarding pipeline, binary frame support, session cleanup
- `relay/ws/ws_acceptor.h` - Added UdsMultiplexer*/RequestRouter* params
- `relay/ws/ws_acceptor.cpp` - Pass UdsMultiplexer/RequestRouter to WsSession::create()
- `relay/relay_main.cpp` - Construct RequestRouter + UdsMultiplexer, pass to WsAcceptor
- `relay/tests/test_translator.cpp` - 35 test cases covering request encoding, response decoding, FlatBuffer types, compound types, roundtrips, schema fixes

## Decisions Made
- Used binary marker prefix (0x02 byte) for WsSession::send_binary() to distinguish binary from text frames in the session queue, avoiding changes to core::Session
- Marked 10 response types as compound (plan specified 7) -- NodeInfoResponse, NamespaceStatsResponse, and StorageStatusResponse also need custom decode due to status-dependent or multi-field wire formats that don't match flat FieldSpec assumptions
- UdsMultiplexer uses raw this pointer (not shared_ptr) since it lives on the stack in main() and outlives all coroutines

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed missing oqs/sha3.h include**
- **Found during:** Task 1 (UdsMultiplexer build)
- **Issue:** OQS_SHA3_sha3_256 not found -- liboqs 0.15 requires explicit sha3.h include alongside oqs.h
- **Fix:** Added `#include <oqs/sha3.h>` to uds_multiplexer.cpp
- **Files modified:** relay/core/uds_multiplexer.cpp
- **Committed in:** 1341879

**2. [Rule 2 - Missing Critical] Added StorageStatusResponse, NamespaceStatsResponse, NodeInfoResponse as compound types**
- **Found during:** Task 2 (translator implementation)
- **Issue:** Plan only marked 7 compound types, but these 3 also have wire formats incompatible with flat FieldSpec decode (status-dependent fields, variable-length strings, mismatched field definitions)
- **Fix:** Marked as is_compound=true in schema, added custom decode helpers in translator.cpp
- **Files modified:** relay/translate/json_schema.cpp, relay/translate/translator.cpp
- **Committed in:** 6c0aa21

---

**Total deviations:** 2 auto-fixed (1 bug, 1 missing critical)
**Impact on plan:** Both fixes necessary for correctness. No scope creep.

## Issues Encountered
- Worktree was behind master (missing Plan 01 output). Resolved with fast-forward merge before starting.
- Identity object went out of scope in relay_main.cpp try block. Fixed by moving declaration before try block.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Full request pipeline operational: client JSON -> translate -> UDS forward -> translate response -> route to client
- Ready for Phase 104: subscription aggregation, notification fan-out, UDS reconnect with subscription replay
- UdsMultiplexer basic reconnect loop is in place; Phase 104 adds subscription state preservation across reconnects

## Self-Check: PASSED

- All 7 created files verified present on disk
- Both task commits (1341879, 6c0aa21) verified in git log
- All 161 relay tests pass (1136 assertions)

---
*Phase: 103-uds-multiplexer-protocol-translation*
*Completed: 2026-04-10*
