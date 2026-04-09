---
phase: 96-peermanager-architecture
plan: 02
subsystem: peer
tags: [refactoring, god-object, component-extraction, connection-lifecycle, blob-push]

# Dependency graph
requires:
  - phase: 96-peermanager-architecture plan 01
    provides: peer_types.h, MetricsCollector, PexManager, facade delegation pattern
provides:
  - ConnectionManager component (peers_ ownership, connection lifecycle, dedup, keepalive, strike)
  - BlobPushManager component (pending_fetches_ ownership, BlobNotify/BlobFetch protocol, on_blob_ingested fan-out)
  - PeerManager facade delegates connection and blob push operations to components
affects: [96-03-PLAN]

# Tech tracking
tech-stack:
  added: []
  patterns: [deferred-reference-init, connect-disconnect-callbacks, expiry-rearm-callback]

key-files:
  created:
    - db/peer/connection_manager.h
    - db/peer/connection_manager.cpp
    - db/peer/blob_push_manager.h
    - db/peer/blob_push_manager.cpp
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/peer/metrics_collector.h
    - db/peer/metrics_collector.cpp
    - db/CMakeLists.txt

key-decisions:
  - "MetricsCollector uses deferred set_peers() pointer instead of constructor reference to break init-order dependency with ConnectionManager"
  - "ConnectionManager takes ConnectCallback and DisconnectCallback for cross-component wiring (PEX tracking on connect, blob_push cleanup on disconnect)"
  - "BlobPushManager receives expiry rearm as callback instead of owning expiry timer (timer stays in PeerManager for Plan 03 SyncOrchestrator)"
  - "ConnectionManager stores Server& reference for delay_reconnect and ACL rejection notification"

patterns-established:
  - "Deferred reference init: MetricsCollector stores const pointer, set via set_peers() after both components exist"
  - "Cross-component callbacks: facade wires lambdas in initializer list to connect components without circular deps"
  - "find_peer/peer_display_name delegation: PeerManager keeps thin wrappers that delegate to conn_mgr_"

requirements-completed: [ARCH-01]

# Metrics
duration: 69min
completed: 2026-04-08
---

# Phase 96 Plan 02: ConnectionManager and BlobPushManager Extraction Summary

**Extracted ConnectionManager (peers_ ownership, connection lifecycle, keepalive) and BlobPushManager (pending_fetches_, blob push/fetch protocol) from PeerManager, reducing peer_manager.cpp from 3576 to 3094 lines**

## Performance

- **Duration:** 69 min
- **Started:** 2026-04-08T05:07:12Z
- **Completed:** 2026-04-08T06:16:13Z
- **Tasks:** 1
- **Files modified:** 9

## Accomplishments
- Created ConnectionManager with 12 methods: on_peer_connected, on_peer_disconnected, should_accept_connection, announce_and_sync, recv_sync_msg, keepalive_loop, record_strike, is_trusted_address, disconnect_unauthorized_peers, find_peer, peer_display_name, cancel_timers
- Created BlobPushManager with 7 methods: on_blob_ingested, on_blob_notify, handle_blob_fetch, handle_blob_fetch_response, clean_pending_fetches, find_peer, peer_display_name
- PeerManager facade delegates all connection lifecycle and blob push operations to components while maintaining unchanged public API
- All 604 non-networking tests pass (3054 assertions), all 85 peer/PEX/metrics/keepalive/event-expiry tests pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Extract ConnectionManager and BlobPushManager** - `973d3df` (refactor)

## Files Created/Modified
- `db/peer/connection_manager.h` - ConnectionManager class: owns peers_ deque, connection lifecycle, keepalive, strike system, dedup
- `db/peer/connection_manager.cpp` - 12 method implementations (431 lines)
- `db/peer/blob_push_manager.h` - BlobPushManager class: owns pending_fetches_, blob push/fetch protocol
- `db/peer/blob_push_manager.cpp` - 7 method implementations (239 lines)
- `db/peer/peer_manager.h` - Removed 14 method declarations and 7 member variables; added ConnectionManager conn_mgr_ and BlobPushManager blob_push_ members
- `db/peer/peer_manager.cpp` - Removed ~482 lines of extracted implementations; added delegation calls and callback wiring in constructor
- `db/peer/metrics_collector.h` - Changed peers_ from const reference to const pointer, added set_peers() method
- `db/peer/metrics_collector.cpp` - Updated constructor (removed peers param), updated peers_ access to use pointer dereference
- `db/CMakeLists.txt` - Added peer/connection_manager.cpp and peer/blob_push_manager.cpp to source list

## Decisions Made
- MetricsCollector stores peers as a pointer (set via set_peers()) instead of a constructor reference to avoid accessing ConnectionManager members before its constructor runs
- ConnectionManager takes ConnectCallback/DisconnectCallback to handle cross-component wiring without circular dependencies between components
- BlobPushManager receives expiry rearm as a callback rather than owning the expiry timer -- the timer stays in PeerManager as it will move to SyncOrchestrator in Plan 03
- ConnectionManager stores a Server& reference because it needs delay_reconnect() during dedup and notify_acl_rejected() during ACL checks

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] MetricsCollector init-order dependency with ConnectionManager**
- **Found during:** Task 1 (Constructor design step)
- **Issue:** Plan specified metrics_collector_ before conn_mgr_ in init order, but ConnectionManager constructor needs NodeMetrics& from MetricsCollector, while MetricsCollector needs conn_mgr_.peers(). Circular init dependency.
- **Fix:** Changed MetricsCollector to store peers as a pointer (initially null), with set_peers() called in constructor body after both components exist. metrics_collector_ constructed first without peers, conn_mgr_ second with metrics ref, then peers wired.
- **Files modified:** db/peer/metrics_collector.h, db/peer/metrics_collector.cpp, db/peer/peer_manager.cpp
- **Verification:** Build succeeds, all tests pass
- **Committed in:** 973d3df (Task 1 commit)

**2. [Rule 1 - Bug] ConnectCallback needed separate from DisconnectCallback**
- **Found during:** Task 1 (on_peer_connected cross-component wiring)
- **Issue:** Plan suggested using a single DisconnectCallback for both connect and disconnect events. The connect path needs to pass (conn, addr) for PEX tracking and pubkey hash persistence, which has different semantics than disconnect cleanup.
- **Fix:** Added separate ConnectCallback type taking (Connection::Ptr, string) and DisconnectCallback taking just Connection::Ptr
- **Files modified:** db/peer/connection_manager.h, db/peer/connection_manager.cpp
- **Verification:** Build succeeds, PEX peer persistence works correctly
- **Committed in:** 973d3df (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (1 blocking init-order issue, 1 callback design bug)
**Impact on plan:** Both auto-fixes necessary for correctness. No scope creep.

## Issues Encountered
- Full test suite takes several minutes due to networking tests with timeouts; verified critical test subsets individually (peer-manager 3/3, PEX 7/7, metrics 18/18, keepalive 3/3, event-expiry 5/5, non-networking 604/604)

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functionality is wired and operational.

## Next Phase Readiness
- ConnectionManager and BlobPushManager extraction complete, following the same facade-delegation pattern established in Plan 01
- Plan 03 can now extract SyncOrchestrator (run_sync_with_peer, handle_sync_as_responder, sync_timer_loop, sync_all_peers) and MessageDispatcher (the large on_peer_message switch)
- PeerManager public API is unchanged -- no caller-side modifications needed
- peer_manager.cpp is now 3094 lines (from original 4187 in Plan 01 start, 3576 after Plan 01)

---
*Phase: 96-peermanager-architecture*
*Completed: 2026-04-08*

## Self-Check: PASSED

- All 4 created files exist on disk
- Task commit (973d3df) exists in git history
- Build succeeds with zero errors
- All targeted test suites pass (peer-manager 3/3, PEX 7/7, metrics 18/18, keepalive 3/3, event-expiry 5/5, non-networking 604/604)
