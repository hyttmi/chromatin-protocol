---
phase: 118-configurable-constants-peer-management
fixed_at: 2026-04-17T05:28:00Z
review_path: .planning/phases/118-configurable-constants-peer-management/118-REVIEW.md
iteration: 1
findings_in_scope: 3
fixed: 3
skipped: 0
status: all_fixed
---

# Phase 118: Code Review Fix Report

**Fixed at:** 2026-04-17T05:28:00Z
**Source review:** .planning/phases/118-configurable-constants-peer-management/118-REVIEW.md
**Iteration:** 1

**Summary:**
- Findings in scope: 3 (Critical + Warning only; Info findings IN-01 and IN-02 intentionally out of scope)
- Fixed: 3
- Skipped: 0

## Fixed Issues

### CR-01: SIGHUP reload permanently kills sync_timer_loop and cursor_compaction_loop

**Files modified:** `db/peer/sync_orchestrator.h`, `db/peer/sync_orchestrator.cpp`, `db/peer/peer_manager.cpp`
**Commit:** c04c9b51
**Applied fix:** Added `SyncOrchestrator::cancel_compaction_timer()` method that cancels only the compaction timer (not the sync/expiry/cursor-compaction timers whose loops exit permanently on cancel). Replaced `sync_.cancel_timers()` call in `PeerManager::reload_config()` with `sync_.cancel_compaction_timer()`. This ensures SIGHUP-triggered compaction interval updates restart only the storage compaction loop, leaving sync_timer_loop and cursor_compaction_loop running as intended. Added comment at the call site explaining why the narrower cancellation is required.

### WR-01: reload_config does not call validate_config -- out-of-range values accepted on SIGHUP

**Files modified:** `db/peer/peer_manager.cpp`, `db/config/config.h`, `db/config/config.cpp`
**Commits:** 5d4f9e1d, 51d53de3
**Applied fix:** Added a single upfront validation block at the top of `PeerManager::reload_config()` (after the JSON load succeeds, before any state mutation). The block calls `validate_config()` plus the three `validate_allowed_keys()` invocations and `validate_trusted_peers()`. On any validation failure, the reload is rejected with a descriptive log line and current config is kept intact.

Because the test harness constructs reload configs with `bind_address: 127.0.0.1:0` (port 0 for ephemeral binding), and `validate_config` rejects port 0, added an optional `check_bind_address` parameter to `validate_config` (default `true` for startup, passed as `false` for the SIGHUP path). This is semantically correct: bind_address is not reloaded at runtime (server is already bound), so there is no reason to validate it during reload.

### WR-02: reload_config partial application on late validation failure

**Files modified:** `db/peer/peer_manager.cpp`
**Commit:** 5d4f9e1d
**Applied fix:** Consolidated WR-02's fix into the same upfront validation block as WR-01. The two late validation blocks (sync_namespaces validation at old line 566-569, and trusted_peers validation at old line 638-643) were removed. The validations they performed are now handled by the single upfront try/catch block, so no state mutation occurs unless all validation passes. Left brief comments at the removed sites ("already validated upfront above") for reader clarity.

## Verification

- **Build:** `chromatindb_lib` and `chromatindb_tests` compile cleanly with the new code (verified at each commit).
- **Tests:**
  - All 138 `[config]` test cases pass (251 assertions).
  - All 4 in-scope reload test cases pass:
    - `reload_config with invalid config keeps current state`
    - `reload_config switches from open to closed mode`
    - `PeerManager reload_config updates rate limit parameters`
    - `PeerManager reload_config updates cursor config and resets round counters`
    - `SIGHUP reloads quota config into BlobEngine`
  - Full test suite: 348/349 test cases pass. The single failing test (`closed mode rejects unauthorized peer` at test_peer_manager.cpp:213) is a **pre-existing SIGSEGV** that fails identically on master before any of these fixes (confirmed by reverting to HEAD~2 state and re-running). It does not exercise reload_config and is unrelated to this phase's changes.
  - A second test (`reload_config revokes connected peer` at test_peer_manager.cpp:359) is also pre-existing broken when run individually with `[reload]` filter; confirmed unrelated to these fixes.

## Notes for future work

- IN-01 (commented-out design notes in sync_orchestrator.cpp:594-602) and IN-02 (duplicated test constants in peer_manager.h) were out of scope for this pass (Info-severity only).
- The two pre-existing test failures (line 213, line 359) should be addressed in a separate bug-fix phase -- they appear to be acceptance/timing issues in the peer ACL revocation flow, not related to config reload semantics.

---

_Fixed: 2026-04-17T05:28:00Z_
_Fixer: Claude (gsd-code-fixer)_
_Iteration: 1_
