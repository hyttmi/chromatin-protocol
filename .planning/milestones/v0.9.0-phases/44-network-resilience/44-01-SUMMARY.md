---
phase: 44-network-resilience
plan: 01
subsystem: networking
tags: [reconnect, backoff, jitter, acl, sighup, asio, coroutines]

# Dependency graph
requires:
  - phase: 42-config-cleanup
    provides: cancel_all_timers pattern, config validation
provides:
  - Auto-reconnect with jittered exponential backoff for all outbound peers
  - ACL rejection tracking with extended 600s backoff after 3 rejections
  - SIGHUP reset of reconnect state for immediate retry
  - connect_once now enters reconnect path (discovered peers survive disconnect)
  - Connection::connect_address for address mapping through handshake
affects: [44-02-PLAN, keepalive-timeout, integration-tests]

# Tech tracking
tech-stack:
  added: []
  patterns: [per-address-reconnect-state, value-copy-across-coawait, duplicate-loop-prevention]

key-files:
  created: []
  modified:
    - db/net/server.h
    - db/net/server.cpp
    - db/net/connection.h
    - db/peer/peer_manager.cpp
    - db/tests/net/test_server.cpp
    - db/tests/peer/test_peer_manager.cpp

key-decisions:
  - "Value copies instead of references across co_await to prevent dangling on Server destruction"
  - "Direct method call (notify_acl_rejected) instead of callback pattern for ACL signaling"
  - "connect_address stored on Connection for reliable address mapping through handshake"
  - "Duplicate reconnect_loop prevention via reconnect_state_ membership check"
  - "First reconnect attempt is immediate (no delay) for newly discovered peers"

patterns-established:
  - "Value-copy-across-coawait: Never hold references to map entries across co_await suspension points"
  - "Duplicate coroutine prevention: Check state map before spawning reconnect_loop"

requirements-completed: [CONN-01, CONN-02]

# Metrics
duration: 48min
completed: 2026-03-20
---

# Phase 44 Plan 01: Auto-Reconnect with ACL-Aware Backoff Summary

**Jittered exponential backoff (1s-60s) for all outbound peers with ACL rejection tracking (3 rejections -> 600s extended backoff) and SIGHUP reset**

## Performance

- **Duration:** 48 min
- **Started:** 2026-03-20T05:39:07Z
- **Completed:** 2026-03-20T06:27:35Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- All outbound peers (bootstrap + discovered + persisted) now auto-reconnect with jittered exponential backoff capped at 60s
- ACL-rejecting peers trigger extended 600s backoff after 3 consecutive rejections, preventing tight retry loops
- SIGHUP resets all ACL rejection counters and cancels sleeping reconnect timers for immediate retry
- Fixed handshake_ok bug: ACL rejection no longer resets delay to 1s (was causing retry storms)
- connect_once converted to use reconnect_loop: discovered peers survive disconnection
- 7 new unit tests covering reconnect state logic, ACL signaling, and connect_address mapping

## Task Commits

Each task was committed atomically:

1. **Task 1: Add reconnect state tracking and jitter to Server reconnect_loop** - `c234565` (feat)
2. **Task 2: Wire ACL rejection signal from PeerManager to Server and add SIGHUP reset** - `8f23d25` (feat)

## Files Created/Modified
- `db/net/server.h` - ReconnectState struct, notify_acl_rejected/clear_reconnect_state methods, reconnect_state_ map, RNG, timer tracking
- `db/net/server.cpp` - Jittered reconnect_loop, ACL-aware backoff, immediate first attempt, draining checks, duplicate prevention
- `db/net/connection.h` - connect_address_ field for original address mapping through handshake
- `db/peer/peer_manager.cpp` - ACL rejection signal via server_.notify_acl_rejected(), SIGHUP calls clear_reconnect_state()
- `db/tests/net/test_server.cpp` - 5 new tests: ReconnectState defaults, ACL rejection counting, threshold escalation, clear_reconnect_state, connect_once reconnect path, connect_address round-trip
- `db/tests/peer/test_peer_manager.cpp` - 2 new tests: connect_address empty for inbound, connect_address set on outbound

## Decisions Made
- Used value copies instead of references when accessing reconnect_state_ across co_await suspension points. References to unordered_map entries become dangling when the Server is destroyed while coroutines are still pending. This is a correctness requirement for coroutine-heavy code.
- Chose direct method call (Server::notify_acl_rejected) over callback pattern. PeerManager already owns server_ as a member, so callback indirection adds complexity without benefit.
- Added Connection::connect_address field to map resolved IP:ephemeral_port back to original address. This is a minimal 4-line addition to Connection that cleanly solves the address mismatch between reconnect_loop (original address) and on_peer_connected (resolved address).
- Prevent duplicate reconnect_loop coroutines by checking reconnect_state_ membership before spawning. Without this, PEX discovery could spawn a second loop for an address that already has an active reconnect_loop.
- First reconnect attempt connects immediately (no timer wait) when delay_sec is 1. This preserves the same latency characteristics as the old connect_once for newly discovered peers.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed dangling reference across co_await suspension points**
- **Found during:** Task 2 (integration testing)
- **Issue:** reconnect_loop held `auto& state = reconnect_state_[address]` across co_await. When Server was destroyed during test teardown, the reference became dangling, causing SIGSEGV.
- **Fix:** Changed to value copies at each loop iteration and direct map access for mutations. No reference held across any co_await point.
- **Files modified:** db/net/server.cpp
- **Verification:** All tests pass, no SIGSEGV on teardown
- **Committed in:** 8f23d25 (Task 2 commit)

**2. [Rule 1 - Bug] Added reconnect timer cancellation in Server::stop()**
- **Found during:** Task 2 (integration testing)
- **Issue:** Server::stop() set draining_=true but didn't cancel sleeping reconnect timers. Coroutines waiting on timers wouldn't wake up until timer expired, causing use-after-free during destruction.
- **Fix:** Added for-loop in stop() to cancel all sleeping reconnect timers before starting drain.
- **Files modified:** db/net/server.cpp
- **Verification:** Clean shutdown in all daemon tests
- **Committed in:** 8f23d25 (Task 2 commit)

**3. [Rule 1 - Bug] Added draining checks after async_resolve and async_connect**
- **Found during:** Task 2 (integration testing)
- **Issue:** reconnect_loop only checked draining_ at timer cancellation. If draining_ was set during resolve or connect, the coroutine would continue and access destroyed state.
- **Fix:** Added `if (draining_) co_return;` after both async_resolve and async_connect.
- **Files modified:** db/net/server.cpp
- **Verification:** Clean shutdown under concurrent reconnect activity
- **Committed in:** 8f23d25 (Task 2 commit)

**4. [Rule 2 - Missing Critical] Added duplicate reconnect_loop prevention**
- **Found during:** Task 2 (integration testing)
- **Issue:** connect_once could spawn multiple reconnect_loop coroutines for the same address (e.g., PEX discovers an address that bootstrap already reconnects to). Multiple loops caused excessive connections and memory pressure.
- **Fix:** Check reconnect_state_.count(address) before spawning. Pre-populate entry to prevent race between co_spawn and coroutine start.
- **Files modified:** db/net/server.cpp
- **Verification:** PEX test no longer causes memory exhaustion
- **Committed in:** 8f23d25 (Task 2 commit)

---

**Total deviations:** 4 auto-fixed (3 bugs, 1 missing critical)
**Impact on plan:** All auto-fixes necessary for correctness. The coroutine lifetime bugs would have caused crashes in production. No scope creep.

## Issues Encountered
- Pre-existing SIGSEGV in "three nodes: peer discovery via PEX" test (test_daemon.cpp:296). Confirmed failing on master before our changes. Logged to deferred-items.md. Not caused by this plan's changes.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Reconnect infrastructure in place for keepalive timeout (44-02)
- clear_reconnect_state() available for future SIGHUP-driven features
- connect_address field available for any feature needing original address mapping

## Self-Check: PASSED

All 6 modified files verified present. Both task commits (c234565, 8f23d25) verified in git log. Build succeeds. 446/446 tests pass (excluding pre-existing PEX failure).

---
*Phase: 44-network-resilience*
*Completed: 2026-03-20*
