# Phase 90: Observability & Documentation - Research

**Researched:** 2026-04-05
**Domain:** Prometheus metrics endpoint (C++ / Standalone Asio) + documentation refresh
**Confidence:** HIGH

## Summary

Phase 90 has two distinct domains: (1) implementing a minimal HTTP /metrics endpoint in the C++ node for Prometheus scraping, and (2) refreshing all project documentation to reflect v2.1.0 features. Both domains are well-constrained by locked decisions in CONTEXT.md.

The metrics endpoint is architecturally simple: a coroutine-based TCP acceptor on the shared io_context that responds to `GET /metrics` with Prometheus text exposition format 0.0.4. All 11 NodeMetrics counters already exist and are tracked in `peer_manager.h`. The HTTP handler is minimal -- no HTTP library, no parsing beyond detecting the GET /metrics request line. The `log_metrics_line()` function already computes all derived values (blob_count, storage_mib, uptime) that the endpoint needs to expose.

The documentation work requires updating four files: PROTOCOL.md (add SyncNamespaceAnnounce type 62 wire format, BlobNotify namespace filtering, /metrics config), README.md (add observability section), SDK README (verify completeness, add Brotli note), and getting-started.md (add metrics config example). PROTOCOL.md already documents Brotli compression (suite 0x02) but is missing type 62 entirely -- not even in the message type table.

**Primary recommendation:** Build the metrics HTTP handler as a standalone coroutine class (or inline in PeerManager), following the same acceptor + coroutine-per-connection pattern used by UdsAcceptor. Config field `metrics_bind` (string, default empty). Documentation updates are straightforward in-place edits.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Minimal HTTP responder on the shared io_context in PeerManager -- no new threads, no HTTP library. Parse only GET /metrics and return Prometheus text exposition format. Anything else gets 404. NOT a general-purpose HTTP server.
- D-02: The HTTP listener is a new asio::ip::tcp::acceptor owned by PeerManager, started alongside other acceptors in start(). One coroutine per accepted connection. Read until \r\n\r\n, check for GET /metrics, respond, close.
- D-03: The endpoint serves plain text content-type text/plain; version=0.0.4; charset=utf-8 (Prometheus text exposition format).
- D-04: All metrics use chromatindb_ prefix. Monotonic NodeMetrics counters map to Prometheus counters (_total suffix). Current-state values (peer count, blob count, storage bytes, namespace count, uptime) map to Prometheus gauges.
- D-05: No labels on metrics (no per-peer or per-namespace breakdown). Single flat namespace. This keeps the implementation trivial and the scrape cheap.
- D-06: No histograms. Counters and gauges only.
- D-07: Include # HELP and # TYPE lines for each metric per Prometheus convention.
- D-08: New config field metrics_bind (string, default empty = disabled). When non-empty, parsed as host:port and used to bind the HTTP listener. Example: "127.0.0.1:9090". Localhost-only by default when enabled.
- D-09: SIGHUP reloadable -- same pattern as max_peers_ reload. If metrics_bind changes from empty to non-empty, start listener. If changes from non-empty to empty, stop listener. If bind address changes, restart listener.
- D-10: Graceful shutdown: metrics acceptor stopped in cancel_all_timers() alongside other resources.
- D-11: Update existing sections in-place. No major restructuring. Add new sections for v2.1.0 features where they naturally fit.
- D-12: PROTOCOL.md updates: document SyncNamespaceAnnounce (type 62) wire format, BlobNotify namespace filtering behavior, envelope compression suite 0x02, and /metrics endpoint configuration.
- D-13: README.md: add Observability section (metrics endpoint config + example Prometheus scrape config), update feature list with v2.1.0 additions.
- D-14: SDK README: already updated with multi-relay failover in Phase 89. Verify completeness, add Brotli compression note if missing.
- D-15: getting-started.md: already updated with multi-relay in Phase 89. Add metrics configuration example for operators.

### Claude's Discretion
- Exact metric names (as long as they follow chromatindb_ prefix and Prometheus naming conventions)
- HTTP response buffer strategy (stack buffer vs dynamic allocation -- both fine for a single /metrics response)
- Order of documentation sections

### Deferred Ideas (OUT OF SCOPE)
None.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| OPS-02 | Node exposes Prometheus-compatible HTTP /metrics endpoint (localhost-only default, opt-in via config) | Asio TCP acceptor pattern from Server/UdsAcceptor; Config struct extension; SIGHUP reload pattern from reload_config(); Prometheus text exposition format 0.0.4 spec |
| OPS-03 | /metrics endpoint exposes all existing metrics (peers, blobs, sync, storage, connections) | NodeMetrics struct (11 counters at peer_manager.h:75-87); log_metrics_line() derived values (blob_count, storage_mib, uptime); peers_.size() for current peer count |
| DOC-01 | PROTOCOL.md updated with compression frame format, SyncNamespaceAnnounce, and /metrics | SyncNamespaceAnnounce (type 62) missing from message table and has no documentation section; Brotli suite 0x02 already documented; /metrics is new node feature |
| DOC-02 | README.md updated with compression, filtering, and observability features | README.md needs Observability section with metrics config and Prometheus scrape example |
| DOC-03 | SDK README updated with multi-relay failover API and Brotli support | SDK README has multi-relay failover (Phase 89) but zero mention of Brotli/compression |
| DOC-04 | Getting-started tutorial updated with metrics and relay resilience | getting-started.md has multi-relay section (Phase 89) but no metrics or compression content |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | (existing, FetchContent) | TCP acceptor + coroutine for HTTP metrics | Already used for all networking; no new dependency |

### Supporting
No new libraries required. The HTTP metrics endpoint is hand-crafted (by design per D-01). Prometheus text format is simple enough to emit with string formatting.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Hand-crafted HTTP | prometheus-cpp | Explicitly OUT OF SCOPE per REQUIREMENTS.md -- conflicts with single-threaded Asio model |
| Hand-crafted HTTP | cpp-httplib | Unnecessary dependency for a single GET endpoint; adds thread complexity |

## Architecture Patterns

### Metrics HTTP Endpoint Integration

The metrics endpoint fits into PeerManager's existing architecture:

```
PeerManager
  |-- Server (TCP acceptor for peers)
  |-- UdsAcceptor (UDS for clients)
  |-- metrics_acceptor_ (NEW: TCP acceptor for HTTP /metrics)
  |-- NodeMetrics metrics_
  |-- Storage& storage_
```

### Pattern 1: Coroutine-per-Connection HTTP Handler
**What:** Each accepted HTTP connection spawns a coroutine that reads the request, formats the response, sends it, and closes.
**When to use:** For the /metrics endpoint (one coroutine per scrape request).
**Example:**
```cpp
// Follows the exact pattern used by UdsAcceptor::accept_loop()
asio::awaitable<void> PeerManager::metrics_accept_loop() {
    while (!stopping_) {
        auto [ec, socket] = co_await metrics_acceptor_->async_accept(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        asio::co_spawn(ioc_, metrics_handle_connection(std::move(socket)),
                       asio::detached);
    }
}

asio::awaitable<void> PeerManager::metrics_handle_connection(
    asio::ip::tcp::socket socket) {
    // Read until \r\n\r\n (HTTP header end)
    // Check for "GET /metrics"
    // Format Prometheus response
    // Send HTTP response
    // Close socket
}
```

### Pattern 2: Timer-Cancel for Metrics Acceptor Lifecycle
**What:** The acceptor pointer is stored as a member and cancelled in cancel_all_timers() and stop().
**When to use:** For graceful shutdown of the metrics listener.
**Example:**
```cpp
// Member: std::unique_ptr<asio::ip::tcp::acceptor> metrics_acceptor_;
// In cancel_all_timers():
if (metrics_acceptor_) metrics_acceptor_->close();
```

### Pattern 3: SIGHUP-Reloadable Config Field
**What:** Mutable member initialized from config, updated in reload_config().
**When to use:** For metrics_bind field that can be toggled on/off via SIGHUP.
**Example:**
```cpp
// In reload_config():
auto new_bind = new_cfg.metrics_bind;
if (new_bind != metrics_bind_) {
    if (!metrics_bind_.empty()) stop_metrics_listener();
    metrics_bind_ = new_bind;
    if (!metrics_bind_.empty()) start_metrics_listener();
}
```

### Recommended Metric Names
Based on Prometheus naming conventions and existing NodeMetrics:

**Counters (monotonic, _total suffix):**
```
chromatindb_ingests_total          -- Successful blob ingestions
chromatindb_rejections_total       -- Failed ingestions (validation errors)
chromatindb_syncs_total            -- Completed sync rounds
chromatindb_rate_limited_total     -- Rate limit disconnections
chromatindb_peers_connected_total  -- Total peer connections since startup
chromatindb_peers_disconnected_total -- Total peer disconnections since startup
chromatindb_cursor_hits_total      -- Namespaces skipped via cursor match
chromatindb_cursor_misses_total    -- Namespaces requiring full hash diff
chromatindb_full_resyncs_total     -- Full resync rounds triggered
chromatindb_quota_rejections_total -- Namespace quota exceeded rejections
chromatindb_sync_rejections_total  -- Sync rate limit rejections
```

**Gauges (current state, no suffix):**
```
chromatindb_peers_connected        -- Current number of connected peers
chromatindb_blobs_stored           -- Total blobs across all namespaces
chromatindb_storage_bytes          -- Current storage usage in bytes
chromatindb_namespaces             -- Number of active namespaces
chromatindb_uptime_seconds         -- Node uptime in seconds
```

### Prometheus Text Exposition Format 0.0.4

Per the official Prometheus specification:

```
# HELP chromatindb_ingests_total Successful blob ingestions since startup.
# TYPE chromatindb_ingests_total counter
chromatindb_ingests_total 1234

# HELP chromatindb_peers_connected Current number of connected peers.
# TYPE chromatindb_peers_connected gauge
chromatindb_peers_connected 5
```

**Key format rules:**
- Lines separated by `\n` (line feed)
- Last line MUST end with `\n`
- `# HELP metric_name Description text` -- one per metric
- `# TYPE metric_name counter|gauge` -- one per metric, before first sample
- `metric_name value` -- no labels per D-05
- Content-Type: `text/plain; version=0.0.4; charset=utf-8`

**HTTP response format:**
```
HTTP/1.1 200 OK\r\n
Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n
Content-Length: {len}\r\n
Connection: close\r\n
\r\n
{prometheus text body}
```

For 404 (non-metrics requests):
```
HTTP/1.1 404 Not Found\r\n
Content-Length: 0\r\n
Connection: close\r\n
\r\n
```

### Anti-Patterns to Avoid
- **Using an HTTP library:** D-01 explicitly prohibits this. The endpoint is intentionally minimal.
- **Labels on metrics:** D-05 says no per-peer or per-namespace breakdown. Flat namespace only.
- **Histograms or summaries:** D-06 says counters and gauges only.
- **New threads:** D-01 requires the shared io_context. No thread pool for HTTP.
- **Persistent HTTP connections:** Each connection should be read-respond-close. No keep-alive.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Prometheus text format | Full metrics library | String formatting (fmt/spdlog-style) | Only 16 metric lines; format is trivial |
| HTTP parsing | Full HTTP parser | Simple string search for "GET /metrics" | Only one endpoint; anything else is 404 |
| Config host:port parsing | Custom parser | Same parse_address() from Server class | Already exists and handles edge cases |

**Key insight:** This is deliberately minimal. The entire HTTP handler is ~100 lines. Fighting the urge to generalize is the right approach here.

## Common Pitfalls

### Pitfall 1: Blocking the io_context with Metrics Computation
**What goes wrong:** Computing blob_count iterates all namespaces via storage_.list_namespaces(). If this blocks the event loop, it delays all other operations.
**Why it happens:** log_metrics_line() already does this every 60s successfully, but high scrape frequency could amplify it.
**How to avoid:** The implementation is fine as-is -- list_namespaces() is O(N namespaces) not O(N blobs), and Prometheus scrapes are typically every 15-30s. No special handling needed.
**Warning signs:** Scrape latency exceeding 100ms on nodes with thousands of namespaces.

### Pitfall 2: Incomplete HTTP Request Read
**What goes wrong:** Reading only a fixed buffer size and missing partial requests, or hanging on slow clients.
**Why it happens:** HTTP requests can arrive in multiple TCP segments.
**How to avoid:** Read until `\r\n\r\n` (end of HTTP headers) with a reasonable timeout (e.g., 5 seconds) and maximum buffer size (e.g., 4096 bytes -- more than enough for any metrics scrape request). Close on timeout or overflow.
**Warning signs:** Prometheus scrape failures with connection timeouts.

### Pitfall 3: Acceptor Lifecycle on SIGHUP
**What goes wrong:** Restarting the metrics acceptor while connections are in-flight causes dangling coroutines.
**Why it happens:** Closing the acceptor stops new connections but in-flight coroutines still reference the old socket.
**How to avoid:** Close the acceptor (stops accept_loop), then create a new one. In-flight metrics_handle_connection coroutines will complete naturally since they own their socket. The stopping_ flag prevents new accept iterations.
**Warning signs:** Segfaults after SIGHUP with metrics_bind change.

### Pitfall 4: PROTOCOL.md Message Type Table Completeness
**What goes wrong:** Adding type 62 documentation section but forgetting to add it to the message type summary table.
**Why it happens:** The table at lines 670-706 ends at type 61. Type 62 was added in Phase 86 but PROTOCOL.md was not updated.
**How to avoid:** Add type 62 to the table AND create a detailed documentation section.
**Warning signs:** Message type table ending at 61 when wire format supports 62.

### Pitfall 5: SDK README Missing Brotli Documentation
**What goes wrong:** SDK README mentions write_encrypted() but says nothing about compression being enabled by default.
**Why it happens:** Phase 87 (Brotli compression) was implemented in the SDK but the README was not updated.
**How to avoid:** DOC-03 explicitly requires adding a Brotli compression note to SDK README. Mention that compression=True is the default in envelope_encrypt() and transparent in decrypt.
**Warning signs:** SDK README showing no mention of compression or Brotli.

## Code Examples

### Prometheus Text Body Generation
```cpp
// Source: existing NodeMetrics struct + log_metrics_line() pattern
std::string PeerManager::format_prometheus_metrics() {
    auto storage_bytes = storage_.used_data_bytes();
    auto uptime = compute_uptime_seconds();
    uint64_t blob_count = 0;
    auto namespaces = storage_.list_namespaces();
    for (const auto& ns : namespaces) {
        blob_count += ns.latest_seq_num;
    }

    std::string body;
    // Counters
    body += "# HELP chromatindb_ingests_total Successful blob ingestions since startup.\n";
    body += "# TYPE chromatindb_ingests_total counter\n";
    body += "chromatindb_ingests_total " + std::to_string(metrics_.ingests) + "\n";
    // ... repeat for all 11 counters ...
    // Gauges
    body += "# HELP chromatindb_peers_connected Current number of connected peers.\n";
    body += "# TYPE chromatindb_peers_connected gauge\n";
    body += "chromatindb_peers_connected " + std::to_string(peers_.size()) + "\n";
    // ... repeat for blob_count, storage_bytes, namespaces.size(), uptime ...
    return body;
}
```

### HTTP Response Assembly
```cpp
// Source: Prometheus text exposition format 0.0.4 specification
std::string body = format_prometheus_metrics();
std::string response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
    "Content-Length: " + std::to_string(body.size()) + "\r\n"
    "Connection: close\r\n"
    "\r\n" + body;
```

### Config Parsing Pattern
```cpp
// Source: existing config.cpp pattern (line 31-52)
// In Config struct:
std::string metrics_bind;  // Empty = disabled, "host:port" = enabled

// In load_config():
cfg.metrics_bind = j.value("metrics_bind", cfg.metrics_bind);

// In known_keys set:
"metrics_bind"

// In validate_config():
if (!cfg.metrics_bind.empty()) {
    auto colon = cfg.metrics_bind.rfind(':');
    if (colon == std::string::npos) {
        errors.push_back("metrics_bind must be host:port format");
    }
    // Same port validation as bind_address
}
```

### SyncNamespaceAnnounce Wire Format Documentation
```
// Source: peer_manager.cpp line 472-529, encode_namespace_list()
// Type 62 payload: [uint16_be count][ns_id:32][ns_id:32]...
// Empty set (count=0) means replicate all namespaces.
// Both peers send this immediately after handshake (before sync-on-connect).
// Relay blocks type 62 from clients.
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| SIGUSR1 dump only | SIGUSR1 + HTTP /metrics | Phase 90 (this phase) | Enables automated Prometheus/Grafana monitoring |
| Undocumented type 62 | PROTOCOL.md with full wire spec | Phase 90 (this phase) | Cross-language implementers can support namespace filtering |
| No compression docs in SDK | SDK README + tutorial with Brotli info | Phase 90 (this phase) | Users know compression is on by default |

## Documentation Gap Analysis

### PROTOCOL.md Gaps (DOC-01)
1. **Type 62 in message table:** Missing. Must add row after type 61.
2. **SyncNamespaceAnnounce section:** Missing entirely. Needs wire format, behavior (both peers send after handshake), empty-set semantics, SIGHUP re-announce behavior.
3. **BlobNotify namespace filtering:** Partially documented (source exclusion and sync suppression mentioned). Missing: "BlobNotify is only sent to peers whose announced namespaces include the blob's namespace."
4. **Compression (suite 0x02):** Already fully documented at lines 1054-1083. No changes needed.
5. **/metrics endpoint:** Not documented. Add operational section with config, format, metric list.

### README.md Gaps (DOC-02)
1. **Observability section:** Missing entirely. Add after Keepalive section.
2. **Feature list:** Does not mention compression, filtering, or multi-relay.

### SDK README Gaps (DOC-03)
1. **Brotli/compression:** Zero mentions. Must add note about transparent compression (enabled by default for encrypted blobs >= 256 bytes).
2. **Multi-relay failover:** Already documented (Phase 89). No changes needed.

### Getting-Started Tutorial Gaps (DOC-04)
1. **Metrics configuration:** Not mentioned. Add operator-facing config example.
2. **Compression:** Not mentioned. Brief note that encryption uses Brotli by default.
3. **Multi-relay failover:** Already documented (Phase 89). No changes needed.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (FetchContent, latest) |
| Config file | db/CMakeLists.txt lines 214-254 |
| Quick run command | `cd build && ctest --test-dir . -R "metrics" --output-on-failure -j1` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| OPS-02 | Config field `metrics_bind` parsed, validated, SIGHUP-reloadable | unit | `ctest -R "config" --output-on-failure` | Existing: db/tests/config/test_config.cpp (extend) |
| OPS-02 | HTTP /metrics endpoint responds with 200 and Prometheus format | integration | Docker integration test | Wave 0 |
| OPS-03 | All 11 counters + 5 gauges present in /metrics output | unit | `ctest -R "metrics" --output-on-failure` | Wave 0 |
| OPS-03 | Non-metrics GET returns 404 | unit | `ctest -R "metrics" --output-on-failure` | Wave 0 |
| DOC-01 | PROTOCOL.md contains type 62, namespace filtering, /metrics | manual | Visual review | N/A |
| DOC-02 | README.md contains observability section | manual | Visual review | N/A |
| DOC-03 | SDK README mentions Brotli compression | manual | Visual review | N/A |
| DOC-04 | Getting-started has metrics config example | manual | Visual review | N/A |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "config|metrics" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before /gsd:verify-work

### Wave 0 Gaps
- [ ] `db/tests/peer/test_metrics_endpoint.cpp` -- covers OPS-02, OPS-03 (metrics format, 404 handling)
- [ ] Add test file to `db/CMakeLists.txt` test executable sources
- [ ] Extend `db/tests/config/test_config.cpp` with metrics_bind parsing tests

## Environment Availability

No external dependencies beyond the existing build toolchain. The metrics endpoint uses only Standalone Asio (already available). No new libraries, services, or tools required.

## Open Questions

1. **Exact metric for UDS connections**
   - What we know: `uds_acceptor_->connection_count()` exists and is logged in dump_metrics().
   - What's unclear: Should /metrics expose a `chromatindb_uds_connections` gauge? D-05 says "no per-peer breakdown" but UDS connection count is a global gauge.
   - Recommendation: Include it as a gauge -- it is a useful operational metric and follows the "expose all existing metrics" requirement (OPS-03). Claude's discretion area.

2. **Compaction metrics**
   - What we know: `compaction_count_` and `last_compaction_time_` exist on PeerManager.
   - What's unclear: Whether to expose these as Prometheus metrics.
   - Recommendation: Include compaction_count as a counter and last_compaction_time as a gauge. Both are useful for operators.

## Sources

### Primary (HIGH confidence)
- `db/peer/peer_manager.h` lines 73-87 -- NodeMetrics struct (all 11 counters verified)
- `db/peer/peer_manager.cpp` lines 3768-3801 -- log_metrics_line() (derived values: blob_count, storage_mib, uptime)
- `db/config/config.h` -- Config struct (metrics_bind field insertion point)
- `db/config/config.cpp` -- Config parsing and validation patterns
- `db/net/uds_acceptor.h` -- Coroutine-based acceptor pattern to follow
- `db/PROTOCOL.md` -- Current documentation state (type 62 confirmed missing)
- [Prometheus Exposition Formats](https://prometheus.io/docs/instrumenting/exposition_formats/) -- Text format 0.0.4 specification

### Secondary (MEDIUM confidence)
- Prometheus naming conventions for `_total` suffix on counters -- verified against official docs

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies; uses existing Asio patterns
- Architecture: HIGH -- all integration points verified in source code; acceptor/coroutine/timer-cancel patterns well-established in codebase
- Pitfalls: HIGH -- based on direct codebase analysis, not speculation
- Documentation gaps: HIGH -- every gap verified by direct file inspection

**Research date:** 2026-04-05
**Valid until:** 2026-05-05 (stable domain, no fast-moving dependencies)
