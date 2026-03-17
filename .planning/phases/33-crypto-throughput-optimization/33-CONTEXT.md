# Phase 33: Crypto Throughput Optimization - Context

**Gathered:** 2026-03-17
**Status:** Ready for planning

<domain>
## Phase Boundary

Eliminate redundant work and copies in the large blob (1 MiB) ingest and sync verification hot path. Five optimizations: hash-then-sign protocol change (PERF-04), OQS_SIG context caching (PERF-02), dedup-before-crypto (PERF-03), re-encode elimination (PERF-01), and sync receive copy reduction (PERF-05). Protocol-breaking change is acceptable (pre-MVP). All 313+ tests must pass with no regressions.

</domain>

<decisions>
## Implementation Decisions

### Hash-then-sign protocol change (PERF-04)
- ML-DSA-87 signs/verifies SHA3-256(namespace||data||ttl||timestamp) — the 32-byte digest, not the raw concatenation
- `build_signing_input()` returns `std::array<uint8_t, 32>` (the hash) instead of `std::vector<uint8_t>` (the raw concat)
- Use incremental SHA3-256 hashing internally — feed namespace, data, ttl, timestamp sequentially into a SHA3 context. Zero intermediate allocation (the ~1 MiB concatenation buffer is never created)
- PROTOCOL.md updated in this phase to reflect the new signing scheme
- Loadgen updated to use the new signing API (build requirement, not scope creep)

### OQS_SIG context caching (PERF-02)
- `Signer::verify()` stays static, uses `thread_local OQS_SIG*` internally
- thread_local chosen over static for future-safety (v0.8.0 thread pool offload)
- Context allocated once on first call, never freed (process-lifetime)

### Dedup-before-crypto (PERF-03)
- has_blob check added to `engine.ingest()` between Step 2 (namespace/delegation) and Step 3 (signature verify)
- Requires computing content hash first, but that's needed for storage anyway
- Sync-received duplicates skip the expensive ML-DSA-87 verify entirely

### Store path optimization (PERF-01 + PERF-05)
- Focus on eliminating the re-encode in store_blob (pass pre-computed hash and/or encoded bytes through)
- Copy reduction focuses on the store path, NOT on TransportCodec::decode or BlobData spans
- TransportCodec/BlobData .assign() copies left as-is — diminishing returns with high async lifetime risk

### Claude's Discretion
- Exact store_blob API change (overload vs new method vs modified signature)
- Whether to pass both hash + encoded bytes, or hash only
- Task ordering and plan decomposition
- Any additional copy elimination opportunities discovered during implementation

</decisions>

<specifics>
## Specific Ideas

- Incremental SHA3-256 via liboqs API: `OQS_SHA3_sha3_256_inc_init/absorb/finalize` — avoids 1 MiB intermediate buffer entirely
- Benchmark validation: run Docker benchmark suite after all optimizations, compare against v0.6.0 baseline (15.3 blobs/sec @ 96% CPU for 1 MiB). Benchmarks are part of verification, not a separate plan.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/crypto/hash.h/cpp`: `sha3_256()` function exists, but takes a contiguous span. Incremental API needs new code using liboqs SHA3 directly.
- `db/crypto/signing.cpp`: `Signer::verify()` at line 74 — the static method to modify
- `db/wire/codec.cpp`: `build_signing_input()` at line 60 — returns vector, needs to return array<32>
- `db/engine/engine.cpp`: `ingest()` at line 40 — main hot path, dedup check goes here
- `db/storage/storage.cpp`: `store_blob()` at line 282 — re-encodes and re-hashes, needs optimization

### Established Patterns
- liboqs incremental SHA3: `OQS_SHA3_sha3_256_inc_ctx`, `_inc_init()`, `_inc_absorb()`, `_inc_finalize()`
- thread_local for process-lifetime caching (no existing examples, but standard C++ pattern)
- Step 0 pattern: cheapest validation first, most expensive last

### Integration Points
- `build_signing_input()` called by: engine.cpp ingest, engine.cpp delete_blob, connection.cpp auth exchange, loadgen_main.cpp
- `Signer::verify()` called by: engine.cpp ingest, engine.cpp delete_blob, connection.cpp handshake auth
- `store_blob()` called by: engine.cpp ingest (step 4), engine.cpp delete_blob (step 5)
- Docker benchmark suite in `deploy/` — run after optimization for throughput comparison

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 33-crypto-throughput-optimization*
*Context gathered: 2026-03-17*
