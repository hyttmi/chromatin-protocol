# Phase 67: Batch/Range Queries & Integration - Context

**Gathered:** 2026-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

Clients can batch-fetch blobs, query peers, query by time range, and all v1.4.0 types are fully integrated across relay, NodeInfo, and documentation. Three new request/response message type pairs (types 53-58) plus milestone-wide integration work (relay filter, NodeInfo supported_types, PROTOCOL.md).

This is the final phase of v1.4.0.

</domain>

<decisions>
## Implementation Decisions

### Claude's Discretion — All Areas

All implementation decisions for this phase are Claude's discretion. The user trusts sensible defaults following established patterns from Phase 65/66. Below are the recommended defaults that Claude should follow:

### BatchReadRequest/BatchReadResponse (QUERY-13)
- **D-01:** Namespace-scoped (one namespace, multiple hashes) — consistent with BatchExistsRequest pattern.
- **D-02:** Request payload: [namespace:32][cap_bytes:4 big-endian][count:4 big-endian][hash_0:32]...[hash_N:32]. Minimum 40 bytes.
- **D-03:** Cumulative response size cap: client-specified via `cap_bytes` field, maximum 4 MiB (4194304), default to 4 MiB if client sends 0 or exceeds max. Bounds memory usage per request.
- **D-04:** Count limit: maximum 256 hashes per request. Strike on >256 or count=0.
- **D-05:** Partial-result semantics: include the blob that crosses the cap (don't leave it half-done), then stop. Response prefix: [truncated:1 byte (0x00=complete, 0x01=truncated)][count:4 big-endian], followed by count entries. The truncated flag tells the client more blobs were requested but cap was reached.
- **D-06:** Per-blob response entry: [status:1 (0x00=not_found, 0x01=found)][hash:32][size:8 big-endian][data:size bytes] for found blobs. Not-found blobs: [status:1 (0x00)][hash:32] — no data bytes. Preserves request order so client can correlate.
- **D-07:** Uses `storage_.get_blob()` per hash — same as ReadRequest but batched with cumulative size tracking.

### PeerInfoRequest/PeerInfoResponse (QUERY-09)
- **D-08:** Request payload: empty (0 bytes valid). No parameters needed.
- **D-09:** Trust-gated response tiers:
  - **Trusted/UDS:** Full detail — per-peer entries with [address (length-prefixed string)][is_bootstrap:1][syncing:1][peer_is_full:1][connected_duration_ms:8 big-endian].
  - **Untrusted:** Reduced — only [peer_count:4 big-endian][bootstrap_count:4 big-endian]. No individual peer addresses or state (prevents network topology mapping by untrusted clients).
- **D-10:** Trust detection: reuse existing `is_trusted_address()` + UDS detection from connection context (same pattern as ACL checks).
- **D-11:** Trusted response format: [peer_count:4 big-endian][bootstrap_count:4 big-endian] followed by peer_count entries of per-peer data. Connected duration computed as `steady_clock::now() - last_message_time` (approximation, not exact connect time).

### TimeRangeRequest/TimeRangeResponse (QUERY-14)
- **D-12:** Request payload: [namespace:32][start_timestamp:8 big-endian][end_timestamp:8 big-endian][limit:4 big-endian]. Minimum 52 bytes. Timestamps are microseconds (matching blob timestamp format).
- **D-13:** No timestamp index exists (explicitly Out of Scope). Implementation: scan seq_map for namespace, read each blob's timestamp from blob_map, filter by range. Bounded by both result limit and scan limit.
- **D-14:** Result limit: client-specified via `limit` field, capped at 100 (same as ListRequest). Default to 100 if client sends 0 or exceeds max.
- **D-15:** Scan limit: stop after scanning 10,000 seq_map entries even if result limit not reached. Response includes [truncated:1 byte] to indicate scan was bounded. Prevents unbounded scan on huge namespaces.
- **D-16:** Response format: [truncated:1][count:4 big-endian] followed by count entries of [blob_hash:32][seq_num:8 big-endian][timestamp:8 big-endian]. Returns references (not full blobs) — client uses ReadRequest or BatchReadRequest to fetch data.
- **D-17:** Validation: strike if start_timestamp > end_timestamp (invalid range).

### Integration (INTEG-01 through INTEG-04)
- **D-18:** Relay filter: add types 53-58 to `is_client_allowed()`, bringing total to 38 client-allowed types.
- **D-19:** NodeInfoResponse `supported[]` array: add all 18 new v1.4.0 types (41-58), bringing the array from 20 to 38 entries.
- **D-20:** PROTOCOL.md: add a new "## v1.4.0 Query Extensions" section documenting wire format for all 10 new request/response pairs (types 41-58) from Phases 65-67. Table format per message type: type enum value, direction, payload layout, field descriptions.
- **D-21:** INTEG-04 (relay forwards all response types): verified by existing relay architecture — relay forwards any message type from node to client without inspection. No code changes needed, just confirmation via test or documentation.

</decisions>

<specifics>
## Specific Ideas

No specific requirements — follow established Phase 65/66 handler patterns. This phase has more breadth (3 handlers + integration) than depth, so plans should be structured to parallelize the integration work.

</specifics>

<canonical_refs>
## Canonical References

### Protocol and wire format
- `db/PROTOCOL.md` — Current wire format documentation, needs v1.4.0 section added
- `db/schemas/transport.fbs` — FlatBuffers schema, types 53-58 available for Phase 67

### Existing query patterns
- `db/peer/peer_manager.cpp` — All Phase 65/66 handlers as templates (lines 836-1291)
- `db/peer/peer_manager.cpp` lines 886-891 — NodeInfoResponse `supported[]` array (needs update for INTEG-02)

### Storage API
- `db/storage/storage.h` — `get_blob()`, `get_blobs_by_seq()`, `get_blob_refs_since()` for BatchRead/TimeRange
- `db/wire/codec.h` — BlobData struct with timestamp field (microseconds)

### Peer management
- `db/peer/peer_manager.h` lines 46-64 — PeerInfo struct fields (address, is_bootstrap, syncing, peer_is_full, bucket_tokens, last_message_time)
- `db/peer/peer_manager.h` line 164 — `is_trusted_address()` for trust-gating

### Relay integration
- `relay/core/message_filter.cpp` — `is_client_allowed()` with 32 types currently
- `db/tests/relay/test_message_filter.cpp` — relay filter test coverage

### Requirements
- `.planning/REQUIREMENTS.md` — QUERY-09, QUERY-13, QUERY-14, INTEG-01, INTEG-02, INTEG-03, INTEG-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `storage_.get_blob(ns, hash)` — for BatchRead, same as ReadRequest but batched
- `storage_.get_blob_refs_since(ns, since_seq, max_count)` — for TimeRange scan backbone (reads seq_map only)
- `storage_.get_blobs_by_seq(ns, since_seq)` — alternative for TimeRange (returns full BlobData with timestamps)
- `is_trusted_address()` — trust detection for PeerInfo gating
- `peers_` deque — iteration for PeerInfo response
- `peer_count()`, `bootstrap_peer_count()` — for untrusted PeerInfo tier

### Established Patterns
- Coroutine-IO dispatch for all read-only query handlers
- Binary response building with big-endian encoding and manual offset tracking
- Step 0 payload size validation + record_strike() on malformed input
- request_id echoed in all responses
- Relay filter: switch/case in is_client_allowed(), test with exact count assertion

### Integration Points
- `PeerManager::on_peer_message()` — 3 new handler blocks
- `transport.fbs` — 6 new enum values (types 53-58)
- `NodeInfoResponse` handler — expand `supported[]` array (INTEG-02)
- `is_client_allowed()` — add 6 new types (INTEG-01)
- `PROTOCOL.md` — new section for all v1.4.0 types (INTEG-03)

</code_context>

<deferred>
## Deferred Ideas

- Timestamp sub-database index for O(1) time-range queries — reconsider if profiling shows seq_map scan is a bottleneck at real-world namespace sizes
- Streaming query results — paginated responses with limits are sufficient for v1.4.0

</deferred>

---

*Phase: 67-batch-range-queries-and-integration*
*Context gathered: 2026-03-26*
