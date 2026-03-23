---
phase: 59-relay-core
plan: 01
subsystem: relay
tags: [message-filter, identity-adapter, relay-session, c++20, coroutines, uds]

# Dependency graph
requires:
  - phase: 58-relay-scaffolding
    plan: 02
    provides: "RelayIdentity class, relay CMake targets, CLI entry point"
provides:
  - "NodeIdentity::from_keys() factory for raw key construction"
  - "RelayIdentity::to_node_identity() adapter for Connection compatibility"
  - "is_client_allowed() message type filter (16 allowed, default-deny)"
  - "type_name() human-readable message type names for logging"
  - "RelaySession class with bidirectional client-node forwarding"
  - "8 unit tests (49 assertions) for message filter and identity adapter"
affects: [59-02-relay-event-loop]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Message filter: switch-based allow-list with default-deny"
    - "RelaySession: shared_from_this + co_spawn for coroutine message forwarding"
    - "Node on_ready gates message forwarding until TrustedHello completes"

key-files:
  created:
    - relay/core/message_filter.h
    - relay/core/message_filter.cpp
    - relay/core/relay_session.h
    - relay/core/relay_session.cpp
    - db/tests/relay/test_message_filter.cpp
  modified:
    - db/identity/identity.h
    - db/identity/identity.cpp
    - relay/identity/relay_identity.h
    - relay/identity/relay_identity.cpp
    - relay/CMakeLists.txt
    - db/CMakeLists.txt

key-decisions:
  - "Message forwarding gated on node on_ready callback to avoid sending before TrustedHello completes"
  - "Blocked message type triggers immediate client disconnect per D-01 (not just drop)"

patterns-established:
  - "RelaySession: co_spawn for async send in synchronous on_message callback"
  - "Identity adapter: from_keys/to_node_identity bridge between relay and db identity types"

requirements-completed: [RELAY-01, RELAY-02, RELAY-03, RELAY-04]

# Metrics
duration: 8min
completed: 2026-03-23
---

# Phase 59 Plan 01: Relay Core Building Blocks Summary

**Identity adapter, default-deny message type filter (16 client types allowed), and RelaySession with bidirectional UDS forwarding (8 tests, 49 assertions)**

## Performance

- **Duration:** 8 min
- **Started:** 2026-03-23T15:40:25Z
- **Completed:** 2026-03-23T15:48:33Z
- **Tasks:** 2
- **Files modified:** 11

## Accomplishments
- NodeIdentity::from_keys() factory enables relay identity to produce Connection-compatible NodeIdentity
- Message type filter allows exactly 16 client operation types, default-denies all 21+ other types including peer-only, handshake, and unknown values
- RelaySession manages paired client TCP + node UDS connections with bidirectional message forwarding
- Session lifecycle: UDS connect, TrustedHello handshake, message forwarding gates on node ready, teardown on any disconnect
- Blocked message types trigger warn-level logging (D-07) and immediate client disconnect (D-01)
- Node UDS loss immediately disconnects client (D-04)

## Task Commits

Each task was committed atomically (TDD RED/GREEN + feat):

1. **Task 1 RED: Failing tests for message filter and identity adapter** - `c55ab86` (test)
2. **Task 1 GREEN: Implement identity adapter and message filter** - `0c55d00` (feat)
3. **Task 2: RelaySession with bidirectional forwarding** - `0585e1f` (feat)

## Files Created/Modified
- `relay/core/message_filter.h` - is_client_allowed() and type_name() declarations
- `relay/core/message_filter.cpp` - Switch-based allow-list with default-deny, EnumNameTransportMsgType wrapper
- `relay/core/relay_session.h` - RelaySession class with create/start/stop/on_close interface
- `relay/core/relay_session.cpp` - Bidirectional forwarding, message filtering, session lifecycle, logging
- `db/tests/relay/test_message_filter.cpp` - 8 test cases covering all 37 message types + identity adapter
- `db/identity/identity.h` - Added from_keys() static factory method
- `db/identity/identity.cpp` - Implemented from_keys() using Signer::from_keypair
- `relay/identity/relay_identity.h` - Added to_node_identity() method, included identity.h
- `relay/identity/relay_identity.cpp` - Implemented to_node_identity() via from_keys()
- `relay/CMakeLists.txt` - Added core/message_filter.cpp and core/relay_session.cpp
- `db/CMakeLists.txt` - Added tests/relay/test_message_filter.cpp

## Decisions Made
- Message forwarding is gated on node_conn_->on_ready() callback -- client messages are not forwarded until TrustedHello handshake with the node completes, preventing sends to an unauthenticated UDS connection
- Blocked message type triggers immediate client disconnect (D-01) rather than silently dropping -- a client sending peer-only types is either buggy or probing

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Minor: unscoped C enum values from FlatBuffers-generated TransportMsgType required `using namespace chromatindb::wire` in message_filter.cpp to bring enum constants into scope. Fixed in GREEN phase.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Relay core components (message filter, session, identity adapter) complete and tested
- Ready for Plan 02 to wire into relay event loop (TCP accept, per-client session spawning, shutdown)
- 34 relay tests pass (15 config + 11 identity + 8 message filter)

## Self-Check: PASSED

- All 5 created files verified present
- All 3 commit hashes (c55ab86, 0c55d00, 0585e1f) verified in git log

---
*Phase: 59-relay-core*
*Completed: 2026-03-23*
