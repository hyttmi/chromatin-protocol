# Technology Stack: v1.4.0 Extended Query Suite

**Project:** chromatindb
**Researched:** 2026-03-26
**Scope:** Stack additions/changes for 11 new query/response message types
**Overall confidence:** HIGH

## Executive Answer

**No new dependencies needed.** The existing stack covers every requirement for all 11 new query types. This is pure application-level code using proven patterns already in the codebase.

The research validates that:
- libmdbx cursor `lower_bound()` + scan already powers time-range queries (used by expiry scanner and seq-based queries)
- Batch operations are just loops over existing point lookups -- no new primitives
- Health/status queries read from in-memory metrics and `mdbx::env::info()` -- zero library additions
- Delegation listing uses the same cursor scan pattern as `list_namespaces()`
- All wire encoding follows the existing binary format (big-endian integers + raw bytes)

## Existing Stack (Validated, Unchanged)

These are locked decisions from 13 shipped milestones. DO NOT change.

| Technology | Version | Purpose |
|------------|---------|---------|
| C++20 | GCC 14+ | Language standard, coroutines |
| CMake | 3.20+ | Build system |
| liboqs | 0.15.0 | ML-DSA-87, ML-KEM-1024, SHA3-256 |
| libsodium | latest (cmake wrapper) | ChaCha20-Poly1305, HKDF-SHA256 |
| libmdbx | v0.13.11 | ACID storage, 7 sub-databases |
| FlatBuffers | v25.2.10 | Wire format for transport envelope |
| Standalone Asio | 1.38.0 | Networking, C++20 coroutines, thread_pool |
| xxHash | (via reconciliation) | XXH3 sync fingerprints |
| Catch2 | v3.7.1 | Unit testing |
| spdlog | v1.15.1 | Structured logging |
| nlohmann/json | v3.11.3 | Config parsing |

## New Dependencies Required

**None.**

## Analysis by Query Type

### 1. TimeRange Query (blobs in namespace within timestamp window)

**Stack requirement:** libmdbx cursor range scan on seq_map, then filter by timestamp.

**Why existing stack is sufficient:**
- The sequence sub-database (`seq_map`) already supports `lower_bound()` seeks. This is the same primitive used by `get_blobs_by_seq()`, `get_blob_refs_since()`, and `list_namespaces()`.
- Blobs are stored as DARE-encrypted FlatBuffers in `blobs_map`. Each blob contains a `timestamp` field (microseconds since epoch). To filter by time range, the pattern is: scan seq entries in a namespace, fetch+decrypt each blob, check `timestamp >= start && timestamp <= end`.
- There is **no dedicated timestamp index** in storage. Creating one would add a new sub-database. However, for v1.4.0, the scan-and-filter approach is correct because:
  1. Timestamps are writer-controlled and in the signed blob data -- they cannot be indexed separately without decryption.
  2. The sequence index provides natural ordering that correlates with time (blobs arrive roughly in timestamp order).
  3. A dedicated timestamp index is a v2.0+ optimization if profiling shows scan-and-filter is too slow. YAGNI.

**Implementation approach:** New `Storage::get_blobs_in_time_range(ns, start_us, end_us, limit)` that uses the existing seq_map cursor scan + per-blob decrypt + timestamp filter. Capped at a configurable limit (e.g., 100) to bound response size.

**Confidence:** HIGH -- libmdbx cursor operations are battle-tested in this codebase (12+ uses of `lower_bound()` in storage.cpp).

### 2. BlobMetadata / Metadata Query (metadata without payload)

**Stack requirement:** Decrypt blob, extract metadata fields, skip `data` field in response.

**Why existing stack is sufficient:**
- `get_blob()` already decrypts and returns a full `wire::BlobData` struct containing `namespace_id`, `pubkey`, `data`, `ttl`, `timestamp`, `signature`.
- Metadata extraction is selecting fields from the decoded struct. No library needed.
- The encrypted-at-rest DARE envelope means we must decrypt the full blob to read any field (AEAD is all-or-nothing). This is inherent to the security model, not a stack limitation.
- Response wire format: binary encoding of `[timestamp_be:8][ttl_be:4][data_size_be:4][seq_num_be:8][signer_pubkey_hash:32]` -- same pattern as ExistsResponse/StatsResponse.

**Note:** BlobMetadata and Metadata appear to be the same concept (metadata without data transfer). The roadmap should merge these into a single message type pair unless there is a distinction in scope.

**Confidence:** HIGH -- uses existing `get_blob()` + field selection.

### 3. BatchExists Query (check multiple blob hashes)

**Stack requirement:** Loop of `has_blob()` calls within a single handler.

**Why existing stack is sufficient:**
- `Storage::has_blob()` is an O(1) key-existence check in `blobs_map` (MDBX btree seek, no value read).
- Batch = iterate over N hashes, call `has_blob()` for each, accumulate results.
- Bounded by a max batch size (e.g., 64 to match `MAX_HASHES_PER_REQUEST` from sync protocol).
- No transactional consistency needed across individual checks (read snapshot from MVCC is already consistent within a single `start_read()` transaction).
- For efficiency: open a single read transaction, call `txn.get()` in a loop rather than per-call `has_blob()`. This avoids per-call transaction overhead. This is a Storage-level optimization, not a new library.

**Implementation approach:** New `Storage::batch_has_blobs(ns, hashes) -> vector<bool>` using a single read transaction. Response: `[count_be:2][exists:1][hash:32][exists:1][hash:32]...`

**Confidence:** HIGH -- trivial composition of existing MDBX operations.

### 4. BatchRead Query (fetch multiple small blobs)

**Stack requirement:** Loop of `get_blob()` calls within a single handler.

**Why existing stack is sufficient:**
- Same pattern as BatchExists but with decrypt + encode for each found blob.
- Must be bounded (e.g., max 32 blobs, max 1 MiB total response) to prevent DoS.
- Single read transaction for consistency.
- Encryption overhead: each blob requires AEAD decryption. For N small blobs, this is N decrypts -- all using the same key. No parallelism needed for small N.

**Implementation approach:** New `Storage::batch_get_blobs(ns, hashes) -> vector<optional<BlobData>>` using single read transaction. Response: `[count_be:2][found:1][encoded_blob_len_be:4][flatbuffer_blob]...`

**Confidence:** HIGH -- composition of existing operations.

### 5. DelegationList Query (active delegations for namespace)

**Stack requirement:** libmdbx cursor scan of `delegation_map` for a namespace prefix.

**Why existing stack is sufficient:**
- `delegation_map` keys are `[namespace:32][delegate_pk_hash:32]`.
- To list all delegations for a namespace: `lower_bound([namespace][0x00...00])`, scan forward while prefix matches namespace, collect delegate_pk_hash values.
- This is the **exact same pattern** as `list_namespaces()` uses on `seq_map` (seek to prefix, scan, break on prefix change).
- Values in delegation_map are `[delegation_blob_hash:32]` -- can include in response for the client to fetch the full delegation blob if needed.

**Implementation approach:** New `Storage::list_delegations(ns) -> vector<{delegate_pk_hash, delegation_blob_hash}>`. Response: `[count_be:2][delegate_pk_hash:32][delegation_blob_hash:32]...`

**Confidence:** HIGH -- existing cursor pattern.

### 6. NamespaceList Query (all namespaces on node)

**Stack requirement:** Already implemented.

**Why existing stack is sufficient:**
- `Storage::list_namespaces()` already exists and returns `vector<NamespaceInfo>` with namespace_id and latest_seq_num.
- `BlobEngine::list_namespaces()` wraps it.
- The existing `NamespaceList` message type (11) is for sync protocol. The new client-facing version needs a different type ID but the data source is the same.

**Implementation approach:** New message type pair that calls existing `engine_.list_namespaces()`. Response: `[count_be:4][namespace_id:32][latest_seq_be:8]...`

**Confidence:** HIGH -- wraps existing method.

### 7. NamespaceStats Query (per-namespace count, bytes, quota)

**Stack requirement:** Already partially implemented.

**Why existing stack is sufficient:**
- StatsRequest/StatsResponse (types 35-36) already returns `[blob_count:8][total_bytes:8][quota_bytes:8]`.
- NamespaceStats extends this with additional fields: quota count limit, tombstone count, delegation count.
- Tombstone count: cursor scan of `tombstone_map` with namespace prefix (same pattern as DelegationList).
- Delegation count: cursor scan of `delegation_map` with namespace prefix.
- Alternatively, use `mdbx::txn::get_map_stat()` for global counts, or add per-namespace counters.

**Question for roadmap:** Is NamespaceStats distinct from StatsRequest, or an extension? If it only adds fields, consider extending StatsResponse rather than adding a new type pair. This reduces message type count.

**Confidence:** HIGH -- uses existing quota API + cursor scans.

### 8. PeerInfo Query (detailed peer connection info)

**Stack requirement:** Read from in-memory `PeerInfo` structs in PeerManager.

**Why existing stack is sufficient:**
- `PeerManager::peers_` (deque of PeerInfo) already contains: address, is_bootstrap, syncing, peer_is_full, subscribed_namespaces, etc.
- Connection provides: remote_address, is_initiator, is_uds, is_authenticated.
- This is a pure in-memory read, serialized to binary wire format.
- No storage access needed.

**Implementation approach:** Inline handler (no co_spawn needed -- pure memory read). Response: `[peer_count_be:2][per-peer: address_len:2, address, is_bootstrap:1, is_syncing:1, is_full:1, ...]`

**Confidence:** HIGH -- reads existing in-memory state.

### 9. Health Query (liveness/readiness check)

**Stack requirement:** Return a fixed response indicating the node is alive.

**Why existing stack is sufficient:**
- Liveness: if the node can process the message, it is alive. Response = `[status:1]` (0x01 = healthy).
- Readiness: add simple checks: storage accessible (try `env.info()`), at least one peer connected, etc.
- No external health check library needed. The protocol IS the health check mechanism.
- This is the simplest handler in the entire milestone -- likely 10-15 lines of code.

**Implementation approach:** Inline handler. Response: `[status:1][uptime_be:8][peer_count_be:4][storage_ok:1]`

**Confidence:** HIGH -- trivial.

### 10. StorageStatus Query (disk usage, quota headroom, tombstone counts)

**Stack requirement:** Read from existing Storage and MDBX env info APIs.

**Why existing stack is sufficient:**
- `Storage::used_bytes()` -- mmap geometry (already exists)
- `Storage::used_data_bytes()` -- actual B-tree occupancy (already exists)
- `mdbx::env::info()` -- provides `mi_geo.current`, `mi_geo.upper`, page counts
- `mdbx::txn::get_map_stat()` -- per-sub-database entry counts (used in integrity_scan)
- Config `max_storage_bytes_` -- total capacity limit
- Tombstone count: `get_map_stat(tombstone_map).ms_entries`
- All data sources already exist in the codebase.

**Implementation approach:** Coroutine handler (storage access). Response: `[used_bytes_be:8][data_bytes_be:8][max_bytes_be:8][tombstone_count_be:8][blob_count_be:8][namespace_count_be:4]`

**Confidence:** HIGH -- uses existing MDBX info/stat APIs.

### 11. FlatBuffers Schema Update (transport.fbs)

**Stack requirement:** Add new enum values to `TransportMsgType`.

**Why existing stack is sufficient:**
- FlatBuffers enums are backwards-compatible when only adding values at the end.
- The existing `TransportMsgType_MAX` moves from `NodeInfoResponse (40)` to the highest new type.
- `flatc` regenerates `transport_generated.h` from the updated schema.
- Relay message filter (`is_client_allowed`) needs new cases for the new types.

**Confidence:** HIGH -- FlatBuffers enum extension is a standard operation done 6+ times already.

## Integration Points

### Storage Layer

New methods needed on `Storage` class (all use existing MDBX primitives):

| Method | MDBX Operation | Existing Pattern |
|--------|---------------|------------------|
| `get_blobs_in_time_range()` | seq_map cursor scan + blobs_map get + decrypt | `get_blobs_by_seq()` |
| `batch_has_blobs()` | blobs_map get in loop (single txn) | `has_blob()` |
| `batch_get_blobs()` | blobs_map get+decrypt in loop (single txn) | `get_blob()` |
| `list_delegations()` | delegation_map cursor prefix scan | `list_namespaces()` |
| `get_storage_status()` | env info + map stats | `integrity_scan()` |
| `count_tombstones_for_ns()` | tombstone_map cursor prefix scan | `list_namespaces()` |

### Wire Protocol

New FlatBuffers enum values (11 request/response pairs, types 41-60+ ):

| Type ID | Name | Payload Size |
|---------|------|--------------|
| 41-42 | TimeRangeRequest/Response | 52 bytes / variable |
| 43-44 | BlobMetadataRequest/Response | 64 bytes / 57 bytes |
| 45-46 | BatchExistsRequest/Response | 34+ bytes / variable |
| 47-48 | BatchReadRequest/Response | 34+ bytes / variable |
| 49-50 | DelegationListRequest/Response | 32 bytes / variable |
| 51-52 | NamespaceListRequest/Response | 0 bytes / variable |
| 53-54 | NamespaceStatsRequest/Response | 32 bytes / variable |
| 55-56 | PeerInfoRequest/Response | 0 bytes / variable |
| 57-58 | HealthRequest/Response | 0 bytes / 14 bytes |
| 59-60 | StorageStatusRequest/Response | 0 bytes / 44 bytes |

**Note:** BlobMetadata and Metadata should be merged into a single type pair, reducing from 11 to 10 new pairs (20 new enum values, types 41-60).

### Dispatch Model

All new query types follow the **coroutine-IO** dispatch pattern (co_spawn on ioc_, stays on IO thread):

```cpp
// Pattern from Phase 62 CONC-04 classification
on_peer_message -> if (type == NewQueryRequest) {
    asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]()
        -> asio::awaitable<void> {
        // validate payload size (Step 0)
        // parse fields from payload
        // call storage/engine method
        // encode binary response
        // co_await conn->send_message(ResponseType, response, request_id)
    }, asio::detached);
}
```

Exceptions: PeerInfo and Health can be **inline** (no co_spawn needed) since they read only in-memory state with no IO.

No thread pool offload needed -- these are read-only operations on MDBX (read transactions are non-blocking in MVCC). The offload pattern is only needed for write operations that involve crypto verification (Data, Delete).

### Relay Message Filter

Add all new request/response types to `is_client_allowed()` in `relay/core/message_filter.cpp`. Mechanical switch-case addition of 20 new enum values.

## What NOT to Add

| Temptation | Why Not |
|------------|---------|
| Dedicated timestamp index (new sub-database) | YAGNI -- scan-and-filter via seq_map is sufficient for v1.4.0. Timestamps are in encrypted blob data; indexing requires decrypting on write, adding write-path complexity. Add only if profiling shows time-range queries are too slow. |
| Batch query library (e.g., protocol buffers for batch encoding) | YAGNI -- simple count + loop encoding is the established pattern (see PEX `encode_peer_list()`). |
| Health check framework (e.g., gRPC health checking) | YAGNI -- the node communicates over its own protocol. A 4-byte response is the health check. |
| JSON response encoding | NO -- all responses use binary wire format. JSON adds parsing overhead and breaks the existing binary-only contract. |
| In-memory metadata cache | YAGNI -- MDBX reads from mmap'd pages, which the OS already caches. Adding an application-level cache adds invalidation complexity for zero benefit. |
| Separate metadata storage (unencrypted) | NO -- breaks DARE security model. All blob data at rest must be encrypted. Storing metadata unencrypted leaks information about stored content. |

## Performance Considerations (No New Libraries Needed)

| Query Type | Expected Latency | Bottleneck | Mitigation |
|------------|------------------|------------|------------|
| TimeRange | Medium (N decrypts) | AEAD decryption per blob | Limit to 100 results; seq_map cursor provides natural time ordering |
| BlobMetadata | Low (1 decrypt) | Single blob decrypt | Single MDBX get + AEAD decrypt -- microseconds |
| BatchExists | Low (N key checks) | MDBX btree seeks | Single transaction, key-only checks -- very fast |
| BatchRead | Medium (N decrypts) | AEAD decryption per blob | Limit batch size (32) and total response size (1 MiB) |
| DelegationList | Low (cursor scan) | Prefix scan breadth | Delegations per namespace are typically few (<100) |
| NamespaceList | Low (cursor scan) | Number of namespaces | Already implemented, proven |
| NamespaceStats | Low (stats + scan) | Per-namespace tombstone/delegation counts | Use map_stat for global counts, cursor scan for per-namespace |
| PeerInfo | Trivial (memory read) | None | In-memory data, no IO |
| Health | Trivial (memory read) | None | Fixed response, no IO |
| StorageStatus | Low (env info) | None | MDBX env info is O(1) |

## Alternatives Considered

| Consideration | Decision | Rationale |
|---------------|----------|-----------|
| New sub-database for timestamp index | **Reject** | Adds write-path complexity (decrypt-index-re-encrypt dance), increases MDBX transaction scope, and v1.4.0 volumes don't warrant it. Seq-scan-and-filter is correct for now. |
| Separate MetadataRequest from BlobMetadataRequest | **Merge** | Same data, same implementation. Two type pairs waste enum space. |
| gRPC/REST health endpoint alongside binary protocol | **Reject** | Adds HTTP dependency. The existing protocol carries health checks natively. External monitoring can use a thin wrapper. |
| Parallel MDBX reads for batch operations | **Reject** | MDBX read transactions are already lock-free (MVCC). Multiple sequential reads in one transaction are faster than spawning parallel transactions due to transaction setup overhead. |

## Summary

The v1.4.0 milestone is a **pure application-layer feature expansion**. Every query type maps to existing MDBX cursor operations, in-memory state reads, or composition of existing Storage methods. The stack is complete. No new dependencies, no CMakeLists.txt changes, no FetchContent additions.

The work is:
1. Add enum values to `transport.fbs` and regenerate
2. Add ~6 new methods to `Storage` class using existing MDBX patterns
3. Add ~10 handler blocks in `PeerManager::on_peer_message` following the coroutine-IO pattern
4. Update relay message filter with new type pairs
5. Write unit tests following existing patterns in `test_storage.cpp` and `test_peer_manager.cpp`

## Sources

- libmdbx v0.13.11 cursor API: verified through codebase analysis of 12+ `lower_bound()` uses in `db/storage/storage.cpp` -- HIGH confidence
- MDBX `get_map_stat()` for entry counts: verified through `integrity_scan()` implementation -- HIGH confidence
- Binary wire format pattern: verified through 6 existing request/response handler implementations in `peer_manager.cpp` -- HIGH confidence
- FlatBuffers enum extension: verified through `transport.fbs` evolution from 30 types (v1.2.0) to 40 types (v1.3.0) -- HIGH confidence
- DARE encryption model (all-or-nothing AEAD): verified through `encrypt_value()`/`decrypt_value()` in storage.cpp -- HIGH confidence
- Dispatch model classification: verified through Phase 62 CONC-03/CONC-04 comments in `peer_manager.cpp` -- HIGH confidence
