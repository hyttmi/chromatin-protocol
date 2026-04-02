# Phase 80: Targeted Blob Fetch - Research

**Researched:** 2026-04-02
**Domain:** Peer-to-peer blob fetch protocol, C++20 Asio coroutines, MDBX storage lookup
**Confidence:** HIGH

## Summary

Phase 80 adds two new message types (BlobFetch=60, BlobFetchResponse=61) that allow a peer receiving a BlobNotify to immediately fetch the specific blob by hash, skipping full reconciliation. The implementation touches five files: transport.fbs (enum), peer_manager.h/cpp (dispatch + handler + pending set), and message_filter.cpp (relay blocklist). All reusable primitives exist: `storage_.has_blob()` for dedup, `storage_.get_blob()` for retrieval, `engine_.ingest()` for ingestion, `wire::encode_blob()`/`wire::decode_blob()` for serialization.

One critical finding: **D-01 (hash-only BlobFetch) is incompatible with the storage layer.** MDBX `get_blob()` requires a compound key `[namespace_id:32][blob_hash:32]`. There is no hash-only index. The BlobFetch payload must include namespace_id (64 bytes total) rather than just the hash (32 bytes). This is trivial since the fetcher extracts both namespace_id and blob_hash from BlobNotify. This changes the wire format from D-01's "32 bytes" to "64 bytes" but is the only correct approach without adding a new MDBX sub-database (which would violate YAGNI).

**Primary recommendation:** Include namespace_id in BlobFetch payload (64 bytes: `[namespace_id:32][blob_hash:32]`), handle BlobFetch/BlobFetchResponse inline in the dispatch switch (no sync session), and use a `std::unordered_set` of 32-byte blob hashes for the pending-fetch dedup set.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** BlobFetch request is hash-only (32 bytes). The SHA3-256 hash from BlobNotify is the sole identifier. Responder looks up by hash. **RESEARCH OVERRIDE NEEDED: Storage requires namespace_id+hash for lookup. Must include namespace_id in BlobFetch payload (64 bytes) or add a new MDBX sub-database.**
- **D-02:** BlobFetchResponse returns the full signed blob exactly as stored (FlatBuffer with namespace, data, signature, TTL, timestamp). Receiver ingests it identically to a sync blob via the existing `ingest()` path.
- **D-03:** Status byte prefix on BlobFetchResponse: 0=found (rest is blob), 1=not-found (just that one byte). Clean, no ambiguity.
- **D-04:** On BlobNotify receipt, check `storage_.has_blob()` (existing key-only MDBX lookup). If blob exists locally, skip fetch entirely.
- **D-05:** Track pending fetches in a hash set. If a second BlobNotify arrives for the same hash while a fetch is in-flight, skip it. Remove hash from set after ingest completes or fails.
- **D-06:** New `PeerManager::on_blob_notify()` handler dispatched from the message loop when BlobNotify arrives. PeerManager owns the dedup set and has access to engine for `has_blob()`. Consistent with existing dispatch patterns.
- **D-07:** Suppress BlobFetch during active sync with the same peer. If `PeerInfo.syncing` is true for the notifying peer, skip the fetch -- full reconciliation will transfer everything. Avoids redundant fetches.
- **D-08:** Not-found response: silent drop. Log at debug level, remove from pending set. Full reconciliation (Phase 82) handles missed blobs.
- **D-09:** No timeout timer for BlobFetch. Fire and forget -- remove from pending set when response arrives or connection drops. Reconcile-on-reconnect handles missed blobs.
- **D-10:** Failed ingestion (signature verification failure): log warning, drop the blob. No disconnect, no retry, no strike. Same as current sync error handling.

### Claude's Discretion
- BlobFetch handler placement in the dispatch switch (inline vs case delegation)
- Pending fetch set data structure (std::unordered_set, flat_hash_set, etc.)
- How BlobFetchResponse handler routes to ingest (direct call vs reuse of existing sync ingest path)
- Relay filter update for types 60/61 (WIRE-04 already pre-blocks 59-61, verify and add explicit cases)
- Cleanup of pending set entries on connection close

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PUSH-05 | Peer receiving a BlobNotify can fetch the specific blob via targeted BlobFetch request (skip full reconciliation) | BlobNotify handler extracts namespace_id+blob_hash, checks has_blob() + pending set + syncing flag, sends BlobFetch(64 bytes), ingests BlobFetchResponse via engine_.ingest() |
| PUSH-06 | BlobFetch is handled inline in the message loop (no sync session handshake required) | BlobFetch/BlobFetchResponse dispatched as inline handlers in on_peer_message switch, no sync_inbox routing, no peer.syncing flag interaction |
| WIRE-02 | New message type BlobFetch (type 60) -- targeted blob request by hash | Add BlobFetch=60 to transport.fbs enum, add explicit case to relay blocklist |
| WIRE-03 | New message type BlobFetchResponse (type 61) -- response with blob data or not-found | Add BlobFetchResponse=61 to transport.fbs enum, add explicit case to relay blocklist |
</phase_requirements>

## Standard Stack

No new dependencies. This phase uses existing libraries only.

### Core (Existing)
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| FlatBuffers | latest via FetchContent | TransportMsgType enum extension | Wire format standard for project |
| libmdbx | latest via FetchContent | has_blob() / get_blob() lookups | Storage layer already in use |
| Standalone Asio | latest via FetchContent | co_spawn for coroutine handlers | Networking/coroutine standard |
| Catch2 | latest via FetchContent | Unit tests | Test framework for project |

No `npm install` or new packages needed.

## Architecture Patterns

### File Change Map
```
db/schemas/transport.fbs           # Add BlobFetch=60, BlobFetchResponse=61
db/peer/peer_manager.h             # Add pending_fetches_ set, on_blob_notify(), handle_blob_fetch()
db/peer/peer_manager.cpp           # Dispatch + handler implementations
relay/core/message_filter.cpp      # Add BlobFetch + BlobFetchResponse to blocklist
db/tests/peer/test_peer_manager.cpp # New test cases
```

### Pattern 1: BlobFetch Wire Format (REVISED from D-01)
**What:** BlobFetch carries `[namespace_id:32][blob_hash:32]` = 64 bytes.
**Why:** Storage `get_blob()` requires compound key `make_blob_key(ns, hash)`. There is no hash-only index in MDBX. Adding one would require a new sub-database (currently at 7 of 8 max_maps) and violates YAGNI. The fetcher already has namespace_id from BlobNotify.
**Wire format:**
```
BlobFetch (type 60): [namespace_id:32][blob_hash:32]  = 64 bytes

BlobFetchResponse (type 61):
  Found:     [0x00][flatbuf_encoded_blob...]  = 1 + blob_size bytes
  Not-found: [0x01]                           = 1 byte
```

### Pattern 2: Receive-Side BlobNotify Handler (Inline Dispatch)
**What:** When BlobNotify arrives, PeerManager performs three cheap checks before spawning a fetch coroutine.
**Dispatch model:** Inline check + co_spawn (same as Data/Delete pattern but simpler -- no engine offload needed for the fetch send, but the ingest on response will need engine offload).

```cpp
// In on_peer_message dispatch:
if (type == wire::TransportMsgType_BlobNotify) {
    // INLINE: all checks are O(1) in-memory
    auto* peer = find_peer(conn);
    if (!peer) return;

    // Parse notification: [ns:32][hash:32][seq:8][size:4][tomb:1]
    if (payload.size() != 77) return;
    std::array<uint8_t, 32> ns{}, hash{};
    std::memcpy(ns.data(), payload.data(), 32);
    std::memcpy(hash.data(), payload.data() + 32, 32);

    // D-07: Suppress during active sync with this peer
    if (peer->syncing) return;

    // D-04: Already have it?
    if (storage_.has_blob(ns, hash)) return;

    // D-05: Already fetching it?
    if (pending_fetches_.count(hash)) return;
    pending_fetches_.insert(hash);

    // Send BlobFetch coroutine
    asio::co_spawn(ioc_, [this, conn, ns, hash]() -> asio::awaitable<void> {
        std::vector<uint8_t> fetch_payload(64);
        std::memcpy(fetch_payload.data(), ns.data(), 32);
        std::memcpy(fetch_payload.data() + 32, hash.data(), 32);
        co_await conn->send_message(wire::TransportMsgType_BlobFetch,
                                     std::span<const uint8_t>(fetch_payload));
    }, asio::detached);
    return;
}
```

### Pattern 3: BlobFetch Handler (Responder Side, Inline Dispatch)
**What:** When a peer receives BlobFetch, it looks up the blob and sends back BlobFetchResponse.
**Dispatch model:** co_spawn on ioc_ (synchronous storage call, similar to ReadRequest pattern).

```cpp
if (type == wire::TransportMsgType_BlobFetch) {
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        if (payload.size() != 64) co_return;
        std::array<uint8_t, 32> ns{}, hash{};
        std::memcpy(ns.data(), payload.data(), 32);
        std::memcpy(hash.data(), payload.data() + 32, 32);

        auto blob = storage_.get_blob(ns, hash);
        if (blob) {
            auto encoded = wire::encode_blob(*blob);
            std::vector<uint8_t> resp(1 + encoded.size());
            resp[0] = 0x00;  // found
            std::memcpy(resp.data() + 1, encoded.data(), encoded.size());
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse,
                                         std::span<const uint8_t>(resp));
        } else {
            std::vector<uint8_t> resp = {0x01};  // not-found
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse,
                                         std::span<const uint8_t>(resp));
        }
    }, asio::detached);
    return;
}
```

### Pattern 4: BlobFetchResponse Handler (Requester Side)
**What:** When BlobFetchResponse arrives, decode status, ingest blob, clean up pending set.
**Dispatch model:** co_spawn with engine offload (same as Data handler -- engine_.ingest() offloads crypto to pool).

```cpp
if (type == wire::TransportMsgType_BlobFetchResponse) {
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        if (payload.empty()) co_return;
        uint8_t status = payload[0];
        if (status == 0x01) {
            // Not found -- debug log, pending set cleanup happens below
            spdlog::debug("BlobFetchResponse not-found from {}", conn->remote_address());
            // Cannot identify which hash this was for without tracking -- see Open Questions
            co_return;
        }
        if (status != 0x00 || payload.size() < 2) co_return;

        try {
            auto blob = wire::decode_blob(std::span<const uint8_t>(payload.data() + 1,
                                                                      payload.size() - 1));
            auto result = co_await engine_.ingest(blob, conn);
            co_await asio::post(ioc_, asio::use_awaitable);  // Transfer back to IO thread

            // Compute blob_hash for pending set cleanup
            auto blob_hash = crypto::sha3_256(wire::encode_blob(blob));
            pending_fetches_.erase(blob_hash);

            if (result.accepted && result.ack &&
                result.ack->status == engine::IngestStatus::stored) {
                on_blob_ingested(blob.namespace_id, result.ack->blob_hash,
                                 result.ack->seq_num,
                                 static_cast<uint32_t>(blob.data.size()),
                                 wire::is_tombstone(blob.data), conn);
            }
        } catch (const std::exception& e) {
            spdlog::warn("malformed BlobFetchResponse from {}: {}", conn->remote_address(), e.what());
            // D-10: no strike, no disconnect
        }
    }, asio::detached);
    return;
}
```

### Pattern 5: Relay Filter Update
**What:** Add explicit `case` entries for BlobFetch and BlobFetchResponse to the relay blocklist.
**Why:** The relay uses a blocklist (explicit cases return false, default returns true). Types 60/61 do not yet exist in the enum. Without explicit cases, the `default: return true` would allow them through the relay, which is wrong -- these are peer-internal messages.

```cpp
// In message_filter.cpp is_client_allowed():
case TransportMsgType_BlobNotify:
case TransportMsgType_BlobFetch:        // Phase 80: peer-internal
case TransportMsgType_BlobFetchResponse: // Phase 80: peer-internal
    return false;
```

### Anti-Patterns to Avoid
- **Hash-only storage lookup:** Storage requires namespace_id+hash compound key. Never attempt to iterate all namespaces to find a blob by hash alone.
- **Routing BlobFetch/BlobFetchResponse through sync_inbox:** These are NOT sync messages. They must NOT set `peer->syncing` or interact with the sync session. The whole point is to work alongside (or instead of) full sync.
- **Using request_id to correlate fetch/response:** D-09 says fire-and-forget. The pending set tracks outstanding hashes, not per-request correlation.
- **Re-hashing blob to identify pending entry on not-found:** BlobFetchResponse not-found is 1 byte (D-03) -- it carries no hash. Cannot clean pending set on not-found without correlation. This is acceptable: the pending set entry leaks until connection close (see cleanup pattern below).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Blob serialization | Custom wire format | `wire::encode_blob()` / `wire::decode_blob()` | FlatBuffer deterministic encoding, battle-tested |
| Blob ingestion + validation | Custom validation pipeline | `engine_.ingest()` with source parameter | Handles namespace check, signature verification, dedup, expiry, quotas |
| Hash-only dedup check | Bloom filter or custom index | `storage_.has_blob(ns, hash)` | O(1) MDBX key-only lookup, already proven |
| Notification fan-out after fetch ingest | New fan-out path | `on_blob_ingested()` with source=conn | Excludes source, notifies all other TCP peers + subscribed clients |
| Send queue serialization | Manual socket write | `conn->send_message()` | Phase 79 send queue handles AEAD nonce ordering |

## Common Pitfalls

### Pitfall 1: Pending Set Leak on Not-Found
**What goes wrong:** BlobFetchResponse not-found (D-03: single 0x01 byte) carries no blob hash. The receiver cannot identify which pending entry to remove.
**Why it happens:** Fire-and-forget design (D-09) trades simplicity for perfect cleanup.
**How to avoid:** Clean all pending entries associated with a peer on connection close (on_peer_disconnected). Alternatively, track `{hash -> connection}` in the pending set to enable targeted cleanup. Given the low-volume nature (notifications are rare), leaked entries are harmless and cleaned on disconnect.
**Warning signs:** Growing pending_fetches_ set size in long-running nodes. Not a real problem since disconnect clears all.

### Pitfall 2: Forgetting IO Thread Transfer After Ingest
**What goes wrong:** `engine_.ingest()` offloads crypto to thread pool. After co_await, the coroutine resumes on the pool thread. Accessing `pending_fetches_`, `on_blob_ingested()`, or any PeerManager state from the pool thread is a data race.
**Why it happens:** Same trap as Phase 62 CONC-03.
**How to avoid:** Always `co_await asio::post(ioc_, asio::use_awaitable)` after `engine_.ingest()` before touching any PeerManager state.
**Warning signs:** TSAN reports on pending_fetches_ or peers_ access.

### Pitfall 3: BlobFetch During Active Sync Causes Duplicate Ingestion
**What goes wrong:** If BlobFetch and sync both deliver the same blob, engine_.ingest() returns IngestStatus::duplicate for the second one, which is harmless. But the notification fan-out only fires for `IngestStatus::stored`, so no double-notification. This is actually fine by design.
**Why it matters:** D-07 suppresses BlobFetch during sync to avoid redundant network traffic, not to avoid data corruption.
**How to avoid:** D-07 is correct -- check `peer->syncing` before sending BlobFetch. But even if a race occurs, ingest dedup handles it.

### Pitfall 4: Relay Blocklist Default-Allow for New Types
**What goes wrong:** The relay uses `default: return true` (allow). Any new TransportMsgType added to the FlatBuffer enum but NOT added to the blocklist switch will be allowed through to clients.
**Why it happens:** Blocklist approach means new types are implicitly allowed.
**How to avoid:** Always add explicit `case` entries for peer-internal types to the blocklist when adding to transport.fbs. Phase 80 must add both BlobFetch and BlobFetchResponse.
**Warning signs:** Client receiving type 60 or 61 messages (should never happen).

### Pitfall 5: Pending Set Hash Tracking Requires Knowing the Hash
**What goes wrong:** On BlobFetchResponse (found), we need to extract the blob hash to clean up the pending set. But the hash isn't in the response wire format -- we'd need to re-compute it from the received blob data.
**Why it happens:** BlobFetchResponse format is `[status:1][blob_flatbuf...]` -- no separate hash field.
**How to avoid:** After decoding the blob, compute its hash via the engine's existing path (or extract from the IngestResult's ack.blob_hash after ingest). The ack.blob_hash is the canonical hash. Use `result.ack->blob_hash` for pending set cleanup.
**Warning signs:** Pending set not shrinking after successful fetches.

## Code Examples

### BlobNotify Payload Decoding (from Phase 79 encode_notification)
```cpp
// Source: db/peer/peer_manager.cpp line 3000-3021
// BlobNotify payload format: [namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
// Total: 77 bytes
static constexpr size_t BLOB_NOTIFY_SIZE = 77;

// Decode namespace_id and blob_hash from notification:
std::array<uint8_t, 32> ns{}, hash{};
std::memcpy(ns.data(), payload.data(), 32);
std::memcpy(hash.data(), payload.data() + 32, 32);
```

### Existing has_blob Pattern (from Phase 63 ExistsRequest)
```cpp
// Source: db/storage/storage.h line 128-130
// Key-only MDBX lookup -- does not read blob data
bool has_blob(std::span<const uint8_t, 32> ns, std::span<const uint8_t, 32> hash);
```

### Existing Blob Lookup Pattern
```cpp
// Source: db/storage/storage.h line 123-125
// Returns full blob data including namespace, data, signature, TTL, timestamp
std::optional<wire::BlobData> get_blob(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> hash);
```

### Engine Ingest with Source (for notification fan-out exclusion)
```cpp
// Source: db/engine/engine.h line 104-106
// source parameter flows through to IngestResult.source for fan-out exclusion
asio::awaitable<IngestResult> ingest(
    const wire::BlobData& blob,
    std::shared_ptr<net::Connection> source = nullptr);
```

### Dispatch Pattern for Inline + Coroutine (from existing Data handler)
```cpp
// Source: db/peer/peer_manager.cpp line 1621-1680
// Pattern: co_spawn on ioc_, engine offload + post-back for IO thread state access
asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
    auto blob = wire::decode_blob(payload);
    auto result = co_await engine_.ingest(blob, conn);
    co_await asio::post(ioc_, asio::use_awaitable);  // CONC-03: transfer back
    // Now safe to access PeerManager state
    // ...
}, asio::detached);
```

### Connection Cleanup Pattern
```cpp
// Source: db/peer/peer_manager.cpp line 463-477
// on_peer_disconnected removes PeerInfo from peers_ deque
// Phase 80 should clear pending_fetches_ entries here too (or track per-peer)
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Timer-based full sync only | Push notification + targeted fetch | Phase 79-80 (v2.0.0) | Latency drops from sync_interval to near-instant |
| BlobRequest/BlobTransfer in sync session | BlobFetch/BlobFetchResponse outside sync | Phase 80 (v2.0.0) | No sync handshake overhead for single-blob fetch |

## Open Questions

1. **Pending set cleanup on not-found response**
   - What we know: Not-found response is 1 byte, carries no hash. Cannot identify which pending entry to remove.
   - What's unclear: Whether to (a) accept the leak until disconnect, (b) track `{hash -> peer_connection}` for targeted cleanup on disconnect, or (c) add the hash to the not-found response.
   - Recommendation: Option (b) -- use `std::unordered_map<std::array<uint8_t,32>, net::Connection::Ptr>` instead of `std::unordered_set`. On disconnect, erase all entries for that connection. On not-found, the entry stays until disconnect (harmless). On found, erase by hash from IngestResult. This avoids protocol format changes and handles all cleanup paths.

2. **D-01 namespace_id inclusion**
   - What we know: Storage requires namespace_id+hash compound key. No hash-only index exists. Adding one requires a new MDBX sub-database (currently 7 of 8 max_maps) and violates YAGNI.
   - What's unclear: Whether the user intended namespace_id to be implicit (from context) or explicitly excluded from the wire format.
   - Recommendation: Include namespace_id in BlobFetch payload (64 bytes). This is the only correct approach. The fetcher already has it from BlobNotify. Document this as a correction to D-01.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (latest via FetchContent) |
| Config file | CMakeLists.txt catch_discover_tests(chromatindb_tests) |
| Quick run command | `cd build && ctest -R "peer" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PUSH-05 | BlobNotify triggers BlobFetch, blob arrives at receiver | integration | `cd build && ctest -R "BlobFetch" --output-on-failure` | Wave 0 |
| PUSH-06 | BlobFetch handled inline without sync handshake | unit | `cd build && ctest -R "BlobFetch.*inline" --output-on-failure` | Wave 0 |
| WIRE-02 | BlobFetch=60 in transport.fbs, blocked by relay | unit | `cd build && ctest -R "relay.*filter" --output-on-failure` | Wave 0 |
| WIRE-03 | BlobFetchResponse=61 in transport.fbs, blocked by relay | unit | `cd build && ctest -R "relay.*filter" --output-on-failure` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "peer" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/peer/test_peer_manager.cpp` -- add test cases for BlobFetch, BlobFetchResponse, dedup, not-found, sync suppression
- [ ] `db/tests/relay/test_message_filter.cpp` -- verify types 60/61 are blocked (may already have relay filter tests)

## Sources

### Primary (HIGH confidence)
- `db/storage/storage.h` -- get_blob() and has_blob() signatures require namespace_id+hash
- `db/storage/storage.cpp` -- make_blob_key() confirmed as 64-byte compound [ns:32][hash:32]
- `db/peer/peer_manager.cpp` -- on_peer_message dispatch pattern, on_blob_ingested fan-out, on_peer_disconnected cleanup
- `db/net/connection.h` -- send_message() API, message_loop() dispatch
- `db/net/connection.cpp` -- message_loop() default case routes to message_cb_
- `db/schemas/transport.fbs` -- current enum ends at BlobNotify=59
- `relay/core/message_filter.cpp` -- blocklist switch with default:true (new types implicitly allowed)
- `db/engine/engine.h` -- ingest() with source parameter, IngestResult with ack.blob_hash
- `db/sync/sync_protocol.h` -- encode_blob_request() includes namespace_id (precedent)
- `db/wire/codec.h` -- encode_blob() / decode_blob() for FlatBuffer serialization

### Secondary (MEDIUM confidence)
- `db/tests/peer/test_peer_manager.cpp` -- Phase 79 BlobNotify test patterns (3-node setup, NotifCapture struct)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing libraries verified in source
- Architecture: HIGH -- all dispatch patterns, storage APIs, and wire formats verified against source code
- Pitfalls: HIGH -- thread safety (CONC-03), relay blocklist, pending set lifecycle all verified against existing code patterns

**Research date:** 2026-04-02
**Valid until:** 2026-05-02 (stable -- internal C++ project, no external dependency changes)
