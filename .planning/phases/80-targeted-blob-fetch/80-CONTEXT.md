# Phase 80: Targeted Blob Fetch - Context

**Gathered:** 2026-04-02
**Status:** Ready for planning

<domain>
## Phase Boundary

Lightweight blob-by-hash fetch after receiving a BlobNotify. Two new message types (BlobFetch=60, BlobFetchResponse=61) handled inline in the message loop. Peers check local storage before fetching, track in-flight requests to dedup concurrent notifications, and suppress fetches during active sync sessions. No full reconciliation triggered.

</domain>

<decisions>
## Implementation Decisions

### BlobFetch wire format
- **D-01:** BlobFetch request is hash-only (32 bytes). The SHA3-256 hash from BlobNotify is the sole identifier. Responder looks up by hash.
- **D-02:** BlobFetchResponse returns the full signed blob exactly as stored (FlatBuffer with namespace, data, signature, TTL, timestamp). Receiver ingests it identically to a sync blob via the existing `ingest()` path.
- **D-03:** Status byte prefix on BlobFetchResponse: 0=found (rest is blob), 1=not-found (just that one byte). Clean, no ambiguity.

### Receive-side dedup logic
- **D-04:** On BlobNotify receipt, check `storage_.has_blob()` (existing key-only MDBX lookup). If blob exists locally, skip fetch entirely.
- **D-05:** Track pending fetches in a hash set. If a second BlobNotify arrives for the same hash while a fetch is in-flight, skip it. Remove hash from set after ingest completes or fails.

### Fetch trigger placement
- **D-06:** New `PeerManager::on_blob_notify()` handler dispatched from the message loop when BlobNotify arrives. PeerManager owns the dedup set and has access to engine for `has_blob()`. Consistent with existing dispatch patterns.
- **D-07:** Suppress BlobFetch during active sync with the same peer. If `PeerInfo.syncing` is true for the notifying peer, skip the fetch — full reconciliation will transfer everything. Avoids redundant fetches.

### Failure handling
- **D-08:** Not-found response: silent drop. Log at debug level, remove from pending set. Full reconciliation (Phase 82) handles missed blobs.
- **D-09:** No timeout timer for BlobFetch. Fire and forget — remove from pending set when response arrives or connection drops. Reconcile-on-reconnect handles missed blobs.
- **D-10:** Failed ingestion (signature verification failure): log warning, drop the blob. No disconnect, no retry, no strike. Same as current sync error handling.

### Claude's Discretion
- BlobFetch handler placement in the dispatch switch (inline vs case delegation)
- Pending fetch set data structure (std::unordered_set, flat_hash_set, etc.)
- How BlobFetchResponse handler routes to ingest (direct call vs reuse of existing sync ingest path)
- Relay filter update for types 60/61 (WIRE-04 already pre-blocks 59-61, verify and add explicit cases)
- Cleanup of pending set entries on connection close

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Connection & transport
- `db/net/connection.h` — Connection class: send_message() (queue-based), message types
- `db/net/connection.cpp` — send_message() via queue, message_loop() dispatch switch
- `db/net/framing.h` — make_nonce(), AEAD constants, frame read/write

### Engine & storage
- `db/engine/engine.h` — BlobEngine: ingest(), has_blob(), get_blob()
- `db/engine/engine.cpp` — ingest() with source parameter, IngestResult

### Peer management & notifications
- `db/peer/peer_manager.h` — PeerInfo (syncing flag), on_blob_ingested(), encode_notification()
- `db/peer/peer_manager.cpp` — on_blob_ingested() fan-out (line ~2926), message dispatch, sync session management

### Wire format
- `db/schemas/transport.fbs` — TransportMsgType enum (BlobNotify=59, add BlobFetch=60, BlobFetchResponse=61)

### Relay filter
- `relay/core/message_filter.cpp` — Blocklist switch (verify types 60/61 are blocked)

### Requirements
- `.planning/REQUIREMENTS.md` — PUSH-05, PUSH-06, WIRE-02, WIRE-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `storage_.has_blob()` — existing key-only MDBX lookup for dedup check (Phase 63 ExistsRequest)
- `engine_.ingest()` — accepts full signed blob, returns IngestResult with Stored/Duplicate/Expired/etc.
- `engine_.get_blob()` — retrieves full blob by namespace+hash for BlobFetchResponse
- `encode_notification()` in peer_manager.h — 77-byte payload containing the hash needed for BlobFetch
- `PeerInfo.syncing` flag — already tracks active sync sessions, usable for fetch suppression (D-07)

### Established Patterns
- Message dispatch: switch on TransportMsgType in PeerManager, delegate to handler methods
- Send via `conn->send_message(type, payload)` — goes through queue (Phase 79)
- Sync blob transfer: BlobRequest(hash) → BlobTransfer(blob) pattern in sync_protocol — similar to BlobFetch/BlobFetchResponse

### Integration Points
- `transport.fbs` — add BlobFetch=60, BlobFetchResponse=61 to enum
- `PeerManager` message dispatch — add cases for types 60/61
- `on_blob_ingested()` fan-out — where BlobNotify is sent; the receive side needs a new on_blob_notify() handler
- `message_filter.cpp` — verify types 60/61 are blocked (may already be covered by WIRE-04 range)

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 80-targeted-blob-fetch*
*Context gathered: 2026-04-02*
