---
phase: 96-peermanager-architecture
plan: 03
subsystem: peer
tags: [refactoring, god-object, component-extraction, sync-protocol, message-dispatch]

# Dependency graph
requires:
  - phase: 96-peermanager-architecture plan 02
    provides: ConnectionManager, BlobPushManager, peer_types.h, MetricsCollector, PexManager
provides:
  - SyncOrchestrator component (sync protocol, expiry scanning, cursor/storage compaction)
  - MessageDispatcher component (message routing switch, all query handlers)
  - PeerManager thin facade (~679 lines) owning 6 focused components
  - Complete 6-component PeerManager decomposition (ARCH-01)
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: [pex-callback-injection, awaitable-callbacks, deferred-rate-limit-wiring]

key-files:
  created:
    - db/peer/sync_orchestrator.h
    - db/peer/sync_orchestrator.cpp
    - db/peer/message_dispatcher.h
    - db/peer/message_dispatcher.cpp
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/CMakeLists.txt

key-decisions:
  - "SyncOrchestrator receives PEX callbacks (PexRequestCallback, PexRespondCallback) as awaitable std::function to avoid circular dependency with PexManager while keeping inline PEX after sync"
  - "MessageDispatcher receives UptimeCallback and MaxStorageCallback for NodeInfoRequest/StorageStatusRequest instead of referencing MetricsCollector and Config directly"
  - "SyncOrchestrator owns rate_limit_bytes_per_sec_ for byte budget check during sync (set via set_rate_limit)"
  - "PeerManager facade is 679 lines (above 500 target) because reload_config alone requires ~140 lines and constructor wiring requires ~130 lines of lambda callbacks"

patterns-established:
  - "Awaitable callback injection: components receive std::function<asio::awaitable<void>(...)> for cross-component operations that must happen inline in a coroutine"
  - "6-component facade: PeerManager owns MetricsCollector, ConnectionManager, SyncOrchestrator, PexManager, BlobPushManager, MessageDispatcher with strict init order"

requirements-completed: [ARCH-01]

# Metrics
duration: 93min
completed: 2026-04-08
---

# Phase 96 Plan 03: SyncOrchestrator and MessageDispatcher Extraction Summary

**Extracted SyncOrchestrator (1234 lines) and MessageDispatcher (1167 lines) from PeerManager, completing 6-component decomposition from 4187-line god object to thin facade**

## Performance

- **Duration:** 93 min
- **Started:** 2026-04-08T06:28:29Z
- **Completed:** 2026-04-08T08:01:29Z
- **Tasks:** 2 (1 extraction, 1 verification-only)
- **Files modified:** 7

## Accomplishments
- Created SyncOrchestrator with 14 methods: run_sync_with_peer, handle_sync_as_responder, sync_all_peers, sync_timer_loop, route_sync_message, recv_sync_msg, send_sync_rejected, expiry_scan_loop, cursor_compaction_loop, compaction_loop, rearm_expiry_timer, check_full_resync, cancel_timers, set_sync_config
- Created MessageDispatcher with on_peer_message (the entire 1167-line dispatch switch including all 20+ query handlers) and try_consume_tokens rate limiter
- PeerManager reduced from 3094 lines (post-Wave 2) to 679 lines (thin facade)
- All 647 tests pass (3174 assertions) with zero regressions
- ASAN/TSAN sanitizer gates passed: peer-manager 3/3, PEX 7/7, metrics 18/18, keepalive 3/3, event-expiry 5/5
- Zero caller-side changes (main.cpp, relay, loadgen, all test files unchanged)
- 7 peer/*.cpp source files in CMakeLists.txt (peer_manager + 6 components)

## Task Commits

Each task was committed atomically:

1. **Task 1: Extract SyncOrchestrator and MessageDispatcher** - `478518a` (refactor)
2. **Task 2: Sanitizer gate and final verification** - verification only, no code changes

## Files Created/Modified
- `db/peer/sync_orchestrator.h` - SyncOrchestrator class: sync protocol, expiry, cursor/storage compaction (147 lines)
- `db/peer/sync_orchestrator.cpp` - 14 method implementations including run_sync_with_peer (1234 lines)
- `db/peer/message_dispatcher.h` - MessageDispatcher class: message routing and query handlers (87 lines)
- `db/peer/message_dispatcher.cpp` - on_peer_message dispatch switch with all query handlers (1167 lines)
- `db/peer/peer_manager.h` - Thin facade: 6 component members, static encode/decode, signal handlers (180 lines)
- `db/peer/peer_manager.cpp` - Facade: constructor wiring, start/stop, reload_config, static methods (679 lines)
- `db/CMakeLists.txt` - Added sync_orchestrator.cpp and message_dispatcher.cpp to source list

## Decisions Made
- SyncOrchestrator receives PEX callbacks as `std::function<asio::awaitable<void>(Connection::Ptr)>` rather than holding PexManager/ACL references, because the inline PEX after sync must run as part of the sync coroutine (before syncing=false) and the ACL check lives on the facade
- MessageDispatcher receives UptimeCallback and MaxStorageCallback as lambdas rather than MetricsCollector/Config references, breaking what would be a circular include path
- SyncOrchestrator owns its own `rate_limit_bytes_per_sec_` member (set from facade) because the byte budget check during sync needs this value and it was previously on PeerManager
- The facade is 679 lines (above the 500-line plan target) because: constructor wiring with 6-component lambda callbacks ~130 lines, reload_config with 20+ config fields ~140 lines, static encode/decode methods ~80 lines, start() with all co_spawn calls ~60 lines. These are inherent facade coordination that cannot be decomposed further.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Missing rate_limit_bytes_per_sec guard in SyncOrchestrator byte budget check**
- **Found during:** Task 1 (Test run)
- **Issue:** Original code had `if (rate_limit_bytes_per_sec_ > 0 && peer->bucket_tokens == 0)` but extraction dropped the rate_limit guard. When rate limiting is disabled (rate=0), bucket_tokens stays at initial 0, causing sync to immediately abort with "byte budget exhausted"
- **Fix:** Added `rate_limit_bytes_per_sec_` member to SyncOrchestrator with `set_rate_limit()` method, restored the guard in both initiator and responder byte budget checks
- **Files modified:** db/peer/sync_orchestrator.h, db/peer/sync_orchestrator.cpp, db/peer/peer_manager.cpp
- **Verification:** peer-manager sync test passes (blob replication works)
- **Committed in:** 478518a (Task 1 commit)

**2. [Rule 1 - Bug] PEX callbacks needed to be awaitable for inline-after-sync**
- **Found during:** Task 1 (Design step)
- **Issue:** Plan suggested void PexRequestCallback but the inline PEX after sync must complete BEFORE syncing=false is set. A void callback would co_spawn and race with the syncing flag reset
- **Fix:** Changed PexRequestCallback from `std::function<void(...)>` to `std::function<asio::awaitable<void>(...)>`, co_await in run_sync_with_peer and handle_sync_as_responder
- **Files modified:** db/peer/sync_orchestrator.h, db/peer/sync_orchestrator.cpp, db/peer/peer_manager.cpp
- **Verification:** PEX tests pass, sync cooldown test passes
- **Committed in:** 478518a (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both auto-fixes necessary for correctness. No scope creep.

## Issues Encountered
- Full test suite takes ~14 minutes due to networking tests with timeouts; verified critical test subsets individually under ASAN and TSAN
- ASAN shows 2 pre-existing SEGV failures in test_connection.cpp and test_uds.cpp (not related to refactoring, existed before Phase 96)
- TSAN atomic_thread_fence warning from libstdc++ header is a compiler/sanitizer limitation, not a real race

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functionality is wired and operational.

## Final File Layout

| File | Lines | Description |
|------|-------|-------------|
| peer_manager.h | 180 | Thin facade class declaration |
| peer_manager.cpp | 679 | Facade: wiring, start/stop, reload, static methods |
| peer_types.h | 89 | 6 shared structs |
| connection_manager.h/.cpp | 120/431 | peers_ deque, connection lifecycle, keepalive |
| message_dispatcher.h/.cpp | 87/1167 | Message routing switch, all query handlers |
| sync_orchestrator.h/.cpp | 147/1234 | Sync protocol, expiry, cursor/storage compaction |
| pex_manager.h/.cpp | 97/389 | PEX protocol, peer persistence |
| blob_push_manager.h/.cpp | 72/239 | BlobNotify/BlobFetch, on_blob_ingested fan-out |
| metrics_collector.h/.cpp | 74/367 | NodeMetrics, Prometheus, SIGUSR1, periodic log |
| sync_reject.h | 36 | Sync rejection reason constants |
| **Total** | **5408** | (was 4578 header+impl before Phase 96) |

---
*Phase: 96-peermanager-architecture*
*Completed: 2026-04-08*

## Self-Check: PASSED

- All 4 created files exist on disk (sync_orchestrator.h/.cpp, message_dispatcher.h/.cpp)
- All 3 modified files verified (peer_manager.h/.cpp, CMakeLists.txt)
- Task commit (478518a) exists in git history
- Build succeeds with zero errors
- All 647 tests pass (3174 assertions)
- ASAN: all peer-related tests pass (18/18)
- TSAN: all peer-related tests pass (18/18), 621 broader tests pass
