---
phase: 118-configurable-constants-peer-management
reviewed: 2026-04-16T12:00:00Z
depth: standard
files_reviewed: 13
files_reviewed_list:
  - db/config/config.cpp
  - db/config/config.h
  - db/main.cpp
  - db/peer/connection_manager.cpp
  - db/peer/connection_manager.h
  - db/peer/peer_manager.cpp
  - db/peer/peer_manager.h
  - db/peer/pex_manager.cpp
  - db/peer/pex_manager.h
  - db/peer/sync_orchestrator.cpp
  - db/peer/sync_orchestrator.h
  - db/tests/config/test_config.cpp
  - db/tests/peer/test_peer_manager.cpp
findings:
  critical: 1
  warning: 2
  info: 2
  total: 5
status: issues_found
---

# Phase 118: Code Review Report

**Reviewed:** 2026-04-16
**Depth:** standard
**Files Reviewed:** 13
**Status:** issues_found

## Summary

Phase 118 adds configurable constants for sync/peer management (blob_transfer_timeout, sync_timeout, pex_interval, strike_threshold, strike_cooldown) and peer management CLI commands (add-peer, remove-peer, list-peers). The config layer is clean -- new fields are parsed, validated with proper range checks, and registered as known keys. Test coverage for config parsing and validation is thorough.

However, the SIGHUP reload path has a critical bug where `sync_.cancel_timers()` permanently kills the safety-net sync loop and cursor compaction loop, since those coroutines exit on cancellation and are never respawned. Additionally, `reload_config()` does not call `validate_config()`, allowing out-of-range values to be applied on SIGHUP.

## Critical Issues

### CR-01: SIGHUP reload permanently kills sync_timer_loop and cursor_compaction_loop

**File:** `db/peer/peer_manager.cpp:666`
**Issue:** The `reload_config()` method calls `sync_.cancel_timers()` to restart the compaction timer with a new interval. However, `SyncOrchestrator::cancel_timers()` cancels ALL four timers: sync_timer, expiry_timer, cursor_compaction_timer, and compaction_timer. The `sync_timer_loop` (line 1109 of sync_orchestrator.cpp) and `cursor_compaction_loop` (line 1204) both have `if (ec || stopping_) co_return;` -- they permanently exit when cancelled. Only `compaction_loop` is conditionally respawned (line 668). After a SIGHUP that triggers this code path, periodic sync and cursor compaction stop forever until daemon restart.
**Fix:** Either (a) add a dedicated `cancel_compaction_timer()` method that only cancels the compaction timer, or (b) respawn sync_timer_loop and cursor_compaction_loop after calling cancel_timers. Option (a) is cleaner:

```cpp
// In SyncOrchestrator:
void cancel_compaction_timer() {
    if (compaction_timer_) compaction_timer_->cancel();
}

// In PeerManager::reload_config(), replace sync_.cancel_timers() with:
sync_.cancel_compaction_timer();
```

## Warnings

### WR-01: reload_config does not call validate_config -- out-of-range values accepted on SIGHUP

**File:** `db/peer/peer_manager.cpp:490-681`
**Issue:** The `reload_config()` method loads the new config via `load_config()` and validates individual pieces (allowed_keys, trusted_peers, sync_namespaces), but never calls `validate_config()` on the full config. This means all range validations (e.g., `blob_transfer_timeout` must be 10-86400, `pex_interval` must be 10-86400, `sync_timeout` must be 5-3600, `max_peers >= 1`, `safety_net_interval_seconds >= 3`, etc.) are bypassed on SIGHUP reload. An operator editing the config to set `pex_interval: 1` or `blob_transfer_timeout: 0` would have the invalid value applied.
**Fix:** Add `validate_config()` early in reload_config, after loading but before applying any values:

```cpp
void PeerManager::reload_config() {
    spdlog::info("reloading access control from {}...", config_path_.string());

    config::Config new_cfg;
    try {
        new_cfg = config::load_config(config_path_);
    } catch (const std::exception& e) {
        spdlog::error("config reload failed (invalid JSON): {} (keeping current config)", e.what());
        return;
    }

    // Validate all ranges before applying anything
    try {
        config::validate_config(new_cfg);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (validation failed): {} (keeping current config)", e.what());
        return;
    }

    // ... rest of reload ...
}
```

Note: `validate_config` currently validates `bind_address` which cannot change at runtime. You may want to skip that specific check on reload, or accept that it will produce a harmless warning if bind_address format is valid (which it should be since the server is already running on it). The current validate_config would reject changes to bind_address port to 0, but since the actual bind_address is not reloaded, this is benign.

### WR-02: reload_config partial application on late validation failure

**File:** `db/peer/peer_manager.cpp:566-569` and `db/peer/peer_manager.cpp:638-643`
**Issue:** Two validation checks (`validate_allowed_keys` for sync_namespaces at line 568, and `validate_trusted_peers` at line 640) can cause early `return` from `reload_config()`. However, these returns happen after many settings have already been applied (ACL, rate limits, subscriptions, sync config, max_peers, etc.). This results in a partially applied config reload -- some settings change, others remain at old values. The log message says "keeping current" but multiple settings have already been mutated.
**Fix:** Move all validation to the top of reload_config, before applying any values. Combined with WR-01's fix (`validate_config()` early), move the specific key validations up as well:

```cpp
// Validate everything first, before touching any state
try {
    config::validate_config(new_cfg);
    config::validate_allowed_keys(new_cfg.allowed_client_keys);
    config::validate_allowed_keys(new_cfg.allowed_peer_keys);
    config::validate_allowed_keys(new_cfg.sync_namespaces);
    config::validate_trusted_peers(new_cfg.trusted_peers);
} catch (const std::exception& e) {
    spdlog::error("config reload rejected: {} (keeping current config)", e.what());
    return;
}

// Now apply all validated values (no early returns needed)
```

## Info

### IN-01: Commented-out design notes left in sync_orchestrator.cpp

**File:** `db/peer/sync_orchestrator.cpp:594-602`
**Issue:** A block of commented-out design reasoning remains in the PEX section after sync. Lines 594-602 contain notes like "DEVIATION: We'll accept the PEX part inline using callbacks" and "Actually, sync protocol always does inline PEX." These read as development notes rather than documentation.
**Fix:** Remove or condense into a single-line comment explaining the design decision:

```cpp
// PEX exchange after sync: initiator sends PeerListRequest via injected callback.
// ACL closed-mode suppression is handled by the PeerManager-injected callback.
if (pex_request_) {
    co_await pex_request_(conn);
}
```

### IN-02: BLOB_TRANSFER_TIMEOUT_DEFAULT constant in peer_manager.h shadows config default

**File:** `db/peer/peer_manager.h:77`
**Issue:** `PeerManager::BLOB_TRANSFER_TIMEOUT_DEFAULT` is defined as `std::chrono::seconds(600)`, duplicating the default value also defined in `config.h` line 53 (`blob_transfer_timeout = 600`). Similarly, `STRIKE_THRESHOLD_DEFAULT` (line 79) and `STRIKE_COOLDOWN_SEC_DEFAULT` (line 80) duplicate config.h defaults. If one changes, the other could fall out of sync. These constants are only used in tests (`test_peer_manager.cpp` line 109).
**Fix:** Tests could reference the config default directly (`Config{}.blob_transfer_timeout`) instead of maintaining parallel constants. Alternatively, document that these are frozen test-assertion values that intentionally match the config defaults, so any config default change triggers a test failure as a safety net. The current approach is acceptable as-is but introduces a subtle maintenance burden.

---

_Reviewed: 2026-04-16_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
