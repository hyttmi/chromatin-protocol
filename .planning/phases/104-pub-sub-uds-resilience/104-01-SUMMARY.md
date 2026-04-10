---
phase: 104-pub-sub-uds-resilience
plan: 01
subsystem: relay
tags: [websocket, pub-sub, subscription-aggregation, notification-fan-out, reference-counting]

# Dependency graph
requires:
  - phase: 103-uds-multiplexer-protocol-translation
    provides: UdsMultiplexer, RequestRouter, Translator, WsSession AUTHENTICATED path, SessionManager
provides:
  - SubscriptionTracker class with reference-counted namespace subscriptions
  - Subscribe/Unsubscribe interception in WsSession before RequestRouter
  - Notification fan-out from UdsMultiplexer to subscribed WebSocket sessions
  - StorageFull/QuotaExceeded broadcast to all connected sessions
  - Client disconnect subscription cleanup via SessionManager
affects: [104-02-uds-reconnect-pending-cleanup]

# Tech tracking
tech-stack:
  added: []
  patterns: [reference-counted subscription aggregation, translate-once fan-out, u16BE namespace list encoding]

key-files:
  created:
    - relay/core/subscription_tracker.h
    - relay/core/subscription_tracker.cpp
    - relay/tests/test_subscription_tracker.cpp
  modified:
    - relay/ws/ws_session.h
    - relay/ws/ws_session.cpp
    - relay/ws/ws_acceptor.h
    - relay/ws/ws_acceptor.cpp
    - relay/ws/session_manager.h
    - relay/ws/session_manager.cpp
    - relay/core/uds_multiplexer.h
    - relay/core/uds_multiplexer.cpp
    - relay/relay_main.cpp
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt

key-decisions:
  - "SubscriptionTracker as standalone component-per-concern class following Authenticator/RequestRouter pattern"
  - "Direct u16BE namespace list encoding bypassing translator HEX_32_ARRAY (u32BE mismatch per RESEARCH Pitfall 1)"
  - "Translate-once notification fan-out: binary_to_json called once, shared JSON sent to all subscribers"
  - "SessionManager callback pattern for disconnect cleanup -> node Unsubscribe forwarding"

patterns-established:
  - "Reference-counted subscription: namespace->session_set with 0->1 and 1->0 transition forwarding"
  - "Namespace32Hash: first 8 bytes as uint64_t for SHA3-256 output"
  - "Subscribe/Unsubscribe interception: after json_to_binary, before RequestRouter, with co_return bypass"

requirements-completed: [MUX-03, MUX-04]

# Metrics
duration: 10min
completed: 2026-04-10
---

# Phase 104 Plan 01: Subscription Aggregation Summary

**Reference-counted subscription tracker with namespace fan-out, per-client 256-cap, and StorageFull/QuotaExceeded broadcast**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-10T07:17:45Z
- **Completed:** 2026-04-10T07:27:51Z
- **Tasks:** 2
- **Files modified:** 14

## Accomplishments
- SubscriptionTracker class with reference-counted subscribe/unsubscribe and client cleanup
- WsSession interception of Subscribe(19)/Unsubscribe(20) with u16BE wire format, 256 namespace cap
- UdsMultiplexer notification fan-out translates once, sends to all subscribed sessions
- StorageFull(22)/QuotaExceeded(25) broadcast to all connected sessions
- 174 relay tests pass (1171 assertions, 13 new subscription_tracker tests)

## Task Commits

Each task was committed atomically:

1. **Task 1: SubscriptionTracker class with unit tests (TDD)**
   - `bf25c16` (test: add failing tests for SubscriptionTracker -- TDD RED)
   - `3eb3c1a` (feat: implement SubscriptionTracker with reference-counted subscriptions -- TDD GREEN)
2. **Task 2: WsSession interception, UdsMultiplexer fan-out, main() wiring** - `33f4970` (feat)

## Files Created/Modified
- `relay/core/subscription_tracker.h` - SubscriptionTracker class with Namespace32, Namespace32Hash, SubscribeResult, UnsubscribeResult
- `relay/core/subscription_tracker.cpp` - Implementation with reference-counted subscribe/unsubscribe/remove_client/get_subscribers
- `relay/tests/test_subscription_tracker.cpp` - 13 test cases covering forwarding, cap, cleanup, lookup
- `relay/ws/ws_session.h` - Added tracker_ member, parse_namespace_list, encode_namespace_list_u16be
- `relay/ws/ws_session.cpp` - Subscribe/Unsubscribe interception after json_to_binary, before RequestRouter
- `relay/ws/ws_acceptor.h` - Added tracker parameter
- `relay/ws/ws_acceptor.cpp` - Pass tracker to WsSession::create
- `relay/ws/session_manager.h` - Added set_tracker, set_on_namespaces_empty
- `relay/ws/session_manager.cpp` - Disconnect cleanup via tracker_->remove_client + callback
- `relay/core/uds_multiplexer.h` - Added set_tracker, handle_notification
- `relay/core/uds_multiplexer.cpp` - Notification fan-out, StorageFull/QuotaExceeded broadcast
- `relay/relay_main.cpp` - SubscriptionTracker construction and wiring
- `relay/CMakeLists.txt` - Added subscription_tracker.cpp
- `relay/tests/CMakeLists.txt` - Added test_subscription_tracker.cpp

## Decisions Made
- Used direct u16BE encoding in encode_namespace_list_u16be() instead of the translator's HEX_32_ARRAY path which uses u32BE count prefix -- wire format mismatch per RESEARCH Pitfall 1
- SessionManager::set_on_namespaces_empty callback pattern keeps SessionManager decoupled from UdsMultiplexer while allowing Unsubscribe forwarding on client disconnect
- Namespace32Hash reads first 8 bytes as uint64_t via memcpy -- SHA3-256 output has excellent distribution, no further mixing needed

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- SubscriptionTracker is ready for Plan 02's subscription replay after UDS reconnect (get_all_namespaces)
- UdsMultiplexer route_response request_id==0 is fully handled (Notification, StorageFull, QuotaExceeded)
- Plan 02 needs to extend UdsMultiplexer read_loop for reconnect lifecycle (AEAD reset, pending cleanup, subscription replay)

## Known Stubs

None - all functionality is fully wired.

## Self-Check: PASSED

All created files verified present. All commit hashes verified in git log.

---
*Phase: 104-pub-sub-uds-resilience*
*Completed: 2026-04-10*
