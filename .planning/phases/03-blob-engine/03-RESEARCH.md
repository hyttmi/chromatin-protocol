# Phase 3: Blob Engine - Research

**Researched:** 2026-03-03
**Domain:** Blob ingest pipeline with namespace/signature verification, query interface, write ACKs
**Confidence:** HIGH

## Summary

Phase 3 builds a `BlobEngine` class that sits between raw storage (Phase 2) and the network layer (Phase 4). It is the verification and query gateway: every blob must pass namespace ownership (SHA3-256(pubkey) == claimed namespace) and signature verification (ML-DSA-87 over canonical signing input) before touching storage. On success, it returns a write ACK with the blob hash, assigned seq_num, and status. On failure, it returns a rejection ACK with a specific error code.

All building blocks already exist. `crypto::Signer::verify()` provides static ML-DSA-87 verification, `crypto::sha3_256()` handles namespace derivation checks, `wire::build_signing_input()` constructs the canonical signing payload, `wire::blob_hash()` computes content hashes, and `Storage` handles persistence with dedup and seq_num assignment. The BlobEngine's job is to compose these into a fail-fast validation pipeline and expose a clean query interface.

The main implementation gap is that `Storage::store_blob()` returns only a `StoreResult` enum (Stored/Duplicate/Error) and does not expose the assigned seq_num or computed blob hash. The engine needs both for write ACKs. The cleanest approach is to extend `Storage` with a richer return type (or add a `store_blob_ext` variant), since the seq_num is computed inside the write transaction and cannot be reliably queried after the fact without a race. Additionally, `list_namespaces` does not exist in Storage -- the engine must implement it by scanning the sequence index for distinct namespace prefixes.

**Primary recommendation:** Single `BlobEngine` class in `src/engine/engine.h` that takes a `Storage&` reference, validates blobs via fail-fast pipeline (pubkey size check -> namespace check -> signing input reconstruction -> signature verify), delegates storage, and returns structured ACK results. Add a `list_namespaces` query by cursor-scanning the sequence sub-database for distinct 32-byte namespace prefixes.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Specific error codes: namespace_mismatch, invalid_signature, malformed_blob (not opaque "rejected")
- Fail-fast validation order: namespace check (cheap SHA3 comparison) before signature verify (expensive ML-DSA-87)
- Log rejections at warn level with truncated namespace ID
- Accept blobs for ANY valid namespace, not just the local node's -- replication requires storing other nodes' blobs
- ACK includes: blob hash, assigned seq_num, status (stored/duplicate), error reason on rejection
- Idempotent on duplicate: returns existing seq_num (same response shape as new storage)
- Replication count field stubbed to 1 (local only) -- ACKW-02 (v2) will populate properly
- Rejection ACK carries specific error reason, consistent with the rejection behavior decision
- `get_blobs_since_seq`: optional `max_count` parameter (default returns all, caller can limit)
- `list_namespaces`: returns namespace IDs + latest seq_num per namespace (enables efficient sync negotiation)
- `get_blob_by_hash`: exposed through engine (wraps Storage::get_blob) -- sync needs this for requesting specific missing blobs
- Single `BlobEngine` class handles both ingest (write path) and queries (read path)

### Claude's Discretion
- Internal struct naming and layout for WriteAck / IngestError types
- Exact error enum values beyond the three specified
- Whether BlobEngine owns Storage or takes a reference
- Test organization and helper utilities

### Deferred Ideas (OUT OF SCOPE)
- None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| NSPC-02 | Node verifies SHA3-256(pubkey) == claimed namespace on every write, rejects mismatches | BlobEngine computes `crypto::sha3_256(blob.pubkey)` and compares to `blob.namespace_id`; fail-fast before signature check |
| NSPC-03 | Node verifies ML-DSA-87 signature over (namespace \|\| data \|\| ttl \|\| timestamp) on every write, rejects invalid | BlobEngine calls `wire::build_signing_input()` then `crypto::Signer::verify()` with the blob's pubkey and signature |
| QURY-01 | Client can request "give me namespace X since seq_num Y" and receive matching blobs | BlobEngine wraps `Storage::get_blobs_by_seq()` with optional `max_count` parameter for caller-controlled pagination |
| QURY-02 | Client can request "list all namespaces" and receive namespace list | BlobEngine scans the sequence sub-database with a cursor, collecting distinct 32-byte namespace prefixes and their latest seq_num |
| ACKW-01 | Node acknowledges blob acceptance after local storage (write ACK) | BlobEngine returns a `WriteAck` struct containing blob hash, seq_num, status, and stubbed replication_count=1 |
</phase_requirements>

## Standard Stack

### Core

No new libraries needed for Phase 3. The engine is pure composition of existing modules.

| Component | Location | Purpose | Already In Use |
|-----------|----------|---------|----------------|
| crypto::sha3_256 | src/crypto/hash.h | Namespace derivation check | Phase 1 |
| crypto::Signer::verify | src/crypto/signing.h | ML-DSA-87 signature verification (static method) | Phase 1 |
| wire::build_signing_input | src/wire/codec.h | Canonical signing input reconstruction | Phase 1 |
| wire::blob_hash | src/wire/codec.h | Content-addressed blob hash | Phase 1 |
| wire::encode_blob | src/wire/codec.h | FlatBuffer encoding (needed for hash computation) | Phase 1 |
| storage::Storage | src/storage/storage.h | Blob persistence, dedup, seq_num assignment | Phase 2 |
| spdlog | CMakeLists.txt | Structured logging for rejections/accepts | Phase 1 |

### Supporting

| Component | Purpose | When to Use |
|-----------|---------|-------------|
| identity::NodeIdentity | Test helper -- generate valid keypairs to create properly signed blobs | Tests only |
| Catch2 | Unit testing framework | Tests |

### Alternatives Considered

None -- this phase is pure internal composition with no new dependencies.

## Architecture Patterns

### Recommended Project Structure

```
src/
├── engine/
│   └── engine.h          # BlobEngine class (header-only or .h + .cpp)
│   └── engine.cpp        # BlobEngine implementation
├── crypto/               # (existing) SHA3-256, ML-DSA-87
├── wire/                  # (existing) FlatBuffers codec
├── storage/               # (existing) libmdbx storage
└── identity/              # (existing) NodeIdentity

tests/
└── engine/
    └── test_engine.cpp    # BlobEngine tests
```

### Pattern 1: Fail-Fast Validation Pipeline

**What:** Validate blob fields in order of increasing cost. Reject as soon as any check fails.
**When to use:** Every blob ingest call.

```cpp
// Validation order (cheap to expensive):
// 1. Structural: pubkey size == 2592, signature non-empty, namespace non-zero
// 2. Namespace:  SHA3-256(pubkey) == namespace_id  (~1 microsecond)
// 3. Signature:  ML-DSA-87 verify over canonical input  (~1-2 milliseconds)
//
// Only if all pass: store_blob() -> ACK

IngestResult BlobEngine::ingest(const wire::BlobData& blob) {
    // Step 1: Structural checks
    if (blob.pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        return IngestResult::rejection(IngestError::malformed_blob, "invalid pubkey size");
    }
    // ... more structural checks ...

    // Step 2: Namespace ownership (cheap hash comparison)
    auto derived_ns = crypto::sha3_256(blob.pubkey);
    if (derived_ns != blob.namespace_id) {
        return IngestResult::rejection(IngestError::namespace_mismatch);
    }

    // Step 3: Signature verification (expensive)
    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    if (!crypto::Signer::verify(signing_input, blob.signature, blob.pubkey)) {
        return IngestResult::rejection(IngestError::invalid_signature);
    }

    // Step 4: Store (handles dedup internally)
    // ... compute hash, store, return ACK ...
}
```

### Pattern 2: Structured Result Types

**What:** Use a tagged union or struct to return rich results with either success data or error data.
**When to use:** Ingest returns and query returns.

```cpp
enum class IngestError {
    namespace_mismatch,
    invalid_signature,
    malformed_blob,
    storage_error
};

enum class IngestStatus {
    stored,
    duplicate
};

struct WriteAck {
    std::array<uint8_t, 32> blob_hash;
    uint64_t seq_num;
    IngestStatus status;
    uint32_t replication_count;  // Stubbed to 1 until ACKW-02
};

struct IngestResult {
    bool accepted;
    std::optional<WriteAck> ack;          // Present on success
    std::optional<IngestError> error;     // Present on failure
    std::string error_detail;             // Human-readable detail on failure

    static IngestResult success(WriteAck ack);
    static IngestResult rejection(IngestError err, std::string detail = "");
};
```

### Pattern 3: Reference-Based Dependency (Storage)

**What:** BlobEngine takes a `Storage&` reference, not ownership. Caller manages Storage lifetime.
**When to use:** Engine construction.

```cpp
class BlobEngine {
public:
    explicit BlobEngine(storage::Storage& store);
    // ...
private:
    storage::Storage& storage_;
};
```

This pattern follows Phase 4's needs: the networking layer will own both Storage and BlobEngine, passing Storage by reference to the engine. It also makes testing trivial -- tests create a Storage and pass it to BlobEngine.

### Pattern 4: Namespace Listing via Cursor Scan

**What:** Scan the sequence sub-database for distinct namespace prefixes.
**When to use:** `list_namespaces()` query.

The sequence index has keys of the form `[namespace:32][seq_be:8]`. To list all namespaces:
1. Open a read cursor on the sequence sub-database
2. Seek to the first key
3. Read the first 32 bytes as a namespace ID
4. Read the last 8 bytes as seq_num -- keep the highest seen per namespace
5. Skip forward by seeking to `[namespace][0xFF...FF]` + 1 (or use `lower_bound` with next-possible-namespace) to jump to the next namespace
6. Repeat until cursor is exhausted

This avoids scanning every key -- it jumps across namespaces efficiently.

### Anti-Patterns to Avoid

- **Validating after storage:** Never store first and verify later. The whole point of BlobEngine is gatekeeping.
- **Catching liboqs exceptions for control flow:** `Signer::verify()` returns bool, not exceptions. Don't wrap it in try/catch for normal reject flow.
- **Re-implementing hash computation:** Use `wire::blob_hash(wire::encode_blob(blob))` -- don't manually hash fields.
- **Trusting timestamp for ordering:** Timestamps are untrusted. Seq_num is the only ordering primitive.
- **Checking namespace == local node's namespace:** The engine accepts blobs for ANY valid namespace (replication use case).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Namespace derivation check | Manual SHA3 + memcmp | `crypto::sha3_256(pubkey)` + `==` on `std::array<uint8_t,32>` | `std::array` has built-in `operator==` |
| Signing input construction | Manual byte concatenation | `wire::build_signing_input()` | Already handles endianness and layout |
| Blob content hash | Manual encode + hash | `wire::blob_hash(wire::encode_blob(blob))` | Canonical encoding + hash in one call chain |
| ML-DSA-87 verification | Direct liboqs calls | `crypto::Signer::verify()` | Static method, handles context allocation/cleanup |
| Blob persistence | Direct libmdbx writes | `storage::Storage::store_blob()` | Handles dedup, seq_num, expiry index |

**Key insight:** Phase 3 is pure composition. Every cryptographic and storage operation already has a tested wrapper. The engine's value is in the validation pipeline and query interface, not new low-level operations.

## Common Pitfalls

### Pitfall 1: Storage::store_blob Doesn't Return seq_num

**What goes wrong:** BlobEngine needs the assigned seq_num for the write ACK, but `Storage::store_blob()` only returns `StoreResult` (Stored/Duplicate/Error). There's no way to get the seq_num after the write without a potential race.

**Why it happens:** Phase 2's Storage was designed as a simple wrapper before the ACK requirement was known.

**How to avoid:** Extend `Storage` with a richer return. Two options:

1. **Change StoreResult to a struct** containing `status`, `seq_num`, and `blob_hash`. This is a small, backward-compatible change (rename old enum, add struct).
2. **Add a `store_blob_ext()` method** that returns the full result, keeping the old method for compatibility.

Option 1 is cleaner. The struct approach:
```cpp
struct StoreResult {
    enum Status { Stored, Duplicate, Error };
    Status status;
    uint64_t seq_num = 0;       // Assigned (or existing) seq_num
    std::array<uint8_t, 32> blob_hash{};  // Computed blob hash
};
```

**Warning signs:** If you find yourself querying seq_num after store_blob, you have the race condition.

### Pitfall 2: Duplicate Returns Existing seq_num (Idempotency)

**What goes wrong:** When a duplicate blob is ingested, the ACK must return the *existing* seq_num, not 0 or a new one. This requires looking up the blob's seq_num during the duplicate path.

**Why it happens:** The current duplicate detection in `Storage::store_blob()` returns `Duplicate` immediately without looking up the seq_num from the sequence index.

**How to avoid:** In the enhanced `store_blob`, when a duplicate is detected:
1. The blob hash is already computed (it was used for the dedup check)
2. Scan the sequence index for this namespace to find the seq entry pointing to this hash
3. Return that seq_num in the result

This is a reverse lookup: for a given namespace + blob_hash, find the seq_num. A full cursor scan of the namespace's seq entries is acceptable since it's the duplicate path (less common). Alternatively, maintain a hash->seq_num reverse index, but that's YAGNI for now.

**Warning signs:** Duplicate ACKs returning seq_num=0 or a different seq_num than the original store.

### Pitfall 3: list_namespaces Missing from Storage

**What goes wrong:** QURY-02 requires listing all namespaces, but Storage has no such method. The BlobEngine must implement this itself.

**Why it happens:** Phase 2 didn't have a query requirement for namespace listing.

**How to avoid:** Two implementation approaches:

1. **BlobEngine scans the sequence sub-database directly** -- requires exposing the libmdbx environment or adding a method to Storage. Since Storage uses Pimpl, exposing internals breaks encapsulation.

2. **Add `list_namespaces()` to Storage** -- cleaner. Storage opens a read cursor on the sequence map, iterates namespace prefixes, collects distinct IDs + latest seq_num. This keeps all libmdbx access inside Storage.

Option 2 is strongly preferred. It's a new read-only method, no risk to existing functionality:
```cpp
struct NamespaceInfo {
    std::array<uint8_t, 32> namespace_id;
    uint64_t latest_seq_num;
};
std::vector<NamespaceInfo> list_namespaces();
```

**Warning signs:** Any approach that requires BlobEngine to directly access libmdbx or break Storage's Pimpl encapsulation.

### Pitfall 4: Expensive Verification on Every Duplicate

**What goes wrong:** If you verify the signature before checking dedup, you waste ~1-2ms of ML-DSA-87 verification on every duplicate blob.

**Why it happens:** Natural instinct to verify everything before storing.

**How to avoid:** Verification must happen before storage per the requirements (reject before any storage occurs). But for duplicates, we ARE rejecting -- we're returning an "already stored" ACK. The question is whether to verify a duplicate's signature.

**Recommendation:** Always verify. The alternative (skip verification for known hashes) creates a subtle vulnerability: an attacker could send a blob with a valid hash but invalid signature, and the engine would happily return a success ACK without verifying ownership. Since dedup uses the content hash (which includes the signature bytes), a blob with a different signature would have a different hash and wouldn't be a duplicate anyway. So the verification cost is only paid for true duplicates (identical bytes), which means the signature is guaranteed valid if the first copy was verified. In practice, verify-then-store is correct and safe. The dedup check inside `Storage::store_blob()` handles the rest.

### Pitfall 5: Pubkey Size Validation Before Signer::verify

**What goes wrong:** `crypto::Signer::verify()` creates a fresh OQS_SIG context and calls `OQS_SIG_verify()` without checking the public key size. If the pubkey is the wrong size, liboqs may return an error or (less likely) read out of bounds.

**Why it happens:** The static verify method trusts the caller to pass correctly-sized keys.

**How to avoid:** BlobEngine should check `blob.pubkey.size() == crypto::Signer::PUBLIC_KEY_SIZE` (2592) as part of structural validation, before calling verify. This is both a safety check and a cheap early rejection for obviously malformed blobs.

## Code Examples

### BlobEngine Header Skeleton

```cpp
// src/engine/engine.h
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "wire/codec.h"

namespace chromatin::storage { class Storage; }

namespace chromatin::engine {

enum class IngestError {
    namespace_mismatch,
    invalid_signature,
    malformed_blob,
    storage_error
};

enum class IngestStatus {
    stored,
    duplicate
};

struct WriteAck {
    std::array<uint8_t, 32> blob_hash{};
    uint64_t seq_num = 0;
    IngestStatus status = IngestStatus::stored;
    uint32_t replication_count = 1;  // Stubbed until ACKW-02
};

struct IngestResult {
    bool accepted = false;
    std::optional<WriteAck> ack;
    std::optional<IngestError> error;
    std::string error_detail;

    static IngestResult success(WriteAck ack);
    static IngestResult rejection(IngestError err, std::string detail = "");
};

struct NamespaceInfo {
    std::array<uint8_t, 32> namespace_id{};
    uint64_t latest_seq_num = 0;
};

class BlobEngine {
public:
    explicit BlobEngine(storage::Storage& store);

    /// Validate and ingest a blob. Returns ACK on success, rejection on failure.
    IngestResult ingest(const wire::BlobData& blob);

    /// Query blobs in a namespace since a given seq_num.
    /// max_count=0 means return all.
    std::vector<wire::BlobData> get_blobs_since(
        std::span<const uint8_t, 32> namespace_id,
        uint64_t since_seq,
        uint32_t max_count = 0);

    /// Query a single blob by namespace + hash.
    std::optional<wire::BlobData> get_blob(
        std::span<const uint8_t, 32> namespace_id,
        std::span<const uint8_t, 32> blob_hash);

    /// List all namespaces with at least one stored blob.
    std::vector<NamespaceInfo> list_namespaces();

private:
    storage::Storage& storage_;
};

} // namespace chromatin::engine
```

### Ingest Pipeline Implementation Pattern

```cpp
IngestResult BlobEngine::ingest(const wire::BlobData& blob) {
    // 1. Structural validation (cheapest)
    if (blob.pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        spdlog::warn("Rejected blob: invalid pubkey size {}", blob.pubkey.size());
        return IngestResult::rejection(IngestError::malformed_blob,
            "pubkey size " + std::to_string(blob.pubkey.size()) +
            " != " + std::to_string(crypto::Signer::PUBLIC_KEY_SIZE));
    }
    if (blob.signature.empty()) {
        spdlog::warn("Rejected blob: empty signature");
        return IngestResult::rejection(IngestError::malformed_blob, "empty signature");
    }

    // 2. Namespace ownership (SHA3 hash comparison, ~1us)
    auto derived_ns = crypto::sha3_256(blob.pubkey);
    if (derived_ns != blob.namespace_id) {
        // Log truncated namespace for debugging
        spdlog::warn("Rejected blob: namespace mismatch (claimed {:02x}{:02x}...)",
            blob.namespace_id[0], blob.namespace_id[1]);
        return IngestResult::rejection(IngestError::namespace_mismatch);
    }

    // 3. Signature verification (ML-DSA-87, ~1-2ms)
    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    if (!crypto::Signer::verify(signing_input, blob.signature, blob.pubkey)) {
        spdlog::warn("Rejected blob: invalid signature (ns {:02x}{:02x}...)",
            blob.namespace_id[0], blob.namespace_id[1]);
        return IngestResult::rejection(IngestError::invalid_signature);
    }

    // 4. Store (dedup handled by Storage)
    auto result = storage_.store_blob(blob);
    // ... build ACK from result ...
}
```

### Test Pattern: Creating Properly Signed Blobs

```cpp
// Tests need real cryptographic blobs, not the fake ones from Phase 2 tests.
// Use NodeIdentity to generate valid keypairs and sign blobs.

chromatin::wire::BlobData make_signed_blob(
    const chromatin::identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 1000)
{
    chromatin::wire::BlobData blob;
    auto ns = id.namespace_id();
    std::copy(ns.begin(), ns.end(), blob.namespace_id.begin());
    auto pk = id.public_key();
    blob.pubkey.assign(pk.begin(), pk.end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;

    auto signing_input = chromatin::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}
```

### Test Pattern: Creating Intentionally Invalid Blobs

```cpp
// For rejection tests, tamper with specific fields of a valid blob.

// Wrong namespace (namespace_mismatch):
auto blob = make_signed_blob(id, "data");
blob.namespace_id.fill(0xFF);  // Doesn't match SHA3(pubkey)

// Bad signature (invalid_signature):
auto blob = make_signed_blob(id, "data");
blob.signature[0] ^= 0xFF;  // Corrupt one byte

// Wrong pubkey size (malformed_blob):
auto blob = make_signed_blob(id, "data");
blob.pubkey.resize(100);  // Wrong size
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Opaque rejection | Specific error codes (namespace_mismatch, invalid_signature, malformed_blob) | CONTEXT.md decision | Better debugging, client can react to specific failures |
| Verify-then-store as two separate steps | Single atomic ingest() call | Phase 3 design | No partial state; verification and storage are one operation |

**Nothing deprecated/outdated** -- this phase uses only the existing Phase 1/2 infrastructure.

## Open Questions

1. **Storage::store_blob return type enhancement**
   - What we know: BlobEngine needs seq_num and blob_hash from the store operation for the write ACK
   - What's unclear: Whether to change the existing `StoreResult` enum to a struct (breaking change to Phase 2 tests) or add a new method
   - Recommendation: Change `StoreResult` to a struct. Phase 2 tests will need updates (comparing `result.status` instead of `result`), but it's cleaner than maintaining two store methods. This is pre-MVP, no backward compat needed.

2. **Duplicate seq_num lookup cost**
   - What we know: Idempotent duplicates must return the existing seq_num
   - What's unclear: Whether cursor-scanning the namespace's seq entries is fast enough for large namespaces
   - Recommendation: Cursor scan is fine for now. The sequence index is sorted, and we can optimize with a reverse index later if profiling shows it's a bottleneck. YAGNI.

3. **list_namespaces in Storage or BlobEngine**
   - What we know: The sequence index contains all the data needed (distinct namespace prefixes + max seq_num)
   - What's unclear: Whether this belongs as a Storage method or BlobEngine logic
   - Recommendation: Add to Storage as a read-only method. Keeps all libmdbx access inside Storage's Pimpl boundary.

## Sources

### Primary (HIGH confidence)

- **Existing codebase** -- all code references verified by reading src/ headers and implementations:
  - `src/crypto/signing.h` -- `Signer::verify()` static method, PUBLIC_KEY_SIZE=2592
  - `src/crypto/hash.h` -- `sha3_256()` function
  - `src/wire/codec.h` -- `build_signing_input()`, `blob_hash()`, `encode_blob()`, `BlobData` struct
  - `src/storage/storage.h` -- `Storage` class, `StoreResult` enum, all query methods
  - `src/storage/storage.cpp` -- `store_blob()` implementation showing seq_num computation inside write txn
  - `src/identity/identity.h` -- `NodeIdentity` for test helpers

### Secondary (MEDIUM confidence)

- **Phase 2 RESEARCH.md** -- libmdbx cursor patterns for sequence/expiry scanning (verified against codebase)
- **CONTEXT.md decisions** -- user-locked validation order, ACK contents, query interface design

### Tertiary (LOW confidence)

- None -- all findings verified against existing code

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, all components exist and are tested (64 tests, 191 assertions)
- Architecture: HIGH -- fail-fast pipeline is a well-understood pattern, BlobEngine is thin composition layer
- Pitfalls: HIGH -- all pitfalls discovered by reading the actual Storage implementation and identifying gaps

**Research date:** 2026-03-03
**Valid until:** 2026-04-03 (stable -- no external dependencies to go stale)
