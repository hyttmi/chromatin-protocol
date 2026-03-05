---
phase: 06-complete-sync-receive-side
plan: 02
subsystem: testing
tags: [e2e, sync, blob-replication, catch2, asio]

# Dependency graph
requires:
  - phase: 06-complete-sync-receive-side (plan 01)
    provides: "Bidirectional sync flow with message queue, run_sync_with_peer, handle_sync_as_responder"
provides:
  - "Strict E2E sync verification proving SYNC-01, SYNC-02, SYNC-03"
  - "Fixed on_connected callback timing (on_ready pattern)"
  - "Fixed Phase C blob exchange deadlock"
  - "Fixed sync-on-connect race condition (initiator-only)"
affects: [phase-07, phase-08]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "on_ready callback: fire post-handshake, pre-message-loop for correct connection lifecycle"
    - "Initiator-only sync-on-connect: prevents dual SyncRequest race"
    - "Phase C send-all-then-process: avoids BlobRequest/BlobTransfer deadlock"

key-files:
  created: []
  modified:
    - tests/test_daemon.cpp
    - src/net/connection.h
    - src/net/connection.cpp
    - src/net/server.cpp
    - src/peer/peer_manager.cpp

key-decisions:
  - "on_ready callback pattern for post-handshake notification (not post-run)"
  - "Only TCP initiator triggers sync-on-connect to avoid dual SyncRequest race"
  - "Phase C sends all BlobRequests upfront, then processes mixed BlobTransfer/BlobRequest responses"
  - "Guard SyncRequest handler with syncing flag to prevent concurrent sync coroutines"

patterns-established:
  - "on_ready: Connection notifies PeerManager after handshake, before message loop"
  - "Initiator-only sync: outbound side initiates sync, inbound waits for SyncRequest"
  - "Real timestamps in E2E tests: use std::time(nullptr) so blobs are not expired during sync"

requirements-completed: [SYNC-01, SYNC-02, SYNC-03]

# Metrics
duration: 11min
completed: 2026-03-05
---

# Phase 6 Plan 02: Strengthen E2E Sync Tests Summary

**Strict E2E tests proving bidirectional blob sync (SYNC-02), hash-list diff (SYNC-01), and expired blob filtering (SYNC-03), with bug fixes for on_connected timing, sync race condition, and Phase C deadlock**

## Performance

- **Duration:** 11 min
- **Started:** 2026-03-05T15:00:39Z
- **Completed:** 2026-03-05T15:12:15Z
- **Tasks:** 1
- **Files modified:** 5

## Accomplishments
- E2E test "two nodes sync blobs end-to-end" verifies bidirectional blob replication with strict assertions
- E2E test "expired blobs not synced" verifies expired blobs are filtered while valid blobs replicate
- Fixed critical bug where on_connected callback fired AFTER connection closed (never during lifetime)
- Fixed Phase C deadlock where both sides sent BlobRequest and waited for BlobTransfer simultaneously
- Fixed sync-on-connect race where both sides sent SyncRequest causing message interleaving
- All 149 tests pass (568 assertions) with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Rewrite E2E sync tests with strict blob verification** - `b47d433` (feat)

**Plan metadata:** pending (docs: complete plan)

## Files Created/Modified
- `tests/test_daemon.cpp` - Rewrote E2E sync tests with strict bidirectional blob verification and expired blob filtering with control blob
- `src/net/connection.h` - Added on_ready callback, is_initiator() accessor, remote_address() accessor
- `src/net/connection.cpp` - Call ready_cb_ after handshake succeeds, before message_loop
- `src/net/server.cpp` - Use on_ready instead of calling on_connected_ after run() returns; fix reconnect_loop lifecycle
- `src/peer/peer_manager.cpp` - Initiator-only sync-on-connect; guard SyncRequest handler; fix Phase C deadlock with send-all-then-process pattern

## Decisions Made
- **on_ready callback pattern:** Added a new `on_ready` callback to Connection that fires between handshake and message_loop. The previous approach called `on_connected_` after `conn->run()` returned, which only happens after the connection closes -- meaning PeerManager never set up message routing during the connection's lifetime.
- **Initiator-only sync-on-connect:** Only the outbound (initiator) side triggers sync on connect. Previously both sides sent SyncRequest simultaneously, causing message interleaving and inbox corruption between competing sync coroutines.
- **Phase C send-all-then-process:** Changed blob exchange to send all BlobRequests first, then process incoming messages (both BlobTransfer and BlobRequest). The previous pattern of send-request-then-wait-for-response deadlocked when both sides sent BlobRequest simultaneously and both waited for BlobTransfer.
- **Real timestamps in tests:** Changed E2E test blobs from timestamp=9000 to std::time(nullptr). With the small timestamp, blobs were treated as expired by the sync protocol's expiry filter (9000 + 604800 << current_time), so they were never included in hash lists.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed on_connected callback timing**
- **Found during:** Task 1 (E2E test verification)
- **Issue:** Server called on_connected_ after conn->run() returned, which only happens when the connection closes. PeerManager never received the connected callback during the connection's lifetime, so message routing was never set up and sync never triggered.
- **Fix:** Added on_ready callback to Connection, called between handshake and message_loop. Updated Server to use on_ready for both inbound and outbound connections.
- **Files modified:** src/net/connection.h, src/net/connection.cpp, src/net/server.cpp
- **Verification:** "Connected to peer" log messages now appear; sync proceeds after handshake
- **Committed in:** b47d433 (Task 1 commit)

**2. [Rule 1 - Bug] Fixed sync-on-connect race condition**
- **Found during:** Task 1 (E2E test verification)
- **Issue:** Both sides of a connection called run_sync_with_peer on connect, both sending SyncRequest simultaneously. Each received the other's SyncRequest and spawned handle_sync_as_responder, creating two competing sync coroutines per peer sharing the same inbox, causing message confusion and AEAD nonce desync.
- **Fix:** Only the initiator (outbound) side triggers sync on connect; added syncing flag check to SyncRequest handler.
- **Files modified:** src/peer/peer_manager.cpp
- **Verification:** Clean sync logs, no AEAD errors, no double-free
- **Committed in:** b47d433 (Task 1 commit)

**3. [Rule 1 - Bug] Fixed Phase C blob exchange deadlock**
- **Found during:** Task 1 (E2E test verification)
- **Issue:** Both sides sent BlobRequest and immediately waited for BlobTransfer. Since both were waiting and neither was responding to the other's BlobRequest, neither side received blobs (0 blobs transferred despite diff detecting missing blobs).
- **Fix:** Changed Phase C to send all BlobRequests upfront, then process incoming messages in a loop that handles both BlobTransfer responses and BlobRequest from the peer.
- **Files modified:** src/peer/peer_manager.cpp
- **Verification:** "received 1 blobs, sent 1 blobs" in sync logs; E2E assertions pass
- **Committed in:** b47d433 (Task 1 commit)

**4. [Rule 1 - Bug] Fixed expired blob timestamps in E2E tests**
- **Found during:** Task 1 (E2E test verification)
- **Issue:** Test blobs used timestamp=9000 with ttl=604800. Since 9000+604800=613800 << current_time (~1.77 billion), blobs were treated as expired by collect_namespace_hashes() and excluded from sync hash lists.
- **Fix:** Changed to std::time(nullptr) for valid blob timestamps so they are not considered expired during sync.
- **Files modified:** tests/test_daemon.cpp
- **Verification:** Hash lists now include valid blobs; sync transfers them successfully
- **Committed in:** b47d433 (Task 1 commit)

---

**Total deviations:** 4 auto-fixed (4 bugs)
**Impact on plan:** All auto-fixes were necessary for the sync protocol to work correctly in E2E tests. The bugs existed in 06-01's implementation but were not caught because the old tests used placeholder assertions (REQUIRE >= 0). No scope creep.

## Issues Encountered
None beyond the auto-fixed bugs above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Sync protocol fully verified end-to-end with strict assertions
- SYNC-01 (hash-list diff), SYNC-02 (bidirectional union), SYNC-03 (expired blob filtering) all proven
- Phase 6 complete, ready for Phase 7

## Self-Check: PASSED

All files verified present, commit b47d433 found, 149 tests pass (568 assertions).

---
*Phase: 06-complete-sync-receive-side*
*Completed: 2026-03-05*
