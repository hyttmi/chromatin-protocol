# Domain Pitfalls

**Domain:** Set reconciliation replacing O(N) hash-list sync, sync rate limiting, and thread pool crypto offload for chromatindb (C++20 distributed blob store)
**Researched:** 2026-03-19
**Stack context:** libmdbx (MMAP/ACID, thread-bound transactions), liboqs (ML-DSA-87, SHA3-256), libsodium (ChaCha20-Poly1305 AEAD with connection-scoped nonce counters), Standalone Asio (C++20 coroutines, single io_context thread), FlatBuffers (deterministic encoding), 110 MiB MAX_FRAME_SIZE

## Critical Pitfalls

Mistakes that cause data corruption, security vulnerabilities, or architectural rewrites.

### Pitfall 1: AEAD Nonce Counter Access from Thread Pool Workers

**What goes wrong:** The thread pool crypto offload dispatches ML-DSA-87 verify and SHA3-256 hash to worker threads. A developer accidentally passes a reference to the Connection object (or its send/recv methods) into the thread pool task. The worker thread calls `send_encrypted()` or `recv_encrypted()`, which increments `send_counter_` or `recv_counter_` concurrently with the event loop. Two frames get the same nonce, or the counter desynchronizes between peers. ChaCha20-Poly1305 with a repeated nonce leaks the XOR of two plaintexts -- catastrophic for a security-critical protocol.

**Why it happens:** The offload boundary is conceptually simple ("just hash/verify on another thread") but the Connection object mixes stateless crypto (verify, hash) with stateful crypto (AEAD encrypt/decrypt with monotonic nonces). When building the async pipeline, it is tempting to capture `conn` or `this` in the lambda posted to the thread pool.

**Consequences:**
- Nonce reuse breaks ChaCha20-Poly1305 confidentiality (keystream XOR leak)
- Nonce desync between peers causes all subsequent frames to fail AEAD decrypt
- Race condition on `send_counter_`/`recv_counter_` (uint64_t increment is not atomic on all platforms)
- Silent data corruption -- no crash, just broken security

**Prevention:**
- Thread pool tasks must accept ONLY the raw bytes needed for the stateless operation (e.g., `std::vector<uint8_t>` for signature, pubkey, message digest) -- never a Connection reference
- Return the result via `asio::post(ioc_, completion_handler)` back to the event loop thread
- Enforce at code review: no `Connection::Ptr`, no `session_keys_`, no `send_counter_`/`recv_counter_` in any thread pool lambda capture list
- The `co_await asio::post(thread_pool_, asio::use_awaitable)` pattern naturally resumes the coroutine on the pool, then a second `co_await asio::post(ioc_, asio::use_awaitable)` resumes on the event loop -- use this two-dispatch pattern consistently

**Detection:** Compile-time: thread pool tasks should take value parameters only, not references to Connection/PeerManager. Runtime: AEAD decrypt failures in logs ("nonce desync or tampered") after adding thread pool. TSAN (ThreadSanitizer) will catch the data race on counter increment.

**Phase:** Thread Pool Crypto Offload (first phase). This is the highest-risk pitfall -- get the boundary wrong and everything breaks.

---

### Pitfall 2: libmdbx Transaction Thread Affinity Violation

**What goes wrong:** A thread pool worker needs to read blob data from storage (e.g., to compute a hash or verify a signature that requires looking up a delegation). The worker opens a read transaction on the pool thread, or worse, uses a transaction handle that was opened on the event loop thread. libmdbx returns `MDBX_THREAD_MISMATCH` and aborts, or with `MDBX_NOSTICKYTHREADS` the behavior becomes subtly undefined for write transactions.

**Why it happens:** The current Storage class is NOT thread-safe (documented in header: "Thread safety: NOT thread-safe. Caller must synchronize access."). Every storage operation opens and commits a transaction on the calling thread. The thread pool breaks this assumption. A developer might think "I just need a quick read" and call `storage_.has_blob()` from a worker thread.

**Consequences:**
- `MDBX_THREAD_MISMATCH` error crashes the node (or silently fails the operation)
- Write transactions opened on pool threads conflict with the single-writer constraint (libmdbx allows only one write transaction, and it must be committed on the same thread it started)
- Read transactions on pool threads with default TLS-based slot tracking corrupt the reader table
- Even with `MDBX_NOSTICKYTHREADS`, concurrent reads from pool threads + writes from event loop can cause snapshot isolation violations if not properly managed

**Prevention:**
- ALL storage access must remain on the event loop thread. The thread pool receives only copied byte buffers, never references to Storage/BlobEngine
- The offload pattern is: (1) read data on event loop, (2) copy bytes to thread pool task, (3) compute on pool, (4) post result back to event loop, (5) write result on event loop
- Do NOT enable `MDBX_NOSTICKYTHREADS` -- it adds complexity and the current architecture does not need it
- The engine.ingest() pipeline already computes `content_hash` and `signing_input` from in-memory `BlobData` fields -- the offload just moves those computations to the pool

**Detection:** libmdbx will assert/error on thread mismatch in debug builds. Test with TSAN. Any `MDBX_THREAD_MISMATCH` in logs is a hard bug.

**Phase:** Thread Pool Crypto Offload. Must be designed correctly from the start -- the offload boundary must be "bytes in, bool/hash out" with zero storage access.

---

### Pitfall 3: Set Reconciliation State Inconsistency with ACID Storage

**What goes wrong:** A Merkle tree (or IBLT index) is maintained alongside the blob storage to enable O(differences) sync. During a write transaction that stores a new blob, the Merkle tree/IBLT is updated in the same transaction. But during a sync round, the reconciliation structure is read at time T1, while blobs are being ingested concurrently (from other peer connections) at time T2. The Merkle root sent to the peer does not match the actual blob set, causing phantom differences or missed blobs.

**Why it happens:** The current system is single-threaded (one io_context), so reads and writes are serialized by the event loop. But set reconciliation introduces a new dimension: the reconciliation structure (Merkle tree or IBLT) must be consistent with the blob set at the moment of sync. If a blob arrives between "read Merkle root" and "send hash list for namespace X", the structure is stale.

**Consequences:**
- Peer receives a Merkle root, walks the tree, finds differences that do not actually exist (blob was just ingested between operations)
- Or peer misses a blob that was ingested between root computation and hash exchange
- With Merkle trees: a partial tree walk based on a stale root may follow paths that were restructured by a concurrent insert
- These are not catastrophic (next sync round fixes them) but they waste bandwidth and create confusing debug logs

**Prevention:**
- Since chromatindb runs on a single event loop, co_await points are the only "concurrency" boundaries. Between two synchronous storage reads (no co_await between them), the data is consistent
- The reconciliation structure and blob set MUST be read in the same synchronous block (no co_await between reading the Merkle root and the hash list for a namespace)
- If using a Merkle tree stored in libmdbx: read the root and relevant subtree in a single read transaction (libmdbx MVCC snapshot guarantees consistency within a transaction)
- If using an in-memory IBLT: build it from a single libmdbx read transaction snapshot, not incrementally updated
- Alternatively, compute the reconciliation data lazily per-sync-round from a single seq_map scan (consistent because single-threaded, no co_await during scan)

**Detection:** Sync rounds that repeatedly find 0 actual differences after requesting blobs (the diff was phantom). Log warnings when BlobRequest returns "not found" for hashes that were in the reconciliation diff.

**Phase:** Set Reconciliation (sync protocol replacement). The phase that designs the reconciliation structure must decide: stored in libmdbx (ACID-consistent) vs. in-memory (rebuilt per round) vs. incrementally maintained. Each has different consistency tradeoffs.

---

### Pitfall 4: IBLT Decode Failure Without Fallback

**What goes wrong:** An IBLT-based reconciliation is deployed. The IBLT is sized for an estimated set difference of D elements. The actual difference exceeds D (e.g., after a long disconnection), and the IBLT fails to decode. The protocol has no fallback path, so the sync round fails entirely and the peers never converge.

**Why it happens:** IBLT decode is probabilistic. Even with correct sizing, decode can fail with small probability. With incorrect sizing (difference underestimated), failure is guaranteed. The existing hash-list protocol is simple (always works, just O(N)), so developers may not realize the new protocol needs a fallback.

**Consequences:**
- Peers cannot sync after extended disconnection (difference too large for IBLT)
- Network partitions become permanent if no fallback exists
- Attacker can create many blobs to inflate the difference beyond IBLT capacity, creating a denial-of-sync attack

**Prevention:**
- ANY reconciliation protocol MUST have a fallback to full hash exchange (the current O(N) protocol) when the efficient method fails
- Design the protocol with explicit failure detection: if IBLT decode fails, fall back to full namespace hash list for that namespace
- Consider a Merkle tree approach instead of IBLT -- tree traversal always succeeds (just takes more round-trips for larger differences), no probabilistic failure
- If using IBLT: implement the "bisection" fallback from minisketch protocol tips -- on decode failure, partition the set and try smaller IBLTs on each half
- Size the IBLT conservatively (2-3x expected difference) and use the cursor system's `latest_seq_num` difference as a hint for expected difference size

**Detection:** IBLT decode failure is detectable (the decode algorithm reports success/failure). Log and count failures. Alert if failure rate exceeds threshold.

**Phase:** Set Reconciliation. The protocol design phase must specify the fallback path before implementing the efficient path.

---

### Pitfall 5: Merkle Tree Per-Write Overhead Destroying Ingest Throughput

**What goes wrong:** A Merkle tree is stored in libmdbx alongside blob data. Every blob ingest requires updating the tree: recomputing hashes along the path from the modified leaf to the root. For a tree with N blobs, this is O(log N) hash computations per write. With ML-DSA-87 verify already costing ~2ms per blob, adding O(log N) SHA3-256 computations to the write path increases latency. For namespaces with millions of blobs, the tree height is ~20, adding 20 hash computations per ingest.

**Why it happens:** Merkle trees are designed for verification, not for high-throughput writes. The canonical approach (update on every write) is correct but slow. Research shows "any additional hash operation directly translates to thousands of cycles of overhead" (ACM 2023).

**Consequences:**
- Ingest throughput regression (measurable in benchmarks)
- Write transaction duration increases (libmdbx single-writer constraint means longer lock hold times)
- The Merkle tree update cannot be offloaded to the thread pool because it requires a write transaction (libmdbx thread affinity)

**Prevention:**
- Do NOT update the Merkle tree on every ingest. Instead, use a lazy/deferred approach: mark the tree as dirty and recompute before the next sync round
- Consider a simpler structure: per-namespace XXH3 fingerprint (XOR of all blob hashes). O(1) to update on insert/delete. Peers compare fingerprints; mismatch triggers hash exchange. This is already partially implemented via `collect_namespace_hashes()` and `diff_hashes()`
- If a tree is needed for sub-namespace diff (avoiding full hash exchange on mismatch), build it lazily from seq_map at sync time, not maintained incrementally
- Keep the reconciliation data structure separate from the hot ingest path

**Detection:** Benchmark ingest throughput before and after adding reconciliation structure. Any regression >5% on small blobs is a red flag.

**Phase:** Set Reconciliation. The choice of data structure directly determines whether this pitfall applies.

## Moderate Pitfalls

### Pitfall 6: Sync Rate Limiting Too Aggressive -- Network Partition

**What goes wrong:** Sync rate limiting is added to prevent abuse (repeated SyncRequest floods). The rate limit is set too low, or the token bucket parameters do not account for legitimate large syncs. A node that has been offline for hours reconnects with thousands of blobs to sync. The rate limiter throttles or disconnects it before sync completes. The node never catches up.

**Why it happens:** The current rate limiting (token bucket on Data/Delete messages) deliberately exempts sync messages: "Sync messages (BlobTransfer, SyncRequest, etc.) are never rate-checked" (peer_manager.cpp line 436). Adding sync rate limiting changes this contract. If the limits are derived from the Data/Delete limits, they will be wrong because sync is inherently bursty (thousands of hashes + blob transfers in rapid succession).

**Consequences:**
- Legitimate peers that fall behind cannot resync (permanent data divergence)
- Bootstrap nodes that serve initial sync to new nodes get rate-limited
- In a 3-node network, if node C disconnects and reconnects, nodes A and B may both throttle C's sync, leaving C perpetually out of date
- Network effectively partitions into "fast" and "slow" nodes

**Prevention:**
- Sync rate limiting must be per-peer, not global
- Use a separate token bucket for sync vs. data messages (different burst profiles)
- Rate limit sync INITIATION (SyncRequest frequency), not sync DATA TRANSFER (BlobTransfer volume within an accepted sync round)
- Allow at least one full sync round per sync_interval_seconds without throttling
- Implement sync rate limiting as "max N concurrent syncs" + "min interval between sync rounds with same peer" rather than byte-based throttling
- Consider: rate limit SyncRequest to at most 1 per peer per sync_interval_seconds. Within an accepted sync round, no rate limit on BlobTransfer (the one-blob-at-a-time constraint already bounds memory)

**Detection:** Monitor cursor convergence across peers. If a peer's cursor never advances (always behind), check if sync rate limiting is the cause. Log when sync is rate-limited separately from data rate limiting.

**Phase:** Sync Rate Limiting. Must be designed with explicit test cases for "large resync after long disconnection" and "bootstrap initial sync".

---

### Pitfall 7: Sync Rate Limiting Too Lenient -- Reflection/Amplification Attack

**What goes wrong:** The sync rate limit allows too many SyncRequest messages. An attacker connects to a node and spams SyncRequest. Each SyncRequest triggers the responder to enumerate all namespaces, collect all hashes, and send them -- an expensive operation that scales with total stored data. The attacker disconnects before sending their own data, wasting the responder's CPU and bandwidth.

**Why it happens:** The current protocol requires the responder to send its full namespace list and hash lists in response to SyncRequest/SyncAccept. This is asymmetric: the initiator pays almost nothing (empty SyncRequest), while the responder does O(N) work. Without sync rate limiting, this is a free amplification vector.

**Consequences:**
- CPU exhaustion on the target node (hash collection for large namespaces)
- Bandwidth exhaustion (sending hash lists to attackers who never reciprocate)
- Memory pressure from building hash list vectors for all namespaces
- With set reconciliation, the Merkle tree traversal or IBLT construction is also triggered by SyncRequest, adding more work

**Prevention:**
- Rate limit SyncRequest per peer: at most 1 per sync_interval_seconds (e.g., 60 seconds)
- Count incomplete syncs (peer disconnects during sync) as strikes -- existing strike system (STRIKE_THRESHOLD = 10) handles this
- Do NOT send any data until the initiator has sent their NamespaceList first (currently both sides send simultaneously in Phase A -- consider making the initiator go first)
- Cap the total bytes/hashes sent per sync round per peer
- With set reconciliation: the reconciliation probe should be small (Merkle root or IBLT sketch) -- if the peer never responds with their probe, abort cheaply

**Detection:** Monitor `metrics_.syncs` vs. incomplete sync count. High incomplete ratio = likely abuse. Log SyncRequest frequency per peer.

**Phase:** Sync Rate Limiting. Must protect against both bandwidth amplification and CPU amplification.

---

### Pitfall 8: Thread Pool Coroutine Resumption on Wrong Executor

**What goes wrong:** A coroutine running on the event loop (`io_context`) dispatches crypto work to the thread pool via `co_await asio::post(pool, use_awaitable)`. The coroutine resumes on the thread pool thread. The next line of code accesses `peers_` (a `std::deque<PeerInfo>` owned by PeerManager), `engine_`, or `storage_` -- all of which are NOT thread-safe and assume single-threaded access from the event loop.

**Why it happens:** The Asio `co_await asio::post(executor, use_awaitable)` pattern resumes the coroutine on the TARGET executor's thread, not the original. This is by design but counter-intuitive. The developer writes:

```cpp
// Running on io_context thread
auto hash = co_await asio::post(pool, [data]() { return sha3_256(data); }, use_awaitable);
// NOW RUNNING ON POOL THREAD -- accessing PeerManager state here is UB
auto result = storage_.has_blob(ns, hash);  // WRONG: libmdbx thread mismatch
```

**Consequences:**
- libmdbx THREAD_MISMATCH errors
- Data races on PeerManager containers (peers_, etc.)
- Sporadic crashes under load (hard to reproduce, classic Heisenbug)

**Prevention:**
- Use the two-dispatch pattern: `co_await asio::post(pool, use_awaitable)` to enter pool, then `co_await asio::post(ioc_, use_awaitable)` to return to event loop
- OR use `asio::co_spawn` with a specific executor and use `asio::post` to bounce between contexts explicitly
- Create a helper function that encapsulates the round-trip:

```cpp
template<typename F>
asio::awaitable<std::invoke_result_t<F>> offload_to_pool(
    asio::thread_pool& pool, asio::io_context& ioc, F&& f) {
    co_await asio::post(pool, asio::use_awaitable);
    auto result = f();
    co_await asio::post(ioc, asio::use_awaitable);
    co_return result;
}
```

- All code after the offload helper returns is guaranteed to be on the event loop thread
- Unit test: assert `std::this_thread::get_id()` matches the event loop thread ID after returning from offload

**Detection:** TSAN catches races. Add debug-mode assertions: `assert(std::this_thread::get_id() == event_loop_thread_id_)` at the start of every PeerManager/Storage/Engine method.

**Phase:** Thread Pool Crypto Offload. The helper function should be the FIRST thing implemented, before any crypto is offloaded.

---

### Pitfall 9: Reconciliation Protocol Exceeds MAX_FRAME_SIZE

**What goes wrong:** The set reconciliation protocol sends a Merkle tree proof or IBLT sketch that exceeds the 110 MiB MAX_FRAME_SIZE. This is the exact problem the current O(N) hash list has (~3.4M blobs = 110 MiB of hashes), and the new protocol inadvertently recreates it.

**Why it happens:** A Merkle tree proof for a large namespace with many differences can be large (each internal node hash is 32 bytes, and the proof includes all sibling hashes along the path). An IBLT for a large estimated difference is also large. If the protocol sends the entire structure in one frame, it hits the same size limit.

**Consequences:**
- Connection drops when frame exceeds MAX_FRAME_SIZE (recv_raw returns nullopt)
- Sync fails for namespaces with large differences
- The "scalability fix" does not actually fix the scalability problem

**Prevention:**
- Design the reconciliation protocol to use multiple round-trips by design (not as a fallback)
- Merkle tree: exchange root, then walk subtrees interactively (each message is O(fanout * hash_size), never O(N))
- IBLT: send in chunks/cells incrementally (rateless IBLT approach -- send cells until receiver decodes)
- Set a per-message size budget well below MAX_FRAME_SIZE (e.g., 1 MiB for reconciliation messages) to leave room for concurrent message types
- The namespace list (Phase A) is already small (40 bytes per namespace). The hash list (Phase B) is the problem. The reconciliation replaces Phase B specifically -- ensure the replacement is bounded per message

**Detection:** Log reconciliation message sizes. Alert if any reconciliation message exceeds 10 MiB.

**Phase:** Set Reconciliation. Protocol design must specify message size bounds.

---

### Pitfall 10: Reconciliation Structure Conflicts with Cursor System

**What goes wrong:** The existing sync cursor system (per-peer per-namespace seq_num) is used to skip unchanged namespaces. The new set reconciliation is used for namespaces that HAVE changed. But the two systems interact poorly: the cursor says "namespace changed" (seq_num mismatch), so the reconciliation runs. But the reconciliation finds 0 differences because the cursor was stale (e.g., peer re-created the same blobs after a deletion/re-write cycle). The reconciliation wasted a round-trip for nothing. Worse: if the reconciliation updates the cursor, but the reconciliation result was incomplete (IBLT decode failure), the cursor is now wrong.

**Why it happens:** The cursor system and reconciliation system operate at different granularities. Cursors track seq_num (monotonic, per-namespace). Reconciliation tracks blob set membership (content-addressed). A namespace can have the same blobs but different seq_nums (after deletion + re-creation), or different blobs with the same max seq_num (impossible by design, but edge cases exist with zero-hash sentinels from deletion).

**Consequences:**
- Wasted reconciliation rounds (cursor says "changed" but blob set is identical)
- Cursor corruption if updated after failed/partial reconciliation
- With zero-hash sentinels: `collect_namespace_hashes()` returns sentinel hashes that do not correspond to real blobs, causing phantom differences in reconciliation

**Prevention:**
- Keep the cursor system as the FIRST gate: cursor hit = skip namespace entirely (no reconciliation needed)
- Cursor miss = run reconciliation for that namespace
- Update cursor ONLY after successful reconciliation completion (the current code already does this -- post-sync cursor update at line 880)
- Filter zero-hash sentinels from reconciliation input (the current `get_hashes_by_namespace()` returns them -- reconciliation must skip entries where hash == all-zeros)
- The cursor and reconciliation are complementary, not competing: cursor handles the common case (no changes), reconciliation handles the uncommon case (some changes, but not all)

**Detection:** Log when reconciliation finds 0 actual differences after cursor indicated a change (phantom reconciliation). Track phantom rate -- if it is high, the cursor is too sensitive.

**Phase:** Set Reconciliation. Must explicitly define the cursor-reconciliation interaction in the protocol design.

## Minor Pitfalls

### Pitfall 11: Thread Pool Dispatch Overhead Worse Than Inline Crypto for Small Blobs

**What goes wrong:** Every blob verification is dispatched to the thread pool, including tiny blobs (< 1 KB). The overhead of posting to the pool (context switch, queue contention, callback dispatch) exceeds the cost of inline SHA3-256 + ML-DSA-87 verify for small blobs. Small-blob throughput regresses.

**Prevention:** Start with unconditional offload (simpler, correct). Measure. If small-blob regression is observed, add a size threshold: blobs below the threshold are verified inline on the event loop. The v1.0.0 requirements explicitly defer this to PERF-11 (future), so do not over-engineer in the first phase.

**Phase:** Thread Pool Crypto Offload (implementation), Benchmark Validation (measurement).

---

### Pitfall 12: FlatBuffers Deterministic Encoding Broken by Reconciliation Metadata

**What goes wrong:** The reconciliation protocol adds metadata to the wire format (e.g., Merkle tree node hashes, IBLT cells). If this metadata is encoded using FlatBuffers without `ForceDefaults(true)`, the encoding is non-deterministic. Peers compute different hashes for the same logical message, breaking signature verification or content-addressed dedup of reconciliation structures.

**Prevention:** All new FlatBuffers tables MUST use `ForceDefaults(true)`. This is already established practice (documented in Key Decisions). New wire message types for reconciliation should be reviewed for this. Alternatively, use the existing hand-rolled big-endian binary encoding (like NamespaceList and HashList) for reconciliation messages -- simpler and deterministic by construction.

**Phase:** Set Reconciliation (wire format design).

---

### Pitfall 13: Reconciliation Allows Namespace Enumeration Attack

**What goes wrong:** The reconciliation protocol reveals which namespaces exist and how many blobs each contains, even to peers that should not have this information. The current protocol already leaks this (NamespaceList in Phase A), but a more detailed reconciliation (Merkle tree with namespace-scoped subtrees) leaks the internal structure.

**Prevention:** This is an existing information leak, not new. The sync_namespaces filter already limits which namespaces are advertised. Closed-mode nodes (allowed_keys) restrict who can connect at all. Accept this as a known limitation of the database layer -- the relay layer (Layer 2) is responsible for higher-level privacy.

**Phase:** Not a blocker for any phase. Document as a known limitation.

---

### Pitfall 14: Thread Pool Shutdown Race with In-Flight Crypto Operations

**What goes wrong:** During graceful shutdown, the thread pool is destroyed while crypto operations are still in flight. The completion handler tries to post back to the io_context, which is already stopped. The handler is silently dropped, or worse, accesses destroyed objects.

**Prevention:**
- Stop accepting new work (set a flag) before stopping the thread pool
- `thread_pool.join()` waits for all in-flight work to complete
- Ensure the io_context is still running when the completion handlers fire -- stop the pool BEFORE stopping the io_context
- Shutdown order: (1) stop accepting new connections, (2) drain in-flight sync rounds, (3) join thread pool, (4) stop io_context

**Detection:** ASAN/LSAN detect use-after-free. Test: initiate graceful shutdown during active sync with large blobs.

**Phase:** Thread Pool Crypto Offload (lifecycle management).

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Mitigation |
|-------------|---------------|------------|
| Thread Pool Crypto Offload | AEAD nonce access from worker thread (Pitfall 1) | Offload only stateless ops; never pass Connection ref to pool |
| Thread Pool Crypto Offload | libmdbx thread mismatch (Pitfall 2) | All storage access on event loop; pool receives copied bytes only |
| Thread Pool Crypto Offload | Coroutine resumes on wrong thread (Pitfall 8) | Two-dispatch pattern with helper function; assert thread ID |
| Thread Pool Crypto Offload | Shutdown race (Pitfall 14) | Join pool before stopping io_context |
| Set Reconciliation | No fallback on decode failure (Pitfall 4) | Always implement hash-list fallback for any efficient method |
| Set Reconciliation | Per-write Merkle overhead (Pitfall 5) | Lazy/deferred tree build, not per-ingest update |
| Set Reconciliation | Exceeds MAX_FRAME_SIZE (Pitfall 9) | Multi-round-trip protocol design; per-message size budget |
| Set Reconciliation | Cursor-reconciliation interaction (Pitfall 10) | Cursor as first gate; reconciliation only on cursor miss |
| Set Reconciliation | ACID consistency window (Pitfall 3) | Read reconciliation data + blob hashes in same synchronous block |
| Sync Rate Limiting | Too aggressive -- partitions network (Pitfall 6) | Rate limit initiation, not transfer; separate bucket for sync |
| Sync Rate Limiting | Too lenient -- amplification attack (Pitfall 7) | Limit SyncRequest frequency; count incomplete syncs as strikes |
| Benchmark Validation | Small-blob regression from pool overhead (Pitfall 11) | Measure; threshold if needed (deferred to PERF-11) |

## Sources

- [libmdbx transaction thread affinity documentation](https://github.com/erthink/libmdbx/blob/master/docs/_starting.md) -- MDBX_THREAD_MISMATCH behavior, MDBX_NOSTICKYTHREADS semantics (HIGH confidence)
- [libmdbx transaction API](https://libmdbx.dqdkfa.ru/group__c__transactions.html) -- Thread binding rules, write transaction same-thread requirement (HIGH confidence)
- [Asio threads documentation](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/core/threads.html) -- Thread safety rules, completion handler thread guarantees (HIGH confidence)
- [Asio C++20 coroutine support](https://think-async.com/Asio/asio-1.20.0/doc/asio/overview/core/cpp20_coroutines.html) -- Executor binding for coroutine resumption (HIGH confidence)
- [minisketch protocol tips](https://github.com/sipa/minisketch/blob/master/doc/protocoltips.md) -- IBLT fallback, bisection, DoS mitigation for set reconciliation (HIGH confidence, Bitcoin Core project)
- [Practical Rateless Set Reconciliation (SIGCOMM 2024)](https://dl.acm.org/doi/10.1145/3651890.3672219) -- IBLT sizing without a priori difference estimation (MEDIUM confidence)
- [CertainSync: Rateless Set Reconciliation with Certainty](https://eprint.iacr.org/2025/623.pdf) -- Parameterless reconciliation approach (MEDIUM confidence, recent preprint)
- [Towards Merkle Trees for High-Performance Data Systems (ACM 2023)](https://dl.acm.org/doi/pdf/10.1145/3595647.3595651) -- Per-write hash computation overhead, parallelization challenges (MEDIUM confidence)
- [Quadrable: Merkle tree database on LMDB](https://github.com/hoytech/quadrable) -- Practical ACID Merkle tree implementation (MEDIUM confidence, real implementation)
- chromatindb source: `db/net/connection.h` lines 133-135 (send_counter_, recv_counter_ member variables) -- nonce state is connection-scoped (HIGH confidence, direct code inspection)
- chromatindb source: `db/storage/storage.h` line 67 ("Thread safety: NOT thread-safe") -- storage thread model (HIGH confidence, direct code inspection)
- chromatindb source: `db/peer/peer_manager.cpp` lines 436-448 -- current rate limiting exempts sync messages (HIGH confidence, direct code inspection)
- chromatindb source: `db/sync/sync_protocol.cpp` lines 28-35 -- hash collection reads from seq_map index (HIGH confidence, direct code inspection)
