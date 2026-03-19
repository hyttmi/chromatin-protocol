---
phase: 38-thread-pool-crypto-offload
plan: 01
subsystem: infra
tags: [asio, thread-pool, crypto-offload, config]

# Dependency graph
requires: []
provides:
  - "worker_threads config field with JSON parsing and auto-detect/clamping"
  - "db/crypto/thread_pool.h with resolve_worker_threads() and offload() awaitable helper"
  - "asio::thread_pool lifecycle in main() (create before ioc, join after ioc.run)"
  - "Thread pool reference plumbed through BlobEngine, SyncProtocol, Connection, Server, PeerManager"
affects: [38-02, 38-03]

# Tech tracking
tech-stack:
  added: [asio::thread_pool, asio::co_spawn offload pattern]
  patterns: [pool-reference-threading, set_pool-forwarding]

key-files:
  created:
    - db/crypto/thread_pool.h
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/main.cpp
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/net/connection.h
    - db/net/server.h
    - db/net/server.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp

key-decisions:
  - "Pool ref as second param in BlobEngine (after store, before config uint64_ts)"
  - "Pool ref as third param in SyncProtocol (after storage, before clock)"
  - "Pool ref via set_pool() on Connection/Server (pointer, not constructor ref) -- Connection is factory-created"
  - "Pool ref as sixth param in PeerManager (after ioc, before acl)"

patterns-established:
  - "set_pool() pattern: Server forwards pool to all Connection instances at creation time"
  - "Pool ownership in main(): created before components, joined after ioc.run()"

requirements-completed: [PERF-09]

# Metrics
duration: 25min
completed: 2026-03-19
---

# Phase 38 Plan 01: Thread Pool Infrastructure Summary

**asio::thread_pool with config-driven worker count, offload() awaitable helper, and pool reference plumbed through the entire object graph (BlobEngine, SyncProtocol, Connection, Server, PeerManager)**

## Performance

- **Duration:** 25 min
- **Started:** 2026-03-19T07:15:11Z
- **Completed:** 2026-03-19T07:41:06Z
- **Tasks:** 2
- **Files modified:** 17 (5 source headers, 6 source implementations, 4 test files, 1 new header, 1 main)

## Accomplishments
- Config struct parses worker_threads from JSON with 0=auto-detect and clamping to hardware_concurrency
- Header-only offload helper (resolve_worker_threads + co_spawn-based offload template) ready for Plans 02/03
- Thread pool created in main() with correct shutdown ordering (ioc.run first, pool.join second)
- Pool reference threaded through all 5 consumer classes without any behavioral change
- All 370 tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Config field + offload helper + main() pool lifecycle** - `1ac1da6` (feat, TDD)
2. **Task 2: Plumb thread pool reference through the object graph** - `3253a02` (feat)

## Files Created/Modified
- `db/crypto/thread_pool.h` - resolve_worker_threads() and offload() awaitable helper (new)
- `db/config/config.h` - worker_threads field added to Config struct
- `db/config/config.cpp` - JSON parsing for worker_threads
- `db/main.cpp` - Thread pool creation, clamping warning, join after ioc.run
- `db/engine/engine.h` / `engine.cpp` - pool_ member, constructor param
- `db/sync/sync_protocol.h` / `sync_protocol.cpp` - pool_ member, constructor param
- `db/net/connection.h` - pool_ pointer, set_pool() method
- `db/net/server.h` / `server.cpp` - pool_ pointer, set_pool(), forwarding to connections
- `db/peer/peer_manager.h` / `peer_manager.cpp` - pool_ ref, constructor param, wires to Server and SyncProtocol
- `db/tests/config/test_config.cpp` - 4 new worker_threads tests
- `db/tests/engine/test_engine.cpp` - All BlobEngine calls updated with pool
- `db/tests/sync/test_sync_protocol.cpp` - All BlobEngine and SyncProtocol calls updated
- `db/tests/peer/test_peer_manager.cpp` - All BlobEngine, PeerManager, SyncProtocol calls updated
- `db/tests/test_daemon.cpp` - All BlobEngine and PeerManager calls updated

## Decisions Made
- Pool ref as constructor parameter for owned objects (BlobEngine, SyncProtocol, PeerManager), set_pool() for factory-created objects (Connection via Server)
- Single shared pool{1} per test case -- minimal overhead, sufficient for compile-time validation

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Plans 02 and 03 can independently add async crypto offload: Plan 02 to BlobEngine (ingest/delete), Plan 03 to Connection (handshake verify)
- offload() helper is ready for use via co_await
- All pool references are in place, no further plumbing needed

## Self-Check: PASSED

- All created files exist (db/crypto/thread_pool.h, 38-01-SUMMARY.md)
- All commits verified (1ac1da6, 3253a02)
- worker_threads in config.h, thread_pool in main.cpp
- pool_ member found in engine.h, connection.h, server.h, peer_manager.h, sync_protocol.h
- 370/370 tests pass

---
*Phase: 38-thread-pool-crypto-offload*
*Completed: 2026-03-19*
