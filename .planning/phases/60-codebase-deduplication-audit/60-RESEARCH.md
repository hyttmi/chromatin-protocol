# Phase 60: Codebase Deduplication Audit - Research

**Researched:** 2026-03-23
**Domain:** C++20 codebase refactoring -- extracting duplicated utility functions into shared headers
**Confidence:** HIGH

## Summary

The codebase contains significantly more duplication than the roadmap's "known duplicates" list suggests. Beyond the 3 production `to_hex()` copies and 3 test `to_hex()`/`ns_to_hex()` copies called out in the roadmap, there are 7 copies of `TempDir` across test files, 4 copies of `run_async`, 3 copies of `current_timestamp`/`TS_AUTO`/`make_signed_blob`/`make_signed_tombstone`, and 2 copies of `make_signed_delegation`/`make_delegate_blob` (with slightly different names in test_sync_protocol.cpp). The loadgen and tools binaries also contain `to_hex`/`from_hex` copies but are standalone executables -- they should still be deduplicated via the shared header.

The deduplication splits cleanly into two shared headers: (1) a production utility header `db/util/hex.h` for `to_hex()` (used by both `db/` and `relay/` production code), and (2) a test utility header `db/tests/test_helpers.h` for test-only helpers (`TempDir`, `run_async`, `current_timestamp`, `make_signed_blob`, `make_signed_tombstone`, `make_signed_delegation`, `make_delegate_blob`, `ns_to_hex`). No CMake changes needed for the headers since they are header-only and the include paths already cover `$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>`.

**Primary recommendation:** Create `db/util/hex.h` (header-only, inline functions in `chromatindb::util` namespace) and `db/tests/test_helpers.h` (header-only test utilities). Replace all anonymous-namespace copies. The peer_manager.cpp variant with `max_len` truncation should become an additional overload in the shared header.

## Standard Stack

Not applicable -- this is a refactoring phase using only existing project infrastructure. No new libraries.

### Relevant Existing Infrastructure

| Component | Location | Purpose |
|-----------|----------|---------|
| chromatindb_lib | db/CMakeLists.txt | Static library, include root is `..` (project root) |
| chromatindb_relay_lib | relay/CMakeLists.txt | Static library, links chromatindb_lib |
| chromatindb_tests | db/CMakeLists.txt | Single test binary, links both libs + Catch2 |
| Catch2 v3.7.1 | FetchContent | Test framework |

## Architecture Patterns

### Recommended Shared Header Locations

```
db/
  util/
    hex.h              # NEW: to_hex(), from_hex() -- production utility
  tests/
    test_helpers.h     # NEW: TempDir, run_async, make_signed_blob, etc.
    acl/
    config/
    crypto/
    engine/
    ...
```

**Why `db/util/hex.h`:** Following the existing `db/{module}/{module}.h` pattern. The `util/` subdirectory is the standard C++ convention for small utilities that don't belong to any specific module. Since the include path is `$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>`, consumers include as `#include "db/util/hex.h"`.

**Why `db/tests/test_helpers.h`:** Co-located with test files. All test files already compile as part of `chromatindb_tests` target, so no CMake changes needed. Include path already covers it.

**Why NOT a top-level `common/` directory:** The project has a clear two-layer structure (db/ and relay/). Relay links chromatindb_lib, so anything in db/ is already accessible to relay code. A top-level `common/` would add CMake complexity for zero benefit.

### Pattern 1: Header-Only Inline Utilities

**What:** Small utility functions defined as `inline` in headers, avoiding the need for a .cpp file or CMake source list changes.
**When to use:** Functions that are trivially small (< 20 lines), have no external dependencies beyond the standard library, and are used across multiple translation units.
**Example:**

```cpp
// db/util/hex.h
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>

namespace chromatindb::util {

/// Convert bytes to lowercase hex string.
inline std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

/// Convert bytes to lowercase hex string, truncated to max_bytes.
/// Useful for logging (e.g., first 8 bytes of a public key).
inline std::string to_hex(std::span<const uint8_t> bytes, size_t max_bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    size_t len = std::min(bytes.size(), max_bytes);
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex_chars[(bytes[i] >> 4) & 0xF];
        result += hex_chars[bytes[i] & 0xF];
    }
    return result;
}

} // namespace chromatindb::util
```

### Pattern 2: Test Helper Header

**What:** Shared test utilities in a single header, avoiding per-test-file anonymous namespace duplication.
**When to use:** Helper functions/classes used by 3+ test files with identical or near-identical implementations.
**Key design choice:** The `TempDir` prefix string varies per test file (`chromatindb_test_peer_`, `chromatindb_test_sync_`, etc.). The shared version should accept an optional prefix parameter or use a single consistent prefix like `chromatindb_test_`.

```cpp
// db/tests/test_helpers.h
#pragma once

#include "db/identity/identity.h"
#include "db/wire/codec.h"

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

namespace chromatindb::test {

/// RAII temporary directory for test isolation.
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& prefix = "chromatindb_test_") {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = std::filesystem::temp_directory_path() /
               (prefix + std::to_string(dist(gen)));
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Run an awaitable synchronously using a temporary io_context.
template <typename T>
T run_async(asio::thread_pool& pool, asio::awaitable<T> aw) {
    asio::io_context ioc;
    T result{};
    asio::co_spawn(ioc, [&result, a = std::move(aw)]() mutable -> asio::awaitable<T> {
        result = co_await std::move(a);
        co_return result;
    }, asio::detached);
    ioc.run();
    return result;
}

/// Get current Unix timestamp in seconds.
inline uint64_t current_timestamp() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Sentinel: pass as timestamp to auto-use current system time.
constexpr uint64_t TS_AUTO = UINT64_MAX;

/// Build a properly signed BlobData using a NodeIdentity.
inline wire::BlobData make_signed_blob(
    const identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;
    auto signing_input = wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);
    return blob;
}

// ... make_signed_tombstone, make_signed_delegation, make_delegate_blob ...

} // namespace chromatindb::test
```

### Anti-Patterns to Avoid

- **Splitting test helpers into too many headers:** One `test_helpers.h` is better than `test_hex.h`, `test_tempdir.h`, `test_blob_factory.h`. The helpers are small, always used together, and adding includes is free.
- **Making test helpers compile-time expensive:** Keep them header-only. Adding a `test_helpers.cpp` would require CMake source list changes and complicate the build for no benefit.
- **Changing function signatures gratuitously:** The shared functions should be drop-in replacements. Callers should not need logic changes, only `#include` and namespace changes.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Hex encoding | Anonymous-namespace `to_hex()` per file | Single `db/util/hex.h` | 8+ copies already exist with subtle signature differences |
| Test blob factories | Per-test-file `make_signed_blob()` | Single `db/tests/test_helpers.h` | 4+ identical copies, bug in one = silent divergence |
| Temp directory RAII | Per-test-file `struct TempDir` | Single `TempDir` in test_helpers.h | 7 copies with only prefix string differing |

**Key insight:** Every anonymous-namespace copy is a maintenance liability. When the signing protocol changes (as it did in v0.7.0 with hash-then-sign), every `make_signed_blob` copy must be updated identically. With 4 copies, this is already error-prone.

## Complete Duplication Inventory

### Production Code Duplicates

| Function | Signature | Files | Notes |
|----------|-----------|-------|-------|
| `to_hex` | `(span<const uint8_t>) -> string` | db/main.cpp, relay/relay_main.cpp, relay/core/relay_session.cpp, loadgen/loadgen_main.cpp | Identical implementations |
| `to_hex` | `(span<const uint8_t>, size_t max_len) -> string` | db/peer/peer_manager.cpp | Truncating variant for logging |
| `to_hex` | `(const array<uint8_t, 32>&) -> string` | loadgen/loadgen_main.cpp | Array overload, same logic |
| `to_hex` | `(const uint8_t*, size_t) -> string` | tools/verify_main.cpp | Pointer+size overload |
| `to_hex` | `(const array<uint8_t, N>&) -> string` | tools/verify_main.cpp | Template overload wrapping pointer version |
| `from_hex` | `(const string&) -> array<uint8_t, 32>` | loadgen/loadgen_main.cpp | Fixed-size 32-byte return |
| `from_hex` | `(const string&) -> vector<uint8_t>` | tools/verify_main.cpp | Variable-size return |
| `from_hex_bytes` | `(const string&) -> vector<uint8_t>` | loadgen/loadgen_main.cpp | Same as verify's from_hex |

**Total production copies:** 8 `to_hex` + 3 `from_hex` = 11 function copies across 5 files.

### Test Code Duplicates

| Function | Files (copies) | Identical? |
|----------|----------------|------------|
| `TempDir` | test_peer_manager.cpp, test_sync_protocol.cpp, test_engine.cpp, test_daemon.cpp, test_storage.cpp, test_master_key.cpp, test_relay_identity.cpp (7) | Yes, only prefix string differs. test_relay_identity creates dir immediately. |
| `run_async` | test_peer_manager.cpp, test_sync_protocol.cpp, test_engine.cpp, test_daemon.cpp (4) | Identical |
| `current_timestamp` | test_peer_manager.cpp, test_sync_protocol.cpp, test_engine.cpp (3) | Identical |
| `TS_AUTO` | test_peer_manager.cpp, test_sync_protocol.cpp, test_engine.cpp (3) | Identical |
| `make_signed_blob` | test_peer_manager.cpp, test_sync_protocol.cpp, test_engine.cpp (3) + test_daemon.cpp (variant: default timestamp=1000 instead of TS_AUTO) | 3 identical + 1 variant |
| `make_signed_tombstone` | test_peer_manager.cpp, test_sync_protocol.cpp, test_engine.cpp (3) | Identical |
| `make_signed_delegation` | test_engine.cpp (1) + test_sync_protocol.cpp as `make_signed_delegation_sync` (1) | Identical logic, different names |
| `make_delegate_blob` | test_engine.cpp (1) + test_sync_protocol.cpp as `make_delegate_blob_sync` (1) | Identical logic, different names |
| `to_hex` (32-byte span) | test_access_control.cpp (1) | Unique signature: `span<const uint8_t, 32>` |
| `ns_to_hex` | test_peer_manager.cpp, test_engine.cpp (2) | Identical (same as `to_hex` for 32 bytes) |
| `make_ns_hash` | test_access_control.cpp (1) | Unique to ACL tests, not duplicated |
| `make_test_blob` | test_storage.cpp (1), test_codec.cpp (1) | Different signatures -- storage version is more complex |

**Total test copies:** ~30 function copies across 7 test files.

### NOT Duplicated (Unique Per File)

These are in scope for audit but do NOT need deduplication:

| Function | File | Reason |
|----------|------|--------|
| `test_clock_value` / `test_clock()` | test_sync_protocol.cpp only | Module-specific test fixture |
| `make_ns_hash` | test_access_control.cpp only | Single use |
| `make_test_blob` (storage) | test_storage.cpp only | Creates unsigned blobs, different purpose |
| `make_test_blob` (codec) | test_codec.cpp only | Minimal wire test, different purpose |
| `compute_hash` | test_storage.cpp only | Single use |
| `print_usage` | main.cpp, relay_main.cpp, loadgen_main.cpp, verify_main.cpp | Each is binary-specific with different output -- NOT a shared function |

### Signature Unification Plan

The `to_hex()` variants have different signatures that must be unified:

| Current Signature | Unified Approach |
|-------------------|------------------|
| `(span<const uint8_t>)` | Primary overload |
| `(span<const uint8_t>, size_t max_len)` | Second overload (truncating) |
| `(span<const uint8_t, 32>)` | Covered by `span<const uint8_t>` (implicit conversion) |
| `(const array<uint8_t, 32>&)` | Covered by `span<const uint8_t>` (implicit conversion) |
| `(const uint8_t*, size_t)` | Add overload or let callers use `span{ptr, len}` |
| Template `(const array<uint8_t, N>&)` | Covered by `span<const uint8_t>` (implicit conversion) |

`std::span<const uint8_t>` implicitly constructs from `std::array<uint8_t, N>` and `std::span<const uint8_t, N>`, so a single `span<const uint8_t>` overload handles most cases. The pointer+size variant in verify_main.cpp can be replaced with `to_hex(std::span<const uint8_t>{ptr, len})` at the call site, or a convenience overload can be added.

For `from_hex`, two overloads are needed: fixed-size `array<uint8_t, 32>` return and variable-size `vector<uint8_t>` return.

## Common Pitfalls

### Pitfall 1: ODR Violations from Non-Inline Functions in Headers
**What goes wrong:** Defining functions in a header without `inline` causes multiple-definition linker errors when the header is included in multiple translation units.
**Why it happens:** All test .cpp files compile into one `chromatindb_tests` binary. Two files including a header with a non-inline function = ODR violation.
**How to avoid:** Every function in the shared headers must be `inline` or a template (templates are implicitly inline).
**Warning signs:** Linker errors like "multiple definition of `to_hex`".

### Pitfall 2: test_daemon.cpp make_signed_blob Has Different Default Timestamp
**What goes wrong:** test_daemon.cpp's `make_signed_blob` defaults timestamp to 1000, while the other 3 copies default to `TS_AUTO` (current time). Blindly replacing with the shared version changes test behavior.
**Why it happens:** test_daemon.cpp was written earlier with a simpler API.
**How to avoid:** Use the `TS_AUTO` version as the shared implementation. Update test_daemon.cpp call sites to pass explicit timestamps where the old default of 1000 was relied upon. Check each call: `make_signed_blob(id, payload, ttl, now)` -- the `now` arg is already explicit in most test_daemon.cpp calls.
**Warning signs:** Tests passing timestamps explicitly won't be affected; only calls using the default parameter need review.

### Pitfall 3: test_sync_protocol.cpp Uses Renamed Versions
**What goes wrong:** test_sync_protocol.cpp has `make_signed_delegation_sync` and `make_delegate_blob_sync` (with `_sync` suffix) instead of the engine names. These are in a second anonymous namespace block (line 631) reopened mid-file.
**Why it happens:** Written independently, same logic, different names to avoid collisions.
**How to avoid:** Replace with shared versions. The `_sync` suffix calls become plain `chromatindb::test::make_signed_delegation()` calls.
**Warning signs:** If the shared header is included but old `_sync` functions are not removed, both versions will exist (no error, just dead code).

### Pitfall 4: TempDir Prefix String Variation
**What goes wrong:** Each TempDir uses a different prefix (e.g., `chromatindb_test_peer_`, `chromatindb_test_sync_`). A shared TempDir with a fixed prefix could theoretically cause temp directory name collisions.
**Why it happens:** Each test file chose its own prefix for debugging clarity.
**How to avoid:** Use a parameterized prefix with a sensible default. Since the random suffix is a uint64_t from `random_device`, collision probability is negligible regardless of prefix. Callers that care about prefix for debugging can pass one; most won't need to.
**Warning signs:** None -- this is cosmetic, not correctness-affecting.

### Pitfall 5: test_relay_identity.cpp TempDir Creates Directory Immediately
**What goes wrong:** Most TempDir implementations do NOT call `fs::create_directories(path)` in the constructor (leaving directory creation to the code under test), but test_relay_identity.cpp's TempDir does. Replacing it with the shared version (which does not create the dir) could break those tests.
**Why it happens:** RelayIdentity tests need the directory to exist before writing key files.
**How to avoid:** The shared TempDir should NOT create the directory (matching the majority pattern -- storage, engine, etc. create their own dirs). test_relay_identity.cpp tests should add an explicit `fs::create_directories(tmp.path)` call after constructing TempDir, or the shared TempDir should accept an `auto_create` parameter.
**Warning signs:** test_relay_identity tests failing with "directory not found" errors.

## Code Examples

### Verified: span<const uint8_t> Accepts array and Fixed-Extent Span

C++20 standard guarantees implicit conversions:

```cpp
// All of these work with to_hex(std::span<const uint8_t> bytes):
std::array<uint8_t, 32> arr;
to_hex(arr);                                          // array -> span
to_hex(std::span<const uint8_t, 32>(arr));           // fixed-extent span -> dynamic span
to_hex(std::span<const uint8_t>(ptr, len));          // pointer+size -> span
```

This means the single `span<const uint8_t>` overload covers the `span<const uint8_t, 32>` signature used in test_access_control.cpp and the `array<uint8_t, 32>` signature used in loadgen.

### Verified: Existing Include Paths

```cmake
# db/CMakeLists.txt
target_include_directories(chromatindb_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

# relay/CMakeLists.txt
target_link_libraries(chromatindb_relay_lib PUBLIC chromatindb_lib)
```

This means:
- `db/main.cpp` can `#include "db/util/hex.h"` -- yes, include root is project root
- `relay/relay_main.cpp` can `#include "db/util/hex.h"` -- yes, links chromatindb_lib which exports the include path
- `loadgen/loadgen_main.cpp` can `#include "db/util/hex.h"` -- yes, links chromatindb_lib
- `tools/verify_main.cpp` can `#include "db/util/hex.h"` -- yes, links chromatindb_lib
- All test files can `#include "db/tests/test_helpers.h"` -- yes, same include root

No CMake changes needed.

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (lines 203-250) |
| Quick run command | `cd build && ctest --test-dir . -R "test_name" --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir . --output-on-failure` |

### Phase Requirements -> Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DEDUP-01 | Production to_hex replaced by shared header | build + existing tests | `cmake --build build && cd build && ctest --output-on-failure` | Existing tests cover all to_hex usage |
| DEDUP-02 | Test to_hex/ns_to_hex replaced by shared header | build + existing tests | `cmake --build build && cd build && ctest --output-on-failure` | Existing tests cover all usage |
| DEDUP-03 | No other duplicates remain across db/ and relay/ | manual audit | grep audit during implementation | N/A -- audit task |
| DEDUP-04 | All existing tests pass | full suite | `cmake --build build && cd build && ctest --output-on-failure` | All 500+ existing tests |

### Sampling Rate
- **Per task commit:** `cmake --build build && cd build && ctest --output-on-failure`
- **Per wave merge:** Same -- full test suite
- **Phase gate:** Full suite green before verify

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. This phase removes code duplication; it does not add new behavior that needs new tests.

## Open Questions

1. **Should loadgen and tools binaries also use the shared headers?**
   - What we know: loadgen has `to_hex`, two `from_hex` variants, and `make_signed_blob`. tools/verify has `to_hex`, `from_hex`. Both link chromatindb_lib.
   - What's unclear: The roadmap success criteria say "across db/ and relay/ source trees" -- loadgen/ and tools/ are separate trees.
   - Recommendation: Include them. They link chromatindb_lib and can use the shared header with zero CMake changes. Leaving duplicates in loadgen/tools defeats the purpose.

2. **Should `from_hex` also go in the shared header?**
   - What we know: 2 files have `from_hex` (loadgen, verify). Not called out in roadmap success criteria.
   - Recommendation: Yes, include it in `db/util/hex.h`. Two copies of decode logic is a maintenance hazard, and they're in the same conceptual domain as `to_hex`.

## Sources

### Primary (HIGH confidence)
- Direct codebase grep and file reading -- all findings verified against actual source files
- CMakeLists.txt include paths verified

### Secondary (MEDIUM confidence)
- C++20 span implicit conversion rules -- verified against standard specification (span constructors from array and fixed-extent span are guaranteed)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, pure refactoring
- Architecture: HIGH -- follows existing project conventions, include paths verified
- Pitfalls: HIGH -- all identified from actual code differences found in the audit
- Duplication inventory: HIGH -- comprehensive grep audit of entire codebase

**Research date:** 2026-03-23
**Valid until:** Indefinite -- this is a snapshot of current code state, valid until code changes
