# Architecture Research: v2.1.0 Integration Points

**Domain:** Compression, filtering, observability, and resilience features for chromatindb
**Researched:** 2026-04-04
**Confidence:** HIGH

## System Overview: Current Architecture

```
                    SDK Clients (Python)
                         |
                    TCP + PQ AEAD
                         |
              +----------v-----------+
              |     Relay (C++)      |  TCP:4201
              |  message_filter.h    |  per-client RelaySession
              |  blocklist approach  |  UDS conn per session
              +----------+-----------+
                         |
                    UDS (TrustedHello)
                         |
              +----------v-----------+
              |  chromatindb Node    |  TCP:4200
              |  PeerManager         |  single io_context thread
              |  BlobEngine          |  thread_pool for crypto
              |  Storage (libmdbx)   |
              +----------+-----------+
                         |
                    TCP + PQ AEAD
                         |
                  Other Peer Nodes
```

### Component Responsibilities (Existing)

| Component | Responsibility | Integration Surface |
|-----------|----------------|---------------------|
| PeerManager | Peer lifecycle, sync, message dispatch, BlobNotify fan-out, SIGHUP reload, metrics | Main coordination point for most new features |
| Connection | AEAD transport, send queue, handshake, keepalive | Compression hooks into send_message/recv |
| RelaySession | Bidirectional message forwarding, per-client UDS | Subscription forwarding, UDS reconnect |
| message_filter | Blocklist of peer-internal types | No changes needed (blocklist approach) |
| Storage | libmdbx blob store, cursors, namespaces | Metrics data source |
| BlobEngine | Signature verification, dedup, ingest pipeline | Compression at ingest boundary |
| Config/RelayConfig | JSON config, validation, SIGHUP reload | Hot reload expansion |

## v2.1.0 Feature Integration Map

### Feature 1: Brotli Blob Compression

**What changes:** Wire-level compression for blob payloads on Data, BlobTransfer, BlobFetchResponse, and ReadResponse messages.

**Integration points:**

```
                  Sender                              Receiver
                    |                                    |
  blob data ----> compress(brotli) ----> AEAD encrypt ----> AEAD decrypt ----> decompress(brotli) ----> blob data
                    |                                    |
            payload = [flag:1][compressed_data]    check flag byte, decompress if set
```

| Component | Change Type | What |
|-----------|-------------|------|
| db/CMakeLists.txt | NEW dependency | FetchContent google/brotli v1.2.0, link brotlienc-static + brotlidec-static |
| db/util/compression.h/.cpp | NEW files | `compress(span<uint8_t>) -> vector<uint8_t>` and `decompress(span<uint8_t>, size_t max) -> vector<uint8_t>`. One-shot BrotliEncoderCompress/BrotliDecoderDecompress. Quality 4 (fast, good ratio). |
| PeerManager::on_peer_message | MODIFY | Decompress payload for Data, BlobTransfer, BlobFetchResponse before engine dispatch |
| PeerManager (send paths) | MODIFY | Compress payload in Data, BlobTransfer, BlobFetchResponse send coroutines |
| PeerManager (client paths) | MODIFY | Compress ReadResponse payload before sending; decompress Data from clients |
| SDK: chromatindb/_codec.py | MODIFY | Compress blob payload in encode_blob_payload, decompress in decode_read_response |
| SDK: pyproject.toml | MODIFY | Add `brotli` pip dependency |

**Wire format decision:** Prefix compressed payloads with a 1-byte flag: `0x00` = uncompressed, `0x01` = brotli. This allows receivers to handle both compressed and uncompressed data during any transition period and makes the format self-describing. The flag byte is part of the plaintext payload, inside AEAD encryption.

**Where NOT to compress:**
- BlobNotify (77 bytes, tiny, fixed-format)
- BlobFetch (64 bytes, just hashes)
- Control messages (Ping, Pong, Subscribe, etc.)
- Sync reconciliation (ReconcileInit/Ranges/Items -- hash data, incompressible)

**Compression threshold:** Only compress payloads larger than 256 bytes. Below that, brotli overhead exceeds savings.

**Architecture pattern:** Compression operates at the PeerManager dispatch layer (after AEAD decrypt, before engine ingest; after engine read, before AEAD encrypt). NOT at the Connection/framing layer -- that would compress all messages including incompressible ones and break the blocklist filter which inspects plaintext type bytes.

### Feature 2: Prometheus /metrics HTTP Endpoint

**What changes:** New HTTP listener on a configurable port serving Prometheus text exposition format.

**Integration points:**

```
  Prometheus scraper ----> HTTP GET /metrics ----> metrics_server
                                                        |
                                                  reads NodeMetrics
                                                  reads Storage stats
                                                  reads PeerManager state
```

| Component | Change Type | What |
|-----------|-------------|------|
| db/net/metrics_server.h/.cpp | NEW files | Standalone Asio TCP acceptor, HTTP/1.1 GET /metrics handler, renders Prometheus text format |
| db/config/config.h | MODIFY | Add `metrics_port` field (uint16_t, 0=disabled, default 0) |
| db/config/config.cpp | MODIFY | Parse and validate metrics_port |
| PeerManager | MODIFY | Construct MetricsServer, pass reference to NodeMetrics + Storage + peer_count + uptime |
| db/CMakeLists.txt | MODIFY | Add metrics_server.cpp to library sources |

**No new dependencies.** The Prometheus text exposition format is trivially simple -- it is plain text with `# HELP`, `# TYPE`, and `metric_name value` lines. Asio already provides TCP acceptor. A minimal HTTP handler (parse `GET /metrics`, respond with `200 OK` + `Content-Type: text/plain; version=0.0.4`) is ~100 LOC. No need for prometheus-cpp or prometheus-cpp-lite libraries -- they bring HTTP servers of their own and unnecessary abstraction for what is a read-only data dump.

**Metrics to expose (mapping from NodeMetrics):**

```
chromatindb_ingests_total              counter   metrics_.ingests
chromatindb_rejections_total           counter   metrics_.rejections
chromatindb_syncs_total                counter   metrics_.syncs
chromatindb_rate_limited_total         counter   metrics_.rate_limited
chromatindb_peers_connected_total      counter   metrics_.peers_connected_total
chromatindb_peers_disconnected_total   counter   metrics_.peers_disconnected_total
chromatindb_cursor_hits_total          counter   metrics_.cursor_hits
chromatindb_cursor_misses_total        counter   metrics_.cursor_misses
chromatindb_full_resyncs_total         counter   metrics_.full_resyncs
chromatindb_quota_rejections_total     counter   metrics_.quota_rejections
chromatindb_sync_rejections_total      counter   metrics_.sync_rejections
chromatindb_peers_current              gauge     peers_.size()
chromatindb_storage_bytes              gauge     storage_.used_data_bytes()
chromatindb_namespaces_current         gauge     storage_.list_namespaces().size()
chromatindb_uptime_seconds             gauge     compute_uptime_seconds()
```

**Thread safety:** MetricsServer runs its own coroutine on the same io_context (single-threaded, no races). It reads NodeMetrics and Storage directly -- same thread model as metrics_timer_loop.

**Architecture pattern:** Separate acceptor on a different port. No authentication (Prometheus endpoints are conventionally unauthenticated; access control via firewall/bind address). Simple request-response, no keep-alive needed.

### Feature 3: Namespace-Scoped BlobNotify Filtering

**What changes:** BlobNotify is only sent to peers that replicate the relevant namespace, instead of broadcasting to all TCP peers.

**Integration points:**

| Component | Change Type | What |
|-----------|-------------|------|
| PeerManager::on_blob_ingested | MODIFY | Check peer's sync_namespaces before sending BlobNotify |
| PeerInfo | MODIFY | Add `std::set<std::array<uint8_t, 32>> peer_sync_namespaces` tracked per-connection |
| PeerManager::on_peer_connected | MODIFY | Exchange sync_namespaces during initial handshake/setup |

**Current behavior (Phase 79):**
```cpp
for (auto& peer : peers_) {
    if (peer.connection == source) continue;
    if (peer.connection->is_uds()) continue;
    // Send BlobNotify to ALL TCP peers
    co_spawn(send BlobNotify...);
}
```

**New behavior:**
```cpp
for (auto& peer : peers_) {
    if (peer.connection == source) continue;
    if (peer.connection->is_uds()) continue;
    // Skip if peer has restricted namespaces and this ns is not in them
    if (!peer.peer_sync_namespaces.empty() &&
        !peer.peer_sync_namespaces.count(namespace_id)) continue;
    co_spawn(send BlobNotify...);
}
```

**How to learn peer's sync_namespaces:** The existing sync protocol already knows about `sync_namespaces_` on the local node. The simplest approach: after handshake, each node sends its `sync_namespaces` set (or empty = all) to the peer. This could be a new lightweight message type, or piggyback on the existing NodeInfoResponse exchange. A dedicated message (e.g., `SyncNamespaceAnnounce = 62`) is cleaner -- it is a one-shot announcement after connect.

**Fallback:** If a peer never sends namespace announcement, treat as "all namespaces" (backward compatible).

### Feature 4: Relay Subscription Forwarding

**What changes:** Relay intercepts Subscribe/Unsubscribe from clients, tracks subscriptions per-session, and filters node-to-client Notification messages so only subscribed-namespace notifications reach each client.

**Integration points:**

```
  Client ---Subscribe(ns)--> Relay ---Subscribe(ns)--> Node
                               |
                          tracks ns in session
                               |
  Client <--Notification(ns)-- Relay <--Notification(ns)-- Node
                               |
                          checks: is ns in session's subscriptions?
                          YES: forward to client
                          NO: drop silently
```

| Component | Change Type | What |
|-----------|-------------|------|
| RelaySession | MODIFY | Add `std::set<std::array<uint8_t, 32>> subscribed_namespaces_` member |
| RelaySession::handle_client_message | MODIFY | Intercept Subscribe/Unsubscribe, update local set, still forward to node |
| RelaySession::handle_node_message | MODIFY | Filter Notification by checking namespace against subscribed set |

**Current behavior:** Relay blindly forwards all node-to-client messages including Notification. The node already filters by subscription -- so this is redundant filtering. But the value is: if the relay manages multiple client sessions over a single UDS connection (future optimization), or if the node sends notifications for namespaces that a stale subscription state still has, the relay provides defense-in-depth.

**Notification payload parsing:** Notifications use the same 77-byte format as BlobNotify: `[namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]`. The relay extracts the first 32 bytes as namespace_id for the filter check.

**Subscribe payload parsing:** `[uint16_be count][ns_id:32][ns_id:32]...` -- relay decodes the namespace list using the same format PeerManager uses.

### Feature 5: Relay Auto-Reconnect to Node

**What changes:** When the UDS connection to the chromatindb node drops, the relay periodically attempts to reconnect instead of permanently killing client sessions.

**Integration points:**

```
  Relay (running)
       |
  UDS connection to node drops
       |
  handle_node_close fires
       |
  Instead of: teardown("node disconnected")
  Now:        enter reconnect loop
              - pause message forwarding (queue or reject)
              - retry UDS connect every 2s with exponential backoff (cap 30s)
              - on success: resume forwarding, re-send client's subscriptions
              - on max retries exceeded: teardown session
```

| Component | Change Type | What |
|-----------|-------------|------|
| RelaySession | MODIFY | Add UDS reconnect coroutine, state machine (active/reconnecting/dead) |
| RelaySession::handle_node_close | MODIFY | Spawn reconnect loop instead of immediate teardown |
| RelaySession | MODIFY | Buffer or reject client messages during reconnect window |

**Critical detail:** After UDS reconnect, the node sees a fresh TrustedHello connection. Client subscriptions must be re-sent to the node because the previous UDS connection's subscription state is gone (subscriptions are connection-scoped per project decision).

**State machine:**
```
ACTIVE --> (UDS drop) --> RECONNECTING --> (UDS success + re-subscribe) --> ACTIVE
                              |
                         (max retries) --> DEAD --> teardown
```

**Client-facing behavior during reconnect:** Client messages that arrive during RECONNECTING are rejected with a log warning and dropped (not queued -- unbounded queuing is dangerous). The client's auto-reconnect will handle any lost requests. This is simpler and safer than buffering.

### Feature 6: Multi-Relay SDK Failover

**What changes:** SDK `connect()` accepts a list of relay endpoints and tries them in order, failing over to the next on connection/handshake failure.

**Integration points:**

| Component | Change Type | What |
|-----------|-------------|------|
| ChromatinClient.connect() | MODIFY | Accept `relays: list[tuple[str, int]]` in addition to single host/port |
| ChromatinClient.__aenter__ | MODIFY | Try each relay endpoint in order; first successful handshake wins |
| ChromatinClient._connection_monitor | MODIFY | On disconnect, try next relay in rotation before backoff |

**API design:**
```python
# Single relay (backward compatible)
async with ChromatinClient.connect("192.168.1.200", 4201, identity) as c:
    ...

# Multi-relay failover
async with ChromatinClient.connect(
    relays=[("192.168.1.200", 4201), ("192.168.1.201", 4201)],
    identity=identity,
) as c:
    ...
```

**Failover strategy:**
1. On initial connect: try relays in order, first success wins.
2. On disconnect with auto_reconnect: try current relay first (likely transient), then rotate to next.
3. Track last-successful relay index for reconnect preference.
4. All relays exhausted: apply backoff_delay before cycling again.

**No new files needed.** This is a change to client.py and _reconnect.py only.

### Feature 7: Hot Config Reload Expansion

**What changes:** Expand SIGHUP reload to include `max_peers`, `allowed_client_keys`, and `allowed_peer_keys` (the latter two are already reloaded; max_peers is new).

**Integration points:**

| Component | Change Type | What |
|-----------|-------------|------|
| PeerManager::reload_config | MODIFY | Read and apply max_peers from new config |
| PeerManager | MODIFY | If max_peers decreased and current peers exceed new limit, disconnect excess (LRU or random) |

**Current SIGHUP-reloadable fields:**
- allowed_client_keys, allowed_peer_keys (ACL)
- rate_limit_bytes_per_sec, rate_limit_burst
- sync_cooldown_seconds, max_sync_sessions
- safety_net_interval_seconds
- sync_namespaces
- full_resync_interval, cursor_stale_seconds

**New SIGHUP-reloadable field:**
- max_peers: Update the value used by `should_accept_connection()`. If current peer count exceeds new max_peers, do not force-disconnect existing peers -- only reject new connections. Forcibly disconnecting peers is disruptive and unnecessary; natural churn will bring the count down.

**Architecture note:** `max_peers` is currently read from `config_.max_peers` in `should_accept_connection()`. Since `config_` is a const reference, reload requires either: (a) storing max_peers as a mutable member (like `rate_limit_bytes_per_sec_`), or (b) making config non-const. Option (a) is consistent with how all other reloadable fields work.

## Data Flow Changes

### Compression Data Flow (New)

```
Ingest Path (peer Data message):
  recv_encrypted() -> decode TransportMessage -> payload[0] check compression flag
      -> if 0x01: BrotliDecoderDecompress(payload[1:])
      -> BlobEngine::ingest(decompressed_data)

Send Path (BlobTransfer during sync):
  Storage::get_blob() -> raw data
      -> if data.size() > 256: BrotliEncoderCompress(quality=4)
      -> payload = [0x01] + compressed
      -> encode TransportMessage -> send_encrypted()

Client Read Path:
  Storage::get_blob() -> raw data
      -> compress -> send ReadResponse with flag
  SDK:
      -> recv -> check flag -> decompress -> return to caller
```

### Metrics HTTP Data Flow (New)

```
Prometheus GET /metrics (port 9100)
  -> MetricsServer accept + read HTTP request
  -> format_metrics():
       read NodeMetrics (same thread, no lock)
       read Storage::used_data_bytes()
       read Storage::list_namespaces().size()
       read peers_.size()
       read compute_uptime_seconds()
  -> HTTP 200 OK text/plain
  -> close connection
```

### Relay Notification Filtering Flow (Modified)

```
Before (current):
  Node -> Notification(ns=X) -> UDS -> RelaySession -> forward to client

After:
  Node -> Notification(ns=X) -> UDS -> RelaySession
    -> check: X in subscribed_namespaces_?
    -> YES: forward to client
    -> NO: drop
```

## Recommended Build Order

Features have the following dependency graph:

```
  (1) Brotli Compression     (independent)
  (2) Namespace BlobNotify    (independent)
  (3) Relay Subscription Fwd  (independent, but pairs well with #2)
  (4) Relay Auto-Reconnect    (independent)
  (5) Multi-Relay Failover    (independent, SDK-only)
  (6) Hot Config Reload       (independent, small)
  (7) Prometheus /metrics     (independent, reads existing NodeMetrics)
```

No hard dependencies between features. Recommended build order based on complexity and risk:

1. **Hot Config Reload (max_peers)** -- Smallest change (~20 LOC). Establishes the pattern for this milestone. Low risk.

2. **Namespace-Scoped BlobNotify** -- Moderate change to PeerManager::on_blob_ingested. Needs a new wire message type (SyncNamespaceAnnounce = 62) or piggyback mechanism. Core protocol change, build early.

3. **Brotli Compression** -- New dependency, new utility files, touches multiple send/receive paths. Significant surface area but mechanically straightforward. Build after BlobNotify so the notification path is clean.

4. **Relay Subscription Forwarding** -- Relay-side change. Pairs naturally with namespace-scoped BlobNotify since both are about namespace filtering. Moderate complexity.

5. **Relay Auto-Reconnect** -- Relay state machine change. Moderate complexity, needs careful lifecycle management. Independent but benefits from relay subscription forwarding being done (re-subscribe on reconnect needs the subscription tracking).

6. **Multi-Relay SDK Failover** -- SDK-only change. No C++ changes. Can be done in parallel with C++ work but logically comes after relay auto-reconnect (both are resilience features, test together).

7. **Prometheus /metrics** -- New HTTP acceptor. Independent, can slot anywhere. Last because it is observability -- useful for monitoring the features built above.

## Architectural Patterns

### Pattern 1: Compression Flag Byte

**What:** Prefix compressed payloads with a flag byte to make format self-describing.
**When to use:** Any message type that carries variable-size data worth compressing.
**Trade-offs:** 1 byte overhead per message (negligible). Enables mixed compressed/uncompressed traffic. Avoids protocol version negotiation.

```cpp
// Compress
std::vector<uint8_t> compress_payload(std::span<const uint8_t> data) {
    if (data.size() <= 256) {
        std::vector<uint8_t> out(1 + data.size());
        out[0] = 0x00;  // uncompressed
        std::memcpy(out.data() + 1, data.data(), data.size());
        return out;
    }
    auto compressed = brotli_compress(data, /*quality=*/4);
    compressed.insert(compressed.begin(), 0x01);  // brotli flag
    return compressed;
}

// Decompress
std::vector<uint8_t> decompress_payload(std::span<const uint8_t> data, size_t max_size) {
    if (data.empty()) throw std::runtime_error("empty payload");
    if (data[0] == 0x00) return {data.begin() + 1, data.end()};
    if (data[0] == 0x01) return brotli_decompress(data.subspan(1), max_size);
    throw std::runtime_error("unknown compression flag");
}
```

### Pattern 2: Hand-Rolled Prometheus Text Format

**What:** Generate Prometheus exposition text directly without a library.
**When to use:** Small, fixed metric set. Read-only scrape endpoint.
**Trade-offs:** No histogram/summary support (not needed). Zero dependencies. Full control over format.

```cpp
std::string format_metrics(const NodeMetrics& m, uint64_t peers,
                           uint64_t storage_bytes, uint64_t uptime) {
    std::string out;
    out += "# HELP chromatindb_ingests_total Total successful blob ingestions\n";
    out += "# TYPE chromatindb_ingests_total counter\n";
    out += "chromatindb_ingests_total " + std::to_string(m.ingests) + "\n";
    // ... repeat for each metric
    return out;
}
```

### Pattern 3: Relay State Machine for UDS Reconnect

**What:** Three-state machine (ACTIVE / RECONNECTING / DEAD) for relay-to-node UDS lifecycle.
**When to use:** Any component that needs reconnect with graceful degradation.
**Trade-offs:** Client messages dropped during RECONNECTING (acceptable -- SDK has auto-reconnect). Simpler than buffering.

## Anti-Patterns

### Anti-Pattern 1: Compressing at the AEAD Layer

**What people do:** Add compression inside Connection::send_encrypted / recv_encrypted.
**Why it is wrong:** (a) Compresses ALL messages including incompressible ones (hashes, fixed-format control). (b) Relay message_filter inspects plaintext type bytes -- compression before type extraction breaks filtering. (c) Compressing encrypted data is useless; compression must happen on plaintext.
**Do this instead:** Compress at PeerManager dispatch level, only for data-carrying message types, before the transport envelope wraps them.

### Anti-Pattern 2: Using a Full Prometheus Client Library

**What people do:** Pull in prometheus-cpp or prometheus-cpp-lite for metrics exposition.
**Why it is wrong:** These libraries bring their own HTTP servers (civetweb, etc.) and thread models. chromatindb already has Asio for networking. Adding a second HTTP stack creates dep conflicts and a second thread model.
**Do this instead:** Hand-roll the Prometheus text format (trivial for counters and gauges) and serve it via an Asio TCP acceptor. ~100 LOC for the HTTP handler.

### Anti-Pattern 3: Buffering Client Messages During Relay Reconnect

**What people do:** Queue client messages while UDS is down and replay on reconnect.
**Why it is wrong:** Unbounded memory growth if node is down for extended period. Message ordering guarantees become complex. Write acknowledgments are delayed, confusing clients.
**Do this instead:** Drop messages during RECONNECTING. SDK auto-reconnect handles retries. Simpler, safer, bounded memory.

### Anti-Pattern 4: Per-Blob Brotli Quality Negotiation

**What people do:** Negotiate compression quality/algorithm during handshake.
**Why it is wrong:** Over-engineering. Single deployment, no backward compatibility needed. Quality 4 is a good balance for all payloads.
**Do this instead:** Fixed quality, flag byte in payload. If compression algorithm needs to change later, allocate a new flag value (0x02, etc.).

## Integration Boundaries

### Internal Boundaries

| Boundary | Communication | Changes in v2.1.0 |
|----------|---------------|-------------------|
| PeerManager <-> Connection | send_message(type, payload, req_id) | No change -- compression happens BEFORE send_message |
| PeerManager <-> BlobEngine | ingest(blob_data) | No change -- decompression happens BEFORE ingest |
| PeerManager <-> Storage | get_blob, has_blob, list_namespaces | No change -- metrics server reads these |
| PeerManager <-> MetricsServer | MetricsServer reads NodeMetrics& | NEW boundary -- read-only reference |
| RelaySession <-> Connection | message callbacks | Subscription tracking added in relay |
| RelaySession <-> Node UDS | Connection lifecycle | Reconnect loop replaces teardown |
| SDK Client <-> Relay | TCP + PQ AEAD | Compression flag in payload; multi-relay endpoint list |

### New Files Summary

| File | Layer | Purpose |
|------|-------|---------|
| db/util/compression.h | Node | Brotli compress/decompress wrappers |
| db/util/compression.cpp | Node | Implementation |
| db/net/metrics_server.h | Node | Prometheus HTTP endpoint |
| db/net/metrics_server.cpp | Node | Implementation |

### Modified Files Summary

| File | Layer | Changes |
|------|-------|---------|
| db/CMakeLists.txt | Node | Add brotli FetchContent, compression.cpp, metrics_server.cpp |
| db/config/config.h | Node | Add metrics_port |
| db/config/config.cpp | Node | Parse/validate metrics_port |
| db/peer/peer_manager.h | Node | Add MetricsServer member, peer_sync_namespaces in PeerInfo, max_peers_ mutable |
| db/peer/peer_manager.cpp | Node | Compression in send/recv paths, namespace BlobNotify filter, max_peers reload, metrics server setup |
| db/schemas/transport.fbs | Node | Add SyncNamespaceAnnounce = 62 |
| relay/core/relay_session.h | Relay | Add subscribed_namespaces_, reconnect state |
| relay/core/relay_session.cpp | Relay | Subscription interception, notification filtering, UDS reconnect loop |
| sdk/python/chromatindb/client.py | SDK | Multi-relay connect, compression flag handling |
| sdk/python/chromatindb/_codec.py | SDK | Compress/decompress in encode/decode helpers |
| sdk/python/chromatindb/_reconnect.py | SDK | Relay rotation in reconnect logic |
| sdk/python/pyproject.toml | SDK | Add brotli dependency |

## Scaling Considerations

| Concern | Current (3 nodes) | At 50 nodes | At 500 nodes |
|---------|-------------------|-------------|--------------|
| BlobNotify fan-out | 2 sends per ingest | 49 sends (namespace filter helps) | Unsustainable without gossip/tree |
| Metrics scrape | N/A | 50 endpoints, fine for Prometheus | Fine, Prometheus handles 1000+ |
| Compression CPU | Negligible | quality=4 is fast, ~100 MB/s | Thread pool offload if needed |
| Relay reconnect | 1 relay | Each relay independent | Each relay independent |

Namespace-scoped BlobNotify is the first step toward scaling fan-out. At 500+ nodes, a gossip protocol or relay tree would be needed -- but that is far beyond current scope.

## Sources

- [google/brotli](https://github.com/google/brotli) -- v1.2.0, CMake support, MIT license
- [brotli PyPI](https://pypi.org/project/brotli/) -- Python bindings for SDK
- [Prometheus exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/) -- text format spec
- [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) -- evaluated and rejected (brings own HTTP server)
- [prometheus-cpp-lite](https://github.com/biaks/prometheus-cpp-lite) -- evaluated and rejected (unnecessary for counter/gauge only)
- Existing codebase: peer_manager.h/cpp, connection.h/cpp, relay_session.h/cpp, config.h/cpp

---
*Architecture research for: v2.1.0 Compression, Filtering & Observability*
*Researched: 2026-04-04*
