# Phase 109: New Features - Research

**Researched:** 2026-04-13
**Domain:** Relay notification source exclusion, blob size limiting, health endpoint
**Confidence:** HIGH

## Summary

Phase 109 implements three independent features across two layers (node + relay). The node fix is a single line adding source exclusion to the Notification(21) fan-out loop in `blob_push_manager.cpp`. The relay needs three additions: (1) a WriteTracker that maps blob_hash to writer_session_id for relay-side source exclusion during notification fan-out, (2) a configurable max_blob_size_bytes field in RelayConfig with pre-translation rejection of oversized Data(8) payloads, and (3) a /health JSON endpoint on the existing MetricsCollector HTTP acceptor.

All three features are well-scoped with clear insertion points in existing code. The node-side fix is trivial (verified by reading blob_push_manager.cpp lines 82-90 -- the Notification loop lacks the `if (peer->connection == source) continue;` check that the BlobNotify loop at line 65 already has). The relay-side features build on established patterns: WriteTracker mirrors SubscriptionTracker's style, the blob size check slots into WsSession::on_message before json_to_binary, and /health adds a second route to MetricsCollector::handle_connection.

**Primary recommendation:** Implement the node fix first (one line), then WriteTracker + relay notification filtering, then blob size limit, then /health endpoint. Each is independently testable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: In blob_push_manager.cpp on_blob_ingested(), add source exclusion to Notification(21) fan-out loop (lines 82-90). Add `if (peer->connection == source) continue;` before the Notification send.
- D-02: This is the root fix -- if the node doesn't send the echo, relay doesn't need to filter it for UDS-connected clients.
- D-03: Relay must also suppress notification echo for relay-connected clients. WriteAck request_id maps to session, but Notification(21) arrives with request_id=0.
- D-04: When relay receives WriteAck (type 30) for Data(8) or Delete(17), extract blob_hash from payload and record {blob_hash -> writer_session_id}. When Notification(21) arrives, extract blob_hash, look up writer session, skip it during fan-out. Expire entries after 5 seconds.
- D-05: New class WriteTracker in relay/core/write_tracker.h -- unordered_map<array<uint8_t,32>, uint64_t> with timestamp-based expiry.
- D-06: New config field max_blob_size_bytes (uint32_t, default 0 = no limit). SIGHUP-reloadable.
- D-07: Relay checks incoming Data(8) JSON data field size (after base64 decode length calculation) against limit BEFORE forwarding. Reject with JSON error {"type": "error", "code": "blob_too_large", "max_size": N}.
- D-08: Relay-only feature -- node has MAX_BLOB_DATA_SIZE (100 MiB). Relay limit meant to be lower.
- D-09: Add /health route to existing MetricsCollector HTTP acceptor. No new listener.
- D-10: Response: 200 OK with {"status": "healthy", "relay": "ok", "node": "connected"} when UDS connected. 503 with {"status": "degraded", "relay": "ok", "node": "disconnected"} when UDS down.
- D-11: Content-Type: application/json. Minimal JSON -- no metrics, no version info.

### Claude's Discretion
- Whether WriteTracker should be a standalone class or inline in UdsMultiplexer
- Whether blob_too_large should be a new error code (7) in error_codes.h or relay-only
- Whether /health should include uptime or connection count

### Deferred Ideas (OUT OF SCOPE)
None
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| FEAT-01 | Source exclusion for notifications -- relay tracks which client wrote a blob, suppresses echo notification to that client | Node fix at blob_push_manager.cpp:82-90 (add source check). Relay: WriteTracker in route_response (WriteAck type 30/18) + handle_notification (type 21) filtering. WriteAck payload format confirmed: [blob_hash:32][seq_num:8BE][status:1] = 41 bytes. |
| FEAT-02 | Relay-side max blob size limit (configurable, separate from node's 100 MiB) | Config field in RelayConfig, base64 length check in WsSession::on_message before json_to_binary call. SIGHUP reload via shared atomic. |
| FEAT-03 | Health check endpoint (HTTP GET /health returns 200 when relay+UDS connected) | Second route in MetricsCollector::handle_connection. UDS connected_ state accessible via UdsMultiplexer::is_connected(). |
</phase_requirements>

## Architecture Patterns

### Recommended Changes by File

```
db/peer/blob_push_manager.cpp     # 1-line fix: source exclusion for Notification(21)
relay/core/write_tracker.h        # NEW: blob_hash -> session_id short-lived map
relay/core/uds_multiplexer.h/cpp  # Wire WriteTracker into route_response + handle_notification
relay/config/relay_config.h/cpp   # Add max_blob_size_bytes field
relay/ws/ws_session.cpp            # Blob size check before json_to_binary
relay/relay_main.cpp               # SIGHUP reload for max_blob_size_bytes, wire WriteTracker
relay/core/metrics_collector.h/cpp # Add /health route, accept UDS health callback
relay/tests/                       # New: test_write_tracker.cpp, extend test_metrics_collector.cpp, test_relay_config.cpp
```

### Pattern 1: Node Source Exclusion (One-Line Fix)

**What:** Add `if (peer->connection == source) continue;` to the Notification(21) loop in `on_blob_ingested()`.

**Current code (blob_push_manager.cpp:82-90):**
```cpp
// Notification (type 21) to subscribed clients -- existing pub/sub behavior
for (auto& peer : peers_) {
    if (peer->subscribed_namespaces.count(namespace_id)) {
        auto conn = peer->connection;
        auto payload_copy = payload;
        asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
            co_await conn->send_message(wire::TransportMsgType_Notification,
                                         std::span<const uint8_t>(p));
        }, asio::detached);
    }
}
```

**Fixed code:**
```cpp
for (auto& peer : peers_) {
    if (peer->connection == source) continue;  // FEAT-01: source exclusion
    if (peer->subscribed_namespaces.count(namespace_id)) {
        // ... unchanged
    }
}
```

**Why this works:** The `source` parameter is already passed as `net::Connection::Ptr` to `on_blob_ingested()`. The BlobNotify(59) loop at line 65 already uses this exact pattern. The Notification(21) loop was simply missing it.

### Pattern 2: WriteTracker Design

**What:** Short-lived map tracking which WebSocket session wrote which blob, so notification fan-out can exclude the writer.

**Recommended:** Standalone class in `relay/core/write_tracker.h` (header-only is sufficient given the simplicity).

**Rationale for standalone vs inline:**
- SubscriptionTracker is a standalone class -- WriteTracker follows the same pattern
- UdsMultiplexer already has 120+ lines of header -- keeping WriteTracker separate is cleaner
- Standalone class is independently testable (matching the project's test-per-component pattern)

**Key data structures:**
```cpp
struct WriteEntry {
    uint64_t session_id;
    std::chrono::steady_clock::time_point created;
};

// blob_hash -> writer session_id with timestamp
std::unordered_map<std::array<uint8_t, 32>, WriteEntry, Namespace32Hash> entries_;
```

**Reuse Namespace32Hash:** The existing `Namespace32Hash` in `subscription_tracker.h` works for any 32-byte array (first 8 bytes as uint64_t). Both blob_hash and namespace_id are SHA3-256 outputs with excellent distribution.

**Insertion point in route_response():**
```cpp
// After resolve_response succeeds and type == 30 (WriteAck) or type == 18 (DeleteAck):
if ((type == 30 || type == 18) && payload.size() >= 32) {
    std::array<uint8_t, 32> blob_hash{};
    std::memcpy(blob_hash.data(), payload.data(), 32);
    write_tracker_.record(blob_hash, pending->client_session_id);
}
```

**Usage in handle_notification():**
```cpp
// After extracting blob_hash from notification payload (offset 32, 32 bytes):
if (payload.size() >= 64) {
    std::array<uint8_t, 32> blob_hash{};
    std::memcpy(blob_hash.data(), payload.data() + 32, 32);
    auto writer_id = write_tracker_.lookup_and_remove(blob_hash);
    // Skip writer_id during fan-out
}
```

**Expiry strategy:** 5-second TTL (D-04). Lazy expiry on each `record()` call -- iterate entries, remove any older than 5s. No background timer needed -- write frequency is bounded by client message rate, and the map stays small.

### Pattern 3: Blob Size Limit Check

**What:** Check Data(8) payload size before translation/forwarding.

**Where:** `WsSession::on_message()`, after type is identified as "data" but BEFORE calling `json_to_binary()`. The check should happen early to avoid wasting memory on base64 decode of huge payloads.

**Base64 size calculation (without decoding):**
```cpp
// base64 encoded length -> decoded length: (len * 3) / 4 (minus padding)
// Quick upper-bound: (base64_len * 3) / 4
size_t base64_len = j["data"].get<std::string>().size();
size_t decoded_upper = (base64_len * 3) / 4;  // Conservative upper bound
```

**Config propagation:** Same pattern as rate_limit -- shared `std::atomic<uint32_t>` set in relay_main, read by WsSession on each message. SIGHUP updates the atomic.

**Error response:** Relay-only JSON error. NOT a new entry in node's error_codes.h (blob_too_large is a relay policy, not a node protocol error):
```json
{"type": "error", "code": "blob_too_large", "max_size": 1048576}
```

### Pattern 4: Health Endpoint

**What:** Add `/health` as second route in MetricsCollector's existing HTTP handler.

**Insertion point:** `MetricsCollector::handle_connection()` already has `if (first_line.find("GET /metrics") ...)` with a 404 else branch. Add another check before the 404:

```cpp
if (first_line.find("GET /health") != std::string::npos) {
    // Build health JSON
}
```

**UDS state access:** MetricsCollector needs to know if UDS is connected. Two approaches:
1. **Callback (like GaugeProvider):** Add a `HealthProvider` callback that returns bool.
2. **Direct pointer:** Pass `UdsMultiplexer*` and call `is_connected()`.

Recommendation: Use a callback (`std::function<bool()>`) -- same pattern as `GaugeProvider`. This avoids adding a UdsMultiplexer include to metrics_collector.h.

```cpp
using HealthProvider = std::function<bool()>;
void set_health_provider(HealthProvider provider);
```

Wired in relay_main:
```cpp
metrics_collector.set_health_provider([&uds_mux]() {
    return uds_mux.is_connected();
});
```

### Anti-Patterns to Avoid
- **Do NOT check blob size after base64 decode:** Decoding a 100 MiB base64 string just to check its size wastes memory. Calculate decoded size from base64 string length.
- **Do NOT use a background timer for WriteTracker expiry:** The map is small (bounded by concurrent writes). Lazy cleanup on insert is sufficient.
- **Do NOT add blob_too_large to node error_codes.h:** This is a relay policy enforcement, not a node protocol error. Keep it relay-only.
- **Do NOT add health provider that requires acquiring locks:** `is_connected()` reads a bool member -- no thread safety issues.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| 32-byte array hash | Custom hash function | Existing `Namespace32Hash` | Already in subscription_tracker.h, works for any 32-byte SHA3-256 output |
| Config reload propagation | Per-component config copies | `std::atomic<uint32_t>` shared pattern | Already used for rate_limit_rate and request_timeout |
| HTTP routing | Full HTTP parser | Simple string find on first line | Already working in MetricsCollector -- just add another route |

## Common Pitfalls

### Pitfall 1: WriteTracker Race Between WriteAck and Notification
**What goes wrong:** If the Notification(21) arrives at the relay BEFORE the WriteAck(30) -- the WriteTracker hasn't recorded the writer yet, so source exclusion fails.
**Why it happens:** The node sends WriteAck (with request_id) and Notification (request_id=0) from the same ingest path. Over UDS (single connection), messages arrive in order.
**How to avoid:** Over a single UDS connection, message ordering is guaranteed. WriteAck is sent from the node's Data handler BEFORE `on_blob_ingested()` fires the Notification. So WriteAck always arrives at the relay first. Verified: the ingest path in engine returns the ack, then the Data handler calls `on_blob_ingested()` which sends Notification.
**Warning signs:** If this assumption breaks (e.g., future multi-UDS), notifications would not be filtered. Add a debug log when a notification's blob_hash is not found in WriteTracker.

### Pitfall 2: Blob Size Check Must Be Before Translation
**What goes wrong:** Calling json_to_binary() on a Data(8) message with a 100 MiB base64 `data` field decodes the entire base64 string, allocates memory, and builds a FlatBuffer -- all wasted if the blob is rejected.
**Why it happens:** The size check is placed after translation instead of before.
**How to avoid:** Check `j["data"]` base64 string length in WsSession::on_message() right after identifying type as "data", before calling json_to_binary(). Use `(base64_len * 3) / 4` as upper-bound decoded size.
**Warning signs:** High memory usage when large blobs are rejected.

### Pitfall 3: WriteTracker Expiry Leak on Session Disconnect
**What goes wrong:** A client writes a blob, gets WriteAck, then disconnects before the Notification arrives. The WriteTracker entry lingers for 5 seconds.
**Why it happens:** Normal behavior -- 5-second TTL is acceptable. The entry will be cleaned on next `record()` call's lazy sweep.
**How to avoid:** This is acceptable. Optionally add `remove_session(uint64_t)` to WriteTracker called from session disconnect, but 5-second lazy expiry handles it cleanly. The map is bounded by concurrent write rate.
**Warning signs:** None needed -- this is by design.

### Pitfall 4: Health Endpoint Must Return Correct Content-Type
**What goes wrong:** Load balancers (HAProxy, ALB, nginx) may not parse health responses correctly without Content-Type: application/json.
**Why it happens:** Easy to copy the /metrics Content-Type (text/plain) instead of using application/json.
**How to avoid:** Set Content-Type: application/json explicitly in the /health response.
**Warning signs:** Health checks failing in production despite relay being healthy.

### Pitfall 5: SIGHUP Reload for max_blob_size_bytes
**What goes wrong:** The relay_main SIGHUP handler doesn't reload the new config field, so changes to max_blob_size_bytes require restart.
**Why it happens:** Forgetting to add the atomic update in the SIGHUP handler lambda.
**How to avoid:** Follow the exact pattern used for rate_limit_rate: shared `std::atomic<uint32_t>`, .store() in SIGHUP handler, .load() in WsSession on each message.
**Warning signs:** Config change via SIGHUP doesn't take effect.

### Pitfall 6: Delete(17) Also Needs WriteTracker Recording
**What goes wrong:** Client sends Delete(17), gets DeleteAck(18) with blob_hash, but the resulting Notification(21) (for the tombstone) is not source-excluded.
**Why it happens:** WriteTracker only records from WriteAck(30), not DeleteAck(18).
**How to avoid:** Record in WriteTracker on both type 30 (WriteAck) AND type 18 (DeleteAck). Both have identical 41-byte payload format: [blob_hash:32][seq_num:8BE][status:1].
**Warning signs:** Delete notifications echoed back to the deleter.

## Code Examples

### WriteTracker Header (Standalone)
```cpp
// relay/core/write_tracker.h
#pragma once

#include "relay/core/subscription_tracker.h"  // for Namespace32Hash

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace chromatindb::relay::core {

using BlobHash32 = std::array<uint8_t, 32>;

class WriteTracker {
public:
    /// Record that session_id wrote the blob identified by blob_hash.
    void record(const BlobHash32& blob_hash, uint64_t session_id) {
        // Lazy expiry: purge stale entries on each record
        auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (now - it->second.created > TTL) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        entries_[blob_hash] = {session_id, now};
    }

    /// Look up and remove the writer session for a blob_hash.
    /// Returns session_id if found and not expired, nullopt otherwise.
    std::optional<uint64_t> lookup_and_remove(const BlobHash32& blob_hash) {
        auto it = entries_.find(blob_hash);
        if (it == entries_.end()) return std::nullopt;
        auto now = std::chrono::steady_clock::now();
        if (now - it->second.created > TTL) {
            entries_.erase(it);
            return std::nullopt;
        }
        auto session_id = it->second.session_id;
        entries_.erase(it);
        return session_id;
    }

    /// Remove all entries for a disconnected session.
    void remove_session(uint64_t session_id) {
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (it->second.session_id == session_id) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        uint64_t session_id;
        std::chrono::steady_clock::time_point created;
    };

    static constexpr auto TTL = std::chrono::seconds(5);
    std::unordered_map<BlobHash32, Entry, Namespace32Hash> entries_;
};

} // namespace chromatindb::relay::core
```

### Blob Size Check in WsSession::on_message
```cpp
// After type_str is extracted and confirmed as "data", before json_to_binary:
if (type_str == "data" && max_blob_size_ && max_blob_size_->load(std::memory_order_relaxed) > 0) {
    uint32_t limit = max_blob_size_->load(std::memory_order_relaxed);
    if (j.contains("data") && j["data"].is_string()) {
        size_t b64_len = j["data"].get_ref<const std::string&>().size();
        size_t decoded_upper = (b64_len * 3) / 4;
        if (decoded_upper > limit) {
            nlohmann::json err = {{"type", "error"}, {"code", "blob_too_large"},
                                  {"max_size", limit}};
            if (request_id) err["request_id"] = *request_id;
            send_json(err);
            co_return;
        }
    }
}
```

### Health Endpoint in MetricsCollector::handle_connection
```cpp
if (first_line.find("GET /health") != std::string::npos) {
    bool node_connected = health_provider_ ? health_provider_() : false;
    nlohmann::json body_json;
    body_json["relay"] = "ok";
    if (node_connected) {
        body_json["status"] = "healthy";
        body_json["node"] = "connected";
    } else {
        body_json["status"] = "degraded";
        body_json["node"] = "disconnected";
    }
    auto body = body_json.dump();
    auto status = node_connected ? "200 OK" : "503 Service Unavailable";
    response = "HTTP/1.1 " + std::string(status) + "\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "Connection: close\r\n"
               "\r\n" + body;
}
```

### WriteTracker Recording in route_response
```cpp
// In route_response(), after resolve_response succeeds, before translate/send:
if ((type == 30 || type == 18) && payload.size() >= 32 && pending) {
    std::array<uint8_t, 32> blob_hash{};
    std::memcpy(blob_hash.data(), payload.data(), 32);
    write_tracker_.record(blob_hash, pending->client_session_id);
}
```

### WriteTracker Filtering in handle_notification
```cpp
// In handle_notification(), after getting subscriber set, before fan-out:
std::optional<uint64_t> writer_session;
if (payload.size() >= 64) {
    std::array<uint8_t, 32> blob_hash{};
    std::memcpy(blob_hash.data(), payload.data() + 32, 32);
    writer_session = write_tracker_.lookup_and_remove(blob_hash);
}

for (uint64_t sid : session_ids) {
    if (writer_session && sid == *writer_session) continue;  // Source exclusion
    auto session = sessions_.get_session(sid);
    if (session) {
        session->send_json(*json_opt);
    }
}
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt (FetchContent) |
| Quick run command | `cd build && ./relay/tests/chromatindb_relay_tests` |
| Full suite command | `cd build && ctest --test-dir relay/tests --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| FEAT-01 (node) | Source exclusion for Notification(21) in on_blob_ingested | unit | `./db/tests/chromatindb_tests "[blob_push]"` | Wave 0 (test_blob_push.cpp exists but needs new test) |
| FEAT-01 (relay WriteTracker) | WriteTracker record/lookup/expiry/remove_session | unit | `./relay/tests/chromatindb_relay_tests "[write_tracker]"` | Wave 0 (new test_write_tracker.cpp) |
| FEAT-01 (relay integration) | handle_notification skips writer session | unit | `./relay/tests/chromatindb_relay_tests "[write_tracker]"` | Wave 0 |
| FEAT-02 | Blob size check rejects oversized Data(8) | unit | `./relay/tests/chromatindb_relay_tests "[relay_config]"` | Config test exists; WsSession integration is E2E |
| FEAT-02 | max_blob_size_bytes config load + validate | unit | `./relay/tests/chromatindb_relay_tests "[relay_config]"` | Extend existing test_relay_config.cpp |
| FEAT-03 | /health returns 200 when connected, 503 when disconnected | unit | `./relay/tests/chromatindb_relay_tests "[metrics_collector]"` | Extend existing test_metrics_collector.cpp |

### Sampling Rate
- **Per task commit:** `cd build && ./relay/tests/chromatindb_relay_tests`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before verification

### Wave 0 Gaps
- [ ] `relay/tests/test_write_tracker.cpp` -- covers FEAT-01 (WriteTracker record/lookup/expiry/remove)
- [ ] Extend `relay/tests/test_metrics_collector.cpp` -- covers FEAT-03 (health endpoint format)
- [ ] Extend `relay/tests/test_relay_config.cpp` -- covers FEAT-02 (max_blob_size_bytes config)
- [ ] `relay/tests/CMakeLists.txt` -- add test_write_tracker.cpp to sources

## Key Implementation Details

### WriteAck Payload Format (confirmed from json_schema.h:103-109)
```
WriteAck (type 30): [blob_hash:32][seq_num:8BE][status:1] = 41 bytes
DeleteAck (type 18): [blob_hash:32][seq_num:8BE][status:1] = 41 bytes (same format)
```
blob_hash is at offset 0, 32 bytes.

### Notification Payload Format (confirmed from blob_push_manager.cpp:61)
```
Notification (type 21): [namespace:32][blob_hash:32][seq_num:8BE][blob_size:4BE][tombstone:1] = 77 bytes
```
blob_hash is at offset 32, 32 bytes.

### UDS Message Ordering Guarantee
Over the single UDS connection, messages are serialized. The node's Data handler sends WriteAck BEFORE calling on_blob_ingested() (which sends Notification). Therefore WriteAck always arrives at the relay before the corresponding Notification. This ordering guarantee makes the WriteTracker approach reliable.

### Base64 Decoded Size Formula
For a base64 string of length L:
- Exact decoded size: `(L * 3) / 4 - padding_count`
- Upper bound (no padding inspection): `(L * 3) / 4`

The upper bound is sufficient for the blob size limit -- a few extra bytes won't matter for a policy check.

### Config Propagation Pattern (max_blob_size_bytes)
Following the established pattern:
1. `RelayConfig::max_blob_size_bytes` -- uint32_t, default 0
2. `std::atomic<uint32_t> max_blob_size{cfg.max_blob_size_bytes}` in relay_main
3. WsSession receives `const std::atomic<uint32_t>*` (same as shared_rate_)
4. SIGHUP handler: `max_blob_size.store(new_cfg.max_blob_size_bytes)`

### Discretion Decisions (Researcher Recommendations)

**WriteTracker: standalone class** -- Recommended. Matches SubscriptionTracker pattern, independently testable, keeps UdsMultiplexer focused. Header-only is clean since the entire implementation is simple enough.

**blob_too_large: relay-only error** -- Recommended. NOT a new entry in error_codes.h. The node has its own MAX_BLOB_DATA_SIZE (100 MiB) with different rejection semantics. The relay's size limit is operator policy, not protocol-level. Keep it as a JSON error string `"blob_too_large"` in the relay only.

**/health: minimal, no uptime/counts** -- Recommended per D-11. Load balancers only need status + node connectivity. Adding uptime or connection count would blur the line with /metrics and is unnecessary for the stated use case (health probing).

## Open Questions

1. **Thread safety of WriteTracker**
   - What we know: UdsMultiplexer methods (route_response, handle_notification) are called on the io_context, same as all relay core logic. NOT multi-threaded like the thread pool might suggest -- route_response/handle_notification are called from the read_loop coroutine on a single io_context.
   - What's unclear: The relay uses multiple threads running ioc.run() (hardware_concurrency). However, route_response is called from within a single coroutine (read_loop), so it's serialized. handle_notification is called from route_response. No concurrent access to WriteTracker.
   - Recommendation: WriteTracker does NOT need to be thread-safe. It's accessed only from the UDS read_loop coroutine. Document this constraint with a comment like SubscriptionTracker's "NOT thread-safe -- all access must be on the same io_context strand."

## Sources

### Primary (HIGH confidence)
- Direct code inspection: blob_push_manager.cpp (node source exclusion pattern), uds_multiplexer.cpp (route_response + handle_notification), metrics_collector.cpp (HTTP handler), relay_config.h/cpp (config pattern), ws_session.cpp (message handling), relay_main.cpp (SIGHUP wiring)
- json_schema.h: WriteAck/DeleteAck field specs confirming [blob_hash:32][seq_num:8BE][status:1] format
- subscription_tracker.h: Namespace32Hash reusable for blob_hash arrays

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all changes use existing libraries (no new deps)
- Architecture: HIGH -- all insertion points verified by reading actual source code
- Pitfalls: HIGH -- based on direct code analysis of data flow and ordering

**Research date:** 2026-04-13
**Valid until:** 2026-05-13 (stable -- all features are local to well-understood code)
