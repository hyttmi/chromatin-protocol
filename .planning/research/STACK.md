# Stack Research: v2.1.0 Compression, Filtering & Observability

**Project:** chromatindb -- Brotli wire compression, Prometheus metrics endpoint, namespace-scoped notifications, relay improvements, multi-relay SDK failover, hot config reload expansion
**Researched:** 2026-04-04
**Confidence:** HIGH

## Scope

This research covers ONLY the new dependencies and patterns required for v2.1.0. The validated stack (C++20, CMake/FetchContent, Standalone Asio 1.38.0, liboqs 0.15.0, libsodium, libmdbx v0.13.11, FlatBuffers 25.2.10, spdlog 1.15.1, nlohmann/json 3.11.3, xxHash, Catch2, Python SDK with asyncio/PyNaCl/liboqs-python/flatbuffers) is shipped and NOT re-researched.

## Verdict: One New C++ Dependency, One New Python Dependency

| Capability | New Dependency? | What | Why |
|------------|-----------------|------|-----|
| Brotli wire compression (C++ node/relay) | YES | google/brotli v1.2.0 | Only production-grade Brotli C library; CMake-native, FetchContent-compatible |
| Brotli wire compression (Python SDK) | YES | `brotli~=1.2.0` pip package | Google's official Python bindings; 3.3M weekly downloads |
| Prometheus /metrics HTTP endpoint | NO | Hand-rolled with Asio | Text format is trivial (~50 lines); avoids threading conflict with Asio io_context |
| Namespace-scoped BlobNotify | NO | Existing wire protocol extension | Filter logic in existing `on_blob_ingested()` |
| Relay subscription forwarding | NO | Existing relay session extension | Filter in `handle_node_message()` |
| Relay auto-reconnect to node | NO | Existing Asio timer patterns | Same jittered backoff pattern as peer reconnect (Phase 42) |
| Multi-relay SDK failover | NO | Python asyncio stdlib | Round-robin with fallback in `connect()` |
| Hot config reload expansion | NO | Existing SIGHUP handler | Add fields to existing reload path |

---

## New Dependency: google/brotli v1.2.0

### Why Brotli (Not zstd)

The project spec calls for Brotli. This is a reasonable choice for chromatindb's wire compression despite zstd being faster at equivalent ratios, because:

1. **Blob data is pre-encryption user content** (documents, JSON, images). Brotli's built-in 120 KiB static dictionary gives 15-20% better compression on text/web content than zstd at comparable speed settings.
2. **At quality level 1** (what we'd use for real-time wire compression), Brotli achieves ~91 MB/s throughput -- more than sufficient for the existing wire protocol where the bottleneck is ML-DSA-87 verification (~2ms per blob), not compression.
3. **Single-blob-at-a-time sync** means only one blob is in flight per connection. Even at 100 MiB max blob size, Brotli-1 compresses in ~1.1 seconds -- acceptable since the verify step takes longer.
4. **Wire compression happens before AEAD encryption** (compress-then-encrypt). Once data enters the AEAD frame, it's random bytes that won't compress. So compression quality on the raw blob payload matters more than raw throughput.

**Important caveat:** Encrypted blobs (envelope-encrypted via KEM-then-Wrap) are already random bytes before wire transmission. Compression will NOT help these. The implementation must detect incompressible data and skip compression (Brotli returns output >= input for random data, which is the signal to send uncompressed).

### C++ Integration

```cmake
# -- google/brotli v1.2.0 (wire compression)
set(BROTLI_BUNDLED_MODE ON CACHE BOOL "" FORCE)
set(BROTLI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(brotli
  GIT_REPOSITORY https://github.com/google/brotli.git
  GIT_TAG        v1.2.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(brotli)
```

**CMake targets (when used as subdirectory):**
- `brotlienc` -- encoder library (compression)
- `brotlidec` -- decoder library (decompression)
- `brotlicommon` -- common functionality (both depend on this)

**Link against:**
```cmake
target_link_libraries(chromatindb_lib PUBLIC brotlienc brotlidec brotlicommon)
```

Note: When `BROTLI_BUNDLED_MODE` is ON (auto-detected when added via `add_subdirectory()`), Brotli skips install targets -- exactly what FetchContent needs.

**API surface (C, not C++):**
```c
#include <brotli/encode.h>   // BrotliEncoderCompress()
#include <brotli/decode.h>   // BrotliDecoderDecompress()
```

Single-call compress/decompress functions. No streaming needed for our use case (we have the full blob in memory already).

### Python Integration

Add to `pyproject.toml` dependencies:
```toml
dependencies = [
    "liboqs-python~=0.14.0",
    "pynacl~=1.5.0",
    "flatbuffers~=25.12",
    "brotli~=1.2.0",
]
```

**Python API:**
```python
import brotli
compressed = brotli.compress(data, quality=1)  # quality 1 for speed
decompressed = brotli.decompress(compressed)
```

### Version Justification

| Version | Released | Why This Version |
|---------|----------|-----------------|
| v1.2.0 | 2024-10-27 | Latest stable release. Includes static initialization to reduce binary size. Security fix for decoder. Previous v1.1.0 was Aug 2024. |

### Compression Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Quality | 1 | Real-time wire compression; ~91 MB/s throughput, acceptable ratio |
| Window size | 22 (default) | 4 MiB window; sufficient for blobs up to 100 MiB |
| Mode | BROTLI_MODE_GENERIC | Blob data is arbitrary content, not specifically text or font |

---

## No New Dependency: Prometheus /metrics Endpoint

### Why NOT prometheus-cpp or prometheus-cpp-lite

Three options were evaluated:

| Library | Version | Deps | HTTP Server | Problem |
|---------|---------|------|-------------|---------|
| prometheus-cpp (jupp0r) | v1.3.0 | zlib, libcurl, civetweb | civetweb (threads) | Heavy deps; civetweb threads conflict with single-threaded Asio io_context model |
| prometheus-cpp-lite (biaks) | v2.0 | None (header-only) | ip-sockets-cpp-lite (blocking sockets) | Blocking socket server runs on separate thread; conflicts with Asio event loop |
| Hand-rolled on Asio | N/A | None | Asio TCP acceptor | Perfect fit: runs on same io_context, zero new deps, ~100 lines |

**Decision: Hand-roll the /metrics endpoint using Asio.**

Rationale:
1. **chromatindb already has NodeMetrics struct** with 12 counters (ingests, rejections, syncs, rate_limited, peers_connected_total, peers_disconnected_total, cursor_hits, cursor_misses, full_resyncs, quota_rejections, sync_rejections). Plus Storage provides blob_count, used_bytes, namespace_count.
2. **Prometheus text format is trivial.** It's `metric_name value\n` with optional `# HELP` and `# TYPE` lines. No complex serialization.
3. **HTTP GET /metrics is trivial.** Parse one line of HTTP, respond with fixed headers + body. No need for a full HTTP framework.
4. **Threading model matters.** Both prometheus-cpp and prometheus-cpp-lite run their own socket/thread for the HTTP server. chromatindb runs everything on a single io_context thread (with thread_pool for crypto offload only). Introducing a second networking thread creates potential data races reading NodeMetrics.
5. **Minimal deps is a project constraint.** Adding civetweb + zlib + libcurl for 12 counters is absurd.

### Implementation Pattern

```cpp
// Minimal HTTP /metrics responder on Asio
class MetricsServer {
    asio::ip::tcp::acceptor acceptor_;
    // ...
    asio::awaitable<void> handle_request(asio::ip::tcp::socket socket) {
        // Read enough for "GET /metrics" (no need to parse full HTTP)
        // Write: "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n\r\n"
        // Write: format_metrics()  -- reads NodeMetrics + Storage stats
    }
};
```

The metrics format:
```
# HELP chromatindb_ingests_total Total successful blob ingestions
# TYPE chromatindb_ingests_total counter
chromatindb_ingests_total 42
# HELP chromatindb_peers_connected Current connected peers
# TYPE chromatindb_peers_connected gauge
chromatindb_peers_connected 3
```

Config addition: `metrics_bind_address` (e.g., `"127.0.0.1:9100"`). Empty = disabled (default).

---

## No New Dependency: Namespace-Scoped BlobNotify

This is pure logic change in existing code. Currently `on_blob_ingested()` sends BlobNotify (type 59) to ALL TCP peers. The change is to check if the receiving peer has any subscription filter, and only send BlobNotify for namespaces the peer cares about.

**Existing infrastructure used:**
- `PeerInfo::subscribed_namespaces` (already exists, used for type 21 Notification filtering)
- `on_blob_ingested()` already iterates `peers_` and checks subscription state for Notification dispatch

No new protocol types needed. BlobNotify already contains the namespace hash in its 77-byte payload.

---

## No New Dependency: Relay Subscription Forwarding

The relay currently forwards ALL node-to-client messages unfiltered (the node only sends client-safe types). For subscription-aware filtering, the relay session tracks which namespaces each client subscribed to, and only forwards BlobNotify/Notification for matching namespaces.

**Existing infrastructure used:**
- `RelaySession::handle_client_message()` already inspects message types (for the blocklist filter)
- Subscribe/Unsubscribe messages (types 19/20) pass through the relay -- the relay can observe them in transit to build its own filter table

No new dependencies. Pure relay session logic.

---

## No New Dependency: Relay Auto-Reconnect to Node

When the relay's UDS connection to the local chromatindb node drops, the relay currently disconnects all client sessions (per D-04). The improvement: relay should attempt UDS reconnection with exponential backoff, and only disconnect clients if reconnection fails after a timeout.

**Existing infrastructure used:**
- `asio::steady_timer` with jittered exponential backoff -- exact same pattern as peer reconnect (Phase 42, db/peer/peer_manager.cpp)
- `asio::local::stream_protocol::socket` for UDS connection

This is relay-only code. No new dependencies.

---

## No New Dependency: Multi-Relay SDK Failover

The Python SDK's `connect()` currently takes a single `(host, port)`. The change: accept a list of relay addresses, try them in order, fall back to next on connection failure, and rotate on reconnect.

**Existing infrastructure used:**
- `asyncio` stdlib: `asyncio.open_connection()` with timeout, exception handling for `ConnectionRefusedError`/`TimeoutError`
- `_reconnect.py` already has backoff logic (Phase 84)

No new pip dependencies. Pure Python asyncio logic.

---

## No New Dependency: Hot Config Reload Expansion

Currently SIGHUP reloads: `allowed_client_keys`, `allowed_peer_keys`, `sync_namespaces`, `expiry_scan_interval_seconds`, `safety_net_interval_seconds`, `rate_limit_bytes_per_sec`, `rate_limit_burst`, `sync_cooldown_seconds`, `compaction_interval_hours`.

New fields to add to hot-reload:
- `max_peers` -- requires checking if current peer count exceeds new limit (soft enforcement: don't disconnect, just refuse new connections)
- `metrics_bind_address` -- restart metrics server if changed (or start/stop)

**Existing infrastructure used:**
- `sighup_handler()` coroutine in `PeerManager` already re-reads and re-applies config
- `config::load_config()` already parses all fields

No new dependencies. Extending existing SIGHUP handler.

---

## Recommended Stack (Complete for v2.1.0)

### New Dependencies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| google/brotli | v1.2.0 | Wire-level blob compression before AEAD encryption | Only production Brotli C library; CMake-native; 91 MB/s at quality 1; 15-20% better ratio than zstd on text content |
| brotli (PyPI) | ~=1.2.0 | SDK-side wire compression matching C++ implementation | Google's official Python bindings; 3.3M weekly downloads; matches C++ brotli v1.2.0 |

### Existing Stack (No Changes)

| Technology | Version | Used For (v2.1.0) |
|------------|---------|-------------------|
| Standalone Asio | 1.38.0 | Prometheus HTTP listener, relay UDS reconnect timer, metrics server coroutine |
| spdlog | 1.15.1 | Structured logging of compression stats, metrics requests |
| nlohmann/json | 3.11.3 | New config fields (metrics_bind_address) |
| FlatBuffers | 25.2.10 | Wire format for compressed payload framing |
| libmdbx | v0.13.11 | Storage stats for /metrics endpoint |
| Catch2 | v3.7.1 | Unit tests for compression, metrics formatting, relay reconnect |
| Python asyncio | stdlib | Multi-relay failover, reconnect logic |

---

## Alternatives Considered

| Category | Recommended | Alternative | Why Not Alternative |
|----------|-------------|-------------|---------------------|
| C++ compression | Brotli v1.2.0 | zstd v1.5.7 (facebook/zstd) | zstd is 2-7x faster at equivalent ratios, but Brotli's static dictionary gives better ratio on text/JSON content common in blob payloads. Project spec calls for Brotli. If profiling shows Brotli is a bottleneck, zstd is a drop-in alternative. |
| C++ compression | Brotli v1.2.0 | lz4 v1.10.0 | lz4 at 660 MB/s is overkill for single-blob-at-a-time sync where ML-DSA verify is the bottleneck. Poor compression ratio (~10%) not worth the complexity. |
| Prometheus metrics | Hand-rolled Asio | prometheus-cpp v1.3.0 | Drags in civetweb + zlib + libcurl. Thread-based HTTP server conflicts with single-thread Asio model. Massive overkill for 15-20 metrics. |
| Prometheus metrics | Hand-rolled Asio | prometheus-cpp-lite v2.0 | Header-only is nice, but uses blocking sockets (ip-sockets-cpp-lite). Would need dedicated thread. Still overkill for trivial text format. |
| Python compression | brotli (PyPI) | brotlicffi | CFFI-based alternative. Unnecessary indirection when brotli (C extension) is the official Google package and faster. |
| SDK multi-relay | asyncio stdlib | External retry library (tenacity) | YAGNI. Simple round-robin with backoff is <50 lines. Adding a dependency for retry logic violates minimal-deps philosophy. |

---

## What NOT to Add

| Avoid | Why | What to Do Instead |
|-------|-----|-------------------|
| prometheus-cpp (jupp0r) | civetweb threads + zlib + libcurl deps | Hand-roll ~100-line Asio HTTP responder |
| prometheus-cpp-lite (biaks) | Blocking sockets conflict with Asio | Hand-roll on Asio |
| zlib/gzip for compression | Inferior ratio to Brotli on text content; no static dictionary | Use Brotli |
| Any HTTP framework (cpp-httplib, Beast) | Massive overkill for single-endpoint GET /metrics | Minimal Asio TCP acceptor |
| tenacity (Python retry lib) | Overkill for simple round-robin failover | asyncio stdlib with for-loop |
| OpenSSL | Project constraint: no OpenSSL | Stick with libsodium + liboqs |

---

## Wire Protocol Compression Design (Stack Implications)

Compression integrates at the **transport framing layer**, between blob serialization and AEAD encryption:

```
Application blob data
    |
    v
[Brotli compress (quality=1)]  <-- NEW: before AEAD
    |
    v
[AEAD encrypt frame]           <-- existing ChaCha20-Poly1305
    |
    v
[TCP send]
```

**Critical constraint:** Compression MUST happen before encryption. Encrypted data (random bytes) does not compress. This means the compression flag must be in the **unencrypted frame header** so the receiver knows to decompress after decryption.

**Frame header change needed:** One bit or byte in the existing 4-byte length prefix or transport envelope to signal "this payload is Brotli-compressed." Options:
1. High bit of length field (current max frame = 110 MiB = ~115M, well under 2^31)
2. New envelope byte after length prefix
3. Compression negotiation in handshake (capability flag in TrustedHello/PQRequired)

Recommendation: **High bit of the 4-byte frame length.** Zero-cost on the wire, backward-compatible (old nodes would interpret as >2 GiB frame and disconnect -- acceptable since pre-production, no backward compat needed).

---

## Installation

### C++ (CMakeLists.txt addition)

```cmake
# -- google/brotli v1.2.0 (wire compression)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(brotli
  GIT_REPOSITORY https://github.com/google/brotli.git
  GIT_TAG        v1.2.0
  GIT_SHALLOW    TRUE
)
```

Add `brotli` to `FetchContent_MakeAvailable(...)` call. Link `brotlienc brotlidec brotlicommon` to `chromatindb_lib`.

### Python SDK (pyproject.toml change)

```toml
dependencies = [
    "liboqs-python~=0.14.0",
    "pynacl~=1.5.0",
    "flatbuffers~=25.12",
    "brotli~=1.2.0",
]
```

---

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| brotli v1.2.0 (C) | CMake >= 3.20 | Requires C99 compiler; GCC/Clang fully supported |
| brotli v1.2.0 (C) | FetchContent | BROTLI_BUNDLED_MODE auto-detected when added via add_subdirectory() |
| brotli ~=1.2.0 (Python) | Python >= 3.10 | Pre-built wheels for CPython 3.10-3.14 on Linux/macOS/Windows |
| brotli ~=1.2.0 (Python) | existing PyNaCl ~=1.5.0 | No interaction -- compression and crypto are independent layers |
| Asio 1.38.0 | Metrics HTTP server | Standard tcp::acceptor + coroutine; no special Asio version requirements |

---

## Sources

- [google/brotli GitHub](https://github.com/google/brotli) -- v1.2.0 release (Oct 2024), CMake target names, BUNDLED_MODE behavior
- [google/brotli CMakeLists.txt](https://github.com/google/brotli/blob/master/CMakeLists.txt) -- target names: brotlienc, brotlidec, brotlicommon; BUILD_SHARED_LIBS control
- [brotli PyPI](https://pypi.org/project/brotli/) -- v1.2.0, 3.3M weekly downloads, CPython 3.10-3.14 wheels
- [Cloudflare Brotli benchmarks](https://blog.cloudflare.com/results-experimenting-brotli/) -- Brotli-1: 91.3 MB/s, Brotli-4: 51 MB/s, comparison with zlib (MEDIUM confidence -- 2017 data but algorithmic characteristics unchanged)
- [Compression algorithm comparison](https://manishrjain.com/compression-algo-moving-data) -- zstd vs Brotli vs lz4 throughput and ratio tradeoffs
- [Prometheus exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/) -- text/plain; version=0.0.4, line-oriented format spec
- [prometheus-cpp GitHub](https://github.com/jupp0r/prometheus-cpp) -- v1.3.0, requires civetweb+zlib+libcurl (HIGH confidence)
- [prometheus-cpp-lite GitHub](https://github.com/biaks/prometheus-cpp-lite) -- v2.0 (Mar 2026), header-only, blocking sockets (HIGH confidence)
- [facebook/zstd releases](https://github.com/facebook/zstd/releases) -- v1.5.7 (Feb 2025), evaluated as alternative

---
*Stack research for: chromatindb v2.1.0 Compression, Filtering & Observability*
*Researched: 2026-04-04*
