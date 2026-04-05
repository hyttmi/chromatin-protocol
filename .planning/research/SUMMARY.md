# Project Research Summary

**Project:** chromatindb v2.1.0 — Compression, Filtering & Observability
**Domain:** Decentralized PQ-secure database node — wire compression, namespace filtering, Prometheus metrics, relay resilience, SDK failover
**Researched:** 2026-04-04
**Confidence:** HIGH

## Executive Summary

chromatindb v2.1.0 adds production-readiness features to an already-shipped, battle-tested system. The architecture is fully established (single io_context thread, per-connection send queues, PQ AEAD transport, relay forwarder), so this milestone is about layering new features onto known boundaries rather than designing from scratch. Research consistently finds that most v2.1.0 features require no new dependencies: namespace filtering, relay reconnect, SDK failover, hot-reload expansion, and Prometheus metrics all extend existing code paths. The single exception is Brotli wire compression, which adds google/brotli v1.2.0 (C++) and the `brotli` pip package (Python SDK).

The recommended approach is to build features in dependency order, starting with small independent changes (max_peers reload, namespace BlobNotify filtering) before the architecturally significant ones (relay auto-reconnect state machine, Brotli compression pipeline). Brotli requires careful integration at the PeerManager dispatch layer — compression must happen inside TransportCodec::encode, before AEAD, with a flag byte prefix for self-describing frames. The Prometheus /metrics endpoint is straightforward but must default to localhost-only binding to avoid exposing node metadata publicly.

The dominant risks are: (1) decompression bombs — Brotli has no built-in output limit and CVE-2025-6176 demonstrates this is real; (2) AEAD nonce desync — if compression is inserted at the wrong layer in the send pipeline, conditionally skipped messages would desync the stream permanently; (3) thundering-herd failover — naive multi-relay or max_peers reload causes simultaneous reconnect storms. All three are preventable with correct design decisions made upfront. The relay auto-reconnect state machine (ACTIVE / RECONNECTING / DEAD) is the most architecturally involved change of the milestone.

## Key Findings

### Recommended Stack

The stack is stable. Only one new C++ dependency (google/brotli v1.2.0) and one new Python dependency (`brotli~=1.2.0`) are added. All other features are implemented on existing infrastructure: Asio handles the Prometheus HTTP acceptor; the existing SIGHUP handler absorbs max_peers reload; the existing asyncio retry machinery absorbs multi-relay failover. Prometheus library alternatives (prometheus-cpp, prometheus-cpp-lite) were evaluated and rejected — both bring their own threading HTTP servers that conflict with chromatindb's single-thread Asio model.

**Core technologies (new in v2.1.0):**
- google/brotli v1.2.0: wire compression before AEAD encrypt — CMake-native, FetchContent-compatible, MIT license, ~91 MB/s at quality 1, 15-20% better ratio than zstd on structured/text content
- brotli~=1.2.0 (PyPI): SDK-side wire compression matching C++ implementation — official Google Python bindings, 3.3M weekly downloads

**Existing stack serving new features (no changes needed):**
- Standalone Asio 1.38.0: Prometheus HTTP acceptor (same io_context thread, no new threads)
- nlohmann/json 3.11.3: new config fields (metrics_bind_address, max_peers)
- FlatBuffers 25.2.10: new SyncNamespaceAnnounce message type (= 62)
- Python asyncio stdlib: multi-relay failover rotation in _reconnect.py

### Expected Features

**Must have (table stakes):**
- Prometheus /metrics endpoint — operators expect a scrape endpoint on any production server daemon; data already exists in NodeMetrics struct, just needs HTTP exposition
- Hot config reload for max_peers — all other config is SIGHUP-reloadable; max_peers is the remaining gap; operators expect full config reloadability
- Relay auto-reconnect to node — current behavior (node restart = all SDK clients disconnect) is unacceptable for production; relay must survive node restarts transparently

**Should have (differentiators):**
- Brotli wire compression — significant savings on structured protocol overhead (PQ pubkeys 2592B, signatures ~4627B, FlatBuffer framing); compress-then-encrypt requires careful PeerManager dispatch-layer integration
- Namespace-scoped BlobNotify filtering — prevents O(peers x namespaces) notification fan-out; must filter by sync_namespaces (node replication config), NOT subscribed_namespaces (client subscription state)
- Multi-relay SDK failover — single relay is a single point of failure; ordered relay list with last-known-good preference and jittered rotation completes the resilience story
- Relay subscription forwarding — end-to-end filtering; relay intercepts Subscribe/Unsubscribe in transit, filters Notification (type 21) before forwarding; stateful but bounded (256 namespaces/client cap)

**Defer (post v2.1.0):**
- Compression statistics in /metrics (bytes saved, per-type ratios)
- Relay /metrics endpoint (relay-specific session and forwarding metrics)
- Dynamic compression level by payload size
- Selective namespace sync (only replicate declared namespaces — extension of BlobNotify filtering)
- SDK health check API exposing current relay, connection state, and metrics

### Architecture Approach

All seven features integrate at well-defined boundaries in the existing three-layer architecture (SDK -> Relay -> Node -> Peers). Compression hooks into PeerManager's send/receive dispatch paths — NOT inside Connection where it would break the relay's blocklist filter and the AEAD nonce contract. The Prometheus server is a new acceptor on a separate port running on the same io_context. The relay gains state (subscription tracking, UDS reconnect state machine) but remains a forwarder in character. No component is redesigned; all are extended.

**Modified components:**
1. PeerManager (db/peer/peer_manager.cpp) — compression in send/recv paths, BlobNotify namespace filter, max_peers reload, metrics server construction
2. RelaySession (relay/core/relay_session.cpp) — subscription interception + forwarding filter, UDS reconnect state machine (ACTIVE/RECONNECTING/DEAD)
3. ChromatinClient (sdk/python/chromatindb/client.py + _reconnect.py) — multi-relay connect() overload, relay rotation on reconnect

**New files:**
1. db/util/compression.h/.cpp — Brotli compress/decompress wrappers with flag byte prefix, output size cap, fallback-to-uncompressed for incompressible data
2. db/net/metrics_server.h/.cpp — Asio TCP acceptor, HTTP GET /metrics, Prometheus text format, reads NodeMetrics + Storage on same io_context thread

### Critical Pitfalls

1. **Decompression bomb (Brotli output unbounded)** — BrotliDecoderDecompress has no built-in output limit; a 6 KB compressed payload can decompress to gigabytes. Must enforce output cap (MAX_BLOB_DATA_SIZE + overhead) before decompression completes. Fuzz-test with adversarial inputs. CVE-2025-6176 is a real instance of this attack.

2. **AEAD nonce desync from wrong compression layer placement** — if compression is inserted after TransportCodec::encode but before send_encrypted, a conditional skip (incompressible data) consumes a nonce without sending, permanently desyncing the AEAD stream. Compression must live inside TransportCodec::encode so data is ready-to-encrypt before it enters the send queue. Flag byte (0x00=uncompressed, 0x01=brotli) ensures incompressible data falls back cleanly without any nonce interaction.

3. **Namespace BlobNotify filter uses the wrong set (subscribed_namespaces vs sync_namespaces)** — BlobNotify to peers must filter by peer's sync_namespaces (node replication config); using subscribed_namespaces (client subscription state) silently breaks peer-to-peer replication. Safety-net sync would catch missing blobs but only every 10 minutes, effectively regressing to timer-based sync.

4. **Metrics endpoint bound to 0.0.0.0** — exposes peer count, storage size, and sync statistics as reconnaissance data. Default must be 127.0.0.1 (or disabled). No pubkey hashes or namespace hashes in metric labels. Operator must explicitly opt in for external binding.

5. **Multi-relay and max_peers thundering-herd reconnects** — naive simultaneous disconnect of N peers causes correlated reconnect storms. For max_peers: graceful drain (refuse new connections, do not mass-disconnect). For multi-relay: randomize relay list order per client on startup, apply jitter (0-2s) before failover, circuit-break after full list exhausted.

## Implications for Roadmap

Seven features, grouped by architectural affinity and dependency order:

### Phase A: Hot Config Reload (max_peers)

**Rationale:** Smallest change (~20 LOC). Completes the SIGHUP reload story and establishes the milestone pattern. Zero risk, zero new dependencies.
**Delivers:** max_peers SIGHUP-reloadable; graceful drain when limit decreased (no mass-disconnect, no thundering herd)
**Addresses:** Table stakes — operators expect full config reloadability
**Avoids:** Thundering-herd pitfall — graceful drain strategy, not mass disconnect

### Phase B: Namespace-Scoped BlobNotify Filtering

**Rationale:** Core protocol change (new SyncNamespaceAnnounce message type, suggested = 62). Build early so subsequent phases have a clean notification path to build on. Low complexity, high traffic-reduction value.
**Delivers:** BlobNotify sent only to peers that replicate the relevant namespace; new wire message type for peer interest declaration after handshake; backward-compatible default (empty interest list = all namespaces)
**Uses:** Existing FlatBuffers transport schema (one new enum value), existing sync_namespaces config
**Avoids:** Critical pitfall — filter by sync_namespaces, NOT subscribed_namespaces

### Phase C: Brotli Wire Compression

**Rationale:** New dependency + protocol change touching multiple send/receive paths. Build after BlobNotify phase so the notification path is clean and message type 62 is settled. Most complex C++ change of the milestone.
**Delivers:** Compress-then-encrypt for Data, BlobTransfer, BlobFetchResponse, ReadResponse messages; self-describing flag byte; incompressible fallback (compressed >= original -> send uncompressed with 0x00 flag); decompression bomb protection; Python SDK brotli support
**Uses:** google/brotli v1.2.0 (NEW C++ dep), brotli~=1.2.0 (NEW Python dep); PeerManager dispatch layer (NOT Connection layer)
**Avoids:** AEAD nonce desync (compression inside encode, not between encode and encrypt); decompression bomb (output size cap enforced before decompression); Brotli FetchContent conflicts (verify CMake integration as first step)

### Phase D: Relay Subscription Forwarding

**Rationale:** Pairs with namespace BlobNotify (both are namespace filtering, completing Phase B). Relay gains per-session subscription state; requires careful state cleanup and per-client cap to avoid memory accumulation.
**Delivers:** Relay intercepts Subscribe/Unsubscribe in transit, tracks per-session subscription set (hard cap: 256 namespaces/client), filters Notification (type 21) before forwarding to client; connection-scoped cleanup on session teardown
**Avoids:** Subscription state accumulation pitfall — hard per-client limit, connection-scoped cleanup

### Phase E: Relay Auto-Reconnect to Node

**Rationale:** Most architecturally involved relay change. Benefits from relay subscription forwarding being complete (re-subscribe after UDS reconnect needs the subscription tracking state from Phase D).
**Delivers:** RelaySession three-state machine (ACTIVE/RECONNECTING/DEAD); jittered exponential backoff (1s-60s); subscription replay on reconnect; client messages rejected (not queued) during RECONNECTING; new socket per reconnect attempt
**Avoids:** UDS-specific reconnect pitfalls — new socket per attempt (POSIX requirement), backoff mandatory even on fast-fail, socket file existence check before connect, log rate bounded

### Phase F: Multi-Relay SDK Failover

**Rationale:** SDK-only change. Logically pairs with relay auto-reconnect (both are resilience features; test together end-to-end). Builds on Phase 84 auto-reconnect machinery.
**Delivers:** connect() overload accepting relay list; last-known-good relay preference on reconnect; jittered failover delay (0-2s); circuit break after full list exhausted; backward compatible with single (host, port); randomized relay order per client at startup for load distribution
**Avoids:** Retry storm pitfall — randomize relay order, jitter failover, circuit break before cycling again

### Phase G: Prometheus /metrics HTTP Endpoint

**Rationale:** Pure observability; independent of all other features. Slot last so it can monitor the features built above. New Asio acceptor on separate port, same io_context thread.
**Delivers:** Configurable metrics_bind_address (default empty = disabled); 15+ counters/gauges in Prometheus text exposition format; no new library dependency; localhost-only default
**Avoids:** Attack surface pitfall — localhost default, no crypto material in labels, disabled-by-default config

### Phase Ordering Rationale

- Phase A first: zero risk, lowest surface area, establishes milestone cadence
- Phase B second: core protocol change (new message type) must settle before compression touches same send paths in Phase C
- Phase C third: new dependency — verify CMake FetchContent integration first; multiple send/receive paths touched; requires Phase B message type to be stable
- Phase D fourth: relay state introduction; pairs conceptually with Phase B filtering; sets up subscription state needed by Phase E
- Phase E fifth: most complex relay change; needs Phase D subscription tracking for post-reconnect subscription replay
- Phase F sixth: SDK-only, can run in parallel with late C++ phases; test together with Phase E for end-to-end resilience verification
- Phase G last: pure observability; independent, slots anywhere; most useful when monitoring the features built above

### Research Flags

Phases needing closer attention during planning:

- **Phase C (Brotli compression):** Compression integration point in the send pipeline requires exact placement decision (inside TransportCodec::encode, not Connection). Decompression bomb mitigation must be the first thing designed. Verify FetchContent integration builds cleanly alongside all existing deps before writing any compression code.
- **Phase E (Relay auto-reconnect):** Three-state lifecycle with concurrent client session management. Lifecycle ordering (race between reconnect coroutine and client disconnect coroutine) needs careful design. Explicit test scenarios required: node stop, node restart, stale UDS socket file.

Phases with standard patterns (skip deep research):

- **Phase A (max_peers reload):** ~20 LOC change to existing SIGHUP handler. Pattern established in 10+ existing config fields.
- **Phase B (BlobNotify filtering):** Single filter check in existing on_blob_ingested() loop. New message type follows existing FlatBuffers schema patterns.
- **Phase D (relay subscription forwarding):** Relay session state pattern mirrors PeerInfo.subscribed_namespaces already in the node. Notification payload parsing is 32 bytes from the front of the existing 77-byte format.
- **Phase F (SDK multi-relay):** Extends existing reconnect loop (_reconnect.py). Adds relay list iteration over existing single-endpoint retry.
- **Phase G (Prometheus /metrics):** Prometheus text format is trivially documented. Hand-rolled Asio HTTP acceptor follows existing acceptor patterns in the codebase.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | New deps (brotli) verified against CMake docs, PyPI, and FetchContent conventions. Prometheus library alternatives evaluated with source analysis. All other features confirmed zero-new-dep. |
| Features | HIGH | All features derived from codebase analysis of existing code paths (peer_manager.cpp, relay_session.cpp, client.py). No speculative features. |
| Architecture | HIGH | Integration points are specific to known components with line-level references. Build order verified against dependency graph. Anti-patterns (compression at AEAD layer, prometheus-cpp) explicitly identified. |
| Pitfalls | HIGH | Pitfalls include CVE references (CVE-2025-6176 decompression bomb), CRIME/BREACH class attacks (compression oracle), and codebase-verified gotchas (AEAD nonce contract, UDS POSIX semantics). All have concrete prevention strategies. |

**Overall confidence:** HIGH

### Gaps to Address

- **Brotli FetchContent compatibility with existing deps:** Must be verified at the start of Phase C (add to CMakeLists.txt and build before writing any compression code). Minor risk: Brotli's CMake predates modern FetchContent conventions. Set BROTLI_BUNDLED_MODE=ON, BROTLI_DISABLE_TESTS=ON.
- **SyncNamespaceAnnounce message type number:** Message type 62 suggested by architecture research, but must confirm no conflict with existing protocol types before finalizing transport.fbs. Verify against current chromatindb message type registry before Phase B.
- **Relay subscription forwarding stateless alternative:** PITFALLS.md notes that keeping the relay stateless (let node handle all subscription filtering, relay just passes Subscribe/Unsubscribe and Notification unchanged) avoids the state accumulation pitfall entirely. This tradeoff should be explicitly evaluated during Phase D planning before committing to stateful relay.

## Sources

### Primary (HIGH confidence)
- google/brotli GitHub (v1.2.0) — CMake target names, BUNDLED_MODE behavior, C API surface
- brotli PyPI (v1.2.0) — Python bindings, CPython 3.10-3.14 wheel availability
- Prometheus exposition format specification — text/plain version=0.0.4 format
- prometheus-cpp and prometheus-cpp-lite source — evaluated and rejected (threading model conflicts with Asio)
- CVE-2025-6176 — Brotli decompression bomb exploit
- CRIME/BREACH — compression oracle class of attacks (HTTPS/TLS context, analyzed for applicability)

### Secondary (MEDIUM confidence)
- Cloudflare Brotli benchmarks (2017) — throughput at quality levels 1-9; algorithmic characteristics unchanged since publication
- Compression algorithm comparisons (zstd vs Brotli vs lz4) — ratio and speed tradeoffs
- Prometheus security model documentation — recommended binding and label practices
- Azure retry storm anti-pattern documentation — thundering herd prevention patterns
- Tailscale relay memory leak #17801, Envoy proxy memory leak #20187 — subscription state accumulation analogies

### Codebase Analysis (HIGH confidence — direct inspection)
- db/peer/peer_manager.cpp — on_blob_ingested, dump_metrics, reload_config, BlobNotify fan-out loop
- relay/core/relay_session.cpp — handle_node_close (teardown path), handle_client_message (blocklist filter)
- relay/core/message_filter.h — blocklist approach
- db/net/connection.h — send queue, AEAD nonce counters, write_frame
- sdk/python/chromatindb/client.py and _reconnect.py — connect() signature, backoff machinery
- db/config/config.h — current config structure and existing SIGHUP-reloadable fields

---
*Research completed: 2026-04-04*
*Ready for roadmap: yes*
