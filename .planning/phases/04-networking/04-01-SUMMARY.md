---
phase: 04-networking
plan: 01
status: complete
started: "2026-03-04"
completed: "2026-03-04"
duration: ~12min
---

# Plan 04-01: Asio + Transport Schema + Encrypted Framing

## What Was Built
Added Standalone Asio 1.38.0 to the build system via FetchContent, defined the FlatBuffer transport message schema with 9 message types (None, KemPubkey, KemCiphertext, AuthSignature, AuthPubkey, Ping, Pong, Goodbye, Data), and implemented the length-prefixed encrypted framing layer with counter-based nonces.

## Key Files

### Created
- `schemas/transport.fbs` -- FlatBuffer transport message schema
- `src/wire/transport_generated.h` -- Generated FlatBuffer header
- `src/net/protocol.h` / `src/net/protocol.cpp` -- TransportCodec encode/decode
- `src/net/framing.h` / `src/net/framing.cpp` -- Frame write/read with AEAD encryption
- `tests/net/test_protocol.cpp` -- Protocol codec tests
- `tests/net/test_framing.cpp` -- Framing + nonce + integration tests

### Modified
- `CMakeLists.txt` -- Added Asio via asiocmake, transport schema compilation, net source files

## Test Results
- 7 new test cases, 104 assertions -- all passing
- 115 total test cases, 444 total assertions -- no regressions

## Decisions Made
- FlatBuffers C-style enum (TransportMsgType_X) -- required by flatc codegen
- Added `None = 0` to enum for FlatBuffer default value compatibility
- Empty associated data for AEAD (counter-nonce provides uniqueness)
- Frame reader throws on structural errors (too short, too large), returns nullopt on decrypt failure

## Self-Check: PASSED
- [x] Asio compiles with C++20 coroutines
- [x] Transport schema round-trips all message types
- [x] Frame write/read round-trips with encryption
- [x] Counter-based nonces produce unique values
- [x] Max frame size enforced (16 MB)
- [x] All existing tests pass
