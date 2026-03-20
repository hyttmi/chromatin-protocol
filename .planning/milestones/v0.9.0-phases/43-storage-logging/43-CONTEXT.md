# Phase 43: Storage & Logging - Context

**Gathered:** 2026-03-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Production-grade observability (structured logs, file output, complete metrics) and storage health (cursor compaction, tombstone GC, integrity verification). Six requirements: STOR-01, STOR-02, STOR-03, OPS-03, OPS-04, OPS-05.

</domain>

<decisions>
## Implementation Decisions

### Log file configuration
- File logging is additive: file sink added alongside console (stdout + file simultaneously)
- Three new config fields: `log_file` (path string, empty=disabled), `log_max_size_mb` (uint32, default 10), `log_max_files` (uint32, default 3)
- Maps directly to spdlog::rotating_file_sink_mt parameters
- If log file path is invalid or unwritable at startup: warn to stderr, fall back to console-only (node still starts)
- All new config fields validated by existing validate_config()

### Structured log format
- New config field: `log_format` with values "text" (default) or "json"
- JSON format produces machine-parseable structured log output
- Validated by validate_config() (reject unknown format values)

### Cursor compaction
- New periodic timer (hardcoded 6-hour interval, no config field -- YAGNI)
- Uses connection-based staleness: prune cursors for peers NOT in the currently-connected set
- Logging: info-level summary only ("cursor compaction: removed N entries for M peers")
- Timer added to cancel_all_timers() following Phase 42 pattern

### Tombstone GC
- Research libmdbx compaction/copy API first (investigate whether file can physically shrink)
- Fallback: Claude's discretion -- if no practical compaction path exists, document as mmap behavior with test evidence proving freed pages are reused
- Must verify the GC code is actually deleting entries correctly (not just a measurement issue)

### Startup integrity scan
- Blocks startup: scan completes before accepting connections
- If inconsistencies found: log warnings but still start (do not refuse to start)
- Read-only scan of all sub-databases (blob, tombstone, expiry, cursor, delegation, seq_map, namespace)

### Metrics completeness
- Add missing counters to log_metrics_line(): quota_rejections, sync_rejections (already tracked in Metrics struct but not emitted in periodic output)
- Add byte/tombstone counters if not already tracked
- Both periodic timer and SIGUSR1 dump_metrics() emit the same complete set

### Claude's Discretion
- JSON log field names and structure
- spdlog formatter pattern for JSON output
- Integrity scan implementation details (which checks per sub-database)
- Compaction approach if libmdbx supports it
- Metrics line format adjustments for new counters

</decisions>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches. User trusts Claude's judgment on implementation details for this infrastructure phase.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/logging/logging.h`: init() and get_logger() -- currently console-only, extend with file sink and JSON formatter
- `db/storage/storage.cpp:cleanup_stale_cursors()`: Already implements connection-based cursor pruning, takes known peer set as parameter
- `db/peer/peer_manager.cpp:log_metrics_line()`: Existing periodic metrics output -- extend with missing counters
- `db/peer/peer_manager.cpp:dump_metrics()`: SIGUSR1 handler -- already calls log_metrics_line() plus per-peer breakdown
- `db/config/config.cpp:validate_config()`: Phase 42 validation framework -- extend with new field checks
- `db/peer/peer_manager.h:cancel_all_timers()`: Phase 42 timer consolidation -- add new cursor compaction timer here

### Established Patterns
- Config loading: nlohmann/json j.value() with defaults, then validate_config()
- Timer ownership: std::unique_ptr<asio::steady_timer> members, nullable, cancel via ->cancel()
- Metrics struct: simple uint64_t counters, incremented inline, read periodically
- Storage sub-databases: named mdbx maps (blob, tombstone, expiry, cursor, delegation, seq_map, namespace)

### Integration Points
- `db/logging/logging.h:init()`: Add file sink + JSON formatter support (new parameters or overload)
- `db/peer/peer_manager.cpp:run()`: Spawn cursor compaction timer loop alongside existing timers
- `db/peer/peer_manager.cpp:log_metrics_line()`: Add quota_rejections, sync_rejections to format string
- `db/main.cpp:cmd_run()`: Pass log_file, log_max_size_mb, log_max_files, log_format to logging::init()
- `db/storage/storage.cpp`: Add integrity_scan() method, called from main.cpp before PeerManager construction

</code_context>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 43-storage-logging*
*Context gathered: 2026-03-20*
