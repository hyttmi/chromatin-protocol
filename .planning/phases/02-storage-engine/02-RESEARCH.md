# Phase 2: Storage Engine - Research

**Researched:** 2026-03-03
**Domain:** libmdbx storage wrapper with blob persistence, sequence indexing, TTL expiry
**Confidence:** HIGH

## Summary

Phase 2 wraps libmdbx in a `chromatin::storage::Storage` class that manages three sub-databases (blobs, sequence, expiry) and provides a BlobData-aware API. The storage layer takes `wire::BlobData` as input, encodes it to FlatBuffers for persistence, and manages content-addressed deduplication, per-namespace sequence numbering, and TTL-based expiry scanning.

libmdbx v0.13.11 (released 2026-01-30) provides both a C API (`mdbx.h`) and a C++ API (`mdbx.h++`) with RAII-managed environment, transaction, and cursor classes. The C++ API is the right choice for this project given the C++20 codebase and RAII patterns established in Phase 1. Key design: single-writer/multi-reader MVCC, zero-copy reads via mmap, and crash-safe ACID guarantees without WAL.

**Primary recommendation:** Use libmdbx's C++ API (`mdbx.h++`) with `env_managed`, `txn_managed`, and `cursor_managed` classes. Keep write transactions short (no crypto inside them). Expose `run_expiry_scan()` as a synchronous method -- no background thread until Phase 4.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Only 3 sub-databases: blobs, sequence, expiry. Peers sub-database deferred to Phase 5
- Eager initialization: create all 3 sub-databases in a single write transaction at startup
- Auto-grow map size: start with a reasonable default, let libmdbx geometry auto-grow as needed
- Storage engine owns data directory lifecycle: creates data_dir if missing, validates write permissions on open
- Expose `run_expiry_scan()` as a public method call -- no background thread, no timer. Tests call it directly
- Purge all expired blobs in one go per scan call (no batch limit)
- Clean deletion from all 3 indexes (blobs, sequence, expiry) in a single write transaction. Seq_nums will have gaps after expiry
- Injectable clock function (defaults to system time). Tests inject fake clock for deterministic expiry testing
- BlobData-aware API: takes/returns `wire::BlobData` directly. Storage internally encodes to FlatBuffers for persistence
- Single-blob writes only: `store_blob()` handles one blob per call. Batch writes added later
- Minimal query set: `get_blob(ns, hash)`, `get_blobs_by_seq(ns, since_seq)`, `has_blob(ns, hash)`
- Result types: `std::optional` for lookups, enum result codes (Stored, Duplicate, Error) for writes. No exceptions in the hot path
- Full FlatBuffer-encoded blob as value in blobs sub-database
- Sequence index value: blob hash only (32 bytes)
- Big-endian uint64 for seq_num and expiry_timestamp keys (lexicographic == numeric order)
- Derive seq_num from DB on each write: cursor to last entry for namespace, read, increment

### Claude's Discretion
- Exact libmdbx environment flags and configuration (NOSUBDIR, WRITEMAP, etc.)
- Initial map size and growth increment values
- Internal key construction helpers and byte serialization utilities
- Error message wording and spdlog integration points
- Test fixture design and helper utilities

### Deferred Ideas (OUT OF SCOPE)
- None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STOR-01 | Node stores signed blobs in libmdbx keyed by namespace + SHA3-256 hash | blobs sub-database with 64-byte composite key [namespace:32][hash:32], FlatBuffer-encoded value |
| STOR-02 | Node deduplicates blobs by content-addressed SHA3-256 hash | Check `has_blob()` before writing; duplicate returns `StoreResult::Duplicate` |
| STOR-03 | Node maintains per-namespace monotonic sequence index (seq_num -> blob hash) | sequence sub-database with 40-byte key [namespace:32][seq_be:8], value is hash (32 bytes) |
| STOR-04 | Node maintains expiry index sorted by expiry timestamp | expiry sub-database with 40-byte key [expiry_ts_be:8][hash:32], value is namespace (32 bytes) |
| STOR-05 | Node automatically prunes expired blobs via background scan | `run_expiry_scan()` with injectable clock, cursor scan from beginning of expiry index |
| STOR-06 | Blobs have configurable TTL (default 7 days, TTL=0 = permanent) | TTL=0 blobs skip expiry index insertion; expiry_time = timestamp + ttl for non-zero |
| DAEM-04 | Node recovers cleanly from crashes (libmdbx ACID guarantees) | libmdbx provides crash-safe ACID via copy-on-write B+ tree; test with kill -9 + reopen |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libmdbx | v0.13.11 | Persistent key-value storage with ACID guarantees | Single-writer/multi-reader MVCC, zero-copy reads, crash-safe without WAL, better than LMDB |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| chromatin::wire (existing) | - | FlatBuffer encode/decode for blob persistence | Every blob store/retrieve -- values stored as encoded FlatBuffers |
| chromatin::crypto (existing) | - | SHA3-256 for blob hashing | blob_hash() used for content-addressed keys |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| libmdbx | RocksDB | Concurrent writers but WAL-based, more complex, larger dependency |
| libmdbx | SQLite | SQL flexibility but no zero-copy, higher overhead for blob storage |
| libmdbx C++ API | libmdbx C API | C API is more documented but lacks RAII; C++ API matches project patterns |

### CMake Integration

libmdbx distributes as amalgamated source (since late 2025). FetchContent with the git tag works:

```cmake
set(MDBX_BUILD_CXX ON CACHE BOOL "" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MDBX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(MDBX_INSTALL_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libmdbx
  GIT_REPOSITORY https://github.com/erthink/libmdbx.git
  GIT_TAG        v0.13.11
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libmdbx)

# Link against: mdbx-static
target_link_libraries(chromatindb_lib PUBLIC mdbx-static)
```

**Key:** `MDBX_BUILD_CXX=ON` is required to build the C++ API (`mdbx.h++`). The static library target is `mdbx-static`.

## Architecture Patterns

### Recommended Module Structure
```
src/storage/
  storage.h         # Storage class declaration, StoreResult enum, clock function type
  storage.cpp       # libmdbx wrapper implementation
tests/storage/
  test_storage.cpp  # All storage tests (store, retrieve, dedup, seq, expiry, crash)
```

### Pattern 1: RAII Environment and Transaction Management

**What:** Use `mdbx::env_managed` for automatic environment cleanup, `mdbx::txn_managed` for automatic transaction abort on scope exit (unless explicitly committed), and `mdbx::cursor_managed` for automatic cursor cleanup.

**When to use:** Always. Every database access.

**Example:**
```cpp
#include <mdbx.h++>

// Environment setup (once at construction)
mdbx::env_managed env;
env = mdbx::env_managed::create_or_open(
    path.c_str(),
    mdbx::env_managed::create_parameters{},
    mdbx::env::operate_parameters{
        /* max_maps */ 3,
        /* max_readers */ 64,
        mdbx::env::mode::write_mapped_io,
        mdbx::env::durability::robust_synchronous
    });

// Write transaction with automatic abort on exception
{
    auto txn = env.start_write();
    txn.put(blobs_map_, key, value, mdbx::put_mode::upsert);
    txn.commit();  // explicit commit required
}  // if commit() not called, destructor aborts

// Read transaction
{
    auto txn = env.start_read();
    auto result = txn.get(blobs_map_, key);
    // result is a mdbx::slice pointing to mmap'd memory -- valid until txn ends
}  // destructor closes read txn, releases MVCC snapshot
```

### Pattern 2: Composite Key Construction

**What:** Build fixed-size byte-array keys for libmdbx by concatenating components. Use big-endian encoding for numeric parts so lexicographic byte order equals numeric order.

**When to use:** Every key construction for blobs (64 bytes), sequence (40 bytes), and expiry (40 bytes).

**Example:**
```cpp
// Blob key: [namespace:32][hash:32] = 64 bytes
inline std::array<uint8_t, 64> make_blob_key(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> hash) {
    std::array<uint8_t, 64> key;
    std::copy(ns.begin(), ns.end(), key.begin());
    std::copy(hash.begin(), hash.end(), key.begin() + 32);
    return key;
}

// Sequence key: [namespace:32][seq_be:8] = 40 bytes
inline std::array<uint8_t, 40> make_seq_key(
    std::span<const uint8_t, 32> ns,
    uint64_t seq_num) {
    std::array<uint8_t, 40> key;
    std::copy(ns.begin(), ns.end(), key.begin());
    // Big-endian for lexicographic == numeric ordering
    for (int i = 7; i >= 0; --i) {
        key[32 + (7 - i)] = static_cast<uint8_t>(seq_num >> (i * 8));
    }
    return key;
}

// Expiry key: [expiry_ts_be:8][hash:32] = 40 bytes
inline std::array<uint8_t, 40> make_expiry_key(
    uint64_t expiry_ts,
    std::span<const uint8_t, 32> hash) {
    std::array<uint8_t, 40> key;
    for (int i = 7; i >= 0; --i) {
        key[7 - i] = static_cast<uint8_t>(expiry_ts >> (i * 8));
    }
    std::copy(hash.begin(), hash.end(), key.begin() + 8);
    return key;
}
```

### Pattern 3: Injectable Clock for Testable Expiry

**What:** Use a `std::function<uint64_t()>` for the current time, defaulting to system clock. Tests inject a fake clock to control time deterministically.

**When to use:** All timestamp comparisons (expiry scanning, TTL computation).

**Example:**
```cpp
using Clock = std::function<uint64_t()>;

inline uint64_t system_clock_seconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
}

class Storage {
public:
    explicit Storage(const std::string& path, Clock clock = system_clock_seconds);

    // In tests:
    // uint64_t fake_time = 1000;
    // Storage store(path, [&]() { return fake_time; });
    // fake_time += 86400 * 8;  // advance 8 days
    // store.run_expiry_scan();  // blobs with 7-day TTL are now expired
};
```

### Pattern 4: Cursor-Based Sequence Number Derivation

**What:** Derive the next seq_num for a namespace by seeking to the last entry for that namespace in the sequence sub-database, reading the current max, and incrementing.

**When to use:** Every `store_blob()` call.

**Example:**
```cpp
uint64_t Storage::next_seq_num(mdbx::txn_managed& txn,
                                std::span<const uint8_t, 32> ns) {
    auto cursor = txn.open_cursor(seq_map_);

    // Build upper bound key: namespace + max seq_num (0xFFFFFFFFFFFFFFFF)
    auto upper = make_seq_key(ns, UINT64_MAX);
    auto upper_slice = mdbx::slice(upper.data(), upper.size());

    // Seek to the position at or before upper bound
    // If found, extract seq_num from last entry for this namespace
    if (cursor.lower_bound(upper_slice)) {
        auto key_slice = cursor.current().key;
        // Check if this entry belongs to our namespace
        if (key_slice.length() == 40 &&
            std::memcmp(key_slice.data(), ns.data(), 32) == 0) {
            // Extract big-endian seq_num from bytes [32..39]
            uint64_t current = decode_be_u64(
                static_cast<const uint8_t*>(key_slice.data()) + 32);
            return current + 1;
        }
    }
    // Use previous entry if lower_bound went past our namespace
    // ... handle edge cases with cursor.to_previous()

    return 1;  // First blob in this namespace
}
```

### Anti-Patterns to Avoid
- **Holding read transactions across async boundaries:** Pins old MVCC pages, causes database file growth. Open, read, close in tight scope.
- **Doing crypto inside write transactions:** Signature verification takes ~0.3ms. Keep write transactions to pure database operations.
- **Opening DBI handles per-transaction:** Open all 3 map handles once at startup, reuse for lifetime of environment.
- **Storing seq_num in memory counter:** Crash loses the counter. Always derive from database.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Key-value storage with ACID | Custom file format | libmdbx | Crash safety, MVCC, zero-copy reads are extremely hard to get right |
| Blob serialization | Custom binary format | FlatBuffers (existing wire::encode/decode) | Already built in Phase 1, deterministic encoding |
| Content-addressed hashing | Custom hash scheme | wire::blob_hash() (existing) | SHA3-256 over full blob, already implemented |
| Big-endian byte encoding | Hand-rolled byte shifts | htobe64/be64toh or constexpr helper | Platform endianness handling is error-prone |

## Common Pitfalls

### Pitfall 1: Map Size Exhaustion (MDBX_MAP_FULL)
**What goes wrong:** Default libmdbx geometry has conservative upper bounds. Database grows beyond initial allocation and writes fail.
**Why it happens:** libmdbx maps a virtual address range up-front; actual disk usage grows dynamically but cannot exceed the geometry upper bound.
**How to avoid:** Set geometry with large upper bound (e.g., 64GB). File does not consume disk until data is written. Use `env.set_geometry()` with appropriate size_lower, size_now, size_upper, growth_step, shrink_threshold, page_size.
**Warning signs:** `mdbx::error` exceptions with MDBX_MAP_FULL error code during writes.

### Pitfall 2: Long-Lived Read Transactions Causing File Growth
**What goes wrong:** A read transaction pins an MVCC snapshot. While it is open, libmdbx cannot reclaim pages freed by subsequent write transactions. The database file grows without bound.
**Why it happens:** Zero-copy reads mean the slice points directly into mmap'd memory. If the read transaction stays open, that memory page cannot be reused.
**How to avoid:** Scope read transactions tightly. Copy data out if needed beyond transaction lifetime. Never hold a read txn while waiting for IO or user input.
**Warning signs:** Database file size growing much larger than actual data size.

### Pitfall 3: MDBX_NOTFOUND vs Exception
**What goes wrong:** libmdbx C++ API throws exceptions on unexpected errors but returns empty/false for "not found" in some methods. Mixing these up causes either silent failures or unexpected exception propagation.
**Why it happens:** The C++ API wraps the C API's error codes. MDBX_NOTFOUND is "expected" for lookups but "unexpected" for cursor operations in some contexts.
**How to avoid:** Use `txn.get()` which returns an optional-like result. Check `.done` or use the bool conversion. Wrap cursor operations in try-catch for robust error handling.
**Warning signs:** Crashes on missing keys, silent data loss on failed lookups.

### Pitfall 4: Sub-database Handle Leaking
**What goes wrong:** Opening a DBI handle (sub-database) inside a normal data transaction instead of at startup.
**Why it happens:** DBI handles in libmdbx must be opened in a write transaction and then reused. Opening them repeatedly wastes internal DBI slots and can cause subtle locking issues.
**How to avoid:** Open all 3 map handles (blobs, sequence, expiry) once in the constructor's initialization write transaction. Store as member variables. Reuse for all subsequent operations.
**Warning signs:** Running out of DBI slots, performance degradation after many transactions.

### Pitfall 5: Expiry Scan Deleting Sequence Index Entries
**What goes wrong:** Expiry scan removes blobs and expiry entries but forgets to clean up the sequence index. The sequence index accumulates orphaned entries pointing to deleted blobs.
**Why it happens:** The expiry index stores `[expiry_ts][hash] -> namespace` but not the seq_num. To clean the sequence index, you need to find the seq_num for a given namespace+hash, which requires scanning.
**How to avoid:** Two approaches: (1) Accept orphaned seq entries and handle "blob not found" gracefully in `get_blobs_by_seq()` (skip missing blobs), or (2) store seq_num in the expiry value alongside namespace. Approach 1 is simpler and recommended per CONTEXT.md ("seq_nums will have gaps after expiry").
**Warning signs:** `get_blobs_by_seq()` returning fewer results than expected.

## Code Examples

### Complete store_blob Flow
```cpp
StoreResult Storage::store_blob(const wire::BlobData& blob) {
    // 1. Encode to FlatBuffers for storage
    auto encoded = wire::encode_blob(blob);

    // 2. Compute content-addressed hash
    auto hash = wire::blob_hash(encoded);

    // 3. Build blob key
    auto blob_key = make_blob_key(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash));

    // 4. Write transaction
    auto txn = env_.start_write();

    // 5. Check dedup
    auto existing = txn.get(blobs_map_,
        mdbx::slice(blob_key.data(), blob_key.size()));
    if (existing) {
        txn.abort();
        return StoreResult::Duplicate;
    }

    // 6. Store blob
    txn.put(blobs_map_,
        mdbx::slice(blob_key.data(), blob_key.size()),
        mdbx::slice(encoded.data(), encoded.size()));

    // 7. Assign and store sequence number
    uint64_t seq = next_seq_num(txn, std::span<const uint8_t, 32>(blob.namespace_id));
    auto seq_key = make_seq_key(std::span<const uint8_t, 32>(blob.namespace_id), seq);
    txn.put(seq_map_,
        mdbx::slice(seq_key.data(), seq_key.size()),
        mdbx::slice(hash.data(), hash.size()));

    // 8. Store expiry entry (skip for TTL=0 permanent blobs)
    if (blob.ttl > 0) {
        uint64_t expiry_time = blob.timestamp + blob.ttl;
        auto exp_key = make_expiry_key(expiry_time, std::span<const uint8_t, 32>(hash));
        txn.put(expiry_map_,
            mdbx::slice(exp_key.data(), exp_key.size()),
            mdbx::slice(blob.namespace_id.data(), blob.namespace_id.size()));
    }

    // 9. Commit
    txn.commit();
    return StoreResult::Stored;
}
```

### Expiry Scan
```cpp
size_t Storage::run_expiry_scan() {
    uint64_t now = clock_();
    size_t purged = 0;

    auto txn = env_.start_write();
    auto cursor = txn.open_cursor(expiry_map_);

    // Scan from beginning -- entries sorted by expiry_ts (big-endian)
    bool has_entry = cursor.to_first(false);
    while (has_entry) {
        auto key_slice = cursor.current().key;
        uint64_t expiry_ts = decode_be_u64(
            static_cast<const uint8_t*>(key_slice.data()));

        if (expiry_ts > now) break;  // All remaining entries are in the future

        // Extract hash and namespace from this expiry entry
        auto hash_ptr = static_cast<const uint8_t*>(key_slice.data()) + 8;
        auto ns_slice = cursor.current().value;
        auto ns_ptr = static_cast<const uint8_t*>(ns_slice.data());

        // Delete from blobs index
        auto blob_key = make_blob_key(
            std::span<const uint8_t, 32>(ns_ptr, 32),
            std::span<const uint8_t, 32>(hash_ptr, 32));
        txn.del(blobs_map_, mdbx::slice(blob_key.data(), blob_key.size()));

        // Delete expiry entry via cursor
        cursor.erase();
        purged++;

        has_entry = cursor.to_next(false);
    }

    txn.commit();
    return purged;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| LMDB (original) | libmdbx (fork with improvements) | 2015+ | Better crash safety, auto-grow geometry, improved API |
| libmdbx git repo with full source | Amalgamated source distribution | Dec 2025 | FetchContent from git still works; amalgamated tarball also available |
| libmdbx C API only | C++ API via mdbx.h++ | Stable in 0.13.x | RAII wrappers, managed classes, exception-based errors |

## Open Questions

1. **libmdbx C++ API `txn.get()` return type**
   - What we know: Returns some optional-like result type for key lookups
   - What's unclear: Exact API for checking "not found" vs extracting slice value. The C++ API documentation is sparse.
   - Recommendation: Check the actual `mdbx.h++` header after FetchContent downloads it. The C API fallback (`mdbx_get()` returning `MDBX_NOTFOUND`) is well-documented.

2. **libmdbx cursor lower_bound behavior at namespace boundaries**
   - What we know: `cursor.lower_bound(key)` seeks to first key >= given key
   - What's unclear: Exact behavior when seeking past the last entry for a namespace in a composite key. Need to verify whether `to_previous()` is needed.
   - Recommendation: Write focused unit tests for seq_num derivation with edge cases (empty namespace, single entry, max seq_num).

3. **libmdbx WRITEMAP flag safety on Linux**
   - What we know: WRITEMAP provides better write performance by mapping pages read-write. Risk: process crash during write can leave partially-written page in the file.
   - What's unclear: Whether libmdbx's crash recovery handles this case on all Linux filesystems.
   - Recommendation: Start with `mdbx::env::mode::write_mapped_io` (WRITEMAP) for performance. Test crash recovery explicitly (kill -9 during writes, reopen, verify data integrity). Fall back to non-WRITEMAP if issues found.

## Sources

### Primary (HIGH confidence)
- [libmdbx GitHub repository](https://github.com/erthink/libmdbx) - CMake targets, build options, release versions
- [libmdbx C++ API documentation](https://libmdbx.dqdkfa.ru/group__cxx__api.html) - Class hierarchy, method signatures
- [libmdbx CMakeLists.txt](https://github.com/erthink/libmdbx/blob/master/CMakeLists.txt) - Target names (mdbx-static), build options (MDBX_BUILD_CXX)
- [libmdbx releases](https://github.com/erthink/libmdbx/releases/) - v0.13.11 latest stable (2026-01-30)

### Secondary (MEDIUM confidence)
- [libmdbx usage documentation](https://libmdbx.dqdkfa.ru/usage.html) - Integration patterns, build guidance
- Architecture research (.planning/research/ARCHITECTURE.md) - Storage schema design, transaction discipline patterns
- Pitfalls research (.planning/research/PITFALLS.md) - libmdbx-specific gotchas

### Tertiary (LOW confidence)
- libmdbx C++ API exact method signatures for `txn.get()` return type and cursor seek behavior -- needs verification against actual header after build

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - libmdbx is well-established, v0.13.11 confirmed stable
- Architecture: HIGH - storage schema and patterns validated in project architecture research
- Pitfalls: HIGH - libmdbx pitfalls well-documented in LMDB/libmdbx community

**Research date:** 2026-03-03
**Valid until:** 2026-04-03 (30 days -- stable domain)

---
*Phase: 02-storage-engine*
*Research completed: 2026-03-03*
