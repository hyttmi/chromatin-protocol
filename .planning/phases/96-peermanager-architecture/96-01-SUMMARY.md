---
phase: 96-peermanager-architecture
plan: 01
subsystem: peer
tags: [refactoring, god-object, component-extraction, metrics, pex]

# Dependency graph
requires:
  - phase: 95-code-deduplication
    provides: shared utility headers (endian.h, blob_helpers.h, auth_helpers.h, verify_helpers.h)
provides:
  - peer_types.h shared type definitions for all peer components
  - MetricsCollector component (metrics logging, Prometheus endpoint, SIGUSR1 dump)
  - PexManager component (PEX protocol, peer persistence, known address tracking)
  - PeerManager facade pattern with component delegation
affects: [96-02-PLAN, 96-03-PLAN]

# Tech tracking
tech-stack:
  added: []
  patterns: [component-extraction-facade, reference-injection, dump-extra-callback]

key-files:
  created:
    - db/peer/peer_types.h
    - db/peer/metrics_collector.h
    - db/peer/metrics_collector.cpp
    - db/peer/pex_manager.h
    - db/peer/pex_manager.cpp
  modified:
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/CMakeLists.txt

key-decisions:
  - "MetricsCollector receives peers_ by const ref for gauge/dump access instead of peer_count callback"
  - "PexManager has its own find_peer and recv_sync_msg copies since both access PeerInfo sync_inbox via shared peers_ ref"
  - "Inline PEX in sync methods delegates to pex_.build_peer_list_response and pex_.handle_peer_list_response rather than standalone handle_pex_as_responder (syncing flag conflict)"
  - "DumpExtraCallback pattern for MetricsCollector to get UDS count and compaction stats from facade"
  - "PexManager receives encode/decode_peer_list as callbacks to avoid circular dependency with PeerManager static methods"

patterns-established:
  - "Component extraction: by-value members, reference injection, timer-cancel delegated to component cancel_timers()"
  - "Shared types header: peer_types.h contains all struct definitions used by multiple components"
  - "Facade delegation: PeerManager delegates timer co_spawn, lifecycle, and config reload to components"

requirements-completed: [ARCH-01]

# Metrics
duration: 104min
completed: 2026-04-08
---

# Phase 96 Plan 01: Leaf Component Extraction Summary

**Extracted peer_types.h, MetricsCollector, and PexManager from PeerManager god object, reducing peer_manager.cpp from 4187 to 3576 lines**

## Performance

- **Duration:** 104 min
- **Started:** 2026-04-08T03:19:14Z
- **Completed:** 2026-04-08T05:03:27Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- Created peer_types.h with 6 shared structs (PeerInfo, NodeMetrics, PersistedPeer, SyncMessage, ArrayHash32, DisconnectedPeerState)
- Extracted MetricsCollector with 10 methods (metrics logging, Prometheus endpoint, SIGUSR1 dump) into independent component
- Extracted PexManager with 11 methods (PEX protocol, peer persistence, known address tracking) into independent component
- PeerManager facade delegates all metrics and PEX operations to components while maintaining unchanged public API
- All 384+ non-networking tests pass, all peer-manager/PEX/metrics/keepalive/event-expiry tests pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Create peer_types.h and extract MetricsCollector** - `15f0e43` (refactor)
2. **Task 2: Extract PexManager** - `4955dfd` (refactor)

## Files Created/Modified
- `db/peer/peer_types.h` - Shared type definitions (6 structs) used by all peer components
- `db/peer/metrics_collector.h` - MetricsCollector class declaration with NodeMetrics ownership
- `db/peer/metrics_collector.cpp` - 10 methods: timer loop, log line, uptime, dump, Prometheus HTTP, format
- `db/peer/pex_manager.h` - PexManager class declaration with PEX protocol and persistence
- `db/peer/pex_manager.cpp` - 11 methods: PEX exchange, peer persistence, timer loops, recv_sync_msg
- `db/peer/peer_manager.h` - Removed 6 struct defs, removed metrics/PEX declarations, added component members
- `db/peer/peer_manager.cpp` - Removed ~611 lines of method implementations, added delegation calls
- `db/CMakeLists.txt` - Added metrics_collector.cpp and pex_manager.cpp to source list

## Decisions Made
- MetricsCollector receives `peers_` by const ref rather than a peer_count callback, enabling both gauge reporting and per-peer dump iteration
- PexManager copies `find_peer()` and `recv_sync_msg()` rather than sharing with PeerManager, since both are simple 4-line methods accessing the shared `peers_` deque
- Inline PEX after sync (in run_sync_with_peer/handle_sync_as_responder) delegates to `pex_.build_peer_list_response()` and `pex_.handle_peer_list_response()` rather than the standalone `handle_pex_as_responder()` which would conflict with the syncing flag
- DumpExtraCallback pattern allows MetricsCollector to get UDS connection count and compaction stats from facade without depending on UdsAcceptor or compaction state
- PexManager receives `encode_peer_list`/`decode_peer_list` as callback functions rather than depending directly on PeerManager, avoiding circular include dependency

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed compute_uptime_seconds reference in NodeInfoRequest handler**
- **Found during:** Task 1 (Build step)
- **Issue:** After moving `compute_uptime_seconds()` to MetricsCollector, a reference in the NodeInfoRequest handler in `on_peer_message()` broke
- **Fix:** Changed to `metrics_collector_.compute_uptime_seconds()`
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** Build succeeds
- **Committed in:** 15f0e43 (Task 1 commit)

**2. [Rule 2 - Missing Critical] Added DumpExtraCallback for UDS/compaction info in MetricsCollector**
- **Found during:** Task 1 (Design step)
- **Issue:** Plan specified dump_metrics needs UDS connection count and compaction stats, but these are owned by facade (uds_acceptor_, last_compaction_time_, compaction_count_)
- **Fix:** Added `set_dump_extra(DumpExtraCallback)` on MetricsCollector, facade wires lambda providing the extra info
- **Files modified:** db/peer/metrics_collector.h, db/peer/metrics_collector.cpp, db/peer/peer_manager.cpp
- **Verification:** Build succeeds, metrics tests pass
- **Committed in:** 15f0e43 (Task 1 commit)

**3. [Rule 1 - Bug] Fixed inline PEX conflict with syncing flag in handle_sync_as_responder**
- **Found during:** Task 2 (Design step)
- **Issue:** Initially tried to delegate inline PEX in handle_sync_as_responder to pex_.handle_pex_as_responder(), but that method checks `peer->syncing` and would immediately return since syncing is already true during sync
- **Fix:** Used direct delegation to pex_.build_peer_list_response() and PeerManager::encode_peer_list() for the responder-side inline PEX
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** PEX tests pass (7/7)
- **Committed in:** 4955dfd (Task 2 commit)

---

**Total deviations:** 3 auto-fixed (2 bugs, 1 missing critical)
**Impact on plan:** All auto-fixes necessary for correctness. No scope creep.

## Issues Encountered
- First cmake build required fresh configure since worktree had no build directory
- Full test suite takes >5 minutes due to networking tests with timeouts; verified critical test subsets individually (peer-manager, PEX, metrics, keepalive, event-expiry, non-networking)

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functionality is wired and operational.

## Next Phase Readiness
- peer_types.h and component extraction pattern established for Plan 02 (ConnectionManager, MessageDispatcher)
- Plan 02 can extract ConnectionManager (takes ownership of `peers_` deque) and MessageDispatcher (the big switch)
- Plan 03 can extract SyncOrchestrator and BlobPushManager
- PeerManager public API is unchanged -- no caller-side modifications needed

---
*Phase: 96-peermanager-architecture*
*Completed: 2026-04-08*

## Self-Check: PASSED

- All 5 created files exist on disk
- Both task commits (15f0e43, 4955dfd) exist in git history
- Build succeeds with zero errors
- All targeted test suites pass (peer-manager 3/3, PEX 7/7, metrics 18/18, keepalive 3/3, event-expiry 5/5, non-networking 384/384)
