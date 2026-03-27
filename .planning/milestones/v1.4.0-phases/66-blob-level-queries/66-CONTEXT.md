# Phase 66: Blob-Level Queries - Context

**Gathered:** 2026-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

Clients can inspect individual blob metadata, check batch existence, and list delegations without transferring payload data. Three new request/response message type pairs (6 enum values, types 47-52) using existing storage methods plus minor additions.

</domain>

<decisions>
## Implementation Decisions

### MetadataRequest/MetadataResponse
- **D-01:** Use existing `storage_.get_blob()` to fetch the full blob, then strip the payload data from the response. No new storage method needed — safest approach, reuses proven code path.
- **D-02:** Response fields: blob_hash (32B), timestamp (8B, big-endian), ttl (4B, big-endian), size (8B, big-endian = raw `data.size()`), signer_pubkey (length-prefixed, full ML-DSA-87 key). Seq_num included (8B, big-endian) — cheap to retrieve and useful for clients.
- **D-03:** Size field is raw data size (what the writer sent), not encrypted envelope size. Clients care about logical size, not storage internals.
- **D-04:** Not-found: return a status byte (0x00 = not found, 0x01 = found) as first byte of response. On not-found, response is just the 1-byte status. On found, status byte followed by all metadata fields.
- **D-05:** Request payload: [namespace:32][blob_hash:32] = 64 bytes minimum.

### BatchExistsRequest/BatchExistsResponse
- **D-06:** Namespace-scoped: one namespace per request, multiple blob hashes. NOT cross-namespace.
- **D-07:** Request payload: [namespace:32][count:4 big-endian][hash_0:32][hash_1:32]...[hash_N:32]. Minimum 36 bytes (namespace + count).
- **D-08:** Limit: maximum 1024 hashes per request. Strike on >1024 (malformed input, not silent truncation).
- **D-09:** Response: byte-per-hash in request order. Response[i] = 0x01 if hash[i] exists, 0x00 otherwise. Result order matches request order — result[i] corresponds to request hash[i].
- **D-10:** Zero hashes (count=0): strike and drop — meaningless request.
- **D-11:** Uses `storage_.has_blob()` per hash — O(1) each, no payload data touched.

### DelegationList request/response
- **D-12:** Request payload: [namespace:32] = 32 bytes minimum. No pagination — delegation counts per namespace are small (practical limit is a handful, not thousands).
- **D-13:** Response: [count:4 big-endian] followed by count entries of [delegate_pk_hash:32][delegation_blob_hash:32] per entry. Returns hashes, not raw pubkeys — the delegation_map already stores `SHA3-256(delegate_pubkey)` as the key.
- **D-14:** Only active delegations returned. Tombstoned (revoked) delegations are already removed from delegation_map on tombstone ingest, so the cursor scan naturally excludes them.
- **D-15:** Uses cursor prefix scan on delegation_map (same pattern as `count_delegations()` but collecting entries instead of counting).

### Claude's Discretion
- Wire format byte layout details beyond what's specified above
- Error handling edge cases (malformed payload details)
- Test structure and coverage strategy
- Whether to extract a helper for delegation_map prefix scanning (count vs list share logic)

</decisions>

<specifics>
## Specific Ideas

No specific requirements beyond the decisions above. Follow the Phase 65 handler patterns (NamespaceListRequest, StorageStatusRequest, NamespaceStatsRequest) as templates for dispatch, validation, and response building.

</specifics>

<canonical_refs>
## Canonical References

### Protocol and wire format
- `db/PROTOCOL.md` — Current wire format documentation, message type registry
- `db/schemas/transport.fbs` — FlatBuffers schema, types 47-52 available for Phase 66

### Existing query patterns (template for new handlers)
- `db/peer/peer_manager.cpp` lines 836-861 — ExistsRequest handler (simplest co_spawn pattern, 64-byte input, binary response)
- `db/peer/peer_manager.cpp` lines 950-1022 — NamespaceListRequest handler (pagination, sorted results)
- `db/peer/peer_manager.cpp` lines 1071-1128 — NamespaceStatsRequest handler (namespace-scoped query with count_delegations call)

### Storage API
- `db/storage/storage.h` — `get_blob()`, `has_blob()`, `count_delegations()`, delegation_map layout
- `db/storage/storage.cpp` lines 954-984 — `count_delegations()` cursor prefix scan pattern (reuse for DelegationList)
- `db/wire/codec.h` — BlobData struct: namespace_id, pubkey, data, ttl, timestamp, signature

### Relay integration
- `db/tests/relay/test_message_filter.cpp` — `is_client_allowed()` test coverage, new types need entries

### Requirements
- `.planning/REQUIREMENTS.md` — QUERY-10, QUERY-11, QUERY-12

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `storage_.get_blob(ns, hash)` — returns full BlobData for MetadataRequest (strip data + signature from response)
- `storage_.has_blob(ns, hash)` — O(1) key-only lookup for BatchExists
- `count_delegations()` cursor prefix scan — same pattern for listing delegations, but collect entries instead of counting
- Phase 65 handler patterns — co_spawn dispatch, big-endian encoding, request_id echo, strike on malformed

### Established Patterns
- Coroutine-IO dispatch: `asio::co_spawn(ioc_, [...] -> asio::awaitable<void> { ... }, asio::detached)`
- Binary response building: manual offset tracking with big-endian encoding
- Input validation: size check as Step 0, `record_strike()` on malformed
- request_id echoed via `conn->send_message(type, payload, request_id)`

### Integration Points
- `PeerManager::on_peer_message()` — new `if (type == ...)` blocks after Phase 65 handlers
- `transport.fbs` — add types 47-52 (MetadataRequest/Response, BatchExistsRequest/Response, DelegationListRequest/Response)
- Relay `is_client_allowed()` — add 6 new types to allow list
- NodeInfoResponse `supported[]` — deferred to Phase 67 (INTEG-02)

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 66-blob-level-queries*
*Context gathered: 2026-03-26*
