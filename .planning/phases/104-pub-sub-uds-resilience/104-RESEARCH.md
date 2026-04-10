# Phase 104: Pub/Sub & UDS Resilience - Research

**Researched:** 2026-04-10
**Domain:** WebSocket pub/sub aggregation, UDS connection lifecycle, coroutine-based reconnect
**Confidence:** HIGH

## Summary

Phase 104 adds subscription aggregation with reference counting, notification fan-out from the node to WebSocket clients, UDS auto-reconnect with subscription replay, and pending request cleanup on disconnect. All five requirements (MUX-03 through MUX-07) are well-scoped with locked decisions in CONTEXT.md.

The implementation introduces one new class (SubscriptionTracker) and extends two existing ones (UdsMultiplexer for reconnect lifecycle, WsSession for Subscribe/Unsubscribe interception). The existing codebase has explicit stubs and TODO comments at the exact insertion points. The relay translator has a wire format mismatch for Subscribe/Unsubscribe payloads (u32BE count vs node's u16BE count) that must be handled by building binary payloads directly in SubscriptionTracker rather than using the generic json_to_binary() path.

**Primary recommendation:** Build SubscriptionTracker as a standalone class following the existing component-per-concern pattern. Have it encode Subscribe/Unsubscribe binary payloads directly with u16BE count prefix (bypassing the translator's HEX_32_ARRAY which uses u32BE). Extend UdsMultiplexer::read_loop to re-enter connect_loop on disconnect with AEAD state reset and subscription replay after successful reconnect.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: New class `SubscriptionTracker` in `relay/core/subscription_tracker.h/cpp`. Separate concern from UdsMultiplexer, follows the existing component-per-concern pattern (RequestRouter, MessageFilter, Authenticator).
- D-02: Data structure: `unordered_map<Namespace32, unordered_set<uint64_t>>` mapping namespace (32-byte array) to set of subscribed session IDs. When a session subscribes and the namespace set transitions from empty to 1 subscriber, send Subscribe to node. When the last session unsubscribes (set becomes empty), send Unsubscribe to node. No Subscribe/Unsubscribe forwarded to node for already-tracked namespaces.
- D-03: Per-client subscription cap: 256 namespaces per session, matching the node's default `max_subscriptions`. Exceeding sends `{"type":"error","code":"subscription_limit","message":"Maximum 256 subscriptions per client"}` with the client's request_id.
- D-04: Interception point: WsSession::on_message() AUTHENTICATED path intercepts Subscribe (type 19) and Unsubscribe (type 20) BEFORE RequestRouter registration. These are not request-response messages that need relay_rid routing -- they go to SubscriptionTracker which decides whether to forward to node.
- D-05: Client disconnect cleanup: `SubscriptionTracker::remove_client(session_id)` removes the session from all namespace sets. For any namespace whose set becomes empty, send Unsubscribe to node. Called from SessionManager when session closes.
- D-06: UdsMultiplexer::route_response for `request_id == 0` with `type == Notification (21)`: extract namespace_id from first 32 bytes of binary payload, query SubscriptionTracker for set of session IDs subscribed to that namespace, translate binary payload to JSON via `binary_to_json()`, send JSON to each subscribed session via SessionManager::get_session() + send_json().
- D-07: Notifications are sent as text frames (opcode 0x1) -- they are small (77 bytes binary, ~200 bytes JSON). Binary frames reserved for ReadResponse/BatchReadResponse only (Phase 103 D-20).
- D-08: StorageFull (22) and QuotaExceeded (25) with request_id=0 are server-initiated broadcasts -- fan out to ALL connected sessions, not just subscribed ones. These are operational warnings, not namespace-specific.
- D-09: Extend existing connect_loop with reconnect awareness. When UDS read_loop exits (socket error/EOF), reset `connected_` flag, clear AEAD state (keys + counters), close socket, and re-enter connect_loop with the same jittered backoff (1s base, 30s cap) from Phase 103 D-04.
- D-10: After successful reconnect (handshake complete), replay all active subscriptions from SubscriptionTracker. Single batched Subscribe message containing all namespaces from the aggregate set (union of all per-namespace keys in the tracker). Same encode_namespace_list format the node expects for Subscribe payloads.
- D-11: Clients stay connected during UDS reconnect. New requests during reconnect receive `{"type":"error","code":"node_unavailable","message":"Node connection not ready"}` (existing Phase 103 D-04 behavior). No client disconnection. Notifications resume automatically after reconnect + subscription replay.
- D-12: No message queuing during UDS reconnect -- this is explicitly out of scope per REQUIREMENTS.md. Pending requests fail, subscriptions replay, clients re-send if needed.
- D-13: On UDS disconnect, RequestRouter bulk-fail: iterate all pending entries, send `{"type":"error","code":"node_disconnected","message":"Node connection lost"}` to each client's session with their original request_id restored, then clear the pending map. Called from UdsMultiplexer when read_loop detects socket close.
- D-14: Ordering: cleanup pending requests FIRST, then reset AEAD state, then re-enter connect_loop. Clean state before retry. Clients can re-send requests once UDS reconnects and `is_connected()` returns true.

### Claude's Discretion
- Internal API design for SubscriptionTracker (method signatures, hash function for Namespace32)
- Whether to translate the notification JSON once and share the string across all subscribers, or translate per-session (former is an obvious optimization)
- Exact log messages and spdlog levels for subscription events
- Test organization within relay/tests/

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| MUX-03 | Subscription aggregation with reference counting (first subscribe sends to node, last unsubscribe sends to node) | SubscriptionTracker class with unordered_map<Namespace32, unordered_set<uint64_t>>. Wire format: u16BE count + 32-byte namespace IDs. Interception in WsSession before RequestRouter. |
| MUX-04 | Notification fan-out from node to subscribed WebSocket clients | UdsMultiplexer::route_response request_id=0 stub at line 458. Notification (type 21) is 77 bytes binary. Translate once, send JSON text frame to all subscribers. StorageFull/QuotaExceeded broadcast to ALL sessions. |
| MUX-05 | UDS auto-reconnect with jittered backoff on node disconnect | Extend existing connect_loop (already has jittered backoff). read_loop exits on recv failure, already re-spawns connect_loop at line 433. Add AEAD state reset + pending request cleanup. |
| MUX-06 | Subscription replay after UDS reconnect | After handshake in connect_loop, call SubscriptionTracker::get_all_namespaces(), encode as single batched Subscribe, send to node. |
| MUX-07 | Pending request timeout on UDS disconnect (no orphaned client requests) | RequestRouter needs bulk_fail_all() method: iterate pending_, send error JSON to each client, clear map. Called from UdsMultiplexer before reconnect. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Catch2 | v3.7.1 | Test framework | Already in relay test suite, FetchContent |
| nlohmann/json | 3.x | JSON creation for error responses and notification translation | Already linked in relay |
| spdlog | 1.x | Structured logging | Already linked in relay |
| Standalone Asio | latest | io_context, timers, coroutines | Already linked in relay |

No new dependencies required. All existing relay libraries suffice.

## Architecture Patterns

### Recommended Project Structure
```
relay/
  core/
    subscription_tracker.h     # NEW: Subscription aggregation
    subscription_tracker.cpp   # NEW: Reference counting + namespace encoding
    uds_multiplexer.h          # MODIFIED: Add SubscriptionTracker ref, reconnect support
    uds_multiplexer.cpp        # MODIFIED: Pending cleanup, subscription replay, notification fan-out
    request_router.h           # MODIFIED: Add bulk_fail_all() method
    request_router.cpp         # MODIFIED: Implement bulk_fail_all()
  ws/
    ws_session.h               # MODIFIED: Add SubscriptionTracker pointer
    ws_session.cpp             # MODIFIED: Subscribe/Unsubscribe interception
    ws_acceptor.h/cpp          # MODIFIED: Pass SubscriptionTracker to WsSession factory
    session_manager.cpp        # MODIFIED (maybe): cleanup hook for SubscriptionTracker
  tests/
    test_subscription_tracker.cpp  # NEW: Unit tests
    test_uds_reconnect.cpp         # NEW: Reconnect lifecycle tests (or extend existing)
```

### Pattern 1: Component-Per-Concern (Locked)
**What:** Each relay subsystem is a separate class in relay/core/ with explicit dependencies injected at construction.
**When to use:** All new relay components.
**Example:**
```cpp
// Follows Authenticator, MessageFilter, RequestRouter pattern
class SubscriptionTracker {
public:
    using Namespace32 = std::array<uint8_t, 32>;

    struct SubscribeResult {
        bool forward_to_node;  // true if first subscriber for this namespace
        std::vector<Namespace32> new_namespaces;  // namespaces to send to node
    };

    struct UnsubscribeResult {
        bool forward_to_node;  // true if last subscriber removed
        std::vector<Namespace32> removed_namespaces;  // namespaces to unsubscribe from node
    };

    SubscribeResult subscribe(uint64_t session_id,
                              const std::vector<Namespace32>& namespaces);
    UnsubscribeResult unsubscribe(uint64_t session_id,
                                  const std::vector<Namespace32>& namespaces);
    void remove_client(uint64_t session_id);

    std::vector<Namespace32> get_all_namespaces() const;
    size_t client_subscription_count(uint64_t session_id) const;

private:
    // D-02: namespace -> set of session IDs
    std::unordered_map<Namespace32, std::unordered_set<uint64_t>, Namespace32Hash> subs_;
    // Per-client tracking for cap enforcement and cleanup
    std::unordered_map<uint64_t, std::unordered_set<Namespace32, Namespace32Hash>> client_subs_;
};
```

### Pattern 2: Subscribe/Unsubscribe Interception (D-04)
**What:** Intercept Subscribe (type 19) and Unsubscribe (type 20) in WsSession::on_message() BEFORE the RequestRouter registration path, similar to the existing fire-and-forget pattern for Ping/Pong/Goodbye.
**When to use:** After type translation, before UDS forwarding.
**Example:**
```cpp
// In WsSession::on_message(), after json_to_binary() succeeds:
auto wire_type = result->wire_type;

// Subscribe/Unsubscribe interception (Phase 104 D-04):
if (wire_type == 19 || wire_type == 20) {
    // Parse namespace list from JSON (not from binary -- avoid u32/u16 mismatch)
    auto namespaces = parse_namespace_list_from_json(j);
    if (wire_type == 19) {
        // Check per-client cap (D-03)
        if (tracker_->client_subscription_count(session_id_) + namespaces.size() > 256) {
            send_error("subscription_limit", "Maximum 256 subscriptions per client", request_id);
            co_return;
        }
        auto result = tracker_->subscribe(session_id_, namespaces);
        if (result.forward_to_node && uds_mux_ && uds_mux_->is_connected()) {
            auto payload = encode_namespace_list_u16be(result.new_namespaces);
            auto msg = wire::TransportCodec::encode(TransportMsgType_Subscribe, payload, 0);
            uds_mux_->send(std::move(msg));
        }
    } else {
        auto result = tracker_->unsubscribe(session_id_, namespaces);
        if (result.forward_to_node && uds_mux_ && uds_mux_->is_connected()) {
            auto payload = encode_namespace_list_u16be(result.removed_namespaces);
            auto msg = wire::TransportCodec::encode(TransportMsgType_Unsubscribe, payload, 0);
            uds_mux_->send(std::move(msg));
        }
    }
    co_return;  // Do NOT register with RequestRouter
}
```

### Pattern 3: Translate-Once Notification Fan-out (D-06, Discretion)
**What:** Translate notification binary-to-JSON once, then send the same JSON string to all subscribed sessions. Avoids redundant binary_to_json() calls for popular namespaces.
**Example:**
```cpp
// In UdsMultiplexer::route_response(), request_id == 0:
if (type == 21) {  // Notification
    if (payload.size() < 32) return;  // Need at least namespace_id

    // Extract namespace from first 32 bytes
    std::array<uint8_t, 32> ns{};
    std::memcpy(ns.data(), payload.data(), 32);

    // Get subscribed sessions
    auto session_ids = tracker_->get_subscribers(ns);
    if (session_ids.empty()) return;

    // Translate ONCE
    auto json_opt = translate::binary_to_json(type, payload);
    if (!json_opt) return;

    // Fan-out to all subscribers
    auto json_str = json_opt->dump();
    for (uint64_t sid : session_ids) {
        auto session = sessions_.get_session(sid);
        if (session) session->send_json(*json_opt);
        // Or: send the pre-serialized string to avoid re-dumping
    }
}
```

### Pattern 4: UDS Reconnect Lifecycle (D-09, D-14)
**What:** When read_loop detects socket close: (1) fail pending requests, (2) reset AEAD state, (3) re-enter connect_loop. After reconnect handshake: (4) replay subscriptions.
**Example:**
```cpp
// Modified read_loop on failure:
asio::awaitable<void> UdsMultiplexer::read_loop() {
    while (connected_) {
        auto msg = co_await recv_encrypted();
        if (!msg) {
            connected_ = false;

            // D-14 ordering: cleanup first
            // 1. Bulk-fail all pending requests
            bulk_fail_pending_requests();

            // 2. Reset AEAD state
            send_key_.clear();
            recv_key_.clear();
            send_counter_ = 0;
            recv_counter_ = 0;

            // 3. Close socket
            asio::error_code ec;
            socket_.close(ec);

            // 4. Clear send queue (stale encrypted data)
            send_queue_.clear();
            draining_ = false;

            // 5. Re-enter connect_loop
            asio::co_spawn(ioc_, connect_loop(), asio::detached);
            co_return;
        }
        // ... existing decode + route ...
    }
}

// Modified connect_loop -- after handshake:
// ... existing handshake code ...
connected_ = true;
attempt = 0;

// Replay subscriptions (D-10)
replay_subscriptions();

// Spawn read loop + cleanup loop as before
```

### Anti-Patterns to Avoid
- **Using json_to_binary() for Subscribe/Unsubscribe forwarding to node:** The translator's HEX_32_ARRAY encoding uses u32BE count prefix, but the node's decode_namespace_list expects u16BE count prefix. Build binary payloads directly with u16BE count.
- **Registering Subscribe/Unsubscribe with RequestRouter:** These are fire-and-forget from the node's perspective. The node does not send any response. Registering them would create orphaned pending entries that time out after 60s.
- **Translating notification JSON per-session:** Wasteful for popular namespaces. Translate once, share the JSON string.
- **Queueing messages during UDS reconnect:** Explicitly out of scope (REQUIREMENTS.md "Out of Scope" table). Pending requests fail, clients re-send.
- **Setting max reconnect attempts:** The new relay should reconnect indefinitely (infinite) since the node is on the same machine. The old relay had a max of 10 attempts -- that was a different architecture.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Namespace list binary encoding | Generic HEX_32_ARRAY translator path | Direct u16BE count + raw namespace bytes | Wire format mismatch (u32BE vs u16BE) |
| Jittered backoff | New backoff implementation | Existing connect_loop backoff pattern | Already proven in Phase 103 |
| Session fan-out iteration | Custom session iteration | SessionManager::get_session() per ID | Already has O(1) lookup by ID |
| Binary-to-JSON notification translation | Custom notification parser | Existing translate::binary_to_json(21, ...) | Already handles NOTIFICATION_FIELDS schema |

**Key insight:** The SubscriptionTracker owns the namespace-to-session mapping, but it does NOT own the UDS send logic. It returns results (forward_to_node, new_namespaces) and the caller (WsSession or UdsMultiplexer) decides whether/how to send.

## Common Pitfalls

### Pitfall 1: Subscribe/Unsubscribe Wire Format Mismatch
**What goes wrong:** The relay translator's HEX_32_ARRAY encoding uses u32BE (4 bytes) for the count prefix. The node's `decode_namespace_list()` in peer_manager.cpp reads u16BE (2 bytes) for the count prefix. If the relay sends a Subscribe payload through the generic translator, the node will misparse the count and reject or misinterpret the namespace list.
**Why it happens:** HEX_32_ARRAY was designed for ListResponse hashes, BatchReadRequest hashes, etc. -- all of which use u32BE counts on the wire. Subscribe/Unsubscribe happen to use the same "array of 32-byte values" pattern but with u16BE counts (PROTOCOL.md: "count: 2 bytes BE uint16").
**How to avoid:** SubscriptionTracker (or a helper function) must encode Subscribe/Unsubscribe payloads directly: `[count_u16_be][ns:32][ns:32]...`. Do NOT use json_to_binary() for these message types.
**Warning signs:** Node logs showing "invalid Subscribe payload" or subscriptions silently failing.

### Pitfall 2: Send Queue Corruption on UDS Disconnect
**What goes wrong:** When read_loop detects disconnect, the send_queue_ may contain messages encrypted with the old AEAD keys. If the queue is not cleared before reconnect, the drain coroutine could try to send stale encrypted data over the new connection.
**Why it happens:** `send()` enqueues to send_queue_ and `drain_send_queue()` may be running concurrently. If drain is mid-flight when disconnect happens, it could interleave with reconnect.
**How to avoid:** On disconnect, set `connected_ = false` FIRST (prevents new enqueue via `send()` guard), then clear send_queue_ and set `draining_ = false`. The drain coroutine checks `connected_` in its loop condition and will exit.
**Warning signs:** AEAD decrypt failures immediately after reconnect.

### Pitfall 3: Pending Request Cleanup Race
**What goes wrong:** bulk_fail_pending_requests() iterates the pending map and sends error JSON to each client's session. If a session is concurrently closing (remove_session called), get_session() returns nullptr but the pending entry still exists.
**Why it happens:** Multi-threaded io_context means session close and UDS disconnect can happen on different threads simultaneously.
**How to avoid:** In bulk_fail_pending_requests(), check get_session() result for nullptr before sending. After iteration, clear the entire pending map regardless. This is already how purge_stale works -- just silently drop entries for gone sessions.
**Warning signs:** Crashes or use-after-free in session send during bulk fail.

### Pitfall 4: Notification Fan-out to Disconnecting Sessions
**What goes wrong:** Between querying SubscriptionTracker for subscribers and calling send_json(), a session may have disconnected. The session's send queue is closed, enqueue fails, but the session pointer is still valid (shared_ptr).
**Why it happens:** Fan-out iterates a snapshot of session IDs. Sessions can close at any time.
**How to avoid:** Check session pointer from get_session() is not nullptr. The existing Session::enqueue() returns false on closed sessions -- this is safe. But do NOT try to remove the client from SubscriptionTracker during fan-out iteration (would invalidate the subscriber set). Let the normal cleanup path handle it.
**Warning signs:** "queue full" warnings during notification fan-out to disconnecting clients.

### Pitfall 5: Subscription Replay Sends Empty Subscribe
**What goes wrong:** After reconnect, if no clients have any subscriptions, replay_subscriptions() could send a Subscribe with count=0 to the node. The node may handle this gracefully (0 namespaces = no-op) but it's unnecessary traffic.
**Why it happens:** Calling get_all_namespaces() when the tracker is empty.
**How to avoid:** Check that get_all_namespaces() returns a non-empty vector before encoding and sending.
**Warning signs:** Unnecessary Subscribe messages in node logs after reconnect.

### Pitfall 6: Namespace32 Hash Function
**What goes wrong:** Using std::hash<std::array<uint8_t,32>> -- not all standard library implementations provide this specialization. Compilation fails on some platforms.
**Why it happens:** std::array does not have a standard hash specialization.
**How to avoid:** Provide a custom hash function for Namespace32. A simple approach: read the first 8 bytes as a uint64_t (the namespace is SHA3-256 output, so the first 8 bytes have excellent distribution).
**Warning signs:** Compilation errors on `unordered_map<Namespace32, ...>`.

## Code Examples

### Namespace32 Hash Function
```cpp
// Source: custom, but trivially correct for SHA3-256 output
struct Namespace32Hash {
    size_t operator()(const std::array<uint8_t, 32>& ns) const noexcept {
        // SHA3-256 output has excellent distribution -- first 8 bytes suffice
        uint64_t h;
        std::memcpy(&h, ns.data(), sizeof(h));
        return static_cast<size_t>(h);
    }
};
```

### Encode Namespace List with u16BE Count (for node wire format)
```cpp
// Source: db/peer/peer_manager.cpp encode_namespace_list() adapted for relay
static std::vector<uint8_t> encode_namespace_list_u16be(
    const std::vector<std::array<uint8_t, 32>>& namespaces) {
    std::vector<uint8_t> result;
    result.reserve(2 + namespaces.size() * 32);
    uint8_t buf[2];
    util::store_u16_be(buf, static_cast<uint16_t>(namespaces.size()));
    result.insert(result.end(), buf, buf + 2);
    for (const auto& ns : namespaces) {
        result.insert(result.end(), ns.begin(), ns.end());
    }
    return result;
}
```

### Parse Namespace List from JSON (client-side format)
```cpp
// Source: derived from json_schema.h SUBSCRIBE_FIELDS
static std::vector<std::array<uint8_t, 32>> parse_namespace_list_from_json(
    const nlohmann::json& j) {
    std::vector<std::array<uint8_t, 32>> result;
    if (!j.contains("namespaces") || !j["namespaces"].is_array()) return result;

    for (const auto& item : j["namespaces"]) {
        auto hex = item.get<std::string>();
        auto bytes = util::from_hex(hex);
        if (!bytes || bytes->size() != 32) continue;
        std::array<uint8_t, 32> ns{};
        std::memcpy(ns.data(), bytes->data(), 32);
        result.push_back(ns);
    }
    return result;
}
```

### RequestRouter Bulk Fail
```cpp
// Extension to request_router.h/cpp
void RequestRouter::bulk_fail_all(
    std::function<void(uint64_t session_id, uint32_t client_rid)> on_fail) {
    for (const auto& [relay_rid, pending] : pending_) {
        on_fail(pending.client_session_id, pending.client_request_id);
    }
    pending_.clear();
}
```

### Notification Fan-out (translate-once optimization)
```cpp
void UdsMultiplexer::handle_notification(std::span<const uint8_t> payload) {
    if (payload.size() < 32) return;

    std::array<uint8_t, 32> ns{};
    std::memcpy(ns.data(), payload.data(), 32);

    auto subscribers = tracker_->get_subscribers(ns);
    if (subscribers.empty()) return;

    // Translate once
    auto json_opt = translate::binary_to_json(21, payload);
    if (!json_opt) return;

    for (uint64_t sid : subscribers) {
        auto session = sessions_.get_session(sid);
        if (session) {
            session->send_json(*json_opt);
        }
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Old relay: per-session NamespaceSet, 3-state lifecycle (Phase 88) | New relay: centralized SubscriptionTracker, infinite reconnect | Phase 104 (now) | Simpler, single source of truth for subscriptions |
| Old relay: max 10 reconnect attempts, then DEAD state | New relay: infinite reconnect with jittered backoff | Phase 104 (now) | Relay never gives up on local node |
| Old relay: replay_pending_ flag gated forwarding during replay | New relay: clients get "node_unavailable" during reconnect, no queueing | Phase 104 (now) | Simpler, explicit failure semantics |

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt |
| Quick run command | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |
| Full suite command | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| MUX-03 | First subscribe sends to node, subsequent are ref-counted; last unsubscribe sends to node | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_tracker]" -x` | Wave 0 |
| MUX-03 | Per-client 256 namespace cap with error response | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_tracker][cap]" -x` | Wave 0 |
| MUX-03 | Client disconnect removes from all namespace sets, unsubscribes from node when last | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_tracker][cleanup]" -x` | Wave 0 |
| MUX-04 | Notification (type 21) routed to subscribed sessions by namespace | unit | `./build/relay/tests/chromatindb_relay_tests "[notification]" -x` | Wave 0 |
| MUX-04 | StorageFull/QuotaExceeded with rid=0 broadcast to ALL sessions | unit | `./build/relay/tests/chromatindb_relay_tests "[broadcast]" -x` | Wave 0 |
| MUX-05 | UDS reconnect with AEAD state reset after disconnect | unit | `./build/relay/tests/chromatindb_relay_tests "[uds_reconnect]" -x` | Wave 0 |
| MUX-06 | Subscription replay after reconnect sends batched Subscribe | unit | `./build/relay/tests/chromatindb_relay_tests "[subscription_replay]" -x` | Wave 0 |
| MUX-07 | Pending requests bulk-failed with error JSON on UDS disconnect | unit | `./build/relay/tests/chromatindb_relay_tests "[bulk_fail]" -x` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cmake --build build && ./build/relay/tests/chromatindb_relay_tests -x`
- **Per wave merge:** Full suite green
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `relay/tests/test_subscription_tracker.cpp` -- covers MUX-03 (reference counting, cap, cleanup)
- [ ] Tests for notification fan-out and bulk-fail can go in existing or new test files

## Sources

### Primary (HIGH confidence)
- `relay/core/uds_multiplexer.h/cpp` -- Current UDS connection lifecycle, route_response stub at line 458
- `relay/core/request_router.h/cpp` -- Pending request map, remove_client pattern
- `relay/ws/ws_session.cpp` lines 454-463 -- Fire-and-forget pattern for Subscribe/Unsubscribe interception
- `relay/ws/session_manager.h` -- SessionManager::get_session() and for_each() for fan-out
- `relay/translate/json_schema.h` lines 72-90 -- SUBSCRIBE_FIELDS, UNSUBSCRIBE_FIELDS, NOTIFICATION_FIELDS
- `db/PROTOCOL.md` lines 521-528 -- Subscribe/Unsubscribe wire format: u16BE count + 32-byte namespace IDs
- `db/peer/peer_manager.cpp` lines 419-444 -- encode_namespace_list/decode_namespace_list (u16BE count)
- `relay/translate/translator.cpp` lines 162-181 -- HEX_32_ARRAY encoder uses u32BE count (mismatch)

### Secondary (MEDIUM confidence)
- `db/peer/blob_push_manager.cpp` lines 60-83 -- Node notification encoding (77 bytes, encode_blob_ref)
- `db/peer/message_dispatcher.cpp` lines 228-268 -- Node Subscribe/Unsubscribe handling

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all existing relay libraries
- Architecture: HIGH - all decisions locked in CONTEXT.md, insertion points identified in source
- Pitfalls: HIGH - wire format mismatch verified by cross-referencing PROTOCOL.md vs translator.cpp
- Code patterns: HIGH - derived directly from existing relay source code

**Research date:** 2026-04-10
**Valid until:** 2026-05-10 (stable -- relay codebase is actively evolving but patterns are established)
