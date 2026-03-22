# Phase 33: Crypto Throughput Optimization - Research

**Researched:** 2026-03-17
**Domain:** C++20 crypto pipeline optimization (ML-DSA-87, SHA3-256, liboqs, FlatBuffers)
**Confidence:** HIGH

## Summary

Phase 33 addresses the known large blob crypto throughput bottleneck: 15.3 blobs/sec at 96% CPU for 1 MiB blobs. Five optimizations are required, all within the existing dependency set (liboqs, libsodium, FlatBuffers, libmdbx). No new libraries needed.

The central protocol-breaking change (PERF-04) converts `build_signing_input()` from returning a raw `std::vector<uint8_t>` concatenation (~1 MiB for large blobs) to returning a 32-byte `std::array<uint8_t, 32>` SHA3-256 digest. This uses the liboqs incremental SHA3-256 API (`OQS_SHA3_sha3_256_inc_*`) to hash namespace||data||ttl||timestamp without ever allocating the intermediate buffer. ML-DSA-87 then signs/verifies this 32-byte digest instead of the raw concatenation. This is the highest-impact single change.

The remaining optimizations are: thread_local OQS_SIG caching in `Signer::verify()` (PERF-02), has_blob dedup check before signature verification in the ingest pipeline (PERF-03), eliminating the redundant encode+hash in `store_blob()` by accepting pre-computed values (PERF-01), and reducing unnecessary copies in the sync receive path (PERF-05). All changes are serial -- thread pool offload is explicitly deferred to v0.8.0.

**Primary recommendation:** Implement PERF-04 (hash-then-sign) first as it has the widest blast radius (touches all callers of `build_signing_input()`) and all other optimizations build on top of or are independent of it.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **Hash-then-sign (PERF-04):** ML-DSA-87 signs/verifies SHA3-256(namespace||data||ttl||timestamp) -- the 32-byte digest, not the raw concatenation. `build_signing_input()` returns `std::array<uint8_t, 32>`. Uses incremental SHA3-256 hashing internally -- zero intermediate allocation. PROTOCOL.md updated. Loadgen updated.
- **OQS_SIG context caching (PERF-02):** `Signer::verify()` stays static, uses `thread_local OQS_SIG*` internally. Allocated once on first call, never freed (process-lifetime).
- **Dedup-before-crypto (PERF-03):** has_blob check added to `engine.ingest()` between Step 2 (namespace/delegation) and Step 3 (signature verify). Requires computing content hash first.
- **Store path optimization (PERF-01 + PERF-05):** Focus on eliminating re-encode in store_blob (pass pre-computed hash and/or encoded bytes through). TransportCodec/BlobData .assign() copies left as-is (diminishing returns, high async lifetime risk).

### Claude's Discretion
- Exact store_blob API change (overload vs new method vs modified signature)
- Whether to pass both hash + encoded bytes, or hash only
- Task ordering and plan decomposition
- Any additional copy elimination opportunities discovered during implementation

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| PERF-01 | Redundant SHA3-256 hash + FlatBuffer re-encode eliminated from ingest pipeline | store_blob() currently calls encode_blob() + blob_hash() redundantly. Pass pre-computed hash (and optionally encoded bytes) from engine.ingest() to store_blob(). See Architecture Pattern 2. |
| PERF-02 | OQS_SIG context cached instead of created/destroyed per verify call | Signer::verify() currently allocates OQS_SIG_new + OQS_SIG_free per call. Replace with thread_local static. See Architecture Pattern 3. |
| PERF-03 | Dedup check runs before signature verification on sync-received blobs | Insert has_blob() between Step 2 and Step 3 in engine.ingest(). Content hash needed first (computed from encoded blob). See Architecture Pattern 4. |
| PERF-04 | Hash-then-sign protocol change -- sign/verify over SHA3-256 digest | build_signing_input() rewritten to use incremental SHA3-256, returns array<32>. Protocol-breaking. All callers updated. See Architecture Pattern 1. |
| PERF-05 | Sync receive path copy reduction | Pass encoded FlatBuffer bytes through to storage without intermediate decode/re-encode where possible. Focus on store path. See Architecture Pattern 2. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs | 0.15.0 | ML-DSA-87 sign/verify, SHA3-256 (one-shot + incremental) | Already in build. Incremental SHA3 API confirmed in `build/_deps/liboqs-src/src/common/sha3/sha3.h`. |
| FlatBuffers | 25.2.10 | Wire format encoding/decoding | Already in build. encode_blob() and decode_blob() are the key functions. |
| libmdbx | 0.13.11 | Blob storage (encrypted at rest) | Already in build. store_blob() is the terminal storage call. |
| Catch2 | 3.7.1 | Test framework | Already in build. 313+ tests must pass. |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| spdlog | 1.15.1 | Logging | Debug/info/warn log messages in modified code paths |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Incremental SHA3 (liboqs) | Pre-allocate reusable buffer | Still requires 1 MiB memcpy. Incremental hashing avoids both allocation AND copy. |
| thread_local OQS_SIG | Static OQS_SIG with mutex | Thread_local is lock-free and future-safe for v0.8.0 thread pool offload. Static with mutex adds contention. |

**Installation:**
No changes. Same CMake FetchContent build.

## Architecture Patterns

### Recommended Change Map
```
db/wire/codec.h        -- build_signing_input() return type: vector -> array<32>
db/wire/codec.cpp      -- build_signing_input() implementation: incremental SHA3-256
db/crypto/signing.cpp  -- Signer::verify(): thread_local OQS_SIG*
db/crypto/hash.h       -- (optional) sha3_256_incremental() helper
db/crypto/hash.cpp     -- (optional) sha3_256_incremental() implementation
db/engine/engine.cpp   -- ingest(): dedup check before crypto, pass hash to store_blob
db/storage/storage.h   -- store_blob() overload or modified signature
db/storage/storage.cpp -- store_blob() accepts pre-computed hash + encoded bytes
db/PROTOCOL.md         -- Canonical Signing Input section updated
loadgen/loadgen_main.cpp -- make_signed_blob() updated for new build_signing_input
bench/bench_main.cpp   -- Updated for new signing API
db/tests/engine/test_engine.cpp        -- make_signed_blob/tombstone/delegation updated
db/tests/peer/test_peer_manager.cpp    -- make_signed_blob/tombstone updated
db/tests/sync/test_sync_protocol.cpp   -- make_signed_blob/tombstone updated
db/tests/test_daemon.cpp               -- make_signed_blob updated
db/tests/wire/test_codec.cpp           -- build_signing_input tests rewritten
```

### Pattern 1: Hash-then-sign with incremental SHA3-256 (PERF-04)

**What:** Replace the raw concatenation buffer with incremental SHA3-256 hashing. The function signature changes from returning `std::vector<uint8_t>` (variable size, up to ~1 MiB) to `std::array<uint8_t, 32>` (fixed 32 bytes).

**When to use:** Every blob sign and verify operation.

**Implementation:**
```cpp
// db/wire/codec.h -- NEW signature
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

// db/wire/codec.cpp -- NEW implementation
#include <oqs/sha3.h>

std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);

    // Feed components sequentially -- same order as before
    OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());

    // TTL as little-endian uint32
    uint8_t ttl_le[4] = {
        static_cast<uint8_t>(ttl),
        static_cast<uint8_t>(ttl >> 8),
        static_cast<uint8_t>(ttl >> 16),
        static_cast<uint8_t>(ttl >> 24),
    };
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_le, 4);

    // Timestamp as little-endian uint64
    uint8_t ts_le[8] = {
        static_cast<uint8_t>(timestamp),
        static_cast<uint8_t>(timestamp >> 8),
        static_cast<uint8_t>(timestamp >> 16),
        static_cast<uint8_t>(timestamp >> 24),
        static_cast<uint8_t>(timestamp >> 32),
        static_cast<uint8_t>(timestamp >> 40),
        static_cast<uint8_t>(timestamp >> 48),
        static_cast<uint8_t>(timestamp >> 56),
    };
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_le, 8);

    std::array<uint8_t, 32> hash{};
    OQS_SHA3_sha3_256_inc_finalize(hash.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);

    return hash;
}
```

**Critical: Callers that need updating (exhaustive list):**
1. `db/engine/engine.cpp` -- `ingest()` line 107 and `delete_blob()` line 200
2. `db/net/connection.cpp` -- NOT affected (handshake signs `session_fingerprint` directly, does not use `build_signing_input()`)
3. `db/net/handshake.cpp` -- NOT affected (same reason)
4. `loadgen/loadgen_main.cpp` -- `make_signed_blob()` line 190
5. `bench/bench_main.cpp` -- any blob construction code
6. `db/tests/engine/test_engine.cpp` -- `make_signed_blob()`, `make_signed_tombstone()`, `make_signed_delegation()`, `make_delegate_blob()` (4 helper functions)
7. `db/tests/peer/test_peer_manager.cpp` -- `make_signed_blob()`, `make_signed_tombstone()` (2 helpers)
8. `db/tests/sync/test_sync_protocol.cpp` -- `make_signed_blob()`, `make_signed_tombstone()` (2 helpers)
9. `db/tests/test_daemon.cpp` -- `make_signed_blob()` (1 helper)
10. `db/tests/wire/test_codec.cpp` -- `build_signing_input` test cases (3 tests)

**What does NOT change:**
- `Signer::sign()` and `Signer::verify()` API -- they already accept `std::span<const uint8_t>`. A 32-byte array converts to span implicitly.
- `NodeIdentity::sign()` -- calls `signer_.sign(message)` which accepts any span. No change needed.
- Handshake paths -- they sign the 32-byte session fingerprint directly.

### Pattern 2: Store path optimization (PERF-01 + PERF-05)

**What:** Eliminate the redundant `encode_blob()` + `blob_hash()` call inside `store_blob()`. Currently, `engine.ingest()` computes these at Step 3.5 (tombstone check), and then `store_blob()` recomputes them.

**Current flow (wasteful):**
```
engine.ingest():
  Step 3.5: encoded = encode_blob(blob)       // 1st encode
            content_hash = blob_hash(encoded)  // 1st hash
  Step 4:   store_blob(blob)
              encoded = encode_blob(blob)      // 2nd encode (REDUNDANT)
              hash = blob_hash(encoded)        // 2nd hash (REDUNDANT)
```

**Optimized flow:**
```
engine.ingest():
  Compute once:  encoded = encode_blob(blob)
                 content_hash = blob_hash(encoded)
  Step 3.5:      use content_hash for tombstone check
  Step 4:        store_blob_precomputed(blob, content_hash, encoded)
```

**Recommended API change -- new overload on Storage:**
```cpp
// db/storage/storage.h -- ADD new overload
StoreResult store_blob(const wire::BlobData& blob,
                       const std::array<uint8_t, 32>& precomputed_hash,
                       std::span<const uint8_t> precomputed_encoded);
```

This is cleaner than modifying the existing signature because:
- The original `store_blob(const BlobData&)` remains available for simple callers (delete_blob path)
- No risk of accidentally passing wrong hash/encoded data
- The new overload skips encode+hash, goes straight to dedup check + encryption

**For PERF-05:** The `precomputed_encoded` span eliminates the re-encode. The encoded bytes flow from `engine.ingest()` -> `store_blob()` -> `encrypt_value()` without intermediate copy. This is the "pass encoded FlatBuffer bytes through to storage" requirement.

### Pattern 3: OQS_SIG context caching (PERF-02)

**What:** Replace per-call `OQS_SIG_new()` + `OQS_SIG_free()` in `Signer::verify()` with a `thread_local` cached context.

**Current code (signing.cpp line 74-87):**
```cpp
bool Signer::verify(std::span<const uint8_t> message, ...) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);  // Allocate
    // ... verify ...
    OQS_SIG_free(sig);  // Deallocate
    return rc == OQS_SUCCESS;
}
```

**Optimized:**
```cpp
bool Signer::verify(std::span<const uint8_t> message, ...) {
    thread_local OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) throw std::runtime_error("...");
    // ... verify using sig (no free) ...
    return rc == OQS_SUCCESS;
}
```

**Key considerations:**
- `thread_local` initialization is guaranteed thread-safe by C++ standard (magic statics for thread_local)
- The OQS_SIG context is never freed -- it lives for the process lifetime (acceptable for a daemon)
- `OQS_SIG_verify()` is a read-only operation on the OQS_SIG context (it only reads algorithm parameters). Safe to reuse.
- Future-proof for v0.8.0 thread pool: each worker thread gets its own context automatically

### Pattern 4: Dedup-before-crypto (PERF-03)

**What:** Add a `has_blob()` check in `engine.ingest()` BEFORE the expensive ML-DSA-87 signature verification, AFTER namespace/delegation checks.

**Why after namespace checks:** We need the namespace_id to compute the content hash (via `encode_blob()` + `blob_hash()`). The namespace check is cheap (one SHA3-256 of pubkey + memcmp). The signature verification is expensive (~67ms for 1 MiB blobs pre-optimization).

**Implementation location:** `engine.cpp ingest()`, between Step 2 and Step 3:
```cpp
// Step 2.5: Dedup check (cheaper than crypto, requires content hash)
auto encoded = wire::encode_blob(blob);
auto content_hash = wire::blob_hash(encoded);
if (storage_.has_blob(blob.namespace_id, content_hash)) {
    WriteAck ack;
    ack.blob_hash = content_hash;
    ack.status = IngestStatus::duplicate;
    return IngestResult::success(std::move(ack));
}

// Step 3: Signature verification (most expensive) -- only for NEW blobs
// ... existing verify code ...

// Step 3.5: Tombstone check -- use already-computed content_hash
// ... existing tombstone code, but reuse content_hash instead of recomputing ...

// Step 4: Store -- pass pre-computed hash and encoded bytes
auto store_result = storage_.store_blob(blob, content_hash, encoded);
```

**Important nuance for seq_num:** When the dedup check short-circuits, we return IngestStatus::duplicate without a seq_num. The current store_blob() duplicate path scans for the existing seq_num, which is somewhat expensive. For the short-circuit path, returning seq_num=0 is acceptable since the WriteAck for duplicates is informational only (the blob is already stored with its correct seq_num). BUT -- check that PeerManager/SyncProtocol consumers of IngestResult handle this correctly. Looking at the code: `SyncStats::blobs_received` only increments on `result.accepted`, and duplicates are accepted. The notification callback fires only for `IngestStatus::stored`, not `duplicate`. So returning seq_num=0 for the early-dedup path is safe.

### Anti-Patterns to Avoid

- **Do NOT change the handshake signing.** The handshake signs the 32-byte session fingerprint directly via `identity_.sign(session_keys_.session_fingerprint)`. This already IS a 32-byte message. No change needed.
- **Do NOT add a HashML-DSA/pre-hash mode.** liboqs has no separate API. Our approach achieves the same effect.
- **Do NOT modify TransportCodec::decode or BlobData .assign() copies.** The CONTEXT.md explicitly marks these as out of scope (diminishing returns, high async lifetime risk).
- **Do NOT touch the Signer::sign() instance method or the Signer constructor.** Only the static `verify()` gets the thread_local optimization. The instance sign() method already has its own OQS_SIG via `sig_`.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Incremental SHA3-256 | Manual sponge state | `OQS_SHA3_sha3_256_inc_*` from liboqs | Proven XKCP implementation with AVX2/AVX512 when available. Handles padding and finalization correctly. |
| Thread-safe lazy init | Manual mutex + flag | C++ `thread_local` | Language-guaranteed initialization safety. Zero runtime cost after first access. |
| Content hash dedup | Custom bloom filter | `Storage::has_blob()` (libmdbx key lookup) | O(1) B-tree lookup, already exists, ACID-consistent. |

**Key insight:** Every optimization uses existing APIs. The work is restructuring call order and data flow, not implementing new algorithms.

## Common Pitfalls

### Pitfall 1: Signing input backward compatibility
**What goes wrong:** After changing `build_signing_input()` to return a hash, blobs signed with the old format (raw concatenation) will fail verification.
**Why it happens:** This is a protocol-breaking change by design (pre-MVP, acceptable).
**How to avoid:** All existing test data and benchmarks must use the new signing format. There is no backward compatibility bridge -- clean break.
**Warning signs:** Any test that hardcodes expected signing input bytes will fail.

### Pitfall 2: Forgetting a build_signing_input() caller
**What goes wrong:** A caller still expects `std::vector<uint8_t>` return, gets compile error or worse -- silently passes the 32-byte array where a larger buffer was expected.
**Why it happens:** build_signing_input is called in 10+ locations across production code, tests, loadgen, and bench.
**How to avoid:** The return type change from `vector` to `array<32>` will cause compile errors everywhere. Fix every single one. The exhaustive caller list is in Pattern 1 above.
**Warning signs:** Compilation failure is the safety net. If it compiles, it's correct (span<const uint8_t> accepts both array and vector).

### Pitfall 3: Dedup check computing hash before store_blob needs it
**What goes wrong:** After the dedup check, we pass the pre-computed hash to store_blob, but store_blob's existing code path recomputes it.
**Why it happens:** The current store_blob always calls encode_blob + blob_hash internally.
**How to avoid:** Create the new store_blob overload that accepts pre-computed values. The original overload remains for callers that don't have pre-computed data (delete_blob path).
**Warning signs:** If store_blob is called twice with different hashes for the same blob, data integrity breaks. Use the overload pattern, not conditional logic.

### Pitfall 4: thread_local OQS_SIG initialization failure
**What goes wrong:** `OQS_SIG_new()` returns nullptr on first access in a thread, and subsequent calls dereference null.
**Why it happens:** liboqs initialization failure (extremely rare in practice but possible).
**How to avoid:** Check the pointer on every verify call and throw if null. The thread_local guarantees single initialization per thread, so the check cost is negligible.
**Warning signs:** Segfault in verify. The check prevents this.

### Pitfall 5: Dedup short-circuit returning incomplete WriteAck
**What goes wrong:** The early dedup return skips the store_blob call, which means seq_num is not looked up.
**Why it happens:** store_blob's duplicate path scans seq_map to find the existing seq_num. The dedup check bypasses this entirely.
**How to avoid:** Return seq_num=0 in the early dedup WriteAck. Verify that no consumer requires a valid seq_num for duplicate blobs. Current consumers: PeerManager uses WriteAck only for logging. SyncProtocol notification callback only fires for `IngestStatus::stored`, not `duplicate`. Both are safe.
**Warning signs:** A test that checks `ack.seq_num > 0` for duplicate ingests will fail.

### Pitfall 6: Tombstone check using wrong hash
**What goes wrong:** The tombstone check at Step 3.5 currently computes `content_hash` via encode_blob + blob_hash. With the dedup optimization, this must use the SAME pre-computed hash from Step 2.5, not recompute.
**Why it happens:** Copy-paste error or not recognizing the dependency.
**How to avoid:** Compute encoded + content_hash ONCE (before Step 2.5), then thread them through to both the tombstone check and store_blob.
**Warning signs:** Double encode_blob call in the optimized code is a bug indicator.

## Code Examples

### Incremental SHA3-256 via liboqs (verified from headers)
```cpp
// Source: build/_deps/liboqs-src/src/common/sha3/sha3.h
#include <oqs/sha3.h>

OQS_SHA3_sha3_256_inc_ctx ctx;
OQS_SHA3_sha3_256_inc_init(&ctx);
OQS_SHA3_sha3_256_inc_absorb(&ctx, data1, len1);
OQS_SHA3_sha3_256_inc_absorb(&ctx, data2, len2);
uint8_t output[32];
OQS_SHA3_sha3_256_inc_finalize(output, &ctx);
OQS_SHA3_sha3_256_inc_ctx_release(&ctx);  // MUST release
```

### thread_local OQS_SIG pattern
```cpp
// Thread-safe, process-lifetime cached context
bool Signer::verify(std::span<const uint8_t> message,
                    std::span<const uint8_t> signature,
                    std::span<const uint8_t> public_key) {
    thread_local OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        throw std::runtime_error("Failed to create ML-DSA-87 context for verification");
    }
    OQS_STATUS rc = OQS_SIG_verify(sig, message.data(), message.size(),
                                    signature.data(), signature.size(),
                                    public_key.data());
    // No OQS_SIG_free -- intentionally leaked (process-lifetime)
    return rc == OQS_SUCCESS;
}
```

### store_blob overload with pre-computed values
```cpp
// Source: new API design based on existing store_blob pattern
StoreResult Storage::store_blob(const wire::BlobData& blob,
                                const std::array<uint8_t, 32>& precomputed_hash,
                                std::span<const uint8_t> precomputed_encoded) {
    try {
        auto blob_key = make_blob_key(blob.namespace_id.data(), precomputed_hash.data());
        auto key_slice = to_slice(blob_key);
        auto txn = impl_->env.start_write();

        // Dedup check
        auto existing = txn.get(impl_->blobs_map, key_slice, not_found_sentinel);
        if (existing.data() != nullptr) {
            // ... existing duplicate handling ...
        }

        // Encrypt the pre-computed encoded bytes (skip encode_blob + blob_hash)
        auto encrypted = impl_->encrypt_value(
            precomputed_encoded,
            std::span<const uint8_t>(blob_key.data(), blob_key.size()));

        // ... rest of store logic unchanged ...
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Raw concat signing input | SHA3-256 pre-hashed signing input | Phase 33 (now) | Eliminates 1 MiB allocation per large blob sign/verify |
| Per-call OQS_SIG alloc | thread_local cached OQS_SIG | Phase 33 (now) | Eliminates ~100 malloc/free pairs per sync round |
| Verify-then-check-dedup | Check-dedup-then-verify | Phase 33 (now) | Skips ~67ms ML-DSA-87 verify for already-stored blobs |
| Double encode in store path | Single encode with passthrough | Phase 33 (now) | Eliminates redundant FlatBuffer serialization + SHA3-256 |

**Deprecated/outdated:**
- `build_signing_input()` returning `std::vector<uint8_t>`: Replaced by `std::array<uint8_t, 32>`. Protocol break.

## Open Questions

1. **store_blob overload vs modified signature**
   - What we know: An overload is cleanest (preserves existing callers like delete_blob)
   - What's unclear: Whether delete_blob should also use pre-computed values (minor optimization, tombstones are tiny)
   - Recommendation: Overload. delete_blob uses the original `store_blob(blob)`. Only engine.ingest() uses the optimized overload. Keep it simple.

2. **Dedup short-circuit seq_num**
   - What we know: Returning seq_num=0 is safe for current consumers
   - What's unclear: Whether future sync resumption (Phase 34) will need the seq_num for duplicate acks
   - Recommendation: Return seq_num=0 now. If Phase 34 needs it, it can be added then. YAGNI.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 3.7.1 |
| Config file | db/CMakeLists.txt (catch_discover_tests) |
| Quick run command | `cmake --build build && ctest --test-dir build --output-on-failure` |
| Full suite command | `cmake --build build && ctest --test-dir build --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PERF-01 | store_blob accepts pre-computed hash/encoded, no redundant encode | unit | `ctest --test-dir build -R storage --output-on-failure` | Existing tests cover store_blob behavior; new overload needs test |
| PERF-02 | OQS_SIG cached in thread_local, verify still works | unit | `ctest --test-dir build -R signing --output-on-failure` | Existing verify tests suffice (behavior unchanged, only allocation pattern changes) |
| PERF-03 | Dedup check before crypto skips verify for existing blobs | unit | `ctest --test-dir build -R engine --output-on-failure` | Existing duplicate ingest tests cover this (but verify the short-circuit) |
| PERF-04 | build_signing_input returns SHA3-256 digest, sign/verify works | unit | `ctest --test-dir build -R codec --output-on-failure` | Existing tests must be rewritten for new return type |
| PERF-05 | Encoded bytes passed through to storage without re-encode | unit | `ctest --test-dir build -R storage --output-on-failure` | Covered by PERF-01 test |

### Sampling Rate
- **Per task commit:** `cmake --build build && ctest --test-dir build --output-on-failure`
- **Per wave merge:** Same (single build target)
- **Phase gate:** Full suite green + Docker benchmark comparison

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. The tests themselves need updating (new `build_signing_input()` return type), but no new test files or framework changes are needed.

## Sources

### Primary (HIGH confidence)
- liboqs 0.15.0 SHA3 incremental API -- verified in `build/_deps/liboqs-src/src/common/sha3/sha3.h` lines 44-96
- liboqs 0.15.0 SHA3 context struct -- verified in `build/_deps/liboqs-src/src/common/sha3/sha3_ops.h` lines 23-26
- Codebase analysis of all callers: `db/wire/codec.cpp`, `db/engine/engine.cpp`, `db/storage/storage.cpp`, `db/crypto/signing.cpp`, `loadgen/loadgen_main.cpp`, `db/net/connection.cpp`, `db/net/handshake.cpp`
- Test file analysis: `db/tests/engine/test_engine.cpp`, `db/tests/peer/test_peer_manager.cpp`, `db/tests/sync/test_sync_protocol.cpp`, `db/tests/test_daemon.cpp`, `db/tests/wire/test_codec.cpp`

### Secondary (MEDIUM confidence)
- `.planning/research/STACK.md` -- v0.7.0 stack research (2026-03-16), confirmed incremental SHA3 API and thread_local approach

### Tertiary (LOW confidence)
- None. All findings verified against actual source code.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already in build, APIs verified in headers
- Architecture: HIGH -- all optimizations map to specific code locations with known callers
- Pitfalls: HIGH -- derived from actual code analysis, not speculation

**Research date:** 2026-03-17
**Valid until:** Indefinite (internal codebase optimization, no external dependency changes)
