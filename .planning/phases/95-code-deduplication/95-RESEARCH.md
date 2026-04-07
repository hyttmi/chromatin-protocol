# Phase 95: Code Deduplication - Research

**Researched:** 2026-04-07
**Domain:** C++20 code refactoring -- extracting duplicate encoding/decoding patterns into shared utility headers
**Confidence:** HIGH

## Summary

Phase 95 is a pure refactoring phase with no new features or wire protocol changes. The codebase contains four distinct categories of duplication: (1) big-endian integer encoding/decoding scattered across 8+ source files as file-local static functions and inline shift patterns, (2) auth payload encode/decode duplicated 4 times in connection.cpp from handshake.cpp's static helpers, (3) signature verification with optional thread pool offload duplicated 6 times across connection.cpp and engine.cpp, and (4) namespace+hash extraction via memcpy(32) patterns appearing 37+ times in peer_manager.cpp alone.

The extraction targets are well-defined by CONTEXT.md decisions. Four new headers will be created under existing directory conventions (`db/util/`, `db/net/`, `db/crypto/`), each with a dedicated test file. The existing `db/util/hex.h` header under `chromatindb::util` establishes the exact pattern to follow. All work is constrained to C++ -- zero SDK changes, zero new wire types, zero behavioral changes.

The primary risk is regressions in the 615+ existing test suite. Since this is mechanical extraction with improved interfaces (bounds checking, span-based), the risk is low but must be validated under ASAN/TSAN/UBSAN after every wave.

**Primary recommendation:** Extract in dependency order -- endian.h first (no deps, most consumers), then blob_helpers.h (depends on endian.h), then auth_helpers.h (depends on endian.h), then verify_helpers.h (depends on crypto/thread_pool.h). Each extraction is independently testable and committable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Improve while extracting -- shared utilities get bounds checking, span-based interfaces, and proper error handling from day one. Not just copy-paste into a header.
- **D-02:** This means the shared helpers may have slightly different signatures than the inline originals. Call sites adapt to the new interfaces.
- **D-03:** Topic-focused headers, each near its domain:
  - `db/util/endian.h` -- BE read/write helpers under `chromatindb::util`
  - `db/net/auth_helpers.h` -- encode_auth_payload / decode_auth_payload
  - `db/crypto/verify_helpers.h` -- verify_with_offload coroutine
  - `db/util/blob_helpers.h` -- namespace/hash extraction and blob reference encoding helpers
- **D-04:** Follows existing pattern: `db/util/hex.h` already lives under `chromatindb::util`
- **D-05:** All 6 verification-with-offload sites are in scope (not just the 4 in DEDUP-03):
  - 4 identical if(pool_)/else blocks in connection.cpp (lines ~378, ~477, ~612, ~702)
  - 2 unconditional offload sites in engine.cpp (lines ~236, ~365)
- **D-06:** Extract a shared `verify_with_offload` coroutine that handles pool/no-pool branch, Signer::verify call, and error path. Engine.cpp's bundled build_signing_input+verify pattern should also use this (may need a variant or the caller bundles separately).
- **D-07:** Each new utility header gets its own dedicated test file
- **D-08:** Tests cover edge cases the inline code never tested
- **D-09:** All 615+ existing unit tests must pass under ASAN/TSAN/UBSAN with zero regressions

### Claude's Discretion
- Exact function signatures for the shared helpers (as long as they include bounds checking and use spans)
- Internal organization within each header (ordering, helper grouping)
- Whether verify_with_offload returns bool or a result type with error context

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DEDUP-01 | Centralized encoding utility header replaces all 40+ inline BE encoding loops | endian.h with write/read_u16_be, write/read_u32_be, write/read_u64_be; consumers identified in 8 source files |
| DEDUP-02 | connection.cpp uses shared encode_auth_payload/decode_auth_payload from handshake.cpp (removes 4 inline copies) | auth_helpers.h promoting handshake.cpp statics; 4 encode + 4 decode sites in connection.cpp mapped |
| DEDUP-03 | Signature verification with thread pool offload extracted to shared method (removes 4+ copies) | verify_helpers.h wrapping crypto::offload + Signer::verify; 6 total sites (4 connection.cpp + 2 engine.cpp) mapped |
| DEDUP-04 | Namespace/hash extraction helper replaces 10+ inline memcpy patterns | blob_helpers.h with extract_namespace_hash; 37+ memcpy(32) sites in peer_manager.cpp alone |
| DEDUP-05 | Blob reference encoding helper replaces 6+ inline patterns | blob_helpers.h with encode_blob_ref; BlobNotify encoding and similar patterns |
</phase_requirements>

## Standard Stack

No new dependencies. This phase uses the existing project stack exclusively.

### Core (existing, no changes)
| Library | Version | Purpose | Role in This Phase |
|---------|---------|---------|-------------------|
| Catch2 | v3.7.1 | Test framework | New test files for utility headers |
| Standalone Asio | 1.38.0 | Async/coroutines | verify_helpers.h returns `asio::awaitable<bool>` |
| liboqs | 0.15.0 | ML-DSA-87 | Signer::verify used by verify_helpers |
| spdlog | 1.15.1 | Logging | Existing log messages preserved |

### No New Dependencies
This is a refactoring phase. Zero new libraries, zero version changes.

## Architecture Patterns

### New Header Layout
```
db/
├── util/
│   ├── hex.h                 # EXISTING - pattern to follow
│   ├── endian.h              # NEW - BE read/write helpers (DEDUP-01)
│   └── blob_helpers.h        # NEW - namespace/hash extraction (DEDUP-04, DEDUP-05)
├── net/
│   ├── auth_helpers.h        # NEW - auth payload encode/decode (DEDUP-02)
│   └── ...
├── crypto/
│   ├── verify_helpers.h      # NEW - verify_with_offload (DEDUP-03)
│   ├── thread_pool.h         # EXISTING - crypto::offload used by verify_helpers
│   └── signing.h             # EXISTING - Signer::verify used by verify_helpers
└── tests/
    ├── util/
    │   ├── test_endian.cpp        # NEW
    │   └── test_blob_helpers.cpp  # NEW
    ├── net/
    │   └── test_auth_helpers.cpp  # NEW
    └── crypto/
        └── test_verify_helpers.cpp # NEW
```

### Pattern 1: Header-Only Inline Utilities (endian.h, blob_helpers.h)
**What:** Constexpr/inline functions in a header under `chromatindb::util`, matching `hex.h` pattern.
**When to use:** Pure computation with no dependencies beyond `<cstdint>`, `<span>`, `<stdexcept>`, `<array>`, `<cstring>`.
**Example signature set for endian.h:**
```cpp
#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace chromatindb::util {

// --- Writers (append to vector) ---

inline void write_u16_be(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val >> 8));
    buf.push_back(static_cast<uint8_t>(val));
}

inline void write_u32_be(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val >> 24));
    buf.push_back(static_cast<uint8_t>(val >> 16));
    buf.push_back(static_cast<uint8_t>(val >> 8));
    buf.push_back(static_cast<uint8_t>(val));
}

inline void write_u64_be(std::vector<uint8_t>& buf, uint64_t val) {
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<uint8_t>(val >> (i * 8)));
}

// --- Writers (to fixed buffer, with bounds check) ---

inline void store_u32_be(uint8_t* out, uint32_t val) {
    out[0] = static_cast<uint8_t>(val >> 24);
    out[1] = static_cast<uint8_t>(val >> 16);
    out[2] = static_cast<uint8_t>(val >> 8);
    out[3] = static_cast<uint8_t>(val);
}

inline void store_u64_be(uint8_t* out, uint64_t val) {
    for (int i = 7; i >= 0; --i)
        out[7 - i] = static_cast<uint8_t>(val >> (i * 8));
}

// --- Readers (from span with bounds check) ---

inline uint16_t read_u16_be(std::span<const uint8_t> data) {
    if (data.size() < 2)
        throw std::out_of_range("read_u16_be: need 2 bytes, got " +
                                std::to_string(data.size()));
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

inline uint32_t read_u32_be(std::span<const uint8_t> data) {
    if (data.size() < 4)
        throw std::out_of_range("read_u32_be: need 4 bytes, got " +
                                std::to_string(data.size()));
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

inline uint64_t read_u64_be(std::span<const uint8_t> data) {
    if (data.size() < 8)
        throw std::out_of_range("read_u64_be: need 8 bytes, got " +
                                std::to_string(data.size()));
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
        val = (val << 8) | data[i];
    return val;
}

// --- Readers (raw pointer, no bounds check -- for pre-validated buffers) ---

inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t read_u64_be(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
        val = (val << 8) | p[i];
    return val;
}

} // namespace chromatindb::util
```

**Key design decisions:**
- Two reader overloads: `span<const uint8_t>` (bounds-checked, for new/untrusted code) and `const uint8_t*` (unchecked, for pre-validated internal paths where length was already checked). The span overload is preferred at call sites.
- Two writer overloads: `write_*_be(vector&, val)` for appending (like sync_protocol/reconciliation patterns) and `store_*_be(uint8_t*, val)` for writing to pre-allocated buffers (like storage.cpp/framing.cpp patterns).
- All inline in the header -- matches hex.h pattern, no .cpp needed.

### Pattern 2: Promoted Static Helpers (auth_helpers.h)
**What:** Functions currently static in handshake.cpp, promoted to a shared header under `chromatindb::net`.
**When to use:** When the implementation is already correct but access-restricted.
**Key improvement over current code:** The `decode_auth_payload` should use endian.h's `read_u32_le` (or keep the current LE pattern since auth payload uses LE encoding per the protocol) with bounds checking via span.

**Important detail:** Auth payload uses LITTLE-endian for the pubkey size field (see handshake.cpp line 120-124). This is intentional per the protocol. Do NOT convert to BE -- this is a different encoding from the wire protocol's BE. The endian.h helpers should include LE variants for u32 or auth_helpers.h should handle LE internally.

### Pattern 3: Coroutine Helper (verify_helpers.h)
**What:** A shared coroutine that wraps the pool/no-pool dispatch pattern.
**When to use:** Any signature verification that may optionally use a thread pool.
**Design considerations:**
- connection.cpp sites have `pool_` as a raw pointer (nullable), engine.cpp always has a pool reference
- The helper must handle both cases: `asio::thread_pool* pool` (nullable) and `asio::thread_pool& pool` (always valid)
- Recommended: single function taking `asio::thread_pool* pool` -- engine.cpp passes `&pool_`, connection.cpp passes `pool_`
- Engine.cpp bundles `build_signing_input + verify` in one offload call for efficiency -- the helper should NOT break this bundling. Engine.cpp should continue to use `crypto::offload` directly for the bundled pattern, but could use the helper for the verify-only portion.

**Recommended signature:**
```cpp
namespace chromatindb::crypto {

/// Verify a signature with optional thread pool offload.
/// If pool is non-null, dispatches to pool. Otherwise verifies inline.
asio::awaitable<bool> verify_with_offload(
    asio::thread_pool* pool,
    std::span<const uint8_t> message,
    std::span<const uint8_t> signature,
    std::span<const uint8_t> public_key);

} // namespace chromatindb::crypto
```

**Engine.cpp consideration:** The 2 engine.cpp sites bundle `build_signing_input + Signer::verify` in a single offload call. This is a performance optimization (one thread pool dispatch instead of two). Options:
1. Keep engine.cpp using `crypto::offload` directly for the bundled pattern (recommended -- don't break the optimization).
2. Create a second helper `verify_blob_with_offload` that bundles both operations.
Option 1 is cleaner. The CONTEXT.md D-06 says "Engine.cpp's bundled build_signing_input+verify pattern should also use this (may need a variant or the caller bundles separately)" -- the "caller bundles separately" path is simplest.

### Pattern 4: Blob Extraction Helpers (blob_helpers.h)
**What:** Helper functions for the repeated namespace+hash extraction pattern.
**Where used:** peer_manager.cpp has 37+ memcpy(data, 32) patterns extracting namespace_id and blob_hash from message payloads.

**Recommended functions:**
```cpp
namespace chromatindb::util {

/// Extract namespace_id (first 32 bytes) from a payload.
/// Throws if payload is too short.
inline std::array<uint8_t, 32> extract_namespace(std::span<const uint8_t> payload) {
    if (payload.size() < 32)
        throw std::out_of_range("extract_namespace: need 32 bytes");
    std::array<uint8_t, 32> ns{};
    std::memcpy(ns.data(), payload.data(), 32);
    return ns;
}

/// Extract namespace_id + blob_hash (first 64 bytes) from a payload.
/// Throws if payload is too short.
inline std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>
extract_namespace_hash(std::span<const uint8_t> payload) {
    if (payload.size() < 64)
        throw std::out_of_range("extract_namespace_hash: need 64 bytes");
    std::array<uint8_t, 32> ns{}, hash{};
    std::memcpy(ns.data(), payload.data(), 32);
    std::memcpy(hash.data(), payload.data() + 32, 32);
    return {ns, hash};
}

/// Encode namespace_id + blob_hash into a 64-byte payload.
inline std::vector<uint8_t> encode_namespace_hash(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t, 32> blob_hash) {
    std::vector<uint8_t> result(64);
    std::memcpy(result.data(), namespace_id.data(), 32);
    std::memcpy(result.data() + 32, blob_hash.data(), 32);
    return result;
}

} // namespace chromatindb::util
```

### Anti-Patterns to Avoid
- **Mega-header with everything:** Don't put all helpers in one file. Topic-focused headers per D-03.
- **Breaking existing optimizations:** Don't force engine.cpp's bundled verify through a single-purpose helper that would require two offload dispatches.
- **Changing LE to BE in auth payload:** The auth payload format uses LE for pubkey_size. Do not "fix" this to BE -- it's protocol-defined.
- **Making endian readers throw on pre-validated paths:** Provide both checked (span) and unchecked (raw pointer) overloads. Many call sites have already validated payload size before reading.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Big-endian encoding | Inline shift expressions | `db/util/endian.h` | 8 files have identical patterns; DRY + bounds checking |
| Auth payload serialization | Copy-paste from handshake.cpp | `db/net/auth_helpers.h` | 4 copies in connection.cpp; single source of truth |
| Verify + offload dispatch | Inline if(pool_)/else | `db/crypto/verify_helpers.h` | 4 identical blocks in connection.cpp |
| Namespace extraction | Inline memcpy(ns.data(), payload.data(), 32) | `db/util/blob_helpers.h` | 37+ sites; bounds checking added |

## Common Pitfalls

### Pitfall 1: Auth Payload Uses Little-Endian (Not Big-Endian)
**What goes wrong:** Assuming auth_payload pubkey_size is big-endian like the rest of the wire protocol, and "fixing" it with endian.h BE helpers.
**Why it happens:** Every other integer in the protocol is big-endian, so it's natural to assume auth is too. But handshake.cpp lines 120-124 explicitly encode as LE: `payload.push_back(pk_size & 0xFF)` (LSB first).
**How to avoid:** auth_helpers.h must use LE encoding for pubkey_size. Either add read/write_u32_le to endian.h or handle LE internally in auth_helpers.h.
**Warning signs:** Handshake tests passing with existing test nodes but failing with fresh keys or different key sizes.

### Pitfall 2: Ambiguous Overload Resolution for read_u32_be
**What goes wrong:** `read_u32_be(payload.data())` is ambiguous if both `read_u32_be(const uint8_t*)` and `read_u32_be(span<const uint8_t>)` exist, because `const uint8_t*` can implicitly convert to span.
**Why it happens:** C++ implicit conversions from raw pointers to spans.
**How to avoid:** The raw-pointer overload will match `const uint8_t*` exactly (better match than span). For span paths, callers should explicitly construct: `read_u32_be(span{payload}.subspan(offset, 4))`. Test compilation early.
**Warning signs:** Compile errors about ambiguous overloads.

### Pitfall 3: verify_with_offload Captures Dangling References
**What goes wrong:** The coroutine captures span parameters by reference/value, but the underlying buffers are moved or deallocated before the thread pool executes.
**Why it happens:** `crypto::offload` posts a lambda to the thread pool. If the coroutine frame holds spans pointing to local vectors that get destroyed, UB.
**How to avoid:** The verify_with_offload coroutine must capture the span data correctly. Since the co_await suspends until offload completes, and the caller's coroutine frame (which owns the data) stays alive during co_await, the spans remain valid. This matches the existing pattern in connection.cpp. TSAN testing will catch any issues.
**Warning signs:** ASAN/TSAN failures in verify paths under load.

### Pitfall 4: Breaking storage.cpp's Fixed-Buffer Encoding
**What goes wrong:** Replacing `encode_be_u64(val, out)` (writes to `uint8_t*`) with `write_u64_be(vec, val)` (appends to vector). Storage.cpp writes to pre-allocated fixed-size arrays, not vectors.
**Why it happens:** Two different write patterns exist: append-to-vector (sync_protocol, reconciliation) and write-to-buffer (storage, framing, peer_manager).
**How to avoid:** Provide both overloads: `write_u64_be(vector&, val)` for appending and `store_u64_be(uint8_t*, val)` for fixed buffers. Map each call site to the correct overload.
**Warning signs:** Compile errors (wrong parameter types) or corrupted storage keys.

### Pitfall 5: CMakeLists.txt Test Registration
**What goes wrong:** New test files added but not registered in `db/CMakeLists.txt`'s `add_executable(chromatindb_tests ...)` list.
**Why it happens:** Manual test file registration (no glob pattern).
**How to avoid:** Add all 4 new test files to the `chromatindb_tests` executable source list. Create the `db/tests/util/` directory (does not exist yet).
**Warning signs:** Tests compile but new tests don't run.

### Pitfall 6: Namespace of verify_helpers.h
**What goes wrong:** Putting verify_helpers in `chromatindb::util` instead of `chromatindb::crypto`.
**Why it happens:** It's a "helper" so it seems like util.
**How to avoid:** It wraps `crypto::Signer::verify` and `crypto::offload` -- it belongs in `chromatindb::crypto`. Per D-03, the file lives at `db/crypto/verify_helpers.h`.
**Warning signs:** Confusing imports, namespace mismatches.

## Code Examples

### Existing Pattern: hex.h (template to follow)
```cpp
// Source: db/util/hex.h
#pragma once
#include <span>
#include <string>
namespace chromatindb::util {
inline std::string to_hex(std::span<const uint8_t> bytes) { ... }
} // namespace chromatindb::util
```

### Existing Pattern: crypto::offload (used by verify_helpers)
```cpp
// Source: db/crypto/thread_pool.h
template <typename F>
    requires std::invocable<F>
asio::awaitable<std::invoke_result_t<F>> offload(asio::thread_pool& pool, F&& fn) {
    // Posts fn to pool, co_returns result
}
```

### Existing Duplication: connection.cpp verify pattern (4 copies)
```cpp
// Source: db/net/connection.cpp lines 378-388, 477-487, 610-620, 700-710
bool valid;
if (pool_) {
    valid = co_await crypto::offload(*pool_,
        [&]() {
            return crypto::Signer::verify(
                session_keys_.session_fingerprint, resp_sig, resp_pk);
        });
} else {
    valid = crypto::Signer::verify(
        session_keys_.session_fingerprint, resp_sig, resp_pk);
}
```

### Existing Duplication: connection.cpp auth encode (4 copies)
```cpp
// Source: db/net/connection.cpp lines 338-346, 436-444, 630-638, 720-728
std::vector<uint8_t> auth_payload;
auto pk = identity_.public_key();
uint32_t pk_size = static_cast<uint32_t>(pk.size());
auth_payload.push_back(static_cast<uint8_t>(pk_size & 0xFF));
auth_payload.push_back(static_cast<uint8_t>((pk_size >> 8) & 0xFF));
auth_payload.push_back(static_cast<uint8_t>((pk_size >> 16) & 0xFF));
auth_payload.push_back(static_cast<uint8_t>((pk_size >> 24) & 0xFF));
auth_payload.insert(auth_payload.end(), pk.begin(), pk.end());
auth_payload.insert(auth_payload.end(), sig.begin(), sig.end());
```

### Existing Duplication: sync_protocol.cpp + reconciliation.cpp (identical file-local helpers)
```cpp
// Source: db/sync/sync_protocol.cpp lines 129-163
// Source: db/sync/reconciliation.cpp lines 15-26
// Identical write_u32_be, read_u32_be in both files
namespace {
void write_u32_be(std::vector<uint8_t>& buf, uint32_t val) { ... }
uint32_t read_u32_be(const uint8_t* p) { ... }
}
```

### Existing Duplication: storage.cpp file-local helpers
```cpp
// Source: db/storage/storage.cpp lines 27-91
static void encode_be_u64(uint64_t val, uint8_t* out) { ... }
static uint64_t decode_be_u64(const uint8_t* data) { ... }
static void encode_be_u32(uint32_t val, uint8_t* out) { ... }
static uint32_t decode_be_u32(const uint8_t* data) { ... }
```

## Duplication Inventory

### DEDUP-01: Big-Endian Encoding (endian.h)

| File | Lines | Pattern | Count |
|------|-------|---------|-------|
| sync_protocol.cpp | 129-163 | File-local write_u32_be, write_u64_be, read_u32_be, read_u64_be | 4 functions |
| reconciliation.cpp | 15-26 | File-local write_u32_be, read_u32_be (identical to sync_protocol) | 2 functions |
| storage.cpp | 27-91 | File-local encode_be_u64, decode_be_u64, encode_be_u32, decode_be_u32 | 4 functions |
| framing.cpp | 32-35 | Inline 4-byte BE write | 1 site |
| peer_manager.cpp | ~30 sites | Inline `for (int i = 7; i >= 0; --i)` loops and `(x << 8) \| payload[n]` reads | ~30 sites |
| connection.cpp | ~8 sites | Inline LE shift patterns for auth payload + occasional BE | ~8 sites |
| codec.cpp | 79-98 | Inline LE encoding for ttl/timestamp in build_signing_input | See note |

**Note on codec.cpp:** The `build_signing_input` function in codec.cpp uses **little-endian** encoding for ttl and timestamp (protocol-defined canonical signing input). These are NOT candidates for endian.h BE helpers -- they stay as-is or get separate LE helpers.

### DEDUP-02: Auth Payload (auth_helpers.h)

| File | Lines | Pattern |
|------|-------|---------|
| handshake.cpp | 114-151 | `static encode_auth_payload()` + `static decode_auth_payload()` -- source of truth |
| connection.cpp | 338-346 | Inline auth encode (initiator trusted fallback) |
| connection.cpp | 436-444 | Inline auth encode (initiator PQ) |
| connection.cpp | 630-638 | Inline auth encode (responder PQ fallback) |
| connection.cpp | 720-728 | Inline auth encode (responder PQ) |
| connection.cpp | 366-376 | Inline auth decode (initiator trusted fallback) |
| connection.cpp | 465-475 | Inline auth decode (initiator PQ) |
| connection.cpp | 597-608 | Inline auth decode (responder PQ fallback) |
| connection.cpp | 686-698 | Inline auth decode (responder PQ) |

Total: 4 inline encode + 4 inline decode in connection.cpp to replace with calls to promoted helpers from handshake.cpp.

### DEDUP-03: Verify with Offload (verify_helpers.h)

| File | Lines | Pattern |
|------|-------|---------|
| connection.cpp | 378-388 | if(pool_) offload verify, else inline verify |
| connection.cpp | 477-487 | Identical pattern |
| connection.cpp | 610-620 | Identical pattern |
| connection.cpp | 700-710 | Identical pattern |
| engine.cpp | 236-241 | Unconditional offload: bundled build_signing_input + verify |
| engine.cpp | 360-368 | Unconditional offload: bundled build_signing_input + verify |

Connection.cpp sites: 4 identical blocks -- direct replacement with `verify_with_offload(pool_, message, sig, pk)`.
Engine.cpp sites: 2 blocks that bundle build_signing_input -- caller continues to use crypto::offload directly for the bundled pattern (per analysis in Architecture Patterns section).

### DEDUP-04 + DEDUP-05: Blob Helpers (blob_helpers.h)

| File | Pattern | Count |
|------|---------|-------|
| peer_manager.cpp | `std::memcpy(ns.data(), payload.data(), 32)` | 16 sites |
| peer_manager.cpp | `std::memcpy(hash.data(), payload.data() + 32, 32)` | 13 sites |
| peer_manager.cpp | `std::memcpy(result.data() + offset, hash.data(), 32)` (blob ref encoding) | 8 sites |
| peer_manager.cpp | BlobNotify encoding at line 3470+ | 1 site |

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `std::endian` + `std::byteswap` (C++23) | Manual shift patterns | C++23 not available (project uses C++20) | Continue with shift patterns in endian.h |
| `std::bit_cast` (C++20) | Available but not needed | C++20 | Shift patterns are clearer for endianness; bit_cast is for type punning |

No deprecated patterns apply -- the project is on C++20 and the manual shift approach is standard practice.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | `db/CMakeLists.txt` (lines 203-253) |
| Quick run command | `cd build/db && ctest --output-on-failure -j4` |
| Full suite command | `cmake -B build -DSANITIZER=asan && cmake --build build && cd build/db && ctest --output-on-failure` (repeat for tsan, ubsan) |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DEDUP-01 | BE read/write helpers correct for all widths, bounds checking throws | unit | `cd build/db && ctest -R test_endian --output-on-failure` | Wave 0 |
| DEDUP-02 | Auth payload encode/decode round-trips, malformed input rejected | unit | `cd build/db && ctest -R test_auth_helpers --output-on-failure` | Wave 0 |
| DEDUP-03 | verify_with_offload returns correct bool with and without pool | unit | `cd build/db && ctest -R test_verify_helpers --output-on-failure` | Wave 0 |
| DEDUP-04 | Namespace/hash extraction from valid and short payloads | unit | `cd build/db && ctest -R test_blob_helpers --output-on-failure` | Wave 0 |
| DEDUP-05 | Blob reference encoding produces correct byte layout | unit | `cd build/db && ctest -R test_blob_helpers --output-on-failure` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build/db && ctest --output-on-failure` (all unit tests, no sanitizers)
- **Per wave merge:** `cmake -B build-asan -DSANITIZER=asan && cmake --build build-asan && cd build-asan/db && ctest --output-on-failure` (repeat for tsan, ubsan)
- **Phase gate:** Full suite green under ASAN + TSAN + UBSAN before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/util/test_endian.cpp` -- covers DEDUP-01
- [ ] `db/tests/util/test_blob_helpers.cpp` -- covers DEDUP-04, DEDUP-05
- [ ] `db/tests/net/test_auth_helpers.cpp` -- covers DEDUP-02
- [ ] `db/tests/crypto/test_verify_helpers.cpp` -- covers DEDUP-03
- [ ] `db/tests/util/` directory creation (does not exist yet)
- [ ] 4 new test .cpp files added to `chromatindb_tests` sources in `db/CMakeLists.txt`

## Open Questions

1. **LE helpers in endian.h or internal to auth_helpers.h?**
   - What we know: Auth payload uses LE for pubkey_size. All other protocol integers are BE.
   - What's unclear: Whether to add write_u32_le/read_u32_le to endian.h (general purpose) or keep LE encoding private to auth_helpers.h (single consumer).
   - Recommendation: Keep LE internal to auth_helpers.h -- there's exactly one LE consumer (auth payload), so YAGNI applies. If a second consumer appears, promote then.

2. **Engine.cpp bundled verify: use helper or not?**
   - What we know: Engine.cpp bundles build_signing_input + verify in one offload for performance.
   - What's unclear: D-06 says "may need a variant or the caller bundles separately."
   - Recommendation: Caller bundles separately. Engine.cpp continues to use `crypto::offload` directly for the bundled pattern. The 4 connection.cpp sites use `verify_with_offload`. This minimizes changes to engine.cpp while still deduplicating the 4 connection.cpp sites.

3. **store_u32_be/store_u64_be naming vs encode_be_u32/encode_be_u64**
   - What we know: storage.cpp uses `encode_be_u64`, sync uses `write_u32_be`, peer_manager uses inline loops.
   - Recommendation: Use `write_*_be` for vector-append and `store_*_be` for fixed-buffer write. Consistent naming, clear distinction.

## Sources

### Primary (HIGH confidence)
- Direct source code inspection of all 8 duplication target files
- `db/util/hex.h` -- existing utility header pattern (verified)
- `db/crypto/thread_pool.h` -- existing offload pattern (verified)
- `db/CMakeLists.txt` -- test registration pattern (verified)
- `db/net/handshake.cpp` -- auth payload source of truth (verified)

### Secondary (HIGH confidence)
- `95-CONTEXT.md` -- all locked decisions verified against source code
- REQUIREMENTS.md -- all 5 DEDUP requirements mapped to source

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, existing stack only
- Architecture: HIGH -- patterns derived from existing codebase conventions (hex.h, thread_pool.h)
- Duplication inventory: HIGH -- every site inspected in source code
- Pitfalls: HIGH -- derived from actual code patterns (LE vs BE, overload resolution, buffer types)

**Research date:** 2026-04-07
**Valid until:** 2026-05-07 (stable -- internal refactoring, no external dependencies)
