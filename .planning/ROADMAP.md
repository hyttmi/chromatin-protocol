# Roadmap: chromatindb v3.0.0 Relay v2

## Overview

Kill the old relay and Python SDK, build a new closed-source WebSocket/JSON/TLS relay from scratch. The relay translates between JSON WebSocket clients and the FlatBuffers node protocol over a single multiplexed UDS connection. PQ challenge-response authentication, per-client send queues with backpressure, subscription aggregation with notification fan-out, and Prometheus observability. Node code (db/) is frozen -- no changes. Six phases: cleanup and foundation, WebSocket transport, authentication and JSON schema, UDS multiplexer with protocol translation, pub/sub with UDS resilience, and operational polish.

## Phases

**Phase Numbering:**
- Continues from v2.2.0 (Phases 95-99)
- v3.0.0 starts at Phase 100

- [x] **Phase 100: Cleanup & Foundation** - Delete old relay/SDK, scaffold new relay binary with per-client send queues and structured logging (completed 2026-04-09)
- [x] **Phase 101: WebSocket Transport** - WSS/WS listener with hand-rolled RFC 6455 framing and SIGHUP-reloadable TLS (completed 2026-04-09)
- [x] **Phase 102: Authentication & JSON Schema** - ML-DSA-87 challenge-response auth over WebSocket, JSON message schema design for all 38 types (completed 2026-04-09)
- [x] **Phase 103: UDS Multiplexer & Protocol Translation** - Single multiplexed UDS to node with request routing, table-driven JSON-to-FlatBuffers translation (completed 2026-04-10)
- [x] **Phase 104: Pub/Sub & UDS Resilience** - Subscription aggregation, notification fan-out, UDS auto-reconnect with subscription replay (completed 2026-04-10)
- [ ] **Phase 105: Operational Polish** - Prometheus metrics, rate limiting, SIGHUP config reload, graceful shutdown

## Phase Details

### Phase 100: Cleanup & Foundation
**Goal**: Clean break from old code and a buildable relay skeleton with the per-client send queue primitive that all downstream phases depend on
**Depends on**: Nothing (first phase)
**Requirements**: CLEAN-01, CLEAN-02, CLEAN-03, SESS-01, SESS-02, OPS-04
**Success Criteria** (what must be TRUE):
  1. Old relay/ directory is gone from the repo
  2. Old sdk/python/ directory is gone from the repo
  3. No stale Docker artifacts, test references, or doc references to old relay/SDK remain
  4. New relay binary compiles and starts (exits cleanly with no config)
  5. Per-client session object exists with bounded send queue and drain coroutine, slow clients disconnected on overflow
**Plans**: 2 plans (Wave 1: 100-01, Wave 2: 100-02)

Plans:
- [x] 100-01-PLAN.md -- Delete old relay and SDK, clean stale references
- [x] 100-02-PLAN.md -- Scaffold relay binary with session send queue and spdlog

### Phase 101: WebSocket Transport
**Goal**: Relay accepts WebSocket connections over TLS and plain TCP with correct RFC 6455 framing
**Depends on**: Phase 100
**Requirements**: TRANS-01, TRANS-02, TRANS-03, TRANS-04
**Success Criteria** (what must be TRUE):
  1. A WebSocket client can connect over WSS with a TLS cert/key configured in the relay config file
  2. A WebSocket client can connect over plain WS for local dev/testing
  3. WebSocket upgrade handshake, text frames, binary frames, ping/pong, and close frames work correctly per RFC 6455
  4. SIGHUP reloads the TLS certificate and key without restarting the relay
**Plans**: 2 plans (Wave 1: 101-01, Wave 2: 101-02)

Plans:
- [x] 101-01-PLAN.md -- WS framing library, HTTP upgrade handshake, Session write callback, TLS config, OpenSSL build
- [x] 101-02-PLAN.md -- WsSession, WsAcceptor, SessionManager, main() integration with thread pool and SIGHUP TLS reload

### Phase 102: Authentication & JSON Schema
**Goal**: Clients authenticate via ML-DSA-87 challenge-response over WebSocket, and the JSON message schema for all 38 relay-allowed types is designed and validated
**Depends on**: Phase 101
**Requirements**: AUTH-01, AUTH-02, AUTH-03, AUTH-04, PROT-02, PROT-03, PROT-05, SESS-03
**Success Criteria** (what must be TRUE):
  1. Relay sends a random 32-byte challenge immediately on WebSocket connect
  2. Client signs the challenge with ML-DSA-87 and relay verifies the signature, establishing the client's identity
  3. Clients that do not authenticate within 10 seconds are disconnected
  4. Signature verification runs on a thread pool and does not block the IO loop
  5. JSON schema covers all 38 relay-allowed message types with hex encoding for hashes/namespaces, base64 for blob data/signatures, and string encoding for uint64 fields
**Plans**: 2 plans (Wave 1: 102-01, Wave 2: 102-02)

Plans:
- [x] 102-01-PLAN.md -- Challenge-response auth: Authenticator class, WsSession state machine, config extensions, SIGHUP reload
- [x] 102-02-PLAN.md -- JSON schema: type registry, field encoding metadata, message filter, WsSession filter wiring

### Phase 103: UDS Multiplexer & Protocol Translation
**Goal**: Relay maintains a single multiplexed UDS connection to the node and translates JSON client requests to FlatBuffers and back, routing responses to the correct client via relay-scoped request_id mapping
**Depends on**: Phase 102
**Requirements**: MUX-01, MUX-02, PROT-01, PROT-04
**Success Criteria** (what must be TRUE):
  1. Relay opens and maintains a single UDS connection to the local node
  2. Client JSON requests are translated to FlatBuffers, sent to node over UDS, and responses are translated back to JSON and routed to the originating client via relay-scoped request_id mapping
  3. Table-driven translation covers all 38 relay-allowed message types without per-type handler functions
  4. Large payloads (ReadResponse, BatchReadResponse) are sent as binary WebSocket frames
**Plans**: 2 plans (Wave 1: 103-01, Wave 2: 103-02)

Plans:
- [x] 103-01-PLAN.md -- Wire infrastructure (AEAD, TransportCodec, utilities, FlatBuffers) and RequestRouter
- [x] 103-02-PLAN.md -- UdsMultiplexer with TrustedHello handshake, table-driven translator, and WsSession integration

### Phase 104: Pub/Sub & UDS Resilience
**Goal**: Clients can subscribe to namespace changes and receive notifications, and the relay recovers gracefully from node disconnects
**Depends on**: Phase 103
**Requirements**: MUX-03, MUX-04, MUX-05, MUX-06, MUX-07
**Success Criteria** (what must be TRUE):
  1. Multiple clients subscribing to the same namespace result in a single Subscribe sent to the node, with unsubscribe sent only when the last client unsubscribes (reference counting)
  2. Notifications from the node are fanned out to all WebSocket clients subscribed to that namespace
  3. When the UDS connection to the node drops, relay reconnects with jittered backoff and replays all active subscriptions
  4. Pending client requests are failed with an error when the UDS connection drops (no orphaned requests)
**Plans**: 2 plans (Wave 1: 104-01, Wave 2: 104-02)

Plans:
- [x] 104-01-PLAN.md -- Subscription aggregation with reference counting and notification fan-out
- [x] 104-02-PLAN.md -- UDS auto-reconnect with subscription replay and pending request cleanup

### Phase 105: Operational Polish
**Goal**: Relay is production-ready with observability, rate limiting, config reload, and graceful shutdown
**Depends on**: Phase 104
**Requirements**: OPS-01, OPS-02, OPS-03, SESS-04
**Success Criteria** (what must be TRUE):
  1. Prometheus /metrics HTTP endpoint exposes connection count, message throughput, and error counters
  2. SIGHUP reloads TLS context, connection limits, and rate limit configuration without restart
  3. Per-client rate limiting enforces messages/sec or bytes/sec limits with disconnect on violation
  4. SIGTERM triggers graceful shutdown: drain send queues, send WebSocket close frames, then exit
**Plans**: 2 plans (Wave 1: 105-01, Wave 2: 105-02)

Plans:
- [ ] 105-01-PLAN.md -- Core components: RateLimiter, RelayMetrics, MetricsCollector, config extensions, tests
- [ ] 105-02-PLAN.md -- Integration: WsSession rate limiting, metrics wiring, SIGHUP extension, SIGTERM drain-first

## Progress

**Execution Order:**
Phases execute in numeric order: 100 -> 101 -> 102 -> 103 -> 104 -> 105

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 100. Cleanup & Foundation | 2/2 | Complete    | 2026-04-09 |
| 101. WebSocket Transport | 2/2 | Complete    | 2026-04-09 |
| 102. Authentication & JSON Schema | 2/2 | Complete    | 2026-04-09 |
| 103. UDS Multiplexer & Protocol Translation | 2/2 | Complete    | 2026-04-10 |
| 104. Pub/Sub & UDS Resilience | 2/2 | Complete    | 2026-04-10 |
| 105. Operational Polish | 0/2 | Not started | - |
