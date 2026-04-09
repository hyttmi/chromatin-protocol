# Architecture Research: Relay v2 (WebSocket/JSON/TLS Gateway)

**Domain:** WebSocket gateway relay for decentralized PQ-secure database node
**Researched:** 2026-04-09
**Confidence:** HIGH (based on direct source analysis of existing codebase + established patterns)

## System Overview

```
                           Relay v2
                   +-----------------------+
  Clients          |                       |          Node (db/)
  (WSS+JSON)       |  +-----------------+ |          (FlatBuffers+UDS)
                   |  |                 | |
 [Client A] ------>|  | WS Session Mgr  | |
 [Client B] ------>|  | (per-client     | |    +--single UDS--+
 [Client C] ------>|  |  state, auth,   |------| TrustedHello |----> PeerManager
                   |  |  subscriptions) | |    | Connection   |      MessageDispatcher
                   |  |                 | |    +--------------+
                   |  +-----------------+ |
                   |  +-----------------+ |
                   |  | Request Router  | |
                   |  | (request_id     | |
                   |  |  mapping,       | |
                   |  |  demux)         | |
                   |  +-----------------+ |
                   |  +-----------------+ |
                   |  | JSON <-> FB     | |
                   |  | Translator      | |
                   |  +-----------------+ |
                   |  +-----------------+ |
                   |  | TLS Termination | |
                   |  | (OpenSSL/asio)  | |
                   |  +-----------------+ |
                   +-----------------------+
```

## Component Responsibilities

| Component | Responsibility | New vs Existing |
|-----------|---------------|-----------------|
| **WsSessionManager** | Client lifecycle: accept, TLS handshake, WS upgrade, ML-DSA-87 challenge-response auth, per-client send queue, close | NEW |
| **WsSession** | Single client state: subscriptions, pubkey, send queue, backpressure | NEW |
| **RequestRouter** | Maps outbound request_ids to (client_id, original_request_id) pairs, demuxes node responses | NEW |
| **JsonFbTranslator** | Bidirectional JSON <-> FlatBuffers for all 38 client-allowed message types | NEW |
| **MessageFilter** | Blocklist for peer-internal types (reuse existing logic) | MODIFIED (extract from relay/core) |
| **UdsMultiplexer** | Single UDS connection to node, sends/receives on behalf of all clients | NEW |
| **NotificationRouter** | Tracks per-client subscriptions, routes Notification messages from node to correct WebSocket sessions | NEW |
| **RelayConfig** | TLS cert/key paths, bind address, UDS path, limits | MODIFIED (add TLS fields, WebSocket config) |

## Recommended Project Structure

```
relay/
  relay_main.cpp              # Entry point, signal handling, accept loop
  config/
    relay_config.h             # Config struct (rewritten: +TLS, +WS settings)
    relay_config.cpp           # JSON config loader
  identity/
    relay_identity.h           # Relay ML-DSA-87 identity (KEEP as-is)
    relay_identity.cpp
  ws/
    ws_acceptor.h              # TLS+WS accept loop, upgrade HTTP->WS
    ws_acceptor.cpp
    ws_session.h               # Per-client session state + send queue
    ws_session.cpp
  core/
    request_router.h           # request_id mapping + demux
    request_router.cpp
    uds_multiplexer.h          # Single UDS conn to node, send/recv
    uds_multiplexer.cpp
    notification_router.h      # Subscription tracking + fan-out
    notification_router.cpp
    message_filter.h           # Blocklist (migrated from existing)
    message_filter.cpp
  translate/
    json_fb_translator.h       # JSON <-> FlatBuffers codec
    json_fb_translator.cpp
    type_registry.h            # Message type <-> JSON type string mapping
    type_registry.cpp
```

### Structure Rationale

- **ws/**: WebSocket-specific concerns (TLS, WS upgrade, session state). Isolated from core logic.
- **core/**: Protocol-level logic (routing, multiplexing, filtering). No knowledge of WebSocket framing.
- **translate/**: Pure codec layer. Takes JSON in, produces FlatBuffers out (and vice versa). No IO, no state, easily unit-tested.
- **config/**: Rewritten config with new TLS/WS fields. Same pattern as db/config/.
- **identity/**: Unchanged. Relay still has its own ML-DSA-87 identity for challenge-response auth.

## Architectural Patterns

### Pattern 1: Request-ID Multiplexing with Client Session Tagging

**What:** The relay maintains a single UDS connection to the node. Multiple clients share this connection. The relay assigns a relay-scoped request_id to each outbound request and maintains a map to route responses back to the originating client.

**Why this works:** The node's transport envelope has a `request_id: uint32` field that is client-assigned and node-echoed. The relay can rewrite request_ids to avoid collisions between clients, then reverse-map responses.

**Design:**

```cpp
// RequestRouter owns the mapping
struct PendingRequest {
    uint64_t client_id;         // Which WsSession owns this request
    uint32_t client_request_id; // Client's original request_id
    std::chrono::steady_clock::time_point created;  // For timeout cleanup
};

class RequestRouter {
    // Relay assigns from this counter (wraps at UINT32_MAX)
    uint32_t next_relay_rid_ = 1;  // 0 reserved for server-initiated

    // relay_request_id -> pending request info
    std::unordered_map<uint32_t, PendingRequest> pending_;

    // Timeout for stale entries (client disconnected without response)
    static constexpr auto REQUEST_TIMEOUT = std::chrono::seconds(60);

public:
    // Client sends request: allocate relay-scoped request_id
    uint32_t register_request(uint64_t client_id, uint32_t client_rid);

    // Node sends response: look up and remove mapping
    std::optional<PendingRequest> resolve_response(uint32_t relay_rid);

    // Periodic cleanup of timed-out entries
    void purge_stale();

    // Client disconnected: remove all pending entries for that client
    void remove_client(uint64_t client_id);
};
```

**Trade-offs:**
- Pro: Node is completely unchanged. Single UDS = single TrustedHello, minimal resource usage.
- Pro: request_id is uint32, giving 4 billion concurrent requests across all clients -- more than enough.
- Con: Relay must track per-request state. Memory bounded by active request count.
- Con: Server-initiated messages (Notification, request_id=0) need separate routing (see NotificationRouter).

**Critical detail -- fire-and-forget messages:** Subscribe, Unsubscribe, Ping, and Goodbye are fire-and-forget (no response with matching request_id). These do NOT go through the RequestRouter. Subscribe/Unsubscribe are intercepted by the NotificationRouter. Ping/Pong is handled by the UdsMultiplexer directly.

### Pattern 2: Relay-Side Subscription Tracking with Aggregate Node Subscription

**What:** The relay tracks per-client subscription sets locally (same as old relay). It sends a single aggregated Subscribe to the node on the multiplexed UDS. The node sends Notifications on that UDS, and the relay fans them out to subscribed WebSocket clients.

**Why:** With a single UDS connection, the node sees one "client" (the relay). The relay must manage the union of all client subscriptions and route notifications.

**Design:**

```cpp
class NotificationRouter {
    // Per-client subscription sets
    std::unordered_map<uint64_t, NamespaceSet> client_subs_;

    // Reference count per namespace (how many clients subscribe)
    std::unordered_map<std::array<uint8_t, 32>, uint32_t, NamespaceHash> ns_refcount_;

    // Reference to UDS multiplexer for sending Subscribe/Unsubscribe
    UdsMultiplexer& uds_;

public:
    // Client subscribes: add to client set, maybe update aggregate
    void client_subscribe(uint64_t client_id,
                          const std::vector<std::array<uint8_t, 32>>& namespaces);

    // Client unsubscribes: remove from client set, maybe update aggregate
    void client_unsubscribe(uint64_t client_id,
                            const std::vector<std::array<uint8_t, 32>>& namespaces);

    // Client disconnected: remove all subscriptions, update aggregate
    void client_removed(uint64_t client_id);

    // Node sends Notification: fan out to subscribed clients
    void route_notification(const std::vector<uint8_t>& payload);
};
```

**Aggregate subscription logic:**
- When a client subscribes to namespace N: increment ns_refcount_[N]. If it was 0 -> 1, send Subscribe({N}) to node.
- When a client unsubscribes from namespace N: decrement ns_refcount_[N]. If it hits 0, send Unsubscribe({N}) to node.
- When a client disconnects: for each namespace in client's set, decrement refcount. Send Unsubscribe for any that hit 0.

**Trade-offs:**
- Pro: Node sends Notification once per blob, relay fans out. Efficient.
- Pro: Reference-counted set is O(1) per subscribe/unsubscribe operation.
- Con: Aggregate management adds complexity vs per-client UDS.
- Note: Per-client subscription cap (256) inherited from old relay. Aggregate cap should be higher (sum of all clients, but bounded by available namespaces).

### Pattern 3: Per-Client Bounded Send Queue with WebSocket Backpressure

**What:** Each WebSocket client session has a bounded send queue. When the queue fills, the relay stops forwarding responses/notifications for that client until drain. If the client is too slow, disconnect.

**Design:**

```cpp
class WsSession {
    static constexpr size_t MAX_SEND_QUEUE = 1024;

    std::deque<std::string> send_queue_;  // JSON strings ready to send
    bool drain_running_ = false;

    // Enqueue a JSON message for this client
    bool enqueue(std::string json_msg);  // returns false if full -> disconnect

    // Drain coroutine: pops from queue, writes to WebSocket
    asio::awaitable<void> drain_send_queue();
};
```

**Trade-offs:**
- Pro: One slow client cannot block other clients or the node UDS.
- Pro: Same pattern as Connection's existing send queue (proven in Phase 79).
- Con: Need per-client memory accounting. 1024 messages * ~1KB avg = ~1MB per client max.

### Pattern 4: JSON <-> FlatBuffers Translation Layer

**What:** Pure stateless codec that converts between JSON text messages and FlatBuffers binary payloads. One function per message type pair in each direction.

**Design:**

```cpp
namespace translate {

// JSON -> FlatBuffers binary payload (for client->node messages)
struct TranslatedMessage {
    wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};
std::optional<TranslatedMessage> json_to_fb(const nlohmann::json& msg);

// FlatBuffers binary payload -> JSON (for node->client messages)
std::optional<nlohmann::json> fb_to_json(wire::TransportMsgType type,
                                          std::span<const uint8_t> payload);
}
```

**JSON message format convention:**

```json
{
    "type": "read",
    "request_id": 42,
    "namespace": "a1b2c3...",
    "hash": "d4e5f6..."
}
```

- `type`: string name of the operation (not the enum integer). Human-readable, forward-compatible.
- `request_id`: client-chosen uint32, echoed on responses (same semantics as binary protocol).
- Binary fields (namespace, hash, pubkey): hex-encoded strings. Debuggable, consistent with node logging, negligible overhead for 32-byte values.
- Large binary fields (blob data, signatures): base64-encoded. Blob data can be up to 100 MiB; hex would double the size, base64 adds ~33%. ML-DSA-87 signatures are 4627 bytes -- base64 saves ~3KB per blob write vs hex.

**Why hex for most fields, base64 for large payloads:**
- Namespace (32 bytes) -> 64 hex chars. Readable, debuggable, matches node's `to_hex()` output.
- Blob data (up to 100 MiB) -> base64 avoids ~50% bloat vs hex.
- Signatures (4627 bytes) -> base64 saves ~3KB per blob.

**Trade-offs:**
- Pro: Stateless, pure functions. Trivially unit-testable with golden JSON fixtures.
- Pro: nlohmann/json is already a project dependency. Zero new deps for JSON handling.
- Con: Must maintain parity with all 38 client-allowed message types. Each type needs encode/decode.
- Con: Translation adds CPU cost and latency. Acceptable for relay use case.

## Data Flow

### Client Request Flow (e.g., ReadRequest)

```
Client (WebSocket)          Relay v2                        Node (UDS)
       |                       |                               |
  1. JSON text msg ---------> |                               |
       |              2. Parse JSON                            |
       |              3. json_to_fb() -> FlatBuffers payload   |
       |              4. RequestRouter: allocate relay_rid,    |
       |                 store mapping (relay_rid -> client,   |
       |                 client_rid)                           |
       |              5. UdsMultiplexer: send_message(         |
       |                 ReadRequest, payload, relay_rid) ---> |
       |                       |                        6. Node processes,
       |                       |                           sends ReadResponse
       |                       |                           with same relay_rid
       |              7. UdsMultiplexer: recv ReadResponse <-- |
       |              8. RequestRouter: resolve relay_rid      |
       |                 -> (client_id, client_rid)            |
       |              9. fb_to_json() -> JSON response         |
       |             10. Set response request_id =             |
       |                 client_rid (original)                 |
       |             11. WsSession: enqueue JSON,              |
       |                 drain to WebSocket                    |
  12. JSON response <-------- |                               |
```

### Notification Flow (Server-Initiated)

```
Node (blob ingested)        Relay v2                     Clients
       |                       |                            |
  1. Notification(type=21)     |                            |
     on UDS, request_id=0 --> |                            |
       |              2. NotificationRouter:                |
       |                 extract namespace from first       |
       |                 32 bytes of payload                |
       |              3. Look up which clients              |
       |                 subscribe to this namespace        |
       |              4. For each subscribed client:        |
       |                 fb_to_json() -> JSON notification  |
       |              5. WsSession::enqueue() for each  --> | [Client A]
       |                                                --> | [Client B]
       |                                                    |
       |                 (Clients NOT subscribed: skipped)   |
```

### WebSocket Session Lifecycle

```
Client TCP Connect
    |
    v
TLS Handshake (asio::ssl)
    |
    v
HTTP Upgrade -> WebSocket
    |
    v
ML-DSA-87 Challenge-Response Auth
    |  Relay sends: {"type":"challenge","nonce":"<32-byte hex>"}
    |  Client sends: {"type":"challenge_response","pubkey":"<hex>","signature":"<hex>"}
    |  Relay verifies signature, sends: {"type":"auth_ok"} or disconnects
    |
    v
Authenticated Session (message loop)
    |  Client sends JSON requests -> translate -> node UDS
    |  Node sends responses -> translate -> client WebSocket
    |  Node sends notifications -> filter -> fan-out to client
    |
    v
Close (client disconnect OR backpressure timeout OR relay shutdown)
    |  RequestRouter: remove_client(client_id)
    |  NotificationRouter: client_removed(client_id)
    |  WsSession: close WebSocket
```

### UDS Multiplexer Lifecycle

```
Relay Start
    |
    v
Connect UDS to Node
    |
    v
TrustedHello Handshake (relay identity)
    |
    v
Message Loop (shared by all clients)
    |  Outbound: requests from all clients (rewritten request_ids)
    |  Inbound: responses demuxed by request_id, notifications fan-out
    |
    v (UDS lost?)
    |
Reconnect with Backoff (same pattern as old relay)
    |  Re-subscribe aggregate namespaces
    |  Pending requests: error/timeout to affected clients
```

## Integration Points

### Existing Components (Node Side -- NO Changes)

| Component | Integration | Notes |
|-----------|-------------|-------|
| `Connection` (UDS) | Relay connects via `create_uds_outbound` | Single connection, TrustedHello. Node sees relay as one UDS "client" with relay's identity. |
| `MessageDispatcher` | Processes requests from relay's UDS | request_id echoed. Subscribe/Unsubscribe tracked in PeerInfo for the relay's single connection. |
| `BlobPushManager` | Sends Notification(type=21) to relay's UDS connection | Relay's PeerInfo has `subscribed_namespaces` set to aggregate of all client subs. |
| `TransportCodec` | Shared encoder/decoder for FlatBuffers envelope | Relay links against db/ library for codec, wire types, hex utilities. |

### New Components (Relay Side)

| Boundary | Communication | Notes |
|----------|---------------|-------|
| WsAcceptor -> WsSession | WsSession created per accepted+upgraded connection | WsAcceptor owns TLS context, does HTTP upgrade. Session takes ownership of WebSocket stream. |
| WsSession -> RequestRouter | Session calls `register_request()` before forwarding | Session passes its client_id + original request_id. Gets back relay_rid. |
| WsSession -> NotificationRouter | Session calls `client_subscribe/unsubscribe` | NotificationRouter updates aggregate, sends to node via UdsMultiplexer. |
| RequestRouter -> WsSession | Router resolves (client_id, client_rid) on response | Dispatch uses client_id to look up WsSession in session map. |
| UdsMultiplexer -> RequestRouter | On inbound message with request_id > 0: resolve pending | On request_id == 0 (Notification/Pong): route to NotificationRouter or handle internally. |
| JsonFbTranslator | Called by WsSession (encode) and UdsMultiplexer dispatch (decode) | Pure functions, no state. Link-time dependency on db/wire/ headers. |

### Critical Integration Detail: Source Exclusion

**Problem:** The node's `BlobPushManager::on_blob_ingested()` uses `peer->connection == source` for source exclusion. With a single multiplexed UDS, ALL client writes arrive from the same Connection::Ptr. The node cannot distinguish which client wrote the blob.

**Consequence:** When Client A writes a blob, the node sends the Notification on the UDS (because the UDS connection has aggregate subscriptions that include A's namespace). Client A receives a notification for its own write.

**Solution options:**

1. **Don't do source exclusion.** Let clients handle duplicate notifications. WebSocket clients are typically idempotent consumers. Document: "clients may receive notifications for their own writes." Simplest approach.

2. **Relay-side write tracking.** The relay records which client_id submitted each Data/Delete. When a Notification arrives, extract namespace+hash from the 77-byte payload, check if any client recently wrote that exact blob, suppress for that client only. Requires the relay to parse notification payloads (namespace:32 + hash:32 at fixed offsets -- straightforward).

**Recommendation:** Start with option 1 (no source exclusion). Add option 2 only if clients demonstrate a real need. The old Python SDK handled duplicate notifications fine.

## Scaling Considerations

| Scale | Architecture Adjustments |
|-------|--------------------------|
| 1-100 clients | Single io_context thread is fine. One UDS connection handles all traffic. |
| 100-1000 clients | Consider io_context thread pool (2-4 threads). JSON parsing becomes CPU bottleneck before UDS. Notification fan-out for popular namespaces is the hot path. |
| 1000+ clients | Multiple relay instances behind a load balancer. Each relay has its own UDS to the node. Node already handles multiple UDS connections (UdsAcceptor tracks a vector). |

### Scaling Priorities

1. **First bottleneck: JSON serialization CPU.** Each message requires JSON parse/serialize + FlatBuffers encode/decode. nlohmann/json is fast enough for typical loads but profile before optimizing. For large blob payloads, base64 encode/decode dominates.
2. **Second bottleneck: Notification fan-out.** One blob write -> N notifications to N subscribed clients. At 1000 subscribed clients on one namespace, one write generates 1000 WebSocket sends. Per-client send queue absorbs bursts. Slow clients get disconnected.
3. **Third bottleneck: UDS throughput.** Single UDS connection has kernel-level throughput limits (~GiB/s on Linux). Unlikely to hit before CPU or WebSocket limits.

## Anti-Patterns

### Anti-Pattern 1: Per-Client UDS Connection (Old Relay Design)

**What people do:** Open a new UDS connection per WebSocket client (the old relay's approach).
**Why it's wrong for v2:** Node creates a PeerInfo entry per UDS connection. At 1000 clients, that's 1000 PeerInfo entries polluting the peer deque. The node's keepalive loop, BlobNotify fan-out, and PEX all iterate `peers_` -- O(N) with client count. Also wastes file descriptors and UDS connection setup cost.
**Do this instead:** Single multiplexed UDS with request_id routing.

### Anti-Pattern 2: Stateful Translation Layer

**What people do:** Make the JSON<->FlatBuffers translator aware of session state or connection context.
**Why it's wrong:** Tight coupling. Translation should be pure byte-level conversion. Session context (which client, subscriptions, auth state) belongs in the session/router layer.
**Do this instead:** Translator takes JSON -> bytes and bytes -> JSON. Router handles all session logic.

### Anti-Pattern 3: Forwarding Binary FlatBuffers over WebSocket

**What people do:** Send raw FlatBuffers bytes over WebSocket binary frames, requiring clients to have a FlatBuffers library.
**Why it's wrong:** Defeats the purpose of Relay v2. The whole point is that clients only need WebSocket + JSON + liboqs. FlatBuffers is an internal wire format.
**Do this instead:** Always translate to/from JSON at the relay boundary.

### Anti-Pattern 4: Shared request_id Space Without Relay Rewriting

**What people do:** Pass client request_ids directly to the node without rewriting.
**Why it's wrong:** Two clients might use the same request_id (e.g., both start at 1). Responses would be ambiguous -- which client does request_id=1 belong to?
**Do this instead:** Relay allocates unique relay-scoped request_ids, maps them to (client_id, original_request_id).

### Anti-Pattern 5: Blocking JSON Parse on IO Thread for Large Messages

**What people do:** Parse JSON synchronously in the WebSocket read handler on the io_context thread.
**Why it's wrong for large messages:** Blob data payloads can be up to 100 MiB. JSON parsing of a ~133 MiB base64-encoded blob blocks the event loop.
**Do this instead:** For small messages (<64 KB), inline parse is fine. For large messages (Data with blob payload), post the parse to the thread pool. Or set a relay-side message size limit and document it.

## WebSocket + TLS Library Decision

**Decision: Hand-rolled WebSocket framing on top of existing standalone Asio + OpenSSL.**

The project uses standalone Asio (not Boost.Asio). Options evaluated:

| Option | Verdict | Rationale |
|--------|---------|-----------|
| **Boost.Beast** (official) | REJECTED | Requires full Boost dependency (~200 MB source). Project explicitly uses standalone Asio. Adding Boost just for WebSocket framing is disproportionate. |
| **beast-asio-standalone** (fork) | REJECTED | Not actively maintained, test coverage uncertain, maintenance risk. Last meaningful update predates current Asio versions. |
| **websocketpp** | REJECTED | C++11-era design, no coroutine support, unmaintained since 2019. |
| **Hand-rolled WS framing** | CHOSEN | WebSocket framing (RFC 6455) is ~200 lines of code: mask, opcode, length. The project has proven ability to implement protocol framing (FlatBuffers transport, AEAD framing, length-prefixed messages). TLS handled by `asio::ssl::stream`. C++20 coroutines work natively with standalone Asio. |

**New dependency: OpenSSL** (system library, not FetchContent). Add `find_package(OpenSSL REQUIRED)` to CMake. liboqs already links against an OpenSSL-compatible API, so this is not truly new -- just explicitly required now.

**Fallback:** If hand-rolled WS is rejected, use beast-asio-standalone with pinned commit hash and acceptance of maintenance risk.

## TLS Integration

```cpp
// TLS context setup (in WsAcceptor)
asio::ssl::context tls_ctx(asio::ssl::context::tls_server);
tls_ctx.use_certificate_chain_file(config.cert_path);
tls_ctx.use_private_key_file(config.key_path, asio::ssl::context::pem);
// TLS 1.3 only (modern, ephemeral keys)
tls_ctx.set_options(asio::ssl::context::no_tlsv1 |
                    asio::ssl::context::no_tlsv1_1 |
                    asio::ssl::context::no_tlsv1_2);
```

Config additions:
```json
{
    "cert_path": "/etc/chromatindb/relay.crt",
    "key_path": "/etc/chromatindb/relay.key",
    "bind_address": "0.0.0.0",
    "bind_port": 4443,
    "uds_path": "/run/chromatindb/node.sock"
}
```

## ML-DSA-87 Challenge-Response Auth over WebSocket

After WebSocket upgrade completes:

```
Relay                           Client
  |                                |
  |  {"type":"challenge",          |
  |   "nonce":"<32-byte hex>"}  -> |
  |                                |  Client signs nonce with ML-DSA-87
  |                                |  private key
  |  <- {"type":"challenge_response",
  |      "pubkey":"<ML-DSA-87 pubkey hex>",
  |      "signature":"<sig hex>"}  |
  |                                |
  | Verify: ML-DSA-87.verify(     |
  |   pubkey, nonce, signature)    |
  |                                |
  |  {"type":"auth_ok",            |
  |   "namespace":"<sha3(pk) hex>"}|
  |     -> (or disconnect)         |
```

- Nonce: 32 random bytes, generated per connection. Prevents replay.
- Client proves ownership of ML-DSA-87 private key without revealing it.
- Relay computes SHA3-256(pubkey) = client's namespace for display/logging.
- No PQ key exchange needed (TLS handles transport encryption).
- Relay only needs liboqs for ML-DSA-87 verification (already a project dependency).

## Key Architectural Decisions

| Decision | Rationale |
|----------|-----------|
| Single multiplexed UDS | Avoids O(N) PeerInfo pollution in node. One connection, one TrustedHello. |
| Relay rewrites request_ids | Prevents collision between clients sharing the UDS. |
| Hex encoding for hash/namespace/pubkey fields | Debuggable, consistent with node logging. 64 chars for 32 bytes. |
| Base64 encoding for blob data + signatures | Large payloads need compact encoding. base64 adds ~33% vs hex's ~100%. |
| Relay-side subscription tracking with refcount | Node sees one connection with one subscription set. Relay manages per-client fan-out. |
| No source exclusion initially | Simpler. Clients handle their own write echoes. Add later if needed. |
| Hand-rolled WebSocket framing | Avoids Boost dependency. ~200 lines. Proven project capability for protocol framing. |
| OpenSSL for TLS | System package. liboqs already links against OpenSSL-compatible API. |
| JSON message type as string | `"type":"read"` not `"type":31`. Human-readable, forward-compatible. |
| TLS 1.3 only | Modern, ephemeral keys. Carrying PQ-encrypted payloads anyway. |

## Build Order (Phase Dependencies)

Recommended implementation order based on dependency analysis:

1. **Delete old relay/ and sdk/python/** -- Clean break. Remove all old code before building new.

2. **RelayConfig + CMake setup** -- New config struct with TLS fields. CMake target for relay v2 binary. find_package(OpenSSL). Depends on: nothing (standalone).

3. **JsonFbTranslator** -- Pure codec layer. Can be built and fully unit-tested with zero IO dependencies. Needs: db/wire/ headers (already exist). Start with 5-6 core types (Data, ReadRequest/Response, ListRequest/Response, Subscribe/Unsubscribe, Notification), expand to all 38 later.

4. **UdsMultiplexer** -- Single UDS connection to node with TrustedHello. Send/receive loop. Reconnect with backoff. Needs: db/net/Connection (already exists, no changes). Can test against running node.

5. **RequestRouter** -- request_id allocation, mapping, demux, stale entry cleanup. Needs: UdsMultiplexer (to send rewritten messages). Pure logic, heavily unit-testable.

6. **NotificationRouter** -- Subscription tracking with reference-counted aggregate, fan-out dispatch. Needs: UdsMultiplexer (to send Subscribe/Unsubscribe to node). Pure logic + UDS interaction.

7. **WsSession + WsAcceptor** -- TLS + WebSocket accept, upgrade, per-client session with send queue. Needs: All core components above (to forward messages).

8. **ML-DSA-87 Challenge-Response Auth** -- Auth handshake over established WebSocket. Needs: WsSession (to send/receive JSON auth messages), relay identity (already exists).

9. **Integration tests** -- Docker-based end-to-end: client -> WSS relay -> UDS -> node. Needs: all components assembled.

**Key dependency chain:** Translator (independent) -> UdsMultiplexer -> RequestRouter -> NotificationRouter -> WsSession (depends on all core) -> Auth -> Integration tests.

## Sources

- Direct source analysis of existing codebase:
  - `db/net/connection.h` -- Connection class, request_id in MessageCallback
  - `db/net/protocol.h` -- TransportCodec, DecodedMessage with request_id
  - `db/schemas/transport.fbs` -- TransportMessage FlatBuffers schema (request_id: uint32)
  - `db/peer/message_dispatcher.cpp` -- request_id echo in all 20+ handlers
  - `db/peer/blob_push_manager.cpp` -- Notification fan-out, source exclusion via `peer->connection == source`
  - `db/peer/peer_types.h` -- PeerInfo with subscribed_namespaces, announced_namespaces
  - `db/net/uds_acceptor.h` -- UdsAcceptor creating Connection objects per UDS client
  - `relay/core/relay_session.h/.cpp` -- Old relay session model (per-client UDS, subscription tracking, reconnect)
  - `relay/relay_main.cpp` -- Old relay accept loop (TCP + PQ handshake)
  - `relay/core/message_filter.h` -- Blocklist approach (21 blocked, rest passes)
  - `db/PROTOCOL.md` -- request_id semantics (client-assigned, node-echoed, per-connection scope)
- Project memory: relay v2 direction doc, stack decisions, milestone history
- [Boost Beast repository](https://github.com/boostorg/beast) -- WebSocket library evaluation
- [beast-asio-standalone](https://github.com/vimpunk/beast-asio-standalone) -- Standalone Asio fork evaluation
- [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455) -- WebSocket protocol framing specification

---
*Architecture research for: Relay v2 WebSocket/JSON/TLS Gateway*
*Researched: 2026-04-09*
