# Technology Stack: v0.8.0 Protocol Scalability

**Project:** chromatindb v0.8.0
**Researched:** 2026-03-19
**Confidence:** HIGH

## Executive Summary

v0.8.0 addresses a fundamental protocol scaling flaw: the current sync protocol exchanges the full hash list for every namespace with changes (O(N) in total blobs, not differences). A namespace with 1M blobs forces 32 MB of hashes on the wire for a single new blob, and ~3.4M blobs hits MAX_FRAME_SIZE (110 MiB) and breaks the connection entirely. This is not a performance optimization -- it is a correctness issue at scale.

The recommended approach is **negentropy** -- a header-only C++ library implementing Range-Based Set Reconciliation (RBSR). Negentropy replaces the full hash list exchange with an O(differences) protocol requiring O(log N) round-trips. It is battle-tested in the Nostr ecosystem (strfry relay syncs 10M+ element datasets in production), MIT-licensed, header-only with a single external dependency (OpenSSL SHA-256) that is trivially replaceable with the existing SHA3-256 from liboqs.

For sync rate limiting, no new library is needed -- the existing token bucket rate limiter (v0.4.0) is extended to cover sync message types. For thread pool crypto offload, `asio::thread_pool` (already bundled in Standalone Asio 1.38.0) provides the offload mechanism, using the `dispatch`/`post` pattern to bridge between io_context coroutines and worker threads.

**One new dependency: negentropy (header-only, MIT). Zero new compiled dependencies. Zero version bumps.**

## Recommended Stack

### Set Reconciliation: negentropy (RBSR)

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| negentropy | latest master (header-only) | Replace O(N) hash list exchange with O(differences) set reconciliation | Battle-tested in Nostr (strfry, 10M+ elements). Header-only C++. Supports frame size limits. LMDB storage backend exists (adaptable to libmdbx). MIT license. |

**Why negentropy over alternatives:**

| Approach | Verdict | Reason |
|----------|---------|--------|
| **negentropy (RBSR)** | RECOMMENDED | O(log N) rounds, O(differences) bandwidth. Stateless (no per-connection tree state). DoS-resistant (adversarial data cannot force worst-case). Header-only. Production-proven at scale. |
| Merkle trees | REJECTED | Require rigid tree structure stored per-dataset. Copy-on-write snapshots needed for concurrent syncs. Adversarial data can construct deliberately unfavorable tree shapes. More complex to implement than RBSR for similar average-case performance. |
| IBLT (Invertible Bloom Lookup Tables) | REJECTED | Require advance estimate of difference count (reconciliation fails if exceeded). False-positive risks. Susceptible to targeted DoS. Available C++ implementations (gavinandresen/IBLT_Cplusplus) are unmaintained research code, not production-ready. |
| minisketch (PinSketch/BCH) | REJECTED | Maximum element size is 64 bits. Our blob hashes are 256 bits (SHA3-256). Would require hashing hashes, adding collision risk. Decoding is O(n^2). Requires advance capacity estimate. Does not scale beyond ~4096 differences. |
| Custom O(N) optimization | REJECTED | Could compress hash lists with delta encoding or bloom filter pre-check, but these are band-aids. The fundamental issue is exchanging all N hashes when only D << N are different. Only set reconciliation fixes this. |

**How negentropy works:**

1. Both sides sort their items by (timestamp, id). Items are 64-bit timestamp + 256-bit ID (our SHA3-256 content hashes).
2. Initiator sends fingerprints covering ranges of their sorted dataset.
3. Responder compares fingerprints. Matching ranges are skipped. Mismatched ranges are recursively split.
4. After O(log N) round-trips, both sides know exactly which IDs the other is missing.
5. Actual blob transfer follows (using existing one-blob-at-a-time protocol).

**Bandwidth comparison for 1M blobs, 10 differences:**
- Current protocol: 32 MB (all 1M hashes x 32 bytes)
- negentropy: ~50-100 KB (fingerprints for log16(1M) ~ 5 levels, then 10 IDs)

**negentropy C++ API overview:**

```cpp
#include "negentropy.h"
#include "negentropy/storage/Vector.h"

// Build sorted set of items
negentropy::storage::Vector storage;
for (auto& [hash, timestamp] : our_blobs) {
    storage.insert(timestamp, hash.data());  // 64-bit ts + 32-byte id
}
storage.seal();  // sorts and deduplicates

// Initiator side
Negentropy ne(storage, frameSizeLimit);
std::string msg = ne.initiate();
// send msg to peer...

// After receiving response:
std::vector<std::string> haveIds, needIds;
auto next = ne.reconcile(response, haveIds, needIds);
// haveIds = what we have that peer needs
// needIds = what peer has that we need
// if next.has_value(), send *next and repeat
```

**OpenSSL dependency replacement:**

negentropy uses OpenSSL SHA-256 in exactly one place: `Accumulator::getFingerprint()`, which calls `SHA256()` on a ~40-byte input (accumulator state + varint count). This is trivially replaceable:

```cpp
// Original (negentropy/types.h):
#include <openssl/sha.h>
SHA256(input_data, input_size, hash);

// Replacement using liboqs SHA3-256:
#include "db/crypto/hash.h"
auto digest = chromatindb::crypto::sha3_256(input_data, input_size);
```

The fingerprint only needs to be a collision-resistant hash of the accumulator state. SHA3-256 is strictly stronger than SHA-256 for this purpose. Since negentropy is header-only, we patch the single `#include <openssl/sha.h>` line and the one `SHA256()` call site. No functional change.

**Alternative: vendor negentropy types.h with the SHA3-256 patch applied.** Since it is MIT-licensed header-only code, vendoring is the clean approach. Place in `db/vendor/negentropy/` with the SHA-256 -> SHA3-256 substitution.

**Storage backend choice: Vector (not BTreeLMDB)**

negentropy offers four storage backends:
- **Vector**: In-memory sorted array. Built from seq_map scan. O(N) construction, O(1) fingerprint via linear scan of tree nodes.
- **BTreeMem**: In-memory B+tree. Supports insert/erase without re-sealing. Logarithmic fingerprint computation.
- **BTreeLMDB**: Persistent B+tree backed by LMDB. Same as BTreeMem but persisted.
- **SubRange**: Proxy for partial sync over a subset of the dataset.

**Use Vector because:**
1. chromatindb already has the definitive dataset in libmdbx. No need to maintain a parallel persistent index.
2. Vector is constructed per-sync-round from the existing `get_hashes_by_namespace()` call (which reads seq_map). This is already O(N) -- the same cost we pay today.
3. BTreeLMDB would require maintaining a second sorted index that must be kept in sync with the primary storage. This adds write amplification on every ingest and creates consistency risk.
4. Vector construction for 1M items is fast (sort 1M x 40-byte structs ~ 100ms). This runs once per sync initiation, not per round-trip.
5. BTreeMem/BTreeLMDB are useful when the dataset changes between reconciliation rounds. For chromatindb, the dataset is snapshotted at sync start -- mutations during sync are handled by the next round.

**Frame size limit integration:**

negentropy supports a `frameSizeLimit` constructor parameter. Set this to match chromatindb's MAX_FRAME_SIZE (110 MiB) or lower. When the reconciliation message would exceed the limit, negentropy truncates the response and marks remaining ranges for the next round-trip. This naturally bounds per-message wire overhead.

**Timestamp mapping:**

negentropy items require a 64-bit timestamp for sorting. chromatindb blobs already have a `timestamp` field (Unix seconds, set by the writer). Use this directly. For blobs with identical timestamps, negentropy sorts lexically by ID (the 32-byte content hash), which provides deterministic ordering.

### Sync Rate Limiting

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| Existing token bucket (PeerInfo) | v0.4.0 (existing) | Extend to cover sync message types | The rate limiter already exists for Data/Delete messages. Sync messages (SyncRequest, BlobTransfer, etc.) are currently explicitly exempted. Extending coverage is a code change, not a library change. |

**Current state:**
```cpp
// peer_manager.cpp line 436-437:
// Sync messages (BlobTransfer, SyncRequest, etc.) are never rate-checked.
if ((type == TransportMsgType_Data || type == TransportMsgType_Delete) &&
    rate_limit_bytes_per_sec_ > 0) {
```

**What needs to change:**

The issue is not just rate limiting sync _data_ (BlobTransfer payloads) -- it is rate limiting sync _initiation_. A malicious peer can repeatedly send SyncRequest, forcing the node to construct Vector storage, compute fingerprints, and send responses. This is a CPU and I/O amplification vector.

Two separate rate limits are needed:

1. **Sync initiation rate limit**: Max SyncRequest messages per peer per time window. A simple counter + cooldown (not token bucket). Example: max 1 SyncRequest per 30 seconds per peer. Exceeding triggers a strike.

2. **Sync data rate limit**: BlobTransfer messages during sync counted against the existing token bucket. This prevents a peer from triggering unlimited blob transfers via reconciliation.

**No new library needed.** The cooldown-based rate limiter is 10 lines of code in PeerInfo:
```cpp
struct PeerInfo {
    // ... existing ...
    uint64_t last_sync_request_time = 0;  // steady_clock ms
};
```

**Config additions:**
```json
{
    "sync_cooldown_seconds": 30
}
```

### Thread Pool Crypto Offload

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| `asio::thread_pool` | 1.38.0 (existing in Standalone Asio) | Offload ML-DSA-87 verify + SHA3-256 hash to worker threads | Event loop is single-threaded. ML-DSA-87 verify blocks all I/O during large blob processing. Thread pool enables parallel verification while I/O continues. |

**The pattern (dispatch-based executor switching):**

```cpp
// In a coroutine running on io_context:
auto executor = co_await asio::this_coro::executor;  // io_context executor

// Offload CPU-bound work to thread pool:
auto [valid, blob_hash] = co_await asio::co_spawn(
    crypto_pool_,
    [&blob]() -> asio::awaitable<std::pair<bool, Hash>> {
        auto hash = crypto::sha3_256(blob.data);
        bool valid = crypto::verify(blob);
        co_return {valid, hash};
    },
    asio::use_awaitable
);

// Execution resumes here on the crypto_pool_ thread.
// Post back to io_context for storage write:
co_await asio::post(executor, asio::use_awaitable);
// Now on io_context thread -- safe to call storage_.store_blob()
```

**Why `co_spawn` on thread_pool + `post` back, not `dispatch`:**

The `co_spawn(pool, ...)` pattern runs the entire lambda on a pool thread. When we `co_await` this from the io_context coroutine, the io_context coroutine suspends until the pool work completes. The io_context thread is free to process other I/O during this time. After the pool work completes, `asio::post(executor, use_awaitable)` explicitly switches execution back to the io_context thread.

**Why this is safe:**
- `crypto::sha3_256()` is a pure function (no mutable state).
- `crypto::verify()` uses a `thread_local` OQS_SIG context (from v0.7.0 optimization).
- `storage_.store_blob()` is NOT thread-safe (single libmdbx write transaction) -- must run on io_context thread.
- The `co_await` suspension point is explicit -- no accidental interleaving.

**Thread pool sizing:**

```cpp
asio::thread_pool crypto_pool_{
    std::max(1u, std::thread::hardware_concurrency() - 1)
};
```

Reserve one core for the io_context thread. On a 6-core Ryzen 5 5600U (benchmark machine), this gives 5 worker threads for parallel verification.

**Expected improvement:**
- Current: 15 blobs/sec at 1 MiB (single-threaded, v0.7.0 serial optimizations applied)
- Target: 60-75 blobs/sec (5 parallel verifications, limited by storage write serialization)
- Actual improvement depends on verification time vs. storage write time ratio

**Integration point:** `BlobEngine::ingest()` or `SyncProtocol::ingest_blobs()`. The verification step (SHA3-256 hash + ML-DSA-87 verify) is extracted into a pure function and posted to the crypto pool. The storage write remains on the io_context thread.

### Build/Configuration Changes

| Change | Current | New | Reason |
|--------|---------|-----|--------|
| New dependency | (none) | negentropy (header-only, vendored) | Set reconciliation protocol |
| New vendored dir | (none) | `db/vendor/negentropy/` | Vendored with SHA-256 -> SHA3-256 patch |
| Wire protocol | SyncRequest + NamespaceList + HashList + BlobTransfer | SyncRequest + Reconcile + BlobRequest + BlobTransfer | New Reconcile message type replaces NamespaceList + HashList |
| Config fields | (none) | `sync_cooldown_seconds` | Sync initiation rate limit |
| Max frame integration | MAX_FRAME_SIZE = 110 MiB | Same, passed to negentropy constructor | Bounds reconciliation message size |

**Wire protocol changes:**

The current sync protocol uses:
- `SyncRequest` (initiator -> responder: "let's sync")
- `SyncAccept` (responder -> initiator: "ready")
- `NamespaceList` (both sides: "here are my namespaces + seq_nums")
- `HashList` (both sides: "here are all hashes for namespace X")
- `BlobRequest` (both sides: "send me these hashes")
- `BlobTransfer` (both sides: "here's the blob")
- `SyncComplete` (both sides: "done with my side")

The new protocol replaces NamespaceList + HashList with negentropy reconciliation messages:
- `SyncRequest` (unchanged)
- `SyncAccept` (unchanged)
- `Reconcile` (new: both sides, carries negentropy wire protocol messages per namespace)
- `BlobRequest` (unchanged)
- `BlobTransfer` (unchanged)
- `SyncComplete` (unchanged)

The negentropy protocol is namespace-scoped: one reconciliation per namespace. The outer protocol iterates namespaces (using cursor-based seq_num comparison from v0.7.0 to skip unchanged namespaces), then runs negentropy reconciliation for each namespace that has changes.

### Dependencies NOT Added

| Considered | Why Not |
|------------|---------|
| OpenSSL | negentropy's only dependency. Replaced with existing SHA3-256 from liboqs. Project constraint: no OpenSSL. |
| LMDB (liblmdb) | negentropy has a BTreeLMDB backend using LMDB. Not needed -- using Vector backend with data from libmdbx. Would add a second memory-mapped database engine. |
| minisketch (bitcoin-core) | 64-bit element limit. Our hashes are 256-bit. Requires capacity estimation. Does not scale beyond ~4096 differences. |
| Any IBLT library | Requires difference count estimation. Failure mode when estimate is wrong. Research-quality C++ implementations only. |
| External Merkle tree library | No standard C++ Merkle tree library exists for set reconciliation. Would need custom implementation. RBSR is simpler and more flexible. |
| Intel TBB / taskflow | `asio::thread_pool` is sufficient for "post work, await result" pattern. Adding a parallel framework dependency for one use case is overkill. |
| Boost | `asio::thread_pool` + `asio::post` provides everything needed without pulling in Boost. |

## Detailed Integration Points

### 1. Negentropy Vendoring and Patch

**Directory structure:**
```
db/vendor/negentropy/
  negentropy.h              -- main header (patched)
  negentropy/
    types.h                 -- patched: SHA-256 -> SHA3-256
    encoding.h              -- unmodified
    storage/
      base.h                -- unmodified
      Vector.h              -- unmodified (primary backend)
```

**Patch scope (types.h only):**

1. Remove `#include <openssl/sha.h>`
2. Add `#include "db/crypto/hash.h"`
3. Replace `SHA256(...)` call in `Accumulator::getFingerprint()`:

```cpp
// Before:
unsigned char hash[SHA256_DIGEST_LENGTH];
SHA256(reinterpret_cast<unsigned char*>(input.data()), input.size(), hash);

// After:
auto digest = chromatindb::crypto::sha3_256(
    reinterpret_cast<const uint8_t*>(input.data()), input.size());
```

4. Copy first `FINGERPRINT_SIZE` (16) bytes from the 32-byte SHA3-256 digest.

This is a ~5-line diff. The fingerprint only needs to be a collision-resistant digest of the accumulator state; SHA3-256 truncated to 16 bytes provides 128-bit collision resistance, matching the original SHA-256 truncation.

**Build integration:**

```cmake
# In db/CMakeLists.txt:
target_include_directories(chromatindb_lib PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
```

No new FetchContent. No new compiled sources. Header-only.

### 2. Sync Protocol Restructure

**New message type in transport.fbs:**
```flatbuffers
// Add to TransportMsgType enum:
Reconcile = 27,  // negentropy reconciliation message
```

**Reconcile message wire format:**
```
[namespace_id:32][payload_length:u32BE][negentropy_payload:variable]
```

Multiple Reconcile messages may be sent per sync round (one per namespace needing reconciliation). The negentropy payload is opaque bytes produced by `Negentropy::initiate()` or `Negentropy::reconcile()`.

**Sync flow (initiator):**
```
1. Send SyncRequest
2. Receive SyncAccept
3. For each namespace (cursor check: skip if seq unchanged):
   a. Build negentropy::storage::Vector from get_hashes_by_namespace()
   b. Create Negentropy(storage, frameSizeLimit)
   c. Send Reconcile(namespace, ne.initiate())
   d. Receive Reconcile(namespace, response)
   e. Call ne.reconcile(response, haveIds, needIds)
   f. If not done, goto (c) with next message
   g. haveIds -> BlobTransfer (send blobs peer needs)
   h. needIds -> BlobRequest (request blobs we need)
4. Send SyncComplete
```

**Round-trip analysis:**
- For well-synced peers (few differences): 1-2 round-trips per namespace
- For divergent peers (many differences): 3-4 round-trips per namespace
- For 1M blobs with 10 differences: ~3 round-trips vs. 1 round-trip (current) BUT current round-trip sends 32 MB while negentropy sends ~100 KB total

### 3. Thread Pool Integration in PeerManager

```cpp
class PeerManager {
    // ... existing ...
    asio::thread_pool crypto_pool_;  // New: sized to hardware_concurrency() - 1
};
```

**Constructor initialization:**
```cpp
PeerManager::PeerManager(...)
    : // ... existing initializers ...
    , crypto_pool_(std::max(1u, std::thread::hardware_concurrency() - 1))
{ }
```

**Shutdown:**
```cpp
void PeerManager::stop() {
    // ... existing stop logic ...
    crypto_pool_.join();  // Wait for in-flight crypto to complete
}
```

### 4. Config Changes

```cpp
struct Config {
    // ... existing ...
    uint32_t sync_cooldown_seconds = 30;   // Min seconds between sync requests per peer
};
```

Hot-reloadable via SIGHUP (same pattern as other config values).

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Set reconciliation | negentropy (RBSR) | Merkle tree | Rigid structure, adversarial worst-case, requires per-connection state snapshots |
| Set reconciliation | negentropy (RBSR) | IBLT | Requires capacity estimation, failure when exceeded, no production C++ library |
| Set reconciliation | negentropy (RBSR) | minisketch | 64-bit element limit (our hashes are 256-bit), O(n^2) decode |
| Set reconciliation | negentropy (RBSR) | Custom bloom filter pre-check | Band-aid. Still O(N) for false-positive resolution. Does not fix the fundamental issue. |
| negentropy backend | Vector (in-memory) | BTreeLMDB (persistent) | Would duplicate data already in libmdbx. Write amplification on every ingest. Consistency risk. |
| negentropy hash | SHA3-256 (from liboqs) | Keep SHA-256 (add OpenSSL) | Project constraint: no OpenSSL. SHA3-256 already available. |
| Sync rate limit | Counter + cooldown | Token bucket for sync messages | Token bucket is for throughput control. Sync initiation needs frequency control. Simpler mechanism is correct. |
| Crypto offload | asio::thread_pool | std::jthread + std::future | No executor integration. Cannot co_await futures natively in Asio coroutines. |
| Crypto offload | asio::thread_pool | Dedicated thread with lock-free queue | Over-engineered. asio::thread_pool + post/co_spawn provides the same with less code and proven correctness. |

## Current Dependency Versions

| Dependency | Version | Status | Changed? |
|------------|---------|--------|----------|
| liboqs | 0.15.0 | Current | No |
| libsodium (cmake wrapper) | master | Tracks upstream | No |
| FlatBuffers | 25.2.10 | Current | No |
| Catch2 | 3.7.1 | Current | No |
| spdlog | 1.15.1 | Current | No |
| nlohmann/json | 3.11.3 | Current | No |
| libmdbx | 0.13.11 | Current | No |
| Standalone Asio | 1.38.0 | Current | No |
| **negentropy** | **latest master** | **NEW (header-only, vendored)** | **Yes** |

## Confidence Assessment

| Area | Confidence | Reason |
|------|------------|--------|
| negentropy suitability | HIGH | Production-proven in strfry (Nostr), 10M+ element datasets. C++ header-only. MIT license. RBSR algorithm published and peer-reviewed (arXiv 2212.13567). |
| OpenSSL removal from negentropy | HIGH | Single SHA256() call in types.h. Trivial replacement with existing sha3_256(). Fingerprint only needs collision resistance; SHA3-256 is stronger. |
| Vector backend choice | HIGH | chromatindb owns definitive data in libmdbx. Vector is reconstructed per-sync from existing seq_map scan. No parallel index to maintain. |
| Sync rate limiting approach | HIGH | Counter + cooldown is standard for frequency control. Existing token bucket handles throughput. Two concerns, two mechanisms. |
| Thread pool crypto offload | HIGH | `asio::thread_pool` confirmed in Asio 1.38.0. `co_spawn` + `post` pattern documented in Asio. ML-DSA-87 verify is a pure function safe for concurrent execution. |
| Wire protocol changes | MEDIUM | New Reconcile message type required. Namespace-scoped reconciliation adds per-namespace round-trips. Need careful timeout handling for multi-round reconciliation. |

## Sources

- [negentropy GitHub repository](https://github.com/hoytech/negentropy) -- C++ header-only RBSR implementation, MIT license
- [Range-Based Set Reconciliation (RBSR) detailed analysis](https://logperiodic.com/rbsr.html) -- Comprehensive technical overview by the author
- [RBSR academic paper (arXiv 2212.13567)](https://arxiv.org/abs/2212.13567) -- Formal algorithm description and analysis
- [Aljoscha Meyer's set-reconciliation description](https://github.com/AljoschaMeyer/set-reconciliation) -- Original informal algorithm description
- [strfry Nostr relay (production user of negentropy)](https://github.com/hoytech/strfry) -- Production deployment at 10M+ elements
- [NIP-77: Negentropy syncing for Nostr](https://nips.nostr.com/77) -- Protocol standardization in Nostr
- [minisketch library (bitcoin-core)](https://github.com/bitcoin-core/minisketch) -- Evaluated and rejected (64-bit element limit)
- [gavinandresen/IBLT_Cplusplus](https://github.com/gavinandresen/IBLT_Cplusplus) -- Evaluated and rejected (unmaintained, no capacity guarantees)
- [Asio C++20 coroutines documentation](https://think-async.com/Asio/asio-1.22.0/doc/asio/overview/core/cpp20_coroutines.html)
- [Asio thread_pool reference](https://think-async.com/Asio/asio-1.22.0/doc/asio/reference/thread_pool.html)
- [Asio issue #1508 -- coroutines with thread pools](https://github.com/chriskohlhoff/asio/issues/1508)
- [Make it Async: dispatch-based executor switching pattern](https://blog.vito.nyc/posts/make-it-async/)
- [libmdbx vs LMDB API compatibility](https://github.com/erthink/libmdbx)
- Codebase verification: `db/sync/sync_protocol.h` (current O(N) hash list exchange)
- Codebase verification: `db/peer/peer_manager.cpp` (sync message rate limiting gap at line 436)
- Codebase verification: `db/crypto/hash.h` (SHA3-256 API for negentropy patch)
