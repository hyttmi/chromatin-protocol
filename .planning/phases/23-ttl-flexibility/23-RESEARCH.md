# Phase 23: TTL Flexibility - Research

**Researched:** 2026-03-14
**Domain:** Blob lifecycle management -- writer-controlled TTL, operator-bounded max_ttl, tombstone garbage collection
**Confidence:** HIGH

## Summary

Phase 23 replaces the hardcoded `BLOB_TTL_SECONDS` constexpr (604800s / 7 days) with a writer-controlled TTL field that already exists in the wire format, bounded by a per-node configurable `max_ttl`. Tombstones, currently permanent (TTL=0), gain a configurable `tombstone_ttl` (default 365 days) so they are garbage-collected by the existing expiry scan.

The scope is clean and well-bounded. The wire format (`blob.fbs`) already has a `ttl:uint32` field that writers populate. The signing input already includes TTL (`build_signing_input` concatenates `namespace || data || ttl || timestamp`). The storage layer already creates expiry index entries for blobs with `ttl > 0` and skips them for `ttl == 0`. The existing `run_expiry_scan()` walks the expiry index in timestamp order and deletes expired blobs. All the infrastructure is in place; this phase adds policy enforcement (max_ttl), extends tombstone handling (tombstone_ttl expiry entries), and removes the now-unnecessary constexpr.

**Primary recommendation:** Add `max_ttl` and `tombstone_ttl` to `Config`, add `ttl_exceeded` to `IngestError`, insert a max_ttl check at Step 0 in `BlobEngine::ingest()`, override tombstone expiry in `Storage::store_blob()`, extend `run_expiry_scan()` to clean tombstone indexes, remove `BLOB_TTL_SECONDS`, and update `reload_config()`.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Reject blobs with TTL exceeding max_ttl outright (no clamping) -- signed TTL IS the TTL
- max_ttl is configurable per-node, SIGHUP-reloadable (consistent with existing config reload pattern)
- Default max_ttl: 7 days (604800s) -- matches current BLOB_TTL_SECONDS, zero behavior change for existing deployments
- max_ttl=0 in config means "no limit" (any TTL accepted) -- consistent with max_storage_bytes=0 convention
- Sync-received blobs follow the same validation path -- one ingest pipeline, no special sync exemptions
- Tombstones expire after configurable tombstone_ttl (default 365 days)
- tombstone_ttl is configurable per-node, SIGHUP-reloadable
- After tombstone expires, if the deleted blob re-arrives via sync, accept it -- tombstones are a protocol courtesy, not eternal guarantees
- Tombstones keep ttl=0 in signed wire data (no wire format change) -- the node creates an expiry index entry using tombstone_ttl config at storage time
- On tombstone expiry: clean all three indexes (blobs_map, expiry_map, AND tombstone_map) -- complete cleanup
- No minimum TTL floor -- TTL=0 (permanent) and any positive value are valid
- Remove the BLOB_TTL_SECONDS constexpr entirely -- the blob's own ttl field and the node's max_ttl config replace it
- No timestamp validation -- nodes don't need synchronized clocks, timestamp is part of signed data
- Add new IngestError variant for TTL exceeded (e.g., ttl_exceeded)
- No retroactive expiry -- max_ttl is enforced at ingest only, already-stored blobs keep their original expiry
- Tombstones use the same expiry scan (run_expiry_scan) -- tombstones will have expiry entries, caught by the same walk
- Tombstone detection during scan: decode blob and check is_tombstone() -- O(1) magic prefix check, no index format changes

### Claude's Discretion
- Log level for tombstone expiry vs regular blob expiry
- Exact error message wording for ttl_exceeded rejection
- Config field naming (max_ttl vs max_ttl_seconds etc.)
- Test structure and organization

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| TTL-01 | Blob TTL is set by the writer (included in signed blob data), not a hardcoded constant | Already supported by wire format (`blob.fbs` has `ttl:uint32`). Remove `BLOB_TTL_SECONDS` constexpr. Engine accepts writer's TTL as-is after max_ttl validation. |
| TTL-02 | Node enforces a configurable maximum TTL (default 7 days) -- blobs with TTL exceeding max are rejected | Add `max_ttl` to `Config` (default 604800, 0=no limit). Add `ttl_exceeded` to `IngestError`. Insert check at Step 0 in `BlobEngine::ingest()` before crypto. |
| TTL-03 | TTL=0 remains valid and means permanent (no expiry) | Already handled: `store_blob()` skips expiry entry when `blob.ttl == 0`. Max_ttl check must skip TTL=0 blobs (permanent is always allowed). |
| TTL-04 | Tombstones have a configurable TTL (default 365 days) instead of being permanent | Add `tombstone_ttl` to `Config` (default 31536000s). In `store_blob()`, when storing a tombstone, create expiry entry using `tombstone_ttl` + current time instead of blob.ttl. |
| TTL-05 | Expired tombstones are garbage collected by the existing expiry scan | Extend `run_expiry_scan()`: after deleting expired blob from `blobs_map`, decode it and if `is_tombstone()`, also clean `tombstone_map`. |
</phase_requirements>

## Standard Stack

### Core
| Library | Purpose | Why Standard |
|---------|---------|--------------|
| libmdbx (C++ API) | Storage engine -- expiry index, blob store, tombstone index | Already in use; expiry_map sorted by timestamp enables efficient scan |
| nlohmann/json | Config file parsing | Already in use for config loading |
| spdlog | Logging | Already in use throughout |
| Catch2 | Testing | Already in use for all 284 tests |

### Supporting
No new libraries needed. This phase uses only existing dependencies.

### Alternatives Considered
None. All decisions are locked -- no library choices to make.

## Architecture Patterns

### Recommended Change Locations

```
db/
  config/
    config.h          # Add max_ttl, tombstone_ttl to Config; remove BLOB_TTL_SECONDS
    config.cpp         # Parse max_ttl, tombstone_ttl from JSON
  engine/
    engine.h           # Add ttl_exceeded to IngestError; add max_ttl_ member
    engine.cpp         # Add max_ttl check at Step 0 in ingest()
  storage/
    storage.h          # Add tombstone_ttl parameter or setter
    storage.cpp        # Override expiry for tombstones in store_blob(); extend run_expiry_scan()
  peer/
    peer_manager.cpp   # Propagate max_ttl, tombstone_ttl on reload_config()
  main.cpp             # Pass max_ttl to BlobEngine constructor
tests/
  config/test_config.cpp     # New/updated tests for max_ttl, tombstone_ttl
  engine/test_engine.cpp     # New tests for ttl_exceeded rejection
  storage/test_storage.cpp   # New tests for tombstone expiry
```

### Pattern 1: Step 0 TTL Validation (Engine)

**What:** Insert max_ttl check before any expensive operations (crypto, storage lookups).
**When to use:** Every blob ingest (direct write and sync path).
**Existing pattern reference:** `BlobEngine::ingest()` already does size check at Step 0, capacity check at Step 0b.

```cpp
// In BlobEngine::ingest(), after Step 0b (capacity check), before Step 1 (structural):
// Step 0c: TTL check (int compare, cheaper than crypto)
// TTL=0 means permanent -- always allowed regardless of max_ttl
if (max_ttl_ > 0 && blob.ttl > 0 && blob.ttl > max_ttl_) {
    spdlog::warn("Ingest rejected: blob TTL {} exceeds max_ttl {}",
                 blob.ttl, max_ttl_);
    return IngestResult::rejection(IngestError::ttl_exceeded,
        "blob TTL " + std::to_string(blob.ttl) +
        " exceeds max_ttl " + std::to_string(max_ttl_));
}
```

**Key guard conditions:**
- `max_ttl_ > 0`: Skip check when max_ttl=0 (no limit)
- `blob.ttl > 0`: TTL=0 (permanent) is always allowed -- never reject permanent blobs
- `blob.ttl > max_ttl_`: Reject only when writer's TTL exceeds the node's maximum

### Pattern 2: Tombstone Expiry Override (Storage)

**What:** When storing a tombstone, create an expiry index entry using `tombstone_ttl` config value instead of the blob's TTL (which is 0 for tombstones).
**When to use:** In `Storage::store_blob()`, within the tombstone detection block.

```cpp
// In Storage::store_blob(), after the existing tombstone index population:
if (wire::is_tombstone(blob.data)) {
    // ... existing tombstone_map population ...

    // Create expiry entry for tombstone using node's tombstone_ttl (not blob.ttl)
    if (tombstone_ttl_ > 0) {
        uint64_t now = clock_();
        uint64_t expiry_time = now + tombstone_ttl_;
        auto exp_key = make_expiry_key(expiry_time, hash.data());
        txn.upsert(expiry_map, to_slice(exp_key),
                    mdbx::slice(blob.namespace_id.data(),
                                blob.namespace_id.size()));
    }
}
```

**Important:** This runs inside the existing `store_blob()` write transaction, maintaining atomicity. The tombstone's wire-level `ttl=0` is unchanged; the expiry entry is a local storage policy decision.

### Pattern 3: Tombstone Cleanup in Expiry Scan (Storage)

**What:** When expiry scan deletes a blob, check if it's a tombstone and clean the tombstone_map index too.
**When to use:** In `run_expiry_scan()`, after deleting from `blobs_map`.

```cpp
// In run_expiry_scan(), after erasing from blobs_map:
// Decode the blob to check if it's a tombstone (O(1) magic prefix check)
auto blob_key = make_blob_key(ns_ptr, hash_ptr);
auto blob_data = txn.get(blobs_map, to_slice(blob_key), not_found_sentinel);
if (blob_data.data() != nullptr) {
    auto blob = wire::decode_blob(std::span<const uint8_t>(
        static_cast<const uint8_t*>(blob_data.data()), blob_data.length()));
    if (wire::is_tombstone(blob.data)) {
        // Clean tombstone_map entry
        auto target_hash = wire::extract_tombstone_target(blob.data);
        auto ts_key = make_blob_key(ns_ptr, target_hash.data());
        try { txn.erase(tombstone_map, to_slice(ts_key)); }
        catch (const mdbx::exception&) { /* already deleted */ }
    }
    // Delete from blobs_map
    txn.erase(blobs_map, to_slice(blob_key));
}
```

**Critical ordering:** Decode the blob BEFORE deleting it from `blobs_map`. The current `run_expiry_scan()` deletes first -- this must be reordered for tombstone detection. Read the blob data, check if tombstone, clean tombstone_map, THEN delete from blobs_map.

### Pattern 4: Config Propagation via SIGHUP (PeerManager)

**What:** On config reload, propagate new `max_ttl` and `tombstone_ttl` values to engine and storage.
**When to use:** In `PeerManager::reload_config()`.
**Existing pattern reference:** Rate limiting values are propagated to `rate_limit_bytes_per_sec_` and `rate_limit_burst_` in `reload_config()`.

The engine and storage need setter methods (or the config values need to be stored as mutable references/pointers). The simplest approach matching the existing pattern (where `rate_limit_bytes_per_sec_` is a PeerManager member, not an engine member):

- **Option A (recommended):** Add `set_max_ttl(uint32_t)` to BlobEngine and `set_tombstone_ttl(uint32_t)` to Storage. Call them from `reload_config()`.
- **Option B:** Store `max_ttl_` in PeerManager and pass by reference to engine -- adds coupling.

Option A matches the principle of encapsulation and is consistent with how `max_storage_bytes` is passed to BlobEngine via constructor.

### Anti-Patterns to Avoid

- **Clamping TTL to max_ttl instead of rejecting:** The signed TTL IS the TTL. Clamping would mean the node's expiry doesn't match the writer's intent, and different nodes would expire the same blob at different times. Reject outright.
- **Adding a separate tombstone expiry scan:** Tombstones use the same expiry_map index and the same `run_expiry_scan()`. No separate scan needed.
- **Modifying tombstone wire format to include TTL:** Tombstones keep `ttl=0` in signed data. The expiry is a local node policy, not part of the signed blob.
- **Retroactive enforcement of max_ttl on stored blobs:** Max_ttl is checked at ingest only. Changing max_ttl on SIGHUP must not trigger re-validation of existing blobs.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Tombstone expiry tracking | Separate tombstone timer/index | Existing `expiry_map` + `run_expiry_scan()` | Same index format, same scan loop, same cleanup pattern. Adding a separate mechanism doubles maintenance. |
| Config reload propagation | Custom observer/event system | Direct setter calls in `reload_config()` | Matches existing pattern (rate_limit, sync_namespaces). Simple, explicit, no framework overhead. |
| TTL validation for sync blobs | Separate sync validation pipeline | Same `BlobEngine::ingest()` path | Locked decision: one ingest pipeline, no special sync exemptions. |

**Key insight:** Nearly all infrastructure exists. This phase is wiring new config fields to existing mechanisms, not building new mechanisms.

## Common Pitfalls

### Pitfall 1: Forgetting TTL=0 Guard in max_ttl Check
**What goes wrong:** If `max_ttl > 0` and the check doesn't exclude `TTL=0`, permanent blobs are rejected because `0 > max_ttl` is false but `0 == 0` could cause logic errors in poorly written checks.
**Why it happens:** TTL=0 has special semantics (permanent) that don't participate in max_ttl enforcement.
**How to avoid:** Explicit guard: `if (max_ttl_ > 0 && blob.ttl > 0 && blob.ttl > max_ttl_)`. Test both `TTL=0 with max_ttl=604800` and `TTL=0 with max_ttl=0`.
**Warning signs:** Test "TTL=0 blob accepted when max_ttl is set" fails.

### Pitfall 2: Expiry Scan Deletes Blob Before Reading It
**What goes wrong:** Current `run_expiry_scan()` deletes from `blobs_map` first, then advances cursor. If we need to decode the blob to check `is_tombstone()`, the data is already gone.
**Why it happens:** The current scan doesn't need to read blob contents -- it only needs the hash and namespace from the expiry key/value.
**How to avoid:** Read and decode the blob BEFORE deleting it from `blobs_map`. If the blob is already gone (deleted by some other path), skip tombstone cleanup -- it's a best-effort operation.
**Warning signs:** Tombstone entries left in `tombstone_map` after their expiry (orphaned entries).

### Pitfall 3: Tombstone Expiry Uses Wrong Timestamp Base
**What goes wrong:** If tombstone expiry is calculated as `blob.timestamp + tombstone_ttl`, the timestamp is the writer's clock, which may differ from the node's clock. This could cause tombstones to expire too early or too late relative to the node's time.
**Why it happens:** Decision says "no timestamp validation -- nodes don't need synchronized clocks."
**How to avoid:** Use the node's current time (`clock_()`) as the base for tombstone expiry: `now + tombstone_ttl`. This is a local storage policy, not a signed protocol field.
**Warning signs:** Tombstones expiring at unexpected times in tests with fixed clocks.

### Pitfall 4: delete_blob() Path Doesn't Create Tombstone Expiry
**What goes wrong:** `BlobEngine::delete_blob()` creates tombstones and calls `storage_.store_blob()`. If the tombstone expiry logic is only in the ingest path, `delete_blob()` tombstones won't get expiry entries.
**Why it happens:** Both `ingest()` and `delete_blob()` end up calling `storage_.store_blob()`, so as long as the tombstone expiry logic is in `store_blob()`, both paths are covered.
**How to avoid:** Place tombstone expiry entry creation in `Storage::store_blob()`, not in `BlobEngine::ingest()`. Verify with a test that tombstones from `delete_blob()` also get expiry entries.
**Warning signs:** Tombstones created via direct delete never expire.

### Pitfall 5: SIGHUP Reload Doesn't Propagate New TTL Values
**What goes wrong:** After SIGHUP, `max_ttl` and `tombstone_ttl` remain at their original values because `reload_config()` doesn't update them.
**Why it happens:** `reload_config()` currently updates ACL, rate limits, and sync_namespaces, but new config fields need to be explicitly added.
**How to avoid:** Add max_ttl and tombstone_ttl propagation to `reload_config()` alongside the existing rate limit propagation. Test that after SIGHUP, new ingest calls use the updated max_ttl.
**Warning signs:** Config change has no effect until daemon restart.

### Pitfall 6: Test Referencing Removed BLOB_TTL_SECONDS
**What goes wrong:** Existing test `"BLOB_TTL_SECONDS is a protocol constant"` and the assertion in `"Default config has sensible values"` will fail to compile after removing the constexpr.
**Why it happens:** These tests explicitly assert the existence and value of the constexpr being removed.
**How to avoid:** Remove or replace these test cases. Replace with tests for `max_ttl` default value (604800) and `tombstone_ttl` default value (31536000).
**Warning signs:** Compilation failure in `tests/config/test_config.cpp`.

## Code Examples

### BlobEngine Constructor Change
```cpp
// Current:
explicit BlobEngine(storage::Storage& store, uint64_t max_storage_bytes = 0);

// New:
explicit BlobEngine(storage::Storage& store, uint64_t max_storage_bytes = 0,
                    uint32_t max_ttl = 604800);

void set_max_ttl(uint32_t max_ttl) { max_ttl_ = max_ttl; }
```

### Config Struct Additions
```cpp
// In config::Config struct:
uint32_t max_ttl = 604800;              // 7 days (0 = no limit)
uint32_t tombstone_ttl = 31536000;      // 365 days (0 = permanent, no expiry)
```

### Config JSON Parsing
```cpp
// In load_config():
cfg.max_ttl = j.value("max_ttl", cfg.max_ttl);
cfg.tombstone_ttl = j.value("tombstone_ttl", cfg.tombstone_ttl);
```

### IngestError Enum Addition
```cpp
enum class IngestError {
    namespace_mismatch,
    invalid_signature,
    malformed_blob,
    oversized_blob,
    storage_error,
    tombstoned,
    no_delegation,
    storage_full,
    ttl_exceeded         // NEW: blob TTL exceeds node's max_ttl
};
```

### Storage Tombstone TTL Setter
```cpp
// In Storage class:
void set_tombstone_ttl(uint32_t ttl) { impl_->tombstone_ttl = ttl; }

// In Storage::Impl:
uint32_t tombstone_ttl = 31536000;  // 365 days default
```

### reload_config() Additions
```cpp
// In PeerManager::reload_config(), after sync_namespaces reload:
engine_.set_max_ttl(new_cfg.max_ttl);
storage_.set_tombstone_ttl(new_cfg.tombstone_ttl);
spdlog::info("config reload: max_ttl={}s tombstone_ttl={}s",
             new_cfg.max_ttl, new_cfg.tombstone_ttl);
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `BLOB_TTL_SECONDS` constexpr (604800s) | Writer-controlled TTL bounded by `max_ttl` config | Phase 23 | Writers control blob lifetime; operators bound it |
| Tombstones permanent (`ttl=0`, never expire) | Tombstones expire after `tombstone_ttl` (365d default) | Phase 23 | Prevents unbounded tombstone accumulation |

**Deprecated/outdated:**
- `BLOB_TTL_SECONDS` constexpr: Removed entirely. Replaced by `Config::max_ttl` (operator policy) and `BlobData::ttl` (writer intent).
- Test `"BLOB_TTL_SECONDS is a protocol constant"`: Must be removed/replaced.

## Open Questions

1. **max_ttl check in delete_blob() path**
   - What we know: `delete_blob()` creates tombstones with `ttl=0`. Max_ttl check should not reject tombstones (they're always `ttl=0`, which is allowed).
   - What's unclear: Should `delete_blob()` also have the max_ttl check for consistency, or is it unnecessary because tombstones are always `ttl=0`?
   - Recommendation: No max_ttl check in `delete_blob()`. Tombstones are `ttl=0` by definition, which bypasses the check. Adding it would be dead code.

2. **Config field naming**
   - Claude's discretion. Options: `max_ttl` vs `max_ttl_seconds`, `tombstone_ttl` vs `tombstone_ttl_seconds`.
   - Recommendation: Use `max_ttl` and `tombstone_ttl` (without `_seconds` suffix). Matches existing pattern: `sync_interval_seconds` has the suffix because `sync_interval` alone is ambiguous, but `ttl` unambiguously means seconds in this protocol. Both are `uint32_t`, consistent with the wire format's `ttl:uint32`.

3. **Log level for tombstone expiry**
   - Claude's discretion. Regular blob expiry logs at `info` level when `purged > 0`.
   - Recommendation: Log tombstone expiry at `info` level (same as regular blob expiry) but with a distinct message: "Expiry scan: purged {} blobs ({} tombstones)". This keeps the single log line but distinguishes the two types.

## Sources

### Primary (HIGH confidence)
- Codebase inspection: `db/config/config.h` -- current `BLOB_TTL_SECONDS` constexpr and Config struct
- Codebase inspection: `db/engine/engine.h` / `engine.cpp` -- IngestError enum, ingest() validation pipeline, Step 0 pattern
- Codebase inspection: `db/storage/storage.h` / `storage.cpp` -- store_blob() expiry entry creation, run_expiry_scan() implementation, tombstone_map handling
- Codebase inspection: `db/wire/codec.h` -- BlobData struct (ttl field), is_tombstone(), extract_tombstone_target()
- Codebase inspection: `db/peer/peer_manager.cpp` -- reload_config() pattern, expiry_scan_loop()
- Codebase inspection: `db/schemas/blob.fbs` -- wire format schema confirming ttl:uint32 field
- Codebase inspection: `db/main.cpp` -- component wiring (BlobEngine constructor, Storage creation)
- Codebase inspection: `tests/config/test_config.cpp` -- existing BLOB_TTL_SECONDS test that must be removed/updated
- Codebase inspection: `tests/engine/test_engine.cpp` -- existing tombstone test patterns
- Codebase inspection: `tests/storage/test_storage.cpp` -- existing expiry scan and tombstone test patterns

### Secondary (MEDIUM confidence)
- 23-CONTEXT.md -- user decisions constraining implementation approach

### Tertiary (LOW confidence)
None.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all changes use existing libraries
- Architecture: HIGH -- all change locations identified via code inspection, patterns match existing codebase
- Pitfalls: HIGH -- identified from direct code analysis of current implementation details

**Research date:** 2026-03-14
**Valid until:** 2026-04-14 (stable -- internal codebase, no external API changes)
