---
phase: 111-single-threaded-rewrite
plan: 01
subsystem: relay
tags: [asio, thread-pool, coroutine, single-threaded, offload]

# Dependency graph
requires: []
provides:
  - "relay/util/thread_pool.h offload() coroutine template under chromatindb::relay::util"
  - "Single-threaded relay_main.cpp event loop model (one ioc.run() thread + offload pool)"
  - "Plain uint32_t SIGHUP-reloadable settings (no atomics)"
affects: [111-02, 111-03]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Single io_context thread + asio::thread_pool offload for CPU-heavy work"]

key-files:
  created: ["relay/util/thread_pool.h"]
  modified: ["relay/relay_main.cpp"]

key-decisions:
  - "offload_pool created at startup with hardware_concurrency() threads, hardcoded (no config field)"
  - "std::atomic<bool> stopping kept for signal handler safety, all other atomics removed"
  - "relay/util/thread_pool.h is a standalone copy (not cross-layer import) per D-06"

patterns-established:
  - "offload() coroutine template: post to thread_pool, co_await result, transfer back to ioc before touching shared state"
  - "Single-threaded relay event loop: all I/O on one ioc.run() thread, no strands needed"

requirements-completed: [CONC-01, CONC-05]

# Metrics
duration: 3min
completed: 2026-04-14
---

# Phase 111 Plan 01: Thread Pool Infrastructure + Single-Threaded Event Loop Summary

**Created relay offload thread pool template and converted relay_main.cpp from N-thread ioc.run() to single event loop thread with separate asio::thread_pool**

## Performance

- **Duration:** 3 min
- **Started:** 2026-04-14T05:28:54Z
- **Completed:** 2026-04-14T05:31:56Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Created relay/util/thread_pool.h with offload() coroutine template under chromatindb::relay::util namespace
- Rewrote relay_main.cpp: removed strand, single ioc.run() thread, separate offload pool
- Converted rate_limit_rate, request_timeout, max_blob_size from std::atomic<uint32_t> to plain uint32_t
- Updated all component wiring calls to pass ioc/offload_pool instead of strand

## Task Commits

Each task was committed atomically:

1. **Task 1: Create relay/util/thread_pool.h offload template** - `f6414796` (feat)
2. **Task 2: Rewrite relay_main.cpp threading model** - `702c6336` (feat)

## Files Created/Modified
- `relay/util/thread_pool.h` - Offload coroutine template for dispatching CPU-heavy work to asio::thread_pool
- `relay/relay_main.cpp` - Single-threaded event loop entry point with offload pool, no strand, plain uint32_t settings

## Decisions Made
None - followed plan as specified.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- relay_main.cpp wiring is complete but will NOT compile until Plan 02 updates component headers (strand removal from UdsMultiplexer, DataHandlers, QueryHandlerDeps, PubSubHandlers, register_auth_routes signatures)
- offload_pool is created but not yet used by any component -- Plan 02 will wire ML-DSA-87 verify through it

## Self-Check: PASSED

- relay/util/thread_pool.h: FOUND
- relay/relay_main.cpp: FOUND
- 111-01-SUMMARY.md: FOUND
- Commit f6414796: FOUND
- Commit 702c6336: FOUND

---
*Phase: 111-single-threaded-rewrite*
*Completed: 2026-04-14*
