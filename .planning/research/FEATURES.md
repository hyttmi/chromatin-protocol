# Feature Landscape

**Domain:** Sync resumption, namespace quotas, and crypto throughput optimization for a decentralized PQ-secure blob store
**Researched:** 2026-03-16
**Overall confidence:** HIGH (primary analysis from existing codebase + domain knowledge; crypto perf numbers MEDIUM)

## Context

chromatindb v0.6.0 shipped with 17,775 LOC C++20, 284 tests, and Docker benchmark infrastructure proving the system works. The v0.6.0 benchmarks revealed a critical performance bottleneck: 1 MiB blob ingest/sync runs at 15 blobs/sec with 96% CPU on the receiving node. This milestone (v0.7.0) addresses that bottleneck alongside production-readiness features: sync resumption (avoid redundant work), namespace quotas (prevent abuse), and cleanup.

The existing codebase already has: hash-list diff sync (full bidirectional), namespace-scoped sync filtering, per-peer NamespaceList exchange with seq_num metadata, global storage limits with StorageFull signaling, DARE, token-bucket rate limiting, and persistent peer lists.

---

## Table Stakes

Features that are expected for a production-ready distributed storage node. Missing any of these would be a real operational problem.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Sync resumption (per-peer cursors) | Without it, every sync round exchanges ALL hashes for ALL namespaces -- O(total_blobs) per peer per round. Nodes with 100K+ blobs waste bandwidth and CPU diffing identical data every 60 seconds. Every production replication system tracks sync position. | Medium | Requires per-peer persistent state (seq_num cursor per namespace per peer). Touches peer_manager sync flow. NamespaceList infrastructure already carries the needed metadata. |
| Large blob crypto throughput fix | 15.3 blobs/sec at 1 MiB with 96% CPU is unusable for any real workload involving documents or images. A node syncing media blobs would be permanently CPU-saturated. The benchmark proved this -- it is not hypothetical. | Medium-High | Root cause: serial crypto on a single thread -- SHA3-256 hashing + ML-DSA-87 verify + redundant re-encoding, all per blob. Multiple optimization angles available. |
| Deletion benchmarks | Tombstone creation, sync propagation, and GC of expired tombstones all have performance characteristics that are currently unmeasured. Deletion was added in v3.0 but never benchmarked. | Low | Extends existing Docker benchmark infrastructure (v0.6.0). No new chromatindb code needed. |
| Test relocation into db/ | Tests for the database component live in top-level `tests/`. This breaks the self-contained component contract established in v0.5.0. Anyone consuming db/ as a library gets the component but no tests. | Low | Mechanical file moves + CMakeLists.txt updates. Same pattern as the v2.0 source restructure (14 minutes). |

## Differentiators

Features that set the system apart or significantly improve operational quality. Not strictly expected but high value.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Namespace storage quotas (per-namespace byte+count limits) | Prevents a single namespace from consuming disproportionate storage. Critical for multi-tenant node operation where relay operators share infrastructure. Without it, one abusive namespace can fill the entire node, and the global max_storage_bytes provides no per-namespace protection. | Medium | New config, new storage tracking (quota_map sub-database), enforcement in ingest pipeline. |
| Sync cursor persistence across restarts | Cursors that survive node restart avoid re-diffing everything on reboot. Reduces first-sync-after-restart from O(total_blobs) to O(delta_since_shutdown). | Low-Medium | Requires persisting cursor state to libmdbx or peers.json. Builds on sync resumption. |
| Per-namespace blob count limits | Complementary to byte limits. A namespace writing millions of tiny blobs stays under byte limits but explodes the seq_map index and sync hash-list exchange. | Low (piggybacks on byte quota infrastructure) | Same enforcement point, same tracking mechanism. |
| Verified-hash dedup before crypto | Check `storage_.has_blob()` before running signature verification on sync-received blobs. O(1) btree lookup that prevents wasted ML-DSA-87 verification when a blob arrived from another peer between hash-list exchange and blob transfer. | Low | Marginal benefit but zero cost to implement. |

## Anti-Features

Features to explicitly NOT build.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| Multithreaded ingest pipeline | chromatindb runs on a single io_context thread by design. All data structures (Storage, PeerManager, Engine) are explicitly NOT thread-safe. Introducing thread pools for the full ingest pipeline would require thread-safe wrappers everywhere, fundamentally changing the architecture. | Offload only the CPU-bound crypto (hash + verify) to a worker thread, post results back to the io_context. Keep all storage/state mutations on the single thread. |
| Per-blob sync cursors | Tracking individual blob positions per peer is excessive granularity. A sync round transfers batches, and the meaningful resumption unit is "which namespaces have new data since last sync." | Per-peer, per-namespace seq_num cursors. Resume at namespace granularity. |
| Hardware crypto acceleration (SHA3-NI, AES-NI) | The project uses liboqs SHA3-256 (software Keccak) and libsodium ChaCha20-Poly1305. Adding hardware acceleration paths requires conditional compilation, CPU feature detection, and runtime dispatch -- significant complexity for marginal gain on the actual bottleneck (ML-DSA-87 verify is the dominant cost, not SHA3). The Ryzen 5 5600U test hardware has no SHA3 instructions (SHA3-NI is Intel Ice Lake server+). | Focus on reducing redundant work and async offloading. |
| Chunked/streaming blob verification | Splitting signature verification across blob chunks requires a fundamentally different signing scheme. ML-DSA-87 requires the full message for verification -- there is no incremental API in liboqs. | Accept per-blob verification cost. Reduce it by eliminating redundant computation and parallelizing across blobs (not within a blob). |
| Complex quota policies (tiered, time-based, burst, priority) | YAGNI. The database is intentionally dumb. Per-namespace byte+count hard limits are sufficient. Sophisticated quota policies (burst allowances, time-weighted averages, priority classes) belong in the relay layer which understands application semantics. | Simple hard limits: max_bytes_per_namespace, max_blobs_per_namespace. Reject at ingest with a clear error. |
| Quota negotiation protocol | Peers do not need to know each other's quota policies. Quotas are node-local operational configuration, exactly like max_storage_bytes. | Node rejects blobs that exceed its quotas. The rejection is a normal ingest failure. No new wire messages needed. |
| Pre-hashing signing input (HashML-DSA) | FIPS 204 defines a "HashML-DSA" mode where the caller pre-hashes the message before signing/verifying. This would eliminate the large-message penalty entirely. However: it changes the signing scheme (breaking all existing blobs), and liboqs does not currently expose a separate HashML-DSA API -- the standard OQS_SIG_sign/verify always use the "pure" mode. | If liboqs adds HashML-DSA support in a future release, adopt it. For v0.7.0, optimize the surrounding work (eliminate redundant encoding, cache OQS context, async offload). |

## Feature Details

### Sync Resumption

**Current behavior:** Every sync round (default 60s), both peers exchange their FULL namespace list and ALL blob hashes per namespace. For a node with 10K blobs across 50 namespaces, that means encoding, sending, and diffing ~320 KiB of hashes every round, even when nothing changed. The diff itself is O(n) (hash set construction + lookup), and the encoding/sending is O(n) on the wire.

**Target behavior:** Each node tracks the last-seen seq_num per namespace per peer. On sync, both sides first exchange their NamespaceList (namespace + latest_seq_num pairs -- this already happens). If a namespace's seq_num has not advanced since the last sync with this peer, skip the hash-list exchange for that namespace entirely. Only exchange hash lists for namespaces with new data.

**Cursor granularity: per-peer, per-namespace seq_num.**

Rationale:
- Per-peer because different peers may be at different sync positions (one peer might be a fresh late-joiner, another fully synced).
- Per-namespace because namespaces are the natural partition boundary. The seq_num index already exists and is monotonically increasing per namespace.
- seq_num (not timestamp) because seq_num is local, monotonic, and gap-free for new writes. Timestamps are untrusted writer-provided values.

**What the cursor stores:**
```
peer_cursors_: map<peer_address, map<namespace_id, last_synced_seq_num>>
```

**How it works:**
1. Node A connects to Node B. Both exchange NamespaceList (this already happens in Phase A of sync). The NamespaceList contains `(namespace_id, latest_seq_num)` pairs.
2. Node A compares B's latest_seq_nums against its stored cursors for B. For each namespace where B's seq > cursor, that namespace has new data -- proceed with hash-list exchange. For namespaces where B's seq == cursor, skip entirely (no hash list needed).
3. After successful sync of a namespace (all blobs transferred), update cursor to B's latest_seq_num for that namespace.
4. On peer disconnect/reconnect: cursors persist (in memory at minimum, optionally on disk), so the next sync resumes where it left off.

**Existing infrastructure that helps:**
- `NamespaceList` already carries `(namespace_id, latest_seq_num)` pairs -- the exact data needed for cursor comparison. No new wire messages required.
- `seq_num` is monotonically increasing per namespace -- safe for cursor advancement.
- `PersistedPeer` already exists in peer_manager.h with `address` and `last_seen` -- can be extended with cursor data.
- `peers.json` persistence already exists -- cursor state can be stored alongside peer addresses.

**Edge cases:**
- New peer (no cursor): sync everything (cursor defaults to 0, meaning "sync all").
- Namespace expiry removes blobs: seq_nums have gaps, but cursor is still valid because it tracks "everything up to seq X was synced." New blobs get higher seq_nums.
- Tombstone creates new seq entry: correctly detected as "something changed" because latest_seq_num advances.
- Node restart without cursor persistence: falls back to full sync once. Acceptable for initial implementation; persistence can be added as a follow-up.
- seq_num rollback (theoretically impossible since seq_nums are monotonically assigned): not a concern.

**Complexity:** Medium. No new wire messages. The main work is:
1. Adding cursor storage (in-memory map, keyed by peer address or pubkey).
2. Modifying `run_sync_with_peer` and `handle_sync_as_responder` to check cursors before exchanging hash lists in Phase A.
3. Updating cursors after successful namespace sync in Phase C.
4. Optional: persisting cursors in peers.json or a dedicated libmdbx sub-database.

**Dependencies:** None on other v0.7.0 features. Can be built independently.

### Namespace Storage Quotas

**Current behavior:** Only global storage limit exists (max_storage_bytes in Config, enforced in BlobEngine::ingest Step 0b). A single namespace can fill the entire node. The StorageFull signal tells peers the node is full, but it does not distinguish which namespace caused it.

**Target behavior:** Per-namespace limits on total bytes stored and total blob count. Enforcement at the ingest pipeline, before signature verification (cheap to check). Configurable with defaults and per-namespace overrides.

**Quota dimensions: bytes AND count.**

Rationale:
- Bytes alone is insufficient: a namespace writing millions of 1-byte blobs stays under byte limits but explodes the seq_map index (one entry per blob) and sync hash-list exchange.
- Count alone is insufficient: a namespace writing a few 100 MiB blobs could exhaust storage quickly.
- Both together provide complete protection.

**Configuration:**
```json
{
  "namespace_quotas": {
    "default_max_bytes": 1073741824,
    "default_max_blobs": 100000,
    "overrides": {
      "abc123...hex...": { "max_bytes": 10737418240, "max_blobs": 1000000 }
    }
  }
}
```

- `default_max_bytes` / `default_max_blobs`: apply to all namespaces not in overrides. 0 = unlimited (no quota for that dimension).
- `overrides`: per-namespace override by 64-char hex namespace ID. Allows the node operator's own namespace or trusted relay namespaces to have higher limits.
- SIGHUP-reloadable (same pattern as allowed_keys and sync_namespaces).

**Enforcement point:** In `BlobEngine::ingest()`, after Step 0b (global capacity check), before Step 1 (structural checks). This is Step 0c -- cheap O(1) lookup before any crypto operations.

**Tracking mechanism:**

New libmdbx sub-database `quota_map`: key = `[namespace:32]`, value = `[used_bytes:u64BE][blob_count:u64BE]`. Updated atomically in the same write transaction as store_blob. O(1) lookup on ingest, O(1) update on store. Adds 16 bytes per namespace of overhead -- negligible.

Counter maintenance:
- `store_blob`: increment bytes (by encoded blob size) and count (by 1) in same txn.
- `delete_blob_data`: decrement bytes and count in same txn.
- `run_expiry_scan`: decrement for each expired blob in the scan loop.

**Interaction with existing features:**
- Tombstones: exempt from quota (small at 36 bytes, and they free space by deleting targets). Same logic as the existing global storage_full exemption in engine.cpp.
- Delegation blobs: count against the owner's namespace quota (they are stored in the owner's namespace).
- Sync ingest: same quota enforcement path. Synced blobs from a namespace that is over quota are rejected silently.
- Global max_storage_bytes: checked before namespace quota. If global limit is hit, namespace quota is irrelevant.

**Complexity:** Medium. Requires:
1. New config fields + validation + SIGHUP reload.
2. New quota_map sub-database in Storage (sixth sub-database alongside blobs, sequence, expiry, delegation, tombstone).
3. Counter maintenance in store_blob, delete_blob_data, run_expiry_scan.
4. Enforcement check in BlobEngine::ingest() at Step 0c.
5. New IngestError variant (namespace_quota_exceeded).
6. Tests for quota enforcement, counter maintenance, and SIGHUP reload.

**Dependencies:** None on other v0.7.0 features. Can be built independently.

### Large Blob Crypto Throughput Optimization

**Root cause analysis (from benchmark data + code review):**

The sync path calls `sync_proto_.ingest_blobs()`, which calls `engine_.ingest()` per blob. For each 1 MiB blob, `ingest()` does:

1. **build_signing_input()** (engine.cpp:107): Allocates ~1 MiB vector, copies namespace(32) + data(1 MiB) + ttl(4) + timestamp(8).
2. **Signer::verify()** (engine.cpp:110): Creates a NEW `OQS_SIG` context (`OQS_SIG_new`), passes the ~1 MiB signing input to `OQS_SIG_verify` (which internally hashes it via SHA3/SHAKE before lattice math), then frees the context (`OQS_SIG_free`). Cost: ~0.23ms for lattice verify on small messages, but the internal SHA3 hash of 1 MiB at ~150 MB/s adds ~6.7ms.
3. **sha3_256(blob.pubkey)** (engine.cpp:77): 2592 bytes -- negligible.
4. **encode_blob() + blob_hash()** for tombstone check (engine.cpp:128): Re-encodes the entire blob to FlatBuffers (~1 MiB allocation + copy), then SHA3-256 hashes the encoded result (~6.7ms). This is REDUNDANT -- the same hash is computed inside store_blob.
5. **Storage encrypt** (storage.cpp:327): ChaCha20-Poly1305 AEAD over ~1 MiB. libsodium does this at ~1 GB/s. Cost: ~1ms.
6. **libmdbx write**: Synchronous disk write. Cost depends on disk, typically <1ms for SSD.

**Per-blob total at 1 MiB: ~20-30ms of CPU work.** At 15.3 blobs/sec, that is ~65ms per blob, suggesting overhead from memory allocation, FlatBuffer encoding (which copies all fields), and the sequential one-blob-at-a-time pipeline.

The 96% CPU on node2 (sync receiver) confirms the single io_context thread is saturated doing crypto + encoding + storage for every blob.

**Optimization approaches (ordered by impact/effort ratio):**

**1. Eliminate redundant encode+hash for tombstone check (LOW effort, MEDIUM impact):**
In engine.cpp line 127-128, for regular (non-tombstone) blobs, the code computes `wire::encode_blob(blob)` + `wire::blob_hash(encoded)` to get the content hash for the tombstone lookup. But `storage_.store_blob()` computes the same hash internally. Fix: compute the content hash once, pass it through to avoid double work. This eliminates one full ~1 MiB FlatBuffer encode + SHA3-256 pass per blob. Saves ~10ms per 1 MiB blob.

**2. Reuse OQS_SIG context in verify (LOW effort, LOW impact):**
`Signer::verify()` creates and destroys an `OQS_SIG` context per call. A static or thread-local context avoids repeated allocation. Impact is modest since `OQS_SIG_new` for ML-DSA is cheap (just a malloc + function pointer setup), but it eliminates unnecessary allocations under load.

**3. Async crypto offload to a worker thread (MEDIUM effort, HIGH impact):**
The highest-impact optimization: offload the CPU-bound portion (build_signing_input + Signer::verify + sha3_256) to a separate thread, then `co_await` the result on the io_context. Pattern:

```
io_context thread:  receive blob -> post crypto to worker -> co_await result -> store to libmdbx
worker thread:      build_signing_input -> verify -> return bool
```

This keeps the io_context responsive while crypto runs. With one worker thread, throughput is the same per-blob, but the io_context can handle other peers, PEX, and pub/sub notifications while waiting. With a small thread pool (2-3 workers), multiple blobs can be verified concurrently if the sync protocol allows it.

Implementation: `asio::post(worker_strand, [...])` with `asio::use_awaitable` adapter. Crypto functions are stateless (no shared mutable state), so thread safety is achieved by copying inputs. All Storage and Engine mutations remain on the io_context thread.

Complexity: the one-blob-at-a-time sync protocol serializes blob processing per peer connection. True parallelism requires either (a) pipelining within a connection (verify blob N+1 while storing blob N), or (b) parallel sync across multiple peer connections. Option (a) is the better fit.

**4. Pre-crypto dedup check on sync receive (LOW effort, LOW impact):**
Before running signature verification on a sync-received blob, check `storage_.has_blob(namespace, content_hash)`. If the blob already exists (a duplicate that arrived from another peer between hash-list exchange and blob transfer), skip the expensive verification. O(1) btree lookup. Marginal benefit in practice but costs nothing.

**Recommended approach for v0.7.0:**
1. Eliminate redundant encode+hash (Step 1 -- immediate win, easy to implement and benchmark).
2. Reuse OQS_SIG context (Step 2 -- trivial).
3. Benchmark after Steps 1-2. If throughput is still inadequate, implement async crypto offload (Step 3).
4. Pre-crypto dedup check (Step 4 -- opportunistic, do alongside other work).

**Dependencies:** Should be benchmarked using the existing Docker infrastructure (v0.6.0). Deletion benchmarks can share the same infrastructure.

### Deletion Benchmarks

**What to measure:**
1. Tombstone creation throughput: signed tombstone blobs/sec at various batch sizes.
2. Tombstone sync propagation latency: time from tombstone creation on node1 to arrival on node2/node3.
3. Deletion effect on stored data: verify target blob is actually removed on all nodes after tombstone propagation.
4. Expiry scan GC throughput: how fast expired tombstones (with TTL > 0) are cleaned up. (Note: current tombstones are TTL=0 permanent, but tombstone TTL was added in v0.5.0.)
5. Storage recovery verification: used_bytes before and after deletion.

**Infrastructure:** Extends existing Docker benchmark suite in `deploy/`. New loadgen profile that creates blobs, then creates tombstones targeting those blobs. New benchmark scenario script.

**Complexity:** Low. 1-2 new benchmark scripts + loadgen profile. No changes to chromatindb code.

**Dependencies:** Existing Docker benchmark infrastructure (v0.6.0). No dependency on other v0.7.0 features.

### Test Relocation

**Current state:** 284 tests in `tests/` at the project root, organized by component (tests/crypto/, tests/storage/, tests/sync/, etc.). The `db/` directory is a self-contained CMake component (established in v0.5.0) but has no tests.

**Target state:** Tests exercising db/ internals move into `db/tests/` with their own CMakeLists.txt. The top-level `tests/test_daemon.cpp` (integration test requiring the full daemon binary) stays in `tests/`.

**Complexity:** Low. Mechanical file moves + CMakeLists.txt updates. Pattern proven in v2.0 Phase 9 (source restructure, completed in 14 minutes).

**Dependencies:** None.

### General Cleanup

**Stale artifacts to sweep:**
- Old standalone benchmark binary from v3.0 (`chromatindb_bench`) -- predates Docker benchmarks, may overlap or be redundant.
- db/README.md may need updates for v0.7.0 features (namespace quotas, sync resumption config).
- Stale config examples or documentation references.
- Any dead code paths identified during other phases.

**Complexity:** Low. Survey + remove/update. Should happen last.

**Dependencies:** All other v0.7.0 features should be built first.

## Feature Dependencies

```
Sync Resumption       (independent)
Namespace Quotas      (independent)
Crypto Optimization   (independent)
Deletion Benchmarks   (independent, uses existing Docker infra)
Test Relocation       (independent)
General Cleanup       (last -- sweeps artifacts from other phases)
```

No hard dependencies between v0.7.0 features. All can be built in any order. Recommended ordering:

1. Test relocation (warm-up, low risk, improves project structure for all subsequent work)
2. Crypto optimization (highest user-visible impact, benchmark immediately)
3. Deletion benchmarks (establishes deletion baseline, exercises benchmark infra)
4. Sync resumption (second highest impact, well-scoped)
5. Namespace quotas (production feature, medium complexity)
6. General cleanup (sweep everything)

## MVP Recommendation

Prioritize by impact-to-effort ratio:

1. **Crypto throughput optimization** -- highest impact. The 96% CPU / 15 blobs/sec bottleneck is the most urgent problem. Known root cause, multiple optimization angles, easy to benchmark.

2. **Sync resumption** -- second highest impact. Every sync round being O(total_blobs) is wasteful and gets worse as data grows. The NamespaceList infrastructure already carries seq_nums; this is primarily logic changes.

3. **Namespace quotas** -- important for production operation. Without quotas, a single misbehaving namespace can fill a node despite global max_storage_bytes.

4. **Test relocation** -- low effort, improves component hygiene. Good first phase.

5. **Deletion benchmarks** -- low effort, establishes baseline for a feature that has never been benchmarked.

6. **General cleanup** -- last, sweeps everything.

Defer: None. All features are scoped for v0.7.0 and none are speculative.

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/peer/peer_manager.cpp` (sync flow lines 565-954, cursor positions, NamespaceList exchange), `db/engine/engine.cpp` (ingest pipeline lines 40-164, crypto hot path with redundant encode+hash at lines 127-128), `db/sync/sync_protocol.cpp` (hash-list diff, blob ingestion), `db/storage/storage.cpp` (libmdbx sub-databases, store_blob, used_bytes), `db/crypto/signing.cpp` (OQS_SIG context lifecycle in verify())
- Benchmark data: `deploy/results/REPORT.md` (v0.6.0 baseline: 15.3 blobs/sec at 1 MiB, 96% CPU on node2, 73% CPU on node1)
- Project context: `.planning/PROJECT.md` (v0.7.0 target features, constraints, key decisions)
- Performance concern: `project_perf_concern.md` (root cause: serial SHA3-256 + ML-DSA-87 verify on sync path)
- Retrospective: `.planning/RETROSPECTIVE.md` (milestone patterns, v2.0 restructure completed in 14 min)

### Secondary (MEDIUM confidence)
- [ML-DSA-87 verification: ~234us per verify on small messages, ~4272 verifications/sec](https://arxiv.org/html/2601.17785v1) -- research paper, January 2026. For large messages, the internal SHA3 hash dominates.
- [SHA3-256 performance: 8-15 cycles/byte on x86 without hardware acceleration](https://en.wikipedia.org/wiki/SHA-3) -- implies ~100-200 MB/s throughput, so ~5-10ms per MiB.
- [Durable Streams: offset-based sync resumption protocol](https://electric-sql.com/blog/2025/12/23/durable-streams-0.1.0) -- pattern validation for cursor-based resumption with monotonic offsets.
- [OQS benchmarking infrastructure](https://openquantumsafe.org/benchmarking/) -- liboqs speed_sig tool for local benchmarking of ML-DSA-87.
- [Kubernetes namespace resource quotas](https://kubernetes.io/docs/concepts/policy/resource-quotas/) -- pattern for per-namespace hard limits with defaults and overrides.
- [Async batch signature verification pattern (Zcash Zebra)](https://github.com/ZcashFoundation/zebra/issues/1944) -- pattern for concurrent crypto verification in blockchain context.
- [ML-DSA and PQ signing overview](https://www.encryptionconsulting.com/ml-dsa-and-pq-signing/) -- general ML-DSA performance characteristics and timeline.
