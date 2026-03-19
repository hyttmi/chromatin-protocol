# Feature Landscape

**Domain:** Sync set reconciliation, sync rate limiting, and thread pool crypto offload for a decentralized PQ-secure blob store
**Researched:** 2026-03-19
**Overall confidence:** HIGH (set reconciliation) / HIGH (thread pool offload patterns) / HIGH (sync rate limiting)

## Context

chromatindb v0.7.0 shipped with 18,000+ LOC C++20, 313+ tests, sync cursors, and crypto hot-path optimizations. A fundamental protocol scaling flaw remains: sync Phase B exchanges the FULL hash list for every namespace with new data. This is O(N) in total blobs per namespace, not O(differences). A namespace with 1M blobs forces 32 MB of hashes on the wire even for a single new blob. At ~3.4M blobs the hash list hits MAX_FRAME_SIZE (110 MiB) and breaks the connection entirely.

Additionally, sync messages bypass rate limiting (only Data and Delete are metered), enabling reflection/resource exhaustion attacks via repeated sync initiation. The event loop is single-threaded -- ML-DSA-87 verify blocks all I/O during large blob processing.

Existing sync infrastructure: SyncRequest/SyncAccept handshake, Phase A (send NamespaceList + HashLists), Phase B (receive NamespaceList + HashLists), Phase C (compute diffs, request/transfer blobs one-at-a-time). Sync cursors skip hash exchange for unchanged namespaces. Per-peer token-bucket rate limiting on Data/Delete messages already exists.

---

## Table Stakes

Features that are mandatory for this milestone. Without these the protocol has a known scaling cliff.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Set reconciliation replacing hash-list exchange | Current O(N) hash exchange per namespace is the single biggest protocol scaling flaw. It linearly degrades with data growth and has a hard cliff at ~3.4M blobs per namespace (110 MiB frame limit). Every production replication system uses sub-linear reconciliation. | High | Replaces Phase A/B hash-list exchange. Must interoperate with existing sync cursors. |
| Sync rate limiting | Sync messages are explicitly exempted from rate limiting (peer_manager.cpp line 436). A malicious peer can initiate unlimited SyncRequests, each triggering full namespace enumeration + hash collection + reconciliation computation. This is a known resource exhaustion vector. | Medium | Extends existing token-bucket infrastructure. Per-peer sync request metering. |
| Thread pool crypto offload | ML-DSA-87 verify + SHA3-256 hash block the single-threaded event loop for 20-30ms per 1 MiB blob. During sync of large blobs, all other I/O (other peers, PEX, pub/sub, client queries) stalls. v0.7.0 serial optimizations helped but the ceiling remains. | Medium-High | Must preserve single-thread invariants for Storage/Engine mutations. Only CPU-bound crypto moves off the event loop. |

## Set Reconciliation: Approach Analysis

Four approaches were researched. The recommendation is negentropy (range-based set reconciliation).

### Approach 1: negentropy / Range-Based Set Reconciliation (RECOMMENDED)

**What it is:** Recursive range fingerprinting. Both peers compute fingerprints over sorted ranges of their elements. Where fingerprints match, that range is identical -- skip it. Where they differ, subdivide and recurse until individual differing elements are identified.

**How it works with chromatindb:**
- Each blob maps to a (timestamp, 32-byte ID) pair. The timestamp is the blob's `timestamp` field (uint64, already in BlobData). The ID is the SHA3-256 content hash (already 32 bytes, already stored in seq_map).
- Fingerprint = addition mod 2^256 of IDs in range, concatenated with count, hashed to 16 bytes. This is an internal computation -- does NOT need to match the content hash algorithm.
- Initiator sends a fingerprint covering its full range. Responder compares against its own fingerprint for the same range. Matching = skip. Mismatching = subdivide. Base case = send individual IDs.
- ~3-5 round-trips for 1M elements with branching factor 16. Communication is O(differences), not O(total).

**Why this approach:**
1. **Production-proven at scale.** The Nostr ecosystem uses negentropy (NIP-77) in strfry relays, routinely synchronizing datasets of tens of millions of elements.
2. **C++ reference implementation exists.** Header-only, MIT-licensed, with Vector and BTreeMem storage backends. The storage interface is templated -- custom backends are possible.
3. **32-byte IDs are native.** negentropy's protocol requires exactly 32-byte IDs. chromatindb already uses 32-byte SHA3-256 content hashes as blob IDs. Perfect fit.
4. **Stateless design.** Unlike Merkle trees, RBSR does not expose tree structure in the protocol. Both sides can use different internal storage layouts.
5. **Frame size limit support.** negentropy natively supports configurable frame size limits, directly solving the MAX_FRAME_SIZE cliff.
6. **Interoperates with sync cursors.** Cursors short-circuit reconciliation for unchanged namespaces. negentropy handles the actual reconciliation for changed namespaces.
7. **No per-connection persistent state.** Reconciliation is computed fresh from the data each time.

**OpenSSL dependency removal:** negentropy uses SHA-256 (from OpenSSL) in exactly one place: `Accumulator::getFingerprint()`. Replace with SHA3-256 from liboqs (already in the project). This is a 5-line patch to `types.h`. SHA3-256 is strictly stronger than SHA-256 for this internal fingerprint use. Since chromatindb nodes only sync with other chromatindb nodes (not Nostr relays), using a different internal hash is transparent.

### Approach 2: Merkle Trees

**Why NOT:**
1. Rigid tree structure requires maintaining a parallel data structure alongside libmdbx.
2. Per-connection state: concurrent syncs with multiple peers require copy-on-write snapshots.
3. Adversarial worst-case: attacker can craft data that forces full dataset transfer.
4. 4-9x higher bandwidth than RBSR for same workload (per Rateless IBLT paper benchmarks).
5. More code than integrating negentropy's header-only library.

### Approach 3: IBLT / Rateless IBLT

**Why NOT (for now):**
1. Classic IBLTs need the difference size estimated in advance. Underestimate = decode failure.
2. No production-quality C++ implementation exists. Available code is research-quality.
3. Decode failure requires fallback to full hash exchange (added complexity).
4. No frame size limit support.

### Approach 4: Minisketch / PinSketch

**Why NOT:**
1. 64-bit element limit. chromatindb uses 256-bit (SHA3-256) hashes.
2. Practical only for up to ~4096 differences.
3. Requires difference count estimation.

## Differentiators

Features that improve quality beyond the minimum but are not strictly required.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| negentropy SubRange for incremental sync | Instead of reconciling the entire namespace, reconcile only the timestamp range since the last sync cursor. Reduces reconciliation scope from O(total) to O(recent). | Low (built into negentropy) | SubRange is a storage adapter. Combined with sync cursors, provides O(new) reconciliation even for large namespaces. |
| Pipelined blob verification | While blob N is being stored to libmdbx (on event loop), blob N+1 is being verified (on thread pool). Overlaps I/O-bound storage with CPU-bound crypto. | Medium | Natural fit with one-blob-at-a-time protocol. Pipeline depth of 2 is sufficient. |
| Sync backpressure signaling | When reconciliation discovers many missing blobs, pace blob requests to avoid overwhelming either side. | Low | Cap outstanding blob requests. Already partially implemented via MAX_HASHES_PER_REQUEST. |
| Adaptive sync interval | If reconciliation found zero differences, increase the sync interval for this peer. If many differences, decrease it. | Low | Heuristic on top of existing sync_interval_seconds. No protocol change. |

## Anti-Features

Features to explicitly NOT build.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| Custom set reconciliation protocol | negentropy exists, is production-proven, MIT-licensed, and purpose-built for this use case. | Integrate negentropy. Patch the SHA-256 call to use SHA3-256 from liboqs. |
| Merkle tree persistent index | Doubles write amplification, requires copy-on-write for concurrent syncs, adds complexity with no benefit over negentropy. | Use negentropy Vector backend populated from libmdbx seq_map per sync round. |
| Full multithreaded Storage/Engine | Requires locks on every read/write path, fundamentally changes the architecture. | Offload only CPU-bound crypto. Post results back to event loop for storage mutations. |
| Sync encryption separate from transport | PQ-encrypted transport already encrypts all traffic. | Rely on existing transport encryption. negentropy messages are opaque bytes inside encrypted frames. |
| IBLT fallback for large differences | Adds complexity without clear benefit. negentropy handles large differences by design. | Accept negentropy's round-trip cost. For massive initial syncs, the bottleneck is blob transfer, not reconciliation. |
| Distributed rate limit coordination | Each node enforces its own limits independently. | Node-local per-peer limits. |

## Feature Details

### Set Reconciliation via negentropy

**Current protocol flow (per namespace with new data):**
```
Initiator                          Responder
    |--- NamespaceList (all ns) --->|
    |--- HashList (ALL hashes) ---->|  // O(N) -- THE PROBLEM
    |--- SyncComplete ------------->|
    |<-- NamespaceList (all ns) ----|
    |<-- HashList (ALL hashes) -----|  // O(N) -- THE PROBLEM
    |<-- SyncComplete --------------|
    |--- BlobRequest (missing) ---->|
    |<-- BlobTransfer (one) --------|
```

**Target protocol flow (per namespace with new data):**
```
Initiator                          Responder
    |--- NamespaceList (all ns) --->|
    |<-- NamespaceList (all ns) ----|
    [cursor check: skip unchanged namespaces]
    |--- Reconcile (initial msg) -->|  // ~few KB fingerprint
    |<-- Reconcile (response) ------|  // ~few KB fingerprint
    |--- Reconcile (refinement) --->|  // only for differing ranges
    |<-- Reconcile (done, IDs) -----|  // individual differing IDs
    [initiator knows: haveIds + needIds]
    |--- BlobRequest (need) ------->|
    |<-- BlobTransfer (one) --------|
    |--- BlobTransfer (have) ------>|  // send blobs they need
```

**Communication cost comparison:**
- Mostly-synced (10 new blobs out of 100K): ~3 round-trips, ~2 KB (vs 3.2 MB for full hash list)
- 1M blobs, 1 difference: ~5 round-trips, ~4 KB (vs 32 MB)
- Completely divergent (fresh peer, 100K blobs): ~5 round-trips, proportional to difference size

**Storage backend:** Use negentropy's `Vector` storage. Construct per-sync-round from existing `get_hashes_by_namespace()`. For 100K blobs, this is ~4 MB held during the sync round -- acceptable.

**Integration with wire format:**
New `Reconcile` TransportMsgType carrying namespace_id + opaque negentropy bytes. Existing NamespaceList exchange remains for cursor-based skip decisions.

### Sync Rate Limiting

**Current state:** Rate limiting (peer_manager.cpp line 435-448) checks only Data and Delete. Sync messages are explicitly exempted.

**Target behavior -- three layers:**

1. **Sync initiation frequency limiting.** Per-peer cooldown: max 1 SyncRequest per `sync_cooldown_seconds` (default 30). Exceeding records a strike.

2. **Sync data byte-rate metering.** Extend existing token bucket to include sync BlobTransfer messages. Same `rate_limit_bytes_per_sec` config.

3. **Concurrent sync session limit.** Max N total concurrent syncs across all peers (default 3). Prevents coordinated attack.

**Gentle response:** For sync rate violations, delay or skip the sync round rather than disconnect. Sync is protocol-initiated, not solely malicious. Reserve disconnection for egregious abuse (repeated violations trigger strike system).

### Thread Pool Crypto Offload

**Pattern:**
```
Event loop:   receive blob -> post(pool, verify_task) -> co_await -> store to libmdbx
Pool worker:  build_signing_input -> SHA3-256 -> ML-DSA-87 verify -> return result
Event loop:   [receives result] -> storage.store_blob() -> send ack
```

**Thread safety:**
- Crypto functions are stateless -- safe with copied inputs on any thread.
- OQS_SIG context: make thread_local (from v0.7.0 static, now per-pool-thread).
- Storage/Engine: called ONLY from event loop thread.

**Expected improvement:** On 6-core machine, event loop free during crypto. Multi-peer sync scales with thread count. Single-peer throughput improves ~1.5x from pipeline overlap.

## Feature Dependencies

```
negentropy integration --> new Reconcile wire message type
                      --> preserves: sync cursors, NamespaceList, BlobRequest/Transfer

Sync rate limiting    --> extends existing token bucket
                      --> adds sync frequency counter per peer
                      --> independent of reconciliation approach

Thread pool offload   --> asio::thread_pool (already in Asio 1.38.0)
                      --> thread_local OQS_SIG (from v0.7.0)
                      --> independent of reconciliation approach
                      --> benefits both sync and direct ingest
```

**Ordering dependencies:**
- negentropy integration is the largest change and should come first. It replaces the HashList code path.
- Sync rate limiting should come after negentropy because rate-limited message types change.
- Thread pool is independent and can be built after the protocol is stable.

## MVP Recommendation

Prioritize:
1. **negentropy set reconciliation** -- fixes the fundamental protocol scaling flaw.
2. **Sync rate limiting** -- closes the abuse vector. Low effort.
3. **Thread pool crypto offload** -- breaks the single-thread ceiling.

Defer: Per-namespace reconciliation parallelism, adaptive sync intervals, SubRange optimization.

## Sources

### Primary (HIGH confidence)
- [negentropy protocol and reference implementations (C++, JS, Rust) -- MIT licensed](https://github.com/hoytech/negentropy)
- [Range-Based Set Reconciliation -- comprehensive analysis by Doug Hoyte](https://logperiodic.com/rbsr.html)
- [NIP-77: Negentropy Syncing -- Nostr protocol extension](https://nips.nostr.com/77)
- [RBSR academic paper (arXiv 2212.13567)](https://arxiv.org/abs/2212.13567)
- [strfry Nostr relay -- production negentropy deployment](https://github.com/hoytech/strfry)
- Codebase: `db/peer/peer_manager.cpp` (sync flow, rate limiting), `db/sync/sync_protocol.h` (HashList), `db/storage/storage.h` (seq_map, SyncCursor)

### Secondary (MEDIUM confidence)
- [Practical Rateless Set Reconciliation -- SIGCOMM 2024](https://arxiv.org/abs/2402.02668) -- IBLT/Merkle bandwidth comparison
- [minisketch library (bitcoin-core)](https://github.com/bitcoin-core/minisketch) -- 64-bit element limit documented
- [Asio thread_pool reference](https://think-async.com/Asio/asio-1.22.0/doc/asio/reference/thread_pool.html)
- [Asio co_spawn + executor switching](https://blog.vito.nyc/posts/make-it-async/)

### Verified implementation details (HIGH confidence)
- negentropy C++ is header-only, sole dep is OpenSSL (SHA-256 in `Accumulator::getFingerprint()`)
- SHA-256 usage is a single `SHA256()` call in types.h -- replaceable with SHA3-256
- 32-byte ID format is protocol-mandated -- matches chromatindb's SHA3-256 content hashes
- 64-bit timestamp is protocol-mandated -- chromatindb BlobData has uint64 timestamp
- Frame size limit is a constructor parameter -- addresses MAX_FRAME_SIZE constraint
- strfry production use: "10s of millions of elements" synchronized routinely
