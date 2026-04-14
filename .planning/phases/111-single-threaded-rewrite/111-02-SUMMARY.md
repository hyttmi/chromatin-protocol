---
phase: 111-single-threaded-rewrite
plan: 02
subsystem: relay
tags: [asio, single-threaded, strand-removal, mutex-removal, offload, ml-dsa-87]

# Dependency graph
requires:
  - "111-01: relay/util/thread_pool.h offload() template, single-threaded relay_main.cpp"
provides:
  - "All 23 relay production files converted to single-threaded model"
  - "ML-DSA-87 verify offloaded via offload() with transfer-back in http_router.cpp"
  - "Zero std::mutex, zero asio::strand in relay production code"
  - "UdsMultiplexer::send() direct inline execution (no strand posting)"
  - "ResponsePromise uses ioc executor instead of strand executor"
affects: [111-03]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Direct state access without synchronization on single event loop thread", "offload() + transfer-back pattern for CPU-heavy crypto"]

key-files:
  created: []
  modified: ["relay/http/http_router.h", "relay/http/http_router.cpp", "relay/http/handlers_data.h", "relay/http/handlers_data.cpp", "relay/http/handlers_query.h", "relay/http/handlers_query.cpp", "relay/http/handlers_pubsub.h", "relay/http/handlers_pubsub.cpp", "relay/http/http_server.h", "relay/http/http_server.cpp", "relay/http/http_connection.h", "relay/http/http_connection.cpp", "relay/http/response_promise.h", "relay/http/token_store.h", "relay/core/uds_multiplexer.h", "relay/core/uds_multiplexer.cpp", "relay/core/authenticator.h", "relay/core/authenticator.cpp", "relay/core/metrics_collector.h", "relay/core/metrics_collector.cpp", "relay/core/request_router.h", "relay/core/subscription_tracker.h", "relay/core/write_tracker.h"]

key-decisions:
  - "ML-DSA-87 verify offloaded to thread pool via offload() in http_router.cpp auth verify handler"
  - "transfer-back via co_await asio::post(ioc, use_awaitable) after offload before shared state access"
  - "UdsMultiplexer::send() simplified to direct inline execution (all callers on event loop)"
  - "RelayMetrics converted from 8 atomic<uint64_t> to plain uint64_t (single-threaded)"
  - "stopping remains std::atomic<bool> (signal handler safety)"

patterns-established:
  - "Direct state access: all shared state accessed without synchronization on event loop thread"
  - "offload() + transfer-back: CPU-heavy work to pool, then co_await asio::post(ioc) before touching shared state"
  - "ioc_.get_executor() for ResponsePromise timer creation (replaces strand executor)"

requirements-completed: [CONC-02, CONC-03, CONC-04]

# Metrics
duration: 12min
completed: 2026-04-14
---

# Phase 111 Plan 02: Strip All Strand/Mutex/Atomic Synchronization from Relay Components Summary

**Removed all strand, mutex, and atomic synchronization from 23 relay production files, wired ML-DSA-87 offload to thread pool with transfer-back**

## Performance

- **Duration:** 12 min
- **Started:** 2026-04-14T05:35:34Z
- **Completed:** 2026-04-14T05:47:36Z
- **Tasks:** 2
- **Files modified:** 23

## Accomplishments
- Stripped all `using Strand`, `strand_` members, `set_strand()`, and `co_await asio::post(strand_, ...)` from 14 HTTP layer files
- Wired ML-DSA-87 signature verification through `offload()` template with transfer-back in auth verify handler
- Removed `tls_mutex_` from HttpServer and `acl_mutex_` from Authenticator (6 lock_guard calls removed)
- Converted RelayMetrics from 8 `std::atomic<uint64_t>` to plain `uint64_t`
- Converted `max_connections_`, `active_connections_` from atomic to plain uint32_t
- Simplified UdsMultiplexer::send() from strand-posting to direct inline execution
- Updated 9 comment headers from "serialized via strand" to "single event loop thread"

## Task Commits

Each task was committed atomically:

1. **Task 1: Strip strands/mutexes from HTTP layer headers and wire offload** - `b03c4240` (feat)
2. **Task 2: Strip strands/mutexes from core layer and update comments** - `2dca6b5a` (feat)

## Files Created/Modified
- `relay/http/http_router.h` - Removed Strand type alias, set_strand(), strand_ member; updated register_auth_routes signature
- `relay/http/http_router.cpp` - Wired offload() for ML-DSA-87 verify with transfer-back; removed async check_auth; simplified dispatch_async
- `relay/http/handlers_data.h` - Removed Strand, converted atomic refs to plain uint32_t refs, stored ioc_ instead of strand_
- `relay/http/handlers_data.cpp` - Removed 4 strand posting calls, use ioc_.get_executor() for promises, plain dereference for config
- `relay/http/handlers_query.h` - Removed Strand type alias and strand pointer from QueryHandlerDeps
- `relay/http/handlers_query.cpp` - Removed Strand type alias, strand param from forward_query, use ioc.get_executor() for promises
- `relay/http/handlers_pubsub.h` - Removed Strand, stored ioc_ instead of strand_
- `relay/http/handlers_pubsub.cpp` - Removed 3 strand posting calls
- `relay/http/http_server.h` - Removed mutex include and tls_mutex_, converted max/active_connections to plain uint32_t
- `relay/http/http_server.cpp` - Removed 3 lock_guard calls, converted atomic load/store to plain operations
- `relay/http/http_connection.h` - Converted active_connections_ and ConnectionGuard from atomic to plain uint32_t
- `relay/http/http_connection.cpp` - Constructor parameter type change
- `relay/http/response_promise.h` - Updated comment to single-threaded model
- `relay/http/token_store.h` - Updated comment to single-threaded model
- `relay/core/uds_multiplexer.h` - Removed Strand type alias and strand_ member, converted request_timeout_ to plain pointer
- `relay/core/uds_multiplexer.cpp` - Simplified send() to direct execution, replaced all strand_ with ioc_
- `relay/core/authenticator.h` - Removed mutex include and acl_mutex_ member
- `relay/core/authenticator.cpp` - Removed 3 lock_guard calls
- `relay/core/metrics_collector.h` - Converted 8 atomic counters to plain uint64_t
- `relay/core/metrics_collector.cpp` - Removed 8 .load(memory_order_relaxed) calls
- `relay/core/request_router.h` - Updated comment to single-threaded model
- `relay/core/subscription_tracker.h` - Updated comment to single-threaded model
- `relay/core/write_tracker.h` - Updated comment to single-threaded model

## Decisions Made
None - followed plan as specified.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all changes are synchronization removal and comment updates; no new functionality that could be stubbed.

## Next Phase Readiness
- All 23 relay production files now match the single-threaded model from Plan 01
- Plan 03 (test adaptation) can now update test files to remove strand parameters
- Relay will NOT compile until Plan 03 updates test files that reference the old strand-based signatures

## Self-Check: PASSED

- relay/http/http_router.h: FOUND
- relay/http/http_router.cpp: FOUND
- relay/core/uds_multiplexer.h: FOUND
- relay/core/uds_multiplexer.cpp: FOUND
- relay/core/authenticator.h: FOUND
- relay/core/authenticator.cpp: FOUND
- Commit b03c4240: FOUND
- Commit 2dca6b5a: FOUND
- Zero std::mutex in production: VERIFIED
- Zero asio::strand in production: VERIFIED
- Zero strand_ in production: VERIFIED
- offload() wired: VERIFIED
- transfer-back present: VERIFIED

---
*Phase: 111-single-threaded-rewrite*
*Completed: 2026-04-14*
