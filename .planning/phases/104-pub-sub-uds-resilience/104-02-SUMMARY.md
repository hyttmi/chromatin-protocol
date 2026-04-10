---
phase: 104-pub-sub-uds-resilience
plan: 02
subsystem: relay
tags: [uds, reconnect, subscription-replay, bulk-fail, aead-reset, pub-sub]

# Dependency graph
requires:
  - phase: 104-pub-sub-uds-resilience plan 01
    provides: SubscriptionTracker class, notification fan-out, Subscribe/Unsubscribe interception
provides:
  - RequestRouter::bulk_fail_all() for pending request cleanup on disconnect
  - UDS D-14 ordered reconnect lifecycle (bulk fail -> AEAD reset -> close -> clear queue -> reconnect)
  - Subscription replay via u16BE batched Subscribe after reconnect
  - UDS reconnect with infinite jittered backoff
affects: [105-hardening-observability]

# Tech tracking
tech-stack:
  added: []
  patterns: [D-14 ordered disconnect cleanup, subscription replay after reconnect]

key-files:
  created: []
  modified:
    - relay/core/request_router.h
    - relay/core/request_router.cpp
    - relay/core/uds_multiplexer.h
    - relay/core/uds_multiplexer.cpp
    - relay/tests/test_request_router.cpp
    - relay/tests/test_subscription_tracker.cpp

key-decisions:
  - "D-14 ordered cleanup on both read_loop and drain_send_queue failure paths"
  - "bulk_fail_pending_requests sends node_disconnected error with original client request_id"
  - "replay_subscriptions uses u16BE count prefix matching PROTOCOL.md Subscribe format"
  - "Empty subscription set skips replay (no empty Subscribe sent to node)"

patterns-established:
  - "D-14 disconnect ordering: bulk fail pending -> AEAD reset -> socket close -> queue clear -> reconnect"
  - "Subscription replay: get_all_namespaces -> encode u16BE -> TransportCodec::encode -> send"

requirements-completed: [MUX-05, MUX-06, MUX-07]

# Metrics
duration: 5min
completed: 2026-04-10
---

# Phase 104 Plan 02: UDS Reconnect Lifecycle Summary

**RequestRouter bulk_fail_all + UDS D-14 ordered reconnect lifecycle with AEAD reset, send queue cleanup, and u16BE subscription replay after handshake**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-10T07:31:33Z
- **Completed:** 2026-04-10T07:37:09Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- RequestRouter::bulk_fail_all() iterates pending map, invokes per-entry callback, clears map
- UDS disconnect triggers D-14 ordered cleanup in both read_loop and drain_send_queue paths: bulk fail pending requests -> AEAD key/counter reset -> socket close -> send queue clear -> infinite reconnect
- Subscription replay after successful reconnect sends batched u16BE Subscribe with all active namespaces from SubscriptionTracker
- 12 new tests covering UDS reconnect contract, subscription replay, notification fan-out, broadcast separation, and u16BE wire format

## Task Commits

Each task was committed atomically:

1. **Task 1: RequestRouter bulk_fail_all and UdsMultiplexer reconnect lifecycle** - `900f5d9` (feat)
2. **Task 2: UDS reconnect, subscription replay, and notification fan-out tests** - `223755b` (test)

## Files Created/Modified
- `relay/core/request_router.h` - Added bulk_fail_all() method with std::function callback
- `relay/core/request_router.cpp` - Implemented bulk_fail_all: iterate pending_, invoke callback, clear
- `relay/core/uds_multiplexer.h` - Added replay_subscriptions() and bulk_fail_pending_requests() private methods
- `relay/core/uds_multiplexer.cpp` - D-14 ordered disconnect cleanup in read_loop and drain_send_queue, subscription replay after connect_loop handshake, nlohmann/json error construction for bulk fail
- `relay/tests/test_request_router.cpp` - 2 bulk_fail tests
- `relay/tests/test_subscription_tracker.cpp` - 12 new tests: uds_reconnect, subscription_replay, notification, broadcast, wire format

## Decisions Made
- D-14 ordered cleanup applied to BOTH read_loop failure and drain_send_queue failure paths (plan only specified read_loop; drain needed same treatment per Pitfall 2)
- replay_subscriptions() exits early on empty namespace set (Pitfall 5 prevention)
- bulk_fail_pending_requests() checks get_session() for nullptr before sending error (Pitfall 3 handling)

## Deviations from Plan
None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 104 complete: subscription aggregation (Plan 01) + UDS reconnect lifecycle (Plan 02) both implemented
- All 188 relay tests pass (1322 assertions)
- Ready for Phase 105 (hardening & observability)

---
*Phase: 104-pub-sub-uds-resilience*
*Completed: 2026-04-10*
