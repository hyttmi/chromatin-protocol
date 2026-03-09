# Phase 16: Storage Foundation - Research

**Researched:** 2026-03-09
**Domain:** libmdbx sub-database indexing, storage capacity enforcement, protocol-level signaling
**Confidence:** HIGH

## Summary

Phase 16 adds three capabilities to chromatindb: O(1) tombstone lookups via an indexed sub-database, configurable storage capacity limits enforced at the ingest boundary, and StorageFull wire messages that suppress sync pushes from peers. All three build directly on established patterns already proven in the codebase (delegation_map indexing, Step 0 ingest checks, PeerInfo flags).

The primary technical risk is low. The `delegation_map` pattern (compound key, empty or hash value, same-transaction writes in `store_blob()`) is an exact template for `tombstone_map`. The `env.get_info()` API provides `mi_geo.current` for database file size in a single call with no scanning. The wire protocol extension follows the same pattern as every prior TransportMsgType addition.

**Primary recommendation:** Follow the delegation_map pattern exactly for tombstone indexing, use `env.get_info().mi_geo.current` for storage size queries, and add StorageFull as TransportMsgType value 23 with an empty payload.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Key schema: `[namespace:32][target_hash:32]` with empty value (existence check only)
- Index writes happen at the storage layer -- `store_blob()` detects tombstone data and writes both `blobs_map` and `tombstone_map` in the same mdbx transaction (atomic)
- Index cleanup: `delete_blob_data()` also removes `tombstone_map` entry if the deleted blob is a tombstone
- Bumps `max_maps` from 5 to 6 (5 sub-databases + 1 spare)
- `has_tombstone_for()` switches from O(n) cursor scan to O(1) `tombstone_map` lookup
- `max_storage_bytes` as a top-level config field (matches existing flat Config struct pattern)
- Distinct `IngestError::storage_full` enum value (not reuse of `storage_error`) -- enables caller to pattern-match and send StorageFull wire message
- Dedicated `StorageFull` TransportMsgType (value 23 in transport.fbs)
- Recovery is reconnection-only -- no StorageAvailable message (YAGNI)
- Peers receiving StorageFull set `peer_is_full` flag on PeerInfo
- Both Data message ingests and sync-received blobs check capacity (consistent enforcement regardless of source)
- Tombstone blobs exempt from capacity check -- they are small (36 bytes) and free space by deleting the target blob
- Capacity check runs as Step 0 in `BlobEngine::ingest()` before any crypto operations

### Claude's Discretion
- Migration strategy for existing tombstones (one-time startup scan vs forward-only indexing)
- In-memory counter vs mdbx stat query for used_bytes tracking
- Default value for max_storage_bytes (0=unlimited or a conservative default like 1 GiB)
- Sync push suppression scope (suppress all sync initiation vs suppress only blob transfers in Phase C)
- StorageFull message frequency (per-rejection vs send-once-then-silent)
- Size calculation for capacity check (blob data size only vs full storage cost estimate)
- Logging verbosity on storage-full transitions
- Whether delegation blobs are exempt from capacity check (tombstones are confirmed exempt)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STOR-01 | Tombstone lookups use O(1) indexed check via dedicated mdbx sub-database instead of O(n) namespace scan | Tombstone index pattern: delegation_map is the exact template. Key `[namespace:32][target_hash:32]`, empty value, `mdbx::txn::get()` with 3-arg sentinel for O(1) lookup. Startup migration populates index from existing tombstones. |
| STOR-02 | Operator can configure a global storage limit (max_storage_bytes) that prevents the node from exceeding disk capacity | Config struct gets `uint64_t max_storage_bytes` field. `load_config()` parses it from JSON. `env.get_info().mi_geo.current` gives actual database file size without scanning. |
| STOR-03 | Storage limit check runs as Step 0 inside synchronous ingest() before any crypto operations | BlobEngine::ingest() already has Step 0 (oversized_blob check). New capacity check inserts before it or immediately after. Needs Storage reference to query size. |
| STOR-04 | Node sends StorageFull wire message to peers when rejecting a blob due to capacity | TransportMsgType_StorageFull = 23 in transport.fbs. Empty payload (no data needed -- the peer knows what it tried to send). Sent from Data message handler in PeerManager. |
| STOR-05 | Peers receiving StorageFull set a peer_is_full flag and suppress sync pushes to that peer | `bool peer_is_full = false` on PeerInfo. Set on StorageFull receipt. Checked before sync Phase C blob transfer initiation. Cleared on reconnect (PeerInfo recreated). |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libmdbx | latest (FetchContent) | Sub-database creation, compound key indexing, env info queries | Already in use; `env.get_info()` provides file size, `txn.get()` with sentinel provides O(1) keyed lookup |
| FlatBuffers | latest (FetchContent) | TransportMsgType enum extension (StorageFull = 23) | Already in use for wire format; `flatc` generates `transport_generated.h` |
| nlohmann/json | latest (FetchContent) | Config parsing for `max_storage_bytes` field | Already in use for config loading |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| spdlog | latest (FetchContent) | Logging storage-full transitions, tombstone migration progress | Already in use everywhere |
| Catch2 | latest (FetchContent) | Testing all three sub-plans | Already in use for all tests |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `env.get_info().mi_geo.current` | In-memory counter (increment on store, decrement on delete) | Counter is O(1) amortized but drifts on crash recovery; `mi_geo.current` is always authoritative and already O(1) from mdbx internal metadata |
| Startup migration scan | Forward-only indexing | Forward-only is simpler but leaves existing tombstones un-indexed; migration is a one-time cost at startup |

## Architecture Patterns

### Recommended Project Structure
No new files or directories. All changes are additions to existing files:
```
db/
  storage/storage.h       # Add tombstone_map handle, used_bytes() method
  storage/storage.cpp     # Add tombstone_map DBI, index writes, migration, size query
  engine/engine.h         # Add IngestError::storage_full, storage capacity check
  engine/engine.cpp       # Add Step 0 capacity check in ingest()
  config/config.h         # Add max_storage_bytes field to Config
  config/config.cpp       # Parse max_storage_bytes from JSON
  peer/peer_manager.h     # Add peer_is_full to PeerInfo
  peer/peer_manager.cpp   # Handle StorageFull, suppress sync pushes
schemas/
  transport.fbs           # Add StorageFull = 23
db/wire/
  transport_generated.h   # Regenerated by flatc
tests/
  storage/test_storage.cpp  # Tombstone index tests
  engine/test_engine.cpp    # Capacity limit tests
  config/test_config.cpp    # max_storage_bytes parsing tests
```

### Pattern 1: Indexed Sub-Database (delegation_map template)
**What:** Compound key `[namespace:32][hash:32]` in a dedicated mdbx DBI for O(1) existence checks.
**When to use:** Whenever a query would otherwise require O(n) scanning of blobs in a namespace.
**Example (from existing delegation_map in storage.cpp):**
```cpp
// In Storage::Impl constructor -- create DBI alongside existing ones
tombstone_map = txn.create_map("tombstone");

// In store_blob() -- populate index atomically in same transaction
if (wire::is_tombstone(blob.data)) {
    auto target_hash = wire::extract_tombstone_target(blob.data);
    auto ts_key = make_blob_key(blob.namespace_id.data(), target_hash.data());
    txn.upsert(impl_->tombstone_map, to_slice(ts_key), mdbx::slice());
}

// In has_tombstone_for() -- O(1) lookup replaces O(n) cursor scan
auto ts_key = make_blob_key(ns.data(), target_blob_hash.data());
auto txn = impl_->env.start_read();
auto val = txn.get(impl_->tombstone_map, to_slice(ts_key), not_found_sentinel);
return val.data() != nullptr;
```

### Pattern 2: Step 0 Capacity Gate
**What:** Cheapest-possible rejection before any cryptographic operations on the ingest path.
**When to use:** Any ingest-time check that can be evaluated with a simple comparison.
**Example (extending existing Step 0 in engine.cpp):**
```cpp
IngestResult BlobEngine::ingest(const wire::BlobData& blob) {
    // Step 0a: Size check (existing -- one integer comparison)
    if (blob.data.size() > net::MAX_BLOB_DATA_SIZE) { ... }

    // Step 0b: Capacity check (new -- query + comparison, still cheaper than crypto)
    // Tombstones exempt: small (36 bytes) and they free space
    if (max_storage_bytes_ > 0 && !wire::is_tombstone(blob.data)) {
        if (storage_.used_bytes() >= max_storage_bytes_) {
            return IngestResult::rejection(IngestError::storage_full,
                "storage capacity exceeded");
        }
    }

    // Step 1: Structural checks ...
}
```

### Pattern 3: Peer Flag with Reconnection Reset
**What:** Boolean flag on PeerInfo that controls sync behavior, automatically cleared on reconnect.
**When to use:** Transient per-connection state that should reset when the connection is re-established.
**Example:**
```cpp
// PeerInfo already recreated on connect -- flag naturally resets
struct PeerInfo {
    // ... existing fields ...
    bool peer_is_full = false;  // Set on StorageFull, cleared on reconnect
};

// In on_peer_message() -- handle StorageFull
if (type == wire::TransportMsgType_StorageFull) {
    auto* peer = find_peer(conn);
    if (peer) {
        peer->peer_is_full = true;
        spdlog::info("Peer {} reported storage full, suppressing sync pushes",
                     conn->remote_address());
    }
    return;
}
```

### Anti-Patterns to Avoid
- **In-memory byte counter without crash recovery:** Maintaining a `uint64_t used_bytes_` counter that increments/decrements on store/delete will drift after a crash. The mdbx `mi_geo.current` value is authoritative because mdbx itself tracks it.
- **Tombstone migration at query time:** Lazily building the tombstone index on first `has_tombstone_for()` call creates unpredictable latency spikes. Do it at startup.
- **StorageFull as error response to sync BlobTransfer:** The sync protocol (Phase A/B/C) has its own flow. StorageFull should cause the receiver to skip the blob silently (via IngestError::storage_full in `ingest_blobs()`) and then send StorageFull back on the connection, not break the sync state machine.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Database file size query | Manual file stat or byte counting | `env.get_info().mi_geo.current` | mdbx tracks this internally; file size includes all overhead and is authoritative |
| O(1) keyed existence check | Hash table or bloom filter in memory | mdbx sub-database with `txn.get()` 3-arg | Already have proven pattern (delegation_map); crash-safe, persistent, zero maintenance |
| Tombstone detection in blob data | Re-parsing blob data every time | `wire::is_tombstone()` | Already exists, checks 4-byte magic prefix |

**Key insight:** libmdbx itself is the index engine. Creating a sub-database with `txn.create_map()` gives you a crash-safe, ACID-consistent O(1) lookup. There is no need for in-memory data structures for any of the persistence concerns in this phase.

## Common Pitfalls

### Pitfall 1: max_maps Misconfiguration
**What goes wrong:** Adding a 5th sub-database (tombstone_map) without increasing `max_maps` causes mdbx to throw at `create_map()` time.
**Why it happens:** `max_maps` is set once at environment open time. Currently set to 5 (4 needed + 1 spare).
**How to avoid:** Bump `max_maps` from 5 to 6 (5 sub-databases: blobs, sequence, expiry, delegation, tombstone + 1 spare). This is in `Storage::Impl` constructor, line ~98.
**Warning signs:** `MDBX_DBS_FULL` error at startup.

### Pitfall 2: Migration Transaction Size
**What goes wrong:** Scanning all blobs across all namespaces in a single write transaction for tombstone migration can hit mdbx transaction size limits or hold the write lock too long.
**Why it happens:** Large databases with many namespaces and blobs.
**How to avoid:** Batch the migration: scan blobs_map with a read transaction, collect tombstone entries, then write them to tombstone_map in batched write transactions (e.g., 1000 entries per batch).
**Warning signs:** Slow startup, mdbx transaction overflow errors.

### Pitfall 3: Capacity Check Race with Concurrent Ingest
**What goes wrong:** Two concurrent ingest paths both pass the capacity check, then both store, pushing past the limit.
**Why it happens:** The capacity check (read) and store (write) are not atomic.
**How to avoid:** This is NOT a real concern for chromatindb because Storage is NOT thread-safe and runs on a single io_context thread. The check-then-store sequence is effectively atomic. Document this assumption.
**Warning signs:** N/A for current architecture.

### Pitfall 4: StorageFull in Sync Breaks Protocol State Machine
**What goes wrong:** Sending StorageFull during sync Phase C instead of completing the sync round causes the peer's sync state machine to hang waiting for SyncComplete.
**Why it happens:** Mixing storage rejection with sync protocol flow.
**How to avoid:** In `SyncProtocol::ingest_blobs()`, when ingest returns `storage_full`, skip the blob silently (log it) and continue processing remaining blobs. After the sync completes normally, the peer_manager can send StorageFull separately. Alternatively, the sync responder can send StorageFull after SyncComplete.
**Warning signs:** Sync hangs, peer timeouts.

### Pitfall 5: Tombstone Index Cleanup on Non-Tombstone Deletion
**What goes wrong:** `delete_blob_data()` tries to clean tombstone_map for every deleted blob, even non-tombstones.
**Why it happens:** Copy-paste from delegation_map cleanup without considering that only tombstone blobs have tombstone_map entries.
**How to avoid:** In `delete_blob_data()`, only attempt tombstone_map cleanup when the blob being deleted is itself a tombstone (`wire::is_tombstone(blob.data)`). This mirrors the existing delegation_map pattern which checks `wire::is_delegation(blob.data)`.
**Warning signs:** Unnecessary mdbx erase calls (harmless but wasteful).

## Code Examples

Verified patterns from existing codebase:

### Sub-Database Creation (from storage.cpp)
```cpp
// Source: db/storage/storage.cpp, Storage::Impl constructor
auto txn = env.start_write();
blobs_map = txn.create_map("blobs");
seq_map = txn.create_map("sequence");
expiry_map = txn.create_map("expiry");
delegation_map = txn.create_map("delegation");
// NEW: tombstone_map = txn.create_map("tombstone");
txn.commit();
```

### O(1) Indexed Lookup (from has_valid_delegation)
```cpp
// Source: db/storage/storage.cpp, has_valid_delegation()
auto deleg_key = make_blob_key(namespace_id.data(), delegate_pk_hash.data());
auto txn = impl_->env.start_read();
auto val = txn.get(impl_->delegation_map, to_slice(deleg_key), not_found_sentinel);
return val.data() != nullptr;
```

### Same-Transaction Index Write (from store_blob delegation)
```cpp
// Source: db/storage/storage.cpp, store_blob()
if (wire::is_delegation(blob.data)) {
    auto delegate_pubkey = wire::extract_delegate_pubkey(blob.data);
    auto delegate_pk_hash = crypto::sha3_256(delegate_pubkey);
    auto deleg_key = make_blob_key(blob.namespace_id.data(), delegate_pk_hash.data());
    txn.upsert(impl_->delegation_map, to_slice(deleg_key),
                mdbx::slice(hash.data(), hash.size()));
}
```

### Index Cleanup on Delete (from delete_blob_data delegation)
```cpp
// Source: db/storage/storage.cpp, delete_blob_data()
if (wire::is_delegation(blob.data)) {
    auto delegate_pubkey = wire::extract_delegate_pubkey(blob.data);
    auto delegate_pk_hash = crypto::sha3_256(delegate_pubkey);
    auto deleg_key = make_blob_key(ns.data(), delegate_pk_hash.data());
    try {
        txn.erase(impl_->delegation_map, to_slice(deleg_key));
    } catch (const mdbx::exception&) {
        // Already deleted -- not an error
    }
}
```

### Config Field Pattern (from config.cpp)
```cpp
// Source: db/config/config.cpp, load_config()
cfg.max_peers = j.value("max_peers", cfg.max_peers);
// NEW: cfg.max_storage_bytes = j.value("max_storage_bytes", cfg.max_storage_bytes);
```

### PeerInfo Flag Pattern (from peer_manager.h)
```cpp
// Source: db/peer/peer_manager.h, PeerInfo struct
struct PeerInfo {
    net::Connection::Ptr connection;
    std::string address;
    bool is_bootstrap = false;
    uint32_t strike_count = 0;
    bool syncing = false;
    // NEW: bool peer_is_full = false;
};
```

### TransportMsgType Extension (from transport.fbs)
```
// Source: schemas/transport.fbs
Notification = 22
// NEW: StorageFull = 23
```

### mdbx Database Size Query
```cpp
// Source: libmdbx mdbx.h++, env::get_info()
// env.get_info() returns MDBX_envinfo struct
// mi_geo.current = current datafile size in bytes (authoritative)
auto info = impl_->env.get_info();
return info.mi_geo.current;  // uint64_t bytes
```

## Discretion Recommendations

Based on codebase analysis, here are recommendations for the areas marked as Claude's discretion:

### Migration Strategy: One-Time Startup Scan
**Recommendation:** Full migration at startup.
**Rationale:** Forward-only indexing leaves existing tombstones un-indexed, meaning `has_tombstone_for()` would need a fallback O(n) path for pre-migration tombstones. A one-time startup scan is simple, runs once, and makes the O(1) guarantee unconditional. Batch the scan in groups of ~1000 to avoid oversized transactions.

### Used Bytes Tracking: mdbx stat query (not in-memory counter)
**Recommendation:** Use `env.get_info().mi_geo.current` for each capacity check.
**Rationale:** This is a metadata read from mdbx internals, not a file stat or scan. It is O(1) and always authoritative. An in-memory counter would need initialization from mdbx stats at startup anyway, would drift on crash recovery, and adds state to manage. The mdbx query is simpler and correct by construction. The cost is negligible compared to the crypto operations that follow.

### Default max_storage_bytes: 0 (unlimited)
**Recommendation:** Default to 0 meaning unlimited.
**Rationale:** Backward compatible with all existing deployments. Operators who want limits opt in explicitly. A 1 GiB default could surprise operators who store many large blobs. The requirement says "configurable" not "default-limited."

### Sync Push Suppression Scope: Suppress only Phase C blob transfers
**Recommendation:** Suppress blob transfer initiation (Phase C) when `peer_is_full` is true, but still perform Phase A/B (namespace and hash exchange).
**Rationale:** Phase A/B is lightweight (namespace lists, hash lists) and allows the non-full peer to still request blobs FROM the full peer. Only outbound blob pushes TO the full peer should be suppressed. This preserves bidirectional sync for the non-full direction.

### StorageFull Message Frequency: Send once per rejection, no dedup
**Recommendation:** Send StorageFull on each Data message rejection (simple, stateless).
**Rationale:** Data messages are client-originated (not high-frequency sync traffic). The peer should set `peer_is_full` idempotently on first receipt. Deduplication adds state for no gain. For sync, the `ingest_blobs()` path skips blobs silently and does not send StorageFull per-blob; instead a single StorageFull is sent after sync completes if any blob was rejected for capacity.

### Size Calculation: Database file size (mi_geo.current)
**Recommendation:** Use `mi_geo.current` (total database file size including overhead).
**Rationale:** This is what actually consumes disk. Using "blob data size only" would undercount (mdbx has B-tree overhead, page alignment, free space management). The operator configures a disk budget; the actual file size is what matters.

### Delegation Blobs: NOT exempt from capacity check
**Recommendation:** Only tombstones are exempt. Delegation blobs count toward storage.
**Rationale:** Delegation blobs are 2596 bytes (4-byte magic + 2592-byte pubkey), non-trivial in size. Unlike tombstones, delegation blobs do not free space. Exempting them would allow unbounded storage growth via delegation spam.

### Logging: Log storage-full transitions, not every rejection
**Recommendation:** Log at `warn` level when transitioning INTO storage-full state (first rejection). Log at `debug` level for subsequent rejections. Log at `info` when transitioning OUT (after space freed by expiry or tombstone).
**Rationale:** Prevents log spam when a full node receives many blobs, while still surfacing the important state transitions.

## Open Questions

1. **BlobEngine needs Storage access for capacity check**
   - What we know: BlobEngine currently holds a `storage::Storage&` reference. It needs to call a new `used_bytes()` method on Storage and compare against `max_storage_bytes`.
   - What's unclear: Should `max_storage_bytes` be passed to BlobEngine constructor, or should it check via a Config reference?
   - Recommendation: Pass `uint64_t max_storage_bytes` to BlobEngine constructor (simple, matches how storage_ is passed). Config is not currently a BlobEngine dependency and adding it would widen the interface unnecessarily.

2. **StorageFull timing in sync flow**
   - What we know: `SyncProtocol::ingest_blobs()` calls `engine_.ingest()` per blob. If ingest returns `storage_full`, the blob is skipped.
   - What's unclear: When exactly to send StorageFull to the sync peer. During sync? After sync completes?
   - Recommendation: After sync completes, if any blob was rejected with `storage_full`, send StorageFull on the connection. This keeps the sync protocol clean and the StorageFull signal orthogonal. The `ingest_blobs()` return stats could include a `storage_full_count` field.

## Sources

### Primary (HIGH confidence)
- libmdbx mdbx.h++ (local: `build/_deps/libmdbx-src/mdbx.h++`) -- env::get_info(), env::get_stat(), txn::get_map_stat(), MDBX_envinfo struct
- libmdbx mdbx.h (local: `build/_deps/libmdbx-src/mdbx.h`) -- MDBX_envinfo.mi_geo.current, MDBX_stat fields
- Existing codebase patterns (local: `db/storage/storage.cpp`) -- delegation_map implementation, store_blob(), delete_blob_data(), has_valid_delegation()
- Existing codebase patterns (local: `db/engine/engine.cpp`) -- Step 0 pattern, IngestError enum, ingest() pipeline
- Existing codebase patterns (local: `db/peer/peer_manager.cpp`) -- PeerInfo struct, on_peer_message routing, sync protocol flow
- FlatBuffers schema (local: `schemas/transport.fbs`) -- TransportMsgType enum values

### Secondary (MEDIUM confidence)
- libmdbx documentation for mi_geo.current semantics (confirmed via header struct comments)

### Tertiary (LOW confidence)
- None -- all findings verified from local source code

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing libraries
- Architecture: HIGH -- every pattern is a direct copy of existing proven code (delegation_map, Step 0, PeerInfo flags)
- Pitfalls: HIGH -- identified from reading actual implementation, not hypothetical

**Research date:** 2026-03-09
**Valid until:** 2026-04-09 (stable -- no external dependency changes expected)
