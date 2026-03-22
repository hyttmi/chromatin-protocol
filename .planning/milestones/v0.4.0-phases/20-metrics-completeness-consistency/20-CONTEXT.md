# Phase 20: Metrics Completeness & Consistency - Context

**Gathered:** 2026-03-13
**Status:** Ready for planning

<domain>
## Phase Boundary

Complete log output for all NodeMetrics counters and make shutdown timer cancellation consistent between `on_shutdown_` callback and `PeerManager::stop()`. Remove stale phase-stub comments. Closes METRICS-LOG-INCOMPLETE from v0.4.0 audit.

</domain>

<decisions>
## Implementation Decisions

### Metrics log format
- Semantic grouping: place `connected_total` and `disconnected_total` next to the live peer count; place `rate_limited` near `rejections`
- Shorten field names: `connected_total`, `disconnected_total`, `rate_limited` (not full struct names)
- Rename existing `connections` field to `peers` to distinguish live count from lifetime totals
- Only update `log_metrics_line()` — SIGUSR1 `dump_metrics()` inherits automatically

### Shutdown timer cancellation
- Inline the 4 missing timer cancels directly in the `on_shutdown_` lambda (do not call `stop()` or extract a helper)
- Place new cancels after the existing `expiry_timer_` cancel, preserving save-first order
- Match `PeerManager::stop()` cancel order: expiry, sync, pex, flush, metrics
- Add inline comment: `// Cancel all timers — must match PeerManager::stop()`

### Stale comment cleanup
- Scan ALL source files (db/ and tests/) for stale "Phase N stub" references
- Delete stale comment lines entirely (don't replace with updated text)
- Fix any stale stubs found in implementation files within this phase (not deferred)

### Claude's Discretion
- Exact position of new fields in the format string (within the semantic grouping constraint)
- Whether to scan for other patterns beyond "Phase.*stub" (e.g., TODO references to completed phases)

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `log_metrics_line()` at `db/peer/peer_manager.cpp:1608` — single function used by both periodic timer and SIGUSR1 dump
- `NodeMetrics` struct fields: `rate_limited`, `peers_connected_total`, `peers_disconnected_total` already exist and are incremented

### Established Patterns
- Timer-cancel pattern: each timer is a raw pointer (`asio::steady_timer*`) set during loop, nulled after wait, cancelled via `if (timer_) timer_->cancel()`
- `PeerManager::stop()` at line 180 is the reference for complete timer cancellation (all 5 timers + 2 signals)
- Key-value log format: `spdlog::info("metrics: key=value key=value ...")`

### Integration Points
- `on_shutdown_` lambda registered at `peer_manager.cpp:148` via `server_.set_on_shutdown()`
- `dump_metrics()` at line 1566 calls `log_metrics_line()` — no changes needed there
- `metrics_timer_loop()` at line 1595 calls `log_metrics_line()` — no changes needed there
- Stale comment at `tests/peer/test_peer_manager.cpp:1416`

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 20-metrics-completeness-consistency*
*Context gathered: 2026-03-13*
