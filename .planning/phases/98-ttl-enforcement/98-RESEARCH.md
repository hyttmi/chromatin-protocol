# Phase 98: TTL Enforcement - Research

**Researched:** 2026-04-08
**Domain:** C++ node TTL enforcement across all query, fetch, sync, and notification paths
**Confidence:** HIGH

## Summary

Phase 98 enforces expiry checks everywhere a blob can exit the node -- query handlers, BlobFetch, sync paths, and notification fan-out. The existing `SyncProtocol::is_blob_expired()` at sync_protocol.cpp:21 provides the core logic but lacks saturating arithmetic and lives in the wrong location. The CONTEXT.md decisions are comprehensive: move the function to codec.h as a free function, add `saturating_expiry()` for overflow-safe timestamp+ttl addition, and wire it into 8 query handlers + 1 BlobFetch handler + 3 sync paths + 1 notification fan-out + 2 engine ingest validations + 1 storage expiry_map calculation.

The work is mechanically straightforward -- each handler already calls `storage_.get_blob()` or `engine_.get_blob()` and has the full `BlobData` available. The expiry check is a single function call inserted between blob retrieval and response encoding. The riskiest changes are: (1) ExistsRequest/BatchExistsRequest upgrade from `has_blob` to `get_blob` (performance regression -- full blob load vs. key-only check), (2) collect_namespace_hashes requiring per-hash `get_blob()` lookups (O(N) full reads where N = namespace blob count), and (3) the Engine ingest path additions (already-expired rejection + tombstone TTL validation).

**Primary recommendation:** Three plans -- (1) core functions + handler enforcement + engine ingest validation, (2) sync path + notification + storage overflow fix, (3) documentation + comprehensive test pass.

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Handler-level checks. Each handler calls a shared `is_blob_expired()` utility after fetching the blob, then treats expired as not-found. No storage or engine API changes.
- D-02: `is_blob_expired` is a free function in `db/wire/codec.h`, colocated with `BlobData` struct. Delete `SyncProtocol::is_blob_expired` and redirect all callers (including sync ingest filter at sync_protocol.cpp:78) to the codec.h version.
- D-03: Handlers get current time via `std::time(nullptr)` inline. No injected clock. Tests use blobs with far-past timestamps to avoid flakiness.
- D-04: Multi-blob handlers (List, BatchRead, TimeRange, BatchExists) compute `now = std::time(nullptr)` once at handler start. Consistent snapshot within a single response.
- D-05: `saturating_expiry(uint64_t timestamp, uint32_t ttl)` function in `db/wire/codec.h` next to `is_blob_expired`. If `ttl == 0`, returns 0 (permanent). If `timestamp + ttl` would overflow, clamps to `UINT64_MAX` (effectively permanent).
- D-06: `is_blob_expired` internally uses `saturating_expiry` -- single source of truth for the addition. `return saturating_expiry(blob.timestamp, blob.ttl) <= now;`
- D-07: `storage.cpp:400` expiry_map key calculation also uses `saturating_expiry`. Overflow -> UINT64_MAX key means blob effectively never expires.
- D-08: ReadRequest (message_dispatcher.cpp:347): `engine_.get_blob()` + `is_blob_expired` check. Expired -> return not-found (0x00).
- D-09: ListRequest (message_dispatcher.cpp:377): For each BlobRef from `get_blob_refs_since`, call `storage_.get_blob()` + `is_blob_expired`. Filter out expired before building response. Capped at 100 results, so max 100 lookups.
- D-10: ExistsRequest (message_dispatcher.cpp:448): Upgrade from `has_blob` to `get_blob` + `is_blob_expired`. Return false for expired.
- D-11: StatsRequest (message_dispatcher.cpp:421): Reports storage reality. No expiry filtering.
- D-12: NamespaceStatsRequest: Same as StatsRequest -- reports storage reality. No filtering.
- D-13: BatchReadRequest (message_dispatcher.cpp:850): After `get_blob` at line 894, check `is_blob_expired`. Expired -> emit status 0x00 (not-found) for that entry.
- D-14: TimeRangeRequest (message_dispatcher.cpp:1025): Add `is_blob_expired` check after `get_blob` at line 1063, before the timestamp range filter.
- D-15: BatchExistsRequest (message_dispatcher.cpp:771): Filter expired for consistency with single ExistsRequest. Upgrade `has_blob` to `get_blob` + `is_blob_expired`.
- D-16: BlobFetch handler (blob_push_manager.cpp:128): After `storage_.get_blob()`, check `is_blob_expired`. Expired -> return not-found (0x01).
- D-17: BlobNotify receiver (blob_push_manager.cpp:113): Upgrade `has_blob` to `get_blob` + `is_blob_expired`. If blob exists but expired, proceed to fetch fresh copy.
- D-18: `on_blob_ingested`: Suppress both BlobNotify (type 59) and Notification (type 21) for expired blobs. Step 0 pattern.
- D-19: `collect_namespace_hashes()`: Filter expired blobs by per-hash `get_blob` + `is_blob_expired` check. O(N) full-blob lookups.
- D-20: `get_blobs_by_hashes()`: Filter expired blobs before sending to peers. Already has full BlobData -- zero extra cost.
- D-21: SyncOrchestrator's individual `engine_.get_blob()` calls at lines 490/520/925/961: Check `is_blob_expired` before sending.
- D-22: `get_blobs_since` (engine.cpp:418): Leave as-is. Only used in tests.
- D-23: Engine::ingest rejects tombstones with TTL > 0. New `IngestError::invalid_ttl` enum value.
- D-24: Keep existing storage-level test for TTL>0 tombstone expiry. ADD new Engine-level test.
- D-25: Engine::ingest rejects regular blobs that are already expired at ingest time. Uses `saturating_expiry`.
- D-26: Debug-level logging when a handler filters an expired blob.
- D-27: No new Prometheus counter for expired query filtering.
- D-28: Update PROTOCOL.md with TTL enforcement section.
- D-29: Update README.md to reflect TTL enforcement behavior.
- D-30: Expired blobs: `timestamp = now - 1000, ttl = 100` (expired 900s ago). Valid blobs: `timestamp = now, ttl = 86400` (1 day).
- D-31: Each handler gets a test proving expired blobs are filtered/rejected.

### Claude's Discretion
- Exact `saturating_expiry` function signature details (as long as semantics match D-05)
- Where in each handler the expiry check goes (as long as it's after blob fetch, before response)
- Plan decomposition (how many plans, ordering)
- Exact log message format for expired blob filtering
- Whether sync hash filtering uses `std::erase_if` or builds a new vector

### Deferred Ideas (OUT OF SCOPE)
- DelegationListRequest: queries delegation_map index, not blob storage. Different mechanism.
- `get_blobs_since` (Engine): test-only API, tests need to see all blobs.
- New Prometheus metrics for expiry filtering.
- SDK changes (REQUIREMENTS.md: "this milestone is C++ node only").

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| TTL-01 | BlobFetch handler checks expiry before serving blobs | D-16: blob_push_manager.cpp:128 has `storage_.get_blob()` already; add `is_blob_expired` check after. Return 0x01 for expired. |
| TTL-02 | All query paths (Read, List, Stats, Exists, BatchRead, TimeRange) filter expired blobs from results | D-08 through D-15: Each handler code path identified with exact line numbers. Stats/NamespaceStats explicitly excluded (report storage reality). ExistsRequest/BatchExistsRequest need `has_blob` -> `get_blob` upgrade. |
| TTL-03 | Expiry timestamp calculation uses saturating arithmetic (no uint64 overflow on timestamp + ttl) | D-05/D-06/D-07: `saturating_expiry()` in codec.h, used by `is_blob_expired()` and storage.cpp:400 expiry_map key calculation. Pattern: `checked_add` in endian.h. |

</phase_requirements>

## Architecture Patterns

### Change Topology

The changes span 8 source files and 2 documentation files:

```
db/wire/codec.h          # New: saturating_expiry(), is_blob_expired() free functions
db/wire/codec.cpp        # (or inline in header -- Claude's discretion)
db/peer/message_dispatcher.cpp  # 6 handler modifications (Read, List, Exists, BatchExists, BatchRead, TimeRange)
db/peer/blob_push_manager.cpp   # 2 modifications (BlobFetch handler, BlobNotify receiver)
db/peer/peer_manager.cpp        # 1 modification (on_blob_ingested notification suppression)
db/sync/sync_protocol.cpp       # 3 modifications (delete is_blob_expired, filter in collect/get_blobs)
db/sync/sync_protocol.h         # Delete is_blob_expired declaration
db/peer/sync_orchestrator.cpp   # 4 modifications (lines 490, 520, 925, 961)
db/storage/storage.cpp          # 1 modification (line 400 overflow fix)
db/engine/engine.h              # 1 modification (new IngestError::invalid_ttl enum value)
db/engine/engine.cpp            # 2 modifications (tombstone TTL check, already-expired check)
db/PROTOCOL.md                  # New TTL enforcement section
db/README.md                    # TTL behavior documentation
```

### Pattern 1: Single-Blob Handler Expiry Check

**What:** Add expiry check between blob retrieval and response encoding.
**When to use:** ReadRequest, BlobFetch, MetadataRequest (if included).

```cpp
// After blob retrieval:
auto blob = engine_.get_blob(ns, hash);  // or storage_.get_blob()
if (blob.has_value()) {
    if (wire::is_blob_expired(*blob, static_cast<uint64_t>(std::time(nullptr)))) {
        spdlog::debug("filtered expired blob in ReadRequest");
        // Treat as not-found
        std::vector<uint8_t> response = {0x00};
        co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                     std::span<const uint8_t>(response), request_id);
        co_return;
    }
    // ... existing found path
}
```

### Pattern 2: Multi-Blob Handler Expiry Filter

**What:** Compute `now` once at handler start, check each blob against it.
**When to use:** ListRequest, BatchReadRequest, TimeRangeRequest, BatchExistsRequest.

```cpp
// At handler start (inside lambda, before loop):
uint64_t now = static_cast<uint64_t>(std::time(nullptr));

// Inside loop, after get_blob:
auto blob = storage_.get_blob(ns, ref.blob_hash);
if (!blob) continue;
if (wire::is_blob_expired(*blob, now)) {
    spdlog::debug("filtered expired blob in ListRequest");
    continue;
}
```

### Pattern 3: has_blob -> get_blob Upgrade

**What:** Replace `has_blob()` (key-only, fast) with `get_blob()` (full blob load) to access timestamp/ttl.
**When to use:** ExistsRequest, BatchExistsRequest, BlobNotify receiver.

```cpp
// Before (ExistsRequest):
bool exists = storage_.has_blob(ns, hash);

// After:
auto blob = storage_.get_blob(ns, hash);
bool exists = blob.has_value() && !wire::is_blob_expired(*blob, static_cast<uint64_t>(std::time(nullptr)));
```

### Pattern 4: Engine Ingest Validation (Step 0)

**What:** Cheapest validation before expensive operations.
**When to use:** Tombstone TTL check, already-expired blob rejection.

```cpp
// In Engine::ingest, after timestamp validation (Step 0c), before structural checks:
// Step 0d: Already-expired blob rejection
if (blob.ttl > 0 && wire::saturating_expiry(blob.timestamp, blob.ttl) <= now) {
    spdlog::warn("Ingest rejected: blob already expired");
    co_return IngestResult::rejection(IngestError::timestamp_rejected,
        "blob already expired");
}

// In Engine::delete_blob, after tombstone format check:
if (delete_request.ttl != 0) {
    co_return IngestResult::rejection(IngestError::invalid_ttl,
        "tombstone must have TTL=0 (permanent)");
}
```

### Anti-Patterns to Avoid
- **Clock injection for production code:** D-03 explicitly says `std::time(nullptr)` inline, no injected clock. Tests use far-past timestamps instead.
- **Filtering in storage layer:** D-01 says handler-level checks only. No storage or engine API changes for filtering.
- **New wire types:** REQUIREMENTS.md says "fix existing, don't add new." BlobFetch expired uses existing 0x01 not-found status.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Overflow-safe expiry calculation | Inline `timestamp + ttl` with manual overflow checks | `saturating_expiry()` in codec.h | Single source of truth; used by `is_blob_expired()` AND storage.cpp expiry_map |
| Blob expiry checking | Per-handler `if (blob.ttl > 0 && blob.timestamp + blob.ttl <= now)` | `wire::is_blob_expired(blob, now)` | Consolidates TTL=0 check + saturating arithmetic in one place |
| Test blob creation with timestamps | Manual BlobData construction + signing | `make_signed_blob(id, "data", ttl, timestamp)` | test_helpers.h already supports ttl and timestamp parameters |

**Key insight:** Every place that computes `timestamp + ttl` MUST use `saturating_expiry()` -- the storage expiry_map key (storage.cpp:400), the sync ingest filter (sync_protocol.cpp:78), and all new handler checks. Three call sites for the same arithmetic.

## Common Pitfalls

### Pitfall 1: ListRequest Over-Fetch After Filtering

**What goes wrong:** ListRequest fetches `limit + 1` refs to detect `has_more`, but after expiry filtering, the returned count may be less than `limit` even though more non-expired blobs exist.
**Why it happens:** `get_blob_refs_since()` returns refs by seq_num order, then expiry filtering removes some. The `has_more` flag becomes unreliable.
**How to avoid:** The current CONTEXT.md approach (D-09) accepts this trade-off: filter the fetched refs, return what's left. This is correct behavior -- the `has_more` flag based on the original ref count is conservative (may say "more" when expired blobs padded the count). The alternative (fetch more to compensate) adds complexity for marginal gain.
**Warning signs:** Test sees `has_more=true` but next page returns 0 results (all expired). This is acceptable edge case behavior.

### Pitfall 2: ExistsRequest Performance Regression

**What goes wrong:** `has_blob()` is a key-only MDBX lookup (fast). `get_blob()` loads the full blob value (slow for large blobs).
**Why it happens:** D-10/D-15 require upgrading to `get_blob()` to access timestamp/ttl fields.
**How to avoid:** Accepted trade-off per CONTEXT.md. BlobRef doesn't carry timestamp/ttl. At current scale (hundreds to low thousands of blobs), the difference is negligible. BatchExistsRequest has a 1024-count cap.
**Warning signs:** Latency spike on ExistsRequest for large blobs. Monitor if this becomes an issue at scale.

### Pitfall 3: Sync collect_namespace_hashes O(N) Full-Blob Lookups

**What goes wrong:** D-19 adds per-hash `get_blob()` calls to `collect_namespace_hashes()`. For a namespace with 10,000 blobs, that's 10,000 full blob loads.
**Why it happens:** BlobRef (from `get_hashes_by_namespace`) doesn't carry timestamp/ttl. Must load each blob to check expiry.
**How to avoid:** Accepted trade-off per CONTEXT.md -- "acceptable at current scale (tens of milliseconds for hundreds-low thousands of blobs)." If this becomes a bottleneck, a future phase could add a timestamp+ttl index to MDBX.
**Warning signs:** Sync rounds taking significantly longer after this change. Safety-net sync interval (600s) provides ample margin.

### Pitfall 4: on_blob_ingested Expiry Check With Zero-TTL Blobs

**What goes wrong:** D-18 suppresses notification for expired blobs. A TTL=0 (permanent) blob should never be suppressed. If `is_blob_expired` is called with `now=0` or `expiry_time=0`, incorrect results could occur.
**Why it happens:** `on_blob_ingested` receives `expiry_time` as a parameter (already computed by the caller). Calling `is_blob_expired` requires the original BlobData which isn't available in this fan-out function.
**How to avoid:** The check in `on_blob_ingested` should use the pre-computed `expiry_time` parameter: `if (expiry_time > 0 && expiry_time <= now)`. For TTL=0 blobs, `expiry_time` is already 0, which skips the check. This is consistent with D-05 (`saturating_expiry` returns 0 for TTL=0).
**Warning signs:** Permanent blobs not triggering notifications after this change.

### Pitfall 5: Sync Ingest Filter Uses Different Clock

**What goes wrong:** `sync_protocol.cpp:78` currently uses the injected clock (`clock_()`) for expiry checks in `ingest_blobs`. After D-02, the new `wire::is_blob_expired` won't have access to the injectable clock -- it expects `uint64_t now` as a parameter.
**How to avoid:** `ingest_blobs` already passes `clock_()` as the `now` parameter. The new `wire::is_blob_expired(blob, now)` call should use the same `now` variable that's already computed at line 72 of sync_protocol.cpp.
**Warning signs:** Sync expiry checks using wall clock instead of test clock, causing test flakiness.

### Pitfall 6: MetadataRequest Not Listed in Decisions

**What goes wrong:** MetadataRequest (message_dispatcher.cpp:704) returns blob metadata including TTL/timestamp. It calls `storage_.get_blob()` and returns data about the blob. If an expired blob is queried, it returns metadata for a blob that ReadRequest would report as not-found.
**Why it happens:** CONTEXT.md D-08 through D-15 enumerate specific handlers but MetadataRequest is not listed. Phase boundary says "every query... path."
**How to avoid:** Include MetadataRequest in the expiry check scope for consistency. Same pattern as ReadRequest (D-08): after `get_blob`, check `is_blob_expired`, return 0x00 if expired. This is a research finding that the planner should include.
**Warning signs:** Inconsistency where `ExistsRequest` returns "not found" but `MetadataRequest` returns metadata for the same expired blob.

## Code Examples

### saturating_expiry and is_blob_expired (codec.h)

```cpp
// In chromatindb::wire namespace, after BlobData struct:

/// Compute expiry timestamp with saturating arithmetic.
/// Returns 0 for permanent blobs (ttl == 0).
/// Clamps to UINT64_MAX on overflow (effectively permanent).
inline uint64_t saturating_expiry(uint64_t timestamp, uint32_t ttl) {
    if (ttl == 0) return 0;  // Permanent
    uint64_t ttl64 = static_cast<uint64_t>(ttl);
    if (timestamp > UINT64_MAX - ttl64) return UINT64_MAX;  // Overflow -> clamp
    return timestamp + ttl64;
}

/// Check if a blob has expired.
/// Permanent blobs (ttl == 0) never expire.
/// Uses saturating arithmetic for overflow safety (TTL-03).
inline bool is_blob_expired(const BlobData& blob, uint64_t now) {
    if (blob.ttl == 0) return false;  // Permanent
    return saturating_expiry(blob.timestamp, blob.ttl) <= now;
}
```

### Storage expiry_map overflow fix (storage.cpp:400)

```cpp
// Before:
uint64_t expiry_time = static_cast<uint64_t>(blob.timestamp) +
                       static_cast<uint64_t>(blob.ttl);

// After:
uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
```

### Engine tombstone TTL rejection (engine.cpp, delete_blob)

```cpp
// After tombstone format validation, before namespace check:
if (delete_request.ttl != 0) {
    spdlog::warn("Delete rejected: tombstone must have TTL=0 (permanent)");
    co_return IngestResult::rejection(IngestError::invalid_ttl,
        "tombstone must have TTL=0 (permanent)");
}
```

### IngestError enum addition (engine.h)

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
    quota_exceeded,
    timestamp_rejected,
    invalid_ttl         // NEW: tombstone with TTL > 0
};
```

### Test pattern for expired blob filtering

```cpp
TEST_CASE("ReadRequest returns not-found for expired blob", "[peer][read][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store an expired blob (timestamp 1000s ago, TTL 100s -> expired 900s ago)
    auto expired_blob = make_signed_blob(server_id, "expired-data", 100, now - 1000);
    auto r1 = run_async(pool, eng.ingest(expired_blob));
    REQUIRE(r1.accepted);
    auto expired_hash = r1.ack->blob_hash;

    // Store a valid blob for control
    auto valid_blob = make_signed_blob(server_id, "valid-data", 86400, now);
    auto r2 = run_async(pool, eng.ingest(valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    // ... PeerManager setup, connect client, send ReadRequest for both hashes
    // Assert: expired_hash -> 0x00 (not-found), valid_hash -> 0x01 (found)
}
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3 (via FetchContent) |
| Config file | db/CMakeLists.txt (line 220) |
| Quick run command | `cd build && ./chromatindb_tests "[ttl]"` |
| Full suite command | `cd build && ./chromatindb_tests` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| TTL-01 | BlobFetch returns not-found for expired blob | unit (handler-level) | `./chromatindb_tests "[ttl][blobfetch]"` | Wave 0 |
| TTL-02 | ReadRequest filters expired blobs | unit (handler-level) | `./chromatindb_tests "[ttl][read]"` | Wave 0 |
| TTL-02 | ListRequest filters expired blobs | unit (handler-level) | `./chromatindb_tests "[ttl][list]"` | Wave 0 |
| TTL-02 | ExistsRequest returns false for expired | unit (handler-level) | `./chromatindb_tests "[ttl][exists]"` | Wave 0 |
| TTL-02 | BatchExistsRequest filters expired | unit (handler-level) | `./chromatindb_tests "[ttl][batchexists]"` | Wave 0 |
| TTL-02 | BatchReadRequest returns not-found for expired | unit (handler-level) | `./chromatindb_tests "[ttl][batchread]"` | Wave 0 |
| TTL-02 | TimeRangeRequest filters expired | unit (handler-level) | `./chromatindb_tests "[ttl][timerange]"` | Wave 0 |
| TTL-03 | saturating_expiry handles overflow | unit (pure function) | `./chromatindb_tests "[ttl][saturating]"` | Wave 0 |
| TTL-03 | is_blob_expired uses saturating arithmetic | unit (pure function) | `./chromatindb_tests "[ttl][saturating]"` | Wave 0 |
| D-23 | Tombstone TTL>0 rejected at ingest | unit (engine-level) | `./chromatindb_tests "[engine][ttl]"` | Wave 0 |
| D-25 | Already-expired blob rejected at ingest | unit (engine-level) | `./chromatindb_tests "[engine][ttl]"` | Wave 0 |
| D-19 | collect_namespace_hashes filters expired | unit (sync-level) | `./chromatindb_tests "[sync][ttl]"` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ./chromatindb_tests "[ttl]"` (all TTL-related tests)
- **Per wave merge:** `cd build && ./chromatindb_tests` (full suite)
- **Phase gate:** Full suite green + ASAN/TSAN/UBSAN clean before verification

### Wave 0 Gaps
- [ ] `db/tests/wire/test_codec.cpp` -- saturating_expiry and is_blob_expired unit tests (pure function tests, no I/O)
- [ ] `db/tests/peer/test_peer_manager.cpp` -- 7 new handler TTL test cases (Read, List, Exists, BatchExists, BatchRead, TimeRange, BlobFetch -- following existing handler test patterns in same file)
- [ ] `db/tests/engine/test_engine.cpp` -- 2 new ingest validation tests (tombstone TTL>0, already-expired blob)
- [ ] `db/tests/sync/test_sync_protocol.cpp` -- update existing tests + new collect_namespace_hashes filtering test
- [ ] No new test files needed -- all tests go in existing files following established patterns

## Open Questions

1. **MetadataRequest scope**
   - What we know: MetadataRequest (message_dispatcher.cpp:704) returns blob metadata via `storage_.get_blob()`. It is not listed in CONTEXT.md decisions D-08 through D-15.
   - What's unclear: Whether this is intentionally excluded or an oversight. The phase boundary says "every query... path" but the handler list doesn't include it.
   - Recommendation: Include MetadataRequest in TTL enforcement for consistency. Same pattern as ReadRequest. If excluded, a client could discover blob existence through metadata even when Read/Exists say "not found."

## Sources

### Primary (HIGH confidence)
- Direct source code inspection of all files listed in CONTEXT.md canonical references
- `db/wire/codec.h` -- BlobData struct, target for new functions
- `db/sync/sync_protocol.cpp:21` -- existing `is_blob_expired` implementation
- `db/peer/message_dispatcher.cpp` -- all 8 query handler implementations
- `db/peer/blob_push_manager.cpp` -- BlobFetch and BlobNotify handlers
- `db/peer/peer_manager.cpp:366` -- on_blob_ingested facade
- `db/engine/engine.cpp:125-142` -- existing timestamp validation (Step 0c)
- `db/engine/engine.h:25` -- IngestError enum
- `db/storage/storage.cpp:400` -- expiry_map key calculation
- `db/util/endian.h:138-150` -- checked_add/checked_mul reference patterns
- `db/tests/peer/test_peer_manager.cpp` -- existing handler test patterns (e.g., ExistsRequest at line 2516)
- `db/tests/sync/test_sync_protocol.cpp:95` -- existing is_blob_expired tests
- `db/tests/test_helpers.h` -- make_signed_blob with ttl/timestamp parameters

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- pure C++ changes to existing codebase, no new dependencies
- Architecture: HIGH -- all handler patterns verified by direct code inspection
- Pitfalls: HIGH -- specific line numbers confirmed, edge cases identified from code reading

**Research date:** 2026-04-08
**Valid until:** 2026-05-08 (stable codebase, no external dependencies)
