# Phase 18: Abuse Prevention & Topology - Research

**Researched:** 2026-03-11
**Domain:** Per-connection rate limiting (token bucket), namespace-scoped sync filtering
**Confidence:** HIGH

## Summary

Phase 18 adds two independent mechanisms to chromatindb: (1) a per-connection token bucket rate limiter for Data and Delete messages that triggers immediate disconnection on exceed, and (2) a configurable namespace filter (`sync_namespaces`) that restricts which namespaces the node replicates and ingests. Both features integrate into well-established patterns: PeerInfo for per-connection state, Config for JSON fields, reload_config() for SIGHUP hot-reload, and the Step 0 pattern for early checks in on_peer_message().

The implementation is entirely internal to existing code -- no new dependencies, no new wire messages, no changes to the FlatBuffers schema. The token bucket is a simple struct in PeerInfo with two uint64_t fields (tokens, last_refill_time). The namespace filter is a `std::set<std::array<uint8_t, 32>>` built from config hex strings, checked at three points: sync Phase A (namespace list assembly), Data ingest, and Delete ingest.

**Primary recommendation:** Implement the token bucket as inline fields in PeerInfo (not a separate class), using `std::chrono::steady_clock` for monotonic refill. Filter namespaces at sync Phase A assembly AND at Data/Delete ingest as the CONTEXT.md specifies. Both features are SIGHUP-reloadable following the existing reload_config() pattern.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Unit: bytes per second (total payload throughput), not message count
- Scope: per-connection only (one token bucket per connected peer)
- Rate limit exceeded = immediate disconnect via close_gracefully() -- no strike system involvement
- Strike system remains separate for validation failures (bad signatures, malformed messages)
- Reconnect allowed immediately -- stateless, no cooldown ban
- Rate limit parameters (bytes/sec, burst) are SIGHUP-reloadable -- follows allowed_keys pattern
- sync_namespaces is SIGHUP-reloadable -- existing sync sessions complete, new syncs use updated filter
- Blobs in now-filtered namespaces: kept in storage but not synced (safe, reversible)
- Namespace filter applies to both sync Phase A AND ingest (Data/Delete messages) -- consistent operator model: "this node doesn't handle that namespace"
- Format: 64-char hex hashes, consistent with allowed_keys
- Model: whitelist only -- sync_namespaces lists what TO replicate, empty = replicate all (PROT-06, mirrors implicit closed mode from allowed_keys)
- Immediate disconnect matches the "no backpressure delay" requirement -- don't slow the peer, cut them
- The allowed_keys implicit-closed-mode pattern (non-empty list = closed) should be replicated exactly for sync_namespaces
- Filtering at ingest prevents storing blobs that would never replicate -- no wasted storage

### Claude's Discretion
- Default rate_limit_bytes_per_sec value
- Default rate_limit_burst value (burst factor)
- rate_limited metric semantics (disconnection events vs dropped messages)
- Log verbosity for rate-limit disconnects
- Reject behavior when peer sends blob to filtered namespace
- Startup validation of sync_namespaces entries

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| PROT-01 | Per-connection token bucket rate limiter applies to Data/Delete messages (not sync BlobTransfer) | Token bucket in PeerInfo struct; check in on_peer_message() before Data/Delete processing; skip for all sync message types (BlobTransfer, SyncRequest, SyncAccept, etc.) |
| PROT-02 | Rate limit exceeded triggers immediate disconnect, no backpressure delay | close_gracefully() call (same pattern as record_strike threshold); increment metrics_.rate_limited; no strike involvement |
| PROT-03 | Rate limit parameters configurable (rate_limit_bytes_per_sec, rate_limit_burst) | Two new uint64_t fields in Config struct; parsed from JSON; SIGHUP-reloadable via reload_config() |
| PROT-04 | Operator can configure sync_namespaces to filter which namespaces the node replicates | New vector<string> in Config, parsed like allowed_keys; converted to std::set<std::array<uint8_t,32>> for O(1) lookup; applied at sync Phase A and Data/Delete ingest |
| PROT-05 | Namespace filter applied at sync Phase A (namespace list assembly), not at blob transfer time | Filter engine_.list_namespaces() result in both run_sync_with_peer() and handle_sync_as_responder() before encode_namespace_list() |
| PROT-06 | Empty sync_namespaces means replicate all (backward compatible default) | Empty set = no filtering applied; mirrors allowed_keys implicit-closed-mode pattern |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| C++20 std::chrono | N/A | Monotonic clock for token bucket refill (steady_clock) | Already used throughout (timer loops), no new dependency |
| std::set<std::array<uint8_t,32>> | N/A | O(log n) namespace filter lookup | Already used in project (subscribed_namespaces in PeerInfo) |

### Supporting
No new dependencies. Everything needed is already in the project stack:
- nlohmann/json for config parsing
- spdlog for logging
- Catch2 for tests

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Token bucket bytes/sec | Leaky bucket | Token bucket is standard, allows bursts, simpler math |
| std::set for namespace filter | std::unordered_set with custom hash | set is simpler, namespace count is tiny (< 100), O(log n) vs O(1) irrelevant at this scale |
| Inline PeerInfo fields | Separate TokenBucket class | YAGNI -- two fields and one method, not worth a class |

## Architecture Patterns

### Recommended Changes
```
db/config/config.h       # Add rate_limit_bytes_per_sec, rate_limit_burst, sync_namespaces fields
db/config/config.cpp     # Parse new fields, validate sync_namespaces (reuse validate_allowed_keys)
db/peer/peer_manager.h   # Add token bucket fields to PeerInfo, sync_namespaces_ set to PeerManager
db/peer/peer_manager.cpp # Rate check in on_peer_message, filter in Phase A, reload in reload_config
tests/config/test_config.cpp        # Config parsing tests for new fields
tests/peer/test_peer_manager.cpp    # Rate limiting + namespace filter integration tests
```

### Pattern 1: Token Bucket in PeerInfo
**What:** Two fields (`bucket_tokens` and `bucket_last_refill`) in PeerInfo, with a helper function to consume tokens.
**When to use:** Every Data or Delete message received.
**Example:**
```cpp
// In PeerInfo struct:
uint64_t bucket_tokens = 0;      // Current token count (bytes)
uint64_t bucket_last_refill = 0; // steady_clock::now() as uint64_t ms

// Helper function (standalone, not a method -- PeerInfo is a struct):
// Returns true if tokens consumed, false if rate exceeded.
bool try_consume_tokens(PeerInfo& peer, uint64_t bytes,
                        uint64_t rate_bytes_per_sec, uint64_t burst_bytes) {
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Refill tokens based on elapsed time
    uint64_t elapsed_ms = now_ms - peer.bucket_last_refill;
    uint64_t refill = (rate_bytes_per_sec * elapsed_ms) / 1000;
    peer.bucket_tokens = std::min(peer.bucket_tokens + refill, burst_bytes);
    peer.bucket_last_refill = now_ms;

    if (bytes > peer.bucket_tokens) {
        return false;  // Rate exceeded
    }
    peer.bucket_tokens -= bytes;
    return true;
}
```

### Pattern 2: Namespace Filter at Phase A
**What:** Filter `engine_.list_namespaces()` result through `sync_namespaces_` set before sending to peer.
**When to use:** In both `run_sync_with_peer()` (initiator) and `handle_sync_as_responder()` (responder).
**Example:**
```cpp
// Phase A: Send our data (filtered)
auto our_namespaces = engine_.list_namespaces();
if (!sync_namespaces_.empty()) {
    std::erase_if(our_namespaces, [this](const storage::NamespaceInfo& ns) {
        return sync_namespaces_.find(ns.namespace_id) == sync_namespaces_.end();
    });
}
auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
```

### Pattern 3: Rate Check as Step 0 in on_peer_message
**What:** Before any Data/Delete processing (before decode_blob), check rate limit.
**When to use:** At the top of Data and Delete handlers in on_peer_message().
**Example:**
```cpp
if (type == wire::TransportMsgType_Data || type == wire::TransportMsgType_Delete) {
    auto* peer = find_peer(conn);
    if (peer && rate_limit_bytes_per_sec_ > 0) {
        if (!try_consume_tokens(*peer, payload.size(),
                                 rate_limit_bytes_per_sec_, rate_limit_burst_)) {
            ++metrics_.rate_limited;
            spdlog::warn("rate limit exceeded by {}, disconnecting",
                         conn->remote_address());
            asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
            return;
        }
    }
}
```

### Pattern 4: Namespace Filter at Ingest
**What:** Before processing Data/Delete messages, check if the blob's namespace is in sync_namespaces.
**When to use:** After decoding the blob but before calling engine_.ingest() or engine_.delete_blob().
**Example:**
```cpp
// After decode_blob, before engine_.ingest():
if (!sync_namespaces_.empty() &&
    sync_namespaces_.find(blob.namespace_id) == sync_namespaces_.end()) {
    // Silent drop -- this node doesn't handle this namespace
    return;
}
```

### Pattern 5: SIGHUP Reload for New Fields
**What:** Extend reload_config() to update rate limit params and sync_namespaces set.
**When to use:** In reload_config(), after existing allowed_keys reload.
**Example:**
```cpp
// In reload_config(), after ACL reload:
// Reload rate limit params
rate_limit_bytes_per_sec_ = new_cfg.rate_limit_bytes_per_sec;
rate_limit_burst_ = new_cfg.rate_limit_burst;
spdlog::info("config reload: rate_limit={}B/s burst={}B",
             rate_limit_bytes_per_sec_, rate_limit_burst_);

// Reload sync_namespaces
sync_namespaces_.clear();
for (const auto& hex : new_cfg.sync_namespaces) {
    std::array<uint8_t, 32> ns{};
    // hex_to_bytes conversion...
    sync_namespaces_.insert(ns);
}
if (sync_namespaces_.empty()) {
    spdlog::info("config reload: sync_namespaces=all (unrestricted)");
} else {
    spdlog::info("config reload: sync_namespaces={} namespaces", sync_namespaces_.size());
}
```

### Anti-Patterns to Avoid
- **Separate TokenBucket class:** YAGNI. Two uint64_t fields and one function. A class adds abstraction for no gain.
- **Rate limiting sync BlobTransfer messages:** PROT-01 explicitly excludes sync traffic. BlobTransfer is cooperative sync, not peer-initiated writes.
- **Per-namespace rate limits:** Out of scope (deferred as relay/app concern). Per-connection only.
- **Backpressure queuing:** CONTEXT.md explicitly rejects this. Disconnect immediately, don't slow.
- **Persistent rate limit state:** CONTEXT.md deferred. Connection-scoped by design; PeerInfo recreation on reconnect resets bucket.
- **Filtering at blob transfer time (Phase C):** PROT-05 requires filtering at Phase A (namespace list assembly). By the time Phase C runs, filtered namespaces should never appear.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Hex string to bytes | Custom parser | Reuse pattern from validate_allowed_keys + simple loop | validate_allowed_keys already validates; conversion is a 5-line loop |
| Namespace set membership | Linear scan | std::set::find() | O(log n) guaranteed, already used for subscribed_namespaces |
| Monotonic time | std::time / wall clock | std::chrono::steady_clock | Wall clock can jump backward (NTP, DST). steady_clock is monotonic. Already used in timer loops. |
| Token refill math | Float-based rate | Integer millisecond math | Avoid floating point drift. `(rate * elapsed_ms) / 1000` is exact for reasonable values. |

**Key insight:** Every component needed already exists in the codebase. The token bucket is trivial math. The namespace filter is a set lookup. The config pattern is copy-paste from allowed_keys. No new abstractions needed.

## Common Pitfalls

### Pitfall 1: Token Bucket Overflow on Long Idle
**What goes wrong:** If a peer is idle for hours, `elapsed_ms * rate_bytes_per_sec` can overflow uint64_t.
**Why it happens:** Multiplication before division. 1 hour = 3,600,000ms; at 10MB/s that's 36 trillion, well within uint64_t, but if rate is very large or idle time is very long, overflow is possible.
**How to avoid:** Cap refill at burst_bytes. The `std::min(tokens + refill, burst)` already handles this since burst is the maximum. But also cap elapsed_ms to prevent intermediate overflow: `elapsed_ms = std::min(elapsed_ms, burst_bytes * 1000 / rate_bytes_per_sec + 1)`.
**Warning signs:** Tokens exceeding burst after long idle (should never happen with min() cap).

### Pitfall 2: Rate Check on Wrong Message Types
**What goes wrong:** Accidentally rate-limiting sync BlobTransfer messages, causing sync failures.
**Why it happens:** Both Data and BlobTransfer carry blob payloads. Easy to check "any blob payload" instead of "Data or Delete type only."
**How to avoid:** Rate check ONLY on TransportMsgType_Data and TransportMsgType_Delete. All sync messages (SyncRequest, SyncAccept, NamespaceList, HashList, BlobRequest, BlobTransfer, SyncComplete) are excluded. The check goes at the top of the Data and Delete handlers, not in a generic "all messages" path.
**Warning signs:** Sync failures or timeouts after enabling rate limiting.

### Pitfall 3: Namespace Filter Inconsistency Between Sync and Ingest
**What goes wrong:** Filtering at sync Phase A but not at Data/Delete ingest allows a peer to write directly to a namespace the operator wants to exclude.
**Why it happens:** PROT-05 specifically mentions sync Phase A, and a naive reading might skip ingest filtering.
**How to avoid:** CONTEXT.md explicitly states: "Namespace filter applies to both sync Phase A AND ingest (Data/Delete messages)." Filter at both points.
**Warning signs:** Blobs appearing in filtered namespaces via direct Data messages.

### Pitfall 4: Rate Limit Reload Affecting Existing Buckets
**What goes wrong:** On SIGHUP, updating rate_limit_bytes_per_sec but not resetting existing PeerInfo bucket_tokens. If rate decreases, peers may have accumulated tokens above the new burst limit.
**Why it happens:** PeerInfo tokens are per-connection state, but rate/burst are global config.
**How to avoid:** On reload, the new rate/burst apply to the next `try_consume_tokens` call. The `std::min(tokens + refill, burst)` automatically caps to the new burst. No need to iterate peers and reset -- the cap handles it naturally.
**Warning signs:** None, if the min() cap is correct.

### Pitfall 5: Namespace Filter Bytes vs Hex Confusion
**What goes wrong:** Comparing hex strings to binary namespace_id arrays, or forgetting to convert.
**Why it happens:** Config stores hex strings (64 chars), PeerInfo/engine uses std::array<uint8_t, 32>.
**How to avoid:** Convert hex to bytes once at config load/reload time. Store the filter as `std::set<std::array<uint8_t, 32>>`. Never compare hex strings at runtime.
**Warning signs:** Filter not matching any namespaces despite correct config.

### Pitfall 6: Forgetting to Initialize bucket_last_refill
**What goes wrong:** If bucket_last_refill starts at 0, the first refill computes elapsed_ms as the entire time since epoch, granting a massive burst.
**Why it happens:** PeerInfo default-constructs with zeros.
**How to avoid:** Initialize bucket_last_refill in on_peer_connected when the PeerInfo is created, using steady_clock::now(). Or initialize bucket_tokens = burst and bucket_last_refill = now in the constructor/setup.
**Warning signs:** First message from any peer always passes rate limit even if massive.

## Code Examples

### Config Fields
```cpp
// In Config struct (config.h):
uint64_t rate_limit_bytes_per_sec = 0;        // 0 = disabled (no rate limiting)
uint64_t rate_limit_burst = 0;                // Burst capacity in bytes
std::vector<std::string> sync_namespaces;     // Hex namespace hashes (64 chars each)
```

### Config Parsing
```cpp
// In load_config() (config.cpp):
cfg.rate_limit_bytes_per_sec = j.value("rate_limit_bytes_per_sec", cfg.rate_limit_bytes_per_sec);
cfg.rate_limit_burst = j.value("rate_limit_burst", cfg.rate_limit_burst);

if (j.contains("sync_namespaces") && j["sync_namespaces"].is_array()) {
    for (const auto& ns : j["sync_namespaces"]) {
        if (ns.is_string()) {
            cfg.sync_namespaces.push_back(ns.get<std::string>());
        }
    }
    validate_allowed_keys(cfg.sync_namespaces);  // Same format: 64-char hex
}
```

### PeerInfo Token Bucket Fields
```cpp
// In PeerInfo struct (peer_manager.h):
uint64_t bucket_tokens = 0;        // Available throughput tokens (bytes)
uint64_t bucket_last_refill = 0;   // steady_clock milliseconds since epoch
```

### PeerManager Namespace Filter Member
```cpp
// In PeerManager private (peer_manager.h):
std::set<std::array<uint8_t, 32>> sync_namespaces_;  // Empty = all
uint64_t rate_limit_bytes_per_sec_ = 0;               // 0 = disabled
uint64_t rate_limit_burst_ = 0;
```

### Hex to Bytes Conversion
```cpp
// Reusable for both sync_namespaces and any future hex-to-bytes needs:
std::array<uint8_t, 32> hex_to_namespace(const std::string& hex) {
    std::array<uint8_t, 32> result{};
    for (size_t i = 0; i < 32; ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        result[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }
    return result;
}
```

### Token Bucket Initialization in on_peer_connected
```cpp
// When creating PeerInfo for new connection:
PeerInfo info;
info.connection = conn;
info.address = conn->remote_address();
// ... existing fields ...
info.bucket_tokens = rate_limit_burst_;  // Start with full burst capacity
info.bucket_last_refill = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
```

### Default Values Recommendation
```cpp
// Recommended defaults (Claude's discretion):
// rate_limit_bytes_per_sec = 0 (disabled by default -- backward compatible)
// rate_limit_burst = 0 (disabled by default)
//
// When enabled, reasonable production values:
// rate_limit_bytes_per_sec = 1048576 (1 MiB/s -- allows ~10 full 100KiB blobs/sec)
// rate_limit_burst = 10485760 (10 MiB -- allows burst of ~1 large blob)
//
// The 0-means-disabled pattern mirrors max_storage_bytes = 0 (unlimited).
// This is backward compatible: existing nodes with no config change see no rate limiting.
```

### Metrics Semantics Recommendation
```cpp
// metrics_.rate_limited should count disconnection events (not individual messages).
// Rationale: each exceed triggers exactly one disconnect, so the counter
// maps 1:1 to "how many peers were cut for abuse." This is the actionable metric.
// The existing stub counter already exists at 0 in NodeMetrics.
```

### Log Verbosity Recommendation
```cpp
// Rate-limit disconnect should log at WARN level (same as strike-threshold disconnect):
spdlog::warn("rate limit exceeded by peer {} ({} bytes, limit {}B/s), disconnecting",
             conn->remote_address(), payload.size(), rate_limit_bytes_per_sec_);
// This matches the existing pattern: record_strike logs at WARN, disconnect at WARN.
```

### Reject Behavior Recommendation for Filtered Namespace
```cpp
// Silent drop (no error response) for blobs to filtered namespaces:
// - Consistent with "this node doesn't handle that namespace"
// - No wire message exists for "namespace rejected" (adding one is out of scope)
// - Peer can reconnect and try another node
// - No strike recorded (not a validation failure, just an operator policy)
// - Log at DEBUG level (normal operation for topology-aware networks)
spdlog::debug("dropping blob for filtered namespace from {}",
              conn->remote_address());
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| No rate limiting (open ingest) | Token bucket per connection | Phase 18 | Prevents write flooding abuse |
| Replicate all namespaces | Configurable sync_namespaces filter | Phase 18 | Enables topology control |

## Open Questions

1. **Namespace filter at ingest: should it count as a rejection metric?**
   - What we know: CONTEXT.md says silent drop. metrics_.rejections currently tracks validation failures.
   - What's unclear: Whether filtered-namespace drops should increment rejections or a separate counter.
   - Recommendation: Do NOT count as rejections. Rejections are for invalid blobs. Filtered namespace is valid-but-unwanted. If operators need visibility, the DEBUG log is sufficient. Adding a new counter is YAGNI unless operators ask for it.

2. **Token bucket burst initialization: full burst or zero?**
   - What we know: PeerInfo defaults to 0 for all fields. A new connection should be able to send immediately.
   - What's unclear: Whether starting at 0 tokens (requiring time to accumulate) or full burst is better.
   - Recommendation: Initialize at full burst capacity. A legitimate peer connecting should be able to send immediately. Starting at 0 would unfairly penalize new connections. Set bucket_tokens = burst in on_peer_connected.

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/config/config.h`, `db/config/config.cpp` -- Config struct and parsing patterns
- Codebase analysis: `db/peer/peer_manager.h`, `db/peer/peer_manager.cpp` -- PeerInfo struct, on_peer_message routing, reload_config(), record_strike(), sync Phase A
- Codebase analysis: `db/sync/sync_protocol.h`, `db/sync/sync_protocol.cpp` -- SyncProtocol encoding, namespace list format
- Codebase analysis: `db/engine/engine.h` -- BlobEngine interface (list_namespaces, ingest, delete_blob)
- Codebase analysis: `db/wire/transport_generated.h` -- TransportMsgType enum (Data=8, Delete=18, BlobTransfer=14)
- Codebase analysis: `db/net/framing.h` -- MAX_BLOB_DATA_SIZE = 100 MiB
- Codebase analysis: `tests/config/test_config.cpp` -- Test patterns for config fields
- Phase 18 CONTEXT.md -- User decisions and locked constraints

### Secondary (MEDIUM confidence)
- Token bucket algorithm: well-known rate limiting algorithm, widely documented. Integer millisecond variant avoids floating point. HIGH confidence in the algorithm itself.

### Tertiary (LOW confidence)
- None. All findings are from direct codebase analysis and well-established algorithms.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all patterns exist in codebase
- Architecture: HIGH -- direct extension of existing PeerInfo, Config, on_peer_message, reload_config patterns
- Pitfalls: HIGH -- identified from direct code analysis (overflow, message type confusion, initialization)

**Research date:** 2026-03-11
**Valid until:** 2026-04-11 (stable -- no external dependencies to go stale)
