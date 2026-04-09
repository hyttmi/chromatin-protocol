---
phase: 102-authentication-json-schema
plan: 01
subsystem: auth
tags: [ml-dsa-87, liboqs, websocket, challenge-response, json, asio, coroutines]

# Dependency graph
requires:
  - phase: 101-websocket-transport
    provides: WsSession, WsAcceptor, SessionManager, ws_frame, core::Session send queue
provides:
  - Authenticator class with ML-DSA-87 challenge-response verification and ACL
  - WsSession AWAITING_AUTH -> AUTHENTICATED state machine
  - Configurable max_connections with SIGHUP reload
  - allowed_client_keys ACL with SIGHUP reload
affects: [102-02-PLAN, 103-uds-translation, 104-subscription-routing]

# Tech tracking
tech-stack:
  added: []
  patterns: [auth-state-machine, thread-pool-offload-via-asio-post, hex-encode-decode-for-json-identity-fields]

key-files:
  created:
    - relay/core/authenticator.h
    - relay/core/authenticator.cpp
    - relay/tests/test_authenticator.cpp
  modified:
    - relay/ws/ws_session.h
    - relay/ws/ws_session.cpp
    - relay/ws/ws_acceptor.h
    - relay/ws/ws_acceptor.cpp
    - relay/config/relay_config.h
    - relay/config/relay_config.cpp
    - relay/relay_main.cpp
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt
    - relay/tests/test_relay_config.cpp

key-decisions:
  - "Auth offload via asio::post(ioc) to any thread pool thread, then post back to session executor"
  - "Hex encoding for all identity fields in JSON (pubkey, signature, namespace hash)"
  - "Fire-and-forget send_json via co_spawn + enqueue for non-coroutine callers"

patterns-established:
  - "SessionState enum: AWAITING_AUTH -> AUTHENTICATED gates all message processing"
  - "Thread pool offload pattern: co_await asio::post(ioc_), do CPU work, co_await asio::post(executor_)"
  - "send_json helper: fire-and-forget JSON via co_spawn + session_.enqueue"
  - "from_hex/to_hex as file-local static helpers for hex encode/decode"

requirements-completed: [AUTH-01, AUTH-02, AUTH-03, AUTH-04, SESS-03]

# Metrics
duration: 15min
completed: 2026-04-09
---

# Phase 102 Plan 01: Authentication Summary

**ML-DSA-87 challenge-response auth over WebSocket with ACL, configurable connection cap, and SIGHUP reload**

## Performance

- **Duration:** 15 min
- **Started:** 2026-04-09T15:44:40Z
- **Completed:** 2026-04-09T15:59:26Z
- **Tasks:** 2
- **Files modified:** 13

## Accomplishments
- Authenticator class with ML-DSA-87 verify, 32-byte RAND_bytes challenge generation, SHA3-256 namespace derivation, and thread-safe ACL with SIGHUP reload
- WsSession gains AWAITING_AUTH -> AUTHENTICATED state machine: challenge sent immediately on WS upgrade, JSON challenge_response parsed and verified with thread pool offload, 10s auth timeout, Close(4001) on any auth failure
- WsAcceptor uses configurable max_connections from config (replaces hardcoded 1024), SIGHUP-reloadable via set_max_connections()
- relay_main.cpp creates Authenticator with ACL from config, SIGHUP handler reloads allowed_client_keys and max_connections
- 72 relay tests passing (9 authenticator + 7 new config + 56 existing)

## Task Commits

Each task was committed atomically:

1. **Task 1: Authenticator class, config extensions, and unit tests** - `698d6d0` (feat)
2. **Task 2: WsSession auth state machine, acceptor connection cap, and SIGHUP reload** - `e829563` (feat)

## Files Created/Modified
- `relay/core/authenticator.h` - Authenticator class: generate_challenge, verify, reload_allowed_keys, AuthResult, ArrayHash32
- `relay/core/authenticator.cpp` - ML-DSA-87 verify via OQS_SIG_verify, SHA3-256 namespace hash, RAND_bytes nonce, ACL check
- `relay/config/relay_config.h` - Added max_connections (uint32_t, default 1024) and allowed_client_keys (vector of hex strings)
- `relay/config/relay_config.cpp` - Parse and validate new config fields (hex format, length, max_connections >= 1)
- `relay/ws/ws_session.h` - SessionState enum, CLOSE_AUTH_FAILURE=4001, auth_timer_, challenge_, client identity storage
- `relay/ws/ws_session.cpp` - Auth state machine: challenge send, JSON parse, hex decode, thread pool offload, state transition, idle timer after auth
- `relay/ws/ws_acceptor.h` - Configurable max_connections_ replaces hardcoded constant, set_max_connections(), Authenticator& reference
- `relay/ws/ws_acceptor.cpp` - Pass authenticator and ioc to WsSession::create(), use max_connections_ in accept_loop
- `relay/relay_main.cpp` - Build Authenticator ACL from config, pass to WsAcceptor, SIGHUP reloads ACL + max_connections
- `relay/CMakeLists.txt` - Added core/authenticator.cpp to library sources
- `relay/tests/test_authenticator.cpp` - 9 unit tests: challenge gen, size checks, valid/invalid verify, ACL accept/reject, reload, has_acl
- `relay/tests/test_relay_config.cpp` - 7 new tests: max_connections parse/default, allowed_client_keys parse/validate/default
- `relay/tests/CMakeLists.txt` - Added test_authenticator.cpp to test sources

## Decisions Made
- Auth verification offloaded via `asio::post(ioc_, use_awaitable)` to the existing hardware_concurrency() thread pool, then posted back to session executor -- same double-post pattern as node's verify_with_offload
- Hex encoding for all identity fields in JSON (5184-char pubkey, 9254-char signature, 64-char namespace hash) -- accepted large payload for consistency with "hex for identity fields" convention from D-19
- send_json() implemented as fire-and-forget via co_spawn + session_.enqueue() since it's called from non-coroutine contexts (send_challenge, handle_auth_message error paths)
- Auth timer and idle timer are separate: auth_timer_ runs during AWAITING_AUTH, idle_timer_ starts only after AUTHENTICATED (Pitfall 4 from context)
- `first_message_received_` removed since auth timer now gates the pre-auth phase (idle timer only relevant post-auth)

## Deviations from Plan
None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Authenticator class is standalone and testable, ready for Plan 02 to reference
- WsSession state machine gates all post-auth messages, ready for Plan 02's message filter and type registry
- relay/translate/ directory has .gitkeep, ready for type_registry.h and json_schema.h
- All 72 relay tests pass, relay binary compiles cleanly

## Self-Check: PASSED

All created files verified, both commit hashes found, all key patterns confirmed in source.

---
*Phase: 102-authentication-json-schema*
*Completed: 2026-04-09*
