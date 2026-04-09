---
phase: 101-websocket-transport
plan: 02
subsystem: relay
tags: [websocket, tls, asio, coroutines, session-management, thread-pool]

# Dependency graph
requires:
  - phase: 101-01
    provides: "WS framing library (encode/decode/mask/reassemble), HTTP upgrade handshake, Session write callback, TLS config fields, OpenSSL linkage"
provides:
  - "WsSession: dual-mode TLS/plain WebSocket session with RFC 6455 lifecycle"
  - "WsAcceptor: accept loop with TLS context management, connection cap, SIGHUP reload"
  - "SessionManager: session tracking by ID for fan-out"
  - "Thread pool relay binary with SIGTERM graceful shutdown"
affects: [102-json-protocol, 103-node-bridge, 104-subscription-fanout, 105-hardening]

# Tech tracking
tech-stack:
  added: []
  patterns: ["dual-mode stream variant (tcp::socket | ssl::stream)", "shared_ptr<ssl::context> swap for hot TLS reload", "co_spawn detached coroutines for session lifecycle", "explicit template instantiation for stream-generic WS upgrade"]

key-files:
  created:
    - relay/ws/ws_session.h
    - relay/ws/ws_session.cpp
    - relay/ws/ws_acceptor.h
    - relay/ws/ws_acceptor.cpp
    - relay/ws/session_manager.h
    - relay/ws/session_manager.cpp
  modified:
    - relay/CMakeLists.txt
    - relay/relay_main.cpp

key-decisions:
  - "std::variant<tcp::socket, TlsStream> for dual-mode stream -- avoids virtual dispatch and heap indirection"
  - "shared_ptr<ssl::context> with mutex for atomic TLS context swap on SIGHUP -- existing connections keep old context via ref-count"
  - "Control frames (ping/pong/close) sent via send_raw() bypassing Session queue -- avoids backpressure blocking keepalive"
  - "Explicit template instantiation of do_ws_upgrade for both stream types -- keeps template impl in .cpp"

patterns-established:
  - "Dual-mode stream: std::variant<tcp::socket, ssl::stream<tcp::socket>> with std::visit for read/write"
  - "Session lifecycle: co_spawn 3 detached coroutines (read_loop, drain_send_queue, ping_loop) from start()"
  - "Idle timeout pattern: steady_timer with cancel on first message"

requirements-completed: [TRANS-01, TRANS-02, TRANS-03, TRANS-04]

# Metrics
duration: 42min
completed: 2026-04-09
---

# Phase 101 Plan 02: WebSocket Session Layer Summary

**Dual-mode WsSession with TLS/plain WebSocket lifecycle, WsAcceptor with SIGHUP TLS reload and 1024-connection cap, thread-pool relay binary with graceful shutdown**

## Performance

- **Duration:** 42 min
- **Started:** 2026-04-09T13:45:38Z
- **Completed:** 2026-04-09T14:27:24Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- WsSession wraps core::Session with RFC 6455 frame lifecycle: read loop with parse/unmask/reassemble, ping/pong keepalive (30s/60s), idle timeout (30s), close handshake
- WsAcceptor manages TLS context with shared_ptr swap for SIGHUP reload, enforces 1024 connection cap, 5s handshake timeout
- SessionManager provides O(1) session lookup by ID with for_each iteration for fan-out
- relay_main.cpp fully integrated: WsAcceptor + SessionManager construction, TLS init, SIGHUP reload, SIGTERM Close(1001), hardware_concurrency() thread pool

## Task Commits

Each task was committed atomically:

1. **Task 1: WsSession, SessionManager, and WsAcceptor classes** - `ea154fe` (feat)
2. **Task 2: Main() integration with thread pool, SIGHUP TLS reload, and SIGTERM shutdown** - `9de9948` (feat)

## Files Created/Modified
- `relay/ws/ws_session.h` - WsSession class with dual-mode TLS/plain stream, read/write/ping/close lifecycle
- `relay/ws/ws_session.cpp` - Full RFC 6455 session implementation (read_loop, ping_loop, close, on_message)
- `relay/ws/ws_acceptor.h` - WsAcceptor class with TLS context management and connection cap
- `relay/ws/ws_acceptor.cpp` - Accept loop, TLS handshake, WS upgrade, connection limit enforcement
- `relay/ws/session_manager.h` - SessionManager with unordered_map session tracking and for_each
- `relay/ws/session_manager.cpp` - add/remove/get/count implementation
- `relay/CMakeLists.txt` - Added ws_session.cpp, ws_acceptor.cpp, session_manager.cpp to relay lib
- `relay/relay_main.cpp` - Integrated WsAcceptor, SessionManager, TLS, SIGHUP, SIGTERM, thread pool

## Decisions Made
- Used std::variant<tcp::socket, TlsStream> for dual-mode stream to avoid virtual dispatch overhead
- Control frames (ping/pong/close) bypass the Session send queue via send_raw() to prevent backpressure from blocking keepalive
- Explicit template instantiation of do_ws_upgrade in .cpp file to avoid header bloat while supporting both stream types
- shared_ptr<ssl::context> with mutex for thread-safe TLS context swap during SIGHUP

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Added ws_session.h include in relay_main.cpp**
- **Found during:** Task 2 (relay_main.cpp integration)
- **Issue:** session_manager.h forward-declares WsSession, but for_each lambda calls session->close() which needs the full type definition
- **Fix:** Added `#include "relay/ws/ws_session.h"` to relay_main.cpp
- **Files modified:** relay/relay_main.cpp
- **Verification:** Build succeeds
- **Committed in:** 9de9948 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Trivial include fix necessary for compilation. No scope creep.

## Issues Encountered
None.

## Known Stubs
- `on_message()` in ws_session.cpp logs messages at debug level only -- Phase 102 will add JSON parsing and message dispatch
- No auth check on connection -- Phase 102 adds challenge-response authentication

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- WebSocket transport layer complete: relay accepts WSS and WS connections with full RFC 6455 lifecycle
- Ready for Phase 102 (JSON protocol): WsSession::on_message() is the hook point for JSON message parsing
- Ready for Phase 103 (node bridge): UDS multiplexing to chromatindb node
- SessionManager provides the fan-out infrastructure for Phase 104 (subscription fanout)

---
*Phase: 101-websocket-transport*
*Completed: 2026-04-09*
