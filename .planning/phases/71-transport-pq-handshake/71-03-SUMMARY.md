---
phase: 71-transport-pq-handshake
plan: 03
subsystem: transport
tags: [integration-tests, kvm-swarm, pq-handshake, ping-pong, python-sdk]
status: complete
---

## Summary

End-to-end integration tests validating the full PQ handshake, AEAD-encrypted Ping/Pong, and connection lifecycle against the live chromatindb relay on the KVM swarm.

## What Was Built

- `sdk/python/tests/test_integration.py` — 4 integration tests marked `@pytest.mark.integration`
  - `test_handshake_connect_disconnect` — Full PQ handshake + Goodbye lifecycle
  - `test_ping_pong` — 3 sequential Ping/Pong to verify nonce counter advancement
  - `test_multiple_connections` — Two sequential connections with different identities
  - `test_handshake_timeout` — Timeout on non-routable IP raises HandshakeError

## Key Decisions

- D-01: Tests skip automatically via `_relay_reachable()` TCP probe when relay offline
- D-02: Default relay port corrected from 4433 to 4201 (matching relay_config.h default)
- D-03: Ping/Pong fixed — C++ relay sends Pong with request_id=0, SDK now uses dedicated `send_ping()` with FIFO pending queue instead of `send_request()` request_id matching
- D-04: TCP connect timeout added — `open_connection` now wrapped in `wait_for(timeout)` alongside handshake

## Bugs Fixed During Verification

1. **Wrong default port** — SDK hardcoded 4433, relay default is 4201
2. **Ping hang** — `send_request()` waited for Pong with matching request_id, but relay sends request_id=0. Fixed with dedicated `Transport.send_ping()` using `_pending_pings` deque
3. **Timeout hang** — `asyncio.open_connection` to non-routable IP wasn't covered by timeout. Now both TCP connect and handshake are independently timed

## Key Files

### Created
- `sdk/python/tests/test_integration.py` — 4 integration tests

### Modified
- `sdk/python/chromatindb/_transport.py` — Added `send_ping()`, `_pending_pings` deque, updated reader loop Pong dispatch
- `sdk/python/chromatindb/client.py` — Wrapped `open_connection` in timeout, `ping()` uses `send_ping()`
- `sdk/python/tests/test_client.py` — Updated ping test to use request_id=0 matching relay behavior
- `sdk/python/tests/conftest.py` — Fixed default port to 4201
- `sdk/python/pyproject.toml` — Fixed port in integration marker

## Self-Check: PASSED

- [x] Integration tests created with 4 test functions
- [x] Tests skip gracefully when relay unreachable
- [x] Tests pass against live relay (human verified)
- [x] All 107 unit tests still pass
- [x] Port, ping, and timeout bugs found and fixed during human verification
