---
phase: 07-peer-discovery
plan: 01
subsystem: peer
tags: [pex, peer-exchange, wire-format, asio, connect-once, discovery]

# Dependency graph
requires:
  - phase: 06-complete-sync-receive-side
    provides: "PeerManager with full bidirectional sync, timer-cancel message queue, Connection"
provides:
  - "PeerListRequest=16 and PeerListResponse=17 wire types in transport.fbs"
  - "Server::connect_once for one-shot outbound connections (no reconnect)"
  - "PEX protocol: encode/decode peer lists, build peer list response, exchange peer lists inline with sync"
  - "PEX timer loop for periodic peer exchange every 5 minutes"
  - "run_pex_with_peer and handle_pex_as_responder coroutines for serialized PEX"
  - "known_addresses_ set for dedup of discovered peers"
affects: [07-02, peer-persistence, e2e-tests]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Inline PEX after sync: PeerListRequest/Response sent within sync coroutine to prevent AEAD nonce desync"
    - "Serialized sends: all send_message calls from a single coroutine per connection, never concurrent co_spawn"
    - "Deque for peers_ to prevent pointer invalidation on push_back during coroutine suspension"
    - "Snapshot iteration: take connection pointer snapshot before iterating peers across co_await points"

key-files:
  created: []
  modified:
    - schemas/transport.fbs
    - src/wire/transport_generated.h
    - src/net/server.h
    - src/net/server.cpp
    - src/peer/peer_manager.h
    - src/peer/peer_manager.cpp
    - tests/peer/test_peer_manager.cpp

key-decisions:
  - "PEX exchange happens inline after sync completes, not via separate co_spawn -- prevents AEAD nonce desync from concurrent sends"
  - "PeerListRequest/PeerListResponse routed through sync_inbox message queue, same as sync messages"
  - "Standalone PEX responder (handle_pex_as_responder) for periodic PEX requests outside of sync flow"
  - "Changed peers_ from std::vector to std::deque to prevent pointer invalidation during push_back"
  - "Connection pointer snapshots in sync_all_peers and request_peers_from_all to avoid iterator invalidation"
  - "Binary big-endian wire encoding for peer lists: [uint16 count][uint16 len, utf8 addr]..."

patterns-established:
  - "Inline protocol extension: PEX messages sent after sync within same coroutine (syncing flag provides mutual exclusion)"
  - "Snapshot iteration pattern: copy connection pointers before iterating with co_await in the loop body"
  - "PEX responder: on_peer_message spawns handle_pex_as_responder only when not syncing, sets syncing flag for duration"

requirements-completed: [DISC-02]

# Metrics
duration: 25min
completed: 2026-03-05
---

# Phase 7 Plan 1: PEX Protocol and Wire Types Summary

**Peer exchange protocol with inline PEX-after-sync, connect_once for discovered peers, and serialized send safety**

## Performance

- **Duration:** ~25 min
- **Started:** 2026-03-05
- **Completed:** 2026-03-05
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Added PeerListRequest/PeerListResponse wire types to transport.fbs
- Implemented Server::connect_once for one-shot outbound connections without reconnect
- Full PEX protocol: encode/decode peer lists, build responses, exchange with peers
- Fixed critical AEAD nonce desync bug by integrating PEX sends into sync coroutines
- Fixed pointer invalidation bug by switching peers_ to std::deque and using snapshot iteration
- PEX unit tests for encode/decode round-trip, empty list, single peer, truncated payload, constants

## Files Created/Modified
- `schemas/transport.fbs` - Added PeerListRequest=16, PeerListResponse=17
- `src/wire/transport_generated.h` - Regenerated from transport.fbs
- `src/net/server.h` - Added connect_once declaration
- `src/net/server.cpp` - Implemented connect_once (no reconnect loop)
- `src/peer/peer_manager.h` - PEX constants, methods, known_addresses_, deque peers_
- `src/peer/peer_manager.cpp` - Full PEX implementation with serialized send safety
- `tests/peer/test_peer_manager.cpp` - 5 PEX unit tests

## Decisions Made
- PEX messages sent inline within sync coroutine (after sync completes) to prevent AEAD nonce desync from concurrent sends on same connection
- PeerListRequest routed through sync_inbox when peer is syncing, spawns handle_pex_as_responder when not
- peers_ changed from std::vector to std::deque to prevent pointer invalidation when new peers connect during coroutine suspension
- sync_all_peers and request_peers_from_all take connection pointer snapshots before iteration

## Deviations from Plan

### Auto-fixed Issues

**1. AEAD nonce desync from concurrent co_spawn sends**
- **Found during:** Test execution after initial implementation
- **Issue:** send_peer_list_request and handle_peer_list_request used co_spawn(detached) to send messages, racing with sync coroutine on the same connection. Both coroutines called send_message concurrently, desynchronizing the AEAD nonce counter and causing decryption failures on the receiver.
- **Fix:** Integrated PEX into sync flow: initiator sends PeerListRequest after sync completes (same coroutine), responder handles it inline. Standalone PEX (periodic timer) uses run_pex_with_peer with syncing flag for mutual exclusion. All send_message calls are serialized per connection.
- **Files modified:** src/peer/peer_manager.h, src/peer/peer_manager.cpp
- **Verification:** All 155 tests pass (586 assertions), zero AEAD decrypt failures

**2. Segfault from pointer invalidation during vector push_back**
- **Found during:** 3-node E2E test after AEAD fix
- **Issue:** peers_ was std::vector. When a new peer connected (push_back) while a coroutine held a PeerInfo* pointer (from find_peer), the vector reallocation invalidated the pointer. Also, iterating peers_ across co_await points was unsafe because push_back could invalidate the iterator.
- **Fix:** Changed peers_ to std::deque (references survive push_back). Added snapshot iteration pattern: copy connection pointers before iterating with co_await.
- **Files modified:** src/peer/peer_manager.h, src/peer/peer_manager.cpp
- **Verification:** All 155 tests pass, no segfaults

---

**Total deviations:** 2 auto-fixed (2 blocking bugs)
**Impact on plan:** Both fixes essential for correctness. AEAD desync was a fundamental concurrency issue with the plan's co_spawn approach. Vector invalidation was a latent bug exposed by dynamic peer addition.

## Issues Encountered
- Plan specified co_spawn(detached) for PEX sends, which races with sync coroutine. Redesigned to inline PEX within sync flow.
- Plan specified std::vector<PeerInfo> which invalidates pointers on push_back. Switched to std::deque.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- PEX protocol fully operational with serialized send safety
- Peer persistence and 3-node E2E test ready to implement (Plan 02)

---
*Phase: 07-peer-discovery*
*Completed: 2026-03-05*
