# Phase 35: Namespace Quotas - Research

**Researched:** 2026-03-18
**Domain:** Per-namespace resource limits in libmdbx-backed blob storage
**Confidence:** HIGH

## Summary

Namespace quotas add per-namespace byte and blob count limits, enforced atomically at ingest. The implementation touches six components: Config (parsing + validation), Storage (7th sub-database for materialized aggregates), BlobEngine (quota check at Step 2a), PeerManager (QuotaExceeded wire message + SIGHUP reload), SyncProtocol (quota_exceeded tracking), and transport.fbs (new message type).

Every integration point follows established patterns already proven in the codebase. The quota sub-database uses the same flat composite key pattern as the other six sub-databases. The quota check in BlobEngine follows the Step 0b capacity check pattern. The QuotaExceeded wire message follows the StorageFull pattern exactly. The SIGHUP reload follows the rate_limit_/cursor config mutable member pattern. The startup rebuild follows the DARE unencrypted-data scan pattern.

**Primary recommendation:** Implement as three plans -- (1) Storage layer: quota sub-database, aggregates, startup rebuild; (2) Engine + Config + Wire: quota check, config parsing, wire message type; (3) PeerManager + SyncProtocol: wire handling, SIGHUP reload, metrics.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Global defaults + per-namespace override map in config.json
- `namespace_quota_bytes` and `namespace_quota_count` as top-level fields (global defaults)
- `namespace_quotas` map keyed by full 64-char hex namespace hash, values are `{ "max_bytes": N, "max_count": N }`
- 0 = unlimited (disabled), consistent with existing `max_storage_bytes` pattern
- Partial overrides allowed: override only what you specify, inherit the rest from global defaults
- Per-namespace override with 0 explicitly exempts that namespace from the global default
- Namespace keys validated as 64-char hex, consistent with `allowed_keys` and `sync_namespaces`
- New `TransportMsgType_QuotaExceeded = 24` wire message type (distinct from StorageFull)
  - NOTE: TransportMsgType 24 is already used by TrustedHello and 25 by PQRequired. QuotaExceeded must use 26.
- New `IngestError::quota_exceeded` enum variant (distinct from `storage_full`)
- PeerManager pattern-matches on error variant to send the correct wire message
- Quota check at Step 2a: after namespace ownership/delegation validation (Step 2), before dedup check (Step 2.5)
- Early rejection avoids wasting dedup lookup and ML-DSA-87 crypto on over-quota blobs
- Final enforcement in the mdbx write transaction inside store_blob (prevents check-then-act race across co_await)
- Byte quota: encoded FlatBuffer size (`wire::encode_blob()` output size)
- Count quota: simple blob count (1 per blob stored)
- Tombstones exempt from quota (consistent with Phase 16 global capacity exemption)
- Delegation blobs count toward quota (regular blobs with special data format)
- Over-quota on SIGHUP: reject new writes only, existing data stays intact
- Immediate in-transaction reclaim on delete/expiry
- Full aggregate rebuild on startup

### Claude's Discretion
- Quota sub-database key/value encoding details (likely `[namespace:32] -> [bytes_be:8][count_be:8]`)
- How BlobEngine receives quota config (constructor param, reload method, or Config reference)
- Sync-received blob quota enforcement (consistent with Data message path)
- Metrics counters for quota state (quota_checks, quota_rejections per namespace or global)
- Log levels and format for quota events
- Exact startup rebuild implementation (cursor scan pattern, progress logging)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| QUOTA-01 | Per-namespace maximum byte limit configurable and enforced at ingest | Config struct fields + BlobEngine Step 2a check + Storage write-txn enforcement |
| QUOTA-02 | Per-namespace maximum blob count limit configurable and enforced at ingest | Same pipeline as QUOTA-01, count tracked alongside bytes in same aggregate |
| QUOTA-03 | Namespace usage tracked via materialized aggregate in libmdbx sub-database (O(1) lookup on write path) | 7th sub-database `quota_map` with `[ns:32] -> [bytes_be:8][count_be:8]`, increment/decrement in write txn |
| QUOTA-04 | Quota exceeded rejection signaled to writing peer with clear error | `IngestError::quota_exceeded` + `TransportMsgType_QuotaExceeded = 26` wire message |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libmdbx | latest (FetchContent) | Quota aggregate sub-database | Already manages 6 sub-dbs; 7th follows same pattern |
| nlohmann/json | latest (FetchContent) | Quota config parsing | Existing config parser uses it |
| FlatBuffers | latest (FetchContent) | Wire message type enum | transport.fbs already defines all message types |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| spdlog | latest (FetchContent) | Quota event logging | All quota check/reject/reload events |
| Catch2 | v3.7.1 (FetchContent) | Quota tests | All unit tests for quota behavior |

No new dependencies required. All libraries already in the build.

## Architecture Patterns

### Recommended Changes by Component

```
db/
  config/config.h           # + namespace_quota_bytes, namespace_quota_count, namespace_quotas map
  config/config.cpp          # + JSON parsing for quota fields, validation
  engine/engine.h            # + IngestError::quota_exceeded, BlobEngine quota params
  engine/engine.cpp          # + Step 2a quota check in ingest()
  storage/storage.h          # + NamespaceQuota struct, get/set/increment/decrement/rebuild API
  storage/storage.cpp        # + quota_map sub-database, aggregate operations
  peer/peer_manager.h        # + quota config mutable members
  peer/peer_manager.cpp      # + QuotaExceeded message handling, SIGHUP reload
  sync/sync_protocol.h       # + SyncStats::quota_exceeded_count
  sync/sync_protocol.cpp     # + quota_exceeded tracking in ingest_blobs
  schemas/transport.fbs      # + QuotaExceeded = 26 message type
  tests/config/test_config.cpp   # + quota config parsing tests
  tests/engine/test_engine.cpp   # + quota enforcement tests
  tests/storage/test_storage.cpp # + quota aggregate tests
```

### Pattern 1: Quota Sub-Database (QUOTA-03)

**What:** 7th libmdbx sub-database storing materialized namespace usage aggregates.
**When to use:** Every blob store, delete, and expiry operation.
**Key encoding:** `[namespace:32]` (32 bytes, same as namespace_id)
**Value encoding:** `[total_bytes_be:8][blob_count_be:8]` (16 bytes, big-endian)

```cpp
// Follows existing sub-database patterns in Storage::Impl
mdbx::map_handle quota_map{0};

// In constructor: open alongside other maps
quota_map = txn.create_map("quota");

// Key/value helpers
static constexpr size_t QUOTA_VALUE_SIZE = 16;

struct NamespaceQuota {
    uint64_t total_bytes = 0;
    uint64_t blob_count = 0;
};

static std::array<uint8_t, QUOTA_VALUE_SIZE> encode_quota_value(const NamespaceQuota& q) {
    std::array<uint8_t, QUOTA_VALUE_SIZE> buf;
    encode_be_u64(q.total_bytes, buf.data());
    encode_be_u64(q.blob_count, buf.data() + 8);
    return buf;
}

static NamespaceQuota decode_quota_value(const uint8_t* data) {
    NamespaceQuota q;
    q.total_bytes = decode_be_u64(data);
    q.blob_count = decode_be_u64(data + 8);
    return q;
}
```

### Pattern 2: In-Transaction Quota Enforcement (QUOTA-01, QUOTA-02)

**What:** Atomic quota check + update inside the mdbx write transaction in store_blob.
**When to use:** Every non-tombstone store_blob call.
**Why:** Prevents check-then-act race across co_await points.

```cpp
// Inside Storage::store_blob, after dedup check, before encryption:
// 1. Read current aggregate from quota_map
// 2. Check against limits (passed as params)
// 3. If over: abort txn, return new QuotaExceeded status
// 4. If under: increment aggregate in same txn

// StoreResult::Status needs QuotaExceeded variant
// OR: BlobEngine does the check by calling Storage::get_namespace_quota()
// and then store_blob does the increment atomically

// Recommended: Two-phase approach
// - BlobEngine::ingest() Step 2a: early check via Storage::get_namespace_quota() (read txn, O(1))
// - Storage::store_blob(): final enforcement + increment in write txn (atomic)
```

### Pattern 3: SIGHUP-Reloadable Quota Config

**What:** Mutable quota config members in PeerManager, reloaded on SIGHUP.
**When to use:** Config reload flow.
**Follows:** Exact same pattern as `rate_limit_bytes_per_sec_`, `full_resync_interval_`, `cursor_stale_seconds_`.

```cpp
// PeerManager private members:
uint64_t namespace_quota_bytes_ = 0;   // Global default (0 = unlimited)
uint64_t namespace_quota_count_ = 0;   // Global default (0 = unlimited)
std::map<std::array<uint8_t, 32>, NamespaceQuotaOverride> namespace_quotas_; // Per-ns overrides

// In reload_config():
namespace_quota_bytes_ = new_cfg.namespace_quota_bytes;
namespace_quota_count_ = new_cfg.namespace_quota_count;
// Parse + validate namespace_quotas map...
```

### Pattern 4: Startup Aggregate Rebuild

**What:** Full namespace scan on startup to compute accurate aggregates from actual stored blobs.
**When to use:** Every startup, before accepting connections.
**Follows:** DARE unencrypted-data validation scan pattern in Storage::Impl constructor.

```cpp
// In Storage::Impl constructor, after validate_no_unencrypted_data:
rebuild_quota_aggregates();

// Implementation: cursor scan over blobs_map
// For each entry: extract namespace (first 32 bytes of key), add encrypted value size
// Group by namespace, write final aggregates to quota_map in one write txn
```

### Anti-Patterns to Avoid
- **Scanning blobs_map on every write to compute quota:** Defeats O(1) requirement. Use materialized aggregate.
- **Checking quota outside write transaction only:** Check-then-act race across co_await. Must enforce inside write txn.
- **Storing per-blob quota entries:** Wasteful. One aggregate per namespace is sufficient.
- **Evicting data when quota shrinks via SIGHUP:** Violates "reject new writes only" decision. Just reject, never evict.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Atomic aggregate update | Separate read+write transactions | Single mdbx write transaction | ACID guarantees prevent races |
| Namespace iteration on startup | Manual cursor walking with ad-hoc key parsing | Cursor scan with namespace prefix matching | Established pattern in list_namespaces() |
| Config validation | Custom hex parsing | Existing validate_allowed_keys() | Same 64-char hex format, already tested |
| Wire message type | Custom message framing | FlatBuffers TransportMsgType enum extension | Consistent with all 25 existing message types |

## Common Pitfalls

### Pitfall 1: TransportMsgType Value Collision
**What goes wrong:** CONTEXT.md says `QuotaExceeded = 24`, but transport.fbs already has `TrustedHello = 24` and `PQRequired = 25`.
**Why it happens:** The existing transport.fbs was not checked during context gathering.
**How to avoid:** Use `QuotaExceeded = 26` (next available value).
**Warning signs:** Compile error or runtime message routing confusion.

### Pitfall 2: Check-Then-Act Race on Quota
**What goes wrong:** BlobEngine checks quota (read txn), then co_await happens, another blob arrives and stores, then original blob stores exceeding quota.
**Why it happens:** Read transactions in mdbx are snapshots; another write can commit between read and write.
**How to avoid:** Final enforcement MUST be in the store_blob write transaction. The BlobEngine Step 2a check is an optimization (early rejection), not the authoritative enforcement.
**Warning signs:** Quota exceeded by exactly 1 blob in concurrent scenarios.

### Pitfall 3: Byte Counting Mismatch
**What goes wrong:** Using different size measurements at check time vs. storage time leads to quota drift.
**Why it happens:** Multiple places compute blob size differently (raw data size vs. encoded FlatBuffer size vs. encrypted envelope size).
**How to avoid:** Consistently use the encrypted envelope size (what actually occupies mdbx), which is `encoded.size() + ENVELOPE_OVERHEAD` (29 bytes overhead). This is the true storage cost. At Step 2a early check, use `wire::encode_blob()` output size as an approximation (close enough for early rejection, final check uses actual stored size).
**Warning signs:** Aggregate total doesn't match sum of individual blob storage costs.

### Pitfall 4: Forgetting to Decrement on Delete/Expiry
**What goes wrong:** Quota aggregate only goes up, never down, even after blobs are deleted or expire.
**Why it happens:** Multiple deletion paths (delete_blob_data, run_expiry_scan) each need decrement logic.
**How to avoid:** Decrement in the same write transaction that removes the blob. Both delete_blob_data and run_expiry_scan already have write transactions -- add decrement there.
**Warning signs:** Namespace appears full after deleting all blobs.

### Pitfall 5: max_maps Limit
**What goes wrong:** Adding a 7th sub-database fails because `operate_params.max_maps` is set to 7 (which means 6 named + 1 default).
**Why it happens:** libmdbx counts the unnamed default database in max_maps.
**How to avoid:** Increase `max_maps` from 7 to 8 (7 named sub-databases + 1 default).
**Warning signs:** mdbx exception on startup when trying to create the quota map.

### Pitfall 6: Tombstone Size Counted in Quota
**What goes wrong:** Tombstones counted toward quota prevents owners from deleting their own blobs when at quota limit.
**Why it happens:** Tombstones go through store_blob like regular blobs.
**How to avoid:** Check `wire::is_tombstone(blob.data)` before quota enforcement, same as the existing capacity check pattern in BlobEngine::ingest() Step 0b.
**Warning signs:** Owner cannot delete blobs when namespace is at quota.

### Pitfall 7: Startup Rebuild Counts Tombstones
**What goes wrong:** Startup rebuild includes tombstones in byte/count aggregate.
**Why it happens:** Rebuild scans all entries in blobs_map without checking tombstone status.
**How to avoid:** During rebuild, decrypt and decode each blob to check `is_tombstone()`. Or, since tombstones are exempt, simply don't count them. However, decrypting every blob on startup is expensive. Alternative: tombstones are tiny (36 bytes data) -- counting them has negligible impact and rebuild can just count everything. Then, when checking quota, the small overhead from tombstones is acceptable.
**Recommended approach:** Count everything in rebuild (simpler, faster -- no decrypt needed), accept the tiny tombstone overhead. The quota check exempts tombstones from being rejected, not from being counted.
**Actually, better:** The rebuild must NOT decrypt. It should use the encrypted envelope size as the byte count -- this is the actual disk cost. Key format `[ns:32][hash:32]` gives us the namespace. Value size gives us the byte count. No decryption needed.

## Code Examples

### Config Struct Extension
```cpp
// In config.h - Config struct:
struct NamespaceQuotaConfig {
    uint64_t max_bytes = 0;  // 0 = use global default
    uint64_t max_count = 0;  // 0 = use global default
    bool has_max_bytes = false;  // Track if explicitly set (for 0 = exempt)
    bool has_max_count = false;
};

// Simpler: just use optional
uint64_t namespace_quota_bytes = 0;     // Global default byte limit (0 = unlimited)
uint64_t namespace_quota_count = 0;     // Global default count limit (0 = unlimited)
// Per-namespace overrides: key is 64-char hex namespace hash
std::map<std::string, std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> namespace_quotas;
```

### Config JSON Parsing
```cpp
// In config.cpp - load_config():
cfg.namespace_quota_bytes = j.value("namespace_quota_bytes", cfg.namespace_quota_bytes);
cfg.namespace_quota_count = j.value("namespace_quota_count", cfg.namespace_quota_count);

if (j.contains("namespace_quotas") && j["namespace_quotas"].is_object()) {
    for (auto& [key, val] : j["namespace_quotas"].items()) {
        // Validate key is 64-char hex
        if (key.size() != 64) {
            throw std::runtime_error("Invalid namespace_quotas key '" + key + "': expected 64 hex characters");
        }
        // Reuse existing hex validation
        validate_allowed_keys({key});
        auto& entry = cfg.namespace_quotas[key];
        if (val.contains("max_bytes")) entry.first = val["max_bytes"].get<uint64_t>();
        if (val.contains("max_count")) entry.second = val["max_count"].get<uint64_t>();
    }
}
```

### IngestError Extension
```cpp
// In engine.h:
enum class IngestError {
    namespace_mismatch,
    invalid_signature,
    malformed_blob,
    oversized_blob,
    storage_error,
    tombstoned,
    no_delegation,
    storage_full,
    quota_exceeded   // NEW: namespace quota exceeded
};
```

### BlobEngine Step 2a Check
```cpp
// In engine.cpp - BlobEngine::ingest(), after Step 2 (namespace/delegation check):
// Step 2a: Namespace quota check (after ownership, before dedup)
// Tombstones exempt: consistent with Step 0b capacity exemption
if (!wire::is_tombstone(blob.data)) {
    auto quota = storage_.get_namespace_quota(blob.namespace_id);
    auto [byte_limit, count_limit] = get_effective_limits(blob.namespace_id);
    if (byte_limit > 0 && quota.total_bytes + estimated_size > byte_limit) {
        return IngestResult::rejection(IngestError::quota_exceeded,
            "namespace byte quota exceeded");
    }
    if (count_limit > 0 && quota.blob_count + 1 > count_limit) {
        return IngestResult::rejection(IngestError::quota_exceeded,
            "namespace count quota exceeded");
    }
}
```

### Storage Aggregate Operations
```cpp
// In storage.h:
struct NamespaceQuota {
    uint64_t total_bytes = 0;
    uint64_t blob_count = 0;
};

NamespaceQuota get_namespace_quota(std::span<const uint8_t, 32> ns);
void rebuild_quota_aggregates();

// In storage.cpp - Inside store_blob write transaction:
// After dedup check, before encryption:
// Read current aggregate
auto ns_slice = mdbx::slice(blob.namespace_id.data(), 32);
auto existing_quota = txn.get(impl_->quota_map, ns_slice, not_found_sentinel);
NamespaceQuota current{};
if (existing_quota.data() != nullptr && existing_quota.length() == QUOTA_VALUE_SIZE) {
    current = decode_quota_value(static_cast<const uint8_t*>(existing_quota.data()));
}
// Increment (actual enforcement delegated to BlobEngine)
current.total_bytes += encrypted.size();  // Actual storage cost
current.blob_count += 1;
auto new_val = encode_quota_value(current);
txn.upsert(impl_->quota_map, ns_slice,
            mdbx::slice(new_val.data(), new_val.size()));
```

### QuotaExceeded Wire Message Handling
```cpp
// In peer_manager.cpp - on_peer_message, Data handler:
if (*result.error == engine::IngestError::quota_exceeded) {
    spdlog::warn("Namespace quota exceeded, notifying peer {}", peer_display_name(conn));
    asio::co_spawn(ioc_, [conn]() -> asio::awaitable<void> {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty);
    }, asio::detached);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| No per-namespace limits | Global max_storage_bytes only | v0.4.0 (Phase 16) | Single limit for all namespaces |
| Full scan for capacity | mdbx env info O(1) | v0.4.0 (Phase 16) | Pattern for cheap capacity checks |
| N/A | Materialized aggregate in sub-db | Phase 35 (this) | O(1) per-namespace quota checking |

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (BUILD_TESTING block) |
| Quick run command | `cd build && ctest --test-dir . -R "quota\|Quota" --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir . --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| QUOTA-01 | Byte limit rejects over-quota blob | unit | `cd build && ctest -R "quota.*byte" --output-on-failure` | No -- Wave 0 |
| QUOTA-01 | Byte limit allows under-quota blob | unit | `cd build && ctest -R "quota.*byte" --output-on-failure` | No -- Wave 0 |
| QUOTA-02 | Count limit rejects over-quota blob | unit | `cd build && ctest -R "quota.*count" --output-on-failure` | No -- Wave 0 |
| QUOTA-02 | Count limit allows under-quota blob | unit | `cd build && ctest -R "quota.*count" --output-on-failure` | No -- Wave 0 |
| QUOTA-03 | Aggregate stored in quota sub-db, O(1) read | unit | `cd build && ctest -R "quota.*aggregate" --output-on-failure` | No -- Wave 0 |
| QUOTA-03 | Aggregate incremented on store | unit | `cd build && ctest -R "quota.*aggregate" --output-on-failure` | No -- Wave 0 |
| QUOTA-03 | Aggregate decremented on delete | unit | `cd build && ctest -R "quota.*aggregate" --output-on-failure` | No -- Wave 0 |
| QUOTA-03 | Aggregate decremented on expiry | unit | `cd build && ctest -R "quota.*expiry" --output-on-failure` | No -- Wave 0 |
| QUOTA-03 | Startup rebuild matches actual storage | unit | `cd build && ctest -R "quota.*rebuild" --output-on-failure` | No -- Wave 0 |
| QUOTA-04 | IngestError::quota_exceeded returned | unit | `cd build && ctest -R "quota.*exceeded" --output-on-failure` | No -- Wave 0 |
| QUOTA-01/02 | Tombstones exempt from quota | unit | `cd build && ctest -R "quota.*tombstone" --output-on-failure` | No -- Wave 0 |
| QUOTA-01/02 | Per-namespace override supersedes global | unit | `cd build && ctest -R "quota.*override" --output-on-failure` | No -- Wave 0 |
| QUOTA-01/02 | 0 override explicitly exempts namespace | unit | `cd build && ctest -R "quota.*exempt" --output-on-failure` | No -- Wave 0 |
| QUOTA-04 | Config parsing for quota fields | unit | `cd build && ctest -R "config.*quota" --output-on-failure` | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure` (full suite)
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- Tests will be added inline with implementation (existing pattern -- no separate test-first files)
- Framework and config already exist -- no test infrastructure gaps
- New tests go into existing test files: `test_storage.cpp`, `test_engine.cpp`, `test_config.cpp`

## Open Questions

1. **Byte measurement: encoded vs encrypted size**
   - What we know: CONTEXT.md says "encoded FlatBuffer size (wire::encode_blob() output size)". Actual storage cost is encrypted envelope size (encoded + 29 bytes overhead).
   - What's unclear: Should the quota track the wire-level size or the actual storage cost?
   - Recommendation: Track actual storage cost (encrypted envelope size) in the aggregate, since that's what occupies disk. For the early check in BlobEngine, use the encoded size as a lower-bound estimate. The write-txn final check uses the actual encrypted size. The 29-byte difference per blob is negligible.

2. **How BlobEngine receives quota config**
   - What we know: Currently BlobEngine takes `max_storage_bytes` as constructor param. Quota config includes global defaults + per-namespace map.
   - Recommendation: Add quota parameters to BlobEngine constructor. For SIGHUP reload, add a `set_quota_config()` method (following the mutable member pattern used in PeerManager). BlobEngine already stores a reference to Storage, so it can call `get_namespace_quota()` directly.

3. **Quota aggregate accuracy with tombstone exemption**
   - What we know: Tombstones are exempt from quota rejection but are stored in blobs_map like regular blobs.
   - Recommendation: Do NOT count tombstones in the aggregate at all. During startup rebuild, tombstones can be identified without decryption by checking if the value size is approximately the known tombstone size (36 bytes data + overhead). However, this is fragile. Better: count everything during rebuild (tombstones are tiny, ~100 bytes stored per tombstone), and just don't increment the aggregate when storing a tombstone. The rebuild will include existing tombstones as a minor overcount, which is acceptable. Alternatively, store a separate tombstone flag or simply accept the overcount.

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/storage/storage.h`, `db/storage/storage.cpp` -- sub-database pattern, key encoding, write transaction pattern
- Codebase analysis: `db/engine/engine.h`, `db/engine/engine.cpp` -- IngestError enum, Step 0b capacity check pattern, ingest pipeline
- Codebase analysis: `db/config/config.h`, `db/config/config.cpp` -- Config struct, JSON parsing, validation functions
- Codebase analysis: `db/peer/peer_manager.h`, `db/peer/peer_manager.cpp` -- StorageFull wire handling, SIGHUP reload, mutable config members
- Codebase analysis: `db/sync/sync_protocol.h`, `db/sync/sync_protocol.cpp` -- SyncStats pattern, storage_full_count tracking
- Codebase analysis: `db/schemas/transport.fbs` -- TransportMsgType enum (current max: PQRequired = 25)
- Codebase analysis: `db/main.cpp` -- BlobEngine construction, component wiring

### Secondary (MEDIUM confidence)
- Phase 35 CONTEXT.md -- locked decisions from user discussion session

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all patterns established in codebase
- Architecture: HIGH -- every integration point follows an existing proven pattern
- Pitfalls: HIGH -- identified from direct codebase analysis (TransportMsgType collision is verified)

**Research date:** 2026-03-18
**Valid until:** 2026-04-18 (stable domain, no external dependencies changing)
