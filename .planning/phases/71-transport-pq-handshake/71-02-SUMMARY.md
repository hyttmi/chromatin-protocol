---
phase: 71-transport-pq-handshake
plan: 02
subsystem: transport
tags: [ml-kem-1024, ml-dsa-87, handshake, asyncio, transport, client, python-sdk]

requires:
  - phase: 71-transport-pq-handshake
    plan: 01
    provides: "_framing.py send_raw/recv_raw/send_encrypted/recv_encrypted, HandshakeError/ConnectionError exceptions"
  - phase: 70-crypto-foundation-identity
    provides: "crypto.py sha3_256/aead_encrypt/aead_decrypt, _hkdf.py hkdf_extract/hkdf_expand, identity.py Identity, wire.py encode/decode_transport_message"

provides:
  - "_handshake.py: perform_handshake, encode_auth_payload, decode_auth_payload, derive_session_keys"
  - "_transport.py: Transport class with background reader, request_id dispatch, notification queue"
  - "client.py: ChromatinClient async context manager with handshake timeout, Goodbye, Ping"
  - "ChromatinClient, ConnectionError, HandshakeError exported at package level"

affects: [71-03-PLAN, 72-data-operations]

tech-stack:
  added: []
  patterns: [pq-handshake-initiator, background-reader-dispatch, context-manager-lifecycle]

key-files:
  created:
    - sdk/python/chromatindb/_handshake.py
    - sdk/python/chromatindb/_transport.py
    - sdk/python/chromatindb/client.py
    - sdk/python/tests/test_handshake.py
    - sdk/python/tests/test_client.py
  modified:
    - sdk/python/chromatindb/__init__.py
    - sdk/python/tests/conftest.py

key-decisions:
  - "Transport._writer typed as object to accept both real StreamWriter and CaptureWriter mock"
  - "Notification queue maxsize=1000 prevents unbounded memory growth"
  - "request_id starts at 1, auto-increments internally (D-09)"

patterns-established:
  - "CaptureWriter mock pattern for testing encrypted frame output without network"
  - "In-memory asyncio.StreamReader.feed_data() + feed_encrypted_frame() for reader loop testing"
  - "Relay simulation via asyncio.start_server for full handshake integration tests"

requirements-completed: [XPORT-02, XPORT-03, XPORT-07]

duration: 5min
completed: 2026-03-29
---

# Phase 71 Plan 02: PQ Handshake, Transport, and Client Summary

**ML-KEM-1024 PQ handshake initiator with HKDF session key derivation, background reader transport with request_id dispatch, and ChromatinClient async context manager with 21 unit tests**

## Performance

- **Duration:** 5 min
- **Started:** 2026-03-29T12:23:14Z
- **Completed:** 2026-03-29T12:28:42Z
- **Tasks:** 2 (both TDD)
- **Files modified:** 7

## Accomplishments

- Implemented _handshake.py with 4-step PQ handshake: KEM exchange, HKDF key derivation (empty salt, directional contexts), mutual auth exchange with LE auth payload encoding
- Implemented _transport.py with background reader loop dispatching responses by request_id, routing notifications to asyncio.Queue, auto Ping/Pong, Goodbye shutdown, and ConnectionError propagation
- Implemented client.py with ChromatinClient async context manager, configurable handshake timeout (default 10s), graceful Goodbye on exit, and Ping keep-alive
- ChromatinClient, ConnectionError, HandshakeError exported at package level
- All 107 tests pass (86 existing Phase 70-71.01 + 11 handshake + 10 client/transport)

## Task Commits

Each task was committed atomically:

1. **Task 1: Handshake module with unit tests** - `fc7522e` (feat)
2. **Task 2: Transport, client, and lifecycle unit tests** - `edd10a7` (feat)

## Files Created/Modified

- `sdk/python/chromatindb/_handshake.py` - PQ handshake initiator: KEM keypair generation, encapsulation, HKDF session key derivation with empty salt, auth payload encode/decode (LE endianness), mutual signature verification
- `sdk/python/chromatindb/_transport.py` - Background reader with request_id dispatch to futures, notification queue (request_id=0), auto Ping/Pong, Goodbye handling, ConnectionError propagation, send_lock serialization
- `sdk/python/chromatindb/client.py` - ChromatinClient: connect() class method returning async context manager, __aenter__ with handshake timeout, __aexit__ with Goodbye + stop, ping() method
- `sdk/python/chromatindb/__init__.py` - Added ChromatinClient, ConnectionError, HandshakeError to package exports and __all__
- `sdk/python/tests/test_handshake.py` - 11 tests: auth payload encode/decode (LE, roundtrip, truncation), session key derivation (empty salt, contexts, fingerprint), full 4-step handshake, KemPubkey structure, invalid auth rejection
- `sdk/python/tests/test_client.py` - 10 tests: reader dispatch, notifications, Ping/Pong auto-response, Goodbye shutdown, disconnect error, request_id auto-increment, closed state, context manager, Goodbye on exit, ping roundtrip
- `sdk/python/tests/conftest.py` - Added identity, relay_host, relay_port fixtures for handshake and integration tests

## Decisions Made

- Transport._writer typed as `object` rather than `asyncio.StreamWriter` to accept both real writers and CaptureWriter test mocks without type errors
- Notification queue maxsize=1000 prevents unbounded memory growth; new notifications dropped when full (no blocking)
- request_id starts at 1 and auto-increments per D-09; request_id=0 reserved for notifications
- send_lock (asyncio.Lock) serializes all outgoing encrypted frames to prevent AEAD nonce counter races between reader loop (Pong) and user code (send_request)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- _handshake.py perform_handshake ready for use in integration tests (Plan 03)
- _transport.py Transport ready for data operations in Phase 72
- ChromatinClient.connect() context manager ready for end-to-end relay testing
- conftest.py relay_host/relay_port fixtures ready for live KVM swarm tests
- 107 total tests passing, zero regressions

## Self-Check: PASSED

- [x] sdk/python/chromatindb/_handshake.py exists
- [x] sdk/python/chromatindb/_transport.py exists
- [x] sdk/python/chromatindb/client.py exists
- [x] sdk/python/tests/test_handshake.py exists
- [x] sdk/python/tests/test_client.py exists
- [x] Commit fc7522e exists in git log
- [x] Commit edd10a7 exists in git log

---
*Phase: 71-transport-pq-handshake*
*Completed: 2026-03-29*
