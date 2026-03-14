# Phase 23: TTL Flexibility - Context

**Gathered:** 2026-03-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Blob lifetimes are controlled by writers and bounded by operators, and tombstones are garbage-collected. Writers set TTL in signed blob data. Nodes enforce a configurable maximum. Tombstones expire after a configurable period and are cleaned up by the existing expiry scan. TTL=0 remains permanent.

</domain>

<decisions>
## Implementation Decisions

### TTL enforcement policy
- Reject blobs with TTL exceeding max_ttl outright (no clamping) — signed TTL IS the TTL
- max_ttl is configurable per-node, SIGHUP-reloadable (consistent with existing config reload pattern)
- Default max_ttl: 7 days (604800s) — matches current BLOB_TTL_SECONDS, zero behavior change for existing deployments
- max_ttl=0 in config means "no limit" (any TTL accepted) — consistent with max_storage_bytes=0 convention
- Sync-received blobs follow the same validation path — one ingest pipeline, no special sync exemptions

### Tombstone expiry
- Tombstones expire after configurable tombstone_ttl (default 365 days)
- tombstone_ttl is configurable per-node, SIGHUP-reloadable
- After tombstone expires, if the deleted blob re-arrives via sync, accept it — tombstones are a protocol courtesy, not eternal guarantees
- Tombstones keep ttl=0 in signed wire data (no wire format change) — the node creates an expiry index entry using tombstone_ttl config at storage time
- On tombstone expiry: clean all three indexes (blobs_map, expiry_map, AND tombstone_map) — complete cleanup

### Writer TTL semantics
- No minimum TTL floor — TTL=0 (permanent) and any positive value are valid
- Remove the BLOB_TTL_SECONDS constexpr entirely — the blob's own ttl field and the node's max_ttl config replace it
- No timestamp validation — nodes don't need synchronized clocks, timestamp is part of signed data
- Add new IngestError variant for TTL exceeded (e.g., ttl_exceeded)

### Stored blob handling
- No retroactive expiry — max_ttl is enforced at ingest only, already-stored blobs keep their original expiry
- Tombstones use the same expiry scan (run_expiry_scan) — tombstones will have expiry entries, caught by the same walk
- Tombstone detection during scan: decode blob and check is_tombstone() — O(1) magic prefix check, no index format changes

### Claude's Discretion
- Log level for tombstone expiry vs regular blob expiry
- Exact error message wording for ttl_exceeded rejection
- Config field naming (max_ttl vs max_ttl_seconds etc.)
- Test structure and organization

</decisions>

<specifics>
## Specific Ideas

- Consistent with existing 0=unlimited convention (max_storage_bytes, rate limiting)
- One validation pipeline for all blobs regardless of arrival path (direct write or sync)
- Wire format is untouched — tombstone ttl=0 in signed data, expiry is local policy

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `config::Config` struct (`db/config/config.h`): Add max_ttl and tombstone_ttl fields alongside existing config
- `engine::IngestError` enum (`db/engine/engine.h`): Add ttl_exceeded variant
- `storage::Storage::run_expiry_scan()` (`db/storage/storage.cpp`): Extend to clean tombstone_map entries
- `wire::is_tombstone()` (`db/wire/codec.h`): Already exists for O(1) tombstone detection during scan
- `storage::Storage::store_blob()` (`db/storage/storage.cpp`): Already creates expiry entries for ttl>0, extend for tombstone_ttl override

### Established Patterns
- Config SIGHUP reload: config_path stored, reload triggered by signal handler coroutine
- Fail-fast validation: cheap checks first (Step 0 pattern) — TTL check before crypto
- 0=unlimited convention: max_storage_bytes=0, rate_limit=0
- Expiry index: [expiry_ts_be:8][hash:32] -> namespace:32

### Integration Points
- `BlobEngine::ingest()` — Add max_ttl check before signature verification (Step 0: int compare before expensive crypto)
- `Storage::store_blob()` — When storing tombstone, create expiry entry using tombstone_ttl instead of blob.ttl
- `run_expiry_scan()` — After deleting expired blob, check if it's a tombstone and also clean tombstone_map
- Config reload handler — Propagate new max_ttl and tombstone_ttl values to engine/storage
- `config::load_config()` — Parse new max_ttl and tombstone_ttl JSON fields

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 23-ttl-flexibility*
*Context gathered: 2026-03-14*
