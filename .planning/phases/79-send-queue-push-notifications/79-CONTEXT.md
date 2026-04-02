# Phase 79: Send Queue & Push Notifications - Context

**Gathered:** 2026-04-02
**Status:** Ready for planning

<domain>
## Phase Boundary

Per-connection send queue serializing all outbound messages to prevent AEAD nonce desync, plus BlobNotify (type 59) fan-out to all connected peers on every blob ingest. Includes source exclusion, storm suppression during active reconciliation, relay filter update, and unification of the existing pub/sub notification trigger with the new engine-level callback.

</domain>

<decisions>
## Implementation Decisions

### Send queue scope
- **D-01:** Send queue serializes ALL outbound messages on a connection (not just notifications). Every `send_message()` call goes through the queue. This fixes the existing AEAD nonce race in pub/sub `co_spawn(detached)` notifications too.
- **D-02:** Queue is internal to Connection — transparent to callers. `send_message()` enqueues and a dedicated drain coroutine serializes writes. All existing callers work unchanged.
- **D-03:** `send_message()` remains awaitable — enqueues the message and `co_await`s until the drain coroutine has written it to the socket. Caller gets backpressure signal and knows delivery succeeded.

### Slow peer policy
- **D-04:** Bounded queue with disconnect. Cap at 1024 messages. If a peer falls behind beyond the cap, disconnect it — it will recover via reconcile-on-connect (Phase 82).
- **D-05:** Simple message count cap (not byte-based). At 77 bytes per notification, 1024 = ~77 KiB. Sync blobs are larger but one-at-a-time so queue rarely holds more than a few.

### Storm suppression
- **D-06:** Suppress BlobNotify to the syncing peer only (the peer that sent the blob via sync). All other connected peers still receive BlobNotify for sync-received blobs — they need to know for fast propagation.
- **D-07:** Client-written blobs are NOT suppressed during sync. If a client writes a blob to node B while B is syncing with peer A, B still sends BlobNotify to all peers (including A). Only blobs arriving via sync from a specific peer are suppressed back to that peer.

### Notification trigger path
- **D-08:** Engine callback — `BlobEngine::ingest()` fires an `on_blob_ingested` callback on every successful ingest (client write or sync). PeerManager registers the callback and handles all fan-out. Single hook point, no missed paths.
- **D-09:** `ingest()` takes an optional `Connection::Ptr source` parameter (nullptr for client writes via relay/UDS). The callback passes it through so PeerManager can exclude the source from BlobNotify fan-out.
- **D-10:** Unify existing pub/sub Notification (client subscriptions) with BlobNotify (peer-to-peer) under the same engine callback. One callback fires on every ingest, PeerManager handles both: (1) BlobNotify fan-out to all peers, and (2) Notification to subscribed clients. Current sync_protocol callback path replaced.

### Claude's Discretion
- Queue data structure (std::deque, asio::experimental::channel, custom ring buffer)
- Drain coroutine lifecycle (started on connection init, stopped on close)
- How handshake messages bypass the queue (they run before the message loop starts, so naturally excluded)
- Error handling when queue is full (log level, disconnect message)
- How the existing `notify_subscribers()` is refactored into the unified callback

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Connection & transport
- `db/net/connection.h` -- Connection class: send_message(), send_encrypted(), send_counter_, recv_counter_, SessionKeys
- `db/net/connection.cpp` -- send_message() implementation (line ~810), send_encrypted() with nonce increment
- `db/net/framing.h` -- make_nonce(), AEAD constants, frame read/write
- `db/net/framing.cpp` -- Nonce construction, write_frame(), read_frame()
- `db/net/protocol.h` -- TransportCodec: encode/decode

### Engine & ingest
- `db/engine/engine.h` -- BlobEngine class definition
- `db/engine/engine.cpp` -- ingest() implementation (lines 102-292), where the on_blob_ingested callback should be added

### Peer management & notifications
- `db/peer/peer_manager.h` -- PeerInfo struct (syncing flag, subscribed_namespaces), encode_notification(), notify_subscribers()
- `db/peer/peer_manager.cpp` -- notify_subscribers() (lines 2923-2949), set_on_blob_ingested callback registration (lines 157-162), run_sync_with_peer() syncing flag management
- `db/sync/sync_protocol.h` -- set_on_blob_ingested() callback interface

### Wire format
- `db/schemas/transport.fbs` -- TransportMsgType enum (highest=58), add BlobNotify=59

### Relay filter
- `relay/core/message_filter.h` -- is_client_allowed() declaration
- `relay/core/message_filter.cpp` -- Blocklist switch statement, add type 59

### Requirements
- `.planning/REQUIREMENTS.md` -- PUSH-01 through PUSH-08, WIRE-01, WIRE-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `encode_notification()` in peer_manager.h — already produces the exact 77-byte payload (namespace:32 + hash:32 + seq_num_be:8 + size_be:4 + tombstone:1) that PUSH-02 requires
- `PeerInfo.syncing` flag — already tracks active sync sessions, usable for storm suppression (D-06)
- `notify_subscribers()` pattern — existing fan-out loop iterating `peers_` with subscription filtering

### Established Patterns
- `co_spawn(asio::detached)` for fire-and-forget notification sends (will be replaced by queued send_message)
- Callback registration via `set_on_blob_ingested()` on SyncProtocol — pattern to replicate on BlobEngine
- Blocklist switch in relay message_filter.cpp — add new type to existing switch

### Integration Points
- `BlobEngine::ingest()` — add optional `Connection::Ptr source` parameter, fire callback on successful ingest
- `Connection::send_message()` — convert from direct co_await send_encrypted() to queue-based
- `PeerManager` constructor — register unified engine callback instead of sync_protocol callback
- `transport.fbs` — add BlobNotify = 59 to TransportMsgType enum
- `message_filter.cpp` — add TransportMsgType_BlobNotify to blocklist

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

*Phase: 79-send-queue-push-notifications*
*Context gathered: 2026-04-02*
