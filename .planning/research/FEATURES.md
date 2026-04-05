# Feature Research

**Domain:** Decentralized PQ-secure database node -- compression, filtering, observability, resilience (v2.1.0)
**Researched:** 2026-04-04
**Confidence:** HIGH

## Feature Landscape

### Table Stakes (Users Expect These)

Features that any production-grade distributed database should have at this maturity level.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Prometheus /metrics endpoint | Standard observability for any server daemon. Operators expect to scrape metrics without log parsing. Every serious service exposes /metrics. | MEDIUM | Already have SIGUSR1 dump + periodic log line with all the right counters. The data exists -- just need an HTTP endpoint exposing it in Prometheus text format. |
| Hot config reload for max_peers | Already SIGHUP-reload ACL keys, rate limits, quotas, sync interval, log level, trusted peers, compaction interval. max_peers is the one missing piece -- operators expect all config to be reloadable. | LOW | Single line: read new_cfg.max_peers, store to member. Existing connections unaffected (only new accepts check limit). |
| Relay auto-reconnect to node | Current relay tears down client immediately when UDS to node drops (relay_session.cpp:151). Node restart = all SDK clients disconnected. Expected: relay retries UDS, buffers or pauses until node returns. | MEDIUM | Relay currently creates one UDS connection per client session. Auto-reconnect needs a persistent relay-to-node connection model OR per-session reconnect with backoff. |

### Differentiators (Competitive Advantage)

Features that go beyond expectations and provide real value for the use case.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Wire-level Brotli compression | Reduces bandwidth for blob transfer between nodes and from relay to SDK. Especially valuable for the 2592-byte ML-DSA-87 pubkey + ~4627-byte signature overhead that exists on every blob. FlatBuffer metadata and PQ crypto fields are highly compressible. | MEDIUM | Compress TransportMessage plaintext before AEAD encryption, decompress after decryption. Frame-level, not blob-level. Must negotiate capability during handshake (version/flag). CRITICAL CAVEAT: envelope-encrypted blob data (ciphertext) is incompressible -- compression only helps with plaintext blob data, protocol overhead (pubkey, signature, FlatBuffer framing), and metadata messages. |
| Namespace-scoped BlobNotify filtering | Currently on_blob_ingested() sends BlobNotify to ALL TCP peers regardless of interest. With 100 namespaces and 10 peers, that is 1000 notifications for every ingest. Filtering at sender reduces this to only peers that care about the namespace. | LOW | Peers already track subscribed_namespaces for client Notification (type 21). Extend the same concept to BlobNotify for peers: let peers declare which namespaces they sync, skip BlobNotify for non-matching namespaces. sync_namespaces config already exists. |
| Multi-relay SDK failover | SDK currently connects to exactly one host:port. If that relay is down, the SDK is dead. Accepting a list of relays with ordered failover makes the SDK resilient to single-relay failure. | MEDIUM | Extend connect() to accept list of (host, port) tuples. Try in order. On reconnect, rotate through the list. Must handle: which relay to try first on reconnect (last-known-good vs round-robin), timeout per attempt, total timeout across all attempts. |
| Relay subscription forwarding | Relay currently forwards ALL node-to-client messages blindly (relay_session.cpp:129 "No filtering on node->client direction"). The node sends Notifications for all subscribed namespaces on the UDS connection. With namespace-aware relay filtering, the relay only forwards notifications matching the client's actual subscriptions. | MEDIUM | Relay needs to intercept Subscribe/Unsubscribe from client, track per-session subscription set, and filter Notification messages from node. Reduces unnecessary traffic when relay serves many clients with different namespace interests. |

### Anti-Features (Commonly Requested, Often Problematic)

Features that seem good but would cause problems in this system.

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| Per-blob compression (compress before signing) | "Compress data field to save storage and bandwidth" | Breaks the signing model. Canonical signing input is SHA3-256(namespace \|\| data \|\| ttl \|\| timestamp). Compressing data before signing means decompression required before verification. Also breaks content-addressing -- blob_hash would be over compressed data, not original. Cross-SDK decompression compatibility nightmare. | Wire-level frame compression: compress the entire FlatBuffer payload at transport layer, transparent to storage and signing. |
| zstd instead of Brotli | "zstd is faster for large binary data" | PROJECT.md explicitly specifies Brotli. While zstd has better speed for large blobs, Brotli has better ratio for the structured/repetitive data that dominates chromatindb wire traffic (FlatBuffer metadata, 2592-byte pubkeys, protocol messages). For encrypted blob data, NEITHER algorithm helps -- ciphertext is incompressible. The compressible portion is protocol overhead where Brotli excels. | Use Brotli at low compression levels (1-4) for speed comparable to zstd while keeping better ratio on structured data. |
| Full-duplex relay UDS multiplexing | "Single UDS connection from relay to node serving all clients" | Breaks per-client session isolation. Current model: each client gets its own UDS connection to node, so subscriptions, rate limits, and ACL checks are per-client. Multiplexing would require the relay to manage client identity multiplexing, subscription routing, and rate limit attribution -- essentially reimplementing the node's session management. | Keep per-client UDS sessions. For relay auto-reconnect, reconnect the individual session's UDS socket. |
| Push metrics to Grafana/external | "Push metrics to monitoring instead of pull" | Prometheus pull model is standard. Push adds a dependency (pushgateway or direct Grafana push), complicates configuration, and the daemon shouldn't need to know about monitoring infrastructure. | Expose /metrics HTTP endpoint. Let Prometheus scrape. Standard pattern. |
| Compression negotiation per-message | "Negotiate compression per message type -- only compress large payloads" | Per-message negotiation adds complexity to every send/receive path. Tiny messages (Ping, Pong, Subscribe) don't benefit from compression but the overhead of checking is negligible. | Compress ALL frames above a minimum size threshold (e.g., 128 bytes). Small frames pass through uncompressed. Single flag in frame header. |

## Feature Dependencies

```
[Brotli wire compression]
    requires: handshake negotiation (capability flag)
    requires: Python SDK brotli support (pip: brotli)

[Namespace-scoped BlobNotify]
    requires: peer namespace interest declaration (new message type or config exchange)
    enhances: [Relay subscription forwarding] (relay can also filter BlobNotify if exposed)

[Multi-relay SDK failover]
    requires: existing SDK auto-reconnect (Phase 84 -- DONE)
    enhances: [Relay auto-reconnect to node] (failover gives SDK path around relay failure)

[Relay subscription forwarding]
    requires: relay intercepts Subscribe/Unsubscribe messages (currently forwards blindly)
    enhances: [Namespace-scoped BlobNotify] (end-to-end filtering from node to SDK)

[Relay auto-reconnect to node]
    independent -- no feature dependencies
    enhances: SDK stability (fewer disconnects propagating to clients)

[Hot config reload for max_peers]
    independent -- trivial addition to existing reload_config()

[Prometheus /metrics endpoint]
    independent -- uses existing metrics_ struct data
    requires: embedded HTTP server (lightweight, listen on separate port)
```

### Dependency Notes

- **Brotli requires handshake negotiation:** Both sides must agree to use compression. A capability flag in TrustedHello/PQRequired exchange or a new CompressedFrame message type is needed. Older peers that don't understand compression must still work (graceful fallback to uncompressed).
- **Multi-relay failover builds on auto-reconnect:** Phase 84 (SDK auto-reconnect) provides the backoff/retry machinery. Multi-relay extends the "where to reconnect" from single endpoint to ordered list.
- **Relay subscription forwarding enables end-to-end filtering:** Without relay filtering, namespace-scoped BlobNotify at the node only helps peer-to-peer traffic. The relay still forwards all Notifications to all clients. With relay filtering, the full path is optimized.
- **Prometheus endpoint is fully independent:** No protocol changes, no peer interaction. Just a lightweight HTTP listener exposing existing counters.

## Feature Details

### 1. Brotli Wire Compression

**What it does:** Compresses the FlatBuffer plaintext payload before AEAD encryption. Decompresses after AEAD decryption. Operates at the frame level -- transparent to all message types.

**Expected behavior:**
- Negotiated during handshake via capability flag (e.g., `supports_compression: bool` in hello exchange)
- Only active if BOTH sides support it
- Frame header includes a 1-byte compression flag: 0x00 = uncompressed, 0x01 = Brotli
- Minimum frame size threshold (e.g., 128 bytes) -- small frames skip compression
- Brotli quality level 1-4 (fast compression, decent ratio)
- Decompression side must enforce max decompressed size (prevent decompression bombs)

**What compresses well (HIGH confidence):**
- ML-DSA-87 public key (2592 bytes, highly structured binary) -- ~60-70% reduction expected
- ML-DSA-87 signature (~4627 bytes) -- moderate reduction (~20-30%)
- FlatBuffer framing overhead -- good reduction
- Protocol messages (NamespaceList, ReconcileRanges, PeerListResponse) -- excellent reduction
- Plaintext blob data (if not envelope-encrypted) -- good reduction

**What does NOT compress (<1% reduction, HIGH confidence):**
- Envelope-encrypted ciphertext (ChaCha20-Poly1305 output is high-entropy)
- KEM ciphertexts in envelope headers (1568 bytes each, random)
- AEAD tags

**Net benefit assessment:** For a typical blob with envelope encryption, the compressible portion is pubkey (2592B) + signature (~4627B) + FlatBuffer overhead (~100B) = ~7.3 KB overhead per blob. At 50% compression of this overhead, savings are ~3.6 KB per blob. For small blobs (<10 KB data), this is significant. For large blobs (1 MiB+), the encrypted data dominates and compression provides negligible benefit. Protocol/sync messages compress very well (60-80%).

**New dependency:** google/brotli (C++, CMake, MIT license). Python: `brotli` pip package.

### 2. Namespace-Scoped BlobNotify Filtering

**What it does:** When a blob is ingested, the node only sends BlobNotify to peers that have declared interest in that namespace, rather than broadcasting to all TCP peers.

**Expected behavior:**
- Peers declare interest via existing `sync_namespaces` config (already parsed, stored in sync_namespaces_ set)
- During connection setup or via a new InterestDeclaration message, peers exchange their namespace interest lists
- Empty interest list = "interested in everything" (backward compatible default)
- on_blob_ingested() checks peer's interest set before sending BlobNotify
- Subscribed clients (Notification type 21) continue to work exactly as before -- this only affects BlobNotify (type 59) to TCP peers

**Complexity:** LOW. The sync_namespaces concept already exists. The main work is:
1. Exchange interest declarations between peers during/after handshake
2. Store per-peer interest set (already have subscribed_namespaces for clients)
3. Filter in on_blob_ingested() BlobNotify loop

### 3. Multi-Relay SDK Failover

**What it does:** SDK connect() accepts a list of relay endpoints instead of a single (host, port). Tries them in order for initial connection and rotates through them on reconnect.

**Expected behavior:**
- `ChromatinClient.connect(relays=[(host1, port1), (host2, port2)], identity=...)` new signature
- Backward compatible: single (host, port) still works
- Initial connect: try relays in order, fail after all exhausted
- On disconnect + auto-reconnect: start with last-known-good relay, then rotate
- Per-relay connect timeout (e.g., 5s) with total timeout across all relays
- `on_disconnect` callback fires once regardless of how many relays are tried
- `on_reconnect` callback fires when ANY relay reconnects successfully
- Track which relay is currently connected (expose via property for logging)

**Complexity:** MEDIUM. The reconnect machinery exists (Phase 84). Changes needed:
1. New connect() overload accepting relay list
2. Reconnect loop iterates relay list instead of single endpoint
3. State tracking for current relay index

### 4. Relay Subscription Forwarding

**What it does:** Relay tracks which namespaces each client has subscribed to, and only forwards Notification messages (type 21) from the node that match the client's subscriptions.

**Expected behavior:**
- Relay intercepts Subscribe (type 19) and Unsubscribe (type 20) messages from client
- Still forwards them to node (node needs to know for its own subscription tracking)
- Relay maintains per-session `std::set<namespace_hash>` of subscribed namespaces
- On Notification (type 21) from node, relay checks if namespace matches client's subscriptions
- Non-matching notifications are dropped at the relay (not forwarded to client)
- Empty subscription set = forward nothing (client hasn't subscribed to anything yet)

**Complexity:** MEDIUM. The relay currently has zero state about message semantics. This adds:
1. Subscription state tracking per RelaySession
2. Notification payload parsing (read first 32 bytes = namespace_id)
3. Filter check before forwarding node->client messages

### 5. Relay Auto-Reconnect to Node

**What it does:** When the UDS connection from relay to node drops (node restart, crash), relay automatically retries instead of tearing down all client sessions.

**Expected behavior:**
- Current behavior: node UDS loss -> immediate client disconnect (relay_session.cpp:151)
- New behavior: node UDS loss -> relay pauses client session, retries UDS connection with backoff
- Backoff: jittered exponential (1s, 2s, 4s, 8s, cap at 30s) -- same pattern as SDK reconnect
- During reconnect: client messages are rejected with a temporary error or queued (TBD)
- On successful UDS reconnect: TrustedHello handshake, replay client subscriptions to node
- Max reconnect attempts before giving up (e.g., 10 attempts = ~2 minutes)
- If reconnect fails permanently, tear down client session as today

**Complexity:** MEDIUM-HIGH. Significant relay architecture change:
1. RelaySession needs UDS reconnect loop (currently UDS connect is fire-once in start())
2. Client message handling during UDS downtime (queue? reject? pause TCP reads?)
3. Re-establishment of node-side state (subscriptions) after reconnect
4. Lifecycle management -- avoid race between reconnect and client disconnect

### 6. Hot Config Reload for max_peers

**What it does:** Makes max_peers SIGHUP-reloadable like all other config parameters.

**Expected behavior:**
- SIGHUP re-reads config file, updates max_peers
- If new max_peers < current connection count: do NOT disconnect existing peers (disruptive)
- Instead: refuse new connections until count drops below new limit naturally
- Log the change: "config reload: max_peers=N (currently M connected)"
- allowed_client_keys and allowed_peer_keys are already SIGHUP-reloadable (confirmed in reload_config())

**Complexity:** LOW. One line to add to reload_config():
```cpp
config_.max_peers = new_cfg.max_peers;  // (or max_peers_ member)
spdlog::info("config reload: max_peers={}", config_.max_peers);
```
The accept loop already checks `peers_.size() < config_.max_peers` on every new connection.

### 7. Prometheus-Compatible HTTP /metrics Endpoint

**What it does:** Exposes existing metrics via HTTP GET /metrics in Prometheus text exposition format.

**Expected behavior:**
- Separate HTTP port (e.g., 9100 or configurable `metrics_port`)
- GET /metrics returns text/plain with Prometheus text format
- No authentication on metrics endpoint (standard practice -- bind to localhost or internal network)
- Metrics exposed (from existing metrics_ struct + storage queries):
  - `chromatindb_peers_connected` (gauge)
  - `chromatindb_peers_connected_total` (counter)
  - `chromatindb_peers_disconnected_total` (counter)
  - `chromatindb_blobs_total` (gauge -- blob count)
  - `chromatindb_storage_bytes` (gauge)
  - `chromatindb_syncs_total` (counter)
  - `chromatindb_ingests_total` (counter)
  - `chromatindb_rejections_total` (counter)
  - `chromatindb_rate_limited_total` (counter)
  - `chromatindb_cursor_hits_total` (counter)
  - `chromatindb_cursor_misses_total` (counter)
  - `chromatindb_full_resyncs_total` (counter)
  - `chromatindb_quota_rejections_total` (counter)
  - `chromatindb_sync_rejections_total` (counter)
  - `chromatindb_uptime_seconds` (gauge)
  - `chromatindb_namespaces_total` (gauge)
- Labels: `{node="pubkey_hash_hex"}`

**Implementation approach:** The Prometheus text format is trivial -- plain text with `metric_name{labels} value` per line. No need for prometheus-cpp library (which brings civetweb, curl, zlib dependencies). Use standalone Asio to serve a minimal HTTP listener on the metrics port. ~150 lines of code. The data already exists in dump_metrics()/log_metrics_line() -- just format as Prometheus text instead of spdlog.

**Complexity:** MEDIUM. Not because the format is hard, but because adding an HTTP listener to a standalone Asio application requires careful lifecycle management (separate acceptor, separate port, graceful shutdown).

## MVP Definition

### Launch With (v2.1.0)

All seven features are targeted for this milestone. Ordered by implementation dependency and value:

- [x] Hot config reload for max_peers -- trivial, unblocks nothing, just do it (P1, LOW effort)
- [x] Prometheus /metrics endpoint -- independent, high operator value (P1, MEDIUM effort)
- [x] Namespace-scoped BlobNotify filtering -- low complexity, reduces peer traffic (P1, LOW effort)
- [x] Relay subscription forwarding -- completes the filtering story end-to-end (P1, MEDIUM effort)
- [x] Relay auto-reconnect to node -- resilience foundation for relay layer (P1, MEDIUM-HIGH effort)
- [x] Multi-relay SDK failover -- resilience foundation for SDK layer (P2, MEDIUM effort)
- [x] Brotli wire compression -- new dependency, protocol change, most complex (P2, MEDIUM effort)

### Add After Validation (post v2.1.0)

- [ ] Compression statistics in /metrics -- track bytes saved, compression ratios per message type
- [ ] Relay /metrics endpoint -- expose relay-specific metrics (sessions, forwarded/dropped messages)
- [ ] Dynamic compression level based on payload size -- lower quality for large blobs, higher for small

### Future Consideration (v2.2.0+)

- [ ] Selective namespace sync (only replicate declared namespaces) -- builds on BlobNotify filtering
- [ ] Relay connection pooling to node (pre-opened UDS connections for faster client onboarding)
- [ ] SDK health check API exposing current relay, connection state, and metrics

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| Hot config reload (max_peers) | MEDIUM | LOW | P1 |
| Prometheus /metrics endpoint | HIGH | MEDIUM | P1 |
| Namespace-scoped BlobNotify filtering | MEDIUM | LOW | P1 |
| Relay subscription forwarding | MEDIUM | MEDIUM | P1 |
| Relay auto-reconnect to node | HIGH | MEDIUM-HIGH | P1 |
| Multi-relay SDK failover | HIGH | MEDIUM | P2 |
| Brotli wire compression | MEDIUM | MEDIUM | P2 |

**Priority key:**
- P1: Must have for launch -- observability, filtering, resilience
- P2: Should have -- compression and SDK failover are valuable but not blocking

## Competitor Feature Analysis

| Feature | etcd | CockroachDB | Consul | chromatindb v2.1.0 Approach |
|---------|------|-------------|--------|---------------------------|
| Wire compression | gRPC built-in (gzip) | gRPC built-in | None (HTTP) | Brotli at AEAD frame level |
| Metrics | Prometheus built-in | Prometheus built-in | Prometheus + StatsD | Prometheus via minimal HTTP endpoint (no library) |
| Topic/namespace filtering | Watch with key prefix filter | CDC with table filter | Blocking queries with index | BlobNotify namespace filter + relay subscription forwarding |
| Multi-endpoint failover | Client library supports endpoint list | Built into connection string | Agent-based (local agent always available) | SDK relay list with ordered failover |
| Hot config reload | Most config via etcdctl | SET CLUSTER SETTING | HCL reload via SIGHUP | SIGHUP reload (adding max_peers) |
| Proxy reconnect | gRPC handles transparently | pgwire handles | Agent is always local | Relay UDS auto-reconnect with backoff |

## Sources

- [google/brotli GitHub](https://github.com/google/brotli) -- MIT license, CMake build system, C API
- [RFC 7932 -- Brotli Compressed Data Format](https://datatracker.ietf.org/doc/rfc7932/) -- specification
- [Prometheus exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/) -- text format spec
- [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) -- evaluated, rejected (too many deps: civetweb, curl, zlib)
- [ZSTD vs Brotli vs GZip Comparison](https://speedvitals.com/blog/zstd-vs-brotli-vs-gzip/) -- compression benchmarks
- [Comparing Compression Algorithms for Moving Big Data](https://manishrjain.com/compression-algo-moving-data) -- binary data compression analysis
- [Redis Pub/Sub docs](https://redis.io/docs/latest/develop/pubsub/) -- namespace/channel filtering patterns
- [Redis client geographic failover](https://redis.io/docs/latest/develop/clients/failover/) -- multi-endpoint failover pattern with circuit breaker
- Codebase analysis: db/peer/peer_manager.cpp (reload_config, on_blob_ingested, dump_metrics), relay/core/relay_session.cpp, sdk/python/chromatindb/client.py, db/PROTOCOL.md

---
*Feature research for: chromatindb v2.1.0 Compression, Filtering & Observability*
*Researched: 2026-04-04*
