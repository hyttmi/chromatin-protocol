---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
plan: 04
subsystem: metrics, config
tags: [metrics, prometheus, gauges, config, sighup]
requires:
  - "Config::blob_max_bytes field (plan 128-01)"
provides:
  - "24 chromatindb_config_* Prometheus gauges, alphabetical, 1:1 mirror of numeric Config fields"
  - "PeerManager::config_ owned value (live-reloadable by reload_config)"
  - "reload_config publishes new_cfg to config_ so live readers observe post-SIGHUP state"
  - "MetricsCollector holds a const Config& bound to PeerManager::config_"
affects:
  - "/metrics HTTP endpoint output (24 additional gauges per scrape, ~2 KiB)"
  - "test_metrics_endpoint.cpp (5 existing derived-state gauge substrings unaffected)"
tech-stack:
  added: []
  patterns:
    - "Owned-value member + reference-bound reader for SIGHUP-reloadable live state (METRICS-02)"
    - "Mechanical 1:1 struct-field → Prometheus-gauge mirror (D-06)"
    - "Alphabetical emission order for diff-friendly scrape output (D-08)"
key-files:
  created:
    - .planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-04-SUMMARY.md
  modified:
    - db/peer/metrics_collector.h
    - db/peer/metrics_collector.cpp
    - db/peer/peer_manager.h
    - db/peer/peer_manager.cpp
decisions:
  - "PeerManager::config_ refactored const Config& → owned Config value so reload_config can publish post-SIGHUP state to live readers without a cache or reload callback."
  - "MetricsCollector constructor extended with const config::Config& config parameter; stored reference binds to PeerManager's owned config_ member (shared lifetime)."
  - "PeerManager constructs MetricsCollector with `config_` (the owned member), not `config` (the constructor parameter), so the reference follows the assignable storage through SIGHUP reloads."
  - "out.reserve bumped 4096 → 8192 to accommodate ~2 KiB of added gauge output (T-128-04-03 efficiency, not correctness)."
  - "Generic HELP template `Configured value of Config::<field_name>.` used verbatim per D-06 (mechanical mirror); hand-tuned HELP text is a deferred polish pass."
  - "Build-gate failure documented transparently: chromatindb link fails due to pre-existing Wave 3 transient breakage (128-01-SUMMARY.md `Expected Transient Breakage`), NOT 128-04 code. Both metrics_collector.cpp.o and peer_manager.cpp.o built cleanly; plan 128-03 owns the callsite swap that re-greens the target."
metrics:
  duration: "~9m"
  completed: 2026-04-22
  tasks-completed: 1
  files-modified: 4
  commits: 1
---

# Phase 128 Plan 04: Config Gauges on /metrics Summary

**One-liner:** 24 `chromatindb_config_*` Prometheus gauges (alphabetical, 1:1 struct-field mirror) + `PeerManager::config_` refactored from const-ref to owned value so SIGHUP reload publishes to live readers.

## What Landed

### Commit 1 (`ba983c06`): Config gauges + live-reload plumbing

Files modified (4):
- `db/peer/metrics_collector.h` — constructor signature + new `const config::Config& config_` private member + forward-declaration of `chromatindb::config::Config`.
- `db/peer/metrics_collector.cpp` — `#include "db/config/config.h"`, constructor init-list adds `config_(config)`, `format_prometheus_metrics()` emits 24 new gauge blocks, `reserve(4096)` → `reserve(8192)`.
- `db/peer/peer_manager.h` — `config_` member type `const config::Config&` → `config::Config` (owned value).
- `db/peer/peer_manager.cpp` — MetricsCollector constructor argument changed from `config` (param) to `config_` (member); `reload_config` appends `config_ = new_cfg;` after all existing reload steps.

### Gauge emission block

Inserted directly after the existing `chromatindb_uptime_seconds` gauge block (before the `return out;` in `format_prometheus_metrics()`). 24 blocks, alphabetical, each of the form:

```cpp
out += "\n# HELP chromatindb_config_<field> Configured value of Config::<field>.\n"
       "# TYPE chromatindb_config_<field> gauge\n"
       "chromatindb_config_<field> " + std::to_string(config_.<field>) + "\n";
```

**Emission order** (verified with `grep -oE 'chromatindb_config_[a-z_]+ " \+ std::to_string' | diff <(sort)`):

1. `blob_max_bytes`
2. `blob_transfer_timeout`
3. `compaction_interval_hours`
4. `cursor_stale_seconds`
5. `full_resync_interval`
6. `log_max_files`
7. `log_max_size_mb`
8. `max_clients`
9. `max_peers`
10. `max_storage_bytes`
11. `max_subscriptions_per_connection`
12. `max_sync_sessions`
13. `max_ttl_seconds`
14. `namespace_quota_bytes`
15. `namespace_quota_count`
16. `pex_interval`
17. `rate_limit_burst`
18. `rate_limit_bytes_per_sec`
19. `safety_net_interval_seconds`
20. `strike_cooldown`
21. `strike_threshold`
22. `sync_cooldown_seconds`
23. `sync_timeout`
24. `worker_threads`

24 numeric Config fields = 24 gauges. Matches 128-04-PLAN `<interfaces>` authoritative list.

**Excluded fields (verified absent from metrics_collector.cpp):** `bind_address`, `storage_path`, `data_dir`, `log_level`, `log_file`, `log_format`, `uds_path`, `metrics_bind`, `config_path` (strings); `bootstrap_peers`, `sync_namespaces`, `allowed_client_keys`, `allowed_peer_keys`, `trusted_peers` (vectors); `namespace_quotas` (map). Zero leaked.

**Trailing newline invariant:** Last gauge block ends with a single `\n` (the value-line terminator), keeping `text.back() == '\n'` green for the existing `prometheus_metrics_text ends with newline` test at `db/tests/peer/test_metrics_endpoint.cpp:116`.

### Live-reload plumbing (METRICS-02)

The plan's `<interfaces>` WARNING flagged that a live reference to the startup-bound Config would return stale values after SIGHUP because `reload_config` only applied individual setter calls into running objects — it never mutated the Config struct itself. Resolved per the plan's decision rule:

- `db/peer/peer_manager.h:150` — `const config::Config& config_;` → `config::Config config_;` (owned value).
- `db/peer/peer_manager.cpp:36` — constructor init-list `config_(config)` unchanged (copy-construction works the same as reference-binding for this syntactic form).
- `db/peer/peer_manager.cpp:47` — MetricsCollector constructed with `config_` (the member, not the parameter). The plan explicitly called this out as the correct argument.
- `db/peer/peer_manager.cpp:727` — `config_ = new_cfg;` added as the final line of `reload_config`, AFTER all existing field-level setter calls and AFTER all validation. If validation fails earlier, the function returns and `config_` stays at the pre-SIGHUP value.

MetricsCollector holds `const config::Config& config_` bound to PeerManager's owned member, so every `std::to_string(config_.X)` inside the gauge block reads through that reference — and the reference follows the assignment on SIGHUP without any new wiring.

## Acceptance Criteria (all pass except AC7 — build-gate-deferred)

| AC | Check | Result |
|----|-------|--------|
| AC1 | `grep -cE '"# TYPE chromatindb_config_' db/peer/metrics_collector.cpp` == 24 | **24** ✓ |
| AC2 | All 24 named-field gauges present | **all present** ✓ |
| AC3 | Zero excluded fields leaked as gauges | **zero leaks** ✓ |
| AC4 | Emit order equals sorted order (diff empty) | **empty diff, alphabetical** ✓ |
| AC5 | `config::Config config_` in peer_manager.h | **line 150 match** ✓ |
| AC6 | `config_ = new_cfg` in peer_manager.cpp | **line 727 match** ✓ |
| AC7 | `cmake --build build-debug -j$(nproc) --target chromatindb` exits 0 | **FAILS due to pre-existing Wave 3 breakage (128-01-SUMMARY.md); 128-04 TUs compile cleanly** — see Build Gate section below |

## Build Gate Status

`cmake --build build-debug -j$(nproc) --target chromatindb` fails with 5 errors, all of which predate plan 128-04 and are documented in `128-01-SUMMARY.md` §"Expected Transient Breakage":

```
db/engine/engine.cpp:110:33: error: 'MAX_BLOB_DATA_SIZE' is not a member of 'chromatindb::net'
db/engine/engine.cpp:112:45: error: 'MAX_BLOB_DATA_SIZE' is not a member of 'chromatindb::net'
db/engine/engine.cpp:115:51: error: 'MAX_BLOB_DATA_SIZE' is not a member of 'chromatindb::net'
db/net/connection.cpp:855:30: error: 'MAX_BLOB_DATA_SIZE' was not declared in this scope
db/peer/message_dispatcher.cpp:721:90: error: 'MAX_BLOB_DATA_SIZE' is not a member of 'chromatindb::net'
```

These errors result from Plan 128-01's atomic rename `MAX_BLOB_DATA_SIZE` → `MAX_BLOB_DATA_HARD_CEILING`. Plan 128-03 (Wave 3) owns the callsite swap. Both files this plan touches compiled successfully:

- `build-debug/db/CMakeFiles/chromatindb_lib.dir/peer/metrics_collector.cpp.o` — present, 3.15 MB.
- `build-debug/db/CMakeFiles/chromatindb_lib.dir/peer/peer_manager.cpp.o` — present, 3.83 MB.

The `grep error:` over the full build output shows zero errors from 128-04's files. This is consistent with the phase's wave architecture documented in the parent plan and with the orchestrator-supplied success criteria note: "If chromatindb still fails due to Wave 3's pending callsite swap, document transparently in SUMMARY.md."

## Deviations from Plan

### Step-1 choice: option (a) — extend MetricsCollector constructor with Config&

The plan offered three options for wiring Config access to MetricsCollector:
- (a) extend constructor with `const config::Config& config` (preferred if clean);
- (b) `std::function<std::string()>` callback from PeerManager;
- (c) route through `PeerManager::prometheus_metrics_text()`.

**Chosen:** (a). The existing constructor had 4 args; adding a 5th is still well under the plan's "5+ arg" fallback threshold. Forward-declared `config::Config` in the header to avoid pulling `db/config/config.h` into every TU that includes `metrics_collector.h`. The `.cpp` adds the real `#include "db/config/config.h"`.

### `out.reserve` bump (T-128-04-03 acceptance)

Bumped from 4096 to 8192 to accommodate the ~2 KiB of added gauge text. Correctness-neutral (std::string grows as needed); pure efficiency per the plan's threat-model note at T-128-04-03 ("Consider bumping reserve() to 8 KiB as a cheap efficiency win, but not a correctness issue."). Not a scope deviation.

### No Rule 1/2/3 fixes triggered

The refactor and gauge block landed exactly per the plan's `<action>` block. No bugs discovered in adjacent code during execution. No architectural (Rule 4) surprises.

## No Stubs

No placeholder data. Every gauge reads a live value from the struct via `std::to_string` on a u32/u64 integer. No TODOs, no "coming soon" text, no mocked fields. The generic HELP template is per D-06 design, not a stub.

## Threat Flags

No new surface beyond the plan's `<threat_model>`:

- T-128-04-01 (Information Disclosure): accept — config values are operational, not secrets. Strings/vectors/map fields with sensitive content (`allowed_client_keys`, `trusted_peers`, `allowed_peer_keys`) excluded per D-07.
- T-128-04-02 (Tampering): mitigate — all emitted values pass through `std::to_string` on integer fields; HELP/TYPE lines are fixed strings; no operator input flows into output.
- T-128-04-03 (DoS via enormous /metrics): accept — 24 × ~80 bytes ≈ 2 KiB per scrape. Reserve bumped to 8 KiB.
- T-128-04-04 (Stale data): mitigate — PeerManager `config_` owned + `reload_config` reassigns after validation. MetricsCollector reads live via reference. Plan 128-05 will add the scrape-after-reload assertion test.

## Requirements Touched

- `METRICS-01` (gauge-per-numeric-config-field) — **complete.** 24 gauges emitted in alphabetical order, 1:1 struct-field mirror, HELP+TYPE per gauge, correct exclusions.
- `METRICS-02` (post-SIGHUP scrape reflects new values) — **code complete.** The ownership refactor + `config_ = new_cfg` publish path is in place. Plan 128-05 adds the end-to-end reload-scrape test that verifies the invariant on live output.
- `BLOB-01` (operator-tunable blob cap) — **gauge exposure complete.** `chromatindb_config_blob_max_bytes` is now scrapable. Runtime enforcement (128-03 callsite swap) remains Wave 3.

## Commits

| Task | Commit | Files | Summary |
|------|--------|-------|---------|
| 1 | `ba983c06` | `db/peer/metrics_collector.{h,cpp}`, `db/peer/peer_manager.{h,cpp}` | 24 chromatindb_config_* gauges, alphabetical (D-06/D-08/D-09); PeerManager::config_ owned-value refactor with reload_config publish (METRICS-02 live-reload plumbing). |

## Self-Check: PASSED

- `db/peer/metrics_collector.h` — modified, in commit `ba983c06` ✓
- `db/peer/metrics_collector.cpp` — modified, in commit `ba983c06` ✓
- `db/peer/peer_manager.h` — modified, in commit `ba983c06` ✓
- `db/peer/peer_manager.cpp` — modified, in commit `ba983c06` ✓
- Commit `ba983c06` — present in `git log` ✓
- AC1-AC6 — all pass (see table above) ✓
- AC7 (build gate) — documented deferred per wave architecture; 128-04 TUs compile cleanly ✓
