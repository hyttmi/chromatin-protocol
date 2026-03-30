# Phase 73: Extended Queries & Pub/Sub - Context

**Gathered:** 2026-03-30
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK exposes all 38 client message types -- 10 query/introspection operations (metadata, batch exists, batch read, time range, namespace list, namespace stats, storage status, node info, peer info, delegation list) plus real-time pub/sub notifications (subscribe, unsubscribe, receive). Builds on Phase 72's client methods and Phase 71's transport layer.

</domain>

<decisions>
## Implementation Decisions

### Notification delivery
- **D-01:** Async iterator pattern -- `async for notification in conn.notifications():`. Lazy, backpressure-friendly, Pythonic. Consumes from existing transport `_notifications` queue.
- **D-02:** Single merged stream for all subscriptions. One `conn.notifications()` yields notifications across all subscribed namespaces. Each notification carries its namespace.
- **D-03:** Notification dataclass: `Notification(namespace: bytes, blob_hash: bytes, seq_num: int)`. Minimal -- user calls `read_blob()` if they want the data.

### Subscription lifecycle
- **D-04:** `subscribe()` awaits server confirmation before returning. Round-trip ensures subscription is active.
- **D-05:** Client tracks active subscriptions in a set. `conn.subscriptions` property returns frozenset. Prevents duplicate subscribes, enables clean unsubscribe-all on disconnect.
- **D-06:** Auto-cleanup on disconnect -- client sends Unsubscribe for all tracked namespaces during graceful shutdown (Goodbye sequence).

### Batch operations
- **D-07:** `list[bytes]` input for batch_exists and batch_read. Consistent with bytes-only convention (Phase 72 D-05).
- **D-08:** `batch_exists()` returns `dict[bytes, bool]` -- maps each hash to existence status. O(1) lookup by hash.
- **D-09:** `batch_read()` returns `BatchReadResult(blobs: dict[bytes, ReadResult | None], truncated: bool)`. Honest about partial results via truncation flag.
- **D-10:** No client-side batch size limits. Server enforces its own limits; truncation flag handles overflow. YAGNI.

### Query result organization
- **D-11:** All result types in single `types.py` module. Currently 5 types (Phase 72), will grow to ~18. Still manageable, one import location.
- **D-12:** Python-native types for introspection results -- `str` for version/address, `list[str]` for supported types, `int` for counts. SDK is the translation layer.
- **D-13:** All new result types exported from `chromatindb` top-level `__init__.py` in `__all__`. Consistent with Phase 72 pattern.

### Claude's Discretion
- Codec function organization for 10+ new encode/decode functions (extend _codec.py or split)
- Exact field names/types for each introspection result type (derived from C++ wire format)
- Integration test organization for 10+ new query types
- Whether metadata query returns full ReadResult or a lighter MetadataResult type
- TimeRangeResult structure (reuse ListPage pattern or new type)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Wire format protocol (binary payload encoding for each query type)
- `db/peer/peer_manager.cpp` -- Authoritative encoder for ALL response payloads. Contains encode/decode for every message type including:
  - MetadataRequest/Response, BatchExistsRequest/Response, BatchReadRequest/Response
  - TimeRangeRequest/Response, NamespaceListRequest/Response, NamespaceStatsRequest/Response
  - StorageStatusRequest/Response, NodeInfoRequest/Response, PeerInfoRequest/Response
  - DelegationListRequest/Response, Subscribe/Unsubscribe/Notification

### Node info wire format (binary, length-prefixed strings + big-endian ints)
- `db/net/node_info.h` -- NodeInfo struct definition and serialization
- `db/net/node_info.cpp` -- NodeInfo serialize/deserialize implementation

### Batch read with truncation
- `db/peer/peer_manager.cpp` -- BatchReadRequest handler with size cap and truncation flag

### Pub/sub implementation (notification payload format)
- `db/peer/peer_manager.cpp` -- Subscribe/Unsubscribe handling, Notification construction

### Existing SDK modules (Phase 70-72, already implemented)
- `sdk/python/chromatindb/client.py` -- ChromatinClient with write/read/delete/list/exists (add query + pub/sub methods here)
- `sdk/python/chromatindb/_transport.py` -- Transport with `_notifications` queue (request_id=0 routing already works)
- `sdk/python/chromatindb/_codec.py` -- Binary payload encode/decode functions (extend with new message types)
- `sdk/python/chromatindb/types.py` -- Frozen result dataclasses (add new types here)
- `sdk/python/chromatindb/wire.py` -- TransportMsgType enum with all 58 message type constants
- `sdk/python/chromatindb/generated/transport_generated.py` -- FlatBuffer TransportMsgType constants

### Known protocol details (carried from Phase 72)
- All multi-byte integers in wire payloads are big-endian (except signing input ttl/timestamp which are little-endian)
- FlatBuffers not deterministic cross-language -- use server-returned hashes
- Relay default port is 4201

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Transport._notifications` queue -- already routes request_id=0 messages (notifications). Foundation for pub/sub consumer.
- `Transport.send_request(msg_type, payload)` -- request-response correlation for all query types
- `_codec.py` pattern -- encode/decode functions per message type, struct.pack for big-endian wire integers
- `types.py` frozen dataclass pattern -- reuse for all new result types
- `client.py` `_request_with_timeout()` -- timeout wrapper for all query methods

### Established Patterns
- Phase 72: Each client method = validate args + encode via codec + send_request + check response type + decode + return typed result
- Phase 72: Codec as standalone module, one encode + one decode function per message type
- Phase 71: Transport dispatches by request_id (>0 to pending futures, 0 to notifications queue)
- Phase 72: D-16 timeout wrapping -- `asyncio.TimeoutError` caught and re-raised as `ConnectionError`

### Integration Points
- `ChromatinClient` gains 10+ new query methods + subscribe/unsubscribe/notifications
- `_codec.py` gains encode/decode functions for all new message types
- `types.py` gains ~13 new dataclasses
- `__init__.py` exports all new types
- Integration tests target KVM relay at 192.168.1.200:4201

</code_context>

<specifics>
## Specific Ideas

- The transport `_notifications` queue already works -- pub/sub just needs a consumer (async iterator) and subscribe/unsubscribe client methods
- Notification message type (21) is already in the TransportMsgType enum
- C++ Pong has request_id=0 -- transport already special-cases this. Notifications also have request_id=0 but different msg_type. Decoder must distinguish.
- PeerInfoRequest is trust-gated in C++ (full detail vs 8-byte summary) -- SDK connects via relay (loopback always trusted per Phase 67 decision), so likely gets full detail

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 73-extended-queries-pub-sub*
*Context gathered: 2026-03-30*
