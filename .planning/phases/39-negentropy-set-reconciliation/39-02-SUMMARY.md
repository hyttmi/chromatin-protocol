---
phase: 39-negentropy-set-reconciliation
plan: 02
subsystem: sync
tags: [reconciliation, peer-manager, sync-protocol, set-reconciliation, phase-b]

# Dependency graph
requires:
  - phase: 39-negentropy-set-reconciliation
    plan: 01
    provides: Reconciliation module (reconciliation.h/.cpp), wire encode/decode, process_ranges, transport.fbs updates
provides:
  - Full integration of reconciliation into live sync flow (run_sync_with_peer + handle_sync_as_responder)
  - Updated PROTOCOL.md documenting the reconciliation-based Phase B wire format
  - Network-style reconciliation integration tests
  - Bidirectional sync via reconciliation (O(diff) not O(N))
affects: [39-03, sync, peer_manager]

# Tech tracking
tech-stack:
  added: []
  patterns: [final-exchange-via-ReconcileItems, always-reconcile-cursor-skip-only-phase-c, has-fingerprint-termination-check]

key-files:
  created: []
  modified:
    - db/peer/peer_manager.cpp
    - db/PROTOCOL.md
    - db/tests/sync/test_reconciliation.cpp
    - db/tests/test_daemon.cpp

key-decisions:
  - "Final exchange uses ReconcileItems (type 29) to break ItemList echo loop between peers"
  - "Reconciliation always runs for all namespaces; cursor skip only affects Phase C blob requests (not reconciliation itself)"
  - "Network termination detected via has_fingerprint check on received ranges; all Skip/ItemList = final exchange"

patterns-established:
  - "ReconcileItems as final-exchange signal: when received in reconciliation loop, peer is done with namespace"
  - "Always reconcile, cursor skip only in Phase C: ensures bidirectional sync works correctly"
  - "has_fingerprint check for protocol termination: avoids infinite ItemList echo"

requirements-completed: [SYNC-07, SYNC-08]

# Metrics
duration: 35min
completed: 2026-03-19
---

# Phase 39 Plan 02: Sync Integration Summary

**Reconciliation-based Phase B replacing O(N) hash list exchange with O(diff) multi-round range reconciliation in both initiator and responder sync flows**

## Performance

- **Duration:** 35 min
- **Started:** 2026-03-19T09:44:28Z
- **Completed:** 2026-03-19T10:20:22Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Replaced Phase B hash list exchange in both run_sync_with_peer (initiator) and handle_sync_as_responder (responder) with multi-round ReconcileInit/Ranges/Items protocol
- Updated PROTOCOL.md with complete Phase B wire format documentation including all three reconciliation message types
- Added 5 network-style reconciliation integration tests that simulate the exact protocol flow used by peer_manager.cpp
- All 400 tests pass (5 new + 395 existing, zero regressions)

## Task Commits

Each task was committed atomically:

1. **Task 1: Integrate reconciliation into sync flow** - `a214fb7` (feat)
2. **Task 2: Update PROTOCOL.md and add integration tests** - `a7075fc` (feat)

## Files Created/Modified
- `db/peer/peer_manager.cpp` - Replaced Phase B in both initiator and responder with reconciliation protocol; added reconciliation.h include
- `db/PROTOCOL.md` - Replaced "Phase B: Hash Diff" with "Phase B: Set Reconciliation"; documented ReconcileInit/Ranges/Items wire formats; updated message type reference table
- `db/tests/sync/test_reconciliation.cpp` - Added 5 network-style integration tests (identical sets, empty side, disjoint, small diff in large sets, single item each)
- `db/tests/test_daemon.cpp` - Updated comment to reflect reconciliation protocol

## Decisions Made
- **ReconcileItems as final-exchange signal:** When one side receives ranges with no Fingerprint entries (all Skip/ItemList), it extracts the peer's items from ItemList ranges and sends its own items via ReconcileItems (type 29). The other side receives ReconcileItems and breaks from the reconciliation loop. This avoids infinite ItemList echo where each side keeps responding to ItemList with ItemList.
- **Always reconcile, cursor skip only in Phase C:** The old protocol had both sides send hash lists regardless of cursor state, with cursor skip only preventing diff computation. The new protocol must also reconcile cursor-hit namespaces so the peer can discover what it is missing. Cursor skip only suppresses BlobRequests in Phase C.
- **has_fingerprint termination check:** Instead of using empty ranges or round counting, the network protocol detects reconciliation completion by checking whether any received range has mode=Fingerprint. If all ranges are Skip or ItemList, this is the final exchange.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed ItemList echo loop causing protocol hang**
- **Found during:** Task 1 (initial implementation)
- **Issue:** process_ranges responds to ItemList with ItemList, creating infinite back-and-forth over the network. The reconcile_local simulation avoids this by breaking on complete=true, but the network version had no clean termination.
- **Fix:** Added has_fingerprint check to detect final exchange. When all received ranges are Skip/ItemList, extract items and send our items via ReconcileItems instead of calling process_ranges.
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** All daemon e2e tests pass (two-node sync, three-node PEX)
- **Committed in:** a214fb7

**2. [Rule 1 - Bug] Fixed cursor-hit namespaces breaking bidirectional sync**
- **Found during:** Task 1 (e2e test failure)
- **Issue:** Cursor-hit namespaces skipped reconciliation entirely. But the peer might have new data (the cursor reflects the PEER's seq_num, not ours). The old protocol always exchanged hash lists so the peer could compute its diff. Skipping reconciliation prevented the peer from discovering what it was missing.
- **Fix:** Always run reconciliation for all namespaces. Cursor skip only affects Phase C (don't send BlobRequests for cursor-hit namespaces, but peer can still request from us).
- **Files modified:** db/peer/peer_manager.cpp
- **Verification:** Tombstone sync tests pass (tombstone propagates between nodes via sync)
- **Committed in:** a214fb7

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both fixes were necessary for protocol correctness. The ItemList echo was a design gap in the plan's description. The cursor-hit skip was explicitly stated in the plan but incorrect for bidirectional sync.

## Issues Encountered
- Initial implementation had 5 test failures due to the ItemList echo loop and cursor-hit skip. Both were diagnosed from test output and fixed in the same commit.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Full reconciliation-based sync is operational between nodes
- Plan 03 (if any) can build on the reconciliation integration
- Wire protocol is complete and documented in PROTOCOL.md
- 400 tests provide comprehensive coverage

## Self-Check: PASSED

- db/peer/peer_manager.cpp exists: YES
- db/PROTOCOL.md exists: YES
- db/tests/sync/test_reconciliation.cpp exists: YES
- db/tests/test_daemon.cpp exists: YES
- Task 1 commit a214fb7 in git log: YES
- Task 2 commit a7075fc in git log: YES
- ReconcileInit in peer_manager.cpp: YES
- ReconcileInit in PROTOCOL.md: YES
- No HashList in peer_manager.cpp: YES
- Full test suite: 400/400 pass

---
*Phase: 39-negentropy-set-reconciliation*
*Completed: 2026-03-19*
