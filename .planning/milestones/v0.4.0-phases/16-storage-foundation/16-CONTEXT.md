# Phase 16: Storage Foundation - Context

**Gathered:** 2026-03-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Node enforces storage capacity at the protocol boundary with O(1) tombstone verification. Three capabilities: tombstone index (O(1) lookups via dedicated mdbx sub-database), storage limits (configurable max_storage_bytes with Step 0 capacity check), and disk-full peer signaling (StorageFull wire message with sync push suppression).

</domain>

<decisions>
## Implementation Decisions

### Tombstone index design
- Key schema: `[namespace:32][target_hash:32]` with empty value (existence check only)
- Index writes happen at the storage layer — `store_blob()` detects tombstone data and writes both `blobs_map` and `tombstone_map` in the same mdbx transaction (atomic)
- Index cleanup: `delete_blob_data()` also removes `tombstone_map` entry if the deleted blob is a tombstone
- Bumps `max_maps` from 5 to 6 (5 sub-databases + 1 spare)
- `has_tombstone_for()` switches from O(n) cursor scan to O(1) `tombstone_map` lookup

### Storage capacity tracking
- `max_storage_bytes` as a top-level config field (matches existing flat Config struct pattern)
- Distinct `IngestError::storage_full` enum value (not reuse of `storage_error`) — enables caller to pattern-match and send StorageFull wire message

### Disk-full peer signaling
- Dedicated `StorageFull` TransportMsgType (value 23 in transport.fbs)
- Recovery is reconnection-only — no StorageAvailable message (YAGNI)
- Peers receiving StorageFull set `peer_is_full` flag on PeerInfo

### Capacity check granularity
- Both Data message ingests and sync-received blobs check capacity (consistent enforcement regardless of source)
- Tombstone blobs exempt from capacity check — they are small (36 bytes) and free space by deleting the target blob
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

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `delegation_map` pattern in `storage.cpp`: O(1) indexed sub-database with compound key — direct template for `tombstone_map`
- `IngestError` enum in `engine.h`: add `storage_full` value after `no_delegation`
- `transport.fbs` TransportMsgType enum: add `StorageFull = 23` after `Notification = 22`
- `PeerInfo` struct in `peer_manager.h`: add `peer_is_full` bool flag
- Step 0 pattern already established in `BlobEngine::ingest()` (cheapest check first)

### Established Patterns
- mdbx sub-databases created together in single write transaction during `Storage()` constructor
- Compound keys: `[namespace:32][hash_or_key:32]` used by blobs_map, delegation_map
- `store_blob()` already handles delegation indexing in the same transaction — tombstone indexing follows identical pattern
- Config uses flat JSON with top-level fields parsed in `load_config()`

### Integration Points
- `Storage::has_tombstone_for()` — replace O(n) scan body with O(1) tombstone_map lookup
- `Storage::store_blob()` — add tombstone_map write when `wire::is_tombstone(blob.data)` is true
- `Storage::delete_blob_data()` — add tombstone_map cleanup for tombstone blobs
- `BlobEngine::ingest()` — add Step 0 capacity check before structural validation
- `PeerManager::handle_message()` for Data type — send StorageFull on storage_full rejection
- `SyncProtocol::ingest_blobs()` — check capacity, skip blob on storage_full
- `Config` struct — add `uint64_t max_storage_bytes` field

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 16-storage-foundation*
*Context gathered: 2026-03-09*
