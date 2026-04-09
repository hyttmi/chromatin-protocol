# Phase 101: WebSocket Transport - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Relay accepts WebSocket connections over TLS (WSS) and plain TCP (WS) with correct RFC 6455 framing and SIGHUP-reloadable TLS. Hand-rolled WebSocket implementation on standalone Asio + system OpenSSL. No auth (Phase 102), no JSON translation (Phase 103), no UDS multiplexing (Phase 103), no pub/sub (Phase 104).

</domain>

<decisions>
## Implementation Decisions

### WS/WSS Configuration
- **D-01:** Auto-detect WS vs WSS from `cert_path`/`key_path` config fields. Fields omitted -> plain WS. Fields present -> must load successfully or relay refuses to start (error exit, no silent fallback to WS).
- **D-02:** Only `cert_path` + `key_path` added to RelayConfig. All transport settings (timeouts, frame sizes, ping intervals) are constexpr constants. Minimal config.
- **D-03:** Plain WS mode: startup warning encouraging TLS. No bind restriction. Blob data is already PQ-encrypted at application layer.

### Session Architecture
- **D-04:** WsSession (ws/) wraps Session (core/). WsSession owns TLS+WS stream and lifecycle. Session provides send queue + drain. do_send() calls back to WsSession::write_frame() via injected callback. Clean separation: ws/ handles framing, core/ handles queuing.
- **D-05:** Separate SessionManager class (not embedded in WsAcceptor). Owns `unordered_map<uint64_t, shared_ptr<WsSession>>` session map. Needed by Phase 104 for notification fan-out.
- **D-06:** Session added to SessionManager after WS upgrade completes. Failed TLS handshake or upgrade attempts are logged and dropped -- no session object created.

### WebSocket Framing
- **D-07:** Accept + reassemble fragmented client frames per RFC 6455 SS5.4. Reassemble up to max message size, reject if exceeded.
- **D-08:** Support both text (0x1) and binary (0x2) opcodes from Phase 101. Text for JSON messages (future Phase 102+), binary prepared for Phase 103 large blob data.
- **D-09:** Max frame sizes: 1 MiB text, 110 MiB binary. Text frames are for JSON control messages (small). Binary frames are for blob data (up to 100 MiB + overhead).
- **D-10:** Malformed frames (bad opcode, unmasked, oversized) -> Close(1002 Protocol Error), log at warn level, disconnect.
- **D-11:** Silently ignore WS extension requests (permessage-deflate etc.) -- omit Sec-WebSocket-Extensions from 101 response. Per RFC, omission means no extensions negotiated.
- **D-12:** Non-upgrade HTTP requests -> 426 Upgrade Required with `Upgrade: websocket` header.

### Connection Lifecycle
- **D-13:** 5-second timeout for TLS handshake + WS upgrade (combined). Prevents slowloris-style resource exhaustion.
- **D-14:** 30-second idle timeout after WS upgrade completion (pre-auth placeholder). Phase 102 replaces this with auth deadline.
- **D-15:** Server-initiated WebSocket Ping every 30 seconds. Disconnect if no Pong within 60 seconds. Same pattern as node's bidirectional keepalive (Phase 83).
- **D-16:** RFC-compliant Close frame handling. Client Close -> echo with same status code, then TCP close. Relay-initiated Close -> send with appropriate status code, wait up to 5s for client echo, then TCP close.
- **D-17:** Hardcoded 1024 max concurrent connections (safety limit). Phase 102 SESS-03 makes this configurable.

### TLS
- **D-18:** TLS 1.3 only. No TLS 1.2 or older.
- **D-19:** SIGHUP TLS reload via `shared_ptr<ssl::context>` swap. New connections get new context. Existing connections keep old context (ref-counted lifetime). Reload failure -> keep old context + log error.
- **D-20:** System OpenSSL 3.3+ (`find_package(OpenSSL 3.3 REQUIRED)`). wolfSSL rejected (GPLv2 blocks closed-source relay without commercial license). LibreSSL considered but OpenSSL is sufficient and universal. Not FetchContent'd -- system package only.

### Threading
- **D-21:** Thread pool: `std::thread::hardware_concurrency()` threads running `ioc.run()`. Per-connection strand via natural Asio coroutine chain (implicit serialization per session). Different clients run concurrently across threads.

### Network
- **D-22:** IPv4 and IPv6 support. Default bind `0.0.0.0`. Operator can use `::` for dual-stack.

### Integration
- **D-23:** Construct SessionManager + WsAcceptor in main(), co_spawn accept loop. SIGHUP handler calls `acceptor.reload_tls()`. SIGTERM stops ioc. Thread pool runs ioc.run(). Same flat pattern as node's main().
- **D-24:** Keep reading from WebSocket always. Rely on send queue backpressure for response side. TCP flow control naturally throttles client if relay stops reading.

### Shutdown
- **D-25:** Best-effort Close(1001 Going Away) to active sessions on SIGTERM. Phase 105 (SESS-04) implements proper graceful shutdown with queue drain.

### Testing
- **D-26:** Unit tests (Catch2) for WS framing logic: frame encode/decode, mask/unmask, fragmentation reassembly, upgrade handshake. Live testing against real node on dev machine.

### Claude's Discretion
- Internal class APIs for WsAcceptor, SessionManager, WsSession (as long as they follow decided patterns)
- WebSocket close status code selection for specific error scenarios
- Exact constexpr values for non-decided constants (e.g., fragment buffer size)
- Whether SIGTERM Close(1001) is worth the complexity in Phase 101 or deferred to Phase 105

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Architecture
- `.planning/research/ARCHITECTURE.md` -- Component layout, data flow diagrams, WebSocket session lifecycle, TLS integration pattern
- `.planning/research/STACK.md` -- Hand-rolled WS rationale, Asio SSL pattern, asio_ssl.cpp separate compilation, frame format specification

### Protocol
- `db/PROTOCOL.md` -- Wire format spec, message types (for understanding what flows through the relay)

### Existing Code (Phase 100 output)
- `relay/relay_main.cpp` -- Current main() with config, identity, signal handling, io_context (add accept loop here)
- `relay/core/session.h` -- Send queue with drain coroutine, do_send() to be wired to WsSession callback
- `relay/core/session.cpp` -- Send queue implementation
- `relay/config/relay_config.h` -- RelayConfig struct (add cert_path, key_path)
- `relay/config/relay_config.cpp` -- Config loader (extend for TLS fields)
- `relay/CMakeLists.txt` -- Standalone CMake (add OpenSSL, new ws/ sources)

### Node Patterns (reference only, not modified)
- `db/net/connection.h` -- Send queue pattern reference (deque + drain coroutine)
- `db/main.cpp` -- Thread pool + signal handling pattern reference
- `db/peer/metrics_collector.cpp` -- Hand-rolled HTTP server pattern (Prometheus endpoint) -- useful as reference for HTTP upgrade parsing

### Requirements
- `.planning/REQUIREMENTS.md` -- TRANS-01, TRANS-02, TRANS-03, TRANS-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/core/session.h/cpp` -- Send queue with drain coroutine. do_send() currently appends to test deque. WsSession wires this to WebSocket write via callback injection.
- `relay/identity/relay_identity.h/cpp` -- ML-DSA-87 identity. Loaded in main() but not used until Phase 102 auth.
- `db/peer/metrics_collector.cpp` -- Hand-rolled HTTP parsing for Prometheus endpoint. Reference for HTTP upgrade request parsing.

### Established Patterns
- Asio coroutine-based accept loops (db/net/uds_acceptor.h, db/net/tcp_acceptor.h)
- Signal handling: SIGTERM/SIGINT for shutdown, SIGHUP for reload (relay_main.cpp:135-148)
- Send queue: deque + drain coroutine + cap + disconnect on overflow (session.h)
- Config: JSON file via nlohmann/json (relay_config.cpp)

### Integration Points
- `relay/ws/` -- Empty directory with .gitkeep. New WS code goes here.
- `relay_main.cpp` -- Add SessionManager + WsAcceptor construction, co_spawn accept loop, thread pool
- `relay/config/relay_config.h` -- Add cert_path, key_path fields
- `relay/CMakeLists.txt` -- Add find_package(OpenSSL), new source files, asio_ssl.cpp compilation unit

</code_context>

<specifics>
## Specific Ideas

- Thread pool from the start (`hardware_concurrency()` threads) rather than single-threaded. User preference for production-ready concurrency.
- Live testing against the real node running on the dev machine via UDS. No Docker integration tests for Phase 101.
- Hybrid frame support (text + binary) from day one so Phase 103 doesn't need transport changes for large blob payloads.
- Session wrapping pattern: WsSession in ws/ wraps Session in core/ -- avoids mixing transport concerns with queuing logic.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 101-websocket-transport*
*Context gathered: 2026-04-09*
