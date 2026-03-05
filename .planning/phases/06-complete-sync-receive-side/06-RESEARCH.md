# Phase 6: Complete Sync Receive Side - Research

**Researched:** 2026-03-05
**Domain:** Async sync orchestration, message-driven state machines, coroutine coordination
**Confidence:** HIGH

## Summary

Phase 6 closes the largest functional gap remaining after Phase 5: the sync protocol only sends data outward but never receives, processes, or ingests data from peers. The current `run_sync_with_peer()` and `handle_sync_as_responder()` both send NamespaceList/HashList/SyncComplete but never wait for or process the peer's equivalent messages. The `on_peer_message()` router does not handle NamespaceList, HashList, BlobRequest, or BlobTransfer message types.

The core architectural challenge is that the sync coroutine (which needs to orchestrate a multi-step request/response flow) operates in a fire-and-forget send mode, while incoming messages arrive asynchronously through Connection's callback-based message_loop. To complete bidirectional sync, the sync coroutine needs a mechanism to receive specific messages from the peer mid-flow.

**Primary recommendation:** Add a per-connection async message queue (a simple channel/promise pattern using Asio's `async_channel` or a manual promise-based approach) so the sync coroutine can `co_await` the next sync-related message. Route sync message types from `on_peer_message()` into this queue instead of handling them inline. This keeps Connection's message_loop unchanged and localizes the change to PeerManager.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SYNC-01 | Nodes exchange blob hash lists to identify missing blobs (hash-list diff) | SyncProtocol already has encode/decode for NamespaceList, HashList, diff_hashes(), get_blobs_by_hashes(). Missing: BlobRequest encode/decode, and the receive-side orchestration in PeerManager that actually calls these methods on incoming data. |
| SYNC-02 | Sync is bidirectional -- both nodes end up with the union of their data | Current code only sends outward. Both run_sync_with_peer() (initiator) and handle_sync_as_responder() need to receive peer messages, compute diffs, request missing blobs, and ingest received blobs. The sync algorithm itself is proven correct in unit tests (test_sync_protocol.cpp "bidirectional sync produces union"). |
| SYNC-03 | Sync skips expired blobs (don't replicate dead data) | Already implemented in SyncProtocol::collect_namespace_hashes() and ingest_blobs(). No changes needed for the filtering logic itself -- just needs to be exercised in the actual network path. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | Async IO, coroutines, timers | Already in project; co_await-based async |
| SyncProtocol | (internal) | Hash-list diff, encoding, ingestion | All sync logic already implemented and tested |
| Connection | (internal) | Encrypted message send/receive | send_message() already works; message_loop() delivers via callback |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| asio::experimental::channel | 1.38.0 | Async message queue between coroutines | For passing received sync messages from callback into sync coroutine |
| spdlog | (existing) | Structured logging | Debug sync flow, log message counts |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| asio::experimental::channel | Manual promise/future with asio::steady_timer | Channel is cleaner but experimental; manual approach more portable but verbose |
| Per-connection channel | Global sync message queue | Per-connection is simpler, no multiplexing needed |
| Coroutine-based sync flow | Callback-based state machine | Coroutine approach matches existing code style and is more readable |

## Architecture Patterns

### Current Architecture (Phase 5 -- the gap)
```
Connection::message_loop()
  -> callback: on_peer_message(conn, type, payload)
    -> SyncRequest: spawn handle_sync_as_responder() [SEND ONLY]
    -> Data: ingest blob
    -> Other sync types: NOT HANDLED (dropped silently)

run_sync_with_peer():
  -> sends SyncRequest
  -> sends NamespaceList
  -> sends HashList per namespace
  -> sends SyncComplete
  -> NEVER receives anything from peer
```

### Target Architecture (Phase 6)
```
Connection::message_loop()
  -> callback: on_peer_message(conn, type, payload)
    -> SyncRequest: spawn handle_sync_as_responder() [FULL FLOW]
    -> Sync-related types (NamespaceList, HashList, BlobRequest,
       BlobTransfer, SyncAccept, SyncComplete):
         -> route to per-peer sync message queue
    -> Data: ingest blob

run_sync_with_peer() [initiator]:
  1. Send SyncRequest
  2. Send our NamespaceList + HashLists + SyncComplete
  3. co_await peer's NamespaceList from queue
  4. co_await peer's HashLists from queue (until SyncComplete)
  5. For each namespace: diff_hashes() -> send BlobRequest
  6. co_await BlobTransfer responses -> ingest_blobs()
  7. Handle incoming BlobRequests -> send BlobTransfers
  8. Done

handle_sync_as_responder() [responder]:
  1. Receive SyncRequest (already routed)
  2. Send SyncAccept
  3. Send our NamespaceList + HashLists + SyncComplete
  4. co_await peer's NamespaceList from queue
  5. co_await peer's HashLists from queue (until SyncComplete)
  6. For each namespace: diff_hashes() -> send BlobRequest
  7. co_await BlobTransfer responses -> ingest_blobs()
  8. Handle incoming BlobRequests -> send BlobTransfers
  9. Done
```

### Pattern 1: Per-Peer Sync Message Queue

**What:** Each PeerInfo gets a message queue (channel) for sync-related messages. The on_peer_message callback pushes sync messages into it. The sync coroutine co_awaits messages from it.

**When to use:** When a coroutine needs to receive messages that arrive via a callback-driven message loop.

**Example approach using asio::experimental::channel:**
```cpp
#include <asio/experimental/channel.hpp>

// In PeerInfo struct:
struct SyncMessage {
    wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};

struct PeerInfo {
    net::Connection::Ptr connection;
    std::string address;
    bool is_bootstrap = false;
    uint32_t strike_count = 0;
    bool syncing = false;
    // NEW: queue for sync messages
    using SyncChannel = asio::experimental::channel<void(asio::error_code, SyncMessage)>;
    std::unique_ptr<SyncChannel> sync_channel;
};
```

**Alternative simpler approach using manual promise:**
```cpp
// Per-peer pending sync message queue (simpler, no experimental dep)
struct PeerInfo {
    // ...existing fields...
    std::deque<SyncMessage> sync_inbox;
    asio::steady_timer* sync_waiter = nullptr;  // waiting coroutine's timer
};

// In on_peer_message:
void enqueue_sync_message(PeerInfo* peer, SyncMessage msg) {
    peer->sync_inbox.push_back(std::move(msg));
    if (peer->sync_waiter) {
        peer->sync_waiter->cancel();  // wake up the co_await
    }
}

// In sync coroutine:
asio::awaitable<std::optional<SyncMessage>> recv_sync_message(PeerInfo* peer) {
    while (peer->sync_inbox.empty()) {
        asio::steady_timer timer(ioc_);
        peer->sync_waiter = &timer;
        timer.expires_after(std::chrono::seconds(30));  // timeout
        auto [ec] = co_await timer.async_wait(use_nothrow);
        peer->sync_waiter = nullptr;
        if (ec == asio::error::operation_aborted) continue;  // woken up
        co_return std::nullopt;  // timeout
    }
    auto msg = std::move(peer->sync_inbox.front());
    peer->sync_inbox.pop_front();
    co_return msg;
}
```

### Pattern 2: BlobRequest Encode/Decode

**What:** Add encode/decode for BlobRequest messages to SyncProtocol. Wire format mirrors HashList since a BlobRequest is "these hashes in this namespace, send me the blobs."

**Wire format (matching existing patterns):**
```
BlobRequest: [ns:32B][count:u32BE][hash1:32B]...[hashN:32B]
```

This is identical to HashList encoding, which means `encode_hash_list` / `decode_hash_list` can be reused directly (or aliased) for BlobRequest.

### Pattern 3: Interleaved Send/Receive

**What:** Both sides send their data AND receive the peer's data. The sync flow must handle both directions concurrently or sequentially.

**Sequential approach (simpler, recommended):**
```
Phase A (both sides): Send our data (NamespaceList, HashLists, SyncComplete)
Phase B (both sides): Receive peer data (NamespaceList, HashLists, SyncComplete)
Phase C (both sides): Compute diffs, exchange BlobRequests and BlobTransfers
```

**Why sequential works:** Since both sides send all their data first, then receive, there is no deadlock. The TCP buffers and Asio's async write pipeline handle the simultaneous sends. The key insight is that sending our data does not depend on receiving the peer's data -- we send what we have, then process what they sent.

**Potential deadlock risk:** If both sides try to send AND receive simultaneously within the same coroutine (e.g., send one namespace then wait for response), they could deadlock if the TCP send buffer fills. The sequential approach avoids this entirely.

### Anti-Patterns to Avoid
- **Inline processing in on_peer_message:** Don't try to process sync messages directly in the callback. The callback runs on the message_loop's coroutine, and calling BlobEngine operations (which touch libmdbx) from there could cause issues if the sync coroutine is also running.
- **Modifying Connection's message_loop:** Don't change Connection to add sync-specific receive logic. Connection should remain a generic encrypted transport. All sync intelligence belongs in PeerManager.
- **Blocking the sync coroutine indefinitely:** Always use timeouts when waiting for peer messages. A misbehaving or crashed peer could leave the sync coroutine hanging forever.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Async message queue | Custom linked list with mutexes | asio timer-cancel pattern or asio::experimental::channel | Correct wakeup semantics with coroutines is tricky |
| BlobRequest encoding | New wire format | Reuse encode_hash_list / decode_hash_list | Wire format is identical (namespace + list of hashes) |
| Sync timeout | Manual timestamp tracking | asio::steady_timer with co_await | Already used throughout the codebase |

**Key insight:** The sync algorithm itself is already correct and tested. The only gap is the async message routing in PeerManager -- getting messages from the callback into the sync coroutine.

## Common Pitfalls

### Pitfall 1: Deadlock from Interleaved Send/Receive
**What goes wrong:** If both peers try to send a large hash list and read simultaneously, and TCP buffers fill, both block on send waiting for the other to read.
**Why it happens:** TCP flow control stalls writes when the receive window is full.
**How to avoid:** Send all outbound data first (NamespaceList + all HashLists + SyncComplete), then receive all inbound data. Both sides send first, both sides receive second. TCP buffers are typically 64-256KB, more than enough for hash lists.
**Warning signs:** Sync hangs indefinitely between two nodes that both have data.

### Pitfall 2: Sync Coroutine vs Message Loop Race
**What goes wrong:** The message_loop callback fires before the sync coroutine has set up its receive channel/queue, causing messages to be dropped.
**Why it happens:** The peer sends its data immediately after receiving SyncRequest. If we spawn the sync coroutine and it hasn't finished setting up, early messages are lost.
**How to avoid:** Set up the sync message queue BEFORE sending/triggering any messages. For the initiator: create the queue before sending SyncRequest. For the responder: create the queue in on_peer_message before spawning the coroutine.
**Warning signs:** Sync works sometimes but not always (race condition).

### Pitfall 3: Missing Timeout on Sync Operations
**What goes wrong:** If a peer disconnects or misbehaves mid-sync, the sync coroutine waits forever.
**Why it happens:** co_await on the message queue never completes if no message arrives.
**How to avoid:** Use a timeout (30-60 seconds) on every co_await in the sync flow. On timeout, log a warning, clean up, and mark sync as failed.
**Warning signs:** Increasing memory usage, sync_count growing without bound.

### Pitfall 4: Iterator Invalidation During Peer Disconnect
**What goes wrong:** Peer disconnects while sync is in progress, PeerInfo gets erased from peers_ vector, sync coroutine accesses dangling pointer.
**Why it happens:** on_peer_disconnected erases from peers_, but the sync coroutine holds a raw PeerInfo* pointer.
**How to avoid:** Check conn->is_authenticated() / find_peer() after every co_await in the sync flow. If peer is gone, bail out immediately. Alternatively, don't erase PeerInfo during active sync -- mark as disconnected and clean up after sync completes.
**Warning signs:** Segfaults during disconnect-while-syncing scenarios.

### Pitfall 5: Co-spawn Lambda vs Coroutine Lifetime
**What goes wrong:** Passing a coroutine member function call directly to co_spawn causes the coroutine frame to outlive the object.
**Why it happens:** Known Phase 5 lesson (STATE.md): "Pass lambda as callable to co_spawn (never invoke with trailing `()` -- coroutine lifetime bug)".
**How to avoid:** Always use `[this, conn]() -> asio::awaitable<void> { co_await ... }` pattern.
**Warning signs:** Crash or UB after PeerManager destruction.

### Pitfall 6: Sending BlobRequest for Namespaces Peer Doesn't Have
**What goes wrong:** We receive the peer's namespace list, then request hash lists for namespaces WE have but the peer doesn't. The peer has no data to send.
**Why it happens:** Confusion about which side's namespaces to diff.
**How to avoid:** The diff algorithm should iterate over the PEER's namespaces (from their NamespaceList), collect OUR hashes for that namespace, compare with THEIR hashes, and request what we're missing. For namespaces we have that the peer doesn't, there's nothing to request. For namespaces the peer has that we don't, we need ALL their hashes.
**Warning signs:** BlobRequest sent for empty namespace, peer responds with empty BlobTransfer.

## Code Examples

### Sync Message Queue (Timer-Cancel Pattern)
```cpp
// Lightweight async message queue for sync coroutine consumption.
// Uses the timer-cancel pattern already established in the codebase.

struct SyncMessage {
    wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};

// Added to PeerInfo:
std::deque<SyncMessage> sync_inbox;
asio::steady_timer* sync_notify = nullptr;

// Called from on_peer_message for sync-typed messages:
void PeerManager::route_sync_message(PeerInfo* peer,
                                      wire::TransportMsgType type,
                                      std::vector<uint8_t> payload) {
    peer->sync_inbox.push_back({type, std::move(payload)});
    if (peer->sync_notify) {
        peer->sync_notify->cancel();  // Wake up waiting coroutine
    }
}

// Called from sync coroutine to receive next sync message:
asio::awaitable<std::optional<SyncMessage>>
PeerManager::recv_sync_msg(PeerInfo* peer, std::chrono::seconds timeout) {
    if (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }

    asio::steady_timer timer(ioc_);
    peer->sync_notify = &timer;
    timer.expires_after(timeout);

    auto [ec] = co_await timer.async_wait(
        asio::as_tuple(asio::use_awaitable));

    peer->sync_notify = nullptr;

    if (peer->sync_inbox.empty()) {
        co_return std::nullopt;  // Timeout
    }

    auto msg = std::move(peer->sync_inbox.front());
    peer->sync_inbox.pop_front();
    co_return msg;
}
```

### Full Sync Flow (Initiator Side)
```cpp
asio::awaitable<void> PeerManager::run_sync_with_peer(Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;

    // Phase A: Send our data
    co_await conn->send_message(TransportMsgType_SyncRequest, {});

    auto our_ns = engine_.list_namespaces();
    co_await conn->send_message(TransportMsgType_NamespaceList,
        SyncProtocol::encode_namespace_list(our_ns));

    for (const auto& ns : our_ns) {
        auto hashes = sync_proto_.collect_namespace_hashes(ns.namespace_id);
        co_await conn->send_message(TransportMsgType_HashList,
            SyncProtocol::encode_hash_list(ns.namespace_id, hashes));
    }
    co_await conn->send_message(TransportMsgType_SyncComplete, {});

    // Phase B: Receive peer's data
    // Wait for peer's NamespaceList
    auto ns_msg = co_await recv_sync_msg(peer, 30s);
    if (!ns_msg || ns_msg->type != TransportMsgType_NamespaceList) { /* bail */ }

    auto peer_namespaces = SyncProtocol::decode_namespace_list(ns_msg->payload);

    // Receive peer's HashLists until SyncComplete
    std::map<std::array<uint8_t,32>, std::vector<std::array<uint8_t,32>>> peer_hashes;
    while (true) {
        auto hl_msg = co_await recv_sync_msg(peer, 30s);
        if (!hl_msg) break;
        if (hl_msg->type == TransportMsgType_SyncComplete) break;
        if (hl_msg->type == TransportMsgType_HashList) {
            auto [ns, hashes] = SyncProtocol::decode_hash_list(hl_msg->payload);
            peer_hashes[ns] = std::move(hashes);
        }
    }

    // Phase C: Compute diffs and exchange blobs
    SyncStats stats;
    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) continue;

        // Send BlobRequest (reuse hash_list encoding)
        co_await conn->send_message(TransportMsgType_BlobRequest,
            SyncProtocol::encode_hash_list(ns, missing));

        // Wait for BlobTransfer response
        auto bt_msg = co_await recv_sync_msg(peer, 30s);
        if (bt_msg && bt_msg->type == TransportMsgType_BlobTransfer) {
            auto blobs = SyncProtocol::decode_blob_transfer(bt_msg->payload);
            auto s = sync_proto_.ingest_blobs(blobs);
            stats.blobs_received += s.blobs_received;
        }
    }

    // Also handle incoming BlobRequests from peer
    // (peer may also need blobs from us)
    // This needs to be handled via the message queue during Phase C

    peer->syncing = false;
}
```

### on_peer_message Routing Update
```cpp
void PeerManager::on_peer_message(Connection::Ptr conn,
                                   TransportMsgType type,
                                   std::vector<uint8_t> payload) {
    // Sync protocol messages -> route to per-peer sync queue
    if (type == wire::TransportMsgType_SyncRequest) {
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await handle_sync_as_responder(conn);
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_SyncAccept ||
        type == wire::TransportMsgType_NamespaceList ||
        type == wire::TransportMsgType_HashList ||
        type == wire::TransportMsgType_BlobRequest ||
        type == wire::TransportMsgType_BlobTransfer ||
        type == wire::TransportMsgType_SyncComplete) {
        auto* peer = find_peer(conn);
        if (peer) {
            route_sync_message(peer, type, std::move(payload));
        }
        return;
    }

    // Data messages...
    if (type == wire::TransportMsgType_Data) {
        // existing blob ingest logic
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Phase 5: send-only sync | Phase 6: full bidirectional sync | Current phase | Completes SYNC-01, SYNC-02, SYNC-03 |
| No BlobRequest handling | BlobRequest + BlobTransfer exchange | Current phase | Nodes can actually request and receive specific missing blobs |

**Not deprecated -- building on existing:**
- SyncProtocol encode/decode methods: All correct, no changes needed
- SyncProtocol::diff_hashes: Proven correct in unit tests
- SyncProtocol::ingest_blobs: Proven correct, handles expiry filtering
- Connection::send_message: Works correctly for all message types
- Wire format: All message types already defined in transport.fbs (BlobRequest=13, BlobTransfer=14)

## Open Questions

1. **Concurrent BlobRequest handling during Phase C**
   - What we know: Both sides need to send BlobRequests AND handle incoming BlobRequests from the peer. In the sequential model, Phase C could interleave: we send a BlobRequest, then before receiving BlobTransfer, we get a BlobRequest from the peer.
   - What's unclear: Whether to handle this via the message queue (pulling messages of either type) or by splitting Phase C into "request our missing blobs" and "respond to their requests" sub-phases.
   - Recommendation: Handle both message types in the Phase C loop. When waiting for BlobTransfer, if we get a BlobRequest instead, handle it (send BlobTransfer back) and continue waiting for our BlobTransfer. This is the most robust approach.

2. **Sync flow coordination between initiator and responder**
   - What we know: The initiator sends SyncRequest, responder sends SyncAccept. But after that, both sides follow essentially the same algorithm.
   - What's unclear: Whether the initiator should wait for SyncAccept before proceeding, or just start sending (current behavior).
   - Recommendation: Wait for SyncAccept for protocol correctness, but don't block long (5s timeout). If the responder rejects (no SyncAccept), bail out.

3. **Large namespace/hash lists exceeding frame size**
   - What we know: MAX_FRAME_SIZE is 16 MB (framing.h line 13). At 32 bytes per hash, that supports ~500K hashes per message. The hash list overhead is 36 bytes (32B namespace + 4B count), so a single HashList message can carry ~524,000 blob hashes.
   - Resolution: Not a concern for v1. The frame size limit is generous. No chunking needed.

## Sources

### Primary (HIGH confidence)
- Existing codebase analysis: src/sync/sync_protocol.{h,cpp}, src/peer/peer_manager.{h,cpp}, src/net/connection.{h,cpp}, src/net/server.{h,cpp}, src/net/protocol.{h,cpp}
- schemas/transport.fbs: All message types (BlobRequest=13, BlobTransfer=14) already defined
- tests/sync/test_sync_protocol.cpp: Bidirectional sync algorithm proven correct in unit tests
- tests/peer/test_peer_manager.cpp: Basic PeerManager lifecycle tests exist
- tests/test_daemon.cpp: Two-node E2E test exists but sync verification is weak (REQUIRE >= 0)

### Secondary (MEDIUM confidence)
- Asio coroutine patterns: Timer-cancel pattern for async notification is a standard Asio idiom, used throughout this codebase (sync_timer_loop, reconnect_loop)
- asio::experimental::channel: Available in Asio 1.38.0 but marked experimental; the timer-cancel approach is simpler and proven in this codebase

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - Everything already in the project, no new dependencies
- Architecture: HIGH - Clear gap analysis from reading the actual code; sync algorithm proven in tests
- Pitfalls: HIGH - Based on actual codebase patterns and Phase 5 lessons (STATE.md)

**Research date:** 2026-03-05
**Valid until:** 2026-04-05 (stable domain, no external dependencies changing)
