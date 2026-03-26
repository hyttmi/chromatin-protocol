# Phase 63: Query Extensions - Context

**Gathered:** 2026-03-25
**Status:** Ready for planning

<domain>
## Phase Boundary

Add ExistsRequest/ExistsResponse and NodeInfoRequest/NodeInfoResponse message types. Update relay message filter to allow them through. Storage already has `has_blob()` — no new storage methods needed.

</domain>

<decisions>
## Implementation Decisions

### Exists semantics
- **D-01:** ExistsResponse returns false for tombstoned blobs. The client is asking "can I fetch this?" — deleted blobs are gone. Matches ReadRequest behavior (returns not-found for tombstoned blobs).
- **D-02:** ExistsRequest/ExistsResponse are CONC-04 cheap ops — IO-thread co_spawn, no thread pool offload. Same pattern as ReadRequest but simpler response.

### NodeInfo visibility
- **D-03:** NodeInfoResponse is identical regardless of ACL mode. The client already passed handshake + ACL check to send the request — they're authorized. No field filtering.
- **D-04:** NodeInfoRequest/NodeInfoResponse are CONC-04 cheap ops — IO-thread co_spawn gathering local state (version, metrics, peer count, storage stats).

### Supported types scope
- **D-05:** The `supported_types` list in NodeInfoResponse contains only client-facing types (the set the relay filter allows through). Sync/PEX/handshake types are excluded — SDKs use this for feature detection, not protocol internals.

### Claude's Discretion
- Wire format layout (field ordering, encoding)
- Exact co_spawn vs inline dispatch choice
- Test structure and coverage approach
- Error handling for malformed requests (follow existing record_strike pattern)

</decisions>

<specifics>
## Specific Ideas

No specific requirements — follow existing ReadRequest/StatsRequest patterns for wire format and dispatch.

</specifics>

<canonical_refs>
## Canonical References

### Requirements
- `.planning/REQUIREMENTS.md` — QUERY-01 through QUERY-04 define the four requirements for this phase

### Prior phase decisions
- `.planning/phases/61-transport-foundation/61-CONTEXT.md` — D-05 (single dispatcher), D-06 (echo request_id), D-09 (server-initiated uses request_id=0)
- `.planning/phases/62-concurrent-dispatch/62-RESEARCH.md` — CONC-04 establishes inline pattern for ExistsRequest and NodeInfoRequest

### Implementation patterns
- `db/peer/peer_manager.cpp` — ReadRequest handler (~line 716) is the template for ExistsRequest; StatsRequest handler is the template for NodeInfoRequest
- `db/storage/storage.h` — `has_blob()` already exists (QUERY-02 satisfied)
- `relay/core/message_filter.cpp` — `is_client_allowed()` switch to add 4 cases
- `db/version.h` — Build-time version string and git hash for NodeInfoResponse

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Storage::has_blob()` — key-existence check without reading blob value, ready for ExistsRequest
- `Engine::list_namespaces()` — namespace count for NodeInfoResponse
- `PeerManager::peer_count()` — connected peer count
- `PeerManager::compute_uptime_seconds()` — uptime calculation
- `NodeMetrics` struct — ingests, syncs, rejections counters
- `Storage::used_data_bytes()` — accurate B-tree storage metric
- `db/version.h` — VERSION and CHROMATINDB_GIT_HASH compile-time constants

### Established Patterns
- Request handlers: co_spawn(ioc_, ...) with payload parsing, try/catch, record_strike on error
- Wire format: big-endian multi-byte integers, fixed-size binary payloads
- Response sending: `co_await conn->send_message(type, payload, request_id)`
- Relay filter: default-deny switch statement in `is_client_allowed()`

### Integration Points
- `on_peer_message` dispatch in peer_manager.cpp — add ExistsRequest + NodeInfoRequest cases
- `is_client_allowed()` in message_filter.cpp — add 4 new type cases
- `transport.fbs` enum — add types 37-40
- Test files: test_peer_manager.cpp + test_message_filter.cpp

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 63-query-extensions*
*Context gathered: 2026-03-25*
