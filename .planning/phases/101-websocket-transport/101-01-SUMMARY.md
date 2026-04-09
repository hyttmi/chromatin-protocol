---
phase: 101-websocket-transport
plan: 01
subsystem: relay
tags: [websocket, rfc6455, openssl, tls, asio-ssl, framing, handshake]

# Dependency graph
requires:
  - phase: 100-cleanup-foundation
    provides: "Relay scaffold with Session send queue, RelayConfig, standalone CMake"
provides:
  - "WS frame encode/decode/mask/unmask library (relay/ws/ws_frame.h)"
  - "HTTP upgrade handshake with RFC 6455 accept key (relay/ws/ws_handshake.h)"
  - "FragmentAssembler for multi-frame message reassembly"
  - "Session write callback injection point for WsSession"
  - "TLS config fields (cert_path/key_path) with validation"
  - "OpenSSL 3.3+ linked to relay, Asio SSL separate compilation unit"
affects: [101-02, 102-json-auth, 103-translation, 104-pubsub]

# Tech tracking
tech-stack:
  added: [OpenSSL 3.3+ (system, for TLS + SHA-1/Base64)]
  patterns: [WS framing as pure encode/decode layer, WriteCallback injection for transport abstraction, asio_ssl.cpp separate compilation]

key-files:
  created:
    - relay/ws/ws_frame.h
    - relay/ws/ws_frame.cpp
    - relay/ws/ws_handshake.h
    - relay/ws/ws_handshake.cpp
    - relay/asio_ssl.cpp
    - relay/tests/test_ws_frame.cpp
    - relay/tests/test_ws_handshake.cpp
  modified:
    - relay/core/session.h
    - relay/core/session.cpp
    - relay/config/relay_config.h
    - relay/config/relay_config.cpp
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt
    - relay/tests/test_relay_config.cpp

key-decisions:
  - "OpenSSL EVP API for SHA-1 + EVP_EncodeBlock for Base64 in accept key computation"
  - "WriteCallback uses span<const uint8_t> (not string ref) since WS frames are binary"
  - "FragmentAssembler checks text vs binary max separately (1 MiB vs 110 MiB)"

patterns-established:
  - "WS framing as stateless encode/decode functions in relay/ws/ namespace"
  - "FragmentAssembler: feed() returns AssembledMessage with result enum for clear state machine"
  - "Session::WriteCallback injection pattern for transport layer abstraction"
  - "asio_ssl.cpp separate compilation unit for ASIO_SEPARATE_COMPILATION projects"

requirements-completed: [TRANS-01, TRANS-02, TRANS-03, TRANS-04]

# Metrics
duration: 18min
completed: 2026-04-09
---

# Phase 101 Plan 01: WebSocket Framing & Handshake Summary

**RFC 6455 WS framing library with all server-side frame operations, HTTP upgrade handshake with OpenSSL SHA-1/Base64, Session write callback injection, TLS config fields, and OpenSSL build integration -- 56 relay tests passing**

## Performance

- **Duration:** 18 min
- **Started:** 2026-04-09T13:19:11Z
- **Completed:** 2026-04-09T13:37:00Z
- **Tasks:** 2
- **Files modified:** 14

## Accomplishments
- Complete RFC 6455 server-side WS framing: encode (3 length modes), parse (masked/unmasked), XOR mask/unmask, fragment reassembly with control frame interleaving
- HTTP upgrade handshake: accept key computation verified against RFC 6455 spec example, request parsing with case-insensitive headers, 101 and 426 response builders
- Session write callback injection (WriteCallback typedef + set_write_callback) preserving backward compatibility with existing test infrastructure
- TLS config: cert_path/key_path with auto-detect (tls_enabled()), both-or-neither validation, file existence check
- OpenSSL 3.3+ linked to relay only (not db/), Asio SSL separate compilation unit

## Task Commits

Each task was committed atomically:

1. **Task 1: WebSocket framing library, HTTP upgrade handshake, and unit tests** - `2b8b25a` (test: TDD RED) + `82befbc` (feat: TDD GREEN)
2. **Task 2: Session write callback, TLS config fields, OpenSSL build integration** - `88ac2f2` (feat)

## Files Created/Modified
- `relay/ws/ws_frame.h` - Frame constants, FrameHeader struct, encode/decode/mask functions, FragmentAssembler class
- `relay/ws/ws_frame.cpp` - Full implementation of all framing operations
- `relay/ws/ws_handshake.h` - UpgradeRequest/ParseResult structs, compute_accept_key, parse/build functions
- `relay/ws/ws_handshake.cpp` - OpenSSL EVP SHA-1 + Base64 accept key, HTTP header parsing
- `relay/asio_ssl.cpp` - Asio SSL separate compilation unit
- `relay/core/session.h` - WriteCallback typedef, set_write_callback method, write_cb_ member
- `relay/core/session.cpp` - Callback delegation in do_send(), set_write_callback implementation
- `relay/config/relay_config.h` - cert_path, key_path fields, tls_enabled() method
- `relay/config/relay_config.cpp` - TLS field loading and both-or-neither + file existence validation
- `relay/CMakeLists.txt` - find_package(OpenSSL 3.3 REQUIRED), new sources, SSL linkage
- `relay/tests/CMakeLists.txt` - New test source files
- `relay/tests/test_ws_frame.cpp` - 26 tests: encode/decode all length modes, mask/unmask, fragment assembly
- `relay/tests/test_ws_handshake.cpp` - 11 tests: accept key, upgrade parsing, response builders
- `relay/tests/test_relay_config.cpp` - 5 new tests for TLS config fields

## Decisions Made
- Used OpenSSL EVP API (EVP_DigestInit_ex/EVP_DigestUpdate/EVP_DigestFinal_ex + EVP_EncodeBlock) for SHA-1 and Base64 -- cleaner than raw OpenSSL functions, aligned with OpenSSL 3.x deprecation schedule
- WriteCallback parameter is `span<const uint8_t>` not `const string&` because WS frames are binary data
- FragmentAssembler returns close_code extracted from close frame payload for convenience

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## Known Stubs

None - all functions fully implemented, no placeholder data.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- WS framing and handshake modules ready for Plan 02 (WsSession, WsAcceptor, SessionManager)
- WsSession will use encode_frame/parse_frame_header for read/write, parse_upgrade_request/build_upgrade_response for connection setup
- Session write callback injection point ready for WsSession to wire WebSocket frame writing
- OpenSSL linked and ssl::context available for TLS stream construction

## Self-Check: PASSED

All 14 key files exist. All 3 commit hashes verified. SUMMARY.md created.

---
*Phase: 101-websocket-transport*
*Completed: 2026-04-09*
