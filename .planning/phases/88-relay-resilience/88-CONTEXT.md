# Phase 88: Relay Resilience - Context

**Gathered:** 2026-04-05
**Status:** Ready for planning

<domain>
## Phase Boundary

The relay survives node restarts transparently — client TCP sessions remain open during UDS reconnection, subscriptions are replayed after reconnect, and Notification messages are filtered per-client by subscribed namespaces. Three capabilities: (1) subscription tracking and notification filtering (FILT-03), (2) UDS auto-reconnect with jittered backoff (RELAY-01), (3) subscription replay after reconnect (RELAY-02).

</domain>

<decisions>
## Implementation Decisions

### UDS reconnection model
- **D-01:** Per-session UDS reconnection (keep current architecture). Each RelaySession reconnects its own UDS independently. No shared UDS — avoids multiplexing complexity and keeps the 1-client-1-UDS-1-node-peer mapping intact.
- **D-02:** Three-state lifecycle: ACTIVE (forwarding), RECONNECTING (backoff loop), DEAD (give up). State machine on RelaySession. New UDS socket per reconnection attempt — don't reuse failed sockets.
- **D-03:** Jittered exponential backoff: 1s base, 30s cap, full jitter (uniform random [0, min(cap, base * 2^attempt)]). Matches SDK auto-reconnect pattern from Phase 84 for consistency.

### Message handling during RECONNECTING state
- **D-04:** Client→node messages are dropped silently during RECONNECTING. No queuing, no error responses. The client SDK has request_id-based timeouts (Phase 84) and will retry after reconnect. This keeps the relay simple and memory-bounded.
- **D-05:** Subscribe and Unsubscribe messages from the client are always tracked locally regardless of UDS state. Even during RECONNECTING, subscription state updates are applied so the replay after reconnect is correct.

### Dead state and client fate
- **D-06:** After 10 consecutive failed reconnection attempts (~5 minutes with 30s cap), the session enters DEAD state. DEAD = disconnect the client TCP immediately. The SDK's auto-reconnect will reconnect to the relay, which will attempt a fresh UDS connection.
- **D-07:** Any successful UDS reconnect resets the attempt counter to 0 and returns to ACTIVE state.

### Subscription tracking and notification filtering (FILT-03)
- **D-08:** Per-session subscription state: `std::unordered_set<std::array<uint8_t, 32>>` on RelaySession storing namespace hashes. 256-namespace cap per client (matching milestone planning decision). Reject Subscribe if cap exceeded.
- **D-09:** Intercept Subscribe (type 19) and Unsubscribe (type 20) in handle_client_message, BEFORE forwarding to node. Parse payload using the same encode_namespace_list/decode_namespace_list format as the node. Track namespaces locally, then forward to node as usual.
- **D-10:** Filter Notification (type 21) in handle_node_message. Extract namespace_id from first 32 bytes of payload (offset 0, per encode_notification format). Forward only if namespace_id is in the client's subscription set. Drop silently otherwise.
- **D-11:** Client disconnect cleans up subscription state automatically — the set lives on RelaySession which is destroyed on teardown. No stale state accumulation.

### Subscription replay (RELAY-02)
- **D-12:** After UDS reconnect (TrustedHello handshake completes via on_ready callback), replay all subscriptions before forwarding normal client messages. For each namespace in the subscription set, send a Subscribe message to the node with the namespace encoded via encode_namespace_list.
- **D-13:** Replay is a batch: encode all namespaces into a single Subscribe message (same format as original client Subscribe). The node's Subscribe handler accumulates namespaces, so one message with all namespaces works.
- **D-14:** Block normal message forwarding until subscription replay completes. Use a `replay_pending_` flag that on_ready sets to true, cleared after replay Send succeeds.

### Claude's Discretion
- Internal state machine implementation details (enum class, transitions)
- Timer/backoff implementation (asio::steady_timer vs manual tracking)
- Whether to add a `SubscriptionTracker` helper class or keep inline on RelaySession
- Test strategy: unit tests for subscription tracking, integration tests for reconnection
- Log levels and messages for state transitions
- How to handle the edge case where client sends Subscribe during replay_pending_ (queue or process immediately)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Relay core (primary modification targets)
- `relay/core/relay_session.h` — RelaySession class, handle_client_message, handle_node_message, teardown
- `relay/core/relay_session.cpp` — Full session lifecycle, UDS connect, message forwarding
- `relay/core/message_filter.h` — is_client_allowed() blocklist, type_name()
- `relay/relay_main.cpp` — Accept loop, session creation, io_context, signal handling

### Relay config
- `relay/config/relay_config.h` — RelayConfig struct, load/validate
- `relay/config/relay_config.cpp` — Config parsing (may need reconnect params)

### Node subscription handling (reference, not modification target)
- `db/peer/peer_manager.cpp:712-738` — Subscribe/Unsubscribe handlers, decode_namespace_list
- `db/peer/peer_manager.h:166-177` — encode_namespace_list, decode_namespace_list, encode_notification signatures

### Wire types
- `db/wire/transport_generated.h` — TransportMsgType enum (Subscribe=19, Unsubscribe=20, Notification=21)

### Connection layer
- `db/net/connection.h` — Connection class, on_ready, on_message, on_close, send_message, is_uds
- `db/net/connection.cpp` — Connection::create_uds_outbound (TrustedHello flow)

### Prior phase context
- `.planning/phases/84-sdk-auto-reconnect/84-CONTEXT.md` — SDK reconnect pattern (consistent backoff params)
- `.planning/phases/86-namespace-filtering-hot-reload/86-CONTEXT.md` — SyncNamespaceAnnounce, BlobNotify filtering

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `encode_namespace_list` / `decode_namespace_list` — static methods on PeerManager, used for Subscribe/Unsubscribe wire format. Relay needs same format for subscription tracking and replay.
- `Connection::create_uds_outbound` — Creates UDS connection with TrustedHello. Relay already uses this in RelaySession::start(). Reconnection reuses same factory.
- `message_filter.h` — Existing blocklist. Subscription filtering is ADDITIONAL to blocklist (node→client direction).

### Established Patterns
- Per-session deque tracking in relay_main.cpp (sessions container)
- Coroutine-based forwarding via co_spawn + lambdas
- Close callback cleanup pattern (on_close removes from sessions deque)
- TrustedHello handshake on UDS (no encryption, just auth)

### Integration Points
- `handle_client_message` — Intercept Subscribe/Unsubscribe here
- `handle_node_message` — Filter Notification here
- `start()` — Add reconnection logic after initial UDS connect
- `handle_node_close` — Trigger RECONNECTING state instead of teardown

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 88-relay-resilience*
*Context gathered: 2026-04-05*
