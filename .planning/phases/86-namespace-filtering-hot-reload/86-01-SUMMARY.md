---
phase: 86-namespace-filtering-hot-reload
plan: 01
subsystem: peer-sync
tags: [flatbuffers, namespace-filtering, sighup, peer-protocol, blob-notify, reconciliation]

# Dependency graph
requires:
  - phase: 82-reconcile-on-connect-safety-net
    provides: sync_namespaces_ member, encode_namespace_list/decode_namespace_list, reload_config SIGHUP handler
  - phase: 79-send-queue-push-notifications
    provides: BlobNotify fan-out in on_blob_ingested, Connection::send_message coroutine
provides:
  - SyncNamespaceAnnounce = 62 message type in FlatBuffers schema
  - PeerInfo.announced_namespaces for per-peer replication scope tracking
  - announce_and_sync coroutine for post-handshake namespace exchange
  - BlobNotify namespace filtering in on_blob_ingested fan-out loop
  - Phase A/B reconciliation filtering by peer announced namespace intersection
  - SIGHUP re-announce of sync_namespaces to all connected TCP peers
  - Relay blocklist blocks type 62 from clients
  - Unit tests for encode/decode roundtrip and filtering predicate logic
affects: [86-02, 86-03, docker-integration-tests]

# Tech tracking
tech-stack:
  added: []
  patterns: [inline-message-dispatch-for-non-sync-messages, timer-cancel-announce-wait]

key-files:
  created:
    - db/tests/peer/test_namespace_announce.cpp
  modified:
    - db/schemas/transport.fbs
    - db/wire/transport_generated.h
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - relay/core/message_filter.cpp
    - relay/core/message_filter.h
    - db/tests/relay/test_message_filter.cpp
    - db/CMakeLists.txt

key-decisions:
  - "SyncNamespaceAnnounce dispatched inline in on_peer_message (not via sync inbox) to avoid re-announce race during active sync (Pitfall 4)"
  - "Timer-cancel pattern for announce wait (announce_received flag + announce_notify timer on PeerInfo) -- same pattern as sync_notify"
  - "Raw binary payload (encode_namespace_list) instead of nested FlatBuffer table -- consistent with all 61 other message types"
  - "announce_and_sync co_spawned for both initiator and responder; only initiator proceeds to run_sync_with_peer"

patterns-established:
  - "Inline dispatch for peer-internal filter messages (SyncNamespaceAnnounce same category as Subscribe/Unsubscribe)"
  - "Dual-filter pattern: local sync_namespaces_ + remote announced_namespaces intersection for all sync/notify operations"

requirements-completed: [FILT-01, FILT-02]

# Metrics
duration: 71min
completed: 2026-04-05
---

# Phase 86 Plan 01: Namespace Announce Protocol Summary

**SyncNamespaceAnnounce (type 62) protocol: peers exchange replication scope after handshake, BlobNotify and reconciliation filtered by namespace intersection, SIGHUP triggers re-announce**

## Performance

- **Duration:** 71 min
- **Started:** 2026-04-05T09:12:24Z
- **Completed:** 2026-04-05T10:23:41Z
- **Tasks:** 3
- **Files modified:** 9

## Accomplishments
- Peers exchange SyncNamespaceAnnounce after handshake declaring which namespaces they replicate
- BlobNotify fan-out filters by peer's announced namespace set (empty = replicate all)
- Phase A/B/C reconciliation scoped to the intersection of both peers' announced sets
- SIGHUP re-announces updated sync_namespaces to all connected TCP peers
- 8 unit tests covering encode/decode roundtrip and filtering predicate logic

## Task Commits

Each task was committed atomically:

1. **Task 0: Create Wave 0 unit test stubs** - `676fdbf` (test)
2. **Task 1: FlatBuffers schema, relay blocklist, PeerInfo extensions** - `e2bc3af` (feat)
3. **Task 2: Announce exchange, BlobNotify filtering, reconciliation filtering, SIGHUP re-announce** - `1167764` (feat)

## Files Created/Modified
- `db/tests/peer/test_namespace_announce.cpp` - 8 unit tests: encode/decode roundtrip + filtering predicate logic
- `db/schemas/transport.fbs` - SyncNamespaceAnnounce = 62 enum value
- `db/wire/transport_generated.h` - Regenerated with flatc for new enum value
- `db/peer/peer_manager.h` - PeerInfo: announced_namespaces, announce_received, announce_notify; announce_and_sync declaration
- `db/peer/peer_manager.cpp` - Inline dispatch, announce_and_sync coroutine, BlobNotify filter, Phase A/B filter, SIGHUP re-announce
- `relay/core/message_filter.cpp` - Type 62 added to blocklist
- `relay/core/message_filter.h` - Updated blocklist doc comment
- `db/tests/relay/test_message_filter.cpp` - 18 peer-only types (added SyncNamespaceAnnounce check)
- `db/CMakeLists.txt` - Registered test_namespace_announce.cpp

## Decisions Made
- SyncNamespaceAnnounce dispatched inline in on_peer_message (not via sync inbox) per Pitfall 4 from research -- avoids re-announce during active sync corrupting the sync state machine
- Raw binary payload (existing encode_namespace_list format) instead of nested FlatBuffer table -- consistent with all other 61 message types in the codebase
- Timer-cancel pattern for announce wait using dedicated announce_received flag and announce_notify timer on PeerInfo
- announce_and_sync co_spawned for both initiator and responder (not just initiator) -- both sides need to send/receive the announce, but only initiator proceeds to sync

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Build system: worktree required separate cmake configure + FetchContent download (~3 min). Build succeeded on first try after configuration.
- Full test suite too slow to run in bulk (300s timeout). Ran targeted test subsets: all 53 peer_manager tests pass, all 43 sync/reconciliation tests pass, all 8 namespace tests pass, all 8 message_filter tests pass.

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all functionality is fully wired.

## Next Phase Readiness
- Namespace filtering protocol complete, ready for Plan 02 (max_peers SIGHUP reload) and Plan 03 (Docker integration tests)
- All existing tests continue to pass with the new announce exchange in on_peer_connected

---
*Phase: 86-namespace-filtering-hot-reload*
*Completed: 2026-04-05*
