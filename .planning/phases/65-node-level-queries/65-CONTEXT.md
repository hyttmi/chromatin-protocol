# Phase 65: Node-Level Queries - Context

**Gathered:** 2026-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

Operators and clients can enumerate namespaces, query storage status, and query per-namespace statistics. Three new request/response message type pairs (6 enum values, types 41-46) using existing storage methods plus minor new ones.

HealthRequest was cut — NodeInfoResponse (Phase 63) already returns uptime, version, peer count, storage used/max. A dedicated health endpoint would be redundant; if the node responds to NodeInfo, it's healthy.

</domain>

<decisions>
## Implementation Decisions

### Scope reduction
- **D-01:** HealthRequest/HealthResponse (QUERY-05) is CUT as redundant with NodeInfoResponse. Phase 65 delivers 3 message type pairs, not 4.
- **D-02:** REQUIREMENTS.md QUERY-05 should be marked as dropped with rationale: "NodeInfoResponse already serves as health check."

### Claude's Discretion
- NamespaceList response shape: fields per entry, limit cap, has_more flag, stale cursor behavior
- StorageStatus fields: global tombstone count approach, quota headroom representation (max+used vs computed remaining)
- NamespaceStats fields: whether to include latest_seq_num, delegation count implementation, not-found response behavior
- Wire format details for all three response types
- New Storage methods needed (tombstone count, delegation count per namespace)
- Default/max pagination limits

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. Follow the NodeInfoResponse and ExistsRequest patterns from Phase 63 as templates for wire format, dispatch, and handler structure.

</specifics>

<canonical_refs>
## Canonical References

### Protocol and wire format
- `db/PROTOCOL.md` — Current wire format documentation, message type registry
- `db/schemas/transport.fbs` — FlatBuffers schema with TransportMsgType enum (types 0-40 allocated)

### Existing query patterns (template for new handlers)
- `db/peer/peer_manager.cpp` lines 836-948 — ExistsRequest and NodeInfoRequest handlers (co_spawn dispatch, binary response building, error handling pattern)
- `db/wire/transport_generated.h` — Generated enum, types 41+ available for v1.4.0

### Storage API
- `db/storage/storage.h` — `list_namespaces()`, `get_namespace_quota()`, `used_data_bytes()`, `used_bytes()`, `has_tombstone_for()`
- `db/engine/engine.h` — `list_namespaces()` engine wrapper

### Relay integration
- `db/tests/relay/test_message_filter.cpp` — `is_client_allowed()` test coverage, new types need entries

### Requirements
- `.planning/REQUIREMENTS.md` — QUERY-05 (dropped), QUERY-06, QUERY-07, QUERY-08

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `storage_.list_namespaces()` — returns `vector<NamespaceInfo>` with `{namespace_id, latest_seq_num}`, cursor-jump scan over seq_map
- `storage_.get_namespace_quota(ns)` — returns `{total_bytes, blob_count}` per namespace, O(1) read
- `storage_.used_data_bytes()` — actual B-tree occupancy (accurate for reporting)
- `storage_.used_bytes()` — mmap geometry size
- `config_.max_storage_bytes` — global storage cap (0 = unlimited)
- `compute_uptime_seconds()`, `peer_count()` — available in PeerManager
- No existing: global tombstone count method, delegation count per namespace method — new Storage methods needed

### Established Patterns
- Coroutine-IO dispatch: `asio::co_spawn(ioc_, [...] -> asio::awaitable<void> { ... }, asio::detached)` for all read-only queries
- Binary response building: manual offset tracking with big-endian encoding (see NodeInfoResponse handler)
- Input validation: size check as Step 0, `record_strike()` on malformed input
- request_id echoed via `conn->send_message(type, payload, request_id)`

### Integration Points
- `PeerManager::on_peer_message()` — new `if (type == ...)` blocks after existing ExistsRequest/NodeInfoRequest handlers
- `transport.fbs` — new enum values (regenerate `transport_generated.h`)
- Relay `is_client_allowed()` — add new types to allow list
- NodeInfoResponse `supported[]` array — add new types (done in Phase 67 per INTEG-02)

</code_context>

<deferred>
## Deferred Ideas

- HealthRequest with startup-aware readiness concept — reconsider if operator monitoring needs arise (currently: NodeInfo suffices)

</deferred>

---

*Phase: 65-node-level-queries*
*Context gathered: 2026-03-26*
