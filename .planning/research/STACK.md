# Technology Stack: v0.7.0 Production Readiness

**Project:** chromatindb v0.7.0
**Researched:** 2026-03-16
**Confidence:** HIGH (no new dependencies, all techniques use existing deps already in the build)

## Executive Summary

No new library dependencies are needed for v0.7.0. All three major features -- sync resumption, namespace quotas, and large blob crypto throughput optimization -- are achievable with the existing dependency set. The stack changes are purely internal: new libmdbx sub-databases for cursor/quota state, `asio::thread_pool` for crypto offload (already bundled in Standalone Asio 1.38.0), and the liboqs incremental SHA3-256 API (`OQS_SHA3_sha3_256_inc_*`) for streaming hash computation.

This is a deliberate zero-new-deps milestone. Every technique leverages APIs that already exist in the bundled libraries but are not yet used by chromatindb. The only build change is adding `max_maps` from 6 to 8 in the libmdbx configuration (two new sub-databases).

## Recommended Stack

### Sync Resumption (Per-Peer Cursor Persistence)

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| libmdbx sub-database "sync_cursors" | v0.13.11 (existing) | Persist per-peer sync cursor state | Same ACID guarantees as all other state. Atomic with blob writes. Cursor data is tiny (peer_id:32 + ns:32 -> seq:8 = 72 bytes per entry). Already have the env open, just `txn.create_map("sync_cursors")`. |
| nlohmann/json | 3.11.3 (existing) | NOT used for cursors | JSON files (like peers.json) work for infrequently-written data. Sync cursors update on every sync round -- must be in libmdbx for atomicity and crash safety. |

**Key: peer_id(32) + namespace_id(32) = 64 bytes.** Peer identity is the SHA3-256 hash of their ML-DSA-87 public key (their namespace). This is stable across reconnections and already exchanged during handshake (AuthPubkey message). No new wire protocol needed for identification.

**Value: last_synced_seq(8) = 8 bytes.** The seq_num of the latest blob we know the peer has for each namespace. On next sync, we can skip sending hashes for blobs with seq <= cursor, and the peer can do the same.

**Why libmdbx, not JSON file:**
- Sync cursors update every sync round (every 60 seconds per peer). JSON files create fsync storms.
- Must be crash-safe. If a cursor is written but the corresponding blob ingest is lost, we get gaps. Bundling cursor updates into the same write transaction as blob storage ensures atomicity.
- libmdbx sub-databases share the same mmap and transaction. Zero additional file descriptors, zero additional memory overhead.

**Why not a separate libmdbx env:**
- One env per process is the standard LMDB/libmdbx pattern. Multiple envs mean multiple mmap regions and independent transaction scopes. All chromatindb state belongs in one env.
- The existing `max_maps = 6` (5 sub-databases + 1 spare) just needs bumping to 8.

**Integration point:** `Storage::Impl` adds `sync_cursors_map` alongside the existing 5 maps. New methods: `set_sync_cursor(peer_id, ns, seq)`, `get_sync_cursor(peer_id, ns) -> optional<uint64_t>`, `clear_sync_cursors(peer_id)`. PeerManager calls these during sync orchestration.

### Namespace Quotas (Size/Count Enforcement)

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| libmdbx sub-database "ns_stats" | v0.13.11 (existing) | Track per-namespace blob count and total data bytes | Enables O(1) quota checks on ingest without scanning. Updated atomically with blob storage in the same write transaction. |
| nlohmann/json config | 3.11.3 (existing) | Quota limits in node config | `max_namespace_bytes` and `max_namespace_blobs` in config.json. Same hot-reload via SIGHUP as other config values. |

**Key: namespace_id(32) = 32 bytes.**
**Value: blob_count(8) + total_bytes(8) = 16 bytes.** Both as little-endian uint64.

**Why a dedicated sub-database, not computed on demand:**
- Computing namespace size requires scanning all blobs for that namespace (O(n) per ingest). With 10K+ blobs per namespace, this is too expensive for the hot path.
- The ns_stats table is a materialized aggregate, updated +1/+size on ingest and -1/-size on delete/expiry. O(1) lookup, O(1) update.
- Crash safety: ns_stats and blob storage are in the same write transaction. If the transaction commits, both are consistent. If it rolls back, neither changes.

**Why not an in-memory cache:**
- Must survive restarts. Rebuilding from a full scan on startup is O(total_blobs), which could be millions.
- In-memory cache diverges from disk if crash recovery truncates transactions.
- libmdbx read-transaction overhead is negligible (just an MVCC snapshot) -- no benefit from caching.

**Enforcement points:**
1. `BlobEngine::ingest()` -- Step 0c: after storage capacity check, before crypto. Query ns_stats for the blob's namespace. Reject if count >= max or bytes + blob_size > max.
2. `Storage::store_blob()` -- atomically increment ns_stats after blob insertion.
3. `Storage::delete_blob_data()` -- atomically decrement ns_stats.
4. `Storage::run_expiry_scan()` -- atomically decrement ns_stats for each expired blob.

**Config additions:** Two new optional fields in Config:
```json
{
  "max_namespace_bytes": 1073741824,
  "max_namespace_blobs": 100000
}
```
Zero means unlimited (default). These are node-level quotas applied uniformly to all namespaces on this node.

### Large Blob Crypto Throughput Optimization

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| `asio::thread_pool` | 1.38.0 (existing in Standalone Asio) | Offload ML-DSA-87 verification to worker threads | The bottleneck is serial crypto on the single io_context thread. ML-DSA-87 verify takes ~74us for small messages, but the signing input for 1 MiB blobs includes the full data -- making the internal SHA3 passes inside ML-DSA expensive. Offloading to a thread pool unblocks the io_context for I/O while crypto runs in parallel. |
| `OQS_SHA3_sha3_256_inc_*` | liboqs 0.15.0 (existing) | Incremental/streaming SHA3-256 hashing | Avoid the double-hash penalty: currently, `build_signing_input()` copies namespace+data+ttl+ts into a contiguous buffer (1 MiB allocation + copy), then ML-DSA internally hashes it again. Using incremental SHA3 eliminates the intermediate buffer entirely. |
| `asio::post()` + `asio::use_future` | 1.38.0 (existing) | Bridge between coroutine and thread pool | `co_await asio::post(pool, use_awaitable)` switches execution to the thread pool. After crypto completes, `co_await asio::post(io_context, use_awaitable)` returns to the io_context for storage/state updates. |

**Root cause analysis (from v0.6.0 benchmarks):**
- 1 MiB blob ingest: 15.3 blobs/sec, 96% CPU on sync verification
- The ingest pipeline for each blob: `build_signing_input()` (1 MiB memcpy + concat) -> `Signer::verify()` (ML-DSA-87 internally hashes the signing input via SHA3) -> `store_blob()` (SHA3-256 hash for content addressing + AEAD encrypt + mdbx write)
- All of this runs serially on the single io_context thread, blocking all I/O during crypto

**Optimization strategy (ranked by impact):**

**1. Thread pool offload (HIGH impact, MEDIUM effort):**
```
asio::thread_pool crypto_pool_{std::thread::hardware_concurrency()};
```
- During sync ingest, `co_await` post the verification to the pool
- The io_context thread continues processing I/O while crypto runs
- Storage writes still happen on the io_context thread (libmdbx is single-writer)
- Expected improvement: N-fold throughput increase where N = number of cores that can verify in parallel. On the benchmark Ryzen 5 5600U (6 cores), expect 4-5x improvement

**Why this is safe:** `Signer::verify()` is a pure function (static method, no mutable state). `build_signing_input()` creates a local buffer. Both can run concurrently without synchronization. Only the subsequent storage write needs to be serialized back to the io_context thread.

**2. Eliminate signing input buffer copy (MEDIUM impact, LOW effort):**
The current `build_signing_input()` allocates a `vector<uint8_t>` of namespace(32) + data(variable) + ttl(4) + timestamp(8). For 1 MiB blobs, this is a 1 MiB+ allocation and memcpy on every ingest. The ML-DSA-87 verify function then internally hashes this input.

Approach: Pre-hash the signing input using liboqs incremental SHA3-256:
```cpp
OQS_SHA3_sha3_256_inc_ctx ctx;
OQS_SHA3_sha3_256_inc_init(&ctx);
OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), 32);
OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());
OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_bytes, 4);
OQS_SHA3_sha3_256_inc_absorb(&ctx, timestamp_bytes, 8);
OQS_SHA3_sha3_256_inc_finalize(hash_output, &ctx);
OQS_SHA3_sha3_256_inc_ctx_release(&ctx);
```

Wait -- this does NOT work directly. ML-DSA-87 (Dilithium) does not support pre-hashed messages in its standard API. The `OQS_SIG_verify()` function takes the full message, not a hash. ML-DSA internally uses its own hash-then-sign construction.

**Revised approach:** The real win is eliminating the intermediate buffer allocation. Instead of building a contiguous vector then passing it to verify, pass the signing components directly. But since `OQS_SIG_verify()` takes a single contiguous buffer, we cannot avoid the concatenation step.

The actual fix here is simpler: **pre-allocate a reusable buffer** per verification context instead of allocating 1 MiB+ per blob. Use a thread-local `std::vector<uint8_t>` that grows to the max size and stays allocated:

```cpp
thread_local std::vector<uint8_t> signing_buf;
signing_buf.clear();
// ... append components (no allocation after first use)
```

This eliminates the allocation cost but not the memcpy. Combined with thread pool offload, the memcpy runs on a worker thread anyway.

**3. Pipeline verification with I/O (MEDIUM impact, HIGH effort -- defer):**
Instead of request-verify-store-request-verify-store, overlap: while blob N is being verified on the thread pool, blob N+1 is being received from the network. This requires restructuring the sync protocol to decouple receiving from ingesting. Defer to a future milestone unless the thread pool alone is insufficient.

**What NOT to do:**

| Anti-approach | Why Not |
|---------------|---------|
| Batch signature verification | liboqs has no `OQS_SIG_verify_batch()` API. ML-DSA-87 does not support batch verification at the algorithm level (unlike Ed25519 which has batch verify). Each signature must be verified independently. |
| Replace SHA3-256 with BLAKE3 | SHA3-256 is used inside ML-DSA-87 itself (the algorithm specification uses SHA3). Replacing the content-addressing hash does not help the bottleneck, which is inside signature verification. Also, mixing hash functions adds confusion. |
| Replace ML-DSA-87 with ML-DSA-65 | Reduces NIST security category from 5 to 3. The project explicitly chose Cat 5 for maximum PQ security. Performance is fixable; security downgrade is permanent. |
| Multi-threaded libmdbx writes | libmdbx supports only one concurrent write transaction. Multiple write threads would need serialization anyway. Keep all writes on the io_context thread. |
| OpenSSL for SHA3 acceleration | Explicit project constraint: no OpenSSL. liboqs SHA3 uses XKCP with AVX2/AVX512 optimizations when available. |

### Build/Configuration Changes

| Change | Current | New | Reason |
|--------|---------|-----|--------|
| `max_maps` in libmdbx | 6 | 8 | Two new sub-databases: sync_cursors, ns_stats |
| Config fields | (none) | `max_namespace_bytes`, `max_namespace_blobs` | Quota limits |
| Transport schema | 26 msg types | 26 msg types (no change) | Sync resumption uses NamespaceList seq_num comparison, no new wire messages needed |

**Why no new wire protocol messages for sync resumption:**
The NamespaceList message already includes `latest_seq_num` per namespace. The cursor tells us what seq the peer had last time. If their seq hasn't changed, skip that namespace entirely. If it has changed, proceed with hash-list diff as before -- but the hash list will be smaller because we only collect hashes with seq > cursor. This optimization is entirely local; the peer doesn't need to know about cursors.

### Dependencies NOT Added

| Considered | Why Not |
|------------|---------|
| Prometheus client library | Overkill for per-namespace stats. Node metrics already use SIGUSR1 + log line pattern. Quota stats can use the same mechanism. |
| `std::execution` (C++23 parallel STL) | Not widely available yet. GCC 14 support is partial. `asio::thread_pool` with `asio::post` is proven and already in the build. |
| LMDB (liblmdb) | Already using libmdbx which is LMDB's successor. No reason to add both. |
| RocksDB or LevelDB | Project is committed to libmdbx. Switching storage engines is out of scope. |
| External thread pool (Intel TBB, taskflow) | `asio::thread_pool` is sufficient and avoids a new dependency. Only need simple "post work, await result" pattern. |
| cppcoro | Abandoned library. `asio::thread_pool` provides the same functionality. |

## Detailed Integration Points

### 1. Storage Layer Changes (storage.h/cpp)

New sub-databases added to `Storage::Impl`:
```
sync_cursors: [peer_id:32][namespace:32] -> [seq_num_be:8]
ns_stats:     [namespace:32] -> [count_le:8][bytes_le:8]
```

New public methods on `Storage`:
```cpp
// Sync cursor persistence
void set_sync_cursor(std::span<const uint8_t, 32> peer_id,
                     std::span<const uint8_t, 32> ns,
                     uint64_t seq_num);
std::optional<uint64_t> get_sync_cursor(std::span<const uint8_t, 32> peer_id,
                                         std::span<const uint8_t, 32> ns);
void clear_sync_cursors(std::span<const uint8_t, 32> peer_id);

// Namespace quota stats
struct NamespaceStats { uint64_t blob_count; uint64_t total_bytes; };
NamespaceStats get_namespace_stats(std::span<const uint8_t, 32> ns);
```

The ns_stats updates are internal -- `store_blob()`, `delete_blob_data()`, and `run_expiry_scan()` update them atomically in their existing write transactions. No caller changes needed for the update path.

### 2. Engine Layer Changes (engine.h/cpp)

New rejection reason:
```cpp
enum class IngestError {
    // ... existing ...
    namespace_quota_exceeded  // New: per-namespace quota check
};
```

New constructor parameter or setter for quota config:
```cpp
BlobEngine(storage::Storage& store,
           uint64_t max_storage_bytes = 0,
           uint64_t max_namespace_bytes = 0,
           uint64_t max_namespace_blobs = 0);
```

### 3. PeerManager Changes (peer_manager.h/cpp)

For sync resumption:
```cpp
struct PeerInfo {
    // ... existing ...
    std::array<uint8_t, 32> peer_namespace_id{};  // From handshake AuthPubkey
};
```

In `run_sync_with_peer()`, before sending NamespaceList, check cursors:
```
for each namespace:
  cursor = storage.get_sync_cursor(peer_ns_id, ns)
  if cursor >= our_latest_seq for ns: skip namespace
  else: collect hashes with seq > cursor (optimization)
```

After successful sync round, update cursors:
```
for each namespace synced:
  storage.set_sync_cursor(peer_ns_id, ns, their_latest_seq)
```

For crypto offload:
```cpp
class PeerManager {
    // ... existing ...
    asio::thread_pool crypto_pool_;  // Sized to hardware_concurrency()
};
```

### 4. Config Changes (config.h/cpp)

```cpp
struct Config {
    // ... existing ...
    uint64_t max_namespace_bytes = 0;   // 0 = unlimited
    uint64_t max_namespace_blobs = 0;   // 0 = unlimited
};
```

Both are hot-reloadable via SIGHUP (same pattern as `max_storage_bytes`).

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Cursor storage | libmdbx sub-database | JSON file (peers.json style) | Cursors update every sync round (~60s). JSON fsync overhead adds latency. No atomicity with blob storage. |
| Cursor storage | libmdbx sub-database | In-memory only (rebuild on start) | Defeats the purpose. First sync after restart would be full sync, wasting bandwidth. |
| Quota tracking | libmdbx ns_stats table | Compute on demand | O(n) per ingest where n = namespace blob count. Unacceptable on hot path. |
| Quota tracking | libmdbx ns_stats table | In-memory cache | Must survive restart. Cache invalidation on crash recovery is error-prone. |
| Crypto offload | asio::thread_pool | std::async + std::future | Uncontrolled thread creation. No executor integration with Asio coroutines. |
| Crypto offload | asio::thread_pool | Manual std::thread pool | Reinventing what asio::thread_pool already provides. |
| Blob hash optimization | Thread-local reusable buffer | mmap the blob data directly | Storage layer already decrypts into a vector. Mmap doesn't help with the crypto pipeline. |

## Current Dependency Versions (Unchanged)

| Dependency | Version | Status |
|------------|---------|--------|
| liboqs | 0.15.0 | Current as of 2026-03 |
| libsodium (cmake wrapper) | master | Tracks upstream |
| FlatBuffers | 25.2.10 | Current |
| Catch2 | 3.7.1 | Current |
| spdlog | 1.15.1 | Current |
| nlohmann/json | 3.11.3 | Current |
| libmdbx | 0.13.11 | Current |
| Standalone Asio | 1.38.0 | Current |

No version bumps needed. All features use existing APIs within these versions.

## Installation

No changes to the install/build process. Same CMake FetchContent setup:
```bash
cmake -B build
cmake --build build
```

The only CMake change is the test target source list (test relocation into db/), which is a file organization change, not a dependency change.

## Confidence Assessment

| Area | Confidence | Reason |
|------|------------|--------|
| Sync cursor storage in libmdbx | HIGH | Already using 5 sub-databases with identical patterns. Adding a 6th is mechanical. |
| Namespace quota via ns_stats | HIGH | Standard materialized-aggregate pattern. Same write-transaction atomicity as delegation/tombstone indexes. |
| asio::thread_pool for crypto | HIGH | `thread_pool.hpp` confirmed present in Asio 1.38.0 bundled in the build. `asio::post()` with coroutines is documented and widely used. |
| Incremental SHA3-256 API | HIGH | Confirmed `OQS_SHA3_sha3_256_inc_*` functions exist in liboqs 0.15.0 headers. However, these cannot be used to pre-hash for ML-DSA verify (ML-DSA takes raw message). Use is limited to content-addressing hash optimization. |
| No new wire protocol needed | HIGH | NamespaceList already carries seq_num. Cursor optimization is local. Quota rejection uses existing StorageFull message type. |
| No batch verify for ML-DSA | HIGH | Confirmed: liboqs has no batch verify API. ML-DSA algorithm does not support it. Thread-level parallelism is the correct approach. |

## Sources

- [libmdbx GitHub -- sub-database support](https://github.com/erthink/libmdbx)
- [liboqs ML-DSA documentation](https://openquantumsafe.org/liboqs/algorithms/sig/ml-dsa.html)
- [liboqs SHA3 incremental API (source)](https://github.com/open-quantum-safe/liboqs/blob/f88e6237c53d481200f9bf80c7c5fe9cde5f6a74/src/common/sha3/xkcp_sha3.c)
- [liboqs OQS_SIG API](https://openquantumsafe.org/liboqs/api/oqssig.html)
- [Asio C++20 coroutines documentation](https://think-async.com/Asio/asio-1.22.0/doc/asio/overview/core/cpp20_coroutines.html)
- [Asio thread_pool reference](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/thread_pool.html)
- [Asio issue #1508 -- coroutines with thread pools](https://github.com/chriskohlhoff/asio/issues/1508)
- [PQMagic ML-DSA optimization research (Springer 2025)](https://link.springer.com/chapter/10.1007/978-3-032-01806-9_9)
- Codebase verification: `/home/mika/dev/chromatin-protocol/build/_deps/liboqs-src/src/common/sha3/sha3.h` (incremental SHA3 API confirmed)
- Codebase verification: `/home/mika/dev/chromatin-protocol/build/_deps/asio-src/include/asio/thread_pool.hpp` (thread_pool confirmed in bundled Asio)
