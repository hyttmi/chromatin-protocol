# Pitfalls Research

**Domain:** Adding compression, filtering, observability, and resilience to an existing PQ-secure database system (chromatindb v2.1.0)
**Researched:** 2026-04-04
**Confidence:** HIGH (codebase-verified, known architecture constraints)

## Critical Pitfalls

### Pitfall 1: Compression oracle side channel (CRIME/BREACH class)

**What goes wrong:**
Brotli compress-then-encrypt leaks information about plaintext through ciphertext size. If an attacker can influence part of the blob data being compressed alongside secret data, they can iteratively guess the secret content by observing compressed ciphertext sizes. This is the same class of attack as CRIME and BREACH against TLS.

**Why it happens:**
Compression ratios depend on data redundancy. When attacker-controlled data partially matches secret data, the compressed output shrinks. After AEAD encryption, the ciphertext length directly reflects the compressed length, creating a measurable side channel.

**How to avoid:**
This is NOT a real risk for chromatindb because blob data is opaque (the node does not mix attacker-controlled data with secrets inside a single blob -- each blob is one self-contained unit signed by its owner). The attacker cannot inject chosen plaintext alongside a target secret within the same compression context. Compress-then-encrypt at the blob level is safe here.

However, document this explicitly in PROTOCOL.md so future developers understand why it is safe (blob-level compression context, no mixed attacker/secret data within a frame) and do not accidentally introduce message-level compression that mixes metadata with secrets.

**Warning signs:**
- Any proposal to compress multiple messages together in a single compression context
- Any proposal to add padding that varies based on content
- Compressing transport-level metadata alongside blob payloads in the same stream

**Phase to address:**
Wire compression phase. Add a PROTOCOL.md note explaining why compress-then-encrypt is safe for blob-level compression and explicitly stating that compression contexts must never span multiple blobs.

---

### Pitfall 2: Brotli decompression bomb -- unbounded memory allocation on receive

**What goes wrong:**
A malicious peer sends a tiny compressed payload (e.g., 6 KB) that decompresses to gigabytes of data, causing OOM on the receiver. Brotli has no built-in output size limit. CVE-2025-6176 demonstrates this exact attack vector.

**Why it happens:**
Brotli's streaming decoder will happily produce unbounded output. The current MAX_FRAME_SIZE check (110 MiB) validates the encrypted/compressed frame, not the decompressed output. If compression is applied before encryption (compress -> encrypt on send; decrypt -> decompress on receive), the frame size check protects only the compressed size.

**How to avoid:**
1. **Output size cap on decompression:** After AEAD decrypt, before Brotli decompress, the decompressed output must be bounded. Use BrotliDecoderDecompressStream with an output buffer capped at MAX_BLOB_DATA_SIZE + protocol overhead (~100 MiB + 1 KiB). Abort and disconnect if output exceeds cap.
2. **Compress at message level, not frame level:** Compress only the blob payload within the TransportCodec::encode() path, not the entire AEAD frame. This means the AEAD frame still has a meaningful size (compressed payload + FlatBuffers overhead), and the decompression output is bounded by MAX_BLOB_DATA_SIZE.
3. **Pre-flight size validation:** Include the uncompressed size as a field in the transport message (e.g., a uint32 before the compressed payload). Validate it before attempting decompression. Disconnect on mismatch.

**Warning signs:**
- Decompression without output size limit
- Decompression happening before any size validation
- Tests only using well-formed compressed data (no adversarial fuzzing)

**Phase to address:**
Wire compression phase. This must be the FIRST thing designed -- the decompression safety boundary.

---

### Pitfall 3: Compression breaks AEAD nonce counter semantics

**What goes wrong:**
If compression is applied at the wrong layer (e.g., inside the drain_send_queue coroutine after encoding), it changes the data being encrypted but the nonce counter has already been allocated. If compression fails or produces larger output (possible for incompressible data with Brotli quality 0), the frame may exceed MAX_FRAME_SIZE, causing a late-stage failure after the nonce has been consumed. The sender's nonce counter advances but nothing was sent, permanently desyncing the AEAD stream.

**Why it happens:**
The send path is: `send_message -> TransportCodec::encode -> enqueue_send -> drain_send_queue -> send_encrypted -> write_frame(plaintext, key, counter++)`. The counter increment happens inside `write_frame`/`send_encrypted`. If compression is inserted after encode but before encrypt, and the compression step can fail or be skipped on a per-message basis, you get conditional nonce advancement.

**How to avoid:**
Compression must happen INSIDE TransportCodec::encode (before the data enters the encrypted send path) or transparently inside the blob payload itself (application-layer compression). The critical invariant: by the time data reaches `enqueue_send`, it must be ready to encrypt as-is. No conditional steps between encode and encrypt.

For incompressible data: Brotli with quality=0 can produce output larger than input (overhead bytes). The encoder must fall back to uncompressed if compressed size >= original size. Use a flag byte (0x00 = uncompressed, 0x01 = brotli) prefix so the receiver knows which path to take.

**Warning signs:**
- Compression logic in `connection.cpp` (wrong layer)
- Conditional compression (some messages compressed, others not) without a framing flag
- Tests passing only with compressible data

**Phase to address:**
Wire compression phase. Design the compression integration point before writing code.

---

### Pitfall 4: Prometheus /metrics HTTP endpoint creates a second attack surface

**What goes wrong:**
The /metrics endpoint is an HTTP server listening on a separate port. It runs outside the PQ-authenticated transport, creating an unauthenticated attack surface. Information disclosed (peer count, pubkey hashes in metric labels, storage sizes, sync stats) is reconnaissance gold for an attacker targeting a specific node.

**Why it happens:**
Prometheus scraping is inherently pull-based over HTTP. There is no standard way to serve Prometheus metrics over a PQ-authenticated channel. The typical pattern (expose /metrics on a port) works fine in a trusted Kubernetes network but is dangerous on internet-facing nodes.

**How to avoid:**
1. **Bind to localhost only by default.** The metrics HTTP listener should default to `127.0.0.1:9100` (or similar), not `0.0.0.0`. Prometheus scraper runs on the same host or through an SSH tunnel.
2. **No sensitive labels.** Metric names and labels must NOT include pubkey hashes, peer IP addresses, namespace hashes, or any cryptographic material. Use opaque counters only: `chromatindb_ingests_total`, `chromatindb_connections_current`, `chromatindb_storage_bytes`.
3. **Config-controlled.** Metrics endpoint should be disabled by default (`metrics_bind_address = ""` in config). Operators opt in.
4. **No prometheus-cpp dependency if avoidable.** The Prometheus text exposition format is trivial (plaintext `metric_name value\n`). A 50-line HTTP handler on a raw Asio TCP acceptor avoids pulling in prometheus-cpp and its transitive deps (civetweb/cpp-httplib). This project values minimal dependencies.

**Warning signs:**
- Binding to 0.0.0.0 by default
- Including pubkey or namespace hashes in metric labels
- Using prometheus-cpp library (unnecessary dependency for text format)
- No way to disable the endpoint

**Phase to address:**
Prometheus metrics phase. Design the endpoint binding and label policy before implementation.

---

### Pitfall 5: Relay subscription forwarding creates unbounded state accumulation

**What goes wrong:**
When the relay starts tracking client subscriptions to forward namespace-scoped BlobNotify/Notification messages, it accumulates per-client subscription state. A malicious or buggy client could subscribe to thousands of namespaces, consuming relay memory. Multiple clients with large subscription sets compound the problem. The relay currently has zero per-client state beyond the forwarding session.

**Why it happens:**
The current relay is a stateless forwarder (one TCP conn -> one UDS conn, bidirectional pass-through with blocklist filter). Adding subscription tracking fundamentally changes the relay from stateless to stateful. Developers underestimate how much state a "simple subscription list" can accumulate across many clients.

**How to avoid:**
1. **Per-client subscription cap.** Hard limit (e.g., 256 namespaces per client). Reject Subscribe messages that would exceed the cap.
2. **Connection-scoped cleanup.** Subscription state MUST be destroyed when the RelaySession is torn down. This already works for the node's PeerInfo.subscribed_namespaces (connection-scoped), but the relay must implement the same pattern.
3. **Consider: keep the relay stateless.** An alternative design is to NOT add subscription tracking to the relay. Instead, let the node handle subscription filtering as it already does (PeerInfo.subscribed_namespaces). The relay just forwards Subscribe/Unsubscribe to the node and Notification responses back to the client -- no new relay state needed. The node already does namespace-scoped Notification dispatch. This avoids the pitfall entirely.

**Warning signs:**
- Relay memory growing proportionally to number of clients * subscriptions
- No subscription limit enforced
- Subscription state outliving client connections
- Duplicating filtering logic that already exists in the node

**Phase to address:**
Relay subscription forwarding phase. Decide stateless vs stateful relay FIRST.

---

### Pitfall 6: Hot-reload of max_peers triggers thundering-herd reconnect

**What goes wrong:**
When max_peers is reduced via SIGHUP, the node must disconnect excess peers. If it disconnects N peers simultaneously, all N attempt to reconnect with jittered exponential backoff. But their initial reconnect attempts cluster within the first 1-2 seconds (jitter range is 0-1s on the first attempt). If the node's new max_peers limit is close to the reconnect burst, connections oscillate: accept, fill, reject, backoff, retry.

**Why it happens:**
The existing reconnect logic (Phase 43, v0.9.0) uses jittered exponential backoff starting at 1s. When many peers are disconnected simultaneously, their backoff timers are correlated because they all start from the same event. The jitter range on the first retry is too narrow to spread them out.

**How to avoid:**
1. **Graceful drain, not mass disconnect.** When max_peers decreases, stop accepting NEW connections and let excess peers naturally disconnect (e.g., via the next keepalive cycle or natural session end). Only force-disconnect if the excess is large (e.g., >50% over new limit).
2. **Staggered disconnect.** If force-disconnecting, spread disconnects over a few seconds (e.g., 1 per 500ms) rather than all at once.
3. **Disconnect non-bootstrap first.** Prioritize keeping bootstrap peers connected; disconnect discovered/PEX peers first.

**Warning signs:**
- All excess peers disconnected in a single `reload_config()` call
- Spike in `peers_connected_total` immediately after `peers_disconnected_total` spike
- Oscillating peer count in metrics after SIGHUP

**Phase to address:**
Hot config reload phase. The disconnect strategy must be designed, not just "kick N peers."

---

### Pitfall 7: Relay UDS auto-reconnect retries during node restart cause socket file contention

**What goes wrong:**
When the relay detects the UDS connection to the node is lost, it starts reconnecting. If the node is restarting, the UDS socket file may be stale (old file, no listener) or temporarily absent. The relay's reconnect loop hammers `connect()` against a non-existent or stale socket path, logging errors rapidly and potentially interfering with the node's bind attempt if the timing overlaps.

**Why it happens:**
UDS reconnect is different from TCP reconnect. A stale socket file returns ECONNREFUSED immediately (no SYN timeout), so the reconnect loop runs at CPU speed if backoff is not applied. Also, `connect()` on a UDS requires creating a NEW socket each time -- you cannot reuse the old socket object after a failed connect (POSIX requirement).

**How to avoid:**
1. **Mandatory exponential backoff.** Even though UDS connect failures are fast, apply the same jittered exponential backoff as TCP reconnect (1s-60s). Do NOT treat fast-fail as "try again immediately."
2. **Socket file existence check before connect.** `stat()` the UDS path before `connect()`. If the file does not exist, skip the connect attempt for this backoff cycle. Avoids unnecessary syscall overhead.
3. **New socket per attempt.** Create a fresh `asio::local::stream_protocol::socket` for each reconnect attempt. Close the old one first. Do not attempt to reuse.
4. **Client session behavior during relay-node disconnect.** While the relay cannot reach the node, existing client sessions must receive an error response (not hang). Forward the UDS disconnect to the client immediately (the relay already does this via `handle_node_close` -> `teardown`). New clients should be refused until the UDS connection is restored.

**Warning signs:**
- Reconnect loop without backoff logging thousands of errors per second
- Reusing socket object after failed connect
- Client requests hanging instead of failing fast during relay-node disconnect
- Log spam from reconnect attempts filling disk

**Phase to address:**
Relay auto-reconnect phase. UDS-specific reconnect semantics must be tested explicitly (node stop, node restart, stale socket file scenarios).

---

### Pitfall 8: Multi-relay failover in SDK creates retry storms

**What goes wrong:**
When the SDK connects to a list of relays with failover, and the primary relay goes down, ALL connected clients simultaneously try the secondary relay. This thundering herd can overwhelm the secondary relay (which now handles 2x the normal load) or the node behind it (which suddenly gets 2x the UDS connections).

**Why it happens:**
Naive failover: "if primary fails, try next in list." With N clients, all N switch at the same time. The secondary relay and its backing node were sized for N/2 clients.

**How to avoid:**
1. **Randomized relay ordering.** On initial connect, shuffle the relay list per-client. This distributes clients across relays even when all are healthy, so failover only shifts a fraction of clients.
2. **Jittered failover delay.** When the current relay disconnects, wait `random(0, 2s)` before attempting the next relay. This spreads the failover window.
3. **Circuit breaker, not retry loop.** After failing to connect to ALL relays in the list, enter a backoff state (exponential, 1s-60s). Do NOT cycle through the list again immediately.
4. **Sticky relay preference.** Once connected to a relay, prefer it on reconnect. Do not round-robin on every disconnect -- try the last-known-good relay first with a short timeout, then fall through to others.

**Warning signs:**
- All clients failing over to the same relay simultaneously
- SDK cycling through relay list in a tight loop when all relays are down
- No jitter in the failover logic
- Secondary relay OOM or connection limit hit during failover

**Phase to address:**
Multi-relay SDK failover phase. The failover algorithm must include jitter and circuit breaking from day one.

---

### Pitfall 9: Namespace-scoped BlobNotify filtering at sender breaks sync correctness

**What goes wrong:**
If BlobNotify is filtered at the sender (only send BlobNotify to peers that have expressed interest in a namespace), peers that have not subscribed to a namespace will never learn about new blobs via push. This is correct for clients (they subscribe to specific namespaces), but dangerous for PEER-to-PEER notifications. Peers need BlobNotify for ALL namespaces (or at least all that match their sync_namespaces config) to maintain push-based replication.

**Why it happens:**
Conflation of client subscription model with peer replication model. Clients subscribe to specific namespaces for UI/application purposes. Peers need to know about ALL new blobs for replication. The current code correctly sends BlobNotify to ALL TCP peers (no filtering) and Notification only to subscribed clients.

**How to avoid:**
1. **Maintain the dual-path invariant.** BlobNotify (type 59, peer-to-peer) must remain unfiltered for peers OR be filtered only by the peer's sync_namespaces config (which already exists). Notification (type 21, to clients) is filtered by subscription.
2. **If adding sender-side filtering for BlobNotify, filter by peer's sync_namespaces, NOT by subscription state.** sync_namespaces is a node config setting for which namespaces to replicate. Subscriptions are a client feature.
3. **Test with mixed configs.** Test a node with sync_namespaces=["A","B"] connected to a node with sync_namespaces=[] (all). Verify node 2 still receives BlobNotify for namespaces C, D, etc. from other peers.

**Warning signs:**
- BlobNotify sending loop checking `subscribed_namespaces` instead of `sync_namespaces`
- Peers missing blobs that they should replicate
- Sync safety-net catching blobs that should have arrived via push

**Phase to address:**
Namespace-scoped notification phase. Define the filtering rules precisely before implementation. The two notification types (BlobNotify for peers, Notification for clients) have different filtering criteria.

---

### Pitfall 10: Adding Brotli as FetchContent dependency conflicts with existing CMake structure

**What goes wrong:**
Brotli's CMake build installs targets with generic names (`brotlidec`, `brotlienc`, `brotlicommon`). If FetchContent pulls Brotli and it installs into the same namespace as other deps, target name collisions or header path pollution can occur. Brotli's CMake also defines `BROTLI_BUILD_PORTABLE` and other cache variables that may interact with the parent build.

**Why it happens:**
Not all CMake projects are well-behaved as FetchContent dependencies. Brotli's CMake predates modern FetchContent conventions and installs headers and targets globally.

**How to avoid:**
1. **Test FetchContent integration early.** Add the FetchContent_Declare/MakeAvailable for Brotli to db/CMakeLists.txt and verify it builds cleanly alongside liboqs, libsodium, libmdbx, FlatBuffers, xxHash, and Asio.
2. **Set BROTLI_DISABLE_TESTS=ON** to avoid building Brotli's test suite.
3. **Use Brotli's C API directly** (`brotli/encode.h`, `brotli/decode.h`), not the C++ wrapper. The C API is stable and well-documented. Link against `brotlidec` and `brotlienc`.
4. **Prefer Brotli's `BrotliEncoderCompress()`/`BrotliDecoderDecompress()` for single-shot** (blob-level compression where the entire input is available). Only use the streaming API for very large blobs (>10 MiB) where memory matters.

**Warning signs:**
- CMake configuration errors after adding Brotli FetchContent
- Header include path conflicts
- Build taking significantly longer (Brotli tests building)

**Phase to address:**
Wire compression phase. Verify FetchContent integration as the first implementation step.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Compressing all messages (not just blob payloads) | Simpler implementation -- one compression point | Compresses 77-byte BlobNotify, 1-byte Ping, etc. with negative ratio. Wastes CPU. | Never -- only compress messages above a size threshold (e.g., > 256 bytes) |
| Using prometheus-cpp library for metrics | Faster initial implementation | New dep with transitive deps (civetweb or cpp-httplib). Violates minimal-dep philosophy. | Never for this project -- text exposition format is trivial to implement |
| Relay subscription tracking duplicating node logic | Relay can filter locally without round-trip | Two implementations of namespace filtering that must stay in sync. Bug surface doubles. | Only if the relay becomes a standalone product; otherwise keep it stateless |
| Hard-coding Brotli quality level | Simpler config | Different blob sizes benefit from different quality levels. Small blobs get negative compression at high quality. | Acceptable for MVP with quality=4 (balanced). Make configurable later. |
| Metrics endpoint without access control | Faster to implement | Anyone who can reach the port can scrape node metadata | Only on localhost-bound endpoints with firewall |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Brotli in AEAD pipeline | Inserting compression between encode and encrypt (breaks nonce invariant) | Compress inside TransportCodec::encode, before the message enters the encrypted send path |
| Prometheus metrics in single-threaded io_context | Blocking HTTP accept/response in the io_context thread stalls all networking | Run metrics HTTP handler on a SEPARATE thread or use asio's strand to avoid blocking. Or: write metrics to a file and serve with a trivial external HTTP server. |
| UDS reconnect in relay | Reusing socket object after failed connect (POSIX violation) | Create a new asio::local::stream_protocol::socket for every reconnect attempt |
| Hot-reload of max_peers | Reading config_.max_peers directly (already done, but reload changes it mid-flight) | Ensure should_accept_connection() reads the value atomically. The current pattern of reading from const Config& reference is safe because reload updates the config on the io_context thread and should_accept_connection also runs on the io_context thread. But if max_peers reload is added, the Config struct must be updated (not replaced) since it is stored by const reference. |
| SDK multi-relay connect() | Connecting to relays sequentially in a tight loop | Use asyncio.wait_for with per-relay timeout, total timeout across all attempts, and jitter between attempts |
| Namespace filtering for BlobNotify vs Notification | Using the same filter criteria for both (subscription set) | BlobNotify to peers: filter by sync_namespaces (node config). Notification to clients: filter by subscribed_namespaces (per-connection state). Different filter criteria. |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Compressing small messages (< 256 bytes) | CPU usage increases, throughput decreases, compressed size > original | Skip compression for payloads below a threshold. BlobNotify is 77 bytes -- never compress it. | Immediately visible with BlobNotify workload |
| Brotli quality > 6 for real-time sync | Compression latency dominates blob transfer time | Use quality 1-4 for wire compression. Higher quality is for archival, not real-time transfer. | At any blob rate > 10/sec with quality 9+ |
| Prometheus scrape interval too low | Metrics handler runs on io_context thread, frequent scrapes block networking | Default scrape interval >= 15s. Rate-limit HTTP requests. | With 1s scrape interval on a busy node |
| Relay notification fan-out with many clients | O(clients) message sends per blob ingestion, each requiring AEAD encrypt | Batching or rate-limiting notification dispatch. Consider a per-client notification queue with coalescing. | > 100 connected clients with active subscriptions |
| Metrics string formatting on every scrape | std::to_string / snprintf for 30+ counters on every /metrics GET | Cache the formatted metrics string, invalidate only when counters change (or on a timer). Counter reads are cheap; string formatting is not. | High scrape frequency (< 5s) |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Metrics endpoint bound to 0.0.0.0 | Exposes peer count, storage size, sync stats to the internet. Reconnaissance data for targeted attacks. | Default to 127.0.0.1. Require explicit opt-in for external binding. |
| Pubkey hashes in metric labels | Attacker can enumerate all connected clients and peers by scraping /metrics | Use opaque counters only. No cryptographic material in labels. |
| Compression ratio as side channel between peers | Peer can infer whether a blob contains duplicate data by observing compressed BlobFetchResponse sizes vs raw sizes | Not exploitable in chromatindb (blobs are signed by their owner, content is opaque to transport). Document this non-risk. |
| UDS socket file permissions after relay reconnect | If relay creates UDS socket with wrong permissions after reconnect, unauthorized local users could connect | Verify socket ownership and permissions (0660 or 0600) on every reconnect, not just initial creation |
| Multi-relay list exposes topology | SDK connect() method takes a list of relay addresses. If leaked, reveals network topology. | Already mitigated: relay addresses are client config, not embedded in protocol. Document as operational concern. |

## "Looks Done But Isn't" Checklist

- [ ] **Wire compression:** Decompression bomb protection -- verify output size cap is enforced before decompression completes, not just after
- [ ] **Wire compression:** Incompressible data fallback -- verify compressed-larger-than-original case sends uncompressed with flag byte
- [ ] **Wire compression:** Both directions -- verify compression works for sync BlobTransfer (peer-to-peer) AND BlobFetchResponse (push path) AND Data (client write via relay)
- [ ] **Wire compression:** Relay transparency -- compressed messages pass through relay unchanged (relay does not decompress/recompress)
- [ ] **Prometheus metrics:** Endpoint disabled when config is empty -- verify no HTTP listener starts
- [ ] **Prometheus metrics:** Thread safety -- verify counter reads do not race with io_context thread counter writes (single-threaded design makes this safe, but verify metrics HTTP handler runs on io_context or reads are atomic)
- [ ] **Namespace filtering:** BlobNotify to peers still works unfiltered for peers with no sync_namespaces restriction
- [ ] **Namespace filtering:** Clients subscribing to a namespace they cannot write to still receive notifications (read access is universal)
- [ ] **Relay auto-reconnect:** Client sessions receive errors during relay-node disconnect (not silent hang)
- [ ] **Relay auto-reconnect:** New client connections are refused while relay-node UDS is down
- [ ] **Multi-relay failover:** SDK handles all relays being down gracefully (raises ConnectionError after backoff exhaustion, does not loop forever)
- [ ] **Multi-relay failover:** Auto-reconnect reuses the failover list, does not only try the original relay
- [ ] **Hot config reload:** max_peers decrease does not disconnect bootstrap peers
- [ ] **Hot config reload:** allowed_peer_keys reload disconnects revoked peers (already works for existing fields, verify for new fields)
- [ ] **Hot config reload:** Config validation failure preserves the ENTIRE old config (partial reload where some fields update and others don't is worse than no reload)

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Compression oracle (if architecture changes later to mix contexts) | MEDIUM | Revert to per-blob compression contexts. Add random padding if mixed contexts are required. |
| Decompression bomb exploit | LOW | Immediate: disconnect the malicious peer. Fix: add output size cap to decompression. No data loss. |
| AEAD nonce desync from compression layer | HIGH | Both peers must disconnect and reconnect. No recovery mid-session -- AEAD stream is permanently broken. Prevention is the only real fix. |
| Metrics endpoint data leak | MEDIUM | Bind to localhost. Rotate any exposed information (peer IPs, etc.). Review what was scraped. |
| Relay subscription state leak | LOW | Restart relay. Connection-scoped cleanup ensures no persistent leak. Fix subscription cap. |
| max_peers thundering herd | LOW | Wait for exponential backoff to naturally spread peers out (60s). Or: temporarily increase max_peers and reduce gradually. |
| UDS reconnect loop without backoff | LOW | Restart relay with fixed backoff. No data loss -- clients reconnect through relay. |
| Retry storm from multi-relay failover | MEDIUM | SDK clients must be restarted with fixed failover logic. Secondary relay may need restart if connection table is exhausted. |
| Namespace filter breaks peer replication | HIGH | Safety-net sync catches missing blobs within 10 minutes. But if the filter is in the wrong place, blobs are silently not pushed and the safety-net timer becomes the only sync mechanism -- effectively regressing to timer-based sync (pre-v2.0.0 behavior). |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Compression oracle side channel | Wire compression | PROTOCOL.md documents compression context scope. Code review confirms per-blob compression only. |
| Decompression bomb | Wire compression | Fuzz test with adversarial compressed payloads. Unit test: 6KB input that would decompress to >200MB is rejected. |
| AEAD nonce desync from compression | Wire compression | Integration test: send mix of compressible and incompressible blobs. Verify all decrypt correctly on receiver. |
| Prometheus attack surface | Prometheus metrics | Config test: default config has no metrics listener. Test: metrics endpoint unreachable from non-localhost by default. |
| Relay subscription state accumulation | Relay subscription forwarding | Load test: 100 clients each subscribing to 256 namespaces. Verify relay memory stable. Disconnect all clients. Verify cleanup. |
| max_peers thundering herd | Hot config reload | Integration test: 10 peers connected, SIGHUP reduces max_peers to 5. Verify staggered disconnect. Verify bootstrap peers retained. |
| UDS reconnect without backoff | Relay auto-reconnect | Integration test: stop node, verify relay reconnect attempts are spaced by exponential backoff. Verify log rate is bounded. |
| Multi-relay retry storm | Multi-relay SDK failover | Integration test: start SDK with 3 relay addresses, all down. Verify SDK enters backoff state after one full cycle. Verify jitter in retry timing. |
| BlobNotify filtering breaks peer push | Namespace-scoped filtering | Integration test: node A writes to namespace X. Node B has sync_namespaces=[] (all). Verify node B receives BlobNotify for namespace X. |
| Brotli FetchContent conflict | Wire compression | CMake build succeeds with all existing deps + Brotli. Run full test suite. |

## Sources

- CRIME/BREACH compression oracle: [BREACH Wikipedia](https://en.wikipedia.org/wiki/BREACH), [CRIME Wikipedia](https://en.wikipedia.org/wiki/CRIME), [Encryption vs Compression analysis](https://kmcd.dev/posts/encryption-vs-compression/)
- Brotli decompression bomb: CVE-2025-6176, [Brotli GitHub issue #389 (memory usage)](https://github.com/google/brotli/issues/389)
- Brotli streaming pitfalls: [Brotli C++ compression guide](https://ssojet.com/compression/compress-files-with-brotli-in-cpp), [Brotli small writes issue](https://github.com/dotnet/runtime/issues/36245)
- Prometheus security: [Prometheus security model](https://prometheus.io/docs/operating/security/), [JFrog Prometheus exposure analysis](https://jfrog.com/blog/dont-let-prometheus-steal-your-fire/), [Aquasec 300K exposed servers](https://www.aquasec.com/blog/300000-prometheus-servers-and-exporters-exposed-to-dos-attacks/)
- Prometheus label cardinality: [CNCF Prometheus labels best practices](https://www.cncf.io/blog/2025/07/22/prometheus-labels-understanding-and-best-practices/)
- UDS reconnect semantics: [connect(2) man page](https://man7.org/linux/man-pages/man2/connect.2.html), [unix(7) man page](https://man7.org/linux/man-pages/man7/unix.7.html)
- Retry storm anti-pattern: [Azure retry storm](https://learn.microsoft.com/en-us/azure/architecture/antipatterns/retry-storm/), [Agoda retry storm fix](https://medium.com/agoda-engineering/how-agoda-solved-retry-storms-to-boost-system-reliability-9bf0d1dfbeee)
- SIGHUP hot-reload: [SIGHUP signal for configuration reloads](https://blog.devtrovert.com/p/sighup-signal-for-configuration-reloads), [Runtime configuration reloading (Rust, but patterns apply)](https://vorner.github.io/2019/08/11/runtime-configuration-reloading.html)
- Relay state accumulation: [Tailscale relay memory leak #17801](https://github.com/tailscale/tailscale/issues/17801), [Envoy proxy memory leak #20187](https://github.com/envoyproxy/envoy/issues/20187)
- Codebase: `db/net/connection.h` (send queue, AEAD nonce counters), `db/net/framing.cpp` (frame encryption), `db/peer/peer_manager.cpp` (BlobNotify fan-out, SIGHUP handler, on_blob_ingested), `relay/core/relay_session.h` (stateless forwarder), `relay/core/message_filter.h` (blocklist), `db/config/config.h` (current config structure)

---
*Pitfalls research for: chromatindb v2.1.0 -- compression, filtering, observability, resilience*
*Researched: 2026-04-04*
