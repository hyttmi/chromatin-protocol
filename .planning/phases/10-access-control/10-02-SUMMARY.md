---
phase: 10-access-control
plan: 02
subsystem: peer
tags: [peer-manager, acl, pex, connection-gating]

requirements_completed:
  - ACL-01
  - ACL-02
  - ACL-03

requires:
  - phase: 10-access-control
    provides: AccessControl class from plan 01
provides:
  - Post-handshake ACL gating in PeerManager.on_peer_connected()
  - PEX disabled at all 4 protocol points in closed mode
  - ACL passed to PeerManager constructor
---

# Plan 10-02 Summary: PeerManager ACL Integration

## What was built

1. **Connection gating** (`db/peer/peer_manager.cpp`):
   - ACL check as FIRST thing in `on_peer_connected()` (before peers_ insert)
   - Derives namespace hash via `crypto::sha3_256(conn->peer_pubkey())`
   - Unauthorized peers get `conn->close()` (silent, no goodbye)
   - Logged at warn level: `access denied: namespace=... ip=...`

2. **PEX disable in closed mode** (4 guard points):
   - `pex_timer_loop()`: `if (acl_.is_closed_mode()) continue;`
   - `handle_pex_as_responder()`: early `co_return` in closed mode
   - `run_sync_with_peer()`: inline PEX block guarded
   - `handle_sync_as_responder()`: inline PEX block guarded

3. **Constructor update**: PeerManager accepts `acl::AccessControl&` as 6th parameter. All call sites updated (main.cpp, test_daemon.cpp, test_peer_manager.cpp).

## Tests added

- 3 ACL integration tests:
  - Closed mode rejects unauthorized peer (verified no sync occurs)
  - Closed mode accepts authorized peer (verified sync works)
  - Closed mode disables PEX discovery (verified peer_count stays at 1)

## Requirements covered

- ACL-01 (complete): Non-empty allowed_keys = closed mode, empty = open mode
- ACL-02 (complete): Unauthorized peers rejected after handshake, before entering PeerManager state
- ACL-03 (complete): PEX fully disabled in closed mode
