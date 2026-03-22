---
phase: 48-access-control-topology
plan: 03
subsystem: testing
tags: [integration-tests, connection-dedup, sighup, acl, topology, docker]

# Dependency graph
requires:
  - phase: 48-access-control-topology
    provides: "Docker compose topologies (acl.yml, dedup.yml), ACL test config patterns"
provides:
  - "Connection dedup in PeerManager::on_peer_connected (namespace-based, deterministic tie-break)"
  - "Server::stop_reconnect() to prevent reconnect-dedup infinite loops"
  - "ACL-05 SIGHUP hot-reload integration test (add key + remove key)"
  - "TOPO-01 connection dedup integration test (peers=1 on mutual peers)"
affects: [49-network-resilience]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Deterministic connection dedup via namespace lexicographic comparison", "stop_reconnect pattern to break reconnect-dedup cycles"]

key-files:
  created:
    - tests/integration/test_acl05_sighup_reload.sh
    - tests/integration/test_topo01_connection_dedup.sh
  modified:
    - db/peer/peer_manager.cpp
    - db/net/server.cpp
    - db/net/server.h

key-decisions:
  - "Connection dedup tie-break: lower namespace_id keeps its initiated connection; both sides reach same conclusion independently"
  - "Server::stop_reconnect() erases reconnect state and cancels timer to prevent infinite reconnect-dedup cycles"
  - "ACL-05 uses 172.29.0.0/16 subnet to avoid conflicts with compose-managed 172.28.0.0/16 networks"
  - "Named Docker volumes preserve node identity across discovery-restart cycle in ACL-05"

patterns-established:
  - "Connection dedup: namespace-based duplicate detection in on_peer_connected with deterministic tie-break"
  - "stop_reconnect: Server-level reconnect suppression for dedup-closed connections"
  - "SIGHUP test pattern: writable bind mount + config rewrite + docker kill -s HUP + auto-reconnect"

requirements-completed: [ACL-05, TOPO-01]

# Metrics
duration: 24min
completed: 2026-03-21
---

# Phase 48 Plan 03: Connection Dedup & SIGHUP/Dedup Tests Summary

**Connection dedup in PeerManager with deterministic namespace tie-break, Server::stop_reconnect() for cycle prevention, ACL-05 SIGHUP hot-reload and TOPO-01 dedup integration tests**

## Performance

- **Duration:** 24 min
- **Started:** 2026-03-21T09:25:07Z
- **Completed:** 2026-03-21T09:50:05Z
- **Tasks:** 1
- **Files modified:** 5

## Accomplishments
- Implemented connection dedup in PeerManager::on_peer_connected with namespace-based duplicate detection and deterministic tie-break (lower namespace keeps its initiator connection)
- Added Server::stop_reconnect() to break infinite reconnect-dedup cycles when mutual bootstrap peers cause repeated duplicate connections
- ACL-05 test verifies SIGHUP hot-reload: adding a key connects a new peer (peers 1->2), removing a key drops the connection (peers 2->1)
- TOPO-01 test verifies two mutual peers produce exactly peers=1 on both sides and sync works on the surviving connection

## Task Commits

Each task was committed atomically:

1. **Task 1: Connection dedup + ACL-05 + TOPO-01** - `f436338` (feat)

## Files Created/Modified
- `db/peer/peer_manager.cpp` - Connection dedup logic in on_peer_connected: namespace comparison, deterministic tie-break, stop_reconnect integration
- `db/net/server.cpp` - stop_reconnect() method, reconnect_loop exit check on erased state
- `db/net/server.h` - stop_reconnect() declaration
- `tests/integration/test_acl05_sighup_reload.sh` - SIGHUP ACL hot-reload test (3-node topology, add key + remove key phases)
- `tests/integration/test_topo01_connection_dedup.sh` - Connection dedup test (2-node mutual-peer topology, peers=1 verification, sync verification)

## Decisions Made
- Connection dedup tie-break uses lexicographic comparison of namespace IDs: the node with the lower namespace_id keeps its initiated (outbound) connection. Since both nodes see the same pair of namespace IDs, they independently close the same duplicate connection.
- Added Server::stop_reconnect() to erase reconnect state and cancel timers for addresses closed by dedup. Without this, the auto-reconnect loop would immediately re-establish the closed connection, creating an infinite reconnect-dedup cycle.
- Added a check in reconnect_loop: if stop_reconnect() erased the state entry, the coroutine exits via co_return.
- ACL-05 test uses 172.29.0.0/16 subnet to avoid conflicts with compose-managed networks (dedup compose uses 172.28.0.0/16).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed infinite reconnect-dedup cycle for mutual bootstrap peers**
- **Found during:** Task 1 (TOPO-01 test)
- **Issue:** After dedup closed the duplicate connection, the auto-reconnect loop immediately re-established it, creating an infinite cycle of connect->dedup->close->reconnect. Both nodes showed endless "duplicate connection" log messages and sync never worked.
- **Fix:** Added Server::stop_reconnect(address) that erases reconnect state and cancels timers. Called from dedup logic for both "keep new" and "close new" paths. Added reconnect_loop exit check when state entry is erased.
- **Files modified:** db/net/server.cpp, db/net/server.h, db/peer/peer_manager.cpp
- **Verification:** TOPO-01 test passes: peers=1 on both nodes, no reconnect cycles, sync works on surviving connection
- **Committed in:** f436338

**2. [Rule 1 - Bug] Fixed pipefail crash in ACL-05 peer count extraction**
- **Found during:** Task 1 (ACL-05 test)
- **Issue:** `docker logs | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+'` with `set -o pipefail` caused script exit when intermediate grep found no matches (no metrics dump yet). The `|| echo "0"` fallback only caught the final command's error.
- **Fix:** Extracted peer count logic into get_peer_count() helper with `|| count="0"` applied to the whole pipeline result.
- **Files modified:** tests/integration/test_acl05_sighup_reload.sh
- **Verification:** ACL-05 test passes reliably

**3. [Rule 3 - Blocking] Fixed Docker network subnet conflict between ACL-05 and compose tests**
- **Found during:** Task 1 (ACL-05 test)
- **Issue:** ACL-05 and docker-compose.dedup.yml both used 172.28.0.0/16, causing "Pool overlaps" errors when both tests' networks existed simultaneously.
- **Fix:** Changed ACL-05 test to use 172.29.0.0/16 subnet.
- **Files modified:** tests/integration/test_acl05_sighup_reload.sh
- **Verification:** Both ACL-05 and TOPO-01 tests pass independently and sequentially

---

**Total deviations:** 3 auto-fixed (2 bugs, 1 blocking)
**Impact on plan:** All fixes necessary for correctness. The reconnect-dedup cycle fix (deviation 1) was the most significant -- it required adding Server::stop_reconnect() which wasn't in the original plan. No scope creep.

## Issues Encountered
- The connection dedup tie-break needed careful coordination with the auto-reconnect system. Simply closing duplicate connections was insufficient because the Server's reconnect_loop immediately re-established them. This required a new Server API (stop_reconnect) to properly break the cycle.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Phase 48 now complete: ACL-01 through ACL-05 and TOPO-01 all have passing integration tests
- Connection dedup code is production-ready for Phase 49+ multi-node topologies
- The stop_reconnect pattern is available for any future scenario where programmatic reconnect suppression is needed

---
*Phase: 48-access-control-topology*
*Completed: 2026-03-21*
