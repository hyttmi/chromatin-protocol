# Phase 88: Relay Resilience - Research

**Researched:** 2026-04-05
**Domain:** C++20 Asio relay session resilience (UDS reconnect, subscription tracking, notification filtering)
**Confidence:** HIGH

## Summary

Phase 88 adds three capabilities to the relay: (1) per-client subscription tracking with notification filtering (FILT-03), (2) automatic UDS reconnection with jittered exponential backoff when the node restarts (RELAY-01), and (3) subscription replay after UDS reconnect so clients resume notifications transparently (RELAY-02). All changes are scoped to relay code only -- the node-side Subscribe/Unsubscribe handling is unchanged.

The relay currently has a simple forwarding model: `RelaySession` pairs one TCP client connection with one UDS connection to the node. Messages pass through `handle_client_message` (client-to-node, with blocklist filtering) and `handle_node_message` (node-to-client, unfiltered). When either side disconnects, `teardown()` closes both. This phase changes `handle_node_close` from teardown to reconnection, adds state tracking to `RelaySession`, and adds subscription-aware filtering to `handle_node_message`.

**Primary recommendation:** Extend `RelaySession` with a three-state enum (ACTIVE/RECONNECTING/DEAD), an `unordered_set<array<uint8_t,32>>` for subscription tracking, and an `asio::steady_timer`-based reconnection loop. The encode/decode functions for namespace lists live on `PeerManager` as static methods -- either call them directly (they are public) or extract the trivial 20-line encode/decode logic into a shared header to avoid the relay depending on PeerManager internals.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Per-session UDS reconnection (keep current architecture). Each RelaySession reconnects its own UDS independently. No shared UDS.
- D-02: Three-state lifecycle: ACTIVE (forwarding), RECONNECTING (backoff loop), DEAD (give up). State machine on RelaySession. New UDS socket per reconnection attempt.
- D-03: Jittered exponential backoff: 1s base, 30s cap, full jitter (uniform random [0, min(cap, base * 2^attempt)]). Matches SDK auto-reconnect from Phase 84.
- D-04: Client-to-node messages dropped silently during RECONNECTING. No queuing, no error responses.
- D-05: Subscribe and Unsubscribe always tracked locally regardless of UDS state. Even during RECONNECTING, subscription state updates are applied.
- D-06: After 10 consecutive failed reconnection attempts (~5 minutes with 30s cap), session enters DEAD state. DEAD = disconnect client TCP immediately.
- D-07: Any successful UDS reconnect resets attempt counter to 0, returns to ACTIVE.
- D-08: Per-session subscription state: `std::unordered_set<std::array<uint8_t, 32>>` with 256-namespace cap. Reject Subscribe if cap exceeded.
- D-09: Intercept Subscribe (type 19) and Unsubscribe (type 20) in handle_client_message BEFORE forwarding to node. Parse using encode_namespace_list/decode_namespace_list format.
- D-10: Filter Notification (type 21) in handle_node_message. Extract namespace_id from first 32 bytes of payload. Forward only if namespace_id is in client's subscription set.
- D-11: Client disconnect cleans up subscription state automatically -- set lives on RelaySession, destroyed on teardown.
- D-12: After UDS reconnect (TrustedHello via on_ready callback), replay all subscriptions before forwarding normal client messages. Encode all namespaces via encode_namespace_list.
- D-13: Replay is a batch: single Subscribe message with all namespaces. Node's Subscribe handler accumulates.
- D-14: Block normal message forwarding until subscription replay completes. Use replay_pending_ flag.

### Claude's Discretion
- Internal state machine implementation details (enum class, transitions)
- Timer/backoff implementation (asio::steady_timer vs manual tracking)
- Whether to add a SubscriptionTracker helper class or keep inline on RelaySession
- Test strategy: unit tests for subscription tracking, integration tests for reconnection
- Log levels and messages for state transitions
- How to handle edge case where client sends Subscribe during replay_pending_ (queue or process immediately)

### Deferred Ideas (OUT OF SCOPE)
None.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| FILT-03 | Relay tracks per-client subscription namespaces and only forwards matching Notification messages | D-08 through D-11 define subscription tracking on RelaySession. Notification payload format confirmed: namespace_id at offset 0 (32 bytes). decode_namespace_list format verified as [u16BE count][ns:32]... |
| RELAY-01 | Relay auto-reconnects to node UDS with jittered backoff when connection is lost | D-01 through D-07 define the three-state lifecycle. Connection::create_uds_outbound is the reconnection factory. asio::steady_timer provides backoff timing. |
| RELAY-02 | Relay replays client subscriptions to node after successful UDS reconnect | D-12 through D-14 define replay behavior. encode_namespace_list produces the Subscribe payload. on_ready callback is the trigger point for replay. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest via FetchContent | io_context, steady_timer, co_spawn, coroutines | Already used throughout relay and db |
| Catch2 | v3.x via FetchContent | Unit tests | Already used for all 600+ tests |
| spdlog | latest via FetchContent | Structured logging | Already used throughout |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| std::unordered_set | C++20 stdlib | Subscription namespace tracking | Per D-08 |
| std::array<uint8_t, 32> | C++20 stdlib | Namespace ID storage | Matches PeerManager pattern |
| std::random_device / mt19937 | C++20 stdlib | Full jitter randomization | For backoff jitter per D-03 |

No new dependencies. All tools already in the project.

## Architecture Patterns

### Modification Targets
```
relay/
  core/
    relay_session.h       # Add state enum, subscription set, reconnect members
    relay_session.cpp     # State machine, reconnect loop, subscription intercept/filter/replay
  config/
    relay_config.h        # Optional: reconnect config params (max_attempts, base_delay, cap_delay)
    relay_config.cpp      # Optional: parse reconnect params from JSON
db/
  tests/
    relay/
      test_relay_session.cpp  # NEW: subscription tracking + namespace filtering unit tests
```

### Pattern 1: Three-State Lifecycle on RelaySession
**What:** Enum class SessionState { ACTIVE, RECONNECTING, DEAD } as a member of RelaySession. State transitions drive message handling behavior.
**When to use:** All message routing decisions check state before forwarding.
**Example:**
```cpp
enum class SessionState { ACTIVE, RECONNECTING, DEAD };

// In handle_client_message:
if (state_ == SessionState::RECONNECTING || state_ == SessionState::DEAD) {
    // D-04: silently drop client->node messages
    return;
}
// But D-05: always track Subscribe/Unsubscribe locally first
```

### Pattern 2: Coroutine Reconnection Loop
**What:** When node UDS closes, instead of teardown, spawn a reconnection coroutine that loops with backoff.
**When to use:** In handle_node_close, transition to RECONNECTING and co_spawn the reconnect loop.
**Example:**
```cpp
void RelaySession::handle_node_close(Connection::Ptr, bool) {
    if (state_ == SessionState::DEAD || stopped_) return;
    state_ = SessionState::RECONNECTING;
    node_conn_.reset();  // Release old connection
    asio::co_spawn(ioc_, reconnect_loop(), asio::detached);
}

asio::awaitable<void> RelaySession::reconnect_loop() {
    auto self = shared_from_this();
    for (uint32_t attempt = 0; attempt < max_reconnect_attempts_; ++attempt) {
        auto delay = jittered_backoff(attempt);
        asio::steady_timer timer(ioc_);
        timer.expires_after(delay);
        co_await timer.async_wait(use_nothrow);
        if (stopped_) co_return;

        // Try new UDS connection
        asio::local::stream_protocol::socket uds_socket(ioc_);
        auto [ec] = co_await uds_socket.async_connect(
            asio::local::stream_protocol::endpoint(uds_path_), use_nothrow);
        if (ec) continue;

        node_conn_ = Connection::create_uds_outbound(std::move(uds_socket), identity_);
        // Wire up on_ready for replay, on_close for re-enter reconnect
        // ...
        asio::co_spawn(ioc_, node_conn_->run(), asio::detached);
        co_return;  // on_ready will transition to ACTIVE after replay
    }
    // Exhausted attempts -> DEAD
    state_ = SessionState::DEAD;
    teardown("reconnection failed after max attempts");
}
```

### Pattern 3: Subscription Interception Before Forwarding
**What:** In handle_client_message, before forwarding Subscribe/Unsubscribe to node, parse the namespace list and update local subscription set.
**When to use:** Every Subscribe (type 19) and Unsubscribe (type 20) message.
**Example:**
```cpp
// In handle_client_message, after is_client_allowed check:
if (type == TransportMsgType_Subscribe) {
    auto namespaces = decode_namespace_list(payload);
    for (const auto& ns : namespaces) {
        if (subscribed_namespaces_.size() >= MAX_SUBSCRIPTIONS) break; // D-08: 256 cap
        subscribed_namespaces_.insert(ns);
    }
}
if (type == TransportMsgType_Unsubscribe) {
    auto namespaces = decode_namespace_list(payload);
    for (const auto& ns : namespaces) {
        subscribed_namespaces_.erase(ns);
    }
}
// Then forward to node (if ACTIVE)
```

### Pattern 4: Notification Filtering (Node-to-Client)
**What:** In handle_node_message, check Notification payloads against the subscription set before forwarding.
**When to use:** Every Notification (type 21) from node.
**Example:**
```cpp
// In handle_node_message:
if (type == TransportMsgType_Notification) {
    if (payload.size() < 32) return;  // Malformed, drop
    std::array<uint8_t, 32> ns_id;
    std::memcpy(ns_id.data(), payload.data(), 32);
    if (subscribed_namespaces_.find(ns_id) == subscribed_namespaces_.end()) {
        return;  // D-10: not subscribed, drop silently
    }
}
// Forward to client
```

### Pattern 5: Subscription Replay After Reconnect
**What:** In the on_ready callback of the new UDS connection, send a single Subscribe with all tracked namespaces before resuming normal forwarding.
**When to use:** After every successful UDS reconnection.
**Example:**
```cpp
// In on_ready callback for reconnected node_conn_:
replay_pending_ = true;
// Build subscribe payload from subscribed_namespaces_
std::vector<std::array<uint8_t, 32>> ns_list(
    subscribed_namespaces_.begin(), subscribed_namespaces_.end());
auto replay_payload = encode_namespace_list(ns_list);
co_await node_conn_->send_message(TransportMsgType_Subscribe, replay_payload);
replay_pending_ = false;
state_ = SessionState::ACTIVE;
attempt_count_ = 0;  // D-07: reset on success
```

### Anti-Patterns to Avoid
- **Reusing failed UDS sockets:** D-02 requires new socket per attempt. AEAD nonce state, send queues, and Connection internals are not resettable. Always create_uds_outbound with a fresh socket.
- **Queueing client messages during RECONNECTING:** D-04 explicitly forbids this. Unbounded queues risk memory exhaustion. Client SDK has request_id timeouts and will retry.
- **Modifying Connection internals for reconnect:** Connection is single-use by design (nonce counters, drain coroutine). Create a new Connection::Ptr for each reconnection.
- **Shared UDS across sessions:** Each RelaySession has its own UDS. The node sees each as a separate peer with independent subscription state.
- **Forgetting to re-wire on_close after reconnect:** The new node_conn_ needs its own on_close handler to re-enter the reconnect loop if the node drops again.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Backoff timing | Manual sleep loops | asio::steady_timer with co_await | Integrates with io_context, cancellable on shutdown |
| Random jitter | Custom PRNG | std::random_device + std::mt19937 + uniform_int_distribution | Thread-safe seed, correct distribution |
| Namespace list encoding | Custom wire format | PeerManager::encode_namespace_list / decode_namespace_list (or extracted copy) | Already validated, matches node's decoder |
| UDS connection creation | Raw socket + handshake | Connection::create_uds_outbound | Handles TrustedHello, AEAD setup, message loop |

**Key insight:** The relay's encode/decode for namespace lists MUST be byte-identical to PeerManager's format. The functions are static on PeerManager and the relay already links chromatindb_lib, so they can be called directly. However, since the relay conceptually should not depend on PeerManager's header, an alternative is to extract the trivial 20 lines of encode/decode into a shared header (e.g., `db/wire/namespace_encoding.h`). Both approaches work -- the static method call is simpler but creates a conceptual coupling. Recommendation: call PeerManager's static methods directly (they ARE public, the relay already links the lib, and the format must stay in sync).

## Common Pitfalls

### Pitfall 1: Lifetime of shared_from_this During Reconnect Loop
**What goes wrong:** The reconnect coroutine captures `shared_from_this()` but the session might be removed from the sessions deque during reconnection (e.g., signal shutdown). The shared_ptr in the coroutine keeps the object alive but io_context might be stopped.
**Why it happens:** co_await suspends the coroutine while the timer waits. Between yields, external shutdown can close the acceptor and clear sessions.
**How to avoid:** Check `stopped_` after every co_await resume in the reconnect loop. If stopped_, co_return immediately without touching other members.
**Warning signs:** Crash on shutdown during reconnection, use-after-free in teardown.

### Pitfall 2: Node Closes Again Immediately After Reconnect
**What goes wrong:** If the node is restarting repeatedly, the relay might reconnect, start replay, then lose the connection again before replay completes. The on_close fires during replay, triggering another reconnect loop. Two reconnect loops run concurrently.
**Why it happens:** on_ready fires synchronously after TrustedHello, but the message loop runs in parallel. If node closes during replay_pending_, on_close fires.
**How to avoid:** Gate reconnect_loop entry on state: only enter if not already RECONNECTING. Use a `reconnecting_` bool or check `state_ == ACTIVE` before transitioning. The on_close handler for the new connection should check if we are already reconnecting.
**Warning signs:** Multiple reconnect coroutines running for same session, doubled log messages.

### Pitfall 3: Subscription Tracking During replay_pending_
**What goes wrong:** Client sends Subscribe while replay is in progress. If tracked locally but not forwarded (because replay_pending_ blocks forwarding), the next replay won't include it because replay already started with the old set.
**Why it happens:** D-05 says always track locally. D-14 says block forwarding during replay.
**How to avoid:** After replay_pending_ clears, check if subscribed_namespaces_ has changed since replay started. If so, send a follow-up Subscribe with the delta. Or, simpler: track locally (D-05), and the Subscribe message will be forwarded to node once replay_pending_ clears (it goes through normal forwarding path). Since the node accumulates subscriptions, duplicate Subscribe is harmless.
**Warning signs:** Client subscribes during reconnect but misses notifications for that namespace after reconnect completes.

### Pitfall 4: encode_namespace_list with Empty Set
**What goes wrong:** If the client has no active subscriptions at reconnect time, sending an empty Subscribe (count=0) is a no-op on the node side (it adds zero namespaces). This is correct behavior -- no replay needed if no subscriptions.
**Why it happens:** Non-issue, but worth noting: skip the replay send entirely if subscribed_namespaces_ is empty. Saves a round-trip.
**How to avoid:** Check `if (!subscribed_namespaces_.empty())` before encoding and sending the replay Subscribe.

### Pitfall 5: Hash Function for unordered_set with array<uint8_t, 32>
**What goes wrong:** `std::unordered_set<std::array<uint8_t, 32>>` does not compile with default std::hash because there is no specialization for std::array.
**Why it happens:** C++ stdlib does not provide std::hash for std::array (unlike std::string).
**How to avoid:** Provide a custom hasher. Since namespace IDs are already SHA3-256 hashes (uniformly distributed), using the first 8 bytes as a size_t hash is sufficient and efficient:
```cpp
struct NamespaceHash {
    size_t operator()(const std::array<uint8_t, 32>& ns) const {
        size_t h;
        std::memcpy(&h, ns.data(), sizeof(h));
        return h;
    }
};
using NamespaceSet = std::unordered_set<std::array<uint8_t, 32>, NamespaceHash>;
```
**Warning signs:** Compile error on `std::unordered_set<std::array<uint8_t, 32>>`.

### Pitfall 6: Jitter Randomness Source
**What goes wrong:** Using `std::random_device` on every backoff call may be slow on some platforms. Using a single `std::mt19937` per session is better.
**Why it happens:** `std::random_device` may block on Linux if entropy is low (unlikely on modern kernels but still a code smell).
**How to avoid:** Seed a `std::mt19937` once in the RelaySession constructor with `std::random_device`. Use `std::uniform_int_distribution` for each jittered delay.

### Pitfall 7: Client Message Forwarding Race After State Transition
**What goes wrong:** handle_client_message is called from the client connection's message loop. The state transition to ACTIVE happens in the node connection's on_ready callback. If both run on the same io_context strand (single-threaded), there's no race. But if handle_client_message checks state_ and then co_spawns a send, the send might execute after state changes.
**Why it happens:** Single io_context means no data races, but coroutine scheduling is non-deterministic.
**How to avoid:** The relay runs on a single io_context (relay_main.cpp line 158). All callbacks execute on the same thread. No mutex needed. State checks and message sends in the same callback frame are safe. The co_spawn for send_message is detached but runs on the same io_context.
**Warning signs:** None if single-threaded model is maintained.

## Code Examples

### Full Jittered Backoff (D-03)
```cpp
// Source: Phase 84 SDK pattern, adapted for C++
std::chrono::milliseconds RelaySession::jittered_backoff(uint32_t attempt) {
    // min(cap, base * 2^attempt) with full jitter [0, delay)
    auto base_ms = 1000u;
    auto cap_ms = 30000u;
    auto exp = std::min(cap_ms, base_ms * (1u << std::min(attempt, 14u)));
    std::uniform_int_distribution<uint32_t> dist(0, exp);
    return std::chrono::milliseconds(dist(rng_));
}
```

### Namespace Wire Format
```cpp
// Source: PeerManager::encode_namespace_list (db/peer/peer_manager.cpp:3340)
// Format: [uint16_be count][ns_id:32]...
// PeerManager::decode_namespace_list (db/peer/peer_manager.cpp:3354)
// Returns empty vector if payload < 2 bytes or size mismatch

// Notification payload format (db/peer/peer_manager.cpp:3369):
// [namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
// = 77 bytes total. namespace_id is at offset 0.
```

### on_ready Replay Pattern
```cpp
// Source: Existing on_ready pattern in relay_session.cpp:62
// After TrustedHello completes on reconnected UDS:
node_conn_->on_ready([self](Connection::Ptr) {
    if (!self->subscribed_namespaces_.empty()) {
        self->replay_pending_ = true;
        // Build replay payload
        std::vector<std::array<uint8_t, 32>> ns_list(
            self->subscribed_namespaces_.begin(),
            self->subscribed_namespaces_.end());
        auto payload = PeerManager::encode_namespace_list(ns_list);
        asio::co_spawn(self->ioc_, [self, payload = std::move(payload)]() -> asio::awaitable<void> {
            co_await self->node_conn_->send_message(
                TransportMsgType_Subscribe, payload);
            self->replay_pending_ = false;
            self->state_ = SessionState::ACTIVE;
            self->reconnect_attempts_ = 0;
            spdlog::info("session {} replayed {} subscriptions after UDS reconnect",
                self->client_pk_hex_, self->subscribed_namespaces_.size());
        }, asio::detached);
    } else {
        self->replay_pending_ = false;
        self->state_ = SessionState::ACTIVE;
        self->reconnect_attempts_ = 0;
    }
    // Re-wire message forwarding callbacks (same as initial start)
    self->wire_message_handlers();
});
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Teardown on node UDS loss (relay_session.cpp:148) | Three-state reconnection lifecycle | Phase 88 | Client TCP sessions survive node restarts |
| Unfiltered node-to-client forwarding (relay_session.cpp:123) | Subscription-aware Notification filtering | Phase 88 | Clients only receive notifications for subscribed namespaces |
| No subscription tracking in relay | Per-session subscription set with cap | Phase 88 | Enables filtering + replay |

## Open Questions

1. **Extract encode/decode to shared header vs call PeerManager statics?**
   - What we know: PeerManager::encode_namespace_list and decode_namespace_list are public static methods. The relay already links chromatindb_lib.
   - What's unclear: Whether the conceptual coupling is acceptable long-term.
   - Recommendation: Call PeerManager statics directly. The format is wire-protocol-level and MUST stay in sync. Extracting would create a copy that could diverge. Leave extraction for a future refactor if the PeerManager header becomes too heavy.

2. **Config params for reconnection?**
   - What we know: D-03 specifies 1s base, 30s cap. D-06 specifies 10 max attempts. These could be hardcoded constants.
   - What's unclear: Whether they should be in relay config JSON.
   - Recommendation: Hardcode as constexpr in relay_session.h. Config file support is YAGNI until someone needs to tune them. Add a comment noting they can be promoted to config.

3. **Subscribe during replay_pending_?**
   - What we know: D-05 says always track locally. D-14 blocks forwarding during replay.
   - What's unclear: Whether the forwarding block applies to the Subscribe message itself, or just to other messages.
   - Recommendation: Track locally always (D-05). Block ALL forwarding during replay (D-14) including new Subscribes. When replay_pending_ clears and ACTIVE resumes, subsequent client Subscribes flow normally. The node already has the replayed set. Any Subscribe that arrived during replay is tracked locally and will be included in the next replay if node drops again. The client SDK doesn't know about relay internals -- from its perspective, the Subscribe "worked" because the relay tracked it.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.x |
| Config file | db/CMakeLists.txt (lines 225-253) |
| Quick run command | `cd build && ctest -R "relay" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| FILT-03-a | Subscribe interception updates local namespace set | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| FILT-03-b | Unsubscribe removes from local set | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| FILT-03-c | Notification filtered by subscription set | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| FILT-03-d | 256-namespace cap enforced | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-01-a | State transitions ACTIVE->RECONNECTING->ACTIVE | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-01-b | State transitions RECONNECTING->DEAD after max attempts | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-01-c | Jittered backoff timing within bounds | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-01-d | Client messages dropped during RECONNECTING | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-02-a | Subscription replay after reconnect | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-02-b | Empty subscription set skips replay | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |
| RELAY-02-c | replay_pending_ blocks forwarding | unit | `cd build && ctest -R "test_relay_session" --output-on-failure` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "relay" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/relay/test_relay_session.cpp` -- NEW test file for subscription tracking, notification filtering, state machine, backoff bounds, replay logic
- [ ] Add `tests/relay/test_relay_session.cpp` to `db/CMakeLists.txt` test sources (line 242)

Note: Full reconnection lifecycle with actual UDS connections is an integration test concern. Unit tests can validate subscription set operations, notification filtering logic, backoff calculation, and state transitions using direct method calls or mock objects. The reconnect loop itself requires io_context + real sockets and is better tested via Docker integration tests (which are outside this phase's scope per project convention -- relay integration tests don't exist yet).

## Sources

### Primary (HIGH confidence)
- `relay/core/relay_session.h` / `.cpp` -- Current relay session implementation (read directly)
- `relay/core/message_filter.h` / `.cpp` -- Current blocklist filter (read directly)
- `relay/relay_main.cpp` -- Accept loop, session lifecycle, signal handling (read directly)
- `relay/config/relay_config.h` / `.cpp` -- Current config structure (read directly)
- `db/peer/peer_manager.cpp:3340-3390` -- encode_namespace_list, decode_namespace_list, encode_notification (read directly)
- `db/peer/peer_manager.h:166-182` -- Public static method signatures for wire encoding (read directly)
- `db/net/connection.h` -- Connection API, create_uds_outbound, on_ready, on_close, send_message (read directly)
- `db/wire/transport_generated.h` -- TransportMsgType enum values (Subscribe=19, Unsubscribe=20, Notification=21) (read directly)
- `db/tests/relay/test_message_filter.cpp` -- Existing relay test patterns (read directly)
- `88-CONTEXT.md` -- All locked decisions D-01 through D-14 (read directly)

### Secondary (MEDIUM confidence)
None needed -- all findings from direct code inspection.

### Tertiary (LOW confidence)
None.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing project tools
- Architecture: HIGH -- all code read directly, patterns derived from existing relay_session.cpp
- Pitfalls: HIGH -- derived from direct code analysis and C++ coroutine experience with this codebase

**Research date:** 2026-04-05
**Valid until:** 2026-05-05 (stable -- relay architecture is mature, no external dependency changes)
