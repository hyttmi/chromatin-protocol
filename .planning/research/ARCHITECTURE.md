# Architecture Patterns

**Domain:** Sync resumption, namespace quotas, crypto throughput, and cleanup for chromatindb
**Researched:** 2026-03-16

## Recommended Architecture

v0.7.0 modifies four existing components (Storage, Engine, SyncProtocol, PeerManager) and relocates tests. No new source directories. No new dependencies. No protocol changes to the FlatBuffers transport schema.

```
db/
  storage/storage.h     (MODIFIED - new quota_map, sync_cursor_map sub-databases)
  storage/storage.cpp   (MODIFIED - quota tracking, cursor persistence, hash caching)
  engine/engine.h       (MODIFIED - quota enforcement on ingest)
  engine/engine.cpp     (MODIFIED - quota check, pre-computed hash threading)
  sync/sync_protocol.h  (MODIFIED - cursor-aware hash collection)
  sync/sync_protocol.cpp (MODIFIED - cursor-based diff, skip already-synced)
  peer/peer_manager.h   (MODIFIED - cursor state in PeerInfo)
  peer/peer_manager.cpp (MODIFIED - cursor read/write around sync rounds)
  crypto/signing.h      (MODIFIED - static OQS_SIG context, optional pre-hash verify)
  crypto/signing.cpp    (MODIFIED - eliminate per-call OQS_SIG_new allocation)
  config/config.h       (MODIFIED - namespace_quotas config field)
  config/config.cpp     (MODIFIED - parse namespace_quotas)
  tests/                (NEW directory - relocated from top-level tests/)
  CMakeLists.txt        (MODIFIED - Catch2 dependency, test target)

CMakeLists.txt          (MODIFIED - remove test sources, update paths)
tests/                  (DELETED - moved into db/tests/)

deploy/
  benchmark/            (MODIFIED - add deletion benchmark scenario)
```

### Component Modification Map

| Component | What Changes | Why |
|-----------|-------------|-----|
| `Storage` | +2 sub-databases (sync_cursor_map, quota_map), quota tracking in store_blob/delete_blob_data, cursor read/write methods, blob hash returned from store without re-computation | Sync cursors and quotas need persistent state; hash caching eliminates redundant SHA3 |
| `Engine` | Quota enforcement before crypto validation (Step 0c), pass pre-computed blob hash through pipeline | Quotas are cheaper than sig verify; hash sharing eliminates 1 MiB re-hash |
| `SyncProtocol` | Cursor-aware collect_namespace_hashes (seq_num > cursor), report new cursor position | Sync resumption: only exchange hashes for blobs since last sync |
| `PeerManager` | Persist cursor per peer pubkey, load on connect, save after sync, cursor in PeerInfo | Cursor state must survive reconnect; pubkey is stable peer identity |
| `Signer::verify` | Static thread_local OQS_SIG context (not per-call allocation) | Eliminates 1 allocation + init per verify call |
| `Config` | `namespace_quotas` map in config struct | Operator-configurable per-namespace limits |

### Data Flow Changes

**Current ingest pipeline (1 MiB blob, sync path):**
```
decode blob from wire
  -> SHA3-256(pubkey) [2.5 KB]                          ~0ms
  -> build_signing_input: copy ns(32)+data(1M)+ttl+ts   ~0.3ms (memcpy 1M)
  -> OQS_SIG_verify(signing_input)                       ~2ms (internally hashes 1M+)
  -> encode_blob -> SHA3-256(encoded) [tombstone check]  ~1.5ms (encode 1M + hash 1M)
  -> store_blob:
       encode_blob AGAIN -> SHA3-256(encoded) AGAIN      ~1.5ms (REDUNDANT)
       encrypt_value (ChaCha20-Poly1305)                  ~0.5ms
       write to mdbx                                      ~0.1ms
                                                     TOTAL: ~6ms per blob
```

**Proposed ingest pipeline:**
```
decode blob from wire
  -> SHA3-256(pubkey) [2.5 KB]                          ~0ms
  -> build_signing_input: ref ns+data, no copy           ~0ms (span, not vector)
  -> OQS_SIG_verify(signing_input)                       ~2ms (unavoidable)
  -> encode_blob -> SHA3-256(encoded) [tombstone check]  ~1.5ms
  -> store_blob_with_hash(blob, pre_computed_hash):
       encode_blob -> skip hash (already have it)         ~0.8ms (encode only, no re-hash)
       encrypt_value (ChaCha20-Poly1305)                  ~0.5ms
       write to mdbx                                      ~0.1ms
                                                     TOTAL: ~4.9ms per blob
```

Savings: ~1.1ms per 1 MiB blob (18% reduction). At 15 blobs/sec, this lifts throughput to ~18 blobs/sec. Not transformative, but free and correct.

The real bottleneck is `OQS_SIG_verify` (2ms per call for 1 MiB messages) plus `SHA3-256` on 1 MiB encoded blobs (1.5ms). These are inherently serial on the single io_context thread. The only way to break this ceiling is to offload crypto to a thread pool, which is a v0.8.0 concern (complex interaction with coroutine model).

## New Sub-Databases

Storage currently manages 5 sub-databases in libmdbx. v0.7.0 adds 2 more.

### sync_cursor_map

**Purpose:** Persist per-peer sync cursor positions across reconnections.

```
Key:   [peer_pubkey_hash:32][namespace:32]    (64 bytes)
Value: [seq_num:u64BE]                         (8 bytes)
```

**Why peer_pubkey_hash, not raw pubkey:** ML-DSA-87 pubkeys are 2,592 bytes. Using SHA3-256(pubkey) as a 32-byte identifier keeps keys compact and B-tree friendly. The peer_pubkey is already available in PeerInfo -- hash it once at connect time.

**Operations:**
- `get_sync_cursor(peer_hash, namespace)` -> optional<uint64_t>
- `set_sync_cursor(peer_hash, namespace, seq_num)` -> void
- `clear_sync_cursors(peer_hash)` -> void (for manual reset)

**Lifecycle:** Written after each successful sync round. Read at sync start. Never expires (cursor is only meaningful while peer exists in the network, but stale cursors just cause a full resync -- harmless).

### quota_map

**Purpose:** Track per-namespace storage usage for quota enforcement.

```
Key:   [namespace:32]                         (32 bytes)
Value: [blob_count:u64BE][total_bytes:u64BE]  (16 bytes)
```

**Operations:**
- `get_namespace_usage(namespace)` -> {count, bytes}
- Increment on `store_blob` (atomically in same write txn)
- Decrement on `delete_blob_data` and `run_expiry_scan` (atomically)

**Why a dedicated sub-database, not computed on-demand:** Computing namespace usage requires scanning the entire seq_map for a namespace + fetching each blob's size from blobs_map. For a namespace with 10K blobs, this is O(10K) reads per ingest -- unacceptable. The quota_map maintains a running total, making enforcement O(1).

**Consistency:** The quota_map is updated in the same mdbx write transaction as the blob store/delete, so it is always consistent. No separate reconciliation needed.

## Sync Resumption Design

### Current Sync Protocol (Phase A/B/C)

```
Initiator                          Responder
    |-- SyncRequest ----------------->|
    |<------------- SyncAccept -------|
    |                                 |
    |-- NamespaceList --------------->|   Phase A: both sides
    |-- HashList (ns1) -------------->|   send ALL hashes for
    |-- HashList (ns2) -------------->|   ALL namespaces
    |-- SyncComplete ---------------->|
    |                                 |
    |<----------- NamespaceList ------|
    |<----------- HashList (ns1) ----|
    |<----------- HashList (ns2) ----|
    |<----------- SyncComplete ------|
    |                                 |
    |-- BlobRequest (missing) ------->|   Phase C: exchange
    |<----------- BlobTransfer -------|   missing blobs
```

**Problem:** For a namespace with 100K blobs, `get_hashes_by_namespace` reads all 100K hashes from seq_map every sync round. This is O(N) per namespace per sync, even when only 3 new blobs exist since the last sync.

### Proposed Sync Resumption

```
Initiator                          Responder
    |-- SyncRequest ----------------->|
    |<------------- SyncAccept -------|
    |                                 |
    |-- NamespaceList --------------->|   Phase A: same as before
    |-- HashList (ns1, since=4500)-->|   BUT: only hashes with
    |-- HashList (ns2, since=0) ---->|   seq_num > stored cursor
    |-- SyncComplete ---------------->|   (since=0 for new namespaces)
    |                                 |
    |<----------- NamespaceList ------|
    |<----------- HashList (ns1) ----|   Responder also sends
    |<----------- HashList (ns2) ----|   only new hashes if it
    |<----------- SyncComplete ------|   has a cursor for this peer
    |                                 |
    |-- BlobRequest (missing) ------->|   Phase C: unchanged
    |<----------- BlobTransfer -------|
```

**Key insight: No protocol changes needed.** The `NamespaceList` already includes `latest_seq_num` per namespace. The receiver compares incoming `latest_seq_num` against its stored cursor to decide whether a full or incremental hash exchange is needed. The `HashList` format is unchanged -- it just contains fewer hashes.

**Implementation in SyncProtocol:**

```cpp
// NEW: collect hashes since a cursor position
std::vector<std::array<uint8_t, 32>> collect_namespace_hashes_since(
    std::span<const uint8_t, 32> namespace_id,
    uint64_t since_seq);
```

This reuses `Storage::get_hashes_by_namespace` but starts the cursor seek at `since_seq + 1` instead of seq 1.

**Implementation in PeerManager (initiator side):**

```cpp
// In run_sync_with_peer, before hash collection:
for (const auto& ns_info : our_namespaces) {
    uint64_t cursor = storage_.get_sync_cursor(peer_hash, ns_info.namespace_id);
    auto hashes = sync_proto_.collect_namespace_hashes_since(
        ns_info.namespace_id, cursor);
    // ... send HashList as before
}

// After successful sync:
for (const auto& ns_info : peer_namespaces) {
    storage_.set_sync_cursor(peer_hash, ns_info.namespace_id,
                              ns_info.latest_seq_num);
}
```

**Edge cases:**
- **First sync with a peer:** cursor = 0, sends all hashes (full sync, same as today).
- **Peer has new namespace we've never seen:** cursor = 0 for that namespace.
- **Peer deleted blobs since last sync:** Tombstones have their own seq_nums. Cursor still works because tombstones are blobs in the seq index.
- **Node restarted, peer reconnects:** Cursor persisted in sync_cursor_map, survives restart.
- **Stale cursor (peer rolled back):** Worst case: we miss some blobs. Next full sync (cursor clear or periodic full resync) catches them. Acceptable for v0.7.0.

**Periodic full resync:** Every Nth sync round (configurable, default 10), ignore cursors and do a full hash exchange. This catches any cursor drift or missed blobs. Simple, defensive.

### Hash Encoding Change for Cursor Support

The current `encode_hash_list` format is `[ns:32B][count:u32BE][hash1:32B]...`. To support cursor-aware sync, the responder needs to know the cursor position so it can compute the diff correctly.

Two options:
1. **Encode the cursor in NamespaceList:** Extend NamespaceList entries to include a `since_seq` field alongside `latest_seq_num`. This changes the wire format.
2. **Receiver computes diff from full context:** The receiver already has the initiator's `latest_seq_num` from NamespaceList. It can compare against its own cursor for that peer to decide what to send.

**Recommendation: Option 2.** No wire format changes. The responder maintains its own cursor for the initiator (by peer pubkey hash) and uses it to filter its hash list. Both sides independently use their own cursors. The diff algorithm in Phase C handles any asymmetry.

## Namespace Quotas Design

### Enforcement Points

Quotas must be enforced at every data entry point:

1. **Client writes (`TransportMsgType_Data`):** Checked in `Engine::ingest()` before signature verification (Step 0c, after size check and capacity check).
2. **Sync ingest (`SyncProtocol::ingest_blobs`):** Also goes through `Engine::ingest()`. Same path.
3. **Delete writes (`TransportMsgType_Delete`):** Tombstones are small (36 bytes) and exempt from quotas, like they are from global capacity checks. They reduce namespace usage by deleting the target blob.

```cpp
// In Engine::ingest(), after storage_full check:
// Step 0c: Namespace quota check
if (!namespace_quotas_.empty() && !wire::is_tombstone(blob.data)) {
    auto it = namespace_quotas_.find(blob.namespace_id);
    if (it != namespace_quotas_.end()) {
        auto usage = storage_.get_namespace_usage(blob.namespace_id);
        if (it->second.max_bytes > 0 && usage.bytes >= it->second.max_bytes) {
            return IngestResult::rejection(IngestError::quota_exceeded, ...);
        }
        if (it->second.max_count > 0 && usage.count >= it->second.max_count) {
            return IngestResult::rejection(IngestError::quota_exceeded, ...);
        }
    }
}
```

### Config Format

```json
{
  "namespace_quotas": {
    "default": {
      "max_bytes": 1073741824,
      "max_count": 0
    },
    "abcdef0123456789...": {
      "max_bytes": 10737418240,
      "max_count": 100000
    }
  }
}
```

- `"default"` applies to any namespace not explicitly listed.
- Per-namespace overrides by hex namespace ID (64 chars).
- `0` = unlimited (no quota for that dimension).
- Empty `namespace_quotas` = no quotas (backward compatible).
- Quotas are hot-reloadable via SIGHUP.

### New IngestError Variant

```cpp
enum class IngestError {
    // ... existing ...
    quota_exceeded    ///< Namespace quota (bytes or count) exceeded.
};
```

No new protocol message type. The rejection is handled the same as `storage_full` -- the peer gets no explicit error message (the Data message is simply not acknowledged with a WriteAck). For sync, the blob is silently skipped like `storage_full` blobs.

## Crypto Throughput Optimization

### Bottleneck Analysis

From the v0.6.0 benchmark report, 1 MiB blob sync reaches 96% CPU on node2 at 15.3 blobs/sec. The CPU is spent on:

1. **ML-DSA-87 verify (~2ms):** Internally hashes the signing input (~1 MiB). Unavoidable per-blob cost.
2. **SHA3-256 blob hash for dedup/tombstone check (~1.5ms):** Requires encoding to FlatBuffer first, then hashing the encoded output.
3. **SHA3-256 blob hash AGAIN in store_blob (~1.5ms):** `store_blob` re-encodes and re-hashes because it doesn't receive the pre-computed hash.
4. **ChaCha20-Poly1305 DARE encryption (~0.5ms):** Encrypts the encoded blob for at-rest storage.
5. **build_signing_input allocation (~0.3ms):** Copies 1 MiB into a new vector.

### Optimization Strategy (ordered by impact/complexity ratio)

**Optimization 1: Eliminate redundant blob hash (HIGH impact, LOW complexity)**

`Engine::ingest()` computes `blob_hash(encode_blob(blob))` at step 3.5 for tombstone check. Then `Storage::store_blob()` computes it again. Pass the pre-computed hash through.

New Storage method:
```cpp
StoreResult store_blob(const wire::BlobData& blob,
                       std::optional<std::array<uint8_t, 32>> precomputed_hash = std::nullopt);
```

If `precomputed_hash` is provided, skip the re-encode + re-hash. Saves ~1.5ms per 1 MiB blob.

**Optimization 2: Eliminate per-call OQS_SIG allocation (MEDIUM impact, LOW complexity)**

`Signer::verify()` creates a new `OQS_SIG*` context on every call via `OQS_SIG_new()`, then frees it. This involves memory allocation and algorithm initialization. Use a `thread_local static` context instead:

```cpp
bool Signer::verify(std::span<const uint8_t> message,
                    std::span<const uint8_t> signature,
                    std::span<const uint8_t> public_key) {
    thread_local OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    // sig is allocated once per thread, reused across calls
    OQS_STATUS rc = OQS_SIG_verify(sig, message.data(), message.size(),
                                    signature.data(), signature.size(),
                                    public_key.data());
    return rc == OQS_SUCCESS;
}
```

Since chromatindb runs on a single io_context thread, `thread_local` is equivalent to a static singleton here. No thread-safety concerns.

**Optimization 3: Avoid signing input copy (LOW impact, LOW complexity)**

`build_signing_input()` allocates a vector and copies ns(32) + data(1MiB) + ttl(4) + ts(8). For 1 MiB blobs, this is a 1 MiB allocation + memcpy.

Alternative: use the OQS incremental signing API (if available) or compute the hash incrementally using SHA3 IUF (Init/Update/Finalize) API and pass the pre-hashed digest to ML-DSA.

However, ML-DSA-87 in "pure" mode requires the full message for verification -- it internally computes `mu = SHAKE256(tr || M')` where `M'` includes the full message. Pre-hashing would require switching to HashML-DSA mode (FIPS 204 Algorithm 5), which uses a different OID and is a different signature scheme. This is NOT backward compatible with existing signatures.

**Recommendation: Skip optimization 3 for v0.7.0.** The 0.3ms memcpy savings is small. Switching to HashML-DSA would require re-signing all existing blobs (protocol-breaking change). The IUF approach would only help if liboqs exposes an incremental verify API, which it does not (the `OQS_SIG_verify` API requires contiguous message bytes).

**Optimization 4: Skip encode_blob in tombstone check path (MEDIUM impact, LOW complexity)**

In `Engine::ingest()`, the tombstone check path (`is_tombstone` is false) does:
```cpp
auto encoded = wire::encode_blob(blob);
auto content_hash = wire::blob_hash(encoded);
if (storage_.has_tombstone_for(blob.namespace_id, content_hash)) { ... }
```

This encodes the entire blob just to get its hash. But the hash is also needed later in `store_blob`. Restructure to compute the hash once and pass it through:

```cpp
auto encoded = wire::encode_blob(blob);
auto content_hash = wire::blob_hash(encoded);
// Use content_hash for tombstone check
if (storage_.has_tombstone_for(blob.namespace_id, content_hash)) { ... }
// Pass to store_blob to avoid re-computation
auto store_result = storage_.store_blob(blob, content_hash);
```

This is essentially the same as Optimization 1 but viewed from the caller side.

### Summary of Achievable Gains

| Optimization | Savings per 1 MiB blob | Complexity | Ship in v0.7.0? |
|-------------|----------------------|------------|-----------------|
| Pass pre-computed hash to store_blob | ~1.5ms | Low | Yes |
| Static OQS_SIG context | ~0.1-0.3ms | Low | Yes |
| Avoid signing input copy (HashML-DSA) | ~0.3ms | High (protocol break) | No |
| Thread pool for crypto | ~4ms (parallelism) | High (coroutine interaction) | No, v0.8.0 |

**Expected result:** From 15.3 blobs/sec to ~18-20 blobs/sec for 1 MiB blobs. The fundamental ceiling is the ML-DSA verify call (~2ms) which cannot be optimized without parallelism.

## Test Relocation

### Current Structure

```
chromatin-protocol/
  CMakeLists.txt          <- owns tests/*, links Catch2
  tests/
    crypto/test_hash.cpp
    crypto/test_signing.cpp
    ...20 test files...
  db/
    CMakeLists.txt        <- no test target, no Catch2 dep
    crypto/hash.h
    ...
```

### Target Structure

```
chromatin-protocol/
  CMakeLists.txt          <- no test sources, delegates to db/
  db/
    CMakeLists.txt        <- owns tests/*, links Catch2
    tests/
      crypto/test_hash.cpp
      crypto/test_signing.cpp
      ...20 test files...
    crypto/hash.h
    ...
```

### Migration Strategy

1. `git mv tests/ db/tests/` -- preserves history.
2. Add Catch2 FetchContent to `db/CMakeLists.txt` (guarded with `if(NOT TARGET Catch2::Catch2WithMain)`).
3. Add test target in `db/CMakeLists.txt`:
```cmake
if(BUILD_TESTING)
  add_executable(chromatindb_tests
    tests/crypto/test_hash.cpp
    ...
  )
  target_link_libraries(chromatindb_tests PRIVATE
    chromatindb_lib
    Catch2::Catch2WithMain
  )
  include(Catch)
  catch_discover_tests(chromatindb_tests)
endif()
```
4. Remove test sources and Catch2 from top-level `CMakeLists.txt`.
5. Top-level `CMakeLists.txt` still has `add_subdirectory(db)` which now includes the test target.

**No include path changes needed.** Tests already use `#include "db/..."` paths because `chromatindb_lib` exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as a public include directory.

**Build behavior is identical.** `cmake --build build` from the top level still discovers and runs all tests via CTest. The only change is the physical file location and which CMakeLists.txt owns the test target.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Storing Full Blob Hash in sync_cursor_map

**What:** Using the last-seen blob hash as the cursor instead of seq_num.

**Why bad:** Hash-based cursors require scanning to find the hash's position in the sequence. Seq_num is monotonically increasing and directly seekable via libmdbx cursor `lower_bound`.

**Instead:** Use seq_num as cursor. It's the natural ordering key in the seq_map and supports O(log n) seek.

### Anti-Pattern 2: Computing Quota Usage by Scanning on Every Ingest

**What:** Counting blobs and summing sizes by iterating the seq_map/blobs_map on each ingest call.

**Why bad:** O(N) per ingest where N = blobs in namespace. For a namespace with 100K blobs, this adds ~50ms per ingest -- far worse than the crypto bottleneck.

**Instead:** Maintain a running total in quota_map, updated atomically in the same write transaction as blob store/delete.

### Anti-Pattern 3: Protocol-Level Cursor Exchange

**What:** Adding a new `SyncCursor` message type to the transport schema so peers negotiate cursor positions explicitly.

**Why bad:** Adds protocol complexity, requires schema changes, creates backward compatibility concerns. The cursor is a local optimization -- each side independently decides what to send.

**Instead:** Each peer maintains its own cursor for each remote peer. The existing NamespaceList + HashList protocol is unchanged. Fewer hashes in the HashList is transparent to the receiver.

### Anti-Pattern 4: Pre-Hash ML-DSA for Backward Compatibility

**What:** Switching from pure ML-DSA to HashML-DSA to avoid passing full messages to OQS_SIG_verify.

**Why bad:** HashML-DSA uses a different signature algorithm (FIPS 204 Algorithm 4/5 vs Algorithm 2/3). Existing signatures would not verify under HashML-DSA. This is a protocol-breaking change that invalidates all stored blobs.

**Instead:** Accept the ML-DSA verify cost as a fixed per-blob overhead. Optimize around it (eliminate redundant hashing, reduce allocations). Defer parallelism to v0.8.0.

### Anti-Pattern 5: Moving Tests Without git mv

**What:** Copying test files to db/tests/ and deleting the originals.

**Why bad:** Loses git history for 20 files with 6 milestones of evolution. `git log --follow` won't work.

**Instead:** `git mv tests/ db/tests/` in a dedicated commit. Git tracks the rename, history is preserved.

## Build Order (Dependency-Aware)

```
Phase 1: Test relocation (tests/ -> db/tests/)
  Prereq: none
  Modifies: db/CMakeLists.txt, CMakeLists.txt
  Risk: LOW (git mv, CMake update)
  Tests: All 284 existing tests pass at same location

Phase 2: Crypto throughput optimization
  Prereq: none (independent of Phase 1)
  Modifies: db/crypto/signing.cpp, db/engine/engine.cpp, db/storage/storage.h+cpp
  Risk: LOW (eliminate redundant work, no behavior change)
  Tests: Existing ingest/storage/signing tests + new benchmark comparison

Phase 3: Sync resumption
  Prereq: Phase 2 (hash caching in storage is a natural foundation)
  Modifies: db/storage/storage.h+cpp, db/sync/sync_protocol.h+cpp,
            db/peer/peer_manager.h+cpp
  Risk: MEDIUM (new persistent state, cursor edge cases)
  Tests: New cursor persistence tests, multi-peer sync resumption tests

Phase 4: Namespace quotas
  Prereq: Phase 2 (quota_map follows same pattern as hash caching)
  Modifies: db/storage/storage.h+cpp, db/engine/engine.h+cpp,
            db/config/config.h+cpp
  Risk: LOW (simple counter tracking, well-isolated)
  Tests: Quota enforcement tests, quota + expiry interaction, SIGHUP reload

Phase 5: Deletion benchmarks
  Prereq: Phase 1 (tests relocated), deletion feature (already shipped)
  Modifies: deploy/benchmark/ scripts
  Risk: LOW (new benchmark scenario only)
  Tests: Benchmark produces deletion throughput numbers

Phase 6: General cleanup
  Prereq: all above phases complete
  Modifies: various (remove stale artifacts, update READMEs)
  Risk: LOW
  Tests: Build succeeds, all tests pass
```

**Ordering rationale:**
- Test relocation first because it is zero-risk and unblocks clean db/ self-containment.
- Crypto throughput before sync resumption because the storage changes (hash passing) inform the storage API changes needed for cursors.
- Sync resumption before quotas because cursors add a sub-database, establishing the pattern that quotas follow.
- Deletion benchmarks late because they depend on everything else being stable.
- Cleanup last because it sweeps up anything missed.

**Phases 1 and 2 can run in parallel** (no file overlap). Phases 3 and 4 can also run in parallel after Phase 2 (different parts of the ingest pipeline).

## Scalability Considerations

| Concern | 1K blobs/ns | 100K blobs/ns | 1M blobs/ns |
|---------|------------|---------------|-------------|
| Sync hash exchange (no cursor) | 32 KB | 3.2 MB | 32 MB per sync round |
| Sync hash exchange (with cursor, 10 new) | 320 B | 320 B | 320 B |
| Quota lookup | O(1) | O(1) | O(1) |
| Cursor storage per peer per ns | 72 B | 72 B | 72 B |
| Cursor storage 100 peers x 50 ns | 360 KB | 360 KB | 360 KB |

Sync resumption transforms sync from O(total_blobs) to O(new_blobs) per round. For a mature network with 100K blobs per namespace and 10 new blobs per sync interval, this is a 10,000x reduction in hash exchange volume.

## Sources

- Existing codebase: all source files listed above were read directly -- HIGH confidence
- v0.6.0 benchmark report (`deploy/results/REPORT.md`): CPU bottleneck data -- HIGH confidence
- libmdbx sub-database pattern: follows existing 5-map pattern in storage.cpp -- HIGH confidence
- ML-DSA verify behavior: liboqs source + [FIPS 204 specification](https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.204.pdf) -- HIGH confidence
- ML-DSA pre-hash incompatibility: [NIST PQC FAQ](https://csrc.nist.gov/csrc/media/Projects/post-quantum-cryptography/documents/faq/fips204-sec6-03192025.pdf) and [NIST PQC Forum discussion](https://groups.google.com/a/list.nist.gov/g/pqc-forum/c/GMMKmejELfQ) -- HIGH confidence
- OQS_SIG_new allocation overhead: [liboqs source](https://github.com/open-quantum-safe/liboqs) -- HIGH confidence
- SHA3 incremental API in liboqs: [liboqs SHA3 implementation](https://github.com/open-quantum-safe/liboqs/blob/f88e6237c53d481200f9bf80c7c5fe9cde5f6a74/src/common/sha3/xkcp_sha3.c) -- MEDIUM confidence (API exists, but ML-DSA verify does not expose incremental interface)
- libmdbx cursor operations: [libmdbx GitHub](https://github.com/erthink/libmdbx) -- HIGH confidence
- [IBM HashML-DSA implementation](https://github.com/IBM/hashMLDSA) -- confirms pre-hash is a distinct algorithm -- HIGH confidence
- [ML-DSA benchmark comparison](https://medium.com/@moeghifar/post-quantum-digital-signatures-the-benchmark-of-ml-dsa-against-ecdsa-and-eddsa-d4406a5918d9) -- MEDIUM confidence (single author benchmark)
