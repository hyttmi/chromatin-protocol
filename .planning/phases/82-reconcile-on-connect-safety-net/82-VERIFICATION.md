---
phase: 82-reconcile-on-connect-safety-net
verified: 2026-04-04T09:15:00Z
status: gaps_found
score: 10/11 must-haves verified
re_verification:
  previous_status: gaps_found
  previous_score: 10/11
  gaps_closed:
    - "deploy/configs/node1-trusted.json renamed sync_interval_seconds to safety_net_interval_seconds: 600"
    - "deploy/configs/node2-trusted.json renamed sync_interval_seconds to safety_net_interval_seconds: 600"
    - "deploy/configs/node3-trusted.json renamed sync_interval_seconds to safety_net_interval_seconds: 600"
  gaps_remaining:
    - "32 integration test scripts in tests/integration/ still use sync_interval_seconds (stale key) — node ignores it and defaults to 600s, breaking all timer-dependent integration tests"
    - "deploy/run-benchmark.sh uses sync_interval_seconds in 11 places — benchmark also runs at 600s default, and jq reads of .sync_interval_seconds return null breaking result parsing"
    - "db/README.md documents sync_interval_seconds (line 127 config example, line 159 field description) — user-facing documentation is incorrect"
  regressions: []
gaps:
  - truth: "Old sync_interval_seconds field is completely removed from codebase"
    status: failed
    reason: "The previous verification incorrectly scoped this gap to only 3 deploy config files. Those 3 files are now fixed. However 32 integration test scripts, deploy/run-benchmark.sh (11 occurrences), and db/README.md still use the old key name. Because sync_interval_seconds is not in the known_keys set in config.cpp, the node logs a warning and silently falls back to 600s. This makes every integration test that depends on sync happening within 5-10 seconds effectively broken — they would time out waiting for a sync event that takes 600 seconds."
    artifacts:
      - path: "tests/integration/test_e2e01_async_delivery.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored by node, sync falls back to 600s default"
      - path: "tests/integration/test_e2e02_history_backfill.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_e2e03_delete_for_everyone.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_e2e04_namespace_isolation.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_acl01_closed_garden.sh"
        issue: "Contains sync_interval_seconds: 5 (4 occurrences) — silently ignored"
      - path: "tests/integration/test_acl05_sighup_reload.sh"
        issue: "Contains sync_interval_seconds: 5 (7 occurrences) — silently ignored"
      - path: "tests/integration/test_stress01_long_running.sh"
        issue: "Contains sync_interval_seconds: 5 (3 occurrences) — silently ignored"
      - path: "tests/integration/test_stress02_peer_churn.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_stress03_namespace_scaling.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_stress04_concurrent_ops.sh"
        issue: "Contains sync_interval_seconds: 5 (4 occurrences) — silently ignored"
      - path: "tests/integration/test_dos01_write_rate_limiting.sh"
        issue: "Contains sync_interval_seconds: 5 (3 occurrences) — silently ignored"
      - path: "tests/integration/test_dos02_sync_rate_limiting.sh"
        issue: "Contains sync_interval_seconds: 5 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_dos03_concurrent_sessions.sh"
        issue: "Contains sync_interval_seconds: 5 and 1 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_dos04_storage_full.sh"
        issue: "Contains sync_interval_seconds: 5 (3 occurrences) — silently ignored"
      - path: "tests/integration/test_dos05_namespace_quotas.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_dos06_thread_pool_saturation.sh"
        issue: "Contains sync_interval_seconds: 5 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_net02_split_brain.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_net06_late_joiner.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_ops01_sighup_config_reload.sh"
        issue: "Contains sync_interval_seconds: 5 (3 occurrences) — silently ignored"
      - path: "tests/integration/test_ops02_sigusr1_metrics.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_ops03_sigterm_graceful.sh"
        issue: "Contains sync_interval_seconds: 5 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_san04_protocol_fuzzing.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_san05_handshake_fuzzing.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_crypt05_mitm_rejection.sh"
        issue: "Contains sync_interval_seconds: 5 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_crypt06_trusted_bypass.sh"
        issue: "Contains sync_interval_seconds: 5 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_ttl01_tombstone_propagation.sh"
        issue: "Contains sync_interval_seconds: 5 (3 occurrences) — silently ignored"
      - path: "tests/integration/test_ttl02_tombstone_ttl_expiry.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_ttl03_permanent_blobs.sh"
        issue: "Contains sync_interval_seconds: 5 (2 occurrences) — silently ignored"
      - path: "tests/integration/test_dr01_dare_forensics.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_dr02_dr03_master_key.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_dr04_data_migration.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "tests/integration/test_dr05_crash_recovery_integrity.sh"
        issue: "Contains sync_interval_seconds: 5 — silently ignored"
      - path: "deploy/run-benchmark.sh"
        issue: "Contains sync_interval_seconds in 11 places (config generation + jq reads). Node ignores key at runtime; jq reads .sync_interval_seconds return null, breaking result parsing."
      - path: "db/README.md"
        issue: "Line 127: config example uses sync_interval_seconds: 60. Line 159: field documented as sync_interval_seconds. New key safety_net_interval_seconds absent from documentation."
    missing:
      - "Rename sync_interval_seconds to safety_net_interval_seconds in all 32 tests/integration/*.sh scripts"
      - "Decide on test value: current value 5 is below the 60s validation minimum. Options: lower minimum for test/integration builds, update tests to use >= 60 (slower tests), or add sync_interval_seconds as a deprecated alias that bypasses the minimum"
      - "Update deploy/run-benchmark.sh: rename key in config generation blocks; update jq reads from .sync_interval_seconds to .safety_net_interval_seconds"
      - "Update db/README.md: replace sync_interval_seconds with safety_net_interval_seconds in example config (line 127) and field description (line 159); add 600s default and 60s minimum to docs"
human_verification:
  - test: "Integration test timing after key rename"
    expected: "After renaming the key in all integration test scripts to safety_net_interval_seconds with an appropriate value (>= 60 or with adjusted validation minimum), tests pass without timing out"
    why_human: "Requires a policy decision on the validation minimum vs. test usability tradeoff, then Docker-based integration test execution to confirm correct behavior"
---

# Phase 82: Reconcile-on-Connect Safety-Net Verification Report

**Phase Goal:** Peers catch up on missed blobs via full reconciliation on connect, with a long-interval safety net and graceful cursor lifecycle
**Verified:** 2026-04-04T09:15:00Z
**Status:** gaps_found
**Re-verification:** Yes — after partial gap closure (deploy/configs fixed; deeper scan revealed integration tests, benchmark, and docs were also missed)

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Config field safety_net_interval_seconds exists with default 600 | VERIFIED | `db/config/config.h:21 uint32_t safety_net_interval_seconds = 600;` |
| 2 | Old sync_interval_seconds field is completely removed from codebase | FAILED | deploy/configs/ now fixed; 32 integration test scripts, deploy/run-benchmark.sh (11 occurrences), and db/README.md still use old key — node silently ignores it and runs at 600s default |
| 3 | Safety-net timer fires every safety_net_interval_seconds_ and runs sync_all_peers() | VERIFIED | `peer_manager.cpp:2668` timer + `2674` co_await sync_all_peers() |
| 4 | safety_net_interval_seconds is SIGHUP-reloadable via mutable member | VERIFIED | `peer_manager.cpp:2817-2818` reload_config updates mutable member |
| 5 | Config validation rejects safety_net_interval_seconds < 60 | VERIFIED | `config.cpp:245-247` + test case boundary checks pass |
| 6 | When a peer connects, full XOR-fingerprint reconciliation runs automatically (initiator-only) | VERIFIED | `peer_manager.cpp:481-485` on_peer_connected spawns run_sync_with_peer for initiator |
| 7 | When a peer disconnects, its disconnect timestamp is recorded in disconnected_peers_ map | VERIFIED | `peer_manager.cpp:503-512` on_peer_disconnected writes DisconnectedPeerState |
| 8 | When a peer reconnects within 5 minutes, cursors preserved | VERIFIED | `peer_manager.cpp:459-471` grace period check; test "reconnect within grace period" passes |
| 9 | When a peer reconnects after 5 minutes, cursor reuse skipped | VERIFIED | `peer_manager.cpp:467-470` logs "cursors expired" path |
| 10 | Stale entries cleaned during safety-net cycle with MDBX cursor deletion | VERIFIED | `peer_manager.cpp:2676-2695` post-sync_all_peers cleanup + delete_peer_cursors call |
| 11 | cursor_compaction_loop excludes recently-disconnected peers from cursor deletion | VERIFIED | `peer_manager.cpp:3716-3726` iterates disconnected_peers_ and includes grace-period hashes |

**Score:** 10/11 truths verified (1 failed)

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/config/config.h` | safety_net_interval_seconds field with default 600 | VERIFIED | Line 21: `uint32_t safety_net_interval_seconds = 600;` |
| `db/config/config.cpp` | JSON parsing, known_keys, validation for safety_net_interval_seconds | VERIFIED | 5 occurrences: parsing (36), known_keys (64), validation (245-247) |
| `db/peer/peer_manager.h` | Mutable safety_net_interval_seconds_ member, DisconnectedPeerState struct, disconnected_peers_ map, CURSOR_GRACE_PERIOD_MS | VERIFIED | All four present |
| `db/peer/peer_manager.cpp` | sync_timer_loop uses mutable member, reload_config updates it, grace period logic, stale cleanup, cursor_compaction_loop update | VERIFIED | Timer at 2668, reload at 2817, grace-period check at 459-471 |
| `db/tests/peer/test_peer_manager.cpp` | Tests for MAINT-04/MAINT-05 tagged [peer-manager][safety-net] | VERIFIED | 3 new tests, 12 assertions, all pass |
| `deploy/configs/node1-trusted.json` | safety_net_interval_seconds: 600 | VERIFIED | Fixed in this iteration — now contains correct key and value |
| `deploy/configs/node2-trusted.json` | safety_net_interval_seconds: 600 | VERIFIED | Fixed in this iteration |
| `deploy/configs/node3-trusted.json` | safety_net_interval_seconds: 600 | VERIFIED | Fixed in this iteration |
| `tests/integration/*.sh` (32 files) | safety_net_interval_seconds key in inline configs | MISSING | All 32 scripts still use sync_interval_seconds; none use new key |
| `deploy/run-benchmark.sh` | safety_net_interval_seconds in config generation and jq reads | MISSING | 11 occurrences of old key; jq reads return null with old key |
| `db/README.md` | Updated config example and field description | MISSING | Lines 127 and 159 still document sync_interval_seconds; new key absent |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|----|--------|---------|
| peer_manager.cpp sync_timer_loop | safety_net_interval_seconds_ | timer.expires_after reads mutable member | WIRED | `2668: timer.expires_after(std::chrono::seconds(safety_net_interval_seconds_))` |
| peer_manager.cpp reload_config | safety_net_interval_seconds_ | SIGHUP reload updates from new config | WIRED | `2817: safety_net_interval_seconds_ = new_cfg.safety_net_interval_seconds;` |
| peer_manager.cpp on_peer_disconnected | disconnected_peers_ | Records timestamp before peers_ erase | WIRED | `511: disconnected_peers_[peer_hash] = DisconnectedPeerState{now_ms};` |
| peer_manager.cpp on_peer_connected | disconnected_peers_ | Checks grace period, erases entry | WIRED | `459: disconnected_peers_.find(peer_hash)` + `471: erase` |
| peer_manager.cpp sync_timer_loop stale cleanup | storage_.delete_peer_cursors | MDBX cursor deletion for expired peers | WIRED | `2689: storage_.delete_peer_cursors(it->first)` |
| peer_manager.cpp cursor_compaction_loop | disconnected_peers_ | Includes grace-period peers in known set | WIRED | `3721: for (const auto& [hash, state] : disconnected_peers_)` |
| tests/integration/*.sh inline configs | safety_net_interval_seconds | Config key in Docker node configs | NOT_WIRED | All 32 scripts use old key; node ignores it, defaults to 600s |
| deploy/run-benchmark.sh jq reads | .safety_net_interval_seconds | jq extracts interval from result JSON | NOT_WIRED | Reads .sync_interval_seconds — returns null |

### Data-Flow Trace (Level 4)

Not applicable — this phase implements behavior logic (timer, grace period tracking, cursor lifecycle), not data rendering.

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| Config validation rejects safety_net_interval_seconds < 60 | `chromatindb_tests "[config]"` | All tests passed (232 assertions in 128 test cases) | PASS |
| Safety-net tests pass | `chromatindb_tests "[peer-manager]"` | All tests passed (12 assertions in 3 test cases) | PASS |
| Node accepts safety_net_interval_seconds | confirmed via config.cpp known_keys set | Key present at line 64 | PASS |
| Node rejects sync_interval_seconds | confirmed via config.cpp known_keys set | Key absent from known_keys; triggers unknown key warning | FAIL — 32 integration test scripts pass this stale key |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| MAINT-04 | 82-02 | Peer cursors compacted immediately on disconnect with 5-min grace period | SATISFIED | DisconnectedPeerState tracking in on_peer_disconnected; cursor_compaction_loop excludes grace-period peers; stale cleanup with delete_peer_cursors in sync_timer_loop |
| MAINT-05 | 82-02 | Full reconciliation runs on peer connect/reconnect (catch-up path) | SATISFIED | on_peer_connected spawns run_sync_with_peer for initiator; unit test "sync-on-connect runs for initiator" verifies blob propagation |
| MAINT-06 | 82-01 | Safety-net reconciliation at long interval (default 600s) | SATISFIED | safety_net_interval_seconds default 600, timer loop runs sync_all_peers every interval |
| MAINT-07 | 82-01 | sync_interval_seconds repurposed to safety_net_interval_seconds with 600s default | PARTIAL | Complete in db/ source and deploy/configs; 32 integration test scripts and deploy/run-benchmark.sh still use old key; db/README.md documents old key |

All 4 requirement IDs from PLAN frontmatter accounted for. No orphaned requirements found.

### Anti-Patterns Found

| File | Lines | Pattern | Severity | Impact |
|------|-------|---------|----------|--------|
| `tests/integration/*.sh` (32 files) | various | `"sync_interval_seconds": 5` in inline configs | Blocker | Node ignores key (unknown key warning), falls back to 600s; all timer-dependent integration tests would time out waiting for sync |
| `deploy/run-benchmark.sh` | 281, 291, 301, 578, 697, 752, 1107, 1179, 1233, 1546, 1739 | `sync_interval_seconds` in config blocks and jq reads | Blocker | Config key silently ignored; jq reads `.sync_interval_seconds` return `null`, breaking benchmark result parsing |
| `db/README.md` | 127, 159 | `sync_interval_seconds` in config example and field description | Warning | User-facing documentation incorrect; operators copying the example config will get an unknown key warning |

Note: `.claude/worktrees/agent-acdd5a02/` contains additional stale references in a git worktree artifact — not production code, not flagged.

Note: The pre-existing test failure "Blob with timestamp 1hr+1s in future rejected" (test_engine.cpp:1731) is unrelated to Phase 82.

### Human Verification Required

#### 1. Integration test timing after key rename

**Test:** After renaming sync_interval_seconds to safety_net_interval_seconds in all 32 integration test scripts, determine the correct value to use. Current test value (5s) is below the 60s validation minimum. Options: lower the minimum in test builds, update test values to >= 60 and accept slower runs, or add sync_interval_seconds as a deprecated alias.
**Expected:** Decision made, tests updated, and integration tests pass end-to-end with the renamed key
**Why human:** Requires a policy decision on the validation minimum tradeoff, then Docker-based test execution

### Gaps Summary

The three deploy/configs files were fixed and that specific gap is closed.

However this re-verification uncovered that the rename gap is substantially larger than the previous verification reported. The initial scan checked `deploy/configs/` and `db/` but missed `tests/integration/` (32 scripts) and `deploy/run-benchmark.sh` (11 occurrences). This is the same root cause (MAINT-07 incomplete rename) with a much larger blast radius.

The practical impact is severe: every integration test that requires sync to happen within seconds passes `sync_interval_seconds: 5` in its inline node config. The node ignores this key (logs a warning) and defaults to 600s. Any integration test that waits for sync propagation would time out. The benchmark script is similarly broken — it reads `.sync_interval_seconds` via jq and gets null.

There is a secondary complication with the minimum validation: the 60s minimum enforced by `config.cpp:245` is correct for production use, but integration tests need sub-minute intervals. This requires a design decision before the integration test scripts can simply be renamed.

The C++ implementation (config, peer_manager, unit tests) is fully correct. The gap is entirely in the operational and integration test layer.

---

_Verified: 2026-04-04T09:15:00Z_
_Verifier: Claude (gsd-verifier)_
