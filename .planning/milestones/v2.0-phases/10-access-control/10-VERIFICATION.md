---
phase: 10-access-control
status: passed
verified: 2026-03-07
---

# Phase 10: Access Control — Verification

## Phase Goal
Node operators can restrict which pubkeys connect, creating a fully closed node that rejects unauthorized peers.

## Success Criteria Verification

### 1. Closed mode rejects unauthorized, open mode allows all (backward compatible)
**Status: VERIFIED**
- `AccessControl::is_allowed()` returns `true` for all peers when `configured_count_ == 0` (open mode) (`db/acl/access_control.cpp:42-50`)
- `is_closed_mode()` returns `true` when `configured_count_ > 0` (`db/acl/access_control.cpp:52-54`)
- Config parsing reads `allowed_keys` JSON array, empty array defaults to open mode (`db/config/config.cpp:42-49`)
- 8 config tests validate parsing, hex validation, empty-array handling
- 5 AccessControl tests validate open/closed mode, allow/reject logic

### 2. Unauthorized peers disconnected after handshake, before PeerManager state
**Status: VERIFIED**
- ACL check is FIRST operation in `on_peer_connected()`, before `peers_.push_back()` (`db/peer/peer_manager.cpp:131-140`)
- Derives namespace hash via `crypto::sha3_256(conn->peer_pubkey())` and checks `acl_.is_allowed()`
- Unauthorized peers get `conn->close()` and immediate `return` — never reach message routing setup (line 172+)
- Integration test "closed mode rejects unauthorized peer" confirms no sync occurs, peer_count stays 0

### 3. PEX disabled in closed mode
**Status: VERIFIED**
- 4 guard points all check `acl_.is_closed_mode()`:
  1. `pex_timer_loop()` — `continue` skips `request_peers_from_all()` (`db/peer/peer_manager.cpp:935`)
  2. `handle_pex_as_responder()` — early `co_return` (`db/peer/peer_manager.cpp:880`)
  3. `run_sync_with_peer()` — inline PEX block guarded (`db/peer/peer_manager.cpp:489`)
  4. `handle_sync_as_responder()` — inline PEX block guarded (`db/peer/peer_manager.cpp:641`)
- Integration test "closed mode disables PEX discovery" confirms 3-node topology stays at peer_count == 1

### 4. SIGHUP reloads allowed_keys and disconnects revoked peers
**Status: VERIFIED**
- `sighup_loop()` via `asio::signal_set` re-arms after each delivery (`db/peer/peer_manager.cpp:711-718`)
- Only enabled when `--config` provided (config_path non-empty) (`db/peer/peer_manager.cpp:77-82`)
- `reload_config()` re-reads config, validates keys, calls `acl_.reload()` with atomic swap (`db/peer/peer_manager.cpp:725-761`)
- Fail-safe: invalid JSON or malformed keys keep current config (logged at error level)
- `disconnect_unauthorized_peers()` snapshots peer list, re-derives namespace hash, closes revoked peers immediately (`db/peer/peer_manager.cpp:763-783`)
- 3 reload tests: revocation works, invalid config preserved, open-to-closed mode switch

## Requirement Coverage

| Requirement | Plan | Verified |
|-------------|------|----------|
| ACL-01: allowed_keys config (closed/open mode) | 10-01, 10-02 | Yes |
| ACL-02: Post-handshake rejection before PeerManager state | 10-02 | Yes |
| ACL-03: PEX disabled in closed mode | 10-02 | Yes |
| ACL-04: SIGHUP hot-reload without restart | 10-03 | Yes |
| ACL-05: Revoked peers disconnected on reload | 10-03 | Yes |

## Test Summary

- **Total tests:** 196 (26 ACL-specific: 8 config, 12 AccessControl, 3 integration, 3 reload)
- **All pass:** Yes
- **Integration tests verified:** "closed mode rejects unauthorized peer", "closed mode accepts authorized peer and syncs", "closed mode disables PEX discovery", "reload_config revokes connected peer", "reload_config with invalid config keeps current state", "reload_config switches from open to closed mode"

## Conclusion

Phase 10 goal achieved. All 5 ACL requirements verified. The node supports full access control with open/closed mode, connection-level gating, PEX disable, hot-reload via SIGHUP, and immediate peer revocation.
