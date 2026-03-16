# Project Research Summary

**Project:** chromatindb v0.7.0 (Production Readiness)
**Domain:** Performance optimization, sync efficiency, and resource governance for a decentralized PQ-secure blob store
**Researched:** 2026-03-16
**Confidence:** HIGH

## Executive Summary

chromatindb v0.7.0 is a zero-new-dependencies milestone that addresses three production blockers identified in v0.6.0 benchmarks: a crypto throughput bottleneck (15 blobs/sec at 1 MiB, 96% CPU), redundant full-network sync on every round (O(total_blobs) hash exchange even when nothing changed), and the absence of per-namespace resource limits. All three are solvable with APIs already present in the bundled libraries (libmdbx sub-databases, thread_local OQS_SIG context reuse, pre-computed hash pass-through) but not yet used by chromatindb. The milestone also includes test relocation (db/ self-containment), deletion benchmarks, and general cleanup.

The recommended approach is incremental optimization over architectural change. The crypto bottleneck is addressed by eliminating redundant SHA3-256 hashing and FlatBuffer re-encoding (two passes reduced to one), then by reusing the OQS_SIG context instead of allocating per-call. Thread pool offload for ML-DSA-87 verification is deferred to v0.8.0 -- the coroutine/AEAD nonce ordering interaction is too risky for this milestone and the serial optimizations should lift throughput from 15 to ~18-20 blobs/sec. Sync resumption adds per-peer, per-namespace seq_num cursors stored in a new libmdbx sub-database, transforming sync from O(total_blobs) to O(new_blobs) per round. Namespace quotas add a second sub-database for materialized aggregate tracking (blob count + byte total), enforced atomically inside the libmdbx write transaction.

The primary risks are cursor staleness after deletions (causing permanent sync divergence), the check-then-act race on quota enforcement across co_await boundaries, and CMake test discovery breaking during relocation. All three have clear mitigations: cursors as optimization hints with periodic full resync fallback, quota enforcement inside the libmdbx write transaction (not the engine layer), and immediate CTest count verification after file moves.

## Key Findings

### Recommended Stack

No new dependencies. All features use existing APIs within bundled libraries at their current versions. The only build change is bumping libmdbx `max_maps` from 6 to 8 for two new sub-databases (sync_cursor_map, ns_stats).

**Core technologies (existing, newly leveraged):**
- **libmdbx sub-databases (v0.13.11):** Two new maps for cursor persistence and quota tracking. Same ACID guarantees, same write-transaction atomicity as existing 5 maps. Zero additional file descriptors or memory overhead.
- **thread_local OQS_SIG context (liboqs 0.15.0):** Eliminates per-call `OQS_SIG_new()`/`OQS_SIG_free()` allocation in the verification hot path. Single-threaded context so thread_local is effectively static.
- **Pre-computed hash pass-through:** Pass blob content hash from Engine to Storage to eliminate redundant FlatBuffer encode + SHA3-256 per blob. Pure code change, no new API.

**Explicitly rejected:**
- No new dependencies (TBB, cppcoro, RocksDB, OpenSSL, Prometheus, C++23 parallel STL)
- No HashML-DSA (breaks existing signatures, liboqs lacks the API)
- No batch ML-DSA verify (algorithm does not support it)
- No hardware crypto acceleration (SHA3-NI not available on target hardware)
- No thread pool crypto offload in v0.7.0 (AEAD nonce desync risk; defer to v0.8.0)

### Expected Features

**Must have (table stakes):**
- **Sync resumption with per-peer cursors** -- Every production replication system tracks sync position. Without it, every 60-second sync round exchanges all hashes for all namespaces, O(total_blobs) per peer. Transforms to O(new_blobs).
- **Large blob crypto throughput fix** -- 15 blobs/sec at 1 MiB with 96% CPU is unusable for real workloads. Root cause is double SHA3-256 + double FlatBuffer encode + per-call OQS context allocation.
- **Test relocation into db/** -- Tests for the database component must live inside the component for self-containment. Mechanical move, proven pattern from v2.0.
- **Deletion benchmarks** -- Tombstone creation, propagation, and GC have never been benchmarked. Extends existing Docker benchmark infrastructure.

**Should have (differentiators):**
- **Namespace storage quotas (bytes + count)** -- Prevents single-namespace abuse on multi-tenant nodes. Per-namespace hard limits with defaults and overrides. SIGHUP-reloadable.
- **Sync cursor persistence across restarts** -- Cursors that survive restart avoid full re-sync after reboot. Stored in new libmdbx sub-database.
- **Pre-crypto dedup check** -- O(1) `has_blob()` check before expensive ML-DSA verify on sync-received blobs. Free to implement, marginal benefit.

**Defer (v0.8.0+):**
- **Thread pool crypto offload** -- Highest potential impact (4-5x throughput) but requires careful coroutine/AEAD interaction design
- **Pipelined verification with I/O** -- Overlap receive and verify; requires restructuring sync protocol
- **Complex quota policies** -- Tiered, time-based, burst, priority belong in relay layer
- **HashML-DSA** -- Pre-hash mode eliminates large-message penalty but is a protocol-breaking signature scheme change

### Architecture Approach

No new components. Four existing components are modified (Storage, Engine, SyncProtocol, PeerManager) plus Config. Two new libmdbx sub-databases are added. The wire protocol is unchanged -- sync resumption is a local optimization (peers independently decide what hashes to send based on their own cursors), and quota rejection uses the existing silent-skip pattern.

**Major component changes:**
1. **Storage** -- +2 sub-databases (sync_cursor_map, ns_stats), quota counter maintenance in store/delete/expiry write transactions, pre-computed hash acceptance in store_blob
2. **Engine** -- Hash pass-through to storage (eliminates redundant encode+hash), quota enforcement at Step 0c (after global capacity, before crypto)
3. **SyncProtocol** -- Cursor-aware hash collection (`collect_namespace_hashes_since(ns, since_seq)`)
4. **PeerManager** -- Cursor state keyed by peer pubkey hash (stable identity, not connection pointer), cursor persistence via Storage
5. **Config** -- `max_namespace_bytes`, `max_namespace_blobs`, SIGHUP-reloadable

### Critical Pitfalls

1. **Sync cursor staleness after deletions (CRITICAL)** -- Cursors that skip past tombstone seq_nums cause permanent sync divergence. Use cursors as optimization hints only (start scanning from seq_num, not skip below), always include tombstone hashes, and fall back to full hash-list diff every Nth sync round.

2. **Quota check-then-act race across co_await (CRITICAL)** -- Two concurrent ingests for the same namespace both pass the quota check before either stores. Enforce quota inside the libmdbx write transaction (in Storage, not Engine) where serialization is guaranteed by libmdbx single-writer constraint.

3. **Thread pool crypto offload breaks AEAD nonces (CRITICAL, deferred)** -- Offloading to a thread pool can break AEAD send/recv counter ordering. Deferred to v0.8.0. For v0.7.0, only serial optimizations.

4. **CMake catch_discover_tests breaks after test relocation (MODERATE)** -- Moving test target to subdirectory changes binary path resolution. Verify `ctest -N` test count matches before/after (must remain 284).

5. **Cursor lost on reconnect if stored in PeerInfo (MODERATE)** -- PeerInfo is connection-scoped, destroyed on disconnect. Store cursors separately, keyed by peer pubkey hash (stable identity), persisted in libmdbx sub-database.

## Implications for Roadmap

Based on research, suggested 6-phase structure:

### Phase 1: Test Relocation

**Rationale:** Zero-risk warm-up. Establishes db/ self-containment before any code changes. Must precede cleanup (cleanup can delete code tests depend on if tests are not yet co-located).
**Delivers:** All tests in db/tests/, db/ is a fully self-contained CMake component with Catch2.
**Addresses:** Test relocation (table stakes), structural hygiene.
**Avoids:** Pitfall 4 (CTest discovery), Pitfall 13 (include paths), Pitfall 8 (cleanup before relocation).

### Phase 2: Crypto Throughput Optimization

**Rationale:** Highest user-visible impact. Addresses the 96% CPU / 15 blobs/sec bottleneck. Storage API changes (pre-computed hash passing) establish patterns used by subsequent phases.
**Delivers:** Redundant encode+hash eliminated, static OQS_SIG context, expected ~18-20 blobs/sec at 1 MiB.
**Uses:** Existing liboqs, libmdbx. No new APIs. store_blob gains optional precomputed_hash parameter.
**Avoids:** Anti-pattern of HashML-DSA switch (protocol break), anti-pattern of thread pool without nonce safety.

### Phase 3: Sync Resumption

**Rationale:** Second highest impact. Transforms O(total_blobs) per sync to O(new_blobs). Adds sync_cursor_map sub-database, establishing the libmdbx sub-database addition pattern.
**Delivers:** Per-peer per-namespace cursor persistence, cursor-aware hash collection, periodic full resync fallback.
**Uses:** libmdbx sync_cursor_map, existing NamespaceList seq_num metadata. No wire protocol changes.
**Avoids:** Pitfall 1 (cursor staleness -- hint-only + periodic full diff), Pitfall 5 (cursor lost on reconnect -- keyed by pubkey, persisted).

### Phase 4: Namespace Quotas

**Rationale:** Production-readiness feature. Follows same sub-database pattern as Phase 3. Independent of sync resumption. Quota enforcement inside write transaction prevents check-then-act race.
**Delivers:** Per-namespace byte + count limits, ns_stats sub-database, SIGHUP-reloadable config, enforcement in ingest pipeline.
**Uses:** libmdbx ns_stats sub-database, Config SIGHUP reload pattern.
**Avoids:** Pitfall 2 (check-then-act race -- enforce in write txn), Pitfall 6 (DARE size inflation -- track pre-encryption size).

### Phase 5: Deletion Benchmarks

**Rationale:** Extends existing Docker benchmark infrastructure. No chromatindb code changes. Establishes deletion performance baseline for a feature that has never been benchmarked.
**Delivers:** Tombstone creation throughput, sync propagation latency, GC performance, storage recovery verification.
**Avoids:** Pitfall 12 (loadgen complexity -- sequential write-then-delete, no interleaving).

### Phase 6: General Cleanup

**Rationale:** Must come last. Sweeps stale artifacts, updates documentation, removes dead code after all feature work is stable.
**Delivers:** Clean codebase, updated db/README.md, stale artifact removal.
**Avoids:** Pitfall 7 (removing bench binary without checking references), Pitfall 8 (deleting code tests use), Pitfall 11 (README before features are done).

### Phase Ordering Rationale

- Test relocation first: zero risk, fast (14 min pattern from v2.0), must precede cleanup
- Crypto optimization second: most urgent bottleneck, storage API changes inform subsequent work
- Sync resumption third: adds first new sub-database, establishes pattern
- Namespace quotas fourth: follows same sub-database pattern, lower risk
- Phases 1 and 2 can run in parallel (no file overlap). Phases 3 and 4 can also run in parallel after Phase 2 (different parts of ingest pipeline)
- Deletion benchmarks and cleanup last, both depend on feature stability

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 3 (Sync Resumption):** Cursor-deletion interaction is subtle. Needs careful plan for how cursors interact with tombstone seq_nums, periodic full resync frequency, and cursor advancement behavior when blobs are rejected (quota exceeded). Edge cases around stale cursors and peer rollback need explicit test coverage.

Phases with standard patterns (skip research-phase):
- **Phase 1 (Test Relocation):** Proven pattern from v2.0 Phase 9. Mechanical git mv + CMake update.
- **Phase 2 (Crypto Optimization):** Root cause fully analyzed at exact line numbers. Changes are local.
- **Phase 4 (Namespace Quotas):** Standard materialized-aggregate pattern. Same sub-database pattern as existing indexes.
- **Phase 5 (Deletion Benchmarks):** Extends existing Docker infrastructure. No chromatindb code changes.
- **Phase 6 (Cleanup):** Survey + remove/update.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Zero new dependencies. All APIs verified in bundled library headers (thread_pool.hpp, SHA3 incremental API confirmed in build tree). |
| Features | HIGH | Feature set derived from concrete v0.6.0 benchmark data (15.3 blobs/sec, 96% CPU) and codebase analysis (redundant encode+hash at engine.cpp:127-128). |
| Architecture | HIGH | All changes follow existing patterns (sub-databases, ingest pipeline steps, SIGHUP config reload). No new architectural patterns. |
| Pitfalls | HIGH | Critical pitfalls identified with specific prevention strategies validated against codebase behavior. Integration pitfalls (cursor + quota, cursor + crypto) explicitly called out. |

**Overall confidence:** HIGH

### Gaps to Address

- **Exact throughput improvement from serial optimizations:** Estimate of 18-20 blobs/sec is based on eliminating ~1.5ms of redundant work from ~6ms total. Actual improvement depends on allocation overhead and FlatBuffer encoding costs not independently profiled. Benchmark immediately after Phase 2 to validate.
- **Periodic full resync frequency:** Research recommends every Nth sync round as cursor staleness safety net but does not establish what N should be. Set conservatively (e.g., 10 rounds = 10 minutes) and tune empirically.
- **Config format for namespace quotas:** STACK.md proposes flat fields (`max_namespace_bytes`, `max_namespace_blobs`). FEATURES.md proposes nested `namespace_quotas` with default + per-namespace overrides. Resolve during requirements: flat is simpler and YAGNI-compliant; nested enables per-namespace overrides for multi-tenant operation.
- **Thread pool design for v0.8.0:** Serial optimizations may prove insufficient. If post-Phase-2 benchmarks still show unacceptable throughput, v0.8.0 thread pool design needs to solve the AEAD nonce ordering problem. The pattern (post to pool, post results back to io_context) is identified but not prototyped.

## Sources

### Primary (HIGH confidence)
- Codebase analysis: engine.cpp (ingest pipeline, redundant encode+hash at lines 127-128), storage.cpp (5 sub-database patterns), peer_manager.cpp (sync flow lines 565-954), signing.cpp (OQS_SIG per-call lifecycle)
- v0.6.0 benchmark report: deploy/results/REPORT.md (15.3 blobs/sec, 96% CPU baseline)
- Bundled library verification: liboqs SHA3 incremental API headers, Asio thread_pool.hpp in build tree
- libmdbx sub-database documentation and existing pattern
- FIPS 204 specification (ML-DSA pure vs HashML-DSA modes)
- NIST PQC FAQ and forum (HashML-DSA backward incompatibility)

### Secondary (MEDIUM confidence)
- ML-DSA-87 verification benchmarks (arxiv.org/html/2601.17785v1) -- 234us per verify on small messages
- SHA3-256 throughput estimates (8-15 cycles/byte on x86 without hardware acceleration)
- Durable Streams offset-based resumption pattern (electric-sql.com)
- Kubernetes namespace resource quotas pattern
- Zcash Zebra async batch verification pattern

### Tertiary (LOW confidence)
- Thread pool throughput multiplier (estimated 4-5x on 6-core Ryzen) -- needs validation via actual benchmark
- FlatBuffer re-encoding cost estimates -- needs independent profiling

---
*Research completed: 2026-03-16*
*Ready for roadmap: yes*
