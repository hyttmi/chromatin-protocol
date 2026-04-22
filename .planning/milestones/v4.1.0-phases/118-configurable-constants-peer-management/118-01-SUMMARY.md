---
phase: 118-configurable-constants-peer-management
plan: 01
subsystem: node-config
tags: [config, sighup, peer-tuning, pidfile]
dependency_graph:
  requires: []
  provides: [configurable-constants, sighup-reload-extended, pidfile]
  affects: [peer-manager, sync-orchestrator, connection-manager, pex-manager, main]
tech_stack:
  added: []
  patterns: [config-field-extension, sighup-reload, member-field-replacement]
key_files:
  created: []
  modified:
    - db/config/config.h
    - db/config/config.cpp
    - db/tests/config/test_config.cpp
    - db/peer/sync_orchestrator.h
    - db/peer/sync_orchestrator.cpp
    - db/peer/connection_manager.h
    - db/peer/connection_manager.cpp
    - db/peer/pex_manager.h
    - db/peer/pex_manager.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
    - db/main.cpp
    - db/tests/peer/test_peer_manager.cpp
decisions:
  - blob_transfer_timeout default raised from 120s to 600s per D-02 (large blob support)
  - strike_cooldown stored in config but not yet enforced at runtime (documented in config.h comment)
  - PeerManager constexprs renamed to _DEFAULT suffix for test backward compat
metrics:
  duration_seconds: 943
  completed: 2026-04-16T12:24:05Z
  tasks_completed: 2
  tasks_total: 2
  files_modified: 13
---

# Phase 118 Plan 01: Configurable Constants Summary

5 hardcoded sync/peer constants moved to Config struct with JSON parsing, range validation, SIGHUP reload for 3 fields, component member fields replacing static constexprs, and pidfile lifecycle in main.cpp.

## What Changed

### Task 1: Config struct fields, parsing, known_keys, and validation
**Commit:** 8fda1f05

Added 5 new fields to Config struct with defaults matching prior hardcoded values (blob_transfer_timeout=600, sync_timeout=30, pex_interval=300, strike_threshold=10, strike_cooldown=300). JSON parsing via `j.value()`, all 5 added to known_keys set, range validation in validate_config() (blob_transfer_timeout 10-86400, sync_timeout 5-3600, pex_interval 10-86400, strike_threshold 1-1000, strike_cooldown 0-86400). 18 new tests: defaults, JSON parsing for each field, validation bounds (min and max), known keys check.

**Files:** db/config/config.h, db/config/config.cpp, db/tests/config/test_config.cpp

### Task 2: Component member fields, constructor plumbing, SIGHUP reload, and pidfile
**Commit:** 2c0c29ad

- **SyncOrchestrator:** Removed `static constexpr BLOB_TRANSFER_TIMEOUT` and both local `constexpr SYNC_TIMEOUT` declarations. Added `blob_transfer_timeout_` and `sync_timeout_` member fields with constructor params and setters. All 7 SYNC_TIMEOUT and 2 BLOB_TRANSFER_TIMEOUT references replaced with member fields.
- **ConnectionManager:** Removed `static constexpr STRIKE_THRESHOLD`. Added `strike_threshold_` member field with constructor param. Both STRIKE_THRESHOLD references replaced.
- **PexManager:** Removed `static constexpr PEX_INTERVAL_SEC`. Added `pex_interval_sec_` member field with constructor param and setter. Timer loop uses member field.
- **PeerManager:** Renamed 4 constexprs to `_DEFAULT` suffix (BLOB_TRANSFER_TIMEOUT_DEFAULT=600s, STRIKE_THRESHOLD_DEFAULT=10, STRIKE_COOLDOWN_SEC_DEFAULT=300, PEX_INTERVAL_SEC_DEFAULT=300). Constructor passes config values to all component constructors. reload_config() calls `set_blob_transfer_timeout`, `set_sync_timeout`, `set_pex_interval` on SIGHUP. strike_threshold and strike_cooldown are not reloaded (D-05: restart only).
- **main.cpp:** Writes pidfile to data_dir/chromatindb.pid after pm.start(), removes on exit. Logs all 5 new config values at startup. Added `#include <fstream>`.
- **Tests:** Updated constant references to _DEFAULT names, BLOB_TRANSFER_TIMEOUT_DEFAULT=600s.

**Files:** db/peer/sync_orchestrator.h, db/peer/sync_orchestrator.cpp, db/peer/connection_manager.h, db/peer/connection_manager.cpp, db/peer/pex_manager.h, db/peer/pex_manager.cpp, db/peer/peer_manager.h, db/peer/peer_manager.cpp, db/main.cpp, db/tests/peer/test_peer_manager.cpp

## Verification Results

- Build: PASS (cmake --build . -j$(nproc) exits 0)
- Config tests: 138 test cases, 251 assertions, all passed
- Peer/PEX unit tests: all passed (strike threshold, PEX constants, sync constants)
- Full suite: 344/345 passed (1 pre-existing SIGSEGV in "closed mode rejects unauthorized peer" integration test -- not caused by these changes)
- No constexpr SYNC_TIMEOUT in sync_orchestrator.cpp: PASS
- No static constexpr STRIKE_THRESHOLD in connection_manager.h: PASS
- No static constexpr PEX_INTERVAL_SEC in pex_manager.h: PASS

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

- `strike_cooldown` is stored in Config and passed through but has no runtime enforcement (ban timer not implemented). Documented in config.h comment: "not yet enforced". This is intentional per the plan and research (Pitfall 2).

## Threat Flags

None found. All new surface (config parsing, validation, pidfile) is covered by the plan's threat model (T-118-01 through T-118-03).

## Self-Check: PASSED

- All 13 modified files exist on disk
- Commit 8fda1f05 (Task 1) found in git log
- Commit 2c0c29ad (Task 2) found in git log
