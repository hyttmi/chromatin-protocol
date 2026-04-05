# Phase 86: Namespace Filtering & Hot Reload - Research

**Researched:** 2026-04-05
**Domain:** C++20 peer-to-peer sync protocol -- namespace filtering, FlatBuffers wire format, SIGHUP config reload
**Confidence:** HIGH

## Summary

Phase 86 adds three capabilities: (1) peers exchange SyncNamespaceAnnounce messages after handshake to declare which namespaces they replicate, (2) BlobNotify fan-out and Phase A/B/C reconciliation are scoped to the namespace intersection, and (3) max_peers becomes SIGHUP-reloadable.

All three requirements build on existing, well-established patterns in the codebase. The SyncNamespaceAnnounce message follows the same FlatBuffers-in-transport-envelope pattern as all other peer messages. The namespace filtering extends existing `sync_namespaces_` logic (sender-side only today) to also incorporate the remote peer's announced set. The max_peers reload follows the exact same pattern as the dozen other SIGHUP-reloadable parameters already in `reload_config()`.

**Primary recommendation:** Implement as three independent work streams -- wire format/schema (FlatBuffers + relay blocklist), peer protocol integration (announce exchange + filtering), and max_peers hot reload -- that converge in unit and Docker integration tests.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Both peers send SyncNamespaceAnnounce after successful auth exchange, before sync-on-connect. Both sides wait to receive the other's announcement before proceeding to the message loop. This guarantees the namespace intersection is known before any sync traffic flows.
- **D-02:** On SIGHUP, when sync_namespaces changes, the node re-sends SyncNamespaceAnnounce to all connected peers immediately. Existing peers update their filter in real-time.
- **D-03:** Re-announce is a passive filter update only. No re-sync triggered on re-announce. The safety-net cycle (600s) catches any gaps naturally.
- **D-04:** SyncNamespaceAnnounce filters BOTH BlobNotify push notifications AND Phase A/B/C reconciliation. During reconciliation, only exchange fingerprints and blobs for namespaces in the intersection of both peers' announced sets.
- **D-05:** Intersection semantics: the effective set is the intersection of both peers' announced namespace sets. Sender doesn't waste bandwidth on namespaces the receiver doesn't replicate, and skips fingerprinting namespaces it doesn't have.
- **D-06:** BlobNotify filter by sync_namespaces (node replication config), NOT subscribed_namespaces (client state).
- **D-07:** No distinction between "never announced" and "announced empty set." Both mean "replicate everything." Empty announcement = receive all BlobNotify, full reconciliation scope.
- **D-08:** All nodes upgrade together (home KVM swarm). No mixed-version backward compatibility needed. Breaking protocol change is acceptable.
- **D-09:** SyncNamespaceAnnounce is message type 62 in the transport FlatBuffers enum. Peer-internal, added to relay blocklist.
- **D-10:** FlatBuffers-encoded payload inside the standard transport envelope (type byte + length-prefixed FlatBuffer). Same dispatch pattern as all other messages via handle_message().
- **D-11:** FlatBuffer schema uses a vector of fixed-size structs: Namespace { hash:[uint8:32] }. Type-safe, FlatBuffers handles alignment, clean iteration.
- **D-12:** max_peers becomes SIGHUP-reloadable in reload_config(). When the new limit is lower than current peer count, refuse new incoming connections until count drops below the limit naturally. No active disconnection of excess peers.
- **D-13:** Log a warning when the node is over the new max_peers limit after SIGHUP.

### Claude's Discretion
- How SyncNamespaceAnnounce integrates into the on_peer_connected coroutine flow (send/recv ordering)
- PeerInfo struct additions to track per-peer announced namespaces
- How sync_protocol.cpp Phase A/B/C skips non-intersecting namespaces (likely filter in fingerprint generation)
- Relay blocklist update mechanism for type 62
- Test strategy and test structure

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| FILT-01 | Peers exchange sync_namespaces after handshake via SyncNamespaceAnnounce message | Wire format design (D-09, D-10, D-11), on_peer_connected integration point, FlatBuffers schema addition, announce exchange protocol |
| FILT-02 | Node only sends BlobNotify to peers whose announced namespaces include the blob's namespace | on_blob_ingested filtering logic, PeerInfo.announced_namespaces field, intersection semantics (D-05, D-07) |
| OPS-01 | max_peers is reloadable via SIGHUP without node restart | Mutable max_peers_ member, reload_config() addition, should_accept_connection() fix, PEX limit fix |
</phase_requirements>

## Architecture Patterns

### Current Connection Lifecycle (on_peer_connected)

The `on_peer_connected()` callback fires after handshake succeeds. Current flow:
1. ACL check (peer allowed?)
2. PeerInfo creation and initialization
3. Connection dedup (tie-break)
4. Add to `peers_` deque
5. Message routing setup (`conn->on_message(...)`)
6. Cursor grace period check
7. If initiator: `co_spawn(run_sync_with_peer)`

**Integration point for SyncNamespaceAnnounce:** Between steps 5 (message routing setup) and 7 (sync-on-connect). Both sides must send and receive the announce before any sync traffic. The challenge: `on_peer_connected` is NOT a coroutine -- it's a void callback. The announce exchange must be co_spawned, with sync-on-connect chained after it completes.

**Recommended approach:** Add a new coroutine `announce_and_sync(conn)` that:
1. Sends SyncNamespaceAnnounce with our `sync_namespaces_` set
2. Waits for peer's SyncNamespaceAnnounce (with timeout)
3. Stores peer's announced set in PeerInfo
4. If initiator: calls `run_sync_with_peer(conn)`

This replaces the current direct `co_spawn(run_sync_with_peer)` for initiators, and is also spawned for responders (who need to receive the announce but don't initiate sync).

**Confidence:** HIGH -- follows existing co_spawn patterns in the codebase.

### How Announce Reception Works

The announce message arrives via the message loop, which calls `on_peer_message()`. But there's a timing subtlety: the message loop is already running when `on_peer_connected` fires (Connection::run() starts message_loop() after handshake, and fires `ready_cb_` which triggers `on_peer_connected`).

Two design options for receiving the peer's announce:

**Option A (Recommended): Route through sync_inbox + recv_sync_msg.**
Use the same queue pattern as sync messages. In the `announce_and_sync` coroutine, wait for a SyncNamespaceAnnounce via `recv_sync_msg()`. Add SyncNamespaceAnnounce to the `on_peer_message` dispatch to route it to `route_sync_message()`. This reuses the existing timer-cancel recv pattern with zero new infrastructure.

**Option B: New dedicated field on PeerInfo.**
Set a flag/promise and have the coroutine poll. More complex, no benefit.

Option A is strongly preferred -- it reuses the proven `recv_sync_msg` mechanism.

**Confidence:** HIGH -- recv_sync_msg is battle-tested across 85 phases.

### BlobNotify Namespace Filtering

Current `on_blob_ingested()` (line 3032) iterates `peers_` and sends BlobNotify to all TCP peers except source. The filter addition is a simple conditional:

```cpp
// In the BlobNotify fan-out loop:
for (auto& peer : peers_) {
    if (peer.connection == source) continue;
    if (peer.connection->is_uds()) continue;

    // NEW: Namespace filtering (D-05, D-07)
    if (!peer.announced_namespaces.empty() &&
        peer.announced_namespaces.count(namespace_id) == 0) continue;

    // ... send BlobNotify
}
```

Empty `announced_namespaces` means "replicate everything" (D-07), so the check is: skip only when the set is non-empty AND doesn't contain the namespace.

**Confidence:** HIGH -- trivial conditional addition to existing loop.

### Phase A/B/C Reconciliation Filtering

Currently, `run_sync_with_peer()` filters by the LOCAL node's `sync_namespaces_` (line 1856). The change adds REMOTE peer's announced namespaces as an additional filter. The effective set for reconciliation is the intersection of:
1. Local `sync_namespaces_` (already applied)
2. Remote peer's `announced_namespaces` (new)

**Integration points:**

In `run_sync_with_peer()` (initiator, line 1942):
```cpp
// Existing local filter:
if (!sync_namespaces_.empty() && sync_namespaces_.find(ns) == sync_namespaces_.end()) continue;

// NEW: Remote peer filter (D-04, D-05):
auto* peer = find_peer(conn);
if (peer && !peer->announced_namespaces.empty() &&
    peer->announced_namespaces.count(ns) == 0) continue;
```

Same pattern in `handle_sync_as_responder()` (line 2257) and in the `all_namespaces` union construction.

Also filter the `our_namespaces` list sent in Phase A NamespaceList (line 1855) -- no point advertising namespaces the peer won't replicate:
```cpp
// After existing sync_namespaces_ filter:
if (peer && !peer->announced_namespaces.empty()) {
    std::erase_if(our_namespaces, [&peer](const storage::NamespaceInfo& ns) {
        return peer->announced_namespaces.count(ns.namespace_id) == 0;
    });
}
```

**Confidence:** HIGH -- direct extension of existing filtering pattern.

### max_peers Hot Reload

**Critical finding:** `should_accept_connection()` (line 307) reads `config_.max_peers`, but `config_` is a `const Config&` reference. SIGHUP cannot modify the original Config object. The fix follows the established pattern: add a mutable `uint32_t max_peers_` member (like `sync_cooldown_seconds_`, `safety_net_interval_seconds_`, etc.), initialize from `config_.max_peers` in the constructor, and use `max_peers_` everywhere.

**Two locations need updating:**
1. `should_accept_connection()` (line 308): `peers_.size() < max_peers_` (was `config_.max_peers`)
2. `handle_peer_list_response()` (line 3353): `peers_.size() >= max_peers_` (was `config_.max_peers`)

**reload_config() addition:** After the existing sync_namespaces reload block:
```cpp
max_peers_ = new_cfg.max_peers;
if (peers_.size() > max_peers_) {
    spdlog::warn("config reload: max_peers={} but {} peers connected (excess will drain naturally)",
                 max_peers_, peers_.size());
} else {
    spdlog::info("config reload: max_peers={}", max_peers_);
}
```

**Confidence:** HIGH -- identical pattern to all other SIGHUP-reloadable params.

### FlatBuffers Schema Addition

Add to `db/schemas/transport.fbs`:
```flatbuffers
// Phase 86: Namespace filtering
SyncNamespaceAnnounce = 62
```

The schema uses a vector of fixed-size 32-byte arrays. FlatBuffers approach per D-11:
```flatbuffers
struct NamespaceHash {
    hash:[uint8:32];
}

table SyncNamespaceAnnounceMessage {
    namespaces:[NamespaceHash];
}
```

However, there's a subtlety: the existing message dispatch does NOT use nested FlatBuffer tables for transport messages -- all payloads are raw bytes inside the `TransportMessage.payload` field. Other messages (BlobNotify, BlobFetch, Subscribe, etc.) use hand-rolled binary formats, not nested FlatBuffer tables.

**Decision point (Claude's discretion):** Use the same raw binary encoding pattern as all other messages (simple `[count:u16BE][ns1:32]...[nsN:32]` -- identical to Subscribe/Unsubscribe format from `encode_namespace_list`), OR use a nested FlatBuffer table inside the payload.

**Recommendation: Use raw binary encoding matching the existing encode_namespace_list format.** Reasons:
1. `encode_namespace_list` / `decode_namespace_list` already exist on PeerManager (lines 163-168) and handle exactly this format: `[uint16_be count][ns_id:32]...`
2. Every other message in the codebase uses raw binary payloads, not nested FlatBuffers
3. The schema addition is just the enum value (SyncNamespaceAnnounce = 62), no table needed
4. Reuse avoids new code for encoding/decoding

**Counter-argument for D-11 (FlatBuffer struct):** D-11 says "FlatBuffer schema uses a vector of fixed-size structs." This could mean the user wants a FlatBuffer-encoded payload. But the context also says "same dispatch pattern as all other messages via handle_message()" (D-10), and all other messages use raw binary. The enum addition to transport.fbs IS the FlatBuffers integration -- the payload encoding is a separate concern.

**Recommendation:** Use raw binary with the existing encode/decode functions. If the user explicitly wants FlatBuffer-encoded payloads, that's a deviation from every other message in the codebase. Flag this as a discretion area for the planner.

**Confidence:** MEDIUM -- D-11 could be interpreted either way. The existing pattern strongly favors raw binary.

### Relay Blocklist Update

The relay's `is_client_allowed()` in `relay/core/message_filter.cpp` uses a blocklist approach (switch-case returns false for peer-internal types). Adding SyncNamespaceAnnounce = 62 requires:
1. Add `case TransportMsgType_SyncNamespaceAnnounce:` to the blocklist switch
2. Update the test in `db/tests/relay/test_message_filter.cpp` (add `CHECK_FALSE(is_client_allowed(TransportMsgType_SyncNamespaceAnnounce))`)
3. Update the comment "17 peer-only types" to "18 peer-only types"

**Confidence:** HIGH -- mechanical addition to existing blocklist.

### PeerInfo Struct Addition

Add `announced_namespaces` field to PeerInfo:
```cpp
// Phase 86: Namespace filtering (peer's declared replication scope)
std::set<std::array<uint8_t, 32>> announced_namespaces;
```

This mirrors the existing `subscribed_namespaces` field (line 58). Both are `std::set<std::array<uint8_t, 32>>`. Connection-scoped (reset on reconnect via PeerInfo recreation).

**Confidence:** HIGH -- exact same type as existing field.

### Re-announce on SIGHUP (D-02)

When sync_namespaces changes on SIGHUP, iterate all TCP peers and send the updated SyncNamespaceAnnounce. This is in reload_config(), after the sync_namespaces reload block:

```cpp
// Re-announce sync_namespaces to all connected peers (D-02)
auto announce_payload = encode_namespace_list(
    std::vector<std::array<uint8_t, 32>>(sync_namespaces_.begin(), sync_namespaces_.end()));
for (auto& peer : peers_) {
    if (peer.connection->is_uds()) continue;
    auto conn = peer.connection;
    auto payload_copy = announce_payload;
    asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
        co_await conn->send_message(wire::TransportMsgType_SyncNamespaceAnnounce,
                                     std::span<const uint8_t>(p));
    }, asio::detached);
}
```

Incoming re-announces from peers are handled in `on_peer_message()` dispatch -- update the peer's `announced_namespaces` set.

**Confidence:** HIGH -- follows co_spawn send pattern from on_blob_ingested.

### Recommended Project Structure Changes

```
db/schemas/transport.fbs           # Add SyncNamespaceAnnounce = 62
db/wire/transport_generated.h      # Auto-regenerated by flatc
db/peer/peer_manager.h             # Add announced_namespaces to PeerInfo, max_peers_ member
db/peer/peer_manager.cpp           # Announce exchange, filtering, max_peers reload
relay/core/message_filter.cpp      # Add type 62 to blocklist
db/tests/peer/test_peer_manager.cpp  # Unit tests for filtering + max_peers
db/tests/relay/test_message_filter.cpp  # Blocklist test update
db/PROTOCOL.md                     # Document SyncNamespaceAnnounce + max_peers reload
```

### Anti-Patterns to Avoid
- **Sending announce as part of handshake:** The announce happens AFTER handshake (post-auth), not during it. Handshake is in Connection class; announce is peer-level logic in PeerManager.
- **Blocking the message loop for announce recv:** Use the async recv_sync_msg pattern, never block.
- **Filtering on subscribed_namespaces for BlobNotify:** D-06 explicitly says use sync_namespaces (replication config), not subscribed_namespaces (client pub/sub).
- **Mass-disconnecting excess peers on max_peers reduction:** D-12 explicitly prohibits this. Refuse new connections, let count drain naturally.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Namespace list encoding | Custom binary encoder | Existing `PeerManager::encode_namespace_list` / `decode_namespace_list` | Already handles `[u16BE count][ns:32]...` format, battle-tested |
| Async message receive with timeout | Custom polling/flag mechanism | Existing `recv_sync_msg()` + `route_sync_message()` | Timer-cancel pattern handles timeout, cancellation, and queue draining |
| FlatBuffers enum code gen | Manual constant definitions | `flatc --cpp --gen-object-api` on transport.fbs | CMake custom command auto-regenerates transport_generated.h |

## Common Pitfalls

### Pitfall 1: config_ is const -- max_peers not writable
**What goes wrong:** Trying to reload max_peers by modifying `config_.max_peers` fails because `config_` is a `const Config&`.
**Why it happens:** Other reloadable params (sync_cooldown_seconds_, safety_net_interval_seconds_) use separate mutable members. max_peers does not (yet).
**How to avoid:** Add `uint32_t max_peers_` member to PeerManager, initialize from `config_.max_peers` in constructor, use `max_peers_` in `should_accept_connection()` and `handle_peer_list_response()`.
**Warning signs:** Compile error on `config_.max_peers = new_cfg.max_peers` (const ref).

### Pitfall 2: Announce-before-message-loop timing
**What goes wrong:** Trying to receive the peer's announce before the message loop is running means no messages will arrive.
**Why it happens:** `on_peer_connected` is fired from `ready_cb_` which runs after handshake but the message loop is running concurrently.
**How to avoid:** Use `recv_sync_msg()` in a co_spawned coroutine. Messages arriving via message_loop are dispatched to `on_peer_message()` which routes them to `route_sync_message()`, making them available to `recv_sync_msg()`.
**Warning signs:** Timeout waiting for SyncNamespaceAnnounce in tests.

### Pitfall 3: Empty announced set vs never announced
**What goes wrong:** Treating a peer that hasn't sent an announce yet differently from one that sent an empty announce.
**Why it happens:** PeerInfo.announced_namespaces starts empty. If filtering is applied before the announce is received, no BlobNotify will be sent to that peer.
**How to avoid:** D-07 says both cases mean "replicate everything." The default empty set in PeerInfo means "no filter applied" (pass all). The filter check is: `if (!announced_namespaces.empty() && !announced_namespaces.count(ns)) skip;`
**Warning signs:** New peers not receiving BlobNotify before announce completes.

### Pitfall 4: Re-announce race with sync-on-connect
**What goes wrong:** SIGHUP triggers re-announce while a sync session is in progress. The re-announce message arrives as a sync_inbox message and confuses the sync state machine.
**Why it happens:** SyncNamespaceAnnounce is routed through route_sync_message (same queue as sync messages).
**How to avoid:** Handle SyncNamespaceAnnounce INLINE in on_peer_message (like Subscribe/Unsubscribe), NOT through the sync inbox. The announce updates PeerInfo.announced_namespaces directly. Only the initial handshake-phase announce uses recv_sync_msg (before sync starts).
**Warning signs:** "unexpected message type 62 during reconciliation" warnings in logs.

**CRITICAL: This pitfall changes the recommended architecture.** The initial announce exchange should use a dedicated mechanism (e.g., a one-shot timer/flag on PeerInfo) rather than the sync inbox, OR the sync inbox dispatch should handle SyncNamespaceAnnounce specially (update filter and continue rather than treating it as unexpected). The simplest approach: always handle SyncNamespaceAnnounce inline in on_peer_message, and for the initial wait, use a dedicated flag/timer on PeerInfo that the announce_and_sync coroutine polls.

**Revised recommendation:** Handle SyncNamespaceAnnounce as an INLINE dispatch in on_peer_message (same category as Subscribe/Unsubscribe). For the initial wait in announce_and_sync, add a `bool announce_received` flag and a `asio::steady_timer* announce_notify` to PeerInfo, using the same timer-cancel pattern as sync_notify.

### Pitfall 5: Forgetting to filter the responder's Phase A namespace list
**What goes wrong:** Responder sends its full namespace list in Phase A, including namespaces the peer doesn't replicate. The initiator then tries to reconcile those namespaces, wasting bandwidth.
**Why it happens:** Currently Phase A NamespaceList is filtered only by local sync_namespaces_, not by the remote peer's announced set.
**How to avoid:** After receiving the peer's announce, also filter the Phase A NamespaceList by the peer's announced_namespaces (in addition to local sync_namespaces_).
**Warning signs:** Reconciliation for namespaces one peer doesn't care about.

## Code Examples

### FlatBuffers Schema Addition
```flatbuffers
// In db/schemas/transport.fbs, add after BlobFetchResponse = 61:
    // Phase 86: Namespace filtering
    SyncNamespaceAnnounce = 62
```
Source: Existing transport.fbs pattern (each new type gets next sequential ID)

### PeerInfo Struct Extension
```cpp
// In db/peer/peer_manager.h, PeerInfo struct:
struct PeerInfo {
    // ... existing fields ...

    // Phase 86: Peer's declared replication scope (empty = replicate all)
    std::set<std::array<uint8_t, 32>> announced_namespaces;

    // Phase 86: Initial announce handshake coordination
    bool announce_received = false;
    asio::steady_timer* announce_notify = nullptr;
};
```

### Inline Announce Handling in on_peer_message
```cpp
// In on_peer_message(), after Subscribe/Unsubscribe handling:
if (type == wire::TransportMsgType_SyncNamespaceAnnounce) {
    auto* peer = find_peer(conn);
    if (peer) {
        auto namespaces = decode_namespace_list(payload);
        peer->announced_namespaces.clear();
        for (const auto& ns : namespaces) {
            peer->announced_namespaces.insert(ns);
        }
        peer->announce_received = true;
        if (peer->announce_notify) peer->announce_notify->cancel();
        spdlog::info("peer {} announced {} sync namespaces",
                     peer_display_name(conn),
                     peer->announced_namespaces.empty() ? "all" :
                     std::to_string(peer->announced_namespaces.size()));
    }
    return;
}
```
Source: Follows Subscribe handling pattern (line 678-689 of peer_manager.cpp)

### Announce-and-Sync Coroutine
```cpp
asio::awaitable<void> PeerManager::announce_and_sync(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer) co_return;

    // Send our sync_namespaces
    auto ns_list = std::vector<std::array<uint8_t, 32>>(
        sync_namespaces_.begin(), sync_namespaces_.end());
    auto payload = encode_namespace_list(ns_list);
    if (!co_await conn->send_message(
            wire::TransportMsgType_SyncNamespaceAnnounce, payload)) {
        co_return;
    }

    // Wait for peer's announce (timeout 5s)
    if (!peer->announce_received) {
        asio::steady_timer timer(ioc_);
        peer->announce_notify = &timer;
        timer.expires_after(std::chrono::seconds(5));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        peer->announce_notify = nullptr;
        if (!peer->announce_received) {
            spdlog::warn("peer {} did not send SyncNamespaceAnnounce within 5s",
                         peer_display_name(conn));
            co_return;  // Treat as "replicate all" (D-07)
        }
    }

    // Initiator triggers sync after announce exchange
    if (conn->is_initiator()) {
        co_await run_sync_with_peer(conn);
    }
}
```

### BlobNotify Filtering in on_blob_ingested
```cpp
// Modified BlobNotify fan-out loop:
for (auto& peer : peers_) {
    if (peer.connection == source) continue;
    if (peer.connection->is_uds()) continue;

    // Phase 86: Namespace filtering (D-05, D-07)
    // Empty announced set = replicate all (no filter)
    if (!peer.announced_namespaces.empty() &&
        peer.announced_namespaces.count(namespace_id) == 0) continue;

    auto conn = peer.connection;
    auto payload_copy = payload;
    asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
        co_await conn->send_message(wire::TransportMsgType_BlobNotify,
                                     std::span<const uint8_t>(p));
    }, asio::detached);
}
```

### max_peers Reload Pattern
```cpp
// In reload_config(), after compaction reload:
auto old_max_peers = max_peers_;
max_peers_ = new_cfg.max_peers;
if (peers_.size() > max_peers_) {
    spdlog::warn("config reload: max_peers={} but {} peers connected "
                 "(excess will drain naturally, new connections refused)",
                 max_peers_, peers_.size());
} else {
    spdlog::info("config reload: max_peers={}", max_peers_);
}
```

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (FetchContent, catch_discover_tests) |
| Quick run command | `cd build && ctest -R "test_name" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| FILT-01 | Peers exchange SyncNamespaceAnnounce after handshake | unit | `cd build && ctest -R "peer_manager.*announce" --output-on-failure` | Wave 0 |
| FILT-01 | FlatBuffers enum includes SyncNamespaceAnnounce=62 | unit | `cd build && ctest -R "message_filter" --output-on-failure` | Extend existing |
| FILT-02 | BlobNotify filtered by peer's announced namespaces | unit | `cd build && ctest -R "peer_manager.*filter" --output-on-failure` | Wave 0 |
| FILT-02 | Empty announced set receives all BlobNotify | unit | `cd build && ctest -R "peer_manager.*empty" --output-on-failure` | Wave 0 |
| OPS-01 | max_peers reloadable via SIGHUP | integration | Docker compose test script | Wave 0 |
| OPS-01 | Excess peers not disconnected on reduction | integration | Docker compose test script | Wave 0 |
| FILT-01+02 | End-to-end: namespace-filtered sync between 3 nodes | integration | Docker compose test script | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "peer_manager|message_filter" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green + Docker integration tests before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/peer/test_namespace_announce.cpp` -- covers FILT-01, FILT-02 (unit tests for announce exchange, filtering logic)
- [ ] `tests/integration/test_filt01_namespace_filtering.sh` -- Docker E2E for namespace-filtered sync
- [ ] `tests/integration/test_ops01_max_peers_sighup.sh` -- Docker E2E for max_peers hot reload
- [ ] Add new test file to `db/CMakeLists.txt` test target source list

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Sender-only namespace filter (sync_namespaces_) | Sender + receiver intersection filter (sync_namespaces_ + announced_namespaces) | Phase 86 | Eliminates wasted BlobNotify bandwidth; reconciliation scoped to mutual interest |
| Fixed max_peers (restart required) | SIGHUP-reloadable max_peers | Phase 86 | Operational flexibility without downtime |
| Full BlobNotify broadcast to all TCP peers | Filtered BlobNotify based on peer's announced replication set | Phase 86 | Bandwidth savings proportional to namespace partitioning |

## Open Questions

1. **D-11 FlatBuffer struct vs raw binary for announce payload**
   - What we know: D-11 mentions "FlatBuffer schema uses a vector of fixed-size structs." All other messages use raw binary payloads with hand-rolled encode/decode.
   - What's unclear: Whether the user wants a FlatBuffer-encoded payload inside TransportMessage.payload, or just the enum addition to the FlatBuffers schema (with raw binary payload like everything else).
   - Recommendation: Use raw binary encoding with existing `encode_namespace_list`/`decode_namespace_list`. Only the enum value needs to be in the FlatBuffer schema. This is consistent with all 61 other message types.

2. **Announce timeout behavior when peer doesn't respond**
   - What we know: D-07 says no distinction between "never announced" and "announced empty set."
   - What's unclear: Should we disconnect a peer that never sends the announce, or just proceed with "replicate all" semantics?
   - Recommendation: Log a warning and proceed with "replicate all" (empty set). This is maximally permissive and matches D-07 intent.

## Sources

### Primary (HIGH confidence)
- `db/peer/peer_manager.h` -- PeerInfo struct, PeerManager class (lines 46-368)
- `db/peer/peer_manager.cpp` -- on_peer_connected (line 311), on_blob_ingested (line 3032), reload_config (line 2745), run_sync_with_peer (line 1824), handle_sync_as_responder (line 2245), on_peer_message dispatch (line 524), should_accept_connection (line 307)
- `db/schemas/transport.fbs` -- TransportMsgType enum (BlobFetchResponse=61 is last, next=62)
- `db/config/config.h` -- Config struct, max_peers field (line 20)
- `db/config/config.cpp` -- load_config, validate_config (max_peers >= 1 validation)
- `relay/core/message_filter.cpp` -- is_client_allowed blocklist switch (line 8-47)
- `db/tests/relay/test_message_filter.cpp` -- Blocklist test (17 peer-only types currently)
- `db/sync/sync_protocol.h` -- SyncProtocol class
- `db/sync/sync_protocol.cpp` -- encode/decode helpers, Phase A/B/C message encoding
- `db/net/connection.h` -- Connection class, send_message, is_uds, is_initiator
- `.planning/phases/86-namespace-filtering-hot-reload/86-CONTEXT.md` -- All locked decisions

### Secondary (MEDIUM confidence)
- Existing `encode_namespace_list`/`decode_namespace_list` format inference from Subscribe/Unsubscribe handling (lines 163-168, 678-704 of peer_manager.cpp)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing C++20/FlatBuffers/Catch2
- Architecture: HIGH -- all patterns directly extend existing codebase mechanisms
- Pitfalls: HIGH -- identified from direct code inspection of the exact files that need modification

**Research date:** 2026-04-05
**Valid until:** 2026-05-05 (stable codebase, no external dependency changes)
