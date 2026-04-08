# Phase 98: TTL Enforcement - Context

**Gathered:** 2026-04-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Enforce expiry checks in every query, fetch, sync, and notification path so that no expired blob is ever served, advertised, or accepted. Additionally enforce that tombstones must always be TTL=0 (permanent), reject already-expired blobs at ingest, apply saturating arithmetic to all expiry timestamp calculations, and document all TTL enforcement behavior in PROTOCOL.md and README.md.

Requirements: TTL-01, TTL-02, TTL-03 (expanded scope covers all code paths, not just the 3 explicit requirements)

</domain>

<decisions>
## Implementation Decisions

### Expiry Check Placement (TTL-01, TTL-02)
- **D-01:** Handler-level checks. Each handler calls a shared `is_blob_expired()` utility after fetching the blob, then treats expired as not-found. No storage or engine API changes.
- **D-02:** `is_blob_expired` is a free function in `db/wire/codec.h`, colocated with `BlobData` struct. Delete `SyncProtocol::is_blob_expired` and redirect all callers (including sync ingest filter at sync_protocol.cpp:78) to the codec.h version.
- **D-03:** Handlers get current time via `std::time(nullptr)` inline. No injected clock. Tests use blobs with far-past timestamps to avoid flakiness.
- **D-04:** Multi-blob handlers (List, BatchRead, TimeRange, BatchExists) compute `now = std::time(nullptr)` once at handler start. Consistent snapshot within a single response.

### Saturating Arithmetic (TTL-03)
- **D-05:** `saturating_expiry(uint64_t timestamp, uint32_t ttl)` function in `db/wire/codec.h` next to `is_blob_expired`. If `ttl == 0`, returns 0 (permanent). If `timestamp + ttl` would overflow, clamps to `UINT64_MAX` (effectively permanent — writer wanted a long-lived blob).
- **D-06:** `is_blob_expired` internally uses `saturating_expiry` — single source of truth for the addition. `return saturating_expiry(blob.timestamp, blob.ttl) <= now;`
- **D-07:** `storage.cpp:400` expiry_map key calculation also uses `saturating_expiry`. Overflow -> UINT64_MAX key means blob effectively never expires (consistent with handler behavior).

### Query Handler Filtering (TTL-02 expanded)
- **D-08:** ReadRequest (message_dispatcher.cpp:347): `engine_.get_blob()` + `is_blob_expired` check. Expired -> return not-found (0x00).
- **D-09:** ListRequest (message_dispatcher.cpp:377): For each BlobRef from `get_blob_refs_since`, call `storage_.get_blob()` + `is_blob_expired`. Filter out expired before building response. Capped at 100 results, so max 100 lookups — acceptable cost.
- **D-10:** ExistsRequest (message_dispatcher.cpp:448): Upgrade from `has_blob` to `get_blob` + `is_blob_expired`. Return false for expired.
- **D-11:** StatsRequest (message_dispatcher.cpp:421): Reports storage reality. No expiry filtering — expired blobs consume storage until scanner removes them.
- **D-12:** NamespaceStatsRequest: Same as StatsRequest — reports storage reality. No filtering.
- **D-13:** BatchReadRequest (message_dispatcher.cpp:850): After `get_blob` at line 894, check `is_blob_expired`. Expired -> emit status 0x00 (not-found) for that entry. Truncation flag unchanged (only fires on size cap).
- **D-14:** TimeRangeRequest (message_dispatcher.cpp:1025): Add `is_blob_expired` check after `get_blob` at line 1063, before the timestamp range filter.
- **D-15:** BatchExistsRequest (message_dispatcher.cpp:771): Filter expired for consistency with single ExistsRequest. Upgrade `has_blob` to `get_blob` + `is_blob_expired`.

### BlobFetch Handler (TTL-01)
- **D-16:** BlobFetch handler (blob_push_manager.cpp:128): After `storage_.get_blob()`, check `is_blob_expired`. Expired -> return not-found (0x01). No new status bytes — REQUIREMENTS.md says no new wire types.

### BlobNotify Receiver
- **D-17:** BlobNotify receiver (blob_push_manager.cpp:113): Upgrade `has_blob` to `get_blob` + `is_blob_expired`. If blob exists but expired, proceed to fetch a fresh copy instead of skipping. Prevents stale expired blobs from blocking re-acquisition.

### Notification Suppression
- **D-18:** `on_blob_ingested`: Suppress both BlobNotify (type 59) to peers AND Notification (type 21) to subscribed clients for expired blobs. Step 0 pattern — cheapest check before fan-out. Avoids wasted BlobFetch round-trips and notifications for blobs clients can't read.

### Sync Path Filtering
- **D-19:** `collect_namespace_hashes()`: Filter expired blobs by per-hash `get_blob` + `is_blob_expired` check. O(N) full-blob lookups where N = hashes in namespace. Acceptable cost at current scale (tens of milliseconds for hundreds-low thousands of blobs). Correctness over micro-optimization.
- **D-20:** `get_blobs_by_hashes()`: Filter expired blobs before sending to peers. Already has full BlobData — zero extra cost.
- **D-21:** SyncOrchestrator's individual `engine_.get_blob()` calls at lines 490/520/925/961: Check `is_blob_expired` before sending. Covers TOCTOU window between hash collection and Phase C transfer.
- **D-22:** `get_blobs_since` (engine.cpp:418): Leave as-is. Only used in tests for verification — tests need to see all stored blobs including expired.

### Tombstone Enforcement
- **D-23:** Engine::ingest rejects tombstones (delete requests) with TTL > 0. Tombstones must always be permanent (TTL=0). New `IngestError::invalid_ttl` enum value. Cheap check — one int compare before signature verification.
- **D-24:** Keep existing storage-level test for TTL>0 tombstone expiry (test_storage.cpp:1203) — validates scanner handles edge case if such data exists. ADD new Engine-level test proving TTL>0 tombstones are rejected at ingest.

### Already-Expired Blob Rejection
- **D-25:** Engine::ingest rejects regular blobs that are already expired at ingest time. After timestamp range validation, if `ttl > 0 && saturating_expiry(timestamp, ttl) <= now`, reject with `IngestError::timestamp_rejected` and detail "blob already expired". Saves storage writes, scanner work, and sync bandwidth.

### Logging
- **D-26:** Debug-level logging when a handler filters an expired blob: `spdlog::debug("filtered expired blob in {handler}")`. Zero overhead in production (spdlog compiles out debug). Helps diagnose "missing blob" reports.
- **D-27:** No new Prometheus counter for expired query filtering. Debug logging is sufficient. Metrics are out of scope for a hardening phase.

### Documentation
- **D-28:** Update PROTOCOL.md with TTL enforcement section: expired blobs not served through any query/fetch path, tombstones must be TTL=0, already-expired blobs rejected at ingest, saturating arithmetic for expiry calculation.
- **D-29:** Update README.md to reflect TTL enforcement behavior and make tombstone TTL=0 requirement explicit and prominent.

### Test Strategy
- **D-30:** Expired blobs: `timestamp = now - 1000, ttl = 100` (expired 900s ago). Valid blobs: `timestamp = now, ttl = 86400` (1 day). Matches existing test_daemon.cpp pattern.
- **D-31:** Each handler gets a test proving expired blobs are filtered/rejected. All tests pass under ASAN/TSAN/UBSAN.

### Claude's Discretion
- Exact `saturating_expiry` function signature details (as long as semantics match D-05)
- Where in each handler the expiry check goes (as long as it's after blob fetch, before response)
- Plan decomposition (how many plans, ordering)
- Exact log message format for expired blob filtering
- Whether sync hash filtering uses `std::erase_if` or builds a new vector

### Out of Scope
- DelegationListRequest: queries delegation_map index, not blob storage. Delegations are permanent (revoked via tombstone). Different mechanism.
- `get_blobs_since` (Engine): test-only API, tests need to see all blobs.
- New Prometheus metrics for expiry filtering.
- SDK changes (REQUIREMENTS.md: "this milestone is C++ node only").

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Query Handlers (TTL-02 targets)
- `db/peer/message_dispatcher.cpp` -- ReadRequest (line 347), ListRequest (line 377), StatsRequest (line 421), ExistsRequest (line 448), BatchExistsRequest (line 771), DelegationListRequest (line 810), BatchReadRequest (line 850), TimeRangeRequest (line 1025), NamespaceStatsRequest (line 653)

### BlobFetch & BlobNotify (TTL-01 target)
- `db/peer/blob_push_manager.cpp` -- handle_blob_fetch (line 128), BlobNotify receiver has_blob check (line 113), on_blob_notify
- `db/peer/peer_manager.cpp` -- on_blob_ingested notification fan-out

### Sync Path
- `db/sync/sync_protocol.cpp` -- is_blob_expired (line 21, to be deleted and moved), collect_namespace_hashes (line 31), get_blobs_by_hashes (line 44), ingest_received_blobs (line 77)
- `db/peer/sync_orchestrator.cpp` -- engine_.get_blob calls at lines 490, 520, 925, 961

### Storage
- `db/storage/storage.cpp` -- expiry_time calculation at line 400 (overflow fix target), get_blob (line 461), has_blob (line 498)

### Expiry Function Home
- `db/wire/codec.h` -- BlobData struct, target for is_blob_expired and saturating_expiry free functions
- `db/wire/codec.cpp` -- implementation if needed (prefer inline in header)

### Engine Ingest
- `db/engine/engine.cpp` -- ingest blob path (timestamp validation at line 132-140), ingest delete path (line 300+), IngestError enum
- `db/engine/engine.h` -- IngestError enum (line 25), needs new invalid_ttl value

### Phase 95 Utilities (reuse)
- `db/util/endian.h` -- checked_add/checked_mul (reference pattern for saturating arithmetic)
- `db/tests/test_helpers.h` -- make_signed_blob, TempDir, listening_address

### Documentation
- `db/PROTOCOL.md` -- TTL enforcement section to add, tombstone TTL=0 to document
- `db/README.md` -- TTL behavior and tombstone TTL=0 to make explicit

### Requirements
- `.planning/REQUIREMENTS.md` -- TTL-01, TTL-02, TTL-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `SyncProtocol::is_blob_expired()` at sync_protocol.cpp:21 -- logic to migrate to codec.h (add saturating arithmetic)
- `checked_add` in endian.h -- reference pattern for overflow-safe arithmetic
- `make_signed_blob` in test_helpers.h -- blob construction for tests (accepts ttl, timestamp params)
- `wire::encode_blob` / `wire::decode_blob` -- blob serialization already used by all handlers

### Established Patterns
- Step 0 pattern: cheapest validation before expensive ops (from Phase 97)
- Handler pattern: `co_spawn` + lambda + `storage_.get_blob()` + response encoding
- `spdlog::debug()` for diagnostic logging (compiled out in release)
- `IngestError` enum + `IngestResult::rejection()` for ingest validation
- Timestamps are SECONDS everywhere. `expiry = timestamp + ttl`.

### Integration Points
- 8 query handlers in message_dispatcher.cpp need expiry checks
- 1 BlobFetch handler in blob_push_manager.cpp needs expiry check
- 1 BlobNotify receiver in blob_push_manager.cpp needs has_blob -> get_blob upgrade
- 1 notification fan-out in peer_manager.cpp needs expiry suppression
- 3 sync paths need expiry filtering (collect_hashes, get_blobs_by_hashes, SyncOrchestrator)
- 1 storage path needs saturating arithmetic (expiry_map key)
- 2 Engine ingest paths need new validation (already-expired, tombstone TTL)
- 2 documentation files need TTL enforcement sections

</code_context>

<specifics>
## Specific Ideas

- `is_blob_expired` must use `saturating_expiry` internally -- single source of truth for timestamp + ttl overflow
- BlobFetch returns 0x01 (not-found) for expired blobs, no new status bytes
- BatchRead: expired blobs emit status 0x00 (not-found) entries, don't skip entirely
- Tombstone TTL=0 requirement must be PROMINENT in PROTOCOL.md and README.md -- "make sure it's known by everyone"
- ListRequest expiry filtering requires per-ref `get_blob()` call (BlobRef has no timestamp/ttl). Capped at 100 results.
- Sync hash collection filtering requires per-hash `get_blob()` call. O(N) full-blob lookups, acceptable at current scale.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope (expanded scope was explicitly confirmed)

</deferred>

---

*Phase: 98-ttl-enforcement*
*Context gathered: 2026-04-08*
