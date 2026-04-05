# Phase 90: Observability & Documentation - Context

**Gathered:** 2026-04-05
**Status:** Ready for planning

<domain>
## Phase Boundary

Expose all existing NodeMetrics counters via a Prometheus-compatible HTTP /metrics endpoint on the C++ node, and refresh all documentation (PROTOCOL.md, README.md, SDK README, getting-started tutorial) to reflect the full v2.1.0 feature set (compression, filtering, multi-relay failover, observability).

</domain>

<decisions>
## Implementation Decisions

### Metrics HTTP Endpoint Architecture
- **D-01:** Minimal HTTP responder on the shared `io_context` in PeerManager — no new threads, no HTTP library. Parse only `GET /metrics` and return Prometheus text exposition format. Anything else gets 404. This is NOT a general-purpose HTTP server.
- **D-02:** The HTTP listener is a new `asio::ip::tcp::acceptor` owned by PeerManager, started alongside other acceptors in `start()`. One coroutine per accepted connection. Read until `\r\n\r\n`, check for `GET /metrics`, respond, close.
- **D-03:** The endpoint serves plain text content-type `text/plain; version=0.0.4; charset=utf-8` (Prometheus text exposition format).

### Prometheus Format
- **D-04:** All metrics use `chromatindb_` prefix. Monotonic NodeMetrics counters map to Prometheus counters (`_total` suffix). Current-state values (peer count, blob count, storage bytes, namespace count, uptime) map to Prometheus gauges.
- **D-05:** No labels on metrics (no per-peer or per-namespace breakdown). Single flat namespace. This keeps the implementation trivial and the scrape cheap.
- **D-06:** No histograms. Counters and gauges only. Keep it simple.
- **D-07:** Include `# HELP` and `# TYPE` lines for each metric per Prometheus convention.

### Configuration and Lifecycle
- **D-08:** New config field `metrics_bind` (string, default empty = disabled). When non-empty, parsed as `host:port` and used to bind the HTTP listener. Example: `"127.0.0.1:9090"`. Localhost-only by default when enabled.
- **D-09:** SIGHUP reloadable — same pattern as `max_peers_` reload. If `metrics_bind` changes from empty to non-empty, start listener. If changes from non-empty to empty, stop listener. If bind address changes, restart listener.
- **D-10:** Graceful shutdown: metrics acceptor stopped in `cancel_all_timers()` alongside other resources.

### Documentation Scope
- **D-11:** Update existing sections in-place. No major restructuring. Add new sections for v2.1.0 features where they naturally fit.
- **D-12:** PROTOCOL.md updates: document SyncNamespaceAnnounce (type 62) wire format, BlobNotify namespace filtering behavior, envelope compression suite 0x02, and /metrics endpoint configuration. These were implemented in phases 86-89 but not yet documented in PROTOCOL.md.
- **D-13:** README.md: add Observability section (metrics endpoint config + example Prometheus scrape config), update feature list with v2.1.0 additions.
- **D-14:** SDK README: already updated with multi-relay failover in Phase 89. Verify completeness, add Brotli compression note if missing.
- **D-15:** getting-started.md: already updated with multi-relay in Phase 89. Add metrics configuration example for operators.

### Claude's Discretion
- Exact metric names (as long as they follow `chromatindb_` prefix and Prometheus naming conventions)
- HTTP response buffer strategy (stack buffer vs dynamic allocation — both fine for a single /metrics response)
- Order of documentation sections

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Node Metrics
- `db/peer/peer_manager.h` lines 73-87 — NodeMetrics struct with all 11 counters
- `db/peer/peer_manager.cpp` `log_metrics_line()` — existing metrics formatting (peers, blobs, storage, syncs, ingests, rejections, rate_limited, cursor_hits/misses, full_resyncs, quota/sync_rejections)
- `db/peer/peer_manager.cpp` `dump_metrics()` — SIGUSR1 handler showing full per-peer breakdown

### Config
- `db/config/config.h` — Config struct (add metrics_bind field here)
- `db/config/config.cpp` — Config parsing and SIGHUP reload logic

### Networking
- `db/net/server.h` — Existing TCP server (acceptor pattern to follow for HTTP listener)
- `db/peer/peer_manager.cpp` `start()` — Where to add metrics listener startup

### Documentation
- `db/PROTOCOL.md` — Wire protocol documentation (needs v2.1.0 additions)
- `README.md` — Project README
- `sdk/python/README.md` — SDK documentation
- `sdk/python/docs/getting-started.md` — SDK tutorial

### Protocol Types (for PROTOCOL.md updates)
- `db/net/message_types.h` — Message type constants (SyncNamespaceAnnounce = 62, BlobNotify = 59, BlobFetch = 60, BlobFetchResponse = 61)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `NodeMetrics` struct already tracks all 11 counter values — /metrics endpoint just reads and formats them
- `log_metrics_line()` already computes derived values (blob_count from namespace seq_nums, storage_mib, uptime) — reuse this logic
- `storage_.used_data_bytes()`, `storage_.list_namespaces()`, `peers_.size()` — all accessible from PeerManager
- Existing SIGHUP reload pattern in `reload_config()` — follow same pattern for metrics_bind changes
- `cancel_all_timers()` — add metrics acceptor cleanup here

### Established Patterns
- Single io_context thread for all network I/O — metrics HTTP must use same pattern (coroutine-based)
- Config parsing in `config.cpp` with JSON field extraction
- Timer-cancel pattern: `asio::steady_timer*` with cancel-and-null in shutdown
- Acceptor pattern in `Server` class (TCP) and UDS acceptor — follow for metrics HTTP

### Integration Points
- PeerManager owns NodeMetrics and all data sources — metrics HTTP handler lives here
- Config struct gets new `metrics_bind` field
- `start()` initializes metrics listener (if configured)
- `reload_config()` handles metrics_bind changes
- `cancel_all_timers()` tears down metrics listener

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. The Prometheus text exposition format is well-defined and straightforward.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 90-observability-documentation*
*Context gathered: 2026-04-05*
