# Feature Research: Event-Driven Sync for Distributed Blob Stores

**Domain:** Push-based replication and event-driven maintenance in distributed blob stores
**Researched:** 2026-04-02
**Confidence:** HIGH (patterns well-established across CouchDB, Syncthing, Cassandra, IPFS, Redis)

## Feature Landscape

### Table Stakes (Users Expect These)

Features that any push-based sync system must have. Without these, the event-driven architecture is incomplete or fragile.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Push notification on blob ingest | Core value prop of event-driven sync. CouchDB continuous changes feed, Syncthing Index Update, IPFS Bitswap wantlist fulfillment -- all notify peers immediately on new data. Without this, "event-driven" is marketing. | MEDIUM | New wire type: BlobNotify (namespace:32 + blob_hash:32 + seq_num:8 + blob_size:4 + is_tombstone:1). Sent to all connected peers on ingest. Reuse existing Notification payload format (already 77 bytes, identical structure). Peer-to-peer analog of client-facing Notification type 21. |
| Targeted blob fetch from notification | Peer receives BlobNotify, fetches that specific blob without full reconciliation. IPFS Bitswap: wantlist -> block response. CouchDB: changes feed -> doc fetch. This is the "O(1) fetch" that replaces "O(diff) reconciliation" for real-time updates. | MEDIUM | New wire types: BlobFetchRequest (namespace:32 + blob_hash:32) and BlobFetchResponse (reuse single-blob-transfer format). Leverage existing SyncProtocol::get_blobs_by_hashes() for lookup. Must handle "blob not found" (peer may have expired/deleted it). |
| Reconcile-on-connect (initial sync) | Every distributed system reconciles state on connection establishment. Syncthing sends full Index on connect. Cassandra uses anti-entropy on node join. CouchDB replicates from last_seq. Without this, peers that reconnect after downtime have stale data until the next periodic sync (10-15 min is too long). | LOW | Already built: run_sync_with_peer() fires on_peer_connected -> on_ready. Just need to keep this behavior as the PRIMARY catchup mechanism. No new code, but must be explicitly preserved in the new architecture. |
| Safety-net periodic reconciliation | All production systems keep a fallback anti-entropy mechanism. Cassandra: nodetool repair. CouchDB: periodic replication check. Syncthing: periodic full index rescan. The push path can lose messages (TCP buffer full, transient disconnect, race conditions). A 10-15 min background reconciliation catches anything missed. | LOW | Reuse existing sync_timer_loop() but increase interval from 60s to 600-900s. This becomes a monitoring signal, not the primary sync path. Log when safety-net finds discrepancies (indicates push path bugs). |
| Application-level keepalive (Ping/Pong heartbeat) | RabbitMQ, gRPC, WebSocket protocols all use application-level heartbeats because TCP keepalive defaults are too slow (2 hours on Linux). Dead connection detection in <30s requires app-level pings. Bidirectional: both sides send, both sides monitor. | MEDIUM | New bidirectional heartbeat. Currently the C++ node only does receiver-side inactivity timeout (120s, no pings sent). Must add: node sends periodic Ping (e.g., every 15s), expects Pong within timeout. Existing Ping/Pong wire types (5/6) already exist. Must be careful about AEAD nonce interaction -- existing decision "receiver-side inactivity (not Ping sender)" was to avoid nonce desync, but with sequential send_counter_ serialization this is safe. |
| SDK auto-reconnect with exponential backoff | PubNub, websockets library, gRPC -- all production SDKs auto-reconnect transparently. Without this, every connection drop requires manual application-level retry. Users expect `async with` to "just work" across transient failures. | HIGH | Requires reworking ChromatinClient from single-use context manager to persistent connection with reconnect loop. Jittered exponential backoff (1s min, 30s max). Must re-establish PQ handshake on each reconnect (new AEAD session). Restore subscriptions after reconnect. Highest complexity feature because it touches transport, handshake, and client layers. |
| SDK subscription restoration on reconnect | WebSocket best practices, PubNub SDK, RingCentral -- all restore subscriptions automatically after reconnect. Without this, any reconnect silently drops pub/sub subscriptions and the application misses notifications with no error. | MEDIUM | Track `_subscriptions` set (already done in client.py). After successful reconnect + handshake, re-send Subscribe messages for all tracked namespaces. Must happen before returning control to caller. Gap between disconnect and reconnect means missed notifications -- document this limitation. |

### Differentiators (Competitive Advantage)

Features that go beyond table stakes. Not required for correctness, but provide measurable quality-of-life or performance benefits.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Event-driven expiry (next-expiry timer) | Replace periodic full-table scan with targeted timer set to the soonest expiring blob. Redis uses lazy expiry + periodic sampling. CouchDB doesn't expire at all. Most systems scan periodically. A next-expiry timer is more CPU-efficient: sleep until the next expiry, process it, set timer to the next one. Especially valuable when most blobs are permanent (TTL=0). | MEDIUM | Requires Storage to expose get_next_expiry_timestamp(). On blob store: if new blob's expiry < current timer target, reset timer. On expiry event: process expired blob, query next expiry, set new timer. Edge case: bulk inserts may thrash the timer -- use a small coalescing window (100ms). Falls back to periodic scan if no TTL blobs exist. |
| Disconnect-triggered cursor cleanup | Replace 6-hour cursor compaction timer with immediate cleanup on peer disconnect. Currently cursors for disconnected peers accumulate for up to 6 hours. Immediate cleanup reduces memory waste and database entries. | LOW | Hook into on_peer_disconnected(). Call storage_.delete_cursors_for_peer(peer_pubkey_hash). Simple, no timer needed. The 6h timer was a YAGNI compromise when event-driven cleanup wasn't available -- now it is. |
| Connection-scoped push state | Track which blobs each peer has been notified about. Prevents redundant notifications and enables smart push decisions. Syncthing tracks per-peer index state. IPFS Bitswap tracks per-peer wantlists. | HIGH | Not recommended for v2.0.0. The existing reconciliation protocol handles de-duplication on connect. Push notifications are cheap (77 bytes). Duplicate fetch requests are detected by content-addressing. The complexity of per-peer bloom filters or notification bitmaps is not justified yet. |
| Notification coalescing / batching | Batch multiple BlobNotify messages into a single frame when multiple blobs arrive in rapid succession. Reduces frame overhead and AEAD encryption operations. gRPC supports message batching. Syncthing batches Index Update messages. | LOW | Simple coalescing: collect notifications for 10-50ms, send as batch. Wire format: [count:4][notification:77]*N. Low complexity, measurable throughput improvement during bulk ingest. Defer to v2.1 if not needed for correctness. |

### Anti-Features (Commonly Requested, Often Problematic)

Features that seem good but create problems in this specific context.

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| Guaranteed delivery / message queue for push | "What if a peer misses a notification?" | Adding delivery guarantees (ACKs, retry queues, sequence tracking per peer) turns a simple push into a complex message broker. Cassandra's hinted handoff requires per-target hint storage, replay logic, and garbage collection. Redis Pub/Sub explicitly chose fire-and-forget because reliable delivery adds enormous complexity. The safety-net reconciliation catches any missed pushes within 10-15 minutes. | Fire-and-forget push + reconcile-on-connect + safety-net periodic reconciliation. Three-layer defense is simpler and more robust than guaranteed delivery. |
| Gossip protocol for propagation | "Use epidemic gossip to spread updates to all nodes" | Gossip is for large clusters (100+ nodes) where direct notification to all peers is impractical. chromatindb has max_peers=32, fully meshed. Direct notification to all connected peers is O(peers) and already more efficient than gossip's O(log N * fanout) rounds. Gossip adds message amplification, probabilistic delivery, and convergence delays. | Direct push to all connected peers. With max_peers=32, this is at most 32 sends per blob ingest -- trivial. |
| Causal ordering / vector clocks for push notifications | "Ensure notifications arrive in causal order" | Notifications are advisory ("go fetch this"). Ordering doesn't matter because the fetch is idempotent and content-addressed. If blob B depends on blob A, the application layer handles ordering, not the sync layer. Vector clocks add per-message metadata overhead and require merge logic. | Unordered notifications + content-addressed fetch. Application builds ordering from blob timestamps/seq_nums if needed. |
| Push-based delete propagation (eager tombstone push) | "Push tombstones immediately like blob notifications" | Tombstones ARE blobs in chromatindb (signed, stored, replicated). They already flow through the same push notification path. No special handling needed. Trying to add a separate "delete propagation" mechanism creates a parallel channel that can diverge. | Tombstones use the exact same BlobNotify path. is_tombstone flag in the notification lets peers handle them appropriately. |
| SDK transparent retry for write operations | "Retry writes automatically on reconnect" | Writes are NOT idempotent in user intent (same data produces same hash, but user may not want double-writes of different data). Auto-retrying a write after reconnect risks: (1) writing stale data if the application state changed, (2) masking failures the application should handle. Reads are safe to retry; writes are not. | Retry reads and queries transparently. Surface write failures to the application. Let the caller decide whether to retry a write. |
| Full-state push (send blob data in notification) | "Include blob data in the notification to avoid a round-trip" | Blob data can be up to 100 MiB. Pushing full blobs would overwhelm peers with unwanted data (they may not care about all namespaces). CouchDB changes feed sends metadata, not document content. IPFS Bitswap sends block only when wantlisted. The pull model (notify metadata, fetch on demand) is universally preferred for large objects. | Push metadata-only notification (77 bytes). Peer decides whether to fetch based on namespace interest. |

## Feature Dependencies

```
[Push Notification on Ingest]
    |
    +--enables--> [Targeted Blob Fetch]
    |                 |
    |                 +--requires--> new BlobFetchRequest/Response wire types
    |
    +--enhances--> [Safety-Net Reconciliation]
    |                 (push handles real-time, reconciliation catches gaps)
    |
    +--independent--> [Reconcile-on-Connect]
                      (already exists, preserved as-is)

[Application-Level Keepalive]
    |
    +--enables--> [SDK Auto-Reconnect]
    |                 |
    |                 +--requires--> dead connection detection
    |                 +--requires--> [SDK Subscription Restoration]
    |
    +--enhances--> existing receiver-side inactivity timeout
                   (replace 120s passive detection with 15-30s active detection)

[Event-Driven Expiry]
    |
    +--requires--> Storage::get_next_expiry_timestamp()
    +--independent--> all sync features (orthogonal concern)

[Disconnect-Triggered Cursor Cleanup]
    |
    +--requires--> on_peer_disconnected hook (already exists)
    +--replaces--> cursor_compaction_loop (6h timer)
```

### Dependency Notes

- **Push Notification requires no new dependencies:** Reuses existing Notification payload format and on_blob_ingested callback infrastructure. The SyncProtocol already has set_on_blob_ingested() which fires for sync-received blobs. Direct ingests (client writes) also need to trigger push.
- **Targeted Blob Fetch requires Push Notification:** Without notifications, there's nothing to trigger a targeted fetch. The notification carries the (namespace, blob_hash) needed for the fetch request.
- **SDK Auto-Reconnect requires Keepalive:** Without bidirectional heartbeat, dead connections are detected by the 120s inactivity timeout (receiver-side only). Keepalive pings bring detection down to ~15-30s, enabling faster reconnect.
- **SDK Subscription Restoration requires Auto-Reconnect:** Subscriptions are connection-scoped in the C++ node. A new connection = empty subscription set. Must re-subscribe after reconnect.
- **Event-Driven Expiry is independent:** Can be implemented in any phase, has no dependency on sync features. However, implementing it alongside the other event-driven changes is logical for consistency.
- **Disconnect-Triggered Cursor Cleanup is independent:** Simple hook, no dependency on push notification. Can replace the 6h timer in any phase.

## MVP Definition

### Launch With (v2.0.0)

The minimum set to replace timer-paced sync with event-driven sync and achieve sub-second cross-node propagation.

- [x] Push notification on blob ingest -- the core value prop
- [x] Targeted blob fetch from notification -- completes the push->fetch loop
- [x] Reconcile-on-connect preserved -- catchup after downtime
- [x] Safety-net periodic reconciliation (10-15 min) -- correctness backstop
- [x] Application-level bidirectional keepalive -- dead connection detection
- [x] SDK auto-reconnect with exponential backoff -- transparent recovery
- [x] SDK subscription restoration on reconnect -- pub/sub continuity
- [x] Event-driven expiry (next-expiry timer) -- replace periodic scan
- [x] Disconnect-triggered cursor cleanup -- replace 6h timer
- [x] Documentation refresh -- PROTOCOL.md, README, SDK docs

### Add After Validation (v2.x)

Features to add once the core event-driven sync is proven in the KVM test swarm.

- [ ] Notification coalescing/batching -- if bulk ingest performance is an issue
- [ ] Per-peer notification deduplication -- if notification volume becomes a problem
- [ ] Configurable heartbeat interval -- YAGNI until operator requests it

### Future Consideration (v3+)

Features to defer until the system needs to scale beyond current architecture.

- [ ] Gossip-based propagation -- only if peer count grows beyond direct notification limits
- [ ] Guaranteed delivery with hint storage -- only if safety-net reconciliation proves insufficient
- [ ] Vector clocks / causal ordering -- only if application layer needs it

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| Push notification on ingest | HIGH | MEDIUM | P1 |
| Targeted blob fetch | HIGH | MEDIUM | P1 |
| Reconcile-on-connect (preserve) | HIGH | LOW | P1 |
| Safety-net reconciliation (reconfig) | HIGH | LOW | P1 |
| Bidirectional keepalive | HIGH | MEDIUM | P1 |
| Event-driven expiry | MEDIUM | MEDIUM | P1 |
| Disconnect-triggered cursor cleanup | MEDIUM | LOW | P1 |
| SDK auto-reconnect | HIGH | HIGH | P1 |
| SDK subscription restoration | MEDIUM | MEDIUM | P1 |
| Documentation refresh | HIGH | MEDIUM | P1 |
| Notification coalescing | LOW | LOW | P3 |

**Priority key:**
- P1: Must have for v2.0.0 launch (all features in this milestone are P1 per PROJECT.md)
- P2: Should have, add when possible
- P3: Nice to have, future consideration

## Competitor Feature Analysis

| Feature | CouchDB | Syncthing | Cassandra | IPFS Bitswap | chromatindb v2.0.0 |
|---------|---------|-----------|-----------|--------------|---------------------|
| Push notification | Continuous changes feed (HTTP long-poll) | Index Update message (BEP) | Write-to-all-replicas (direct) | Wantlist fulfillment | BlobNotify to all connected peers |
| Fetch mechanism | GET /db/doc_id | Block Request/Response | Read from coordinator | Block request by CID | BlobFetchRequest/Response |
| Initial sync | Replicate from last_seq | Full Index on connect | Anti-entropy with Merkle trees | Bitswap session wantlist | XOR-fingerprint reconciliation (existing) |
| Anti-entropy | Continuous replication monitors | Periodic full rescan | nodetool repair + read repair | None (wantlist is ongoing) | Safety-net reconciliation (10-15 min) |
| Keepalive | HTTP/TCP level | BEP Ping message | Gossip heartbeat (1s) | libp2p connection manager | Bidirectional Ping/Pong (15-30s) |
| Dead connection detection | HTTP timeout | BEP timeout | Gossip failure detector (phi) | libp2p identify | Keepalive timeout + inactivity timeout |
| Expiry handling | No built-in expiry | No expiry (file sync) | TTL per column, lazy + compaction | Pin/unpin + GC | Next-expiry timer (event-driven) |
| Client reconnect | HTTP retry (stateless) | Automatic reconnect (BEP) | Driver-level reconnect | libp2p reconnect | SDK auto-reconnect with jittered backoff |
| Subscription restore | N/A (HTTP polling) | N/A (always syncs all folders) | N/A | N/A | Re-subscribe tracked namespaces on reconnect |

### Key Takeaways from Competitor Analysis

1. **CouchDB's changes feed** is the closest analog to chromatindb's push notification. The key lesson: changes feed sends metadata (doc_id, rev, seq), not full documents. chromatindb should do the same: send notification metadata, let the peer fetch.

2. **Syncthing's Index Update** is a batch notification mechanism. After the initial full Index, only changed files are sent as Index Updates. chromatindb's per-blob BlobNotify is finer-grained but the principle is the same.

3. **Cassandra's hinted handoff** is heavyweight for chromatindb's scale. With max 32 peers and direct connectivity, fire-and-forget push + reconciliation is simpler and sufficient.

4. **IPFS Bitswap's wantlist** is demand-driven (pull), not push. chromatindb's push model is better for its use case (replicate everything to all peers) because peers don't need to express interest -- they just receive.

5. **All systems keep a fallback anti-entropy mechanism.** No system trusts push alone. chromatindb's safety-net reconciliation follows this universal pattern.

## Implementation Notes for Roadmap

### C++ Node Changes (Estimated)

1. **New wire types** (2): BlobFetchRequest (type 59?), BlobFetchResponse (type 60?)
2. **Push notification path**: Hook into BlobEngine::ingest success path (both client writes and sync-received). Call notify_all_peers() instead of just notify_subscribers(). Separate concept: peer-to-peer BlobNotify vs client-facing Notification.
3. **Bidirectional keepalive**: PeerManager sends periodic Ping, monitors Pong response. Replace passive inactivity_check_loop with active keepalive_loop.
4. **Safety-net timer**: Change sync_interval_seconds default from 60 to 600-900.
5. **Event-driven expiry**: Add Storage::get_next_expiry_timestamp(). Rewrite expiry_scan_loop to use targeted timer.
6. **Cursor cleanup**: Add cursor deletion to on_peer_disconnected. Remove cursor_compaction_loop (or reduce to very infrequent safety check).
7. **Relay filter update**: Add BlobFetchRequest/Response to allowed types if clients need targeted fetch.

### Python SDK Changes (Estimated)

1. **Auto-reconnect wrapper**: Rework ChromatinClient.connect() to support reconnection. Internal reconnect loop with jittered exponential backoff (1s-30s).
2. **Subscription restoration**: After reconnect, re-send Subscribe for all tracked namespaces.
3. **Read/query retry**: Transparently retry read-only operations on ConnectionError. Do NOT retry writes.
4. **Keepalive integration**: Client must respond to server Ping with Pong (already implemented in _transport.py _send_pong). May also need client-initiated Ping for its own dead connection detection.

### Documentation Changes

1. **PROTOCOL.md**: New BlobNotify and BlobFetchRequest/Response wire formats. Updated sync model description. Keepalive specification.
2. **README.md**: Updated architecture description reflecting event-driven sync.
3. **SDK README + tutorial**: Auto-reconnect usage, subscription behavior on reconnect.

## Sources

- [CouchDB Replication Protocol](https://docs.couchdb.org/en/stable/replication/protocol.html) -- changes feed, continuous replication
- [CouchDB Changes API](https://docs.couchdb.org/en/stable/api/database/changes.html) -- since parameter, longpoll/continuous feed types
- [Syncthing BEP v1 Protocol](https://docs.syncthing.net/specs/bep-v1.html) -- Index/Index Update messages, ClusterConfig, Request/Response
- [IPFS Bitswap Protocol](https://specs.ipfs.tech/bitswap-protocol/) -- wantlist, HAVE/DONT_HAVE, block exchange
- [Cassandra Hinted Handoff](https://cassandra.apache.org/doc/4.0/cassandra/operating/hints.html) -- hint storage, segment replay
- [Cassandra Gossip Protocol](https://docs.datastax.com/en/cassandra-oss/3.x/cassandra/architecture/archGossipAbout.html) -- failure detection, state propagation
- [Redis Keyspace Notifications](https://redis.io/docs/latest/develop/pubsub/keyspace-notifications/) -- event-driven expiry, fire-and-forget pub/sub
- [RabbitMQ Heartbeats](https://www.rabbitmq.com/docs/heartbeats) -- TCP keepalive vs application-level heartbeat comparison
- [gRPC Connection Backoff Protocol](https://grpc.github.io/grpc/core/md_doc_connection-backoff.html) -- jittered exponential backoff spec
- [gRPC Keepalive](https://grpc.io/docs/guides/keepalive/) -- HTTP/2 PING-based keepalive, dead connection detection
- [WebSocket Reconnection Guide](https://websocket.org/guides/reconnection/) -- state sync, subscription restoration
- [PubNub Asyncio Reconnection Policies](https://www.pubnub.com/docs/sdks/asyncio/reconnection-policies) -- Python SDK auto-reconnect patterns
- [Anti-Entropy in Distributed Systems](https://systemdesignschool.io/blog/anti-entropy) -- push/pull/push-pull reconciliation patterns

---
*Feature research for: event-driven sync in distributed blob stores*
*Researched: 2026-04-02*
