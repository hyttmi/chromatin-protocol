# Requirements: chromatindb Relay v2

**Defined:** 2026-04-09
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v3.0.0 Requirements

Requirements for Relay v2. Each maps to roadmap phases.

### Cleanup

- [ ] **CLEAN-01**: Old relay/ directory deleted
- [ ] **CLEAN-02**: Old sdk/python/ directory deleted
- [ ] **CLEAN-03**: Old relay/SDK Docker artifacts, test references, and doc references removed

### Transport

- [ ] **TRANS-01**: Relay accepts WebSocket Secure (WSS) connections with TLS cert/key from config
- [ ] **TRANS-02**: Relay accepts plain WebSocket (WS) connections for local dev/testing
- [ ] **TRANS-03**: Hand-rolled RFC 6455 WebSocket framing (upgrade handshake, text/binary frames, ping/pong/close)
- [ ] **TRANS-04**: SIGHUP-reloadable TLS context via atomic ssl::context swap

### Authentication

- [ ] **AUTH-01**: Relay sends random 32-byte challenge on WebSocket connect
- [ ] **AUTH-02**: Client signs challenge with ML-DSA-87 private key, relay verifies
- [ ] **AUTH-03**: Auth timeout (10s) disconnects unresponsive clients
- [ ] **AUTH-04**: Auth verification offloaded to thread pool (non-blocking)

### Protocol Translation

- [ ] **PROT-01**: Table-driven JSON to FlatBuffers translation for all 38 relay-allowed message types
- [ ] **PROT-02**: Binary fields encoded as hex (32-byte hashes, namespaces) or base64 (blob data, signatures)
- [ ] **PROT-03**: uint64 fields serialized as JSON strings (prevents JavaScript truncation)
- [ ] **PROT-04**: Binary WebSocket frames for large payloads (ReadResponse, BatchReadResponse)
- [ ] **PROT-05**: Message type filtering (blocklist for peer-internal types, same as old relay)

### UDS Multiplexer

- [ ] **MUX-01**: Single multiplexed UDS connection from relay to node
- [ ] **MUX-02**: Relay-scoped request_id allocation with client-to-relay mapping for response routing
- [ ] **MUX-03**: Subscription aggregation with reference counting (first subscribe sends to node, last unsubscribe sends to node)
- [ ] **MUX-04**: Notification fan-out from node to subscribed WebSocket clients
- [ ] **MUX-05**: UDS auto-reconnect with jittered backoff on node disconnect
- [ ] **MUX-06**: Subscription replay after UDS reconnect
- [ ] **MUX-07**: Pending request timeout on UDS disconnect (no orphaned client requests)

### Session Management

- [ ] **SESS-01**: Per-client bounded send queue with drain coroutine
- [ ] **SESS-02**: Backpressure: disconnect slow clients on queue overflow
- [ ] **SESS-03**: Configurable max concurrent WebSocket connections
- [ ] **SESS-04**: Graceful shutdown on SIGTERM (drain queues, close frames)

### Observability & Operations

- [ ] **OPS-01**: Prometheus /metrics HTTP endpoint (connections, messages, errors)
- [ ] **OPS-02**: SIGHUP config reload (TLS context, connection limits, rate limits)
- [ ] **OPS-03**: Per-client rate limiting (messages/sec or bytes/sec)
- [ ] **OPS-04**: Structured logging via spdlog

## Future Requirements

### Post-v3.0.0

- **FUTURE-01**: Source exclusion for notifications (relay tracks which client wrote, suppresses echo)
- **FUTURE-02**: Relay-side max blob size limit (separate from node's 100 MiB)
- **FUTURE-03**: Health check endpoint (HTTP GET /health)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Boost.Beast / WebSocket++ | Incompatible with standalone Asio (project constraint) |
| Per-client UDS connections | Old pattern — multiplexed UDS is architecturally superior |
| WebSocket message compression | Encrypted payloads are incompressible by design |
| REST API | WebSocket is the transport; no HTTP API beyond /metrics |
| HTTP auth tokens / JWT | ML-DSA-87 challenge-response is the auth mechanism |
| Multi-node relay | One relay talks to one node. Scale by running more relays. |
| Message queuing during UDS reconnect | Too complex; fail pending requests, replay subscriptions only |
| Auto TLS provisioning (ACME) | Operators provide certs; relay just loads them |
| Application-level keepalive | WebSocket ping/pong handles connection liveness |
| Public documentation | Relay is closed source; PROTOCOL.md covers node wire format for third-party relays |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| (populated by roadmapper) | | |

**Coverage:**
- v3.0.0 requirements: 27 total
- Mapped to phases: 0
- Unmapped: 27

---
*Requirements defined: 2026-04-09*
*Last updated: 2026-04-09 after initial definition*
