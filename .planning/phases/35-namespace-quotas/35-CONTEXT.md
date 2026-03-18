# Phase 35: Namespace Quotas - Context

**Gathered:** 2026-03-18
**Status:** Ready for planning

<domain>
## Phase Boundary

Node operators can limit per-namespace resource usage with byte and blob count caps enforced atomically at ingest. Global defaults with per-namespace overrides. Quota tracking via materialized aggregates in a libmdbx sub-database for O(1) write-path enforcement. SIGHUP-reloadable. Requirements: QUOTA-01, QUOTA-02, QUOTA-03, QUOTA-04.

</domain>

<decisions>
## Implementation Decisions

### Quota configuration model
- Global defaults + per-namespace override map in config.json
- `namespace_quota_bytes` and `namespace_quota_count` as top-level fields (global defaults)
- `namespace_quotas` map keyed by full 64-char hex namespace hash, values are `{ "max_bytes": N, "max_count": N }`
- 0 = unlimited (disabled), consistent with existing `max_storage_bytes` pattern
- Partial overrides allowed: override only what you specify, inherit the rest from global defaults
- Per-namespace override with 0 explicitly exempts that namespace from the global default
- Namespace keys validated as 64-char hex, consistent with `allowed_keys` and `sync_namespaces`

### Quota exceeded signaling
- New `TransportMsgType_QuotaExceeded = 24` wire message type (distinct from StorageFull)
- New `IngestError::quota_exceeded` enum variant (distinct from `storage_full`)
- PeerManager pattern-matches on error variant to send the correct wire message
- Writers can distinguish "node is full" (StorageFull) from "this namespace hit its limit" (QuotaExceeded)

### Ingest pipeline placement
- Quota check at Step 2a: after namespace ownership/delegation validation (Step 2), before dedup check (Step 2.5)
- Early rejection avoids wasting dedup lookup and ML-DSA-87 crypto on over-quota blobs
- Final enforcement in the mdbx write transaction inside store_blob (prevents check-then-act race across co_await)

### What counts toward quota
- **Byte quota**: encoded FlatBuffer size (`wire::encode_blob()` output size) — includes data, pubkey, signature, metadata. Close proxy for actual storage cost
- **Count quota**: simple blob count (1 per blob stored)
- **Tombstones exempt**: consistent with Phase 16 global capacity exemption. Tombstones are tiny (36 bytes), serve critical deletion function, and owners must always be able to delete at any quota level
- **Delegation blobs count**: regular blobs with special data format, impact is negligible (~100 bytes each)

### Over-quota behavior on SIGHUP
- Reject new writes only — existing data stays intact, no forced eviction
- Consistent with how `max_storage_bytes` works (doesn't delete existing data)
- SIGHUP reloads quota config and new limits take effect immediately
- Consistent with other SIGHUP-reloadable fields (allowed_keys, trusted_peers, full_resync_interval, cursor config)

### Quota reclaim and accuracy
- Immediate in-transaction reclaim: decrement aggregate in the same mdbx transaction that removes blob (delete or expiry)
- Full aggregate rebuild on startup: scan all namespaces, compute aggregates from actual stored blobs, write to quota sub-db
- Guarantees accuracy after corruption, manual edits, or first run after upgrade
- Consistent with existing startup patterns (DARE unencrypted scan, cursor cleanup scan)

### Claude's Discretion
- Quota sub-database key/value encoding details (likely `[namespace:32] -> [bytes_be:8][count_be:8]`)
- How BlobEngine receives quota config (constructor param, reload method, or Config reference)
- Sync-received blob quota enforcement (consistent with Data message path)
- Metrics counters for quota state (quota_checks, quota_rejections per namespace or global)
- Log levels and format for quota events
- Exact startup rebuild implementation (cursor scan pattern, progress logging)

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `IngestError` enum in `engine.h`: add `quota_exceeded` after `storage_full`
- `TransportMsgType` in `transport.fbs`: add `QuotaExceeded = 24` after `StorageFull = 23`
- `wire::encode_blob()`: already used in ingest path, output size is the byte quota measurement
- Step 0b capacity check pattern in `BlobEngine::ingest()`: quota check follows identical pattern at Step 2a
- `PeerManager::handle_message()` StorageFull send pattern: template for QuotaExceeded send
- libmdbx sub-database pattern: 6 existing sub-dbs, 7th (quota) follows established pattern
- Config flat struct + `load_config()` JSON parsing: extend with new fields
- SIGHUP reload chain: `handle_sighup()` → `reload_config()` — extend to reload quota limits

### Established Patterns
- Flat composite keys: `[namespace:32][hash:32]` (blobs, delegation, tombstone)
- Mdbx transactions: atomic multi-map writes in `store_blob()` (blobs + seq + expiry + delegation + tombstone)
- Config reload: mutable members following `rate_limit_` pattern for SIGHUP
- Startup scans: DARE unencrypted-data scan, cursor cleanup scan
- Step 0 fail-fast: cheapest check first in ingest pipeline

### Integration Points
- `Storage::Impl` constructor: open 7th sub-database (quota aggregates)
- `Storage::store_blob()`: increment aggregate in write transaction
- `Storage::delete_blob_data()`: decrement aggregate in write transaction
- `Storage::run_expiry_scan()`: decrement aggregate for each expired blob
- `BlobEngine::ingest()`: add Step 2a quota check after namespace validation
- `BlobEngine` constructor or reload: receive quota config (global + per-namespace map)
- `PeerManager::handle_message()`: send QuotaExceeded on quota_exceeded error
- `SyncProtocol::ingest_blobs()`: quota enforcement consistent with Data message path
- `Config` struct: add `namespace_quota_bytes`, `namespace_quota_count`, `namespace_quotas` map
- `PeerManager::handle_sighup()`: reload quota limits
- Startup: full aggregate rebuild scan before accepting connections

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 35-namespace-quotas*
*Context gathered: 2026-03-18*
