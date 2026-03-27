# Architecture: v1.4.0 Extended Query Suite Integration

**Domain:** 11 new query/response message types for chromatindb node
**Researched:** 2026-03-26
**Confidence:** HIGH (analysis based on direct source code reading, not external references)

## Executive Summary

The 11 new query types integrate into the existing three-tier dispatch architecture with minimal structural changes. No new sub-databases are needed. No new async primitives are needed. The proven patterns from v1.3.0 (coroutine-IO dispatch, binary wire format, request_id echo, relay filter update) apply directly to every new type. The primary engineering effort is in three areas: (1) new Storage methods for queries not yet supported (time-range, metadata-without-data, delegation listing, tombstone counting), (2) wire format design for variable-length and batch responses, and (3) relay filter expansion from 20 to 42 client-facing types.

## Recommended Architecture

### Component Integration Map

Each new query type touches exactly four components in a fixed pattern:

```
FlatBuffers schema (transport.fbs)  -->  add type IDs 41-62
     |
Storage (storage.h/.cpp)           -->  add new query methods where needed
     |
PeerManager (peer_manager.cpp)     -->  add dispatch case in on_peer_message()
     |
Relay filter (message_filter.cpp)  -->  add Request/Response pairs to allowlist
     |
PROTOCOL.md                        -->  document wire formats
```

No changes to: Connection, Engine, Server, Handshake, Sync, Reconciliation, PEX, Config, Identity, or Crypto.

### New Type ID Allocation

Current enum range: 0-40 (41 values). New allocation:

| Type ID | Name | Direction |
|---------|------|-----------|
| 41 | TimeRangeRequest | Client -> Node |
| 42 | TimeRangeResponse | Node -> Client |
| 43 | BlobMetadataRequest | Client -> Node |
| 44 | BlobMetadataResponse | Node -> Client |
| 45 | BatchExistsRequest | Client -> Node |
| 46 | BatchExistsResponse | Node -> Client |
| 47 | BatchReadRequest | Client -> Node |
| 48 | BatchReadResponse | Node -> Client |
| 49 | DelegationListRequest | Client -> Node |
| 50 | DelegationListResponse | Node -> Client |
| 51 | NamespaceListRequest | Client -> Node |
| 52 | NamespaceListResponse | Node -> Client |
| 53 | NamespaceStatsRequest | Client -> Node |
| 54 | NamespaceStatsResponse | Node -> Client |
| 55 | MetadataRequest | Client -> Node |
| 56 | MetadataResponse | Node -> Client |
| 57 | PeerInfoRequest | Client -> Node |
| 58 | PeerInfoResponse | Node -> Client |
| 59 | HealthRequest | Client -> Node |
| 60 | HealthResponse | Node -> Client |
| 61 | StorageStatusRequest | Client -> Node |
| 62 | StorageStatusResponse | Node -> Client |

**Rationale:** Contiguous block starting at 41. Request/Response pairs are consecutive (even easier to read). The FlatBuffers enum type is `byte` (int8_t), which supports values -128 to 127. With 63 values (0-62) we are well within range.

**Note on BlobMetadata vs Metadata:** The milestone description lists both "BlobMetadata" and "Metadata" as separate queries. Analysis of the semantics: BlobMetadata returns metadata for a single blob identified by namespace+hash (like ReadRequest but without the payload). "Metadata" appears to be the same concept. **Recommendation:** Merge these into a single BlobMetadataRequest/Response pair (types 43/44). This leaves type IDs 55/56 available for future use, but they should be allocated in the schema as Reserved to maintain contiguous numbering.

If these are genuinely distinct (BlobMetadata = by hash, Metadata = by some other key), then keep both. The wire format section below covers both interpretations.

## Per-Type Dispatch Model Analysis

### Decision Framework

The dispatch model is determined by two questions:
1. Does the handler need to call `co_await` (for async send_message)? If yes: **coroutine**.
2. Does the handler call engine methods that offload to thread pool? If yes: **coroutine-offload-transfer**.

All 11 new types are read-only queries. None call engine_.ingest() or engine_.delete_blob(). Therefore none need the offload-transfer pattern. All follow the **coroutine-IO** pattern established by ReadRequest/ListRequest/StatsRequest/ExistsRequest/NodeInfoRequest in Phase 62/63.

### Dispatch Assignment

| Query Type | Dispatch Model | Rationale |
|------------|----------------|-----------|
| TimeRangeRequest | coroutine-IO | Storage cursor scan (seq_map + blob decode for timestamp check) |
| BlobMetadataRequest | coroutine-IO | Single blob lookup + decode (no data transfer) |
| BatchExistsRequest | coroutine-IO | Multiple has_blob() calls (fast, key-only) |
| BatchReadRequest | coroutine-IO | Multiple get_blob() calls (heavier, but still read-only) |
| DelegationListRequest | coroutine-IO | Delegation sub-database cursor scan |
| NamespaceListRequest | coroutine-IO | Existing list_namespaces() storage method |
| NamespaceStatsRequest | coroutine-IO | Existing get_namespace_quota() + effective_quota() |
| MetadataRequest | coroutine-IO | Same as BlobMetadata (merge recommended) |
| PeerInfoRequest | coroutine-IO | In-memory peer state (peers_ deque) + send_message |
| HealthRequest | coroutine-IO | Trivial: return "alive" with uptime |
| StorageStatusRequest | coroutine-IO | Storage metrics: used_bytes/used_data_bytes + tombstone count |

**Key insight:** None of these queries touch the crypto thread pool. All storage reads run synchronously on the IO thread (mdbx read transactions are fast, non-blocking). The coroutine-IO pattern is needed solely because `send_message()` is an awaitable that must run on the IO thread (AEAD nonce serialization).

### Handler Implementation Pattern

Every handler follows the same template (copying the ExistsRequest pattern from Phase 63):

```cpp
if (type == wire::TransportMsgType_FooRequest) {
    asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            // 1. Validate payload size (Step 0: cheapest check first)
            if (payload.size() < EXPECTED_SIZE) {
                record_strike(conn, "FooRequest too short");
                co_return;
            }
            // 2. Parse fields from binary payload
            // 3. Call storage/engine methods
            // 4. Serialize response to binary
            // 5. co_await conn->send_message(ResponseType, response, request_id);
        } catch (const std::exception& e) {
            spdlog::warn("FooRequest handler error from {}: {}", conn->remote_address(), e.what());
            record_strike(conn, e.what());
        }
    }, asio::detached);
    return;
}
```

No new patterns. Pure composition of existing infrastructure.

## New Storage Methods Required

### Methods That Already Exist (reuse directly)

| Query | Existing Method | Notes |
|-------|----------------|-------|
| NamespaceListRequest | `storage_.list_namespaces()` | Returns `vector<NamespaceInfo>` |
| NamespaceStatsRequest | `storage_.get_namespace_quota(ns)` + `engine_.effective_quota(ns)` | Same as StatsRequest but iterates all namespaces |
| BatchExistsRequest | `storage_.has_blob(ns, hash)` in a loop | Key-only lookup, O(1) per hash |
| BatchReadRequest | `engine_.get_blob(ns, hash)` in a loop | Full blob read + DARE decrypt |
| HealthRequest | No storage needed | Pure runtime state |

### Methods That Need New Implementation

#### 1. `Storage::get_blob_metadata(ns, hash) -> optional<BlobMetadata>`

**Purpose:** Return blob metadata (namespace, pubkey, ttl, timestamp, signature, data size) without the actual data payload.

**Implementation approach:** Same as `get_blob()` but after decrypting and decoding, return metadata struct instead of full BlobData. The blob must be fully decrypted and decoded because DARE encrypts the entire FlatBuffer; there is no way to read metadata without decrypting the whole thing.

**Optimization consideration:** This means BlobMetadataRequest is NOT cheaper than ReadRequest in terms of storage I/O -- the savings are entirely on the wire (response is ~2700 bytes for metadata vs potentially megabytes for full blob). This is still highly valuable: the client avoids downloading large payloads just to check metadata.

**Struct:**
```cpp
struct BlobMetadata {
    std::array<uint8_t, 32> namespace_id{};
    std::vector<uint8_t> pubkey;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    uint64_t data_size = 0;       // size of data field (not encoded size)
    std::array<uint8_t, 32> blob_hash{};  // computed from encoded form
    uint64_t seq_num = 0;         // from seq_map lookup
};
```

**Confidence:** HIGH. Direct reading of storage.cpp confirms DARE envelope wraps entire blob.

#### 2. `Storage::get_blobs_by_time_range(ns, min_ts, max_ts, limit) -> vector<BlobRef>`

**Purpose:** Return blob refs within a timestamp window.

**Implementation challenge:** There is NO timestamp index. The existing indexes are:
- `blobs_map`: keyed by `[namespace:32][hash:32]` -- no timestamp in key
- `seq_map`: keyed by `[namespace:32][seq_be:8]` -- ordered by insertion order
- `expiry_map`: keyed by `[expiry_ts_be:8][hash:32]` -- ordered by expiry time (not creation time)

**Approach options:**

**Option A: Scan seq_map + decode blobs for timestamp filtering.**
Walk seq_map for the namespace, fetch each blob from blobs_map, decrypt, decode, check timestamp. This is O(N) in the namespace blob count with DARE decryption per blob. Expensive for large namespaces.

**Option B: New timestamp index sub-database.**
Add `timestamp_map`: keyed by `[namespace:32][timestamp_be:8][hash:32]`, value empty. Range scan is then a simple cursor seek + walk. This is the clean solution but requires:
- New sub-database (max_maps goes from 8 to 9)
- Index population on every store_blob()
- Index cleanup on delete_blob_data() and expiry
- Migration: rebuild from existing blobs on first startup

**Option C: Use expiry_map as proxy.**
The expiry index is keyed by `[expiry_ts_be:8][hash:32]` where `expiry_ts = timestamp/1000000 + ttl`. For blobs with the same TTL, expiry ordering matches timestamp ordering. But TTLs vary, so this is unreliable. Also, permanent blobs (TTL=0) have no expiry entry.

**Recommendation: Option A for v1.4.0, defer Option B.**

Reasoning:
1. Option A works today with zero schema changes.
2. TimeRange queries are client-initiated (not sync-path), so latency is acceptable.
3. The limit cap (100, matching ListRequest) bounds worst-case work.
4. If profiling shows TimeRange is too slow, add the timestamp index in a future milestone.
5. YAGNI: don't add an 8th sub-database until proven necessary.

**Implementation:** Iterate seq_map for the namespace (ascending seq order), for each non-zero-hash entry, look up the blob in blobs_map, decrypt+decode, check `blob.timestamp` against the range, collect matching BlobRef entries up to limit. Skip blobs whose seq_num < some heuristic starting point if the caller provides one.

**Performance note:** For a namespace with 1000 blobs, this means up to 1000 DARE decrypt+decode operations to find matches. Each decrypt is ~microseconds (ChaCha20 is fast). The main cost is mdbx reads. Realistically, this is sub-second for moderate namespaces and acceptable for a client query. For million-blob namespaces, this would be too slow -- but that scale is far beyond current usage.

**Confidence:** HIGH. Verified all existing index keys; no timestamp-ordered index exists.

#### 3. `Storage::list_delegations(ns) -> vector<DelegationInfo>`

**Purpose:** List all active delegations for a namespace.

**Implementation:** Cursor scan of `delegation_map` with prefix `[namespace:32]`. The delegation key is `[namespace:32][delegate_pk_hash:32]`, value is `[delegation_blob_hash:32]`. For each entry, return the delegate pubkey hash and optionally the delegation blob hash.

**Struct:**
```cpp
struct DelegationInfo {
    std::array<uint8_t, 32> delegate_pubkey_hash{};
    std::array<uint8_t, 32> delegation_blob_hash{};
};
```

**This is straightforward:** delegation_map is prefix-scannable by namespace. A single read transaction cursor walk retrieves all delegations.

**Confidence:** HIGH. Direct reading of delegation_map key structure in storage.cpp.

#### 4. `Storage::tombstone_count() -> uint64_t`

**Purpose:** Return total number of tombstones stored (for StorageStatus).

**Implementation:** `txn.get_map_stat(impl_->tombstone_map).ms_entries`. Already done in integrity_scan(). Just expose it as a public method.

**Could also add:** `Storage::tombstone_count_for_namespace(ns)` via cursor prefix scan of tombstone_map, but the milestone spec says "tombstone counts" (aggregate), not per-namespace.

**Confidence:** HIGH. Proven in integrity_scan().

#### 5. No new method needed for `PeerInfoRequest`

PeerInfo data comes from `peers_` deque in PeerManager (in-memory). No storage call. The handler reads peer state directly, similar to NodeInfoRequest.

#### 6. No new method needed for `HealthRequest`

Pure liveness check. Returns uptime (from `start_time_`), peer count, and a status byte. No storage needed.

## Wire Format Design

### Pattern: Follow existing conventions

All existing wire formats in chromatindb use:
- Big-endian integers (uint16, uint32, uint64)
- Raw 32-byte arrays for hashes/namespace IDs
- Length-prefixed strings (1-byte length prefix for short strings)
- Status/flag bytes as uint8
- No padding/alignment -- packed binary

New formats follow the same conventions.

### Simple Queries (fixed-size request, fixed-size response)

#### HealthRequest/Response (types 59/60)

**Request:** Empty payload (0 bytes). Same pattern as NodeInfoRequest.

**Response:**
```
HealthResponse wire format:
[status:1]              -- 0x01 = healthy, 0x00 = degraded
[uptime_secs_be:8]      -- big-endian uint64
[peer_count_be:4]       -- big-endian uint32
[namespace_count_be:4]  -- big-endian uint32
[total_blobs_be:8]      -- big-endian uint64
= 25 bytes
```

**Rationale:** Minimal. A client doing a health check wants fast confirmation. Status byte distinguishes healthy from degraded (future: could reflect disk-full, no-peers, etc.). The rest echoes lightweight NodeInfo fields without the variable-length version/git strings.

#### StorageStatusRequest/Response (types 61/62)

**Request:** Empty payload (0 bytes).

**Response:**
```
StorageStatusResponse wire format:
[used_bytes_be:8]         -- storage_.used_bytes() (mmap geometry)
[used_data_bytes_be:8]    -- storage_.used_data_bytes() (B-tree occupancy)
[max_storage_bytes_be:8]  -- config_.max_storage_bytes (0 = unlimited)
[tombstone_count_be:8]    -- tombstone sub-database entry count
[blob_count_be:8]         -- total blobs across all namespaces
[namespace_count_be:4]    -- number of namespaces
= 44 bytes
```

#### BlobMetadataRequest/Response (types 43/44)

**Request:** 64 bytes (same as ReadRequest/ExistsRequest).
```
[namespace_id:32][blob_hash:32]
```

**Response (found):**
```
[0x01]                    -- found flag
[namespace_id:32]         -- echo
[blob_hash:32]            -- echo
[seq_num_be:8]            -- sequence number in namespace
[timestamp_be:8]          -- blob timestamp (seconds)
[ttl_be:4]                -- TTL in seconds (0 = permanent)
[data_size_be:4]          -- size of blob data field in bytes
[pubkey_len:2 BE]         -- public key length (2592 for ML-DSA-87)
[pubkey:pubkey_len]       -- signer public key
= 1 + 32 + 32 + 8 + 8 + 4 + 4 + 2 + 2592 = 2683 bytes typical
```

**Response (not found):**
```
[0x00]
```

**Design choice:** Include full pubkey in response. The signer's identity is metadata that clients need for verification and display. The 2592-byte ML-DSA-87 pubkey dominates the response, but this is still dramatically smaller than transferring the actual blob data (which can be up to 100 MiB).

### Batch Queries (variable-length request, variable-length response)

#### BatchExistsRequest/Response (types 45/46)

**Request:**
```
[namespace_id:32]         -- all hashes are in this namespace
[count_be:2]              -- number of hashes (uint16, max 256)
[hash:32] * count         -- blob hashes
= 34 + count * 32 bytes
```

**Response:**
```
[count_be:2]              -- number of results (matches request count)
[result:1] * count        -- 0x01 = exists, 0x00 = not found
= 2 + count bytes
```

**Design decisions:**
- Single namespace per request. Cross-namespace batch is rare and adds complexity.
- Max 256 hashes per request (uint16 count). Bounds response to 258 bytes and storage to 256 key lookups.
- Results in request order. Client correlates by position, not by echoed hash.

**Why not echo hashes in response?** Request_id already correlates the response. Within a batch, positional ordering is unambiguous and saves 32 * count bytes on the wire. The echo pattern in ExistsResponse (33-byte response) exists because pipelined single-exists requests need hash echo for correlation. Batch responses are self-correlating by position.

#### BatchReadRequest/Response (types 47/48)

**Request:**
```
[namespace_id:32]         -- all hashes are in this namespace
[count_be:2]              -- number of hashes (uint16, max 32)
[hash:32] * count         -- blob hashes
= 34 + count * 32 bytes
```

**Response:**
```
[count_be:2]              -- number of results
[result] * count          -- variable-length per-blob results
```

**Per-blob result:**
```
[status:1]                -- 0x01 = found, 0x00 = not found
if found:
  [data_len_be:4]         -- length of FlatBuffer-encoded blob
  [blob_data:data_len]    -- FlatBuffer-encoded blob (same as ReadResponse)
```

**Design decisions:**
- Max 32 blobs per request (not 256). BatchRead transfers full blob data, which can be large. 32 * 100 MiB = 3.2 GiB theoretical max is already beyond MAX_FRAME_SIZE. Practical limit: the response must fit in a single AEAD frame (110 MiB). The node should enforce a total response size cap and truncate with a "partial" flag if needed.
- Length-prefixed per-blob results allow variable-size blobs in a single response.

**Response size guard:** If total encoded response exceeds 100 MiB, truncate and set a partial flag:
```
[count_be:2]              -- number of results actually included
[partial:1]               -- 0x01 = truncated (more blobs were requested but response full)
[result] * count
```

#### DelegationListRequest/Response (types 49/50)

**Request:**
```
[namespace_id:32]
```

**Response:**
```
[count_be:2]              -- number of delegations
[delegation] * count
```

**Per-delegation:**
```
[delegate_pubkey_hash:32] -- SHA3-256(delegate_pubkey)
[delegation_blob_hash:32] -- hash of the delegation blob itself
= 64 bytes per delegation
```

**Why delegate_pubkey_hash, not full pubkey?** The delegation_map stores SHA3-256(delegate_pubkey) as the index key, not the full 2592-byte pubkey. To return the full pubkey, we would need to fetch and decrypt the delegation blob from blobs_map for each delegation. The hash is sufficient for the client to identify the delegate (they know the pubkey that hashes to it).

**Limit:** Cap at 100 delegations per response (matching List pattern). Unlikely to be hit in practice.

#### NamespaceListRequest/Response (types 51/52)

**Request:** Paginated:
```
[since_ns:32]             -- return namespaces after this one (zeros = from start)
[limit_be:4]              -- max results (capped at 100)
= 36 bytes
```

**Response:**
```
[count_be:4]              -- number of namespaces in this page
[entry] * count
[has_more:1]              -- pagination flag
```

**Per-entry:**
```
[namespace_id:32]
[latest_seq_num_be:8]
= 40 bytes per entry
```

This is identical to the existing `NamespaceInfo` struct. The existing `storage_.list_namespaces()` already returns this data. We just need to add pagination support (skip namespaces <= since_ns, limit to count).

**Limit:** 100 per page, matching ListRequest.

#### NamespaceStatsRequest/Response (types 53/54)

**Distinction from StatsRequest (type 35):** StatsRequest queries a single namespace. NamespaceStatsRequest queries all namespaces at once (batch stats).

**Request:**
```
[count_be:2]              -- number of namespaces to query (0 = all)
[namespace_id:32] * count -- specific namespaces (omitted if count=0)
```

**Response:**
```
[count_be:4]              -- number of entries
[entry] * count
```

**Per-entry:**
```
[namespace_id:32]
[blob_count_be:8]
[total_bytes_be:8]
[quota_bytes_be:8]
[quota_count_be:8]
= 64 bytes per entry
```

**Limit:** 100 entries per response when querying all.

#### TimeRangeRequest/Response (types 41/42)

**Request:**
```
[namespace_id:32]
[min_timestamp_be:8]      -- seconds (matches blob timestamp format)
[max_timestamp_be:8]      -- seconds
[limit_be:4]              -- max results (capped at 100)
[since_seq_be:8]          -- pagination cursor (0 = from start)
= 60 bytes
```

**Response:**
```
[count_be:4]              -- number of matching blob refs
[entry] * count
[has_more:1]              -- pagination flag
```

**Per-entry:**
```
[blob_hash:32]
[seq_num_be:8]
[timestamp_be:8]
= 48 bytes per entry
```

**Design note:** Returns BlobRef + timestamp (not full blob data). Client uses ReadRequest/BatchReadRequest to fetch actual blobs. This keeps the response small and the handler fast.

**Pagination:** The `since_seq` cursor works the same as ListRequest: scan seq_map entries with seq > since_seq, filter by timestamp, return up to limit. This is important because without a timestamp index, the scan walks sequentially -- the cursor lets clients resume where they left off.

#### PeerInfoRequest/Response (types 57/58)

**Request:** Empty payload (0 bytes).

**Response:**
```
[count_be:4]              -- number of connected peers
[entry] * count
```

**Per-peer entry:**
```
[pubkey_hash:32]          -- SHA3-256(peer_pubkey), or zeros if unknown
[address_len:1]           -- length of address string
[address:address_len]     -- remote address string (e.g., "1.2.3.4:4200")
[is_bootstrap:1]          -- 0x01 = bootstrap peer
[is_uds:1]                -- 0x01 = UDS connection
[syncing:1]               -- 0x01 = currently syncing
[subscriptions_be:2]      -- number of namespace subscriptions
[peer_is_full:1]          -- 0x01 = peer reported storage full
```

**Total per-peer:** variable (address_len) + 38 fixed = ~70 bytes typical.

**Security consideration:** PeerInfo reveals network topology to clients. This is acceptable for administrative queries (the relay already exposes the node's existence). If this is a concern, restrict PeerInfoRequest to UDS-only connections in a future milestone.

## Relay Filter Impact

### Current State

The relay message filter (`relay/core/message_filter.cpp`) uses a switch-case allowlist. Currently allows 20 types. All new Request/Response pairs must be added.

### Changes Required

Add 22 new cases (11 Request + 11 Response types) to `is_client_allowed()`:

```cpp
// v1.4.0 Extended queries
case TransportMsgType_TimeRangeRequest:
case TransportMsgType_TimeRangeResponse:
case TransportMsgType_BlobMetadataRequest:
case TransportMsgType_BlobMetadataResponse:
case TransportMsgType_BatchExistsRequest:
case TransportMsgType_BatchExistsResponse:
case TransportMsgType_BatchReadRequest:
case TransportMsgType_BatchReadResponse:
case TransportMsgType_DelegationListRequest:
case TransportMsgType_DelegationListResponse:
case TransportMsgType_NamespaceListRequest:
case TransportMsgType_NamespaceListResponse:
case TransportMsgType_NamespaceStatsRequest:
case TransportMsgType_NamespaceStatsResponse:
case TransportMsgType_MetadataRequest:
case TransportMsgType_MetadataResponse:
case TransportMsgType_PeerInfoRequest:
case TransportMsgType_PeerInfoResponse:
case TransportMsgType_HealthRequest:
case TransportMsgType_HealthResponse:
case TransportMsgType_StorageStatusRequest:
case TransportMsgType_StorageStatusResponse:
```

Total client-facing types: 20 (current) + 22 (new) = 42.

### NodeInfoResponse supported_types Update

The `supported[]` array in the NodeInfoRequest handler must be updated to include all new type IDs. Currently lists 20 types; will grow to 42.

### Test Updates

`test_message_filter.cpp` must add:
- CHECK cases for all 22 new types (allowed)
- Verify existing blocked types remain blocked
- Update "20 client-allowed types" comment to "42 client-allowed types"

## Component Boundaries

### What Changes

| Component | Change | Scope |
|-----------|--------|-------|
| `db/schemas/transport.fbs` | Add 22 new enum values (41-62) | Regenerate transport_generated.h |
| `db/storage/storage.h` | Add BlobMetadata struct, DelegationInfo struct, 3-4 new methods | Public API additions |
| `db/storage/storage.cpp` | Implement new methods | ~200 LOC new |
| `db/peer/peer_manager.cpp` | Add 11 dispatch cases in on_peer_message() | ~400 LOC new (follows template) |
| `relay/core/message_filter.cpp` | Add 22 cases to switch | ~25 LOC |
| `db/tests/storage/test_storage.cpp` | Tests for new storage methods | ~200 LOC |
| `db/tests/net/test_connection.cpp` or new | Tests for new handlers | ~400 LOC |
| `db/tests/relay/test_message_filter.cpp` | Tests for new filter entries | ~25 LOC |
| `db/PROTOCOL.md` | Document all new wire formats | ~200 lines |

### What Does NOT Change

| Component | Why Unchanged |
|-----------|---------------|
| `db/net/connection.h/.cpp` | Dispatch is in PeerManager, not Connection |
| `db/engine/engine.h/.cpp` | New queries are read-only; engine handles writes |
| `db/sync/sync_protocol.h/.cpp` | Sync is peer-to-peer, not client-facing |
| `db/sync/reconciliation.h/.cpp` | No sync changes |
| `db/crypto/*` | No new crypto operations |
| `db/config/config.h/.cpp` | No new config options needed |
| `db/identity/identity.h/.cpp` | No identity changes |
| `db/net/server.h/.cpp` | Server manages connections, not messages |
| `db/net/framing.h/.cpp` | Frame format unchanged |
| `db/wire/codec.h/.cpp` | Blob encoding unchanged |
| `relay/core/relay_session.cpp` | Session forwards based on filter; no logic change |
| `relay/config/relay_config.h/.cpp` | No new relay config |

## Data Flow

### Request Processing Flow (all 11 types identical)

```
Client --[PQ encrypted]--> Relay --[UDS TrustedHello]--> Node
                                                          |
                                                   Connection::message_loop()
                                                          |
                                                   FlatBuffer decode
                                                          |
                                                   PeerManager::on_peer_message()
                                                          |
                                                   rate limit check (token bucket)
                                                          |
                                                   type switch dispatch
                                                          |
                                                   asio::co_spawn(ioc_, handler_lambda)
                                                          |
                                                   payload validation (Step 0)
                                                          |
                                                   storage/engine query
                                                          |
                                                   serialize response
                                                          |
                                                   co_await conn->send_message()
                                                          |
Node --[UDS]--> Relay --[PQ encrypted]--> Client
```

### No IO-thread transfer needed

Unlike Data/Delete handlers which do:
```
co_await engine_.ingest()          // resumes on thread pool
co_await asio::post(ioc_)         // transfer back to IO thread
co_await conn->send_message()     // AEAD nonce safety
```

The new query handlers do:
```
auto result = storage_.some_query()  // synchronous on IO thread
co_await conn->send_message()        // already on IO thread
```

No thread pool involvement. No transfer needed. AEAD nonce safety is automatic because the entire handler runs on the IO thread.

## Suggested Build Order

### Rationale for Ordering

Dependencies flow in one direction: Schema -> Storage -> Dispatch -> Filter -> Tests -> Docs. Within the dispatch layer, types are independent and can be built in any order. The suggested grouping optimizes for: (1) building on existing patterns first, (2) getting simple types done before complex ones, (3) batch types together since they share patterns.

### Phase Structure Recommendation

**Phase 1: Schema + Simple Queries (zero new storage methods)**

Types that reuse existing storage methods with zero new code in storage layer:
- HealthRequest/Response -- pure runtime state, no storage
- StorageStatusRequest/Response -- existing storage_.used_bytes(), used_data_bytes(), tombstone count via get_map_stat()
- NamespaceListRequest/Response -- existing storage_.list_namespaces()
- NamespaceStatsRequest/Response -- existing storage_.get_namespace_quota() in a loop

Also includes: FlatBuffers schema update (all 22 new type IDs), regenerate transport_generated.h. This gives every subsequent phase the full type enum.

**Rationale:** Get the schema change done once upfront. These 4 query types require zero new Storage methods -- they compose existing infrastructure.

**Phase 2: Metadata + Delegation Queries (new storage methods)**

Types that need new Storage methods:
- BlobMetadataRequest/Response -- needs `get_blob_metadata()`
- MetadataRequest/Response -- merge with BlobMetadata, or implement as alias
- DelegationListRequest/Response -- needs `list_delegations()`

**Rationale:** Group types that extend the storage API together. The new methods are simple (single-blob lookup, cursor prefix scan) but benefit from being developed and tested as a unit.

**Phase 3: Batch Operations**

Types with variable-length multi-item responses:
- BatchExistsRequest/Response -- multi-hash existence check
- BatchReadRequest/Response -- multi-blob fetch with size guard

**Rationale:** Batch operations share the count-prefixed variable-length response pattern and the response-size-guard concern. Building them together establishes the batch pattern once.

**Phase 4: Time Range + Peer Info**

Types with scan-heavy or topology-exposing responses:
- TimeRangeRequest/Response -- seq_map scan with timestamp filtering (heaviest new query)
- PeerInfoRequest/Response -- in-memory peers_ walk

**Rationale:** TimeRange is the most complex new query (no timestamp index, requires full blob decrypt for filtering). PeerInfo exposes runtime topology. Group these as the "advanced queries" phase.

**Phase 5: Relay Filter + Documentation**

- Update relay message filter (all 22 new types)
- Update NodeInfoResponse supported_types
- Update PROTOCOL.md with all new wire formats
- Update README.md with v1.4.0 capabilities

**Rationale:** Following the v1.3.0 pattern: documentation-only final phase. The relay filter is a mechanical update that belongs with documentation.

### Alternative: Compressed Phase Structure

If velocity is the priority (v1.3.0 was completed in 1 day), this could be 2-3 phases:
1. Schema + all storage methods + all handlers
2. Relay filter + tests
3. Documentation

The 5-phase structure above is recommended for cleaner commits and easier review, but the codebase is mature enough that a compressed schedule would also work.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Per-query thread pool offload
**What:** Offloading read-only storage queries to the thread pool.
**Why bad:** Storage is NOT thread-safe (documented). mdbx read transactions are fast on the IO thread. Adding offload would require either: (a) Storage mutex (contention), or (b) separate Storage instance per thread (memory waste). Plus it adds the IO-thread transfer overhead.
**Instead:** Run all read queries synchronously on the IO thread via coroutine-IO pattern.

### Anti-Pattern 2: Echoing full request in response
**What:** Including the full request namespace+hash in every response.
**Why bad:** Wastes wire bytes. The `request_id` field already correlates request and response.
**Instead:** Echo only where positional ambiguity exists (ExistsResponse echoes hash for pipelined single-check correlation). Batch responses use positional ordering.

### Anti-Pattern 3: New sub-database for TimeRange
**What:** Adding a timestamp index sub-database to support TimeRange queries.
**Why bad:** Premature optimization. Adds write-path overhead to every store_blob() call. Requires migration logic. YAGNI until profiling shows seq_map scan is too slow.
**Instead:** Scan seq_map + decrypt + filter. Cap at 100 results. Profile later.

### Anti-Pattern 4: Different wire format conventions
**What:** Using little-endian, protobuf varint, or other encoding for new types.
**Why bad:** Inconsistency. Every existing type uses big-endian fixed-width integers and raw byte arrays.
**Instead:** Follow the established convention exactly. Big-endian. Fixed-width. Packed binary.

### Anti-Pattern 5: Unbounded response sizes
**What:** Allowing batch responses to grow without limit.
**Why bad:** Can exceed MAX_FRAME_SIZE (110 MiB). Can cause OOM on node or client.
**Instead:** Cap every list/batch response at a fixed count (100 for lists, 256 for BatchExists, 32 for BatchRead). Add has_more/partial flags for pagination.

## Scalability Considerations

| Concern | At 100 namespaces | At 10K namespaces | At 1M namespaces |
|---------|--------------------|--------------------|-------------------|
| NamespaceList | Returns all in one page | ~100 pages | ~10K pages, needs pagination |
| NamespaceStats (all) | 100 quota lookups | 10K lookups (~ms each) | Impractical; require specific-namespace mode |
| TimeRange scan | Fast (few blobs per ns) | Still fast if limit=100 | Each scan reads up to 100 blobs from seq_map |
| BatchExists (256) | 256 key lookups per request | Same | Same -- key lookup is O(1) per hash |
| BatchRead (32) | 32 blob reads per request | Same | Same -- blob read is O(1) per hash |
| DelegationList | Few delegations per ns | Same | Same -- delegation count is per-namespace |
| PeerInfo | Few peers | Same (max_peers bounds this) | Same |
| Health/StorageStatus | Constant time | Constant time | list_namespaces() in StorageStatus is O(N) |

**Mitigation for 10K+ namespaces:** NamespaceList pagination (100/page) handles scale naturally. NamespaceStats with count=0 (all) should be avoided at scale; client should query specific namespaces. StorageStatus could cache namespace_count to avoid repeated list_namespaces() calls.

## Sources

All analysis based on direct source code reading:
- `db/peer/peer_manager.cpp` -- dispatch model, on_peer_message() handler structure (lines 468-948)
- `db/storage/storage.h` -- Storage public API, all method signatures
- `db/storage/storage.cpp` -- index structure, 7 sub-databases, Impl struct, key formats
- `db/engine/engine.h` -- BlobEngine API, effective_quota()
- `db/schemas/transport.fbs` -- FlatBuffers enum, current 41 values (0-40)
- `db/wire/transport_generated.h` -- generated enum with int8_t type
- `relay/core/message_filter.cpp` -- relay allowlist, 20 current client-facing types
- `db/tests/relay/test_message_filter.cpp` -- filter test coverage
- `db/PROTOCOL.md` -- existing wire format documentation, type table
- `.planning/PROJECT.md` -- project context, all validated requirements
- `.planning/RETROSPECTIVE.md` -- v1.3.0 patterns (dispatch model, binary wire format)
