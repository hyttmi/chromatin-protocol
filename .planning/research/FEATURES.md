# Feature Landscape: Relay v2 (WebSocket/JSON/TLS Gateway)

**Domain:** WebSocket relay gateway translating JSON clients to FlatBuffers database node
**Researched:** 2026-04-09
**Confidence:** HIGH (well-understood domain, existing node protocol frozen, clear architectural direction)

## Context: What Already Exists

The Relay v2 builds on top of a frozen, production-grade C++20 database node. Understanding what the node already provides defines the relay's scope.

| Existing Capability | Where | Relay Implication |
|---------------------|-------|-------------------|
| 63 wire message types (FlatBuffers) | `db/wire/transport_generated.h` | 38 client-allowed types need JSON translation |
| Blocklist message filter (21 blocked types) | `relay/core/message_filter.h` | Reuse blocklist approach -- new node types pass through |
| UDS local access (TrustedHello, no PQ) | `db/net/connection.h` | Relay connects to node via UDS with lightweight handshake |
| request_id correlation (uint32) | Transport envelope | Map to JSON `id` field for request-response pairing |
| Subscribe/Unsubscribe (namespace-scoped) | Types 19/20, 32-byte namespace IDs | Relay tracks per-client subscriptions for notification routing |
| Notification fan-out (77-byte payload) | Type 21 | Relay must route notifications only to subscribed clients |
| Per-connection send queue with drain | `db/net/connection.h` | Node already handles UDS send buffering |
| ML-DSA-87 identity (2592-byte pubkeys) | `db/identity/identity.h` | Challenge-response auth uses relay's own identity |

**Key architectural shift:** Old relay was per-client UDS (one UDS connection per client session). New relay uses a single multiplexed UDS connection to the node. This fundamentally changes how request routing works -- the relay must correlate responses from the node back to the correct WebSocket client.

## Table Stakes

Features users expect. Missing any of these means the relay is non-functional or insecure.

| Feature | Why Expected | Complexity | Dependencies |
|---------|--------------|------------|--------------|
| **WebSocket server (WSS)** | The entire point of the relay. Clients connect via standard WebSocket over TLS. Every browser, every language has a WebSocket client library. | LOW | Asio Beast WebSocket + asio::ssl |
| **TLS termination** | WSS requires TLS. Config takes cert_path + key_path. No plaintext WebSocket option -- always TLS. | LOW | OpenSSL (via asio::ssl). Unavoidable for TLS. |
| **JSON message format** | Clients send/receive JSON instead of binary FlatBuffers. Every message has `type`, `id` (request_id), and type-specific fields. Hex-encoded byte arrays for hashes/pubkeys/signatures. | MEDIUM | nlohmann/json (already in stack). 38 message types to define JSON schemas for. |
| **JSON-to-FlatBuffers translation** | Relay converts inbound JSON to binary payloads the node understands, and outbound binary payloads to JSON for the client. This is the relay's core function. | HIGH | Must handle all 38 client-allowed message types. Each type has a unique wire format (documented in PROTOCOL.md). Most complex part of the relay. |
| **ML-DSA-87 challenge-response auth** | Client proves identity by signing a random challenge with their ML-DSA-87 private key. Relay verifies signature. No PQ key exchange needed (TLS handles transport). | MEDIUM | liboqs for ML-DSA-87 verification. Challenge: relay sends 32 random bytes. Client returns pubkey + signature. Relay verifies. |
| **Message type filtering** | Blocklist approach: block 21 peer-internal types (handshake, sync, PEX, internal signals), allow everything else. Same as old relay. Future node types pass through without relay changes. | LOW | Reuse existing blocklist logic from `relay/core/message_filter.h` |
| **UDS connection to node** | Single UDS connection to the local chromatindb node. TrustedHello handshake (no PQ crypto to own node). | LOW | Existing UDS + TrustedHello code in `db/net/connection.h` |
| **Request-response correlation** | Map client JSON `id` field to node's uint32 `request_id` in transport envelope. Route responses back to the correct WebSocket client. | MEDIUM | With multiplexed UDS: relay must maintain a mapping of {request_id -> client_session}. Allocate unique request_ids across all clients. |
| **Subscription tracking** | Track which WebSocket clients are subscribed to which namespaces. When a Notification arrives from the node, route it only to subscribed clients. | MEDIUM | Per-client NamespaceSet (like old relay's `subscribed_namespaces_`). Intercept Subscribe/Unsubscribe JSON messages before forwarding to node. 256-namespace cap per client. |
| **Notification routing** | When node sends Notification (type 21), extract namespace_id from first 32 bytes, look up which clients are subscribed, translate to JSON, and send to each. | MEDIUM | Depends on subscription tracking. Must be efficient -- O(subscribers) per notification, not O(clients). |
| **Per-client bounded send queue** | Each WebSocket client gets a bounded outbound message queue. When the queue fills (client not reading fast enough), disconnect the client. Prevents slow clients from consuming unbounded relay memory. | MEDIUM | Queue cap (e.g., 1024 messages). Backpressure strategy: disconnect on overflow. No dropping -- chromatindb messages are not idempotent/replaceable. |
| **Connection lifecycle management** | Accept WebSocket upgrade, run auth challenge, create session, forward messages, handle disconnect/timeout, clean up state. | MEDIUM | State machine: UPGRADING -> AUTHENTICATING -> ACTIVE -> CLOSING. Timeouts for auth (e.g., 10s). Graceful Goodbye forwarding. |
| **Graceful shutdown** | SIGTERM/SIGINT: stop accepting new connections, send WebSocket close frames to all clients, wait for in-flight messages to drain, then exit. | LOW | Standard pattern. Close acceptor, iterate sessions, send close, wait with timeout, force-close stragglers. |
| **Configuration** | JSON config file: bind address, port, TLS cert/key paths, UDS path, log level, log file, max connections. Fail-fast validation on startup. | LOW | Same pattern as old relay + node config. Add TLS fields. |
| **Logging** | Structured logging with client identity (pubkey hash hex), connection events, auth results, errors. spdlog. | LOW | Same as old relay. Per-session context (pubkey hash, remote address). |

## Differentiators

Features that are not strictly required for function but significantly improve operational quality or developer experience.

| Feature | Value Proposition | Complexity | Dependencies |
|---------|-------------------|------------|--------------|
| **Multiplexed UDS (single connection)** | Old relay opened one UDS per client. With 100 clients, that was 100 UDS connections to the node, each with its own AEAD state. Single multiplexed UDS dramatically reduces node connection overhead. | HIGH | Core architectural decision. Requires request_id multiplexing, subscription aggregation on the node side, and careful response routing. This is the hardest feature in the relay. |
| **SIGHUP config reload** | Reload TLS certs, log level, max connections without restart. Zero-downtime cert rotation. Operators expect this from the node -- relay should match. | LOW | Same coroutine-based SIGHUP pattern as the node. TLS context can be swapped atomically. |
| **Health check endpoint** | HTTP GET /health on the WebSocket port (or separate port). Returns 200 if relay is connected to node and accepting clients. Useful for load balancers, monitoring. | LOW | Beast already handles HTTP upgrade -- non-upgrade GET requests can return health status before WebSocket handshake. |
| **Prometheus /metrics endpoint** | HTTP GET /metrics with relay-specific counters: active clients, messages forwarded, auth failures, queue overflows, node connection status. Matches node's existing Prometheus endpoint. | LOW | Same pattern as node's `db/peer/metrics_collector.h`. Share counter naming convention (chromatindb_relay_ prefix). |
| **Rate limiting per client** | Per-client message rate limiting (token bucket). Prevents a single client from flooding the relay and consuming all UDS bandwidth. | LOW | Same token bucket pattern as node's rate limiter. Configurable rate/burst per client. |
| **Connection limits** | Max total connections, max connections per IP. Prevents resource exhaustion from connection floods. | LOW | Atomic counter for total. unordered_map<ip, count> for per-IP. Reject with HTTP 503 before WebSocket upgrade. |
| **Auth timeout** | If client does not complete challenge-response within N seconds after WebSocket upgrade, disconnect. Prevents lingering unauthenticated connections. | LOW | Timer started at upgrade, cancelled on auth success. Default 10s. |
| **Binary payload encoding** | JSON uses hex strings for byte arrays (hashes, pubkeys, signatures). This is human-readable but verbose. Support optional base64 encoding for large payloads (blob data, signatures) to reduce JSON size. Hex for 32-byte hashes, base64 for large fields. | LOW | Convention decision: hex for fixed-size identifiers (namespace_id, blob_hash), base64 for variable-length data (blob data, pubkey, signature). Document clearly. |
| **UDS auto-reconnect** | If UDS connection to node drops, attempt reconnection with jittered backoff. During reconnect, hold client connections open (or send error). Better than immediately disconnecting all clients. | MEDIUM | Old relay had this (ACTIVE/RECONNECTING/DEAD lifecycle). Reimplement for multiplexed UDS. During RECONNECTING, queue or reject client messages. |

## Anti-Features

Features to explicitly NOT build. Each has been considered and rejected with rationale.

| Anti-Feature | Why Tempting | Why Wrong | What to Do Instead |
|--------------|-------------|-----------|-------------------|
| **Per-client UDS connections** | Old relay did this. Simple 1:1 mapping. No multiplexing complexity. | Does not scale. 100 clients = 100 UDS connections = 100 TrustedHello handshakes = 100 AEAD states on the node. Node already has connection dedup logic that would interfere. | Single multiplexed UDS. Harder to build, dramatically better at scale. |
| **WebSocket subprotocol negotiation** | RFC 6455 supports subprotocol negotiation. Could offer "chromatindb-json-v1" as a subprotocol. | Unnecessary complexity. There is exactly one protocol. No negotiation needed. Just use JSON over the WebSocket connection. Adding subprotocol headers adds nothing. | No subprotocol. JSON messages on the WebSocket. Period. |
| **Message compression (permessage-deflate)** | WebSocket extension for per-message compression. Could reduce bandwidth. | Blob data is already encrypted (envelope encryption) -- encrypted data is incompressible. The JSON overhead for metadata messages is small. permessage-deflate adds CPU cost and memory per connection (zlib context). Node already decided wire compression is Out of Scope for this exact reason. | No compression. Encrypted payloads are incompressible. Metadata messages are small enough. |
| **Binary WebSocket frames** | WebSocket supports both text (UTF-8) and binary frames. Could send binary for efficiency. | Defeats the purpose of the relay. The entire point is JSON accessibility -- any language, any tool (wscat, browser console) can interact. Binary frames require a codec. | Text frames only. JSON. Human-readable. Debuggable with standard tools. |
| **HTTP REST API alongside WebSocket** | "Support both REST and WebSocket for flexibility." | Duplicates the entire message handling surface. REST requires separate request routing, cannot do pub/sub notifications, and adds a whole HTTP handler layer. The WebSocket JSON protocol already covers all operations. | WebSocket only (plus /health and /metrics as plain HTTP). All operations go through WebSocket. |
| **Client-side keepalive (application-level)** | Old relay's PQ transport had Ping/Pong at the application layer. Could add JSON ping/pong. | WebSocket protocol has built-in Ping/Pong frames (opcode 0x9/0xA). Beast handles these automatically. Application-level keepalive is redundant. | Use WebSocket-level Ping/Pong. Beast's `auto_ping` or manual ping timer. No JSON-level keepalive. |
| **Authentication via HTTP headers/tokens** | JWT, API keys, bearer tokens in the HTTP upgrade request. Standard for many WebSocket APIs. | chromatindb's identity model is ML-DSA-87 keypairs, not tokens. Adding a token layer would require a separate auth service, token issuance, token validation -- none of which exist. The challenge-response auth over WebSocket is simple and proves cryptographic identity. | Challenge-response over WebSocket after upgrade. Client proves they hold the ML-DSA-87 private key. No tokens, no external auth service. |
| **Multi-node relay (connect to multiple db nodes)** | "Relay could load-balance across multiple nodes." | A relay is a gateway to ONE node. Multi-node would require routing logic, consistency decisions, and breaks the simple gateway model. The node's own peer mesh handles replication. | One relay = one node. Deploy multiple relays if needed (one per node). |
| **Message queuing during UDS reconnect** | "Buffer client messages while reconnecting to node, replay when reconnected." | Unbounded memory risk. Clients could send megabytes of blob data during reconnect window. Order-sensitive protocol state could be violated. | Reject messages with an error response during RECONNECTING state. Client retries. Simple and safe. |
| **Relay-to-relay communication** | "Relays should discover each other for redundancy." | Relay is a stateless gateway. It has no data to share with other relays. Client failover is the client's responsibility (connect to a different relay). | Independent relay instances. No relay mesh. Client decides which relay to connect to. |
| **Automatic TLS cert provisioning (ACME/Let's Encrypt)** | "Relay should auto-provision TLS certs." | Adds HTTP-01/DNS-01 challenge handling, cert storage, renewal timers, ACME client library. Massive scope increase for something that certbot/caddy handle externally. | Operator provides cert_path + key_path. Use certbot/systemd timer externally. SIGHUP to reload new certs. |

## Feature Dependencies

```
[WebSocket server (WSS)]
    |
    +--requires--> [TLS termination]
    |
    +--enables--> [Connection lifecycle management]
                      |
                      +--enables--> [Challenge-response auth]
                      |                 |
                      |                 +--enables--> [Session creation (ACTIVE state)]
                      |
                      +--enables--> [JSON message forwarding]
                                        |
                                        +--requires--> [JSON-to-FlatBuffers translation]
                                        |
                                        +--requires--> [Message type filtering]
                                        |
                                        +--requires--> [Request-response correlation]
                                        |                   |
                                        |                   +--requires--> [Multiplexed UDS]
                                        |
                                        +--requires--> [Subscription tracking]
                                        |                   |
                                        |                   +--enables--> [Notification routing]
                                        |
                                        +--requires--> [Per-client bounded send queue]

[Multiplexed UDS]
    |
    +--requires--> [UDS connection to node]
    |
    +--enables--> [UDS auto-reconnect]
    |
    +--requires--> [Request ID allocation + response routing]

[Configuration]
    +--enables--> [SIGHUP config reload]

[Logging]
    (standalone, no dependencies)

[Graceful shutdown]
    +--requires--> [Connection lifecycle management]
```

### Critical Path

The hardest dependency chain is:

1. Multiplexed UDS with request_id routing (HIGH complexity)
2. JSON-to-FlatBuffers translation for 38 message types (HIGH complexity, HIGH volume of work)
3. Subscription tracking + notification routing on multiplexed UDS (MEDIUM complexity)

Everything else is LOW complexity and follows established patterns from the existing codebase.

## MVP Recommendation

### Phase 1: Foundation (build order matters)

Prioritize in this order:

1. **WebSocket server with TLS** -- the transport layer everything else sits on
2. **Challenge-response authentication** -- must happen before any messages flow
3. **UDS connection to node** -- single multiplexed connection
4. **JSON message format definition** -- define the JSON schema for all 38 types
5. **Configuration + logging** -- operational basics

### Phase 2: Core Protocol

6. **JSON-to-FlatBuffers translation** -- the bulk of the work, type by type
7. **Request-response correlation** -- request_id allocation and response routing
8. **Message type filtering** -- blocklist, same as old relay
9. **Per-client bounded send queue** -- backpressure

### Phase 3: Pub/Sub + Lifecycle

10. **Subscription tracking** -- intercept Subscribe/Unsubscribe
11. **Notification routing** -- route notifications to subscribed clients
12. **Connection lifecycle management** -- auth timeouts, graceful disconnect
13. **Graceful shutdown** -- SIGTERM handling

### Phase 4: Operational Polish

14. **Health check endpoint** -- /health
15. **Prometheus /metrics** -- operational observability
16. **SIGHUP config reload** -- zero-downtime cert rotation
17. **Rate limiting** -- per-client throttling
18. **Connection limits** -- max total, max per-IP
19. **UDS auto-reconnect** -- resilience

### Defer

- **Binary payload encoding (base64):** Nice optimization, not needed for correctness. Add when someone complains about JSON size.
- **Auth timeout:** Can use WebSocket-level idle timeout initially. Dedicated auth timeout is polish.

## Message Type Translation Inventory

The 38 client-allowed message types, grouped by translation complexity:

### Simple (fixed-size, straightforward mapping) -- 20 types

| Type | Name | Direction | Wire Size | JSON Fields |
|------|------|-----------|-----------|-------------|
| 5 | Ping | C->N | 0 | `{}` |
| 6 | Pong | N->C | 0 | `{}` |
| 7 | Goodbye | Both | 0 | `{}` |
| 18 | DeleteAck | N->C | 0 | `{}` |
| 35 | StatsRequest | C->N | 32 | namespace_id (hex) |
| 36 | StatsResponse | N->C | 24 | blob_count, total_bytes, quota_bytes |
| 37 | ExistsRequest | C->N | 64 | namespace_id, blob_hash (hex) |
| 38 | ExistsResponse | N->C | 33 | exists (bool), blob_hash (hex) |
| 39 | NodeInfoRequest | C->N | 0 | `{}` |
| 43 | StorageStatusRequest | C->N | 0 | `{}` |
| 44 | StorageStatusResponse | N->C | 44 | fields as integers |
| 45 | NamespaceStatsRequest | C->N | 32 | namespace_id (hex) |
| 46 | NamespaceStatsResponse | N->C | 41 | found, count, bytes, quota, seq |
| 47 | MetadataRequest | C->N | 64 | namespace_id, blob_hash (hex) |
| 49 | BatchExistsRequest | C->N | 36+N*32 | namespace_id, hashes[] (hex) |
| 50 | BatchExistsResponse | N->C | N bytes | results[] (bool) |
| 51 | DelegationListRequest | C->N | 32 | namespace_id (hex) |
| 55 | PeerInfoRequest | C->N | 0 | `{}` |
| 57 | TimeRangeRequest | C->N | 52 | namespace_id, start, end, limit |

### Medium (variable-size, structured response) -- 12 types

| Type | Name | Direction | Notes |
|------|------|-----------|-------|
| 19 | Subscribe | C->N | Namespace list encoding |
| 20 | Unsubscribe | C->N | Namespace list encoding |
| 21 | Notification | N->C | 77-byte fixed, but relay intercepts for routing |
| 30 | WriteAck | N->C | 41-byte, blob_hash + seq + status |
| 31 | ReadRequest | C->N | 64-byte, namespace + hash |
| 33 | ListRequest | C->N | 44-byte, namespace + cursor + limit |
| 34 | ListResponse | N->C | Variable, array of hash+seq pairs |
| 40 | NodeInfoResponse | N->C | Variable, length-prefixed strings + arrays |
| 42 | NamespaceListResponse | N->C | Variable, namespace array + cursor |
| 52 | DelegationListResponse | N->C | Variable, pk_hash+blob_hash pairs |
| 56 | PeerInfoResponse | N->C | Trust-gated, variable |
| 58 | TimeRangeResponse | N->C | Variable, hash+seq+timestamp triples |

### Complex (FlatBuffers encoding/decoding, large payloads) -- 6 types

| Type | Name | Direction | Notes |
|------|------|-----------|-------|
| 8 | Data | C->N | FlatBuffer Blob. Must construct from JSON fields (namespace, pubkey, data, ttl, timestamp, signature). Largest payload (up to 100 MiB data field). |
| 17 | Delete | C->N | FlatBuffer Blob (tombstone). Same encoding as Data but with tombstone magic + target hash. |
| 32 | ReadResponse | N->C | Status byte + optional FlatBuffer Blob. Must decode Blob fields to JSON. |
| 48 | MetadataResponse | N->C | Variable, includes pubkey (2592 bytes as hex = 5184 chars). |
| 53 | BatchReadRequest | C->N | namespace + cap_bytes + count + hashes |
| 54 | BatchReadResponse | N->C | Truncated flag + multiple status+hash+data entries. Most complex response. |

### Translation Strategy

All binary byte arrays in JSON use lowercase hex encoding. Rationale:
- 32-byte hash -> 64-char hex string (manageable)
- 2592-byte pubkey -> 5184-char hex string (large but acceptable for JSON)
- Blob data (up to 100 MiB) -> base64 encoding (hex would double an already large payload)

**Decision: hex for fixed identifiers, base64 for blob data and signatures.** This balances human readability for identifiers with reasonable size for large payloads.

## Sources

- [Boost.Beast WebSocket documentation](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_websocket.html)
- [Beast async-ssl WebSocket server example](https://www.boost.org/doc/libs/master/libs/beast/example/websocket/server/async-ssl/websocket_server_async_ssl.cpp)
- [WebSocket best practices for production (WebSocket.org)](https://websocket.org/guides/best-practices/)
- [JSON event-based convention for WebSockets (Thoughtbot)](https://thoughtbot.com/blog/json-event-based-convention-websockets)
- [Backpressure in WebSocket Streams (Skyline Codes)](https://skylinecodes.substack.com/p/backpressure-in-websocket-streams)
- [WebSocket architecture best practices (Ably)](https://ably.com/topic/websocket-architecture-best-practices)
- [WebSocket security: auth, TLS, rate limiting (WebSocket.org)](https://websocket.org/guides/security/)
- [How to handle WebSocket rate limiting (OneUptime)](https://oneuptime.com/blog/post/2026-01-24-websocket-rate-limiting/view)
- [How to handle graceful shutdown for WebSocket servers (OneUptime)](https://oneuptime.com/blog/post/2026-02-02-websocket-graceful-shutdown/view)
- [Protocol translation patterns (OneUptime)](https://oneuptime.com/blog/post/2026-01-30-protocol-translation/view)
- [PQ session protocol with ML-KEM + ML-DSA (Frontiers in Physics)](https://www.frontiersin.org/journals/physics/articles/10.3389/fphy.2025.1723966/full)

---
*Feature research for: Relay v2 WebSocket/JSON/TLS Gateway (chromatindb v3.0.0)*
*Researched: 2026-04-09*
