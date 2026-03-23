# Phase 57: Client Protocol Extensions - Research

**Researched:** 2026-03-23
**Domain:** Wire protocol extensions (FlatBuffers schema + C++20 message dispatch)
**Confidence:** HIGH

## Summary

Phase 57 adds four new client-facing protocol operations to the chromatindb node: WriteAck, ReadRequest/ReadResponse, ListRequest/ListResponse, and StatsRequest/StatsResponse. The engine and storage layers already implement the underlying functionality (get_blob, get_blobs_since, get_namespace_quota) -- this phase wires those existing methods to new FlatBuffers message types in the transport schema and adds dispatch logic in PeerManager::on_peer_message.

The implementation follows an established pattern: the Delete/DeleteAck pair (types 18/19, added in Phase 12) is the exact blueprint for how WriteAck should work, and the existing on_peer_message dispatch switch shows the pattern for handling new message types. The new types extend the TransportMsgType enum starting at 31 (current max is SyncRejected = 30).

**Primary recommendation:** Follow the Delete/DeleteAck pattern exactly -- new FlatBuffers enum entries, hand-coded binary payloads (no new FlatBuffers tables), coroutine dispatch with co_spawn for async operations.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PROTO-01 | Node sends WriteAck (type 31) back to client after successful blob ingest, containing blob hash and seq_num | Engine already returns IngestResult with WriteAck struct (blob_hash + seq_num). DeleteAck (type 19) provides exact pattern for wire encoding: [blob_hash:32][seq_num_be:8][status:1]. Currently the Data handler in on_peer_message silently ingests -- just needs to send the ack back. |
| PROTO-02 | Client can fetch a specific blob by namespace + hash via ReadRequest (type 32) / ReadResponse (type 33) | BlobEngine::get_blob(namespace_id, blob_hash) already exists and returns optional<BlobData>. Storage::get_blob does the decryption transparently. Payload: request=[ns:32][hash:32], response=FlatBuffer-encoded blob (or empty payload if not found). |
| PROTO-03 | Client can list blobs in a namespace with pagination via ListRequest (type 34) / ListResponse (type 35), using since_seq cursor + limit | BlobEngine::get_blobs_since(namespace_id, since_seq, max_count) already exists with pagination support. Request=[ns:32][since_seq_be:8][limit_be:4], Response=[count_be:4][{blob_hash:32, seq_num_be:8}*count][has_more:1]. |
| PROTO-04 | Client can query namespace usage (blob count, total bytes, quota remaining) via StatsRequest (type 36) / StatsResponse (type 37) | Storage::get_namespace_quota(ns) returns NamespaceQuota{total_bytes, blob_count}. BlobEngine::effective_quota() is private but can be exposed or computed from config. Request=[ns:32], Response=[blob_count_be:8][total_bytes_be:8][quota_bytes_be:8]. |
</phase_requirements>

## CRITICAL: Type Number Conflict

The REQUIREMENTS.md specifies WriteAck as type 25, ReadRequest/ReadResponse as 26/27, etc. **These type numbers conflict with existing types:**

| Requested Type | Already Assigned To |
|---------------|---------------------|
| 25 (WriteAck) | PQRequired |
| 26 (ReadRequest) | QuotaExceeded |
| 27 (ReadResponse) | ReconcileInit |
| 28 (ListRequest) | ReconcileRanges |
| 29 (ListResponse) | ReconcileItems |
| 30 (StatsRequest) | SyncRejected |

**Resolution:** The new types MUST start at 31 (one past the current max SyncRejected = 30):

| Message | Corrected Type |
|---------|---------------|
| WriteAck | 31 |
| ReadRequest | 32 |
| ReadResponse | 33 |
| ListRequest | 34 |
| ListResponse | 35 |
| StatsRequest | 36 |
| StatsResponse | 37 |

The planner MUST use these corrected type numbers. The requirement descriptions (behavior) are correct; only the numeric type IDs are wrong.

## Standard Stack

No new dependencies needed. All work is within the existing stack.

### Core (Already in Project)
| Library | Version | Purpose | Role in This Phase |
|---------|---------|---------|-------------------|
| FlatBuffers | 25.2.10 | Wire format | Extend TransportMsgType enum with 7 new values |
| Standalone Asio | latest | Networking + coroutines | co_spawn for async message handlers |
| Catch2 | 3.7.1 | Testing | Unit tests for new wire types and dispatch |
| spdlog | 1.15.1 | Logging | Log new message handling |

### No New Dependencies
This phase adds zero new dependencies -- all work is schema extension and dispatch logic using existing infrastructure.

## Architecture Patterns

### Existing Message Flow (Data -> Ingest)
```
Client sends Data(8) message
    -> Connection::message_loop() decrypts frame
    -> Connection invokes message_cb_ (PeerManager::on_peer_message)
    -> on_peer_message matches type == TransportMsgType_Data
    -> co_spawn async handler:
        1. wire::decode_blob(payload) -> BlobData
        2. engine_.ingest(blob) -> IngestResult
        3. (currently) notify_subscribers if stored
        4. (NEW) send WriteAck back to connection
```

### Delete/DeleteAck Pattern (Blueprint for WriteAck)
The existing Delete handler (peer_manager.cpp:641-687) is the EXACT pattern:
```cpp
// Existing: Delete handler sends DeleteAck
if (type == wire::TransportMsgType_Delete) {
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            auto blob = wire::decode_blob(payload);
            auto result = co_await engine_.delete_blob(blob);
            if (result.accepted && result.ack.has_value()) {
                // Build DeleteAck payload: [blob_hash:32][seq_num_be:8][status:1]
                auto ack = result.ack.value();
                std::vector<uint8_t> ack_payload(41);
                std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                for (int i = 7; i >= 0; --i) {
                    ack_payload[32 + (7 - i)] = static_cast<uint8_t>(ack.seq_num >> (i * 8));
                }
                ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                co_await conn->send_message(wire::TransportMsgType_DeleteAck,
                                             std::span<const uint8_t>(ack_payload));
            }
        } catch (...) { ... }
    }, asio::detached);
}
```

### New Message Handlers Pattern
All four new request types follow the same dispatch structure:
```cpp
if (type == wire::TransportMsgType_ReadRequest) {
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            // 1. Decode request payload (hand-coded binary, not FlatBuffers)
            // 2. Call engine/storage method
            // 3. Encode response payload (hand-coded binary)
            // 4. co_await conn->send_message(ResponseType, response)
        } catch (const std::exception& e) {
            spdlog::warn("malformed request from {}: {}", conn->remote_address(), e.what());
            record_strike(conn, e.what());
        }
    }, asio::detached);
    return;
}
```

### Wire Payload Formats (Hand-Coded Binary)

The project uses hand-coded binary payloads for all non-blob messages (not FlatBuffers tables). This is the established pattern -- DeleteAck, NamespaceList, PeerList, Notification all use hand-coded binary. Follow this pattern.

**WriteAck (type 31) -- response only, after successful Data ingest:**
```
[blob_hash:32][seq_num_be:8][status:1]
Total: 41 bytes
status: 0 = stored, 1 = duplicate
```
Identical to DeleteAck format. Sent after engine_.ingest() succeeds.

**ReadRequest (type 32):**
```
[namespace_id:32][blob_hash:32]
Total: 64 bytes
```

**ReadResponse (type 33):**
```
If found:  [found:1 = 0x01][flatbuffer_blob:variable]
If not found: [found:1 = 0x00]
```
The blob portion is the FlatBuffer-encoded Blob (same encoding as Data message payload). This allows the client to decode it identically to received Data messages.

**ListRequest (type 34):**
```
[namespace_id:32][since_seq_be:8][limit_be:4]
Total: 44 bytes
```
limit=0 means "use server default" (the planner should define a sensible server-side max, e.g., 100).

**ListResponse (type 35):**
```
[count_be:4][{blob_hash:32, seq_num_be:8}*count][has_more:1]
Total: 4 + (count * 40) + 1 bytes
```
has_more=1 means the client should send another ListRequest with since_seq = last returned seq_num. The response contains hash+seq pairs (not full blobs) to keep the response small. The client can then use ReadRequest to fetch individual blobs.

**StatsRequest (type 36):**
```
[namespace_id:32]
Total: 32 bytes
```

**StatsResponse (type 37):**
```
[blob_count_be:8][total_bytes_be:8][quota_bytes_be:8]
Total: 24 bytes
```
quota_bytes=0 means unlimited. The quota remaining can be computed client-side as (quota_bytes - total_bytes).

### Files That Need Modification

1. **db/schemas/transport.fbs** -- Add 7 new enum values (WriteAck=31 through StatsResponse=37)
2. **db/wire/transport_generated.h** -- Regenerated by flatc (CMake custom command handles this)
3. **db/peer/peer_manager.cpp** -- Add dispatch for all 7 new types in on_peer_message
4. **db/peer/peer_manager.h** -- No changes needed (no new members)
5. **db/engine/engine.h** -- May need to expose effective_quota() as public (for StatsResponse)
6. **db/tests/net/test_protocol.cpp** -- Add round-trip tests for new TransportMsgType values
7. **db/tests/wire/test_codec.cpp** -- Optional: test new payload encode/decode helpers
8. **db/PROTOCOL.md** -- Document new message types

### Files That Do NOT Need Modification

- **db/engine/engine.cpp** -- All needed methods already exist
- **db/storage/storage.h/cpp** -- All needed queries already exist
- **db/wire/codec.h/cpp** -- No new FlatBuffers tables, just binary payloads
- **db/net/connection.h/cpp** -- Message transport layer unchanged
- **db/net/protocol.h/cpp** -- TransportCodec works with any type enum value

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Blob retrieval | Custom storage query | engine_.get_blob(ns, hash) | Already handles DARE decryption, returns BlobData |
| Blob listing | Custom seq scan | engine_.get_blobs_since(ns, seq, limit) | Already handles pagination, seq gaps, decryption |
| Namespace stats | Custom aggregation | storage_.get_namespace_quota(ns) | Maintains quota sub-database with O(1) lookups |
| Wire encoding | New FlatBuffers tables | Hand-coded binary payloads | Project pattern: all non-blob payloads are hand-coded binary |
| FlatBuffer regen | Manual flatc invocation | CMake custom command | Already configured in CMakeLists.txt, auto-runs on schema change |

## Common Pitfalls

### Pitfall 1: Type Number Collision
**What goes wrong:** Using the type numbers from REQUIREMENTS.md (25-31) will collide with existing types (PQRequired=25, QuotaExceeded=26, etc.)
**Why it happens:** Requirements were written without checking current enum max
**How to avoid:** Use types 31-37, starting one past current max (SyncRejected=30)
**Warning signs:** Compile error in FlatBuffers (duplicate enum value) or runtime type confusion

### Pitfall 2: WriteAck Only on New Blobs vs All Ingests
**What goes wrong:** Sending WriteAck only when status==stored, missing duplicate acks
**Why it happens:** The current Data handler only notifies subscribers on stored (not duplicates)
**How to avoid:** Send WriteAck for ALL accepted ingests (stored AND duplicate). The client needs confirmation regardless. The Delete handler already does this correctly -- follow its pattern.
**Warning signs:** Client never gets ack for duplicate blob writes

### Pitfall 3: Missing Namespace Filter for New Message Types
**What goes wrong:** ReadRequest/ListRequest/StatsRequest bypass the sync_namespaces_ filter
**Why it happens:** New message types added without considering namespace filtering
**How to avoid:** These are READ operations, not writes. Namespace filtering (sync_namespaces_) controls what data the node REPLICATES, not what it serves locally. ReadRequest/ListRequest/StatsRequest should NOT be filtered -- they serve whatever is in storage. Only Data/Delete writes check the namespace filter. This is consistent with how the existing blob query methods work (sync uses them unfiltered too).
**Warning signs:** Client can't read blobs that the node has stored

### Pitfall 4: Forgetting to Handle Malformed Requests
**What goes wrong:** Buffer underrun or crash on too-short payloads
**Why it happens:** Hand-coded binary parsing without size validation
**How to avoid:** Every handler must validate payload.size() >= expected minimum BEFORE reading bytes. If too short, log + strike + return. Follow the try/catch + record_strike pattern from Delete handler.
**Warning signs:** ASAN heap-buffer-overflow, crash on malformed input

### Pitfall 5: Engine Quota Exposure for StatsResponse
**What goes wrong:** Cannot compute quota_remaining because BlobEngine::effective_quota() is private
**Why it happens:** effective_quota() was designed for internal ingest checks only
**How to avoid:** Either (a) make effective_quota() public, or (b) add a new public method like get_namespace_stats() that returns {count, bytes, quota_limit}, or (c) have PeerManager compute it from config directly. Option (a) is simplest and cleanest.
**Warning signs:** Cannot access quota limit for StatsResponse

### Pitfall 6: ListResponse Returning Full Blobs
**What goes wrong:** Sending full encoded blobs in ListResponse makes responses huge (100 MiB * N)
**Why it happens:** Temptation to reuse engine_.get_blobs_since() which returns full BlobData
**How to avoid:** ListResponse should return hash+seq_num pairs ONLY (40 bytes each). The client fetches individual blobs via ReadRequest. This keeps list responses bounded. Alternative: return the encoded blobs directly but cap the response size. The hash+seq approach is cleaner and consistent with how sync NamespaceList works.
**Warning signs:** OOM or MAX_FRAME_SIZE exceeded on large namespaces

### Pitfall 7: Big-Endian Encoding Inconsistency
**What goes wrong:** Mixing up endianness between new and existing payloads
**Why it happens:** Project uses big-endian for all wire integers (seq_num, count, etc.)
**How to avoid:** All multi-byte integers in wire payloads are big-endian. Follow the existing encoding pattern from DeleteAck:
```cpp
for (int i = 7; i >= 0; --i) {
    buf[offset + (7 - i)] = static_cast<uint8_t>(value >> (i * 8));
}
```
**Warning signs:** Values parsed incorrectly by client, seq_nums look wrong

## Code Examples

### Example 1: WriteAck in Data Handler (Modification to Existing Code)
```cpp
// Source: db/peer/peer_manager.cpp:706-762 (existing Data handler)
// ADD: After successful ingest, send WriteAck
if (result.accepted && result.ack.has_value()) {
    auto ack = result.ack.value();
    // Build WriteAck payload: [blob_hash:32][seq_num_be:8][status:1]
    std::vector<uint8_t> ack_payload(41);
    std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
    for (int i = 7; i >= 0; --i) {
        ack_payload[32 + (7 - i)] = static_cast<uint8_t>(ack.seq_num >> (i * 8));
    }
    ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
    co_await conn->send_message(wire::TransportMsgType_WriteAck,
                                 std::span<const uint8_t>(ack_payload));
    // Existing: notify_subscribers (only on stored, not duplicate)
    if (ack.status == engine::IngestStatus::stored) {
        notify_subscribers(...);
    }
}
```

### Example 2: ReadRequest Handler
```cpp
// New handler in on_peer_message
if (type == wire::TransportMsgType_ReadRequest) {
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            if (payload.size() < 64) {
                record_strike(conn, "ReadRequest too short");
                co_return;
            }
            std::array<uint8_t, 32> ns{};
            std::array<uint8_t, 32> hash{};
            std::memcpy(ns.data(), payload.data(), 32);
            std::memcpy(hash.data(), payload.data() + 32, 32);

            auto blob = engine_.get_blob(ns, hash);
            if (blob.has_value()) {
                auto encoded = wire::encode_blob(*blob);
                std::vector<uint8_t> response(1 + encoded.size());
                response[0] = 0x01;  // found
                std::memcpy(response.data() + 1, encoded.data(), encoded.size());
                co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                             std::span<const uint8_t>(response));
            } else {
                std::vector<uint8_t> response = {0x00};  // not found
                co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                             std::span<const uint8_t>(response));
            }
        } catch (const std::exception& e) {
            spdlog::warn("malformed ReadRequest from {}: {}", conn->remote_address(), e.what());
            record_strike(conn, e.what());
        }
    }, asio::detached);
    return;
}
```

### Example 3: FlatBuffers Schema Extension
```
// Source: db/schemas/transport.fbs
// ADD after SyncRejected = 30:
    // Phase 57: Client protocol extensions
    WriteAck = 31,
    ReadRequest = 32,
    ReadResponse = 33,
    ListRequest = 34,
    ListResponse = 35,
    StatsRequest = 36,
    StatsResponse = 37
```

### Example 4: ListRequest Handler with Pagination
```cpp
if (type == wire::TransportMsgType_ListRequest) {
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            if (payload.size() < 44) {
                record_strike(conn, "ListRequest too short");
                co_return;
            }
            std::array<uint8_t, 32> ns{};
            std::memcpy(ns.data(), payload.data(), 32);
            uint64_t since_seq = 0;
            for (int i = 0; i < 8; ++i)
                since_seq = (since_seq << 8) | payload[32 + i];
            uint32_t limit = 0;
            for (int i = 0; i < 4; ++i)
                limit = (limit << 8) | payload[40 + i];

            // Server-side cap to prevent huge responses
            constexpr uint32_t MAX_LIST_LIMIT = 100;
            if (limit == 0 || limit > MAX_LIST_LIMIT)
                limit = MAX_LIST_LIMIT;

            // Fetch limit+1 to detect has_more
            auto blobs = engine_.get_blobs_since(ns, since_seq, limit + 1);
            bool has_more = (blobs.size() > limit);
            if (has_more) blobs.resize(limit);

            // Build response: [count_be:4][{hash:32, seq_be:8}*count][has_more:1]
            uint32_t count = static_cast<uint32_t>(blobs.size());
            std::vector<uint8_t> response(4 + count * 40 + 1);
            // ... encode count, hash+seq pairs, has_more flag
            co_await conn->send_message(wire::TransportMsgType_ListResponse,
                                         std::span<const uint8_t>(response));
        } catch (...) { /* strike */ }
    }, asio::detached);
    return;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| No client read path | Clients use sync protocol to get blobs | v1.0 | Sync is peer-to-peer, not client-facing |
| WriteAck only internal | WriteAck struct in engine, not on wire | v0.7.0 | Engine returns it but node doesn't send it |
| DeleteAck on wire | Delete handler sends 41-byte ack | Phase 12 | This is the exact pattern WriteAck should follow |

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | Built via FetchContent in db/CMakeLists.txt |
| Quick run command | `cd build && ./chromatindb_tests "[protocol]" -c "New message types"` |
| Full suite command | `cd build && ./chromatindb_tests` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PROTO-01 | WriteAck sent after Data ingest | unit | `./chromatindb_tests "[protocol]" -c "WriteAck"` | No -- Wave 0 |
| PROTO-02 | ReadRequest/ReadResponse round-trip | unit | `./chromatindb_tests "[protocol]" -c "ReadRequest"` | No -- Wave 0 |
| PROTO-03 | ListRequest/ListResponse pagination | unit | `./chromatindb_tests "[protocol]" -c "ListRequest"` | No -- Wave 0 |
| PROTO-04 | StatsRequest/StatsResponse | unit | `./chromatindb_tests "[protocol]" -c "StatsRequest"` | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** `./chromatindb_tests "[protocol]"` (transport codec tests)
- **Per wave merge:** Full suite `./chromatindb_tests`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/net/test_protocol.cpp` -- Add sections for WriteAck, ReadRequest, ReadResponse, ListRequest, ListResponse, StatsRequest, StatsResponse TransportCodec round-trips
- [ ] Payload encode/decode helpers (if extracted to functions) -- test correct binary encoding

### Test Approach Notes
The unit tests for PROTO-01 through PROTO-04 will test at the wire level (TransportCodec encode/decode round-trips for new types) and at the payload level (binary payload encoding/decoding). Full integration testing (send Data, receive WriteAck over real connection) would be integration-level and can reuse the existing test_connection.cpp or test_peer_manager.cpp patterns if needed. The current test suite at 500+ tests provides solid regression coverage.

## Open Questions

1. **ListResponse: hash+seq pairs vs full blobs?**
   - What we know: Full blobs could exceed MAX_FRAME_SIZE for large namespaces
   - What's unclear: Whether clients always want full blobs or metadata-first-then-fetch
   - Recommendation: Return hash+seq pairs in ListResponse (lightweight). Client uses ReadRequest to fetch individual blobs. This is consistent with how sync NamespaceList works (metadata only). Keeps responses bounded.

2. **effective_quota() exposure pattern**
   - What we know: BlobEngine::effective_quota() is private, returns {byte_limit, count_limit}
   - What's unclear: Best way to expose for StatsResponse without breaking encapsulation
   - Recommendation: Make effective_quota() public. It's a pure query method with no side effects. Alternatively, add a public get_quota_limit(ns) -> pair<uint64_t, uint64_t> method that delegates to it.

3. **WriteAck for rejected ingests?**
   - What we know: DeleteAck is only sent on success, not rejection. Rejections get strikes + error signals (StorageFull, QuotaExceeded).
   - What's unclear: Should WriteAck also be sent for rejections (with an error code)?
   - Recommendation: NO. Follow DeleteAck pattern -- only send WriteAck on accepted ingests. Rejections already have specific signals (StorageFull, QuotaExceeded messages) or result in strikes. Adding rejection acks would be a protocol complexity increase for no benefit.

## Sources

### Primary (HIGH confidence)
- `db/schemas/transport.fbs` -- Current enum values, type numbering (max = SyncRejected = 30)
- `db/peer/peer_manager.cpp:641-762` -- Delete/DeleteAck and Data handler patterns
- `db/engine/engine.h:39-44` -- WriteAck struct definition
- `db/engine/engine.h:117-134` -- get_blobs_since, get_blob, list_namespaces API
- `db/storage/storage.h:59-62,237-239` -- NamespaceQuota struct, get_namespace_quota API
- `db/wire/codec.h` -- BlobData struct, encode_blob/decode_blob, blob_hash
- `db/net/protocol.h/cpp` -- TransportCodec encode/decode (works with any type value)

### Secondary (HIGH confidence)
- `db/PROTOCOL.md` -- Wire format documentation, payload encoding conventions
- `db/CMakeLists.txt:127-149` -- FlatBuffers code generation setup
- `db/tests/net/test_protocol.cpp` -- Existing TransportCodec test patterns

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- No new dependencies, pure extension of existing code
- Architecture: HIGH -- Direct codebase investigation, exact patterns identified in Delete/DeleteAck
- Pitfalls: HIGH -- Type collision found by comparing REQUIREMENTS.md vs actual transport.fbs enum
- Wire formats: HIGH -- All formats follow established project conventions (hand-coded binary, big-endian)

**Research date:** 2026-03-23
**Valid until:** 2026-04-23 (stable -- no external dependency changes)
