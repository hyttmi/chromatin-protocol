# Phase 28: Load Generator - Research

**Researched:** 2026-03-15
**Domain:** C++20 load generation tool, chromatindb wire protocol, latency measurement
**Confidence:** HIGH

## Summary

Phase 28 builds `chromatindb_loadgen`, a standalone C++ binary that connects to a running chromatindb node as a protocol-compliant peer, performs a PQ handshake, and sends signed blobs at a configurable rate. The tool measures per-blob latency and emits summary statistics as JSON to stdout.

The key insight from codebase investigation: **Data messages are fire-and-forget** -- the chromatindb protocol has no DataAck message. The node silently ingests valid blobs. However, the Notification (pub/sub) mechanism provides an end-to-end ACK path: the loadgen subscribes to its own namespace, sends Data messages, and receives Notification callbacks when blobs are ingested. This gives true per-blob ACK latency (send -> node ingest -> notification received).

**Primary recommendation:** Build a single-file loadgen tool that reuses `chromatindb_lib` (identity, crypto, wire, net layers) for connection and blob construction. Use Asio steady_timer for timer-driven scheduling to prevent coordinated omission. Use pub/sub Notification as the ACK mechanism for latency measurement.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| LOAD-01 | chromatindb_loadgen C++ binary connects as protocol-compliant peer, performs PQ handshake, sends signed blobs | Reuse `Connection::create_outbound()` + `chromatindb_lib` for full protocol compliance. No custom handshake code needed -- the existing Connection class handles PQ handshake transparently. |
| LOAD-02 | Load generator supports configurable blob count, sizes, and write rate with timer-driven scheduling (no coordinated omission) | Use `asio::steady_timer` with fixed interval scheduling. Pre-schedule send times at startup; timer fires at next scheduled time regardless of whether previous send completed. Track intended vs actual send time for accurate latency. |
| LOAD-03 | Mixed-size workload mode distributes blobs across small/medium/large sizes | Simple weighted random distribution: small (1 KiB), medium (100 KiB), large (1 MiB). Configurable ratios via CLI. Default: 70/20/10. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| chromatindb_lib | (project) | Full protocol stack: crypto, wire, net, identity | Reuse the exact same code the daemon uses -- guarantees protocol compliance |
| Standalone Asio | 1.38.0 | TCP connection, coroutines, timer scheduling | Already in project; steady_timer is the right primitive for fixed-rate scheduling |
| nlohmann/json | 3.11.3 | JSON output of metrics | Already in project; trivial to serialize stats |
| spdlog | 1.15.1 | Structured logging (debug/progress) | Already in project |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| (none) | - | - | No new dependencies needed. All required functionality exists in chromatindb_lib |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Notification-based ACK | Pure fire-and-forget send rate | Loses per-blob latency measurement; only measures throughput. Notification path gives true end-to-end latency. |
| Custom handshake code | Connection class reuse | No reason to hand-roll; Connection handles PQ/trusted handshake transparently |
| External HDR histogram lib | Simple sorted-vector percentile calculation | For <100K blobs, sorted vector with nth_element is sufficient and avoids a new dependency |

**Installation:**
```bash
# No new dependencies -- everything comes from chromatindb_lib
```

## Architecture Patterns

### Recommended Project Structure
```
loadgen/
    loadgen_main.cpp       # Single file: CLI parsing, connection, scheduling, stats
```

Plus changes to:
```
CMakeLists.txt             # Add chromatindb_loadgen executable target
Dockerfile                 # Add chromatindb_loadgen to build and copy
```

### Pattern 1: Timer-Driven Fixed-Rate Scheduling (Anti-Coordinated-Omission)
**What:** Pre-compute all send times at startup. An Asio steady_timer fires at each scheduled time. If a send is late (because previous send blocked), the timer catches up by immediately firing the overdue sends. Latency is measured from *scheduled* time, not actual send time.
**When to use:** Always. This is the correct way to do load generation.
**Example:**
```cpp
// Source: Coordinated omission literature (Gil Tene / wrk2 pattern)
// Schedule: blob i should be sent at start_time + i * interval
auto scheduled_time = start_time_ + i * send_interval_;
timer_.expires_at(scheduled_time);
co_await timer_.async_wait(use_nothrow);

auto actual_send_time = Clock::now();
// Record latency from scheduled_time (not actual_send_time)
// This captures queueing delay from slow previous sends
co_await send_blob();
```

### Pattern 2: Pub/Sub Notification as ACK
**What:** The loadgen subscribes to its own namespace immediately after handshake. When a blob is ingested by the node, the node sends a Notification (type 22) back to the loadgen with the blob_hash, seq_num, and size. The loadgen matches this to the outstanding send and records the latency.
**When to use:** For per-blob ACK latency measurement.
**Example:**
```cpp
// After handshake, before sending blobs:
// Subscribe to our own namespace
auto ns_payload = encode_namespace_subscribe({identity_.namespace_id()});
co_await conn->send_message(TransportMsgType_Subscribe, ns_payload);

// In message callback, match Notification to pending sends:
// Notification payload: [namespace_id:32][blob_hash:32][seq_num:8][blob_size:4][is_tombstone:1]
```

### Pattern 3: Connection as Initiator (Outbound)
**What:** The loadgen connects to the target node as an outbound (initiator) peer. It creates a TCP socket, connects, then uses `Connection::create_outbound()` followed by `conn->run()` which handles the handshake automatically.
**When to use:** This is how the loadgen establishes its connection.
**Example:**
```cpp
// Based on existing codebase pattern (PeerManager outbound connects)
asio::ip::tcp::socket socket(ioc);
co_await socket.async_connect(endpoint, use_nothrow);
auto conn = Connection::create_outbound(std::move(socket), identity);
conn->on_message([this](auto conn, auto type, auto payload) {
    handle_response(conn, type, std::move(payload));
});
co_await conn->run();  // Handles PQ handshake + enters message loop
```

### Pattern 4: Percentile Calculation Without HDR Histogram
**What:** For sub-100K measurement counts, store latencies in a vector, sort, and compute percentiles by index. p50 = sorted[n*0.50], p95 = sorted[n*0.95], p99 = sorted[n*0.99].
**When to use:** When the number of measurements is bounded (load generator runs are typically <100K blobs).
**Example:**
```cpp
std::vector<double> latencies;
// ... collect latencies ...
std::sort(latencies.begin(), latencies.end());
auto p = [&](double pct) -> double {
    size_t idx = static_cast<size_t>(pct * latencies.size());
    return latencies[std::min(idx, latencies.size() - 1)];
};
double p50 = p(0.50), p95 = p(0.95), p99 = p(0.99);
```

### Anti-Patterns to Avoid
- **Response-driven scheduling (coordinated omission):** Never wait for one blob to be ACK'd before scheduling the next. The timer fires at fixed intervals regardless of response status.
- **Custom handshake implementation:** The existing `Connection` class handles everything. Do not re-implement the KEM/auth exchange.
- **Blocking random data generation:** Pre-generate all blob payloads before the timed run starts. Random data generation during the timed loop skews latency measurements.
- **Using `std::chrono::system_clock` for latency:** Use `steady_clock` -- system clock can jump (NTP corrections). All Asio timers use steady_clock internally.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| PQ handshake | Custom KEM/auth exchange | `Connection::create_outbound()` + `conn->run()` | 713 lines of tested handshake code already exists |
| Blob construction & signing | Manual SHA3/ML-DSA calls | `wire::build_signing_input()` + `identity.sign()` | Existing `make_signed_blob` pattern in bench_main.cpp |
| Wire encoding | Manual FlatBuffer construction | `wire::encode_blob()` + `TransportCodec::encode()` | Tested codec with deterministic encoding |
| AEAD encryption | Manual encrypt/decrypt | `Connection::send_message()` handles this transparently | Nonce counter management is error-prone |
| Notification encoding | Manual byte packing | Decode using same 77-byte format as PeerManager | Fixed format documented in PROTOCOL.md |

**Key insight:** The entire protocol stack is in `chromatindb_lib`. The loadgen is a thin orchestration layer on top -- it generates blobs, schedules sends, and collects metrics. Everything else is reused.

## Common Pitfalls

### Pitfall 1: Coordinated Omission
**What goes wrong:** Load generator sends blob, waits for response/completion, then sends next blob. If the system is slow, the generator backs off, missing the very measurements that matter most (tail latency during high load).
**Why it happens:** Natural instinct is to use request-response loop.
**How to avoid:** Timer-driven scheduling with pre-computed send times. Measure latency from scheduled time, not actual send time.
**Warning signs:** p99 latency looks suspiciously close to p50 under load.

### Pitfall 2: AEAD Nonce Desync
**What goes wrong:** If the loadgen sends messages out-of-order or skips the handshake counter setup, the AEAD nonce counter desyncs and all subsequent messages fail to decrypt.
**Why it happens:** Connection manages nonce counters internally. If you bypass `send_message()` and manually encrypt, counters diverge.
**How to avoid:** Always use `Connection::send_message()`. Never encrypt manually.
**Warning signs:** "AEAD decrypt failed" in server logs after first message.

### Pitfall 3: Measuring Pre-Generation Time in Latency
**What goes wrong:** Blob signing with ML-DSA-87 takes ~1-2ms. If blob construction happens inside the timed send loop, it inflates latency measurements.
**Why it happens:** Lazy initialization -- generating blobs on-the-fly seems simpler.
**How to avoid:** Pre-generate all blobs (or at least the signatures) before starting the timed run. Use a pool of pre-signed blobs.
**Warning signs:** Minimum latency matches ML-DSA-87 sign time (~1.5ms).

### Pitfall 4: Connection Not Surviving Async Message Loop
**What goes wrong:** `Connection::run()` enters a message_loop that co_awaits on recv_encrypted. If you need to send messages concurrently, you must co_spawn the sending coroutine separately.
**Why it happens:** `run()` blocks (awaits) on the recv loop. Sends must happen from a different coroutine.
**How to avoid:** Set up `on_message` callback, `co_spawn` the `conn->run()` coroutine, then `co_spawn` the send-scheduling coroutine separately. Both run on the same io_context strand.
**Warning signs:** Sends never execute because `run()` blocks the coroutine.

### Pitfall 5: No Notification Subscription Before Sending
**What goes wrong:** Blobs are sent before the Subscribe message is processed. Notifications for early blobs are never received, causing timeout or missing latency data.
**Why it happens:** Subscribe is async -- sending it doesn't guarantee immediate activation on the server side.
**How to avoid:** Send Subscribe, then wait briefly (or for a Pong round-trip) before starting blob sends. Alternatively, tolerate missing a few early notifications.
**Warning signs:** First N notifications missing, then latencies stabilize.

### Pitfall 6: Large Blob Memory Pressure
**What goes wrong:** Pre-generating 1000 x 1 MiB blobs = 1 GiB of RAM just for payloads (plus signatures, encoding overhead).
**Why it happens:** Naive pre-generation of all blobs.
**How to avoid:** Pre-generate random data buffers of each size class (1 KiB, 100 KiB, 1 MiB). Reuse data buffers but with unique timestamps (each blob gets a unique timestamp to avoid deduplication). Only sign at generation time.
**Warning signs:** OOM during pre-generation phase.

## Code Examples

Verified patterns from the existing codebase:

### Blob Construction (from bench_main.cpp)
```cpp
// Source: bench/bench_main.cpp lines 74-92
static chromatindb::wire::BlobData make_signed_blob(
    const chromatindb::identity::NodeIdentity& id,
    const std::vector<uint8_t>& data,
    uint32_t ttl,
    uint64_t timestamp)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data = data;
    blob.ttl = ttl;
    blob.timestamp = timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}
```

### Sending a Data Message
```cpp
// Source: db/peer/peer_manager.cpp (inferred from receive side, line 479-523)
// The node receives Data messages as:
//   type = TransportMsgType_Data
//   payload = FlatBuffer-encoded Blob
// So the loadgen sends:
auto encoded = chromatindb::wire::encode_blob(blob);
co_await conn->send_message(
    chromatindb::wire::TransportMsgType_Data,
    std::span<const uint8_t>(encoded));
```

### Notification Decode (77-byte fixed format)
```cpp
// Source: db/PROTOCOL.md and db/peer/peer_manager.cpp line 1237-1242
// Notification payload: [namespace_id:32][blob_hash:32][seq_num:8][blob_size:4][is_tombstone:1]
struct NotificationData {
    std::array<uint8_t, 32> namespace_id;
    std::array<uint8_t, 32> blob_hash;
    uint64_t seq_num;
    uint32_t blob_size;
    bool is_tombstone;
};
// Parse from 77-byte payload (big-endian integers)
```

### Subscribe Message Encoding
```cpp
// Source: db/peer/peer_manager.h lines 132-137
// Uses PeerManager::encode_namespace_list() format:
//   [uint16_be count][ns_id:32][ns_id:32]...
// The loadgen subscribes to its own namespace:
std::vector<std::array<uint8_t, 32>> namespaces = {identity.namespace_id_array()};
auto payload = chromatindb::peer::PeerManager::encode_namespace_list(namespaces);
co_await conn->send_message(TransportMsgType_Subscribe, payload);
```

### Outbound Connection (from test_daemon.cpp pattern)
```cpp
// Source: db/net/connection.h lines 44-45, test_daemon.cpp two-node pattern
asio::ip::tcp::resolver resolver(ioc);
auto endpoints = resolver.resolve(host, port);
asio::ip::tcp::socket socket(ioc);
co_await asio::async_connect(socket, endpoints, use_nothrow);
auto conn = chromatindb::net::Connection::create_outbound(
    std::move(socket), identity);
conn->on_message(message_handler);
conn->on_close(close_handler);
co_await conn->run();  // PQ handshake + message loop
```

### JSON Output Format (for LOAD-02 stats requirement)
```cpp
// Target JSON format for stdout:
{
    "scenario": "fixed-1k",
    "total_blobs": 1000,
    "duration_sec": 10.5,
    "blobs_per_sec": 95.2,
    "mib_per_sec": 0.093,
    "latency_ms": {
        "p50": 2.1,
        "p95": 5.3,
        "p99": 12.7,
        "min": 1.2,
        "max": 45.8,
        "mean": 3.1
    },
    "blob_sizes": {
        "small_1k": 700,
        "medium_100k": 200,
        "large_1m": 100
    },
    "errors": 0
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Response-driven load gen | Timer-driven (wrk2/Hyperfoil model) | ~2015 (Gil Tene's talks) | Accurate tail latency measurement |
| HDR Histogram for percentiles | Simple sorted vector (for small N) | N/A (both valid) | HDR is overkill for <100K samples |
| Separate load gen protocol | Reuse protocol-compliant peer | N/A (project-specific) | Ensures load gen exercises the real code path |

**Deprecated/outdated:**
- Nothing in this domain is deprecated. The chromatindb protocol is stable at v0.6.0.

## Open Questions

1. **Should the loadgen wait for all Notifications before reporting, or use a timeout?**
   - What we know: Notifications are async, some may be delayed or lost if the connection drops.
   - What's unclear: How long to wait after the last send before declaring the run complete.
   - Recommendation: Use a 5-second drain timeout after the last send. Report received-count vs sent-count. Missing notifications are reported but don't fail the run.

2. **Should the loadgen handle StorageFull from the node?**
   - What we know: Node sends StorageFull (type 23) when capacity is exceeded.
   - What's unclear: Should the loadgen abort, warn, or ignore?
   - Recommendation: Log a warning and stop sending. Report partial results. This is important for Phase 30 scenarios where we hit storage limits.

3. **CLI interface design**
   - What we know: Needs: target address, blob count, blob size, rate, mixed-mode toggle.
   - Recommendation: Use the same `parse_args` pattern as the main daemon: positional command + flag options. Example: `chromatindb_loadgen --target 127.0.0.1:4200 --count 1000 --rate 100 --size 1024 --mixed`

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | CMakeLists.txt (catch_discover_tests) |
| Quick run command | `cmake --build build && ./build/chromatindb_tests -t "[loadgen]"` |
| Full suite command | `cmake --build build && ctest --test-dir build` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| LOAD-01 | Loadgen connects, handshakes, sends accepted blobs | integration (E2E) | Start a node, run loadgen with --count 5, verify node ingested 5 blobs | No -- manual verification via daemon logs / blob count |
| LOAD-02 | Timer-driven scheduling, no coordinated omission | design verification | Inspect code: sends scheduled by steady_timer, latency from scheduled time | No -- code review |
| LOAD-03 | Mixed-size distribution across small/medium/large | integration | Run loadgen with --mixed --count 100, verify distribution in JSON output | No -- manual verification via JSON output |

Note: The loadgen is a standalone tool, not a library with unit-testable interfaces. Validation is primarily through integration testing (run loadgen against a node) and output inspection. Phase 30 (Benchmark Scenarios) will exercise the loadgen extensively, serving as the comprehensive test.

### Sampling Rate
- **Per task commit:** Build `chromatindb_loadgen` successfully
- **Per wave merge:** Run loadgen with `--count 10 --rate 5` against a local node, verify JSON output
- **Phase gate:** All three success criteria met (connect+send, timer-driven, mixed-size, JSON stats)

### Wave 0 Gaps
- [ ] `loadgen/loadgen_main.cpp` -- the loadgen source file (new)
- [ ] CMakeLists.txt update -- add `chromatindb_loadgen` target
- [ ] Dockerfile update -- build and copy `chromatindb_loadgen` binary

## Sources

### Primary (HIGH confidence)
- **Codebase analysis** -- db/net/connection.h, db/net/connection.cpp, db/peer/peer_manager.cpp, db/wire/codec.h, db/engine/engine.h, bench/bench_main.cpp, tests/test_daemon.cpp, db/PROTOCOL.md
- **Wire protocol** -- db/wire/transport_generated.h (all 26 message types verified)
- **Existing patterns** -- make_signed_blob() in bench_main.cpp and test_daemon.cpp

### Secondary (MEDIUM confidence)
- [Gil Tene / Coordinated Omission](https://redhatperf.github.io/post/coordinated-omission/) -- timer-driven scheduling pattern
- [ScyllaDB on Coordinated Omission](https://www.scylladb.com/2021/04/22/on-coordinated-omission/) -- open vs closed workload models

### Tertiary (LOW confidence)
- None. All findings are from direct codebase inspection.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - direct codebase inspection, all libraries already in project
- Architecture: HIGH - Connection class API verified, Data message flow traced end-to-end, Notification mechanism confirmed
- Pitfalls: HIGH - identified from protocol semantics (no DataAck, nonce counters, async message loop)

**Research date:** 2026-03-15
**Valid until:** 2026-04-15 (stable -- internal project, no external API changes)
