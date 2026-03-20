# Phase 43: Storage & Logging - Research

**Researched:** 2026-03-20
**Domain:** spdlog multi-sink/JSON logging, libmdbx storage health, periodic timer patterns
**Confidence:** HIGH

## Summary

Phase 43 adds production observability (file logging, structured JSON output, complete metrics) and storage health features (cursor compaction, tombstone GC investigation, startup integrity scan) to chromatindb. All six requirements (STOR-01 through STOR-03, OPS-03 through OPS-05) use existing dependencies with zero new library additions.

The spdlog 1.15.1 library already ships with `rotating_file_sink_mt` and `pattern_formatter` -- the file logging and JSON formatting features can be built entirely with existing spdlog sink and formatter APIs. The key implementation pattern is multi-sink loggers: a console sink plus an optional rotating file sink, both sharing the same pattern (text or JSON). For JSON output, spdlog has no built-in JSON formatter, but the `pattern_formatter` with custom patterns can produce JSON-structured lines using existing format flags (`%Y`, `%l`, `%n`, `%v`, etc.).

For storage health, libmdbx v0.13.11 provides `env::copy(path, compactify=true)` for offline compaction and `txn::get_map_stat()` for per-sub-database entry counts, which are the tools needed for both the tombstone GC investigation and the integrity scan. The `used_bytes()` method currently returns `mi_geo.current` (the current datafile size allocated by mmap geometry), which includes freed-but-reusable pages -- this is the root cause of the tombstone GC "not reclaiming" issue. The file size reflects mmap geometry, not actual data volume. Freed pages ARE reused by subsequent writes, but the file only physically shrinks when freed space exceeds `shrink_threshold` (currently set to 4 MiB).

**Primary recommendation:** Implement in three waves: (1) logging enhancements (OPS-04, OPS-05), (2) metrics completeness (OPS-03), (3) storage health (STOR-01, STOR-02, STOR-03). Each wave is independently testable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- File logging is additive: file sink added alongside console (stdout + file simultaneously)
- Three new config fields: `log_file` (path string, empty=disabled), `log_max_size_mb` (uint32, default 10), `log_max_files` (uint32, default 3)
- Maps directly to spdlog::rotating_file_sink_mt parameters
- If log file path is invalid or unwritable at startup: warn to stderr, fall back to console-only (node still starts)
- All new config fields validated by existing validate_config()
- New config field: `log_format` with values "text" (default) or "json"
- JSON format produces machine-parseable structured log output
- Validated by validate_config() (reject unknown format values)
- Cursor compaction: new periodic timer (hardcoded 6-hour interval, no config field -- YAGNI)
- Uses connection-based staleness: prune cursors for peers NOT in the currently-connected set
- Logging: info-level summary only ("cursor compaction: removed N entries for M peers")
- Timer added to cancel_all_timers() following Phase 42 pattern
- Startup integrity scan blocks startup: scan completes before accepting connections
- If inconsistencies found: log warnings but still start (do not refuse to start)
- Read-only scan of all sub-databases (blob, tombstone, expiry, cursor, delegation, seq_map, namespace/quota)
- Add missing counters to log_metrics_line(): quota_rejections, sync_rejections (already tracked in Metrics struct but not emitted in periodic output)
- Both periodic timer and SIGUSR1 dump_metrics() emit the same complete set

### Claude's Discretion
- JSON log field names and structure
- spdlog formatter pattern for JSON output
- Integrity scan implementation details (which checks per sub-database)
- Compaction approach if libmdbx supports it
- Metrics line format adjustments for new counters

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STOR-01 | Tombstone GC correctly reclaims storage -- root cause identified and fixed (or documented if mmap behavior) | Research confirms `mi_geo.current` reports mmap file size, not data volume. Freed pages are reused internally. `shrink_threshold` of 4 MiB controls when file physically shrinks. `txn::get_map_stat().ms_entries` provides actual entry counts for verification. `env::copy(path, true)` available for compacted backup if needed. |
| STOR-02 | Node automatically prunes cursor entries for peers not seen within configurable age threshold | Existing `cleanup_stale_cursors()` already implements connection-based pruning. Only needs periodic timer wrapper (6-hour interval per CONTEXT.md). Timer pattern identical to existing `expiry_scan_loop()`. |
| STOR-03 | Node performs read-only integrity scan of all sub-databases at startup, logging any inconsistencies | `txn::get_map_stat(map_handle)` returns `ms_entries`, `ms_depth`, `ms_branch_pages`, `ms_leaf_pages` for each sub-database. Cross-referencing entry counts between related maps (blobs vs seq_map, tombstone vs blobs) catches orphaned entries. |
| OPS-03 | All tracked metrics counters emitted in periodic and SIGUSR1 log output | `quota_rejections` and `sync_rejections` already tracked in `NodeMetrics` struct but missing from `log_metrics_line()` format string. Direct addition to the existing format string. |
| OPS-04 | Log output available in structured JSON format for machine parsing | spdlog 1.15.1 `pattern_formatter` supports all needed format flags. JSON output achievable via custom pattern string with escaped field values: `{"ts":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","logger":"%n","msg":"%v"}`. |
| OPS-05 | Node can log to rotating file in addition to stdout (configurable path, max size, max files) | spdlog ships `rotating_file_sink_mt` accepting `base_filename`, `max_size` (bytes), `max_files`. Multi-sink logger via `spdlog::logger(name, {sink1, sink2})` constructor. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| spdlog | v1.15.1 | Structured logging, file sinks, formatters | Already in stack; ships rotating_file_sink_mt and pattern_formatter |
| libmdbx | v0.13.11 | Storage health checks, stat API | Already in stack; provides per-map stats and env info |
| nlohmann/json | v3.11.3 | Config field parsing for new log fields | Already in stack for config loading |

### Supporting
No new libraries needed. All features build on existing dependencies.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Custom JSON pattern string | spdlog bundled JSON sink (does not exist in v1.15.1) | No alternative -- must hand-craft JSON pattern |
| mi_geo.current for storage size | mi_last_pgno * pagesize for actual data | mi_last_pgno * pagesize reflects actual B-tree usage more accurately than mmap geometry |

## Architecture Patterns

### Recommended Changes

```
db/
  logging/
    logging.h         # Extend init() signature with file/format params
    logging.cpp        # Add multi-sink setup, JSON pattern
  config/
    config.h           # Add log_file, log_max_size_mb, log_max_files, log_format fields
    config.cpp         # Parse + validate new fields
  storage/
    storage.h          # Add integrity_scan() public method
    storage.cpp        # Implement integrity_scan()
  peer/
    peer_manager.h     # Add cursor_compaction_timer_ member, cursor_compaction_loop()
    peer_manager.cpp   # Implement timer loop, extend log_metrics_line()
  main.cpp             # Pass new log config to init(), call integrity_scan() before PeerManager
```

### Pattern 1: Multi-Sink Logger Setup
**What:** All loggers share the same set of sinks (console + optional file), set at init time.
**When to use:** When file logging is additive to console.
**Example:**
```cpp
// Source: spdlog v1.15.1 API (rotating_file_sink.h, logger.h)
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

void init(const std::string& level,
          const std::string& log_file,
          uint32_t max_size_mb,
          uint32_t max_files,
          const std::string& log_format) {
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

    std::vector<spdlog::sink_ptr> sinks = {console_sink};

    if (!log_file.empty()) {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file,
                static_cast<size_t>(max_size_mb) * 1024 * 1024,
                static_cast<size_t>(max_files));
            sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            // File sink failed -- fall back to console only
            fmt::print(stderr, "WARNING: log file '{}' not writable: {}\n",
                       log_file, ex.what());
        }
    }

    auto default_logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());
    // Set pattern based on format
    if (log_format == "json") {
        default_logger->set_pattern(
            R"({"ts":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","logger":"%n","msg":"%v"})");
    } else {
        default_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
    }
    default_logger->set_level(parse_level(level));
    spdlog::set_default_logger(default_logger);
}
```

### Pattern 2: Periodic Timer with cancel_all_timers()
**What:** New timer coroutine following the exact same pattern as existing timer loops.
**When to use:** For the 6-hour cursor compaction timer.
**Example:**
```cpp
// Source: db/peer/peer_manager.cpp existing pattern (expiry_scan_loop, metrics_timer_loop)
asio::awaitable<void> PeerManager::cursor_compaction_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        cursor_compaction_timer_ = &timer;
        timer.expires_after(std::chrono::hours(6));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        cursor_compaction_timer_ = nullptr;
        if (ec || stopping_) co_return;

        // Build set of currently-connected peer hashes
        std::vector<std::array<uint8_t, 32>> connected;
        for (const auto& p : peers_) {
            connected.push_back(/* peer pubkey hash */);
        }
        auto removed = storage_.cleanup_stale_cursors(connected);
        if (removed > 0) {
            spdlog::info("cursor compaction: removed {} entries", removed);
        }
    }
}
```

### Pattern 3: Integrity Scan via Map Stats
**What:** Read-only transaction scanning all sub-database statistics at startup.
**When to use:** For STOR-03 startup integrity verification.
**Example:**
```cpp
// Source: libmdbx v0.13.11 C++ API (mdbx.h++ txn::get_map_stat)
void Storage::integrity_scan() {
    auto txn = impl_->env.start_read();

    auto blob_stat = txn.get_map_stat(impl_->blobs_map);
    auto seq_stat  = txn.get_map_stat(impl_->seq_map);
    auto exp_stat  = txn.get_map_stat(impl_->expiry_map);
    auto ts_stat   = txn.get_map_stat(impl_->tombstone_map);
    auto cur_stat  = txn.get_map_stat(impl_->cursor_map);
    auto del_stat  = txn.get_map_stat(impl_->delegation_map);
    auto quo_stat  = txn.get_map_stat(impl_->quota_map);

    spdlog::info("integrity scan: blobs={} seq={} expiry={} tombstone={} "
                 "cursor={} delegation={} quota={}",
                 blob_stat.ms_entries, seq_stat.ms_entries, exp_stat.ms_entries,
                 ts_stat.ms_entries, cur_stat.ms_entries, del_stat.ms_entries,
                 quo_stat.ms_entries);

    // Cross-reference checks:
    // 1. seq_map entries should >= blobs_map entries (seq never deleted, blobs can be)
    // 2. Each tombstone_map entry should have a corresponding blob in blobs_map
    //    (orphaned tombstone_map entries = cleanup failure)
    // 3. quota_map entries should match number of namespaces with blobs
}
```

### Anti-Patterns to Avoid
- **Separate formatters per sink:** Both console and file sinks MUST use the same formatter (text or JSON). Mixing formats makes log correlation impossible.
- **Blocking file I/O in init():** The `rotating_file_sink_mt` constructor tests file writability on construction. If the path is invalid, it throws `spdlog_ex`. Catch this specifically, do not let it abort the node.
- **Using std::call_once for init with new parameters:** The current `logging::init()` uses `std::call_once` which prevents re-initialization. The new implementation must set up sinks on every call (or restructure to accept parameters on first call only). Since `init()` is called once at startup, removing `call_once` and just setting up fresh is cleaner.
- **mi_geo.current as "used storage":** This is the mmap file size, not the actual data volume. Use `mi_last_pgno * pagesize` for actual B-tree occupancy, or `get_map_stat().ms_entries` for entry counts.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Rotating file logs | Custom file rotation logic | spdlog::sinks::rotating_file_sink_mt | Handles rotation, deletion of old files, thread safety |
| JSON log formatting | Custom JSON serializer per log call | spdlog pattern_formatter with JSON pattern string | Consistent formatting, zero per-message overhead |
| Per-map entry counts | Manual cursor iteration to count | txn.get_map_stat(map).ms_entries | O(1) B-tree metadata read, no data scanning |
| Cursor staleness check | Custom peer tracking data structure | Existing cleanup_stale_cursors() | Already implemented and tested |

**Key insight:** spdlog's sink architecture handles all the file rotation complexity. The JSON formatting is achievable with a pattern string rather than a custom formatter class, keeping the implementation minimal.

## Common Pitfalls

### Pitfall 1: spdlog Pattern JSON Escaping
**What goes wrong:** The `%v` (message) pattern flag does not escape JSON special characters. If a log message contains quotes or backslashes, the JSON output becomes malformed.
**Why it happens:** spdlog's pattern formatter does simple string substitution, not JSON-aware escaping.
**How to avoid:** For the metrics and structured log lines we control, this is acceptable since our messages are format-string controlled (no user input). For log messages that could contain arbitrary strings (peer addresses, file paths), avoid including unescaped user data in the message field. Alternatively, accept that JSON output is "best effort" for human-written log messages -- the primary use case is machine parsing of metrics lines where we control the content.
**Warning signs:** JSON parse errors when consuming log output.

### Pitfall 2: std::call_once Preventing Re-init
**What goes wrong:** Current `logging::init()` uses `std::call_once` which means the sink setup only runs once. If `init()` is called with new parameters, they are silently ignored.
**Why it happens:** The original design assumed init is called once with only a level parameter.
**How to avoid:** Remove `std::call_once` since `init()` is only called once at startup anyway. Or, restructure so that the first call does full setup and subsequent calls only adjust the level.
**Warning signs:** File sink not created despite correct config.

### Pitfall 3: Tombstone GC "Not Working" is a Measurement Problem
**What goes wrong:** After deleting blobs with `run_expiry_scan()`, `used_bytes()` (which returns `mi_geo.current`) does not decrease, leading to the conclusion that GC is broken.
**Why it happens:** `mi_geo.current` is the mmap file size, which only shrinks when freed space exceeds `shrink_threshold` (4 MiB in current config). Freed pages are recycled internally by libmdbx's B-tree garbage collector.
**How to avoid:** Verify GC correctness by checking entry counts (`get_map_stat().ms_entries`) before and after GC, not file size. Document that file size is a cosmetic metric for mmap databases.
**Warning signs:** Storage monitoring showing "growing" database size despite active GC.

### Pitfall 4: get_logger() Creates Loggers with Wrong Sinks
**What goes wrong:** Current `get_logger()` calls `spdlog::stderr_color_mt(name)` which creates a new logger with only a console sink, bypassing the file sink.
**Why it happens:** Named loggers created after init() don't automatically inherit the default logger's sinks.
**How to avoid:** Change `get_logger()` to create new loggers using the same sink vector as the default logger. Store the sinks vector as a module-level variable, or clone from the default logger's sinks.
**Warning signs:** Named loggers not appearing in the log file.

### Pitfall 5: validate_config Called Before logging::init
**What goes wrong:** Phase 42 established that `validate_config()` is called before `logging::init()` and uses `std::cerr` for errors. New log-related config fields (log_file, log_format) must be validated with cerr, not spdlog.
**Why it happens:** The validation order is: parse args -> validate_config -> logging::init.
**How to avoid:** Keep the existing order. New config field validation uses the same error accumulation pattern as other fields. The log_file path writability check happens in logging::init(), not in validate_config().
**Warning signs:** Validation errors about log fields silently swallowed.

## Code Examples

### Extending Config Struct
```cpp
// Source: db/config/config.h -- add new fields with defaults
struct Config {
    // ... existing fields ...
    std::string log_file;                           // Empty = disabled (no file logging)
    uint32_t log_max_size_mb = 10;                  // Max size per log file in MiB
    uint32_t log_max_files = 3;                     // Max number of rotated log files
    std::string log_format = "text";                // "text" or "json"
};
```

### Extending validate_config
```cpp
// Source: db/config/config.cpp -- add to validate_config()
// log_format validation
static const std::set<std::string> valid_formats = {"text", "json"};
if (valid_formats.find(cfg.log_format) == valid_formats.end()) {
    errors.push_back("log_format must be 'text' or 'json' (got '" +
                      cfg.log_format + "')");
}

// log_max_size_mb validation
if (cfg.log_max_size_mb < 1) {
    errors.push_back("log_max_size_mb must be >= 1 (got " +
                      std::to_string(cfg.log_max_size_mb) + ")");
}

// log_max_files validation
if (cfg.log_max_files < 1) {
    errors.push_back("log_max_files must be >= 1 (got " +
                      std::to_string(cfg.log_max_files) + ")");
}
```

### Extending log_metrics_line with Missing Counters
```cpp
// Source: db/peer/peer_manager.cpp -- extend format string
spdlog::info("metrics: peers={} connected_total={} disconnected_total={} "
             "blobs={} storage={:.1f}MiB "
             "syncs={} ingests={} rejections={} rate_limited={} "
             "cursor_hits={} cursor_misses={} full_resyncs={} "
             "quota_rejections={} sync_rejections={} uptime={}",
             peers_.size(),
             metrics_.peers_connected_total,
             metrics_.peers_disconnected_total,
             blob_count,
             storage_mib,
             metrics_.syncs,
             metrics_.ingests,
             metrics_.rejections,
             metrics_.rate_limited,
             metrics_.cursor_hits,
             metrics_.cursor_misses,
             metrics_.full_resyncs,
             metrics_.quota_rejections,
             metrics_.sync_rejections,
             uptime);
```

### JSON Pattern String for spdlog
```cpp
// All spdlog format flags used in pattern:
// %Y-%m-%dT%H:%M:%S.%e  = ISO-8601 timestamp with milliseconds
// %l                     = log level (info, warn, error, etc.)
// %n                     = logger name
// %v                     = log message body
//
// Note: %v content is NOT JSON-escaped by spdlog. This is acceptable
// because our log messages are format-string-controlled (no raw user input).
constexpr auto JSON_PATTERN =
    R"({"ts":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","logger":"%n","msg":"%v"})";
```

### Integrity Scan Cross-Checks
```cpp
// Checks to perform per sub-database:
//
// 1. blobs_map vs seq_map:
//    - seq_map entries >= blobs_map entries (seq entries survive blob deletion)
//    - Warning if seq_map has entries but blobs_map is empty (complete data loss)
//
// 2. tombstone_map vs blobs_map:
//    - Each tombstone_map key [ns:32][target_hash:32] should have a corresponding
//      tombstone blob in blobs_map at [ns:32][tombstone_blob_hash:32]
//    - Orphaned tombstone_map entries (no backing blob) = stale index
//    - Note: checking this fully requires iterating tombstone_map and verifying
//      backing blobs exist. For large datasets, just report entry counts.
//
// 3. quota_map:
//    - Number of quota entries should match number of distinct namespaces in blobs_map
//    - rebuild_quota_aggregates() already exists for correction
//
// 4. General health:
//    - All maps should open successfully (verified implicitly by get_map_stat)
//    - Report ms_depth, ms_entries for operational visibility
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Console-only logging | Multi-sink (console + file) | This phase | Production log persistence |
| Unstructured text logs | JSON-structured output option | This phase | Machine-parseable observability |
| used_bytes() = mi_geo.current | Entry counts via get_map_stat() | This phase | Accurate storage health reporting |
| Startup cursor cleanup only | Periodic cursor compaction | This phase | Prevents unbounded cursor growth |

**Key insight on tombstone GC:** The existing GC code in `run_expiry_scan()` correctly deletes tombstone blobs and their tombstone_map index entries when TTL expires (lines 916-924 of storage.cpp). The "not reclaiming" issue is purely a measurement artifact: `mi_geo.current` reports mmap file geometry, not actual data volume. Freed B-tree pages are returned to libmdbx's internal freelist and reused by subsequent writes. The file only physically shrinks when freed space exceeds `shrink_threshold` (4 MiB). This matches the REQUIREMENTS.md language: "root cause identified and fixed (or documented if mmap behavior)".

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (BUILD_TESTING section) |
| Quick run command | `cd build && ./db/chromatindb_tests "[config]" -c "compact"` |
| Full suite command | `cd build && ./db/chromatindb_tests` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| STOR-01 | Tombstone GC verified via entry counts (not file size) | unit | `./db/chromatindb_tests "[storage][tombstone-gc]" -c "compact"` | No -- Wave 0 |
| STOR-02 | Cursor compaction removes entries for disconnected peers | unit | `./db/chromatindb_tests "[storage][cursor-compaction]" -c "compact"` | Partial (cleanup_stale_cursors tested, periodic timer not) |
| STOR-03 | Integrity scan detects inconsistencies and reports them | unit | `./db/chromatindb_tests "[storage][integrity]" -c "compact"` | No -- Wave 0 |
| OPS-03 | log_metrics_line includes quota_rejections, sync_rejections | unit | `./db/chromatindb_tests "[config]" -c "compact"` | No -- verifiable by code review (format string change) |
| OPS-04 | JSON log format produces valid JSON output | unit | `./db/chromatindb_tests "[logging][json]" -c "compact"` | No -- Wave 0 |
| OPS-05 | Rotating file sink creates and rotates log files | unit | `./db/chromatindb_tests "[logging][file]" -c "compact"` | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ./db/chromatindb_tests "[storage]" "[config]" -c "compact"`
- **Per wave merge:** `cd build && ./db/chromatindb_tests`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/storage/test_storage.cpp` -- add STOR-01 tombstone GC entry-count verification tests
- [ ] `db/tests/storage/test_storage.cpp` -- add STOR-03 integrity_scan() tests
- [ ] `db/tests/config/test_config.cpp` -- add log_file, log_max_size_mb, log_max_files, log_format validation tests
- [ ] Logging tests may be hard to unit test (requires capturing sink output); code review + manual test acceptable for OPS-04/OPS-05

## Open Questions

1. **JSON message escaping**
   - What we know: spdlog's `%v` flag does not JSON-escape the message body. Our controlled format strings (metrics lines) are safe. Arbitrary log messages with quotes could produce invalid JSON.
   - What's unclear: Whether this is acceptable for production log ingestion pipelines.
   - Recommendation: Accept as-is for v0.9.0. Document the limitation. If needed later, a custom spdlog formatter class could be added (30-40 lines of code).

2. **Tombstone GC: shrink_threshold behavior in Docker**
   - What we know: libmdbx's `shrink_threshold` is set to 4 MiB. In Docker benchmarks with small datasets, freed pages may never exceed this threshold, so the file never shrinks.
   - What's unclear: Whether to lower `shrink_threshold` or just document the behavior.
   - Recommendation: Document as mmap behavior. Verify with entry counts that GC is working correctly. The file size is cosmetic -- freed pages are reused. Consider adding `mi_last_pgno * pagesize` as an additional metric for "actual data size" alongside the existing `mi_geo.current` "file size" metric.

## Sources

### Primary (HIGH confidence)
- spdlog v1.15.1 source code (build/_deps/spdlog-src/) -- rotating_file_sink.h, pattern_formatter.h, base_sink.h, logger.h
- libmdbx v0.13.11 source code (build/_deps/libmdbx-src/) -- mdbx.h++, mdbx.h (MDBX_stat, MDBX_envinfo, env::copy, txn::get_map_stat)
- Project source code -- db/logging/logging.cpp, db/storage/storage.cpp, db/peer/peer_manager.cpp, db/config/config.cpp

### Secondary (MEDIUM confidence)
- Project memory (MEMORY.md) -- tombstone GC known issue documentation, architectural decisions

### Tertiary (LOW confidence)
- None -- all findings verified against source code

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already in use, APIs verified in source headers
- Architecture: HIGH -- follows established project patterns (timer loops, config loading, storage API)
- Pitfalls: HIGH -- identified from direct code reading (call_once issue, get_logger sink issue, mi_geo.current semantics verified in mdbx.h)
- Tombstone GC root cause: HIGH -- verified mi_geo.current documentation in mdbx.h line 2812 ("Current datafile size") vs mi_last_pgno ("Number of the last used page")

**Research date:** 2026-03-20
**Valid until:** 2026-04-20 (stable -- no moving targets, all deps pinned)
