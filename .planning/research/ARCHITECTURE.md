# Architecture Patterns: v0.8.0 Protocol Scalability

**Domain:** Sync set reconciliation, sync rate limiting, and thread pool crypto offload for chromatindb
**Researched:** 2026-03-19

## Recommended Architecture

v0.8.0 modifies four existing components (SyncProtocol, PeerManager, Engine, Config) and adds two new pieces: a vendored negentropy library (header-only, SHA3-patched) and a CryptoPool thread pool service. No new libmdbx sub-databases. One new wire message type (Reconcile = 27).

```
db/
  thread_pool/
    thread_pool.h               (NEW - CryptoPool service with co_await offload)
    thread_pool.cpp             (NEW - implementation)

  vendor/
    negentropy/                 (NEW - vendored header-only, SHA-256 -> SHA3-256 patch)
      negentropy.h
      negentropy/
        types.h                 (PATCHED - SHA3-256 via db/crypto/hash.h replaces OpenSSL SHA-256)
        encoding.h
        storage/
          base.h
          Vector.h

  sync/
    sync_protocol.h             (MODIFIED - add reconcile encode/decode, remove diff_hashes)
    sync_protocol.cpp           (MODIFIED - reconciliation logic replaces hash-list diff)
    reconciliation.h            (NEW - materialize_namespace() for negentropy Vector)
    reconciliation.cpp          (NEW - implementation)

  peer/peer_manager.h           (MODIFIED - CryptoPool& ref, sync cooldown state in PeerInfo)
  peer/peer_manager.cpp         (MODIFIED - Phase N reconciliation, crypto offload, sync rate check)
  engine/engine.h               (MODIFIED - ingest_async() with CryptoPool)
  engine/engine.cpp             (MODIFIED - two-dispatch crypto offload pipeline)
  config/config.h               (MODIFIED - thread_pool_workers, sync_rate_limit_per_min)
  config/config.cpp             (MODIFIED - parse new fields)
  schemas/transport.fbs         (MODIFIED - Reconcile = 27)
```

### Component Modification Map

| Component | What Changes | Why |
|-----------|-------------|-----|
| `SyncProtocol` | Add `encode_reconcile()` / `decode_reconcile()`. Remove `diff_hashes()`. | Reconcile messages replace HashList; hash-list diff logic is dead code. |
| `PeerManager` | Store `CryptoPool&`. Phase N reconciliation replaces HashList exchange. Per-peer sync cooldown. Route Reconcile messages to sync inbox. | Three features all touch sync orchestration. |
| `BlobEngine` | New `ingest_async(blob, pool)` with `co_await pool.offload()` for verify + hash. | Thread-safe crypto offload requires separating verify from storage mutation. |
| `Config` | `thread_pool_workers` (uint32, default hardware_concurrency). `sync_rate_limit_per_min` (uint32, default 0 = unlimited). | New features need configuration. |
| `transport.fbs` | `Reconcile = 27` in TransportMsgType enum. | Wire protocol for negentropy reconciliation messages. |
| `PeerInfo` | `last_sync_window_start`, `sync_count_in_window` fields. | Per-peer sync rate tracking. |

## Data Flow: Sync Protocol (New)

**Before (v0.7.0):**
```
Initiator                              Responder
  |                                      |
  |--- SyncRequest ------------------>   |
  |<-- SyncAccept -------------------    |
  |                                      |
  |--- NamespaceList (all ns+seq) -->    |   Phase A: both sides
  |--- HashList(ns1, ALL hashes) --->    |   send ALL hashes for
  |--- HashList(ns2, ALL hashes) --->    |   ALL namespaces
  |--- SyncComplete ---------------->    |
  |                                      |
  |<-- NamespaceList (all ns+seq) ---    |   Phase B: same, O(N)
  |<-- HashList(ns1, ALL hashes) ----    |
  |<-- SyncComplete -----------------    |
  |                                      |
  |  [diff_hashes: unordered_set]        |   Phase C: exchange
  |--- BlobRequest(batch) ---------->    |   missing blobs
  |<-- BlobTransfer(one) -----------     |   one at a time
```

**After (v0.8.0):**
```
Initiator                              Responder
  |                                      |
  |--- SyncRequest ------------------>   |
  |<-- SyncAccept -------------------    |
  |                                      |
  |--- NamespaceList (all ns+seq) -->    |   Phase A: namespace exchange
  |<-- NamespaceList (all ns+seq) ---    |   (cursor check: skip unchanged)
  |                                      |
  [for each namespace where seq changed:]
  |                                      |
  |  [build Vector from seq_map]         |   Phase N: reconciliation
  |--- Reconcile(ns, ne.initiate()) ->   |   ~few KB fingerprints
  |<-- Reconcile(ns, ne.reconcile()) -   |   ~few KB fingerprints
  |--- Reconcile(ns, ne.reconcile()) ->  |   refinement round (if needed)
  |<-- Reconcile(ns, done + IDs) ----    |   individual differing IDs
  |                                      |
  |--- SyncComplete ---------------->    |   end of reconciliation
  |                                      |
  [initiator knows: haveIds + needIds]   |   Phase C: blob exchange
  |--- BlobRequest(batch) ---------->    |   request needed blobs
  |<-- BlobTransfer(one) -----------     |   one at a time
  |--- BlobTransfer(one) ----------->    |   push blobs peer needs
```

**Key differences:**
1. NamespaceList is exchanged bidirectionally before any per-namespace work (cursor comparison happens here).
2. Per-namespace reconciliation runs only for namespaces with seq_num changes (cursor miss).
3. Reconciliation is multi-round-trip: 2-3 round-trips per namespace, each message ~few KB.
4. Blob exchange is driven by negentropy have/need IDs, not hash-list diff.
5. Total wire overhead for 1M blobs, 10 differences: ~50-100 KB (vs 32 MB before).

### Why Negentropy (Not Merkle Tree, Not IBLT)

**Negentropy** because:
1. **O(diff) communication** -- only differences transmitted. A single new blob in a 1M-blob namespace costs ~50 KB fingerprints, not 32 MB of hashes.
2. **Stateless server** -- no persistent tree per-namespace. Responder builds Vector from seq_map on-the-fly. No new libmdbx sub-databases.
3. **Frame size limit native** -- negentropy constructor accepts `frameSizeLimit`, automatically splits large reconciliations into additional round trips. Solves the "3.4M blobs breaks 110 MiB frame" problem permanently.
4. **Proven at scale** -- strfry relay syncs 10M+ element datasets in production (Nostr NIP-77).
5. **Header-only C++** -- vendor headers, patch one SHA-256 call. No new library dependencies.
6. **Logarithmic round trips** -- ~2.5 for 1M elements, ~4 for 1B elements.
7. **Composes with cursor optimization** -- cursor hit skips entire namespace. Cursor miss enters negentropy instead of full hash dump.

**Not Merkle tree** because:
- Requires persistent per-namespace tree (new sub-database, write amplification on every ingest)
- Per-connection state during sync (snapshot or copy-on-write)
- Vulnerable to adversarial tree imbalance (DoS via crafted data)
- More implementation complexity for equivalent O(diff) result

**Not IBLT** because:
- Requires estimating diff size in advance (wrong estimate = full retransmission fallback)
- Less battle-tested for general set reconciliation at scale
- More complex parameter tuning (filter size, hash count)

**Confidence: HIGH** -- negentropy is the proven choice in the decentralized data sync space.

### Negentropy Integration Details

**SHA-256 replacement:** The negentropy C++ library is header-only with one dependency: SHA-256 from OpenSSL. chromatindb prohibits OpenSSL. The SHA-256 is used only in `Accumulator::getFingerprint()` in `types.h`: it hashes the 32-byte accumulator + varint count, truncated to 16 bytes. Replace this single call with `OQS_SHA3_sha3_256()` (already available via liboqs).

This breaks interoperability with standard negentropy implementations. Acceptable because chromatindb speaks its own protocol -- no cross-protocol sync requirement.

**Negentropy data model mapping:**
```
Negentropy concept    chromatindb equivalent
-----------------    ----------------------
timestamp (u64)      seq_num (monotonic per-namespace, u64, in seq_map)
id (32 bytes)        blob content hash (SHA3-256, 32 bytes, in seq_map values)
sorted order         seq_map already sorted by (namespace, seq_num) in libmdbx
```

**Storage adapter approach: Pre-materialize Vector from seq_map.**

At reconciliation start for each namespace, read (seq_num, hash) pairs from seq_map into `negentropy::storage::Vector`. Filter zero-hash sentinels (v0.7.0 deletion markers). Memory: 40 bytes/blob (8-byte timestamp + 32-byte hash). For 1M blobs: ~38 MiB. Acceptable because reconciliation is one-namespace-at-a-time and the vector is freed immediately after.

```cpp
// db/sync/reconciliation.h
namespace chromatindb::sync {

/// Materialize a namespace's blob inventory into negentropy Vector format.
/// Reads seq_map entries, filters zero-hash sentinels.
negentropy::storage::Vector materialize_namespace(
    storage::Storage& storage,
    std::span<const uint8_t, 32> namespace_id);

/// Encode a Reconcile message payload: [namespace_id:32][negentropy_bytes:var]
std::vector<uint8_t> encode_reconcile(
    std::span<const uint8_t, 32> namespace_id,
    const std::string& negentropy_msg);

/// Decode a Reconcile message payload.
std::pair<std::array<uint8_t, 32>, std::string>
    decode_reconcile(std::span<const uint8_t> payload);

} // namespace chromatindb::sync
```

**Zero-hash sentinel handling:** v0.7.0 writes all-zeros hashes into seq_map when blobs are deleted (preserving seq_num monotonicity for cursor change detection). The materialization function must filter these: `if (hash == zero_array) continue;`. This is critical -- negentropy would treat zero-hash entries as real items and produce incorrect diffs.

**No schema changes needed:** seq_map already provides sorted iteration. The Vector is a read-only materialization. No new sub-databases in libmdbx.

### Wire Format Changes

Add to `transport.fbs`:
```flatbuffers
Reconcile = 27      // Negentropy reconciliation message
```

Reconcile message payload:
```
[namespace_id:32][negentropy_msg_bytes:variable]
```

The 32-byte namespace prefix identifies which namespace. The rest is raw negentropy protocol bytes (self-describing: version byte 0x61 + range sequences). Same pattern as other binary payloads (NamespaceList, HashList, BlobRequest).

**Frame size safety:** Set negentropy `frameSizeLimit` to `MAX_FRAME_SIZE - 36` (32-byte prefix + headroom). Negentropy automatically splits into additional round trips when a single message would exceed the limit.

### Reconciliation Integration in PeerManager

```
PeerManager::run_sync_with_peer()     (initiator)
  |
  +-- NamespaceList exchange           (existing, unchanged)
  +-- Cursor optimization              (existing, unchanged -- skip cursor-hit namespaces)
  |
  +-- Phase N (NEW): for each namespace needing sync:
  |     +-- materialize_namespace() -> negentropy::storage::Vector
  |     +-- Negentropy<Vector> ne(storage, frameSizeLimit)
  |     +-- msg = ne.initiate()
  |     +-- Loop: send Reconcile(ns, msg)
  |     |         recv Reconcile response
  |     |         next = ne.reconcile(response, have, need)
  |     |         collect have_ids (peer needs from us)
  |     |         collect need_ids (we need from peer)
  |     |         if (!next) break
  |     |         msg = *next
  |
  +-- SyncComplete
  |
  +-- Phase C: BlobRequest/BlobTransfer for need_ids (existing pattern)
  |     +-- Also push BlobTransfer for have_ids
  |
  +-- Post-sync: cursor update, capacity signals, PEX

PeerManager::handle_sync_as_responder()
  |
  +-- NamespaceList exchange           (existing, unchanged)
  +-- Phase N: for each incoming Reconcile(ns, msg):
  |     +-- materialize_namespace() -> Vector
  |     +-- ne.reconcile(msg) -> response
  |     +-- send Reconcile(ns, response)
  +-- Phase C: respond to BlobRequests (existing pattern)
```

**Message routing:** `on_peer_message()` routes `TransportMsgType_Reconcile` to the sync inbox (same as HashList, BlobRequest, etc. today).

**Removed code:** `diff_hashes()` in SyncProtocol becomes dead code. HashList send/receive in sync flow is removed. `collect_namespace_hashes()` is replaced by `materialize_namespace()`.

## Data Flow: Crypto Offload Pipeline

```
io_context thread:
  receive_blob_from_peer()
    |
    v
  Step 0-2: cheap checks (size, capacity, structural, namespace, quota)
    |  [all fast, stay on io_context]
    v
  co_await crypto_pool_.offload([&]() {
    |  io_context thread is FREE for other connections
    |
    |  Worker thread:
    |    auto encoded = wire::encode_blob(blob);     // FlatBuffer serialize
    |    auto hash = wire::blob_hash(encoded);        // SHA3-256
    |    auto signing_input = wire::build_signing_input(...);
    |    bool ok = crypto::Signer::verify(signing_input, sig, pubkey);
    |    return VerifyResult{ok, hash, std::move(encoded)};
  });
    |
    v  [back on io_context -- coroutine resumed]
  if (result.ok) {
    dedup_check = storage_.has_blob(ns, result.hash);   // mdbx read
    tombstone_check = ...                                 // mdbx read
    storage_.store_blob(blob, result.hash, result.encoded); // mdbx write
  }
```

**Thread boundary rule:** Everything before and after the `co_await offload()` runs on io_context. The lambda captures blob data by reference (blob is alive on the coroutine stack). Only stateless crypto work runs on pool workers.

**What crosses the boundary:**
- INTO pool: `const wire::BlobData&` (by reference, coroutine suspended, blob on stack)
- OUT of pool: `bool valid`, `array<uint8_t, 32> content_hash`, `vector<uint8_t> encoded`
- NEVER crosses: `Storage&`, `Engine&`, `Connection::Ptr`, `PeerInfo*`

### CryptoPool Service

```cpp
// db/thread_pool/thread_pool.h
namespace chromatindb::pool {

class CryptoPool {
public:
    explicit CryptoPool(uint32_t worker_count);
    ~CryptoPool();

    /// Offload a callable to thread pool, co_await result, resume on io_context.
    template<typename F>
    asio::awaitable<std::invoke_result_t<F>> offload(F&& func);

private:
    asio::thread_pool pool_;
};

} // namespace chromatindb::pool
```

**offload() implementation pattern (two approaches, choose during implementation):**

Approach A (co_spawn across executors):
```cpp
template<typename F>
asio::awaitable<std::invoke_result_t<F>> CryptoPool::offload(F&& func) {
    using R = std::invoke_result_t<F>;
    co_return co_await asio::co_spawn(pool_.get_executor(),
        [f = std::forward<F>(func)]() -> asio::awaitable<R> { co_return f(); },
        asio::use_awaitable);
}
```

Approach B (post + post round-trip):
```cpp
template<typename F>
asio::awaitable<std::invoke_result_t<F>> CryptoPool::offload(F&& func) {
    using R = std::invoke_result_t<F>;
    auto executor = co_await asio::this_coro::executor;
    co_await asio::post(pool_.get_executor(), asio::use_awaitable);
    // Now running on pool thread
    R result = func();
    co_await asio::post(executor, asio::use_awaitable);
    // Now running on io_context thread
    co_return result;
}
```

**Confidence: MEDIUM** -- both patterns are documented but the exact Standalone Asio (not Boost) behavior with C++20 coroutines needs validation during implementation. Approach B is more explicit about executor switching.

### BlobEngine Async Pipeline

Current `ingest()` (synchronous, blocks event loop):
```
Step 0:  Size check               ~0ms
Step 0b: Capacity check           ~0ms
Step 1:  Structural checks        ~0ms
Step 2:  Namespace/delegation     ~0ms  (SHA3-256 of 2.5 KB pubkey)
Step 2a: Quota check              ~0ms
Step 2.5: encode + hash           ~1.5ms  (SHA3-256 of up to 100 MiB blob)
         Dedup check              ~0ms
Step 3:  ML-DSA verify            ~2ms   (signature verification)
Step 3.5: Tombstone handling      ~0ms
Step 4:  Store                    ~0.1ms
                             TOTAL: ~3.6ms blocking io_context per 1 MiB blob
```

New `ingest_async()` (crypto offloaded):
```
Step 0-2a: cheap checks           ~0ms   (io_context)
Step 2.5+3: encode + hash + verify   ~3.5ms (OFFLOADED to pool worker)
           [io_context FREE for ~3.5ms]
Step 3.5-4: tombstone + store     ~0.1ms (io_context, after co_await returns)
```

With N worker threads, the node can process N blobs' crypto in parallel while the io_context handles I/O for all connections. For a 6-core machine: theoretical ~6x throughput for CPU-bound blob verification.

### thread_local OQS_SIG Safety

`Signer::verify()` uses `thread_local static OQS_SIG*` (v0.7.0 optimization). With thread pool offload, `verify()` now runs on worker threads. `thread_local` is correct: each worker gets its own OQS_SIG context. No mutex needed.

## Sync Rate Limiting Design

### Problem

Current rate limiting bypasses sync messages entirely (peer_manager.cpp line 437: only Data/Delete are checked). A malicious peer can:
1. Spam SyncRequest to force expensive materialization + reconciliation
2. Trigger CPU-bound crypto verification via sync blob ingestion
3. Exhaust memory by forcing concurrent namespace materializations

### Solution: Per-Peer Sync Cooldown

Rate-limit sync round initiation (SyncRequest messages), not individual sync messages within a round. Granularity is "sync rounds per time window" because:
- A sync round has bounded cost (reconciliation per namespace, bounded by frame size)
- Individual messages are small (negentropy messages < 1 KiB typically)
- Throttling mid-sync would waste completed work

### Implementation

PeerInfo additions:
```cpp
uint64_t last_sync_window_start = 0;   // steady_clock ms
uint32_t sync_count_in_window = 0;     // Rounds in current 60s window
```

Config addition:
```cpp
uint32_t sync_rate_limit_per_min = 0;  // Max sync rounds per peer per minute (0 = unlimited)
```

Rate check in `on_peer_message()`:
```cpp
if (type == TransportMsgType_SyncRequest) {
    auto* peer = find_peer(conn);
    if (peer && !peer->syncing) {
        if (sync_rate_limit_per_min_ > 0) {
            auto now = steady_clock_ms();
            if (now - peer->last_sync_window_start >= 60000) {
                peer->last_sync_window_start = now;
                peer->sync_count_in_window = 0;
            }
            if (peer->sync_count_in_window >= sync_rate_limit_per_min_) {
                ++metrics_.sync_rate_limited;
                record_strike(conn, "sync rate limit exceeded");
                return;  // Silently drop
            }
            peer->sync_count_in_window++;
        }
        // Proceed with sync...
    }
}
```

Metrics addition: `uint64_t sync_rate_limited = 0;` in NodeMetrics.

**Confidence: HIGH** -- follows the exact pattern of existing Data/Delete rate limiting.

## Patterns to Follow

### Pattern 1: Two-Dispatch Executor Switching

**What:** Offload CPU-bound work to thread pool, resume on io_context.
**When:** Any crypto operation > 100us on hot path.
**Example:**
```cpp
auto [valid, hash] = co_await crypto_pool_.offload([&]() {
    auto encoded = wire::encode_blob(blob);
    auto h = wire::blob_hash(encoded);
    auto si = wire::build_signing_input(blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    bool ok = crypto::Signer::verify(si, blob.signature, blob.pubkey);
    return std::make_pair(ok, h);
});
// Back on io_context -- safe to access storage
if (valid) storage_.store_blob(blob, hash, encoded);
```

### Pattern 2: Per-Namespace Reconciliation Loop

**What:** Multi-round negentropy reconciliation as async coroutine loop.
**When:** During sync Phase N, for each namespace with seq_num changes.
**Example:**
```cpp
auto items = sync::materialize_namespace(storage_, namespace_id);
Negentropy<negentropy::storage::Vector> ne(items, FRAME_SIZE_LIMIT);
auto msg = ne.initiate();
std::vector<std::string> all_have, all_need;
while (true) {
    co_await conn->send_message(TransportMsgType_Reconcile,
        sync::encode_reconcile(namespace_id, msg));
    auto resp = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
    if (!resp || resp->type != TransportMsgType_Reconcile) break;
    auto [ns, resp_bytes] = sync::decode_reconcile(resp->payload);
    std::vector<std::string> have, need;
    auto next = ne.reconcile(resp_bytes, have, need);
    all_have.insert(all_have.end(), have.begin(), have.end());
    all_need.insert(all_need.end(), need.begin(), need.end());
    if (!next) break;
    msg = *next;
}
```

### Pattern 3: Fresh Storage View per Reconciliation

**What:** Rebuild negentropy Vector from seq_map at the start of each sync round.
**When:** Every reconciliation for every namespace.
**Why:** Storage changes between syncs. Negentropy's design advantage is stateless servers -- lean into it rather than maintaining persistent trees.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Passing Connection or PeerInfo to Thread Pool

**What:** Capturing `Connection::Ptr` or `PeerInfo*` in pool lambda.
**Why bad:** Connection contains AEAD nonce counters (send_counter_, recv_counter_) -- io_context-only. PeerInfo has non-thread-safe containers. Data races + nonce reuse.
**Instead:** Copy only needed bytes into lambda. Return results by value.

### Anti-Pattern 2: Storage Access from Thread Pool

**What:** Calling any Storage method from a pool worker.
**Why bad:** libmdbx transactions are thread-bound. Write transactions are single-writer on io_context. Even read transactions from pool threads can corrupt reader table.
**Instead:** Read all needed data on io_context before posting to pool. Crypto needs only in-memory blob data.

### Anti-Pattern 3: Persistent Negentropy B-Tree

**What:** Maintaining BTreeLMDB alongside libmdbx blob storage.
**Why bad:** Write amplification (every ingest updates tree). Consistency risk (two databases). Data duplication. New dependency (lmdbxx bindings).
**Instead:** Build Vector per-sync-round from seq_map. Construction cost is O(N) -- same as current hash collection -- but reconciliation saves are O(diff) wire overhead vs O(N).

### Anti-Pattern 4: Single-Frame Reconciliation

**What:** Trying to fit entire reconciliation into one message.
**Why bad:** Recreates the MAX_FRAME_SIZE cliff for large namespaces.
**Instead:** Use negentropy's frameSizeLimit. Multiple round trips are cheap and automatic.

### Anti-Pattern 5: Fine-Grained Sync Rate Limiting

**What:** Rate-limiting individual Reconcile/BlobTransfer messages within a round.
**Why bad:** Interrupting mid-sync wastes all completed work. Negentropy is multi-round -- throttling one message kills entire reconciliation.
**Instead:** Rate-limit SyncRequest initiation. Let accepted rounds complete.

### Anti-Pattern 6: Thread Pool for AEAD Operations

**What:** Offloading ChaCha20-Poly1305 encrypt/decrypt to thread pool.
**Why bad:** AEAD uses connection-scoped nonce counters. Concurrent access = nonce reuse = security break.
**Instead:** AEAD stays on io_context. ChaCha20 is fast (~0.5ms for 100 MiB). Bottleneck is ML-DSA verify + SHA3 hash.

### Anti-Pattern 7: HashList as Fallback

**What:** Falling back to HashList exchange if reconciliation fails.
**Why bad:** Two code paths doubles testing and maintenance. Negentropy doesn't "fail" recoverbly -- connection breaks cause full sync retry anyway.
**Instead:** Remove HashList-based sync entirely. Keep enum value for wire stability, never send it.

## Scalability Considerations

| Concern | 10K blobs/ns | 100K blobs/ns | 1M blobs/ns | 10M blobs/ns |
|---------|-------------|---------------|-------------|--------------|
| Reconciliation wire (10 diffs) | ~1 KB | ~5 KB | ~50 KB | ~200 KB |
| Current HashList wire | 320 KB | 3.2 MB | 32 MB | 320 MB (BREAKS) |
| Vector construction time | <1ms | ~10ms | ~100ms | ~1s |
| Vector memory | ~400 KB | ~4 MB | ~38 MB | ~380 MB |
| Reconciliation rounds | 1-2 | 2-3 | 2-3 | 3-4 |
| Thread pool benefit (1 MiB blobs) | Negligible | Significant | Critical | Critical |
| Thread pool: 6-core throughput | ~90 blobs/s | ~90 blobs/s | ~90 blobs/s | ~90 blobs/s |
| Thread pool: 1-core (current) | ~18 blobs/s | ~18 blobs/s | ~18 blobs/s | ~18 blobs/s |

**10M+ blob namespaces:** Vector materialization (~380 MB, ~1s) becomes the bottleneck. Mitigation: port negentropy's BTreeLMDB to libmdbx. This is future work -- v0.8.0 targets 100K-1M range.

## Build Order (Dependency-Aware)

### Dependency Graph

```
Thread Pool (Phase 1) -- foundational, no dependencies
    |
    +---> BlobEngine Async (Phase 2) -- needs thread pool
    |
    |         Sync Rate Limiting (Phase 3) -- independent
    |         Negentropy Vendor + Adapter (Phase 4) -- independent
    |              |
    +--------------+
            |
            v
    Sync Protocol Integration (Phase 5) -- needs thread pool + negentropy + async ingest
            |
            v
    Benchmark Validation (Phase 6) -- needs everything
```

### Suggested Phase Order

1. **Thread Pool Foundation** -- CryptoPool, config, offload helper, unit tests
2. **BlobEngine Async Ingest** -- ingest_async, PeerManager caller updates, all tests pass
3. **Sync Rate Limiting** -- PeerInfo state, config, SyncRequest rate check, metrics
4. **Negentropy Vendor + Storage Adapter** -- vendored headers, SHA3 patch, materialize_namespace, encode/decode_reconcile, Reconcile = 27 wire type, unit tests
5. **Sync Protocol Integration** -- Phase N replaces HashList in run_sync_with_peer/handle_sync_as_responder, message routing, integration tests
6. **Benchmark Validation** -- Docker suite, bandwidth comparison, throughput comparison

**Critical path:** 1 -> 2 -> 5 -> 6. Phases 3 and 4 are parallelizable but sequenced for clean execution.

## Sources

- [Negentropy reference implementation (C++ header-only)](https://github.com/hoytech/negentropy) -- HIGH confidence
- [Range-Based Set Reconciliation specification](https://logperiodic.com/rbsr.html) -- HIGH confidence
- [NIP-77: Negentropy syncing for Nostr](https://nips.nostr.com/77) -- MEDIUM confidence
- [Asio C++20 coroutines](https://think-async.com/Asio/asio-1.22.0/doc/asio/overview/core/cpp20_coroutines.html) -- HIGH confidence
- [Asio thread_pool + coroutines (issue #1508)](https://github.com/chriskohlhoff/asio/issues/1508) -- MEDIUM confidence
- Codebase: `db/sync/sync_protocol.h+cpp`, `db/peer/peer_manager.h+cpp`, `db/engine/engine.h+cpp`, `db/storage/storage.h`, `db/wire/codec.h`, `db/net/framing.h`, `db/schemas/transport.fbs` -- HIGH confidence
