# Phase 55: Runtime Compaction - Context

**Gathered:** 2026-03-22
**Status:** Ready for planning

<domain>
## Phase Boundary

Automatic mdbx compaction for long-running nodes on constrained devices. Node reclaims disk space without restart by periodically copying the database with COMPACT flag and swapping the result in. Single requirement: COMP-01.

</domain>

<decisions>
## Implementation Decisions

### Compaction trigger
- Periodic timer, same pattern as cursor_compaction_loop() and expiry_scan_loop()
- NOT threshold-based — simple timer is more predictable for operators
- Configurable via `compaction_interval_hours` in config
- 0 disables compaction entirely (consistent with inactivity_timeout_seconds=0 pattern)
- SIGHUP hot-reload: yes — cancel current timer, restart with new interval (Phase 54 precedent)

### Default schedule
- Default interval: 6 hours (matches cursor compaction cadence, appropriate for constrained devices with high churn)
- Minimum: 1 hour (enforced by validate_config — compaction is I/O heavy, full DB copy)
- No maximum constraint

### Operator visibility
- Single info-level log line on completion: "compaction complete: {before} -> {after} ({reduction}%) in {duration}s"
- Add `last_compaction_time` and `compaction_count` to SIGUSR1 metrics dump
- No separate "starting" log line — one line with all the signal

### Claude's Discretion
- mdbx_env_copy2 implementation details (temp file path, atomic swap strategy)
- How to handle compaction failure (retry logic, error logging)
- Whether compaction needs to close/reopen the environment or can use live copy
- Test strategy for verifying file size reduction
- PROTOCOL.md updates if needed

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `cursor_compaction_loop()` in `db/peer/peer_manager.cpp:2408` — timer-cancel pattern, exact template for compaction loop
- `expiry_scan_loop()` in `db/peer/peer_manager.cpp:1824` — another timer loop with SIGHUP reload (Phase 54)
- `used_bytes()` in `db/storage/storage.cpp:991` — returns mmap geometry (before-compaction size)
- `used_data_bytes()` in `db/storage/storage.cpp:1000` — returns B-tree occupancy (actual data volume)
- SIGUSR1 metrics dump in `db/peer/peer_manager.cpp:2298` — add compaction stats here

### Established Patterns
- Config field pattern: add to `Config` struct, parse in `from_json`, validate in `validate_config`, log at startup in `main.cpp`
- Timer-cancel pattern: `steady_timer* member_ = nullptr`, cancel in `cancel_all_timers()`, check `stopping_` in loop
- SIGHUP reload: `reload_config()` reads new config, updates member, cancels timer to restart with new interval
- Pimpl for Storage: `Impl` struct holds `mdbx::env_managed env` and all map handles

### Integration Points
- `db/storage/storage.h` — add compaction method to Storage class (accesses `impl_->env`)
- `db/config/config.h` — add `compaction_interval_hours` field
- `db/peer/peer_manager.cpp` — add compaction timer loop, wire SIGHUP reload
- `db/peer/peer_manager.cpp` SIGUSR1 handler — add compaction metrics
- `mdbx::env_managed::copy()` or `mdbx_env_copy2()` with `MDBX_CP_COMPACT` — the actual compaction API

</code_context>

<specifics>
## Specific Ideas

- The known issue documents that mmap file size doesn't shrink after tombstone GC (freed pages reused internally). mdbx_env_copy2(COMPACT) is the standard solution — creates a fresh copy with only live data.
- shrink_threshold is already set to 4 MiB in geometry, but that only handles geometry-level shrinking, not page-level fragmentation.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 55-runtime-compaction*
*Context gathered: 2026-03-22*
