# Phase 14: Pub/Sub Notifications - Context

**Gathered:** 2026-03-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Connected peers receive real-time notifications when blobs are ingested or deleted in namespaces they subscribe to. Subscriptions are connection-scoped (no persistence). Requirements: SUB-01 through SUB-05.

</domain>

<decisions>
## Implementation Decisions

### Notification content
- Fields: namespace_id (32 bytes) + blob_hash (32 bytes) + seq_num (uint64) + blob_size (uint32) + is_tombstone (bool)
- No delegate/writer pubkey in notifications — subscriber fetches blob if they need to know who wrote it
- For tombstone notifications, blob_hash is the tombstone's own hash (not the deleted blob's hash) — uniform "blob_hash = what was stored" semantic
- Subscriber discovers the deleted blob hash from the tombstone data if needed

### Subscription granularity
- SUBSCRIBE message carries a list of namespace_ids (batch subscribe in one message)
- Additive/merge semantics: subsequent SUBSCRIBEs add to existing subscription set, don't replace
- UNSUBSCRIBE mirrors SUBSCRIBE: message with list of namespace_ids to remove
- No subscribe-all wildcard — explicit namespaces only

### Notification delivery
- Fire-and-forget: node sends notification, no ACK required. Missed notifications caught up via sync
- Async fan-out: ingest completes and returns WriteAck immediately; notification dispatch is a separate async step that doesn't block the ingest path
- Best-effort ordering: notifications go out in ingest order naturally; no explicit protocol ordering guarantees. Subscriber uses seq_num to sort

### Trigger scope
- Notify on ALL ingests — direct writes and sync-received blobs both trigger notifications
- Notify all subscribers including the connection that wrote the blob (uniform, no self-exclusion)
- No source distinction in notifications — subscriber doesn't know if blob arrived via sync or direct write
- Subscribe/unsubscribe allowed anytime after handshake, independent of sync sessions

### Claude's Discretion
- Subscription limit per connection (if any)
- Backpressure strategy for slow subscribers (bounded buffer vs inline send)
- Wire format details for SUBSCRIBE/UNSUBSCRIBE/NOTIFICATION FlatBuffer tables
- New TransportMsgType enum values for Subscribe, Unsubscribe, Notification

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `TransportMsgType` enum in `schemas/transport.fbs`: add Subscribe, Unsubscribe, Notification values
- `TransportCodec` in `db/net/protocol.h`: encode/decode for new message types (same pattern as existing)
- `PeerInfo` struct in `db/peer/peer_manager.h`: attach subscription set here (per-connection state)
- `BlobEngine::ingest()` and `delete_blob()` in `db/engine/engine.h`: trigger points for notification dispatch

### Established Patterns
- Timer-cancel pattern for async message queues (used in sync_inbox) — applicable for notification dispatch
- Message routing via `on_peer_message()` in PeerManager — add Subscribe/Unsubscribe handling here
- FlatBuffers for wire format with deterministic encoding
- PEX encoding pattern (`encode_peer_list`/`decode_peer_list`) as model for subscription list encoding

### Integration Points
- `PeerManager::on_peer_message()` — route new Subscribe/Unsubscribe message types
- `PeerManager::on_peer_disconnected()` — clean up subscription state (connection-scoped)
- `BlobEngine::ingest()` / `delete_blob()` — hook for notification dispatch (callback or observer pattern)
- `schemas/transport.fbs` — add new enum values
- `db/wire/codec.h` — new FlatBuffer table for notification payload

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 14-pub-sub-notifications*
*Context gathered: 2026-03-08*
