# Feature Landscape: v1.4.0 Extended Query Suite

**Domain:** Decentralized database node -- query API expansion
**Researched:** 2026-03-26

## Analysis: Merge Metadata and BlobMetadata

The milestone lists both "BlobMetadata" (item 2) and "Metadata" (item 8) as separate queries. Both are described as "blob metadata without data/payload." These are the same operation and MUST be merged into a single message type called `MetadataRequest/MetadataResponse`.

**Rationale:** Two message types doing the same thing violates the protocol's zero-duplication principle, wastes enum slots, confuses SDK authors, and would require maintaining two handlers with identical logic. The merged type covers both descriptions.

**Result: 10 new message types, not 11.**

## Table Stakes

Features users/SDKs expect from a database node with the existing query model. Missing = protocol feels incomplete.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Health | Every deployable daemon needs a liveness/readiness probe. Without this, orchestrators (Docker, K8s, systemd) cannot determine node status. | Low | Cheapest handler in the set. No storage access for liveness. |
| MetadataRequest | Clients already have ReadRequest (full blob) and ExistsRequest (yes/no). The obvious middle ground -- "give me info about this blob without the payload" -- is missing. Essential for building UIs, quota displays, audit trails. | Low | Decode stored blob, strip data field, return fields. |
| NamespaceList | NodeInfoResponse already reports namespace_count but clients cannot enumerate them. Any admin/monitoring tool needs this. | Low | storage_.list_namespaces() already exists. Add pagination. |
| NamespaceStats | StatsRequest exists but only returns blob_count + total_bytes + quota_bytes. Per-namespace stats with quota headroom is the natural extension for monitoring dashboards. | Low | Reuses get_namespace_quota() + effective_quota(). May be a superset of existing StatsResponse. |
| StorageStatus | Operators need disk usage, quota headroom, tombstone counts for capacity planning. NodeInfoResponse has storage_used/max but lacks tombstone and quota detail. | Low | Aggregation query from existing sub-databases. |

## Differentiators

Features that set the protocol apart. Not strictly expected, but high value for SDK usability and operational efficiency.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| BatchExists | Check N blob hashes in one round-trip instead of N ExistsRequests. Critical for sync clients verifying local cache freshness. Reduces round-trips from O(N) to O(1). | Low | N calls to has_blob() in a loop. Wire format is straightforward. |
| BatchRead | Fetch N small blobs in one round-trip. For messaging/chat use cases where payloads are small (< 1 KiB) and latency per round-trip matters more than bandwidth. | Medium | Response size bounds, partial failure handling, aggregate response encoding. |
| TimeRange | Query blobs by timestamp window. Enables "what happened in the last hour?" queries. Currently impossible without listing all blobs and client-side filtering. | Medium-High | Requires new storage index or seq_map scan with blob decode. See detailed analysis below. |
| DelegationList | List active delegations for a namespace. Enables delegation management UIs and audit. Currently no way to discover who has write access. | Medium | Requires new storage cursor iteration over delegation_map. |
| PeerInfo | Detailed per-peer connection info (addresses, handshake type, sync state). Goes beyond NodeInfoResponse's peer_count. Enables operational dashboards. | Medium | Exposes PeerManager internal state. Privacy/security considerations. |

## Anti-Features

Features to explicitly NOT build in this milestone.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| Full-text search or blob content queries | chromatindb is a signed blob store, not a database engine. Content interpretation belongs in the application layer above. | SDK can fetch blobs and filter client-side. |
| Aggregation queries (sum, avg, group-by) | Same reason -- the database stores opaque blobs. | Application layer responsibility. |
| Cross-namespace queries | Each namespace is cryptographically owned. Cross-namespace queries leak information about what namespaces exist and their contents. | Query one namespace at a time. NamespaceList + per-namespace queries. |
| Blob content modification/update | Blobs are signed and immutable. "Update" = write new blob + tombstone old. | Existing Data + Delete operations. |
| Historical query (point-in-time snapshots) | Would require MVCC or versioning infrastructure that doesn't exist. | Timestamp-based queries give a practical approximation. |
| BlobMetadata as separate type from Metadata | Duplicate functionality. See merge analysis above. | Single MetadataRequest/MetadataResponse type. |

## Detailed Feature Specifications

### 1. Health (HealthRequest / HealthResponse)

**Purpose:** Liveness and readiness probing for orchestrators, load balancers, and monitoring systems.

**Semantics (Kubernetes model):**
- **Liveness:** "Is the process alive and can it handle messages?" Always returns OK if the message handler is running. No external dependency checks. Liveness failure = restart the process.
- **Readiness:** "Is the node ready to serve client requests?" Checks that storage is accessible (can open a read transaction). Readiness failure = stop routing traffic, but do not restart.

**Request payload:** 1 byte

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| check_type | 0 | 1 | uint8 | 0x00 = liveness, 0x01 = readiness |

**Response payload:** 2 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| status | 0 | 1 | uint8 | 0x01 = OK, 0x00 = NOT_OK |
| check_type | 1 | 1 | uint8 | Echo of request check_type |

**Dispatch:** Inline (liveness) or coroutine-IO (readiness, needs storage probe).

**Complexity:** Low. Liveness is trivially always-OK. Readiness is a single MDBX read txn open+abort.

**Confidence:** HIGH -- standard pattern across all infrastructure systems.

---

### 2. MetadataRequest / MetadataResponse (merged BlobMetadata + Metadata)

**Purpose:** Return blob metadata (size, timestamp, TTL, seq_num, signer public key hash) without transferring the blob payload. Saves bandwidth for large blobs where clients only need to display metadata.

**Request payload:** 64 bytes (same as ReadRequest/ExistsRequest)

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| namespace_id | 0 | 32 | Target namespace |
| blob_hash | 32 | 32 | Content hash of the blob |

**Response payload (found):** 86 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| found | 0 | 1 | uint8 | 0x01 = found |
| blob_hash | 1 | 32 | raw bytes | Echo of requested hash |
| data_size | 33 | 4 | big-endian uint32 | Size of blob data field in bytes |
| timestamp | 37 | 8 | big-endian uint64 | Blob timestamp (seconds) |
| ttl | 45 | 4 | big-endian uint32 | TTL in seconds (0 = permanent) |
| seq_num | 49 | 8 | big-endian uint64 | Sequence number in namespace |
| signer_hash | 57 | 32 | raw bytes | SHA3-256(pubkey) -- namespace_id for owners, delegate's namespace for delegates |
| is_tombstone | 89 | 1 | uint8 | 0x01 if tombstone, 0x00 otherwise |
| is_delegation | 90 | 1 | uint8 | 0x01 if delegation, 0x00 otherwise |

**Response payload (not found):** 1 byte `[0x00]`

**Implementation:** get_blob() from storage, decode FlatBuffer, extract fields, discard data payload. Need to find seq_num by scanning seq_map for the matching hash (same pattern as duplicate detection in store_blob). Alternatively, add a reverse index `[namespace:32][hash:32] -> seq_num:8` to make this O(1).

**Dispatch:** Coroutine-IO (storage read + decode).

**Complexity:** Low-Medium. The storage read and decode are straightforward. Finding seq_num for a given hash is the only non-trivial part (currently no reverse index exists).

**Confidence:** HIGH -- obvious gap in existing query model.

---

### 3. BatchExists (BatchExistsRequest / BatchExistsResponse)

**Purpose:** Check existence of multiple blob hashes in a single round-trip. Reduces latency for sync verification and cache freshness checks.

**Request payload:** 34 + N * 32 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| namespace_id | 0 | 32 | raw bytes | Target namespace |
| count | 32 | 2 | big-endian uint16 | Number of hashes (max 256) |
| hashes | 34 | count * 32 | raw bytes | Blob hashes to check |

**Response payload:** 2 + N bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| count | 0 | 2 | big-endian uint16 | Number of results (matches request count) |
| results | 2 | count | uint8[] | Per-hash: 0x01 = exists, 0x00 = not found |

**Why uint16 count, max 256:** Bounds response to reasonable size. 256 hashes * 32 bytes = 8 KiB request, 258-byte response. Fits comfortably in a single frame. If client needs more, send multiple BatchExists.

**Dispatch:** Coroutine-IO (N sequential has_blob() calls in a single MDBX read txn for efficiency).

**Complexity:** Low. N calls to has_blob() in a loop. The only subtlety is opening a single read transaction for the entire batch rather than N separate transactions.

**Confidence:** HIGH -- batch existence checks are standard in distributed KV stores.

---

### 4. BatchRead (BatchReadRequest / BatchReadResponse)

**Purpose:** Fetch multiple blobs in a single round-trip. Optimized for small-blob workloads (messaging, config, metadata) where per-request latency dominates.

**Request payload:** 36 + N * 32 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| namespace_id | 0 | 32 | raw bytes | Target namespace |
| count | 32 | 2 | big-endian uint16 | Number of hashes to fetch (max 64) |
| max_total_bytes | 34 | 4 | big-endian uint32 | Response size cap in bytes (0 = server default 4 MiB) |
| hashes | 38 | count * 32 | raw bytes | Blob hashes to fetch |

**Response payload:** variable

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| count | 0 | 2 | big-endian uint16 | Number of results |
| results | 2 | variable | per-blob entries | See below |
| truncated | end-1 | 1 | uint8 | 0x01 if max_total_bytes caused early stop |

Per-blob entry:

| Field | Size | Encoding | Description |
|-------|------|----------|-------------|
| status | 1 | uint8 | 0x01 = found, 0x00 = not found |
| blob_len | 4 | big-endian uint32 | Length of FlatBuffer blob (0 if not found) |
| blob_data | blob_len | raw bytes | FlatBuffer-encoded blob (absent if not found) |

**Why max 64 and 4 MiB cap:** Prevents a single BatchRead from creating a 100 MiB response frame. MAX_FRAME_SIZE is 110 MiB. With 64 blobs at ~1.5 MiB average, a 4 MiB default cap plus count limit keeps responses bounded. Server stops adding blobs once cumulative size exceeds max_total_bytes and sets truncated=1.

**Dispatch:** Coroutine-IO (sequential get_blob() calls in single read txn).

**Complexity:** Medium. The per-blob entry encoding with variable lengths requires careful offset tracking. The truncation logic (stop early when response exceeds budget) adds a code path. Partial results (some found, some not) need clear encoding.

**Confidence:** HIGH -- batch read is standard in distributed stores. The response format is well-precedented.

---

### 5. TimeRange (TimeRangeRequest / TimeRangeResponse)

**Purpose:** Query blobs in a namespace within a timestamp window. Enables "what changed recently?" queries, audit logs, and time-based data retrieval.

**CRITICAL IMPLEMENTATION NOTE:** The current storage layer has NO timestamp index. Timestamps are stored inside the encrypted FlatBuffer blob data. A TimeRange query would need to either:
1. **Add a new sub-database** `timestamp_map: [namespace:32][timestamp_be:8] -> hash:32` (efficient, O(log N) range scan)
2. **Scan seq_map + decode each blob** (inefficient, O(N) per namespace, decrypts every blob)

**Recommendation:** Add a timestamp index. Without it, TimeRange is O(N) and impractical for large namespaces. The index is populated at ingest time alongside the existing expiry_map and seq_map.

**Request payload:** 52 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| namespace_id | 0 | 32 | raw bytes | Target namespace |
| start_time | 32 | 8 | big-endian uint64 | Start of window (inclusive, seconds since epoch) |
| end_time | 40 | 8 | big-endian uint64 | End of window (inclusive, seconds since epoch) |
| limit | 48 | 4 | big-endian uint32 | Max entries (0 or >100 = server default 100) |

**Response payload:** same format as ListResponse

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| count | 0 | 4 | big-endian uint32 | Number of entries |
| entries | 4 | count * 40 | {hash:32, seq_be:8} | Blob hash + sequence number pairs |
| has_more | 4 + count*40 | 1 | uint8 | 1 = more entries in window |

**Pagination:** Client sends `start_time` = timestamp of last blob in previous response + 1 microsecond. Or better: use a cursor token approach with the last-seen timestamp+hash pair.

**Dispatch:** Coroutine-IO (timestamp index range scan).

**Complexity:** Medium-High. The new timestamp index is the main cost -- it needs to be populated at ingest, cleaned at expiry, and zeroed at deletion (like seq_map sentinels). The query itself is a standard MDBX range scan once the index exists.

**Confidence:** MEDIUM -- the feature itself is standard, but the storage index requirement adds significant implementation scope. This is the most complex feature in the set.

---

### 6. DelegationList (DelegationListRequest / DelegationListResponse)

**Purpose:** List active delegations for a namespace. Enables delegation management UIs and security audits ("who can write to my namespace?").

**Request payload:** 32 bytes

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| namespace_id | 0 | 32 | Target namespace |

**Response payload:** variable

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| count | 0 | 2 | big-endian uint16 | Number of delegations |
| entries | 2 | count * 64 | per-delegation entries | See below |

Per-delegation entry:

| Field | Size | Description |
|-------|------|-------------|
| delegate_pk_hash | 32 | SHA3-256(delegate_pubkey) -- the delegate's namespace ID |
| delegation_blob_hash | 32 | Hash of the delegation blob itself (for revocation via tombstone) |

**Why return delegation_blob_hash:** The client needs this to revoke a delegation (tombstone the delegation blob). Without it, the client would have to re-derive the hash, which requires the full delegation blob data.

**No pagination needed:** Delegations per namespace are expected to be small (tens, not thousands). A 2-byte count supports up to 65,535 entries at 64 bytes each = 4 MiB, well within frame limits. If a namespace has 65K+ delegates, the protocol has bigger problems.

**Implementation:** Requires a new Storage method `list_delegations(namespace_id)` that iterates the delegation_map sub-database for all keys starting with the given namespace prefix.

**Dispatch:** Coroutine-IO (delegation_map cursor iteration).

**Complexity:** Medium. The delegation_map already exists with the right key structure `[namespace:32][delegate_pk_hash:32] -> delegation_blob_hash:32`. Just needs a cursor range scan prefix-matching on the namespace.

**Confidence:** HIGH -- straightforward cursor iteration over existing sub-database.

---

### 7. NamespaceList (NamespaceListRequest / NamespaceListResponse)

**Purpose:** Enumerate all namespaces stored on the node. Currently, NodeInfoResponse gives namespace_count but no way to get the actual namespace IDs.

**Note:** The wire type name "NamespaceList" conflicts with the existing sync-internal type `NamespaceList (11)`. The new client-facing type MUST use a different name: `NamespaceListRequest/NamespaceListResponse` (paired request/response like all other client queries).

**Request payload:** 12 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| since_seq | 0 | 8 | big-endian uint64 | Return namespaces with any blob seq > this (0 = all) |
| limit | 8 | 4 | big-endian uint32 | Max entries (0 or >100 = server default 100) |

**Response payload:** variable

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| count | 0 | 4 | big-endian uint32 | Number of namespace entries |
| entries | 4 | count * 40 | {namespace_id:32, latest_seq_be:8} | Namespace ID + latest seq pairs |
| has_more | 4 + count*40 | 1 | uint8 | 1 = more namespaces available |

**Pagination:** Use since_seq of the last returned namespace's latest_seq. This is imperfect since multiple namespaces can share the same latest_seq, but in practice namespace counts are small enough that a single page suffices for most nodes. Alternative: use the last namespace_id as cursor (lexicographic ordering in MDBX).

**Better pagination approach:** Since storage_.list_namespaces() uses cursor jumps on the seq_map, the natural ordering is lexicographic by namespace_id. Pagination by last-seen namespace_id:

**Request payload (revised):** 36 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| after_namespace | 0 | 32 | raw bytes | Return namespaces after this (zero-filled = from start) |
| limit | 32 | 4 | big-endian uint32 | Max entries (0 or >100 = server default 100) |

**Implementation:** storage_.list_namespaces() already exists but returns ALL namespaces. Needs a paginated variant or the handler paginates the full result. Given expected namespace counts (hundreds to low thousands), returning all and slicing in the handler is acceptable. For scaling, add a Storage method that accepts a `after_namespace` cursor.

**Dispatch:** Coroutine-IO (seq_map scan).

**Complexity:** Low. storage_.list_namespaces() exists. Pagination wrapper is straightforward.

**Confidence:** HIGH -- natural extension of existing list_namespaces().

---

### 8. NamespaceStats (NamespaceStatsRequest / NamespaceStatsResponse)

**Purpose:** Per-namespace detailed statistics: count, bytes, quota usage, quota limits.

**Overlap with StatsRequest:** The existing StatsRequest (type 35) already returns `[blob_count:8][total_bytes:8][quota_bytes:8]` for a namespace. NamespaceStats is a superset. Two options:
1. **Extend StatsResponse** to include additional fields (quota count limit, count_limit remaining). Breaking change.
2. **New message type** with richer fields. StatsRequest remains for backward compat.

**Recommendation:** New message type. StatsRequest is already deployed in the protocol spec and may be used by relay test infrastructure. Keep it stable. NamespaceStats is the "v2" stats query.

**Request payload:** 32 bytes

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| namespace_id | 0 | 32 | Target namespace |

**Response payload:** 48 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| blob_count | 0 | 8 | big-endian uint64 | Number of blobs in namespace |
| total_bytes | 8 | 8 | big-endian uint64 | Total encrypted bytes stored |
| quota_bytes | 16 | 8 | big-endian uint64 | Byte quota limit (0 = unlimited) |
| quota_count | 24 | 8 | big-endian uint64 | Count quota limit (0 = unlimited) |
| tombstone_count | 32 | 8 | big-endian uint64 | Number of tombstones in namespace |
| delegation_count | 40 | 8 | big-endian uint64 | Number of active delegations |

**Implementation:** Combines get_namespace_quota(), effective_quota(), and new Storage methods to count tombstones and delegations for a namespace (prefix scans of tombstone_map and delegation_map).

**Dispatch:** Coroutine-IO (multiple storage reads).

**Complexity:** Low-Medium. The quota part reuses existing methods. Tombstone and delegation counts need new prefix-scan methods in Storage.

**Confidence:** HIGH -- straightforward aggregation of existing data.

---

### 9. PeerInfo (PeerInfoRequest / PeerInfoResponse)

**Purpose:** Expose detailed peer connection information for operational dashboards. Goes beyond NodeInfoResponse's peer_count.

**Security consideration:** This exposes internal network topology (peer IP addresses, handshake types). Should only be available on trusted connections (UDS or trusted TCP). The handler should check connection trust level before responding.

**Request payload:** 0 bytes (empty)

**Response payload:** variable

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| count | 0 | 2 | big-endian uint16 | Number of peers |
| entries | 2 | variable | per-peer entries | See below |

Per-peer entry:

| Field | Size | Encoding | Description |
|-------|------|----------|-------------|
| addr_len | 1 | uint8 | Length of address string |
| address | addr_len | UTF-8 | Peer address ("host:port") |
| is_inbound | 1 | uint8 | 0x01 = inbound, 0x00 = outbound |
| is_uds | 1 | uint8 | 0x01 = UDS, 0x00 = TCP |
| is_bootstrap | 1 | uint8 | 0x01 = bootstrap peer |
| is_syncing | 1 | uint8 | 0x01 = sync in progress |
| is_full | 1 | uint8 | 0x01 = peer reported storage full |
| subscriptions | 2 | big-endian uint16 | Number of subscribed namespaces |

**Dispatch:** Coroutine-IO (iterates peers_ deque, reads PeerInfo fields).

**Complexity:** Medium. Iterating peers_ and extracting connection metadata. Trust-gating adds a code path. Need to decide whether to expose pubkey hashes (useful for debugging, but leaks identity info).

**Confidence:** MEDIUM -- the feature is standard for admin endpoints, but the trust-gating and privacy decisions need careful design.

---

### 10. StorageStatus (StorageStatusRequest / StorageStatusResponse)

**Purpose:** Comprehensive storage status for capacity planning: disk usage, quota headroom, tombstone counts, mmap geometry.

**Request payload:** 0 bytes (empty)

**Response payload:** 56 bytes

| Field | Offset | Size | Encoding | Description |
|-------|--------|------|----------|-------------|
| used_data_bytes | 0 | 8 | big-endian uint64 | Actual B-tree data size |
| used_mmap_bytes | 8 | 8 | big-endian uint64 | mmap geometry size (file on disk) |
| max_storage_bytes | 16 | 8 | big-endian uint64 | Configured limit (0 = unlimited) |
| namespace_count | 24 | 4 | big-endian uint32 | Number of namespaces |
| total_blob_count | 28 | 8 | big-endian uint64 | Total blobs across all namespaces |
| total_tombstone_count | 36 | 8 | big-endian uint64 | Total tombstones |
| total_delegation_count | 44 | 8 | big-endian uint64 | Total delegations |
| cursor_peer_count | 52 | 4 | big-endian uint32 | Number of peers with stored cursors |

**Implementation:** Combines used_data_bytes(), used_bytes(), config.max_storage_bytes, and new aggregate count methods. Total blobs from sum of list_namespaces() seq_nums (with the known overcount caveat). Tombstone and delegation counts from MDBX map stats (ms_entries on the sub-database handles -- O(1)).

**Dispatch:** Coroutine-IO (multiple O(1) storage calls plus list_namespaces scan).

**Complexity:** Low. Most fields are single O(1) calls. The tombstone/delegation totals can use `txn.get_map_stat()` which returns entry count in O(1).

**Confidence:** HIGH -- all data already accessible through existing storage methods.

## Feature Dependencies

```
ExistsRequest (v1.3.0, SHIPPED) --> BatchExists (extends existence check to batch)
ReadRequest  (v1.2.0, SHIPPED)  --> BatchRead (extends read to batch)
ReadRequest  (v1.2.0, SHIPPED)  --> MetadataRequest (reads blob, strips payload)
ListRequest  (v1.2.0, SHIPPED)  --> TimeRange (similar response format, needs new index)
StatsRequest (v1.2.0, SHIPPED)  --> NamespaceStats (superset of StatsResponse)
NodeInfoRequest (v1.3.0, SHIPPED) --> StorageStatus (complementary operational data)
NodeInfoRequest (v1.3.0, SHIPPED) --> PeerInfo (complementary operational data)
list_namespaces() (storage, SHIPPED) --> NamespaceList (exposes existing to wire)
delegation_map (storage, SHIPPED) --> DelegationList (iterates existing index)
```

No circular dependencies. All features depend only on already-shipped infrastructure.

## Internal Dependencies Between New Features

```
NamespaceList --> (useful with) NamespaceStats (enumerate, then stats per namespace)
MetadataRequest --> (useful with) BatchExists (check existence, then get metadata for found)
Health --> (independent, no dependencies)
StorageStatus --> (independent, no dependencies)
PeerInfo --> (independent, no dependencies)
TimeRange --> (requires new storage timestamp index -- independent of other features)
```

## Complexity and Ordering Recommendation

### Phase 1: Simple query handlers (Low complexity, high value)
These require no storage layer changes -- just new message type handlers using existing methods.

1. **Health** -- cheapest to implement, immediately useful for deployment
2. **NamespaceList** -- storage_.list_namespaces() exists, just wire it up
3. **StorageStatus** -- aggregation of existing O(1) methods
4. **NamespaceStats** -- extends existing StatsRequest pattern

### Phase 2: Blob-level queries (Low-Medium complexity)
Require storage reads and decoding but no new indexes.

5. **MetadataRequest** -- get_blob() + strip payload
6. **BatchExists** -- N * has_blob() in single txn
7. **DelegationList** -- delegation_map prefix scan (new Storage method)

### Phase 3: Batch and range queries (Medium-High complexity)
Require more complex wire formats or new storage infrastructure.

8. **BatchRead** -- variable-length response encoding, truncation logic
9. **PeerInfo** -- trust-gating, privacy decisions, PeerManager state exposure
10. **TimeRange** -- requires new timestamp sub-database index

### Rationale for this order:
- Phase 1 items need zero Storage API changes
- Phase 2 items need minor Storage API additions (1-2 new methods)
- Phase 3 items need either complex wire logic (BatchRead) or new storage indexes (TimeRange)
- TimeRange is last because the timestamp index has the largest blast radius (affects ingest, expiry, delete code paths)

## New Storage Methods Needed

| Method | Used By | Description |
|--------|---------|-------------|
| `list_delegations(namespace_id)` | DelegationList | Prefix scan delegation_map, return {delegate_pk_hash, blob_hash} pairs |
| `count_tombstones(namespace_id)` | NamespaceStats | Prefix scan tombstone_map, return count |
| `count_delegations(namespace_id)` | NamespaceStats | Prefix scan delegation_map, return count |
| `total_tombstone_count()` | StorageStatus | MDBX map stats ms_entries on tombstone_map (O(1)) |
| `total_delegation_count()` | StorageStatus | MDBX map stats ms_entries on delegation_map (O(1)) |
| `get_blobs_by_time_range(ns, start, end, limit)` | TimeRange | New timestamp index range scan |

Note: `count_tombstones` and `count_delegations` per-namespace require cursor iteration (not O(1)) since MDBX map stats give total counts, not per-prefix counts. Alternative: maintain per-namespace counters in quota_map.

## Wire Type Enum Allocation

Current max: 40 (NodeInfoResponse). New types would be 41-60:

| Value | Name | Direction |
|-------|------|-----------|
| 41 | HealthRequest | Client -> Node |
| 42 | HealthResponse | Node -> Client |
| 43 | MetadataRequest | Client -> Node |
| 44 | MetadataResponse | Node -> Client |
| 45 | BatchExistsRequest | Client -> Node |
| 46 | BatchExistsResponse | Node -> Client |
| 47 | BatchReadRequest | Client -> Node |
| 48 | BatchReadResponse | Node -> Client |
| 49 | TimeRangeRequest | Client -> Node |
| 50 | TimeRangeResponse | Node -> Client |
| 51 | DelegationListRequest | Client -> Node |
| 52 | DelegationListResponse | Node -> Client |
| 53 | NamespaceListRequest | Client -> Node |
| 54 | NamespaceListResponse | Node -> Client |
| 55 | NamespaceStatsRequest | Client -> Node |
| 56 | NamespaceStatsResponse | Node -> Client |
| 57 | PeerInfoRequest | Client -> Node |
| 58 | PeerInfoResponse | Node -> Client |
| 59 | StorageStatusRequest | Client -> Node |
| 60 | StorageStatusResponse | Node -> Client |

20 new enum values (10 request/response pairs). All must be added to:
- transport.fbs enum
- relay message_filter (is_client_allowed)
- NodeInfoResponse supported_types array
- PROTOCOL.md wire spec

## Relay Filter Impact

All 20 new types (41-60) must be added to `is_client_allowed()` in the relay message filter. They are all client-facing operations.

**PeerInfo privacy concern:** Consider whether PeerInfoRequest should be blocked at the relay level. It exposes internal network topology. Options:
1. Allow through relay, node decides based on connection trust level
2. Block at relay, only available on UDS connections
3. Allow through relay, return sanitized data (no IP addresses, just counts)

**Recommendation:** Option 1 -- allow through relay, node returns reduced data for untrusted connections (count only, no addresses) and full data for trusted/UDS connections.

## MVP Recommendation

**Must-have for v1.4.0:**
1. Health -- deployment blocker
2. MetadataRequest -- completes the Read/Exists/Metadata trio
3. BatchExists -- high-value batch operation
4. NamespaceList -- needed for any admin tooling
5. NamespaceStats -- needed for monitoring
6. StorageStatus -- needed for capacity planning

**Should-have for v1.4.0:**
7. BatchRead -- high value but more complex encoding
8. DelegationList -- useful for delegation management

**Can defer to v1.5.0:**
9. PeerInfo -- nice-to-have, privacy decisions needed
10. TimeRange -- requires new storage index, highest complexity

## Sources

- Kubernetes liveness/readiness probe semantics: [Kubernetes docs](https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/)
- Health check best practices: [OneUptime blog](https://oneuptime.com/blog/post/2026-02-09-health-checks-liveness-vs-readiness/view)
- API batch operation patterns: [API Design Patterns Ch. 18](https://livebook.manning.com/book/api-design-patterns/chapter-18/v-7/)
- Timestamp-based pagination: [Web API Pagination with Timestamp_ID](https://phauer.com/2018/web-api-pagination-timestamp-id-continuation-token/)
- Storage quota API patterns: [StorageManager.estimate()](https://developer.mozilla.org/en-US/docs/Web/API/StorageManager/estimate)
- Existing chromatindb protocol spec: db/PROTOCOL.md (internal)
- Existing storage API: db/storage/storage.h (internal)
- Existing dispatch model: db/peer/peer_manager.cpp lines 523-538 (internal)
