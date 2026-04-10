# Phase 104: Pub/Sub & UDS Resilience - Context

**Gathered:** 2026-04-10
**Status:** Ready for planning

<domain>
## Phase Boundary

Clients subscribe to namespace changes via JSON Subscribe/Unsubscribe messages. The relay aggregates subscriptions (reference counting) so the node sees a single Subscribe per namespace regardless of how many clients are subscribed. Notifications from the node are fanned out to all subscribed WebSocket clients. When the UDS connection to the node drops, the relay reconnects with jittered backoff, replays all active subscriptions, and fails all pending client requests with an error. No rate limiting (Phase 105), no Prometheus metrics (Phase 105), no graceful shutdown (Phase 105).

</domain>

<decisions>
## Implementation Decisions

### Subscription Aggregation (MUX-03)
- **D-01:** New class `SubscriptionTracker` in `relay/core/subscription_tracker.h/cpp`. Separate concern from UdsMultiplexer, follows the existing component-per-concern pattern (RequestRouter, MessageFilter, Authenticator).
- **D-02:** Data structure: `unordered_map<Namespace32, unordered_set<uint64_t>>` mapping namespace (32-byte array) to set of subscribed session IDs. When a session subscribes and the namespace set transitions from empty to 1 subscriber, send Subscribe to node. When the last session unsubscribes (set becomes empty), send Unsubscribe to node. No Subscribe/Unsubscribe forwarded to node for already-tracked namespaces.
- **D-03:** Per-client subscription cap: 256 namespaces per session, matching the node's default `max_subscriptions`. Exceeding sends `{"type":"error","code":"subscription_limit","message":"Maximum 256 subscriptions per client"}` with the client's request_id.
- **D-04:** Interception point: WsSession::on_message() AUTHENTICATED path intercepts Subscribe (type 19) and Unsubscribe (type 20) BEFORE RequestRouter registration. These are not request-response messages that need relay_rid routing -- they go to SubscriptionTracker which decides whether to forward to node.
- **D-05:** Client disconnect cleanup: `SubscriptionTracker::remove_client(session_id)` removes the session from all namespace sets. For any namespace whose set becomes empty, send Unsubscribe to node. Called from SessionManager when session closes.

### Notification Fan-out (MUX-04)
- **D-06:** UdsMultiplexer::route_response for `request_id == 0` with `type == Notification (21)`: extract namespace_id from first 32 bytes of binary payload, query SubscriptionTracker for set of session IDs subscribed to that namespace, translate binary payload to JSON via `binary_to_json()`, send JSON to each subscribed session via SessionManager::get_session() + send_json().
- **D-07:** Notifications are sent as text frames (opcode 0x1) -- they are small (77 bytes binary, ~200 bytes JSON). Binary frames reserved for ReadResponse/BatchReadResponse only (Phase 103 D-20).
- **D-08:** StorageFull (22) and QuotaExceeded (25) with request_id=0 are server-initiated broadcasts -- fan out to ALL connected sessions, not just subscribed ones. These are operational warnings, not namespace-specific.

### UDS Auto-Reconnect (MUX-05, MUX-06)
- **D-09:** Extend existing connect_loop with reconnect awareness. When UDS read_loop exits (socket error/EOF), reset `connected_` flag, clear AEAD state (keys + counters), close socket, and re-enter connect_loop with the same jittered backoff (1s base, 30s cap) from Phase 103 D-04.
- **D-10:** After successful reconnect (handshake complete), replay all active subscriptions from SubscriptionTracker. Single batched Subscribe message containing all namespaces from the aggregate set (union of all per-namespace keys in the tracker). Same encode_namespace_list format the node expects for Subscribe payloads.
- **D-11:** Clients stay connected during UDS reconnect. New requests during reconnect receive `{"type":"error","code":"node_unavailable","message":"Node connection not ready"}` (existing Phase 103 D-04 behavior). No client disconnection. Notifications resume automatically after reconnect + subscription replay.
- **D-12:** No message queuing during UDS reconnect -- this is explicitly out of scope per REQUIREMENTS.md. Pending requests fail, subscriptions replay, clients re-send if needed.

### Pending Request Cleanup (MUX-07)
- **D-13:** On UDS disconnect, RequestRouter bulk-fail: iterate all pending entries, send `{"type":"error","code":"node_disconnected","message":"Node connection lost"}` to each client's session with their original request_id restored, then clear the pending map. Called from UdsMultiplexer when read_loop detects socket close.
- **D-14:** Ordering: cleanup pending requests FIRST, then reset AEAD state, then re-enter connect_loop. Clean state before retry. Clients can re-send requests once UDS reconnects and `is_connected()` returns true.

### Claude's Discretion
- Internal API design for SubscriptionTracker (method signatures, hash function for Namespace32)
- Whether to translate the notification JSON once and share the string across all subscribers, or translate per-session (former is an obvious optimization)
- Exact log messages and spdlog levels for subscription events
- Test organization within relay/tests/

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Architecture
- `.planning/research/ARCHITECTURE.md` -- Subscription aggregation design (if present), UDS lifecycle, relay component layout

### Protocol
- `db/PROTOCOL.md` -- Subscribe (type 19) and Unsubscribe (type 20) payload format (namespace list encoding), Notification (type 21) 77-byte payload layout, TransportMessage envelope

### Wire Format
- `relay/translate/json_schema.h` lines 71-90 -- Subscribe, Unsubscribe, Notification field specs (SUBSCRIBE_FIELDS, UNSUBSCRIBE_FIELDS, NOTIFICATION_FIELDS)
- `relay/translate/type_registry.h` -- Type mapping for subscribe(19), unsubscribe(20), notification(21)

### Existing Relay Code (Phase 100-103 output)
- `relay/core/uds_multiplexer.h` -- UdsMultiplexer class with connect_loop, read_loop, AEAD state, send queue
- `relay/core/uds_multiplexer.cpp` lines 456-466 -- route_response request_id=0 stub ("Phase 104 handles Notification fan-out")
- `relay/core/request_router.h` -- RequestRouter with pending map, remove_client, purge_stale
- `relay/ws/ws_session.cpp` lines 454-463 -- Fire-and-forget bypass for Ping/Pong/Goodbye (pattern for Subscribe/Unsubscribe interception)
- `relay/ws/ws_session.cpp` lines 465-480 -- RequestRouter registration + UDS forwarding flow
- `relay/ws/session_manager.h` -- SessionManager with for_each(), get_session(), remove_session()
- `relay/core/session.h` -- Session send queue with enqueue/drain

### Node Code (reference for wire format, NOT linked by relay)
- `db/peer/message_dispatcher.cpp` lines 228-268 -- How node handles Subscribe/Unsubscribe (namespace list decode, subscribed_namespaces set)
- `db/peer/blob_push_manager.cpp` lines 60-83 -- How node sends Notification to subscribed peers (encode_notification, 77-byte payload)
- `db/peer/peer_manager.h` line 101 -- encode_notification() signature and decode_namespace_list()

### Prior Phase Context
- `.planning/phases/103-uds-multiplexer-protocol-translation/103-CONTEXT.md` -- UDS multiplexer design, request routing, translation pipeline

### Requirements
- `.planning/REQUIREMENTS.md` -- MUX-03, MUX-04, MUX-05, MUX-06, MUX-07

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/core/uds_multiplexer.cpp` route_response() -- request_id=0 stub is the exact insertion point for notification fan-out (D-06)
- `relay/ws/ws_session.cpp` on_message() AUTHENTICATED path -- fire-and-forget pattern at lines 454-463 is the template for Subscribe/Unsubscribe interception (D-04)
- `relay/translate/json_schema.h` SUBSCRIBE_FIELDS/NOTIFICATION_FIELDS -- existing FieldSpec metadata for JSON<->binary translation of subscription messages
- `relay/core/request_router.h` -- pending map iteration pattern can be extended for bulk-fail (D-13)
- `relay/ws/session_manager.h` for_each() -- already designed for fan-out iteration

### Established Patterns
- Component-per-concern: Authenticator, MessageFilter, RequestRouter are each separate classes in relay/core/. SubscriptionTracker follows this pattern.
- Asio coroutine read/write loops (connect_loop, read_loop, drain_send_queue in UdsMultiplexer)
- Send queue: deque + drain coroutine pattern (Session, UdsMultiplexer)

### Integration Points
- `relay/core/uds_multiplexer.cpp` route_response() request_id=0 case -- plug in notification fan-out
- `relay/ws/ws_session.cpp` on_message() -- intercept Subscribe/Unsubscribe before RequestRouter
- `relay/core/uds_multiplexer.cpp` connect_loop/read_loop -- extend for reconnect lifecycle
- `relay/ws/session_manager.cpp` remove_session() -- trigger SubscriptionTracker::remove_client()
- `relay/relay_main.cpp` -- construct SubscriptionTracker, pass to UdsMultiplexer and WsSession factory

</code_context>

<specifics>
## Specific Ideas

- Subscribe/Unsubscribe payloads are namespace lists: `[count_u16_BE][ns_32_bytes][ns_32_bytes]...`. The translator already handles this via HEX_32_ARRAY FieldEncoding. SubscriptionTracker receives the parsed namespace list from JSON, not raw binary.
- Node sends Notification (type 21) with request_id=0 to the relay's UDS connection. The relay never sends a Subscribe request_id that gets echoed back -- subscriptions are fire-and-forget from the node's perspective.
- The translate-once optimization (translate notification JSON once, send same string to all subscribers) should be implemented -- avoids redundant binary_to_json() calls for popular namespaces.
- Phase 103 D-03 correction: UDS IS encrypted (HKDF-SHA256 key derivation + ChaCha20-Poly1305 AEAD). The AEAD state (send_key_, recv_key_, send_counter_, recv_counter_) must be reset on reconnect before re-handshaking.
- Old relay (Phase 88) had a three-state lifecycle (ACTIVE/RECONNECTING/DEAD) with max 10 reconnect attempts. For the new relay, infinite reconnect with backoff is more appropriate -- the relay should never give up on the node, since it's on the same machine.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 104-pub-sub-uds-resilience*
*Context gathered: 2026-04-10*
