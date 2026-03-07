---
phase: 10-access-control
plan: 03
subsystem: peer
tags: [sighup, hot-reload, revocation, config]

requirements_completed:
  - ACL-04
  - ACL-05

requires:
  - phase: 10-access-control
    provides: ACL integration from plan 02
provides:
  - SIGHUP handler via asio::signal_set coroutine
  - reload_config() public method for config hot-reload
  - disconnect_unauthorized_peers() for immediate revocation
---

# Plan 10-03 Summary: SIGHUP Reload + Peer Revocation

## What was built

1. **SIGHUP handler** (`db/peer/peer_manager.cpp`):
   - `asio::signal_set` with coroutine loop (`sighup_loop()`)
   - Only enabled when `--config` was provided (config_path non-empty)
   - Re-arms automatically after each SIGHUP delivery

2. **Config reload** (`reload_config()`):
   - Re-reads only `allowed_keys` from config file
   - Validates strictly via `validate_allowed_keys()`
   - Fail-safe: invalid JSON or malformed keys keep current config
   - Calls `acl_.reload()` and logs diff (+N keys, -N keys)
   - Calls `disconnect_unauthorized_peers()` when keys removed or in closed mode

3. **Peer revocation** (`disconnect_unauthorized_peers()`):
   - Snapshots peer list to avoid iterator invalidation
   - Re-derives namespace hash for each connected peer
   - Calls `conn->close()` for revoked peers (immediate, no goodbye)
   - Logged at warn level per peer + summary count

## Tests added

- 3 reload tests:
  - Revocation: connected peer disconnected after config reload removes their key
  - Fail-safe: invalid config file keeps current ACL state
  - Mode switch: open to closed mode via reload

## Requirements covered

- ACL-04 (complete): SIGHUP hot-reload of allowed_keys without restart
- ACL-05 (complete): Revoked peers disconnected immediately on reload
