# Technology Stack: v0.4.0 Production Readiness

**Project:** chromatindb v0.4.0
**Researched:** 2026-03-08
**Confidence:** HIGH (all claims verified against source code and library headers in build/_deps/)

## Executive Summary

No new dependencies are needed. Every v0.4.0 feature is achievable with the existing stack. The key insight: chromatindb already has most of the infrastructure — graceful shutdown is coded, peer persistence is coded, libmdbx exposes storage stats via `mdbx_env_info_ex()`. The work is wiring existing primitives together and filling gaps.

The only structural additions are: a new `tombstone_map` mdbx sub-database (adds a fifth DBI, libmdbx default is 1024), a `NodeMetrics` struct with `std::atomic<uint64_t>` counters, and a per-connection token bucket using `std::chrono::steady_clock`.

## Stack Decision: Zero New Dependencies

| Feature | Approach | Why No New Lib |
|---------|----------|----------------|
| Global storage limits | `mdbx_env_info_ex()` — already in libmdbx | API exists, just not called yet |
| Disk-full reporting | New `IngestError` variant + existing protocol rejection path | FlatBuffers transport already has error message type |
| Persistent peer list | Already implemented — `peers.json` + nlohmann/json | `load_persisted_peers` / `save_persisted_peers` exist in peer_manager.cpp |
| Graceful shutdown | Already implemented — `Server::drain()` + `Connection::close_gracefully()` | Needs integration testing, not new code |
| Metrics/observability | `std::atomic<uint64_t>` counters + SIGUSR1/SIGINFO dump | No HTTP server needed; log-based export is sufficient for v0.4.0 |
| Rate limiting | Token bucket with `std::chrono::steady_clock` + `double tokens` per connection | 30 lines of code, no external dep |
| Tombstone index | New `tombstone_map` mdbx DBI — key `[ns:32][target_hash:32]` | libmdbx already handles 4 DBIs; adding a 5th is trivial |
| Namespace-scoped sync | Config field + filter in sync initiator loop | Pure code change in peer_manager.cpp |

## Existing Components: What Changes

### Storage Layer (storage.h / storage.cpp)

| Change | Detail |
|--------|--------|
| Add `tombstone_map` DBI | New `mdbx::map_handle tombstone_map{0}` in `Storage::Impl`. Key: `[namespace:32][target_hash:32]`. Value: empty (presence = tombstone exists). Replaces O(n) blob scan in `has_tombstone_for()` with O(log n) btree lookup. |
| Add `get_used_bytes()` | Calls `impl_->env.get_info()` which wraps `mdbx_env_info_ex()`. Returns `info.mi_last_pgno * info.mi_dxb_pagesize` as bytes used. Call without a transaction (reads environment state). |
| Update `store_blob` for tombstones | When storing a tombstone blob, also insert into `tombstone_map` at key `[namespace:32][target_hash:32]`. |
| Update `run_expiry_scan` | No change needed for tombstones — tombstones have TTL=0 (permanent), so expiry scan already skips them. |

**libmdbx storage stats API** (verified from `build/_deps/libmdbx-src/mdbx.h` and `mdbx.h++`):

```cpp
// C++ wrapper (mdbx.h++)
env::info info = impl_->env.get_info();
// Returns MDBX_envinfo with:
//   info.mi_last_pgno    -- last used page number (uint64_t)
//   info.mi_dxb_pagesize -- page size in bytes (uint32_t)
//
// Used bytes = mi_last_pgno * mi_dxb_pagesize
// This is an approximation; actual file size may be larger due to
// freed pages pending GC reclamation. For a limit check, this
// is the correct "high-water mark" approach.
uint64_t used_bytes = info.mi_last_pgno * info.mi_dxb_pagesize;
```

**Confidence: HIGH** — verified via `build/_deps/libmdbx-src/mdbx.h` lines 2808-2886 and `mdbx.h++` lines 5486-5495.

### Engine Layer (engine.h / engine.cpp)

| Change | Detail |
|--------|--------|
| Add `storage_full` IngestError | New variant: `IngestError::storage_full`. Checked in `ingest()` after structural checks but before namespace verification (cheaper than crypto). |
| Check storage limit in `ingest()` | If `storage_.get_used_bytes() >= max_storage_bytes_`, return `IngestResult::rejection(IngestError::storage_full, ...)`. |
| Inject `max_storage_bytes` | Passed in via constructor (from `Config::max_storage_bytes`). 0 = unlimited (default). |

**Step 0 order in `ingest()`:**

```
1. Structural checks (pubkey size, sig non-empty)       [existing]
2. Blob size check (> MAX_BLOB_DATA_SIZE)               [existing]
3. Storage limit check (used_bytes >= max)              [NEW — cheap uint comparison]
4. Namespace ownership (SHA3-256(pubkey) == ns)         [existing]
5. Signature verification (ML-DSA-87)                  [existing]
6. Tombstone check (has_tombstone_for)                  [existing, now O(1)]
7. store_blob                                           [existing]
```

This preserves the "cheapest first" validation ordering from the key decisions log.

### Config (config.h / config.cpp)

| Change | Detail |
|--------|--------|
| Add `max_storage_bytes` | `uint64_t max_storage_bytes = 0` — 0 means unlimited. JSON field: `"max_storage_bytes"`. Support suffixed strings in config parsing ("10GB", "500MB") for usability, or plain integer bytes. |
| Add `sync_namespaces` | `std::vector<std::string> sync_namespaces` — hex namespace IDs. Empty = sync all namespaces (default behavior preserved). |
| Rate limit config | `uint32_t rate_limit_bytes_per_sec = 0` — per-connection inbound rate limit. 0 = no limit. JSON field: `"rate_limit_bytes_per_sec"`. |

### PeerManager (peer_manager.h / peer_manager.cpp)

| Change | Detail |
|--------|--------|
| Token bucket per PeerInfo | Add `double rate_tokens = 0.0` and `std::chrono::steady_clock::time_point last_token_refill` to `PeerInfo`. Refill before each frame receive. |
| Namespace-scoped sync filter | In sync initiator: skip namespaces not in `config_.sync_namespaces` (when list is non-empty). One check per namespace before hash collection. |
| Disk-full peer notification | When ingest returns `storage_full`, send protocol-level error message to the writing peer instead of silently dropping. |
| Graceful shutdown hardening | Verify `PeerManager::stop()` calls `server_.stop()` and waits for in-flight coroutines to drain before returning. Review SIGTERM path in main.cpp. |

### NodeMetrics (new file: db/metrics/metrics.h)

```cpp
namespace chromatindb::metrics {

struct NodeMetrics {
    std::atomic<uint64_t> blobs_ingested{0};
    std::atomic<uint64_t> blobs_rejected{0};
    std::atomic<uint64_t> blobs_synced_sent{0};
    std::atomic<uint64_t> blobs_synced_received{0};
    std::atomic<uint64_t> peers_connected{0};
    std::atomic<uint64_t> peers_rejected{0};
    std::atomic<uint64_t> bytes_ingested{0};
    std::atomic<uint64_t> storage_bytes_used{0};  // sampled periodically
    std::atomic<uint64_t> sync_rounds_completed{0};
    std::atomic<uint64_t> rate_limit_drops{0};
};

} // namespace chromatindb::metrics
```

**Export strategy:** On SIGUSR1 (or SIGINFO on BSD), log all counters via spdlog at INFO level. This is sufficient for v0.4.0 — no HTTP server, no Prometheus push, no external dependency. Operator can parse logs or use spdlog's custom sink if desired.

Why not prometheus-cpp: The library adds civetweb (HTTP server), protobuf, and a thread pool. For a daemon that already exposes everything via its binary protocol, adding an HTTP endpoint is out of scope (PROJECT.md: "No HTTP/REST API"). The `std::atomic` approach has zero overhead.

### Rate Limiter Design

Token bucket per connection — implemented inline in `PeerInfo`, no separate class needed:

```cpp
// In PeerInfo
double rate_tokens = 0.0;
std::chrono::steady_clock::time_point last_token_refill{std::chrono::steady_clock::now()};

// Before accepting each frame (in recv_raw):
void refill_tokens(double rate_bytes_per_sec) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_token_refill).count();
    last_token_refill = now;
    rate_tokens = std::min(rate_bytes_per_sec,  // bucket capacity = 1 second of tokens
                           rate_tokens + elapsed * rate_bytes_per_sec);
}

bool consume_tokens(size_t frame_bytes) {
    if (rate_tokens < static_cast<double>(frame_bytes)) return false;  // drop
    rate_tokens -= static_cast<double>(frame_bytes);
    return true;
}
```

**Bucket capacity = 1 second of tokens.** This allows bursts up to the rate limit but prevents sustained abuse. Frames that fail the check are dropped and the connection is struck (reusing the existing strike system).

Since PeerManager runs on a single io_context thread, there is no concurrent access to PeerInfo — no atomics needed in the token bucket itself. `std::chrono::steady_clock` is lock-free on all Linux targets.

### Tombstone Index: New mdbx DBI

**Current:** `has_tombstone_for()` in storage.cpp performs an O(n) scan of all blobs in a namespace, decoding each FlatBuffer to check if it's a tombstone. Correctness over performance (per the original comment in storage.h line 116).

**New design:** Add a 5th mdbx DBI, `tombstone_map`:

```
Key:   [namespace:32][target_blob_hash:32] = 64 bytes
Value: empty (1-byte magic or zero-length value)
```

On tombstone store: insert into both `blobs_map` and `tombstone_map`.
On tombstone lookup: `txn.get(impl_->tombstone_map, to_slice(key), not_found)` — O(log n) btree.
On tombstone expiry: tombstones have TTL=0, so expiry scan never touches them. The tombstone_map entry persists forever alongside the tombstone blob in blobs_map (consistent).

**libmdbx DBI limit:** Default `MDBX_MAX_DBI = 32768`. Current code uses 4 DBIs (blobs, sequence, expiry, delegation). Adding tombstone = 5. Well within limits.

**Migration:** Existing deployments have tombstones stored only as blobs in `blobs_map`. On first start with v0.4.0, `tombstone_map` will be empty. The `has_tombstone_for` check should fall back to blob scan if tombstone_map lookup misses AND blob is very new (or just accept that historical tombstones without index entries are re-indexed on next write). Simplest approach: one-time migration scan at startup that populates `tombstone_map` from existing tombstone blobs. Runs once, then DBI is authoritative.

### Namespace-Scoped Sync

**Mechanism:** When `config_.sync_namespaces` is non-empty, the sync initiator skips namespaces not in the set. This is a filter at the `list_namespaces()` → namespace iteration loop in `run_sync_with_peer()`.

```cpp
// In sync initiator, before sending namespace list:
auto all_ns = engine_.list_namespaces();
std::vector<storage::NamespaceInfo> filtered;
if (config_.sync_namespaces.empty()) {
    filtered = all_ns;  // sync all (default)
} else {
    for (const auto& ns : all_ns) {
        if (sync_ns_set_.count(hex_encode(ns.namespace_id))) {
            filtered.push_back(ns);
        }
    }
}
// send filtered namespace list to peer
```

`sync_ns_set_` is a `std::unordered_set<std::string>` built from `config_.sync_namespaces` at startup. Same pattern as `allowed_set_` in ACL.

Note: namespace-scoped sync is one-sided — the local node only requests blobs for its configured namespaces. The peer still offers all of its namespaces. This is correct: the filter is "what do I want to replicate", not "what do I offer to others".

## Existing Components: Confirmed No Changes Needed

| Component | Current Value | Status |
|-----------|---------------|--------|
| libmdbx `MDBX_MAX_DBI` | 32768 default | Adding 5th DBI is trivial |
| libmdbx `mdbx_env_info_ex` | Present in v0.13.11 | API verified in build/_deps headers |
| FlatBuffers error message type | Present in transport.fbs | Disk-full rejection reuses existing error path |
| `Connection::close_gracefully()` | Present and tested | Graceful shutdown uses this |
| `Server::drain()` | Present in server.cpp | Server-level shutdown exists |
| spdlog | v1.15.1 | Metrics export via log is sufficient |
| nlohmann/json | v3.11.3 | Config extension for new fields |
| `PeerManager::save_persisted_peers()` | Present in peer_manager.cpp | Persistent peer list already coded |
| SIGTERM handling | Present via asio::signal_set in server.cpp | May need audit of shutdown sequence |

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Metrics export | `std::atomic` + SIGUSR1 log dump | prometheus-cpp | prometheus-cpp adds HTTP server (civetweb), protobuf, threads. Out of scope per PROJECT.md "No HTTP/REST API". |
| Metrics export | SIGUSR1 log dump | StatsD push | StatsD adds network dependency and UDP socket management. Not worth it for v0.4.0. |
| Rate limiter | Token bucket in PeerInfo (inline) | Separate RateLimiter class | No benefit for per-connection limiting on a single-threaded io_context. Inline is simpler. |
| Tombstone index | New mdbx DBI | Serialize tombstone set to disk (JSON) | JSON tombstone file requires full-load on startup and is slower to update. mdbx DBI is ACID, crash-safe, and consistent with existing storage design. |
| Storage stats | `mdbx_env_info_ex()` | `std::filesystem::space()` | `filesystem::space()` reports disk partition usage, not database usage. A 1 GiB node on a 1 TB disk would never hit its limit. Database-level accounting is correct. |
| Namespace-scoped sync | Config filter applied at initiator | Per-connection subscription negotiation | Per-connection negotiation is more flexible but adds protocol complexity (new message types). Config filter is sufficient and simpler. |
| Graceful shutdown | Audit existing `drain()` path | Rewrite shutdown sequence | Drain coroutine and `close_gracefully()` already exist. The gap is likely in main.cpp SIGTERM wiring, not the implementation. |

## What NOT to Add

| Technology | Why Not |
|------------|---------|
| prometheus-cpp | Adds HTTP server (civetweb), protobuf dependency. PROJECT.md explicitly prohibits HTTP/REST API. `std::atomic` counters with log dump is sufficient for v0.4.0. |
| StatsD client | External network dependency for observability. Over-engineering for a daemon that already logs structured data via spdlog. |
| Rate limiting library (e.g., mfycheng/ratelimiter) | Token bucket is 30 lines of code. External dependency for 30 lines is wasteful. |
| inotify / file watcher | SIGHUP hot-reload is already implemented. inotify adds complexity with no benefit. |
| LevelDB / RocksDB | libmdbx already handles the tombstone index. No reason to add a second storage engine. |
| Chunked transfer for storage accounting | Storage limit applies to stored data, not in-flight data. No protocol changes needed. |

## Integration Point Summary

```
storage.h / storage.cpp:
  + tombstone_map mdbx DBI (5th DBI, key=[ns:32][target_hash:32])
  + get_used_bytes() -> uint64_t  (calls env.get_info())
  + store_blob: also writes to tombstone_map for tombstone blobs
  + has_tombstone_for: replaced O(n) scan with O(log n) tombstone_map lookup
  + migrate_tombstone_index(): one-time startup scan to populate tombstone_map

engine.h / engine.cpp:
  + IngestError::storage_full
  + Storage limit check in ingest() before namespace verify (Step 3)
  + Constructor takes max_storage_bytes from Config

config.h / config.cpp:
  + max_storage_bytes: uint64_t (0 = unlimited)
  + rate_limit_bytes_per_sec: uint32_t (0 = no limit)
  + sync_namespaces: vector<string> (empty = sync all)

db/metrics/metrics.h (new file):
  + NodeMetrics struct with 10x std::atomic<uint64_t> counters
  + dump_metrics(spdlog::logger) function

peer_manager.h / peer_manager.cpp:
  + PeerInfo: add rate_tokens, last_token_refill for token bucket
  + Token bucket consume before each frame acceptance
  + NodeMetrics& injected, counters incremented at ingest/sync events
  + Namespace filter built from config_.sync_namespaces at startup
  + Disk-full: send error response to peer on IngestError::storage_full
  + SIGUSR1 handler: calls dump_metrics()
  + Audit stop() → server_.stop() → drain() sequence for SIGTERM

main.cpp:
  + Wire SIGTERM to PeerManager::stop() (if not already)
  + Instantiate NodeMetrics, pass to PeerManager
  + Wait for io_context to drain before exit
```

## Installation

No changes to build process. No new FetchContent declarations.

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Sources

- `build/_deps/libmdbx-src/mdbx.h` lines 2808-2886 — `MDBX_envinfo` struct with `mi_last_pgno` and `mi_dxb_pagesize` — **HIGH confidence**
- `build/_deps/libmdbx-src/mdbx.h++` lines 5486-5495 — `env::get_info()` C++ wrapper returning `MDBX_envinfo` — **HIGH confidence**
- `db/storage/storage.cpp` lines 591-635 — current O(n) `has_tombstone_for` implementation — **HIGH confidence** (source code)
- `db/storage/storage.cpp` lines 78-111 — existing 4 mdbx DBIs (blobs, sequence, expiry, delegation) — **HIGH confidence**
- `db/peer/peer_manager.cpp` — `load_persisted_peers`, `save_persisted_peers`, `update_persisted_peer` — **HIGH confidence** (source code)
- `db/net/server.h` lines 33, 65 — `Server::stop()` and `Server::drain()` exist — **HIGH confidence**
- `db/net/connection.h` lines 56-57 — `Connection::close_gracefully()` exists — **HIGH confidence**
- `db/engine/engine.h` — `IngestError` enum, current validation pipeline — **HIGH confidence**
- [github.com/DarkWanderer/metrics-cpp](https://github.com/DarkWanderer/metrics-cpp) — confirms `std::atomic` is sufficient for lock-free metrics at nanosecond overhead — **MEDIUM confidence** (third-party benchmark)
- [Token Bucket Algorithm — Medium](https://medium.com/@sahilbitsp/rate-limiting-algorithms-c-d185f942a7db) — confirms `chrono::steady_clock` pattern for token bucket — **MEDIUM confidence**

---
*Stack research for: chromatindb v0.4.0 — Production Readiness*
*Researched: 2026-03-08*
