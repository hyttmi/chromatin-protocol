---
phase: quick
plan: 260402-a2o
subsystem: acl
tags: [access-control, config, peer-manager, version-bump]
dependency_graph:
  requires: []
  provides: [split-acl-client-peer]
  affects: [db/acl, db/config, db/peer, db/main, integration-tests, docs]
tech_stack:
  added: []
  patterns: [connection-type-branching]
key_files:
  created: []
  modified:
    - CMakeLists.txt
    - db/acl/access_control.h
    - db/acl/access_control.cpp
    - db/config/config.h
    - db/config/config.cpp
    - db/main.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/tests/acl/test_access_control.cpp
    - db/tests/config/test_config.cpp
    - db/tests/peer/test_peer_manager.cpp
    - db/tests/test_daemon.cpp
    - db/README.md
    - db/PROTOCOL.md
    - tests/integration/test_acl01_closed_garden.sh
    - tests/integration/test_acl05_sighup_reload.sh
    - tests/integration/test_crypt05_mitm_rejection.sh
    - tests/integration/test_crypt06_trusted_bypass.sh
decisions:
  - "allowed_peer_keys for TCP peer-to-peer, allowed_client_keys for UDS client connections"
  - "PEX suppression uses is_peer_closed_mode() since PEX is peer-to-peer protocol"
  - "validate_allowed_keys function name unchanged (validates any hex key list)"
  - "Old allowed_keys field treated as unknown config key (warns on load)"
metrics:
  duration_seconds: 2521
  completed: "2026-04-02"
  tasks_completed: 2
  tasks_total: 2
  files_modified: 18
  tests_passed: 576
  test_assertions: 2590
---

# Quick Task 260402-a2o: Split allowed_keys into allowed_client_keys and allowed_peer_keys

Split ACL into independent client (UDS) and peer (TCP) key lists with connection-type branching in on_peer_connected, version bump to 1.8.0.

## Task Results

| Task | Name | Commit | Key Changes |
|------|------|--------|-------------|
| 1 | Split AccessControl, Config, PeerManager, and all unit tests | 95ba563 | AccessControl two-list constructor, Config split fields, on_peer_connected is_uds() branch, PEX uses is_peer_closed_mode(), 576 tests pass |
| 2 | Update docs, default configs, and Docker integration tests | c523114 | README, PROTOCOL.md, 4 integration test scripts updated, zero stale references |

## Changes Made

### AccessControl (db/acl/access_control.h, .cpp)
- Replaced single `allowed_keys_` set with `allowed_client_keys_` and `allowed_peer_keys_`
- New constructor: `AccessControl(client_hex_keys, peer_hex_keys, own_namespace)`
- Split methods: `is_client_allowed()` / `is_peer_allowed()`, `is_client_closed_mode()` / `is_peer_closed_mode()`, `client_allowed_count()` / `peer_allowed_count()`
- `reload()` takes two key lists, returns `ReloadResult` with per-list diffs
- Own namespace implicitly allowed in both lists when that list is non-empty

### Config (db/config/config.h, .cpp)
- Replaced `allowed_keys` field with `allowed_client_keys` and `allowed_peer_keys`
- Two new JSON parsing blocks for the new fields
- `known_keys` set updated: removed `allowed_keys`, added `allowed_client_keys` and `allowed_peer_keys`
- Old `allowed_keys` in config JSON now triggers unknown key warning

### PeerManager (db/peer/peer_manager.cpp)
- `on_peer_connected`: branches on `conn->is_uds()` for correct ACL check
- `disconnect_unauthorized_peers`: checks correct ACL per connection type
- `reload_config`: validates and reloads both key lists, logs per-list diffs
- PEX suppression (4 locations): changed from `is_closed_mode()` to `is_peer_closed_mode()`
- Startup logging: separate client/peer access control mode lines

### Unit Tests
- `test_access_control.cpp`: 19 test cases (was 11) covering independent client/peer modes, cross-list isolation, both-closed with different key sets
- `test_config.cpp`: all ACL tests updated for new field names, added test for old field warning
- `test_peer_manager.cpp`: all ACL constructor calls updated to 3-arg form with empty client keys
- `test_daemon.cpp`: same pattern update

### Documentation
- `README.md`: config example, field descriptions, closed mode section, SIGHUP section updated
- `PROTOCOL.md`: UDS vs TCP ACL branching documented

### Integration Tests
- `test_acl01_closed_garden.sh`: `allowed_keys` -> `allowed_peer_keys` in configs and comments
- `test_acl05_sighup_reload.sh`: same pattern in all 3 config writes and comments
- `test_crypt05_mitm_rejection.sh`: all references updated
- `test_crypt06_trusted_bypass.sh`: all references updated

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Updated test_daemon.cpp (not in plan)**
- **Found during:** Task 1 (compilation)
- **Issue:** `db/tests/test_daemon.cpp` also uses AccessControl with the old API
- **Fix:** Updated 8 constructor calls and field references to match new API
- **Files modified:** `db/tests/test_daemon.cpp`
- **Commit:** 95ba563

## Known Stubs

None.

## Self-Check: PASSED

- All 17 modified files exist
- Both commits found: 95ba563, c523114
- Version check: 1.8.0 confirmed
- Stale reference check: only `validate_allowed_keys` function docstring (acceptable -- function name unchanged)
