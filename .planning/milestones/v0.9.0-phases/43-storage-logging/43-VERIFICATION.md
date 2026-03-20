---
phase: 43-storage-logging
verified: 2026-03-20T05:10:00Z
status: passed
score: 9/9 must-haves verified
re_verification: false
---

# Phase 43: Storage & Logging Verification Report

**Phase Goal:** Node provides production-grade observability (structured logs, file output,
complete metrics) and maintains healthy storage (cursor compaction, tombstone GC,
integrity verification)
**Verified:** 2026-03-20T05:10:00Z
**Status:** passed
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Node logs to rotating file + stdout when log_file configured | VERIFIED | `rotating_file_sink_mt` created in `init()` when `log_file` non-empty; both sinks added to `shared_sinks` vector |
| 2 | Node emits JSON-structured log output when log_format=json | VERIFIED | Pattern `{"ts":...}` set on all sinks when `log_format == "json"` in `logging.cpp:51` |
| 3 | Node falls back to console-only if log file path is invalid | VERIFIED | `spdlog_ex` caught in `logging.cpp:42-45`; `fmt::print(stderr,...)` warning, execution continues |
| 4 | Named loggers (get_logger) write to file sink too | VERIFIED | `get_logger()` creates logger from `shared_sinks.begin(), shared_sinks.end()` — inherits all sinks from `init()` |
| 5 | New log config fields validated at startup | VERIFIED | `validate_config()` rejects invalid `log_format`, `log_max_size_mb < 1`, `log_max_files < 1` |
| 6 | Tombstone GC root cause identified and documented | VERIFIED | Comment block in `storage.h:163-166` documents mmap geometry behavior; `used_data_bytes()` provides accurate B-tree metric |
| 7 | Startup integrity scan reports entry counts for all 7 sub-databases | VERIFIED | `integrity_scan()` reads `ms_entries` for `blobs_map`, `seq_map`, `expiry_map`, `tombstone_map`, `cursor_map`, `delegation_map`, `quota_map` in `storage.cpp:1006-1019` |
| 8 | Cursor compaction timer periodically prunes disconnected peer cursors | VERIFIED | `cursor_compaction_loop()` runs every 6h, calls `storage_.cleanup_stale_cursors(connected)` in `peer_manager.cpp:2287-2310` |
| 9 | Periodic and SIGUSR1 metrics includes quota_rejections and sync_rejections | VERIFIED | `log_metrics_line()` format string includes both counters at `peer_manager.cpp:2259`; `dump_metrics()` also logs them separately at `peer_manager.cpp:2210-2211` |

**Score:** 9/9 truths verified

---

### Required Artifacts

#### Plan 01 Artifacts (OPS-04, OPS-05)

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/logging/logging.h` | Extended `init()` signature with file/format parameters | VERIFIED | 5-param `init()` at line 16-20; `get_logger()` unchanged |
| `db/logging/logging.cpp` | Multi-sink logger setup, JSON pattern, fixed `get_logger` | VERIFIED | `shared_sinks` vector; `rotating_file_sink_mt`; JSON pattern; `get_logger` uses shared sinks |
| `db/config/config.h` | Four new config fields with defaults | VERIFIED | `log_file=""`, `log_max_size_mb=10`, `log_max_files=3`, `log_format="text"` at lines 40-43 |
| `db/config/config.cpp` | Parsing + validation of new log config fields | VERIFIED | `j.value()` for all four fields at lines 47-50; validation rules at lines 265-277; all four in `known_keys` at lines 65-66 |
| `db/main.cpp` | New log config params passed to `logging::init()` | VERIFIED | 5-arg call at lines 108-112 |

#### Plan 02 Artifacts (STOR-01, STOR-02, STOR-03, OPS-03)

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/storage/storage.h` | `integrity_scan()` and `used_data_bytes()` declarations | VERIFIED | Both declared at lines 172-177; `used_bytes()` documented with mmap behavior note |
| `db/storage/storage.cpp` | `integrity_scan()` implementation with cross-reference checks, `used_data_bytes()` | VERIFIED | `integrity_scan()` at lines 998-1037; scoped txn pattern; 2 cross-reference warnings; `used_data_bytes()` at lines 991-996 |
| `db/peer/peer_manager.h` | `cursor_compaction_timer_` member, `cursor_compaction_loop()` declaration | VERIFIED | `cursor_compaction_loop()` at line 235; `cursor_compaction_timer_` at line 294 |
| `db/peer/peer_manager.cpp` | Cursor compaction timer loop, extended `log_metrics_line()` | VERIFIED | `cursor_compaction_loop()` at lines 2287-2310; format string includes both new counters at line 2259 |
| `db/tests/storage/test_storage.cpp` | Tests for `integrity_scan()`, GC entry count verification | VERIFIED | 4 tests at lines 1908-1977 covering: empty scan, populated scan, GC correctness, `used_bytes`/`used_data_bytes` |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/main.cpp` | `db/logging/logging.cpp` | `logging::init()` call with new parameters | VERIFIED | 5-arg call at `main.cpp:108-112` matches new signature |
| `db/config/config.cpp` | `db/config/config.h` | `j.value()` parsing for new fields | VERIFIED | All 4 fields parsed at `config.cpp:47-50`; all 4 in `known_keys` at lines 65-66 |
| `db/logging/logging.cpp` | spdlog sinks | `rotating_file_sink_mt` + `stderr_color_sink_mt` in shared sinks vector | VERIFIED | Both sinks in `shared_sinks`; `get_logger()` uses `shared_sinks.begin(), shared_sinks.end()` |
| `db/peer/peer_manager.cpp` | `db/storage/storage.cpp` | `cleanup_stale_cursors()` call from `cursor_compaction_loop` | VERIFIED | `storage_.cleanup_stale_cursors(connected)` at `peer_manager.cpp:2305` |
| `db/peer/peer_manager.cpp` | `db/peer/peer_manager.h` | `cursor_compaction_timer_` in `cancel_all_timers()` | VERIFIED | `if (cursor_compaction_timer_) cursor_compaction_timer_->cancel()` at `peer_manager.cpp:239` |
| `db/main.cpp` | `db/storage/storage.cpp` | `integrity_scan()` called before PeerManager construction | VERIFIED | `storage.integrity_scan()` at `main.cpp:149`, before PeerManager at line 159+ |
| `db/peer/peer_manager.cpp` | `metrics_` | `quota_rejections` and `sync_rejections` in `log_metrics_line()` | VERIFIED | Format string at `peer_manager.cpp:2259`; args at lines 2272-2273 |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| STOR-01 | 43-02 | Tombstone GC root cause identified and fixed/documented | SATISFIED | `used_bytes()` comment in `storage.h:163-166` documents mmap geometry behavior; `used_data_bytes()` added as accurate alternative |
| STOR-02 | 43-02 | Node automatically prunes cursor entries for disconnected peers | SATISFIED | `cursor_compaction_loop()` runs every 6h, calls `cleanup_stale_cursors()` |
| STOR-03 | 43-02 | Read-only integrity scan at startup, logs inconsistencies | SATISFIED | `integrity_scan()` called in `main.cpp:149`; logs all 7 sub-database entry counts; 2 cross-reference warnings |
| OPS-03 | 43-02 | All tracked metrics counters emitted in periodic and SIGUSR1 output | SATISFIED | `log_metrics_line()` includes `quota_rejections` + `sync_rejections`; `dump_metrics()` (SIGUSR1 path) also logs them |
| OPS-04 | 43-01 | Log output available in JSON format | SATISFIED | JSON pattern `{"ts":...}` applied when `log_format=="json"` |
| OPS-05 | 43-01 | Node can log to rotating file in addition to stdout | SATISFIED | `rotating_file_sink_mt` added alongside `stderr_color_sink_mt` when `log_file` is non-empty |

No orphaned requirements. All 6 IDs from plan frontmatter appear in REQUIREMENTS.md and all 6 are mapped to Phase 43.

---

### Anti-Patterns Found

No blockers or warnings found in modified files. Specific checks:

- No `TODO/FIXME/PLACEHOLDER` comments in modified files
- No stub return values (`return {}`, `return null`, `return []`) in new implementations
- `integrity_scan()` is substantive: reads 7 sub-databases, performs 2 cross-reference checks, logs results
- `cursor_compaction_loop()` is substantive: builds connected set, calls `cleanup_stale_cursors`, logs removed count
- `get_logger()` fix is complete: uses `shared_sinks` vector, not `spdlog::stderr_color_mt(name)`
- `log_metrics_line()` format string confirmed to include both `quota_rejections` and `sync_rejections`

---

### Human Verification Required

None. All observable truths are mechanically verifiable from the source code. The implementation is complete and wired.

Optional smoke test (not blocking):

**Test:** Start node with `"log_file": "/tmp/chromatin-test.log", "log_format": "json"` in config
**Expected:** `/tmp/chromatin-test.log` contains newline-delimited JSON objects
**Why human:** Runtime behavior, not statically verifiable

---

### Commit Verification

All 5 commits documented in SUMMARYs confirmed present in git history:

| Commit | Description |
|--------|-------------|
| `6a171ca` | feat(43-01): add logging config fields with validation |
| `4556f88` | feat(43-01): multi-sink logging with file rotation and JSON format |
| `70a7e7a` | test(43-02): add failing tests for integrity scan and GC verification |
| `dfe7092` | feat(43-02): implement integrity scan, used_data_bytes, and tombstone GC docs |
| `63f9ea9` | feat(43-02): add cursor compaction timer and complete metrics output |

---

### Summary

Phase 43 goal is fully achieved. Both plans executed cleanly with no deviations except one auto-fixed bug (MDBX_BAD_RSLOT in `integrity_scan()` — scoped txn pattern applied). All 6 requirements satisfied. All 9 observable truths verified against actual source code. No stubs, no orphaned artifacts, no anti-patterns.

The node now has:
- Rotating file logging with JSON format option (OPS-04, OPS-05)
- Complete metrics visibility including quota and sync rejection counters (OPS-03)
- Startup integrity scan for all 7 sub-databases (STOR-03)
- Cursor compaction timer preventing unbounded cursor growth (STOR-02)
- Tombstone GC behavior documented and accurate storage metric available (STOR-01)

---

_Verified: 2026-03-20T05:10:00Z_
_Verifier: Claude (gsd-verifier)_
