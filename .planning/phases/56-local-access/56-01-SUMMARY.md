---
phase: 56-local-access
plan: 01
subsystem: networking
tags: [uds, unix-domain-socket, asio, generic-stream, config]

# Dependency graph
requires: []
provides:
  - "Config uds_path field with JSON parsing and validation"
  - "Connection generic stream socket (supports TCP and UDS)"
  - "UDS factory methods: create_uds_inbound, create_uds_outbound"
  - "UdsAcceptor class for Unix domain socket listening"
affects: [56-02]

# Tech tracking
tech-stack:
  added: [asio::generic::stream_protocol, asio::local::stream_protocol]
  patterns: [generic-socket-factory, uds-acceptor-coroutine]

key-files:
  created:
    - db/net/uds_acceptor.h
    - db/net/uds_acceptor.cpp
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/net/connection.h
    - db/net/connection.cpp
    - db/CMakeLists.txt

key-decisions:
  - "Generic stream protocol socket: Connection uses asio::generic::stream_protocol::socket internally, TCP and UDS factories convert typed sockets via native handle release"
  - "UDS always trusted: is_uds_ flag bypasses trust_check in do_handshake, always taking TrustedHello path"
  - "Remote address capture in factory: moved from constructor to factory methods to avoid generic endpoint IP extraction complexity"
  - "sockaddr cast via void* for trust check IP extraction from generic endpoint"

patterns-established:
  - "Socket factory pattern: typed socket -> release native handle -> construct generic socket"
  - "UdsAcceptor follows Server accept_loop pattern: co_spawn accept_loop, same callback wiring"

requirements-completed: []

# Metrics
duration: 8min
completed: 2026-03-22
---

# Phase 56 Plan 01: UDS Foundation Summary

**Config uds_path field, Connection refactored to generic stream socket, and UdsAcceptor for Unix domain socket listening**

## Performance

- **Duration:** 8 min
- **Started:** 2026-03-22T15:05:32Z
- **Completed:** 2026-03-22T15:13:14Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Config struct supports `uds_path` with absolute-path and 107-char-limit validation
- Connection class uses `asio::generic::stream_protocol::socket` internally, transparent to TCP callers
- UDS connections (via `create_uds_inbound`/`create_uds_outbound`) always take TrustedHello handshake path
- UdsAcceptor listens on Unix socket, handles stale socket cleanup, sets 0660 permissions
- All 94 config tests and 9 connection/server/handshake tests pass unchanged

## Task Commits

Each task was committed atomically:

1. **Task 1: Add uds_path config field and Connection generic socket refactor** - `fe2682a` (feat)
2. **Task 2: Create UdsAcceptor class for Unix domain socket listening** - `af3304b` (feat)

## Files Created/Modified
- `db/config/config.h` - Added `uds_path` field to Config struct
- `db/config/config.cpp` - Added uds_path parsing, known_keys entry, validation (absolute path, length limit)
- `db/net/connection.h` - Changed socket_ to generic stream, added UDS factories and is_uds() getter
- `db/net/connection.cpp` - Refactored constructor/factories for generic socket, UDS trust bypass in handshake
- `db/net/uds_acceptor.h` - UdsAcceptor class declaration
- `db/net/uds_acceptor.cpp` - UDS accept loop, stale socket cleanup, permissions, shutdown cleanup
- `db/CMakeLists.txt` - Added uds_acceptor.cpp to library sources

## Decisions Made
- Used `asio::generic::stream_protocol::socket` as the universal internal socket type, converting typed sockets via native handle release in factory methods
- Moved remote address capture from Connection constructor to factory methods (TCP factories capture before move, UDS factories set "uds")
- UDS connections set `is_uds_ = true`, which causes `do_handshake()` to unconditionally set `peer_is_trusted = true`
- For TCP trust check with generic endpoint, extract IP by casting endpoint data through `void*` to `sockaddr_in`/`sockaddr_in6`

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed static_cast from sockaddr* to sockaddr_in***
- **Found during:** Task 1 (Connection generic socket refactor)
- **Issue:** `static_cast<const sockaddr_in*>(gen_ep.data())` fails because data() returns `sockaddr*`, not `void*`
- **Fix:** Cast through `const void*` first: `static_cast<const sockaddr_in*>(data)` where `data = gen_ep.data()`
- **Files modified:** db/net/connection.cpp
- **Verification:** Build succeeds, all tests pass
- **Committed in:** fe2682a (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Trivial cast correction, no scope creep.

## Issues Encountered
None beyond the auto-fixed cast issue above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- UDS foundation complete: config, Connection, UdsAcceptor all ready
- Plan 02 will integrate UdsAcceptor into PeerManager and add tests

---
*Phase: 56-local-access*
*Completed: 2026-03-22*
