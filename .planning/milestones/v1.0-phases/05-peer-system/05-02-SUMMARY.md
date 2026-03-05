# Plan 05-02 Summary: PeerManager with Sync Orchestration

**Status:** Complete
**Duration:** ~15 min (executed in parallel with Plan 05-03)

## What was built

- Extended Server with PeerManager integration callbacks (set_on_connected, set_on_disconnected, set_accept_filter)
- Added `remote_address()` to Connection for peer identification
- Created PeerManager class: peer tracking, connection limits, sync-on-connect, periodic sync timer, strike system
- Fixed coroutine lifetime bug in Server (lambda invocation vs callable passing to co_spawn)
- Fixed iterator invalidation in Server::drain() by snapshotting connections vector
- Tests: 3 test cases, 5 assertions

## Key decisions

- PeerManager owns Server as a member, wires callbacks in constructor
- Sync orchestration uses coroutines: sync_with_peer runs the full protocol as initiator
- Bootstrap peer detection uses address-to-pubkey mapping via Server's connect_to_peer addresses
- Strike system is simple counter per PeerInfo, resets on reconnect
- Server's reconnect_loop only triggers after handshake succeeds (prevents double-reconnect)

## Files

### key-files
created:
- src/peer/peer_manager.h
- src/peer/peer_manager.cpp
- tests/peer/test_peer_manager.cpp

modified:
- src/net/server.h (added callbacks, accept filter)
- src/net/server.cpp (integrated callbacks, fixed coroutine lifetime, fixed drain iterator invalidation)
- src/net/connection.h (added remote_address())
- src/net/connection.cpp (capture remote endpoint in constructor)
- CMakeLists.txt

## Bug fixes

- **Coroutine lifetime (SIGSEGV):** `co_spawn(ioc, [captures]() -> awaitable<void> { ... }(), detached)` -- the trailing `()` invokes the lambda immediately, creating a coroutine whose frame may reference the destroyed lambda captures. Fixed by passing the lambda as a callable without `()`.
- **Iterator invalidation (SIGSEGV):** `drain()` iterated over `connections_` while `close_gracefully()` triggered `close_cb_` which called `remove_connection()`, modifying the vector during iteration. Fixed by snapshotting the vector before iterating.
- **Double reconnect:** `connect_to_peer` entered `reconnect_loop` on handshake failure AND `on_close` also spawned `reconnect_loop`. Fixed by only setting reconnect in `on_close` after handshake succeeds.

## Test results

```
All tests passed (563 assertions in 149 test cases)
```

## Self-Check: PASSED
- [x] PeerManager enforces max_peers connection limit
- [x] Sync triggered on connect (SYNC-01, SYNC-02)
- [x] Periodic sync timer functional
- [x] Strike system constants defined (STRIKE_THRESHOLD=10, STRIKE_COOLDOWN_SEC=300)
- [x] Bootstrap peers reconnected via Server's reconnect_loop
- [x] Non-bootstrap peers not reconnected
- [x] Sync summary logged at info level per round
- [x] No regressions in existing tests
