---
phase: 04-networking
plan: 03
status: complete
started: "2026-03-04"
completed: "2026-03-04"
duration: ~15min
---

# Plan 04-03: TCP Server + Connection Manager

## What Was Built
Implemented the full TCP connection lifecycle using Asio C++20 coroutines: Connection class handles PQ handshake over TCP, encrypted frame IO with counter-based nonces, and message loop with Ping/Pong/Goodbye. Server class manages inbound accept loop, outbound bootstrap connections with exponential backoff reconnect, and SIGINT/SIGTERM graceful drain.

## Key Files

### Created
- `src/net/connection.h` -- Connection class (shared_ptr, enable_shared_from_this)
- `src/net/connection.cpp` -- Full handshake over TCP, encrypted IO, message loop, graceful close
- `src/net/server.h` -- Server class with accept loop, signal handling, drain
- `src/net/server.cpp` -- Accept loop, outbound connections, reconnect with backoff, graceful shutdown
- `tests/net/test_connection.cpp` -- 4 test cases: handshake, auth verification, data exchange, goodbye
- `tests/net/test_server.cpp` -- 4 test cases: accept, drain state, bootstrap connect, full handshake

### Modified
- `CMakeLists.txt` -- Added connection.cpp, server.cpp, test files

## Architecture

### Connection
- `create_inbound()` / `create_outbound()` factory methods (private constructor)
- `run()` coroutine: do_handshake() -> message_loop() -> cleanup
- Raw frame IO (send_raw/recv_raw): 4-byte BE length prefix + payload, used for KEM exchange
- Encrypted frame IO (send_encrypted/recv_encrypted): AEAD encrypt -> send_raw, recv_raw -> AEAD decrypt
- Handshake performs KEM exchange + mutual auth inline (not using HandshakeInitiator/Responder classes which are designed for in-memory testing)
- Message loop dispatches Ping->Pong, handles Goodbye, invokes callback for Data
- `use_nothrow = asio::as_tuple(asio::use_awaitable)` for non-throwing async ops

### Server
- `start()`: binds acceptor, spawns accept_loop, connects to bootstrap_peers, registers signal handler
- `accept_loop()`: co_await acceptor.async_accept in loop, creates inbound Connections
- `connect_to_peer()`: resolves + connects, creates outbound Connection, on_close triggers reconnect
- `reconnect_loop()`: exponential backoff 1s -> 2s -> 4s -> ... -> 60s cap
- `drain()`: sends goodbye to all authenticated connections, 5s timeout, force closes remainder, ioc_.stop()
- Signal handling: first SIGTERM/SIGINT -> stop(), second -> std::_Exit(1)

## Bug Fixed During Implementation
- `send_encrypted` initially used `write_frame()` which adds an inner length prefix, then `send_raw()` added another outer prefix. The receiver's `recv_raw()` stripped only the outer prefix, leaving the inner prefix as part of the ciphertext, causing AEAD decrypt failure. Fixed by encrypting directly with `crypto::AEAD::encrypt()` in `send_encrypted()` instead of using `write_frame()`.

## Decisions Made
- Connection does handshake inline rather than delegating to HandshakeInitiator/HandshakeResponder (those classes use write_frame/read_frame which add their own framing; Connection uses send_raw/recv_raw for the transport layer)
- Test ports use fixed high numbers (44201-44205) since Server doesn't expose the bound port
- Tests use `ioc.run_for()` for bounded execution

## Test Results
- 8 new test cases, 14 assertions -- all passing
- 128 total test cases, 493 total assertions -- no regressions

## Self-Check: PASSED
- [x] Server listens on configurable bind address
- [x] Server connects to bootstrap peers
- [x] Inbound connections complete PQ handshake
- [x] Outbound connections complete PQ handshake
- [x] Encrypted data exchange works after handshake
- [x] Goodbye sends properly and is detected by peer
- [x] Server stop triggers draining state
- [x] Graceful shutdown sends goodbye and drains within timeout
- [x] Reconnect with exponential backoff on disconnect
- [x] Second signal during drain forces immediate exit (code path exists)
- [x] All existing tests pass (no regressions)
