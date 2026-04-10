# Requirements: chromatindb Relay v2

**Defined:** 2026-04-09
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v3.0.0 Requirements

Requirements for Relay v2. Each maps to roadmap phases.

### Cleanup

- [x] **CLEAN-01**: Old relay/ directory deleted
- [x] **CLEAN-02**: Old sdk/python/ directory deleted
- [x] **CLEAN-03**: Old relay/SDK Docker artifacts, test references, and doc references removed

### Transport

- [x] **TRANS-01**: Relay accepts WebSocket Secure (WSS) connections with TLS cert/key from config
- [x] **TRANS-02**: Relay accepts plain WebSocket (WS) connections for local dev/testing
- [x] **TRANS-03**: Hand-rolled RFC 6455 WebSocket framing (upgrade handshake, text/binary frames, ping/pong/close)
- [x] **TRANS-04**: SIGHUP-reloadable TLS context via atomic ssl::context swap

### Authentication

- [x] **AUTH-01**: Relay sends random 32-byte challenge on WebSocket connect
- [x] **AUTH-02**: Client signs challenge with ML-DSA-87 private key, relay verifies
- [x] **AUTH-03**: Auth timeout (10s) disconnects unresponsive clients
- [x] **AUTH-04**: Auth verification offloaded to thread pool (non-blocking)

### Protocol Translation

- [x] **PROT-01**: Table-driven JSON to FlatBuffers translation for all 38 relay-allowed message types
- [x] **PROT-02**: Binary fields encoded as hex (32-byte hashes, namespaces) or base64 (blob data, signatures)
- [x] **PROT-03**: uint64 fields serialized as JSON strings (prevents JavaScript truncation)
- [x] **PROT-04**: Binary WebSocket frames for large payloads (ReadResponse, BatchReadResponse)
- [x] **PROT-05**: Message type filtering (blocklist for peer-internal types, same as old relay)

### UDS Multiplexer

- [x] **MUX-01**: Single multiplexed UDS connection from relay to node
- [x] **MUX-02**: Relay-scoped request_id allocation with client-to-relay mapping for response routing
- [x] **MUX-03**: Subscription aggregation with reference counting (first subscribe sends to node, last unsubscribe sends to node)
- [x] **MUX-04**: Notification fan-out from node to subscribed WebSocket clients
- [ ] **MUX-05**: UDS auto-reconnect with jittered backoff on node disconnect
- [ ] **MUX-06**: Subscription replay after UDS reconnect
- [ ] **MUX-07**: Pending request timeout on UDS disconnect (no orphaned client requests)

### Session Management

- [x] **SESS-01**: Per-client bounded send queue with drain coroutine
- [x] **SESS-02**: Backpressure: disconnect slow clients on queue overflow
- [x] **SESS-03**: Configurable max concurrent WebSocket connections
- [ ] **SESS-04**: Graceful shutdown on SIGTERM (drain queues, close frames)

### Observability & Operations

- [ ] **OPS-01**: Prometheus /metrics HTTP endpoint (connections, messages, errors)
- [ ] **OPS-02**: SIGHUP config reload (TLS context, connection limits, rate limits)
- [ ] **OPS-03**: Per-client rate limiting (messages/sec or bytes/sec)
- [x] **OPS-04**: Structured logging via spdlog

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
| CLEAN-01 | Phase 100 | Complete |
| CLEAN-02 | Phase 100 | Complete |
| CLEAN-03 | Phase 100 | Complete |
| SESS-01 | Phase 100 | Complete |
| SESS-02 | Phase 100 | Complete |
| OPS-04 | Phase 100 | Complete |
| TRANS-01 | Phase 101 | Complete |
| TRANS-02 | Phase 101 | Complete |
| TRANS-03 | Phase 101 | Complete |
| TRANS-04 | Phase 101 | Complete |
| AUTH-01 | Phase 102 | Complete |
| AUTH-02 | Phase 102 | Complete |
| AUTH-03 | Phase 102 | Complete |
| AUTH-04 | Phase 102 | Complete |
| PROT-02 | Phase 102 | Complete |
| PROT-03 | Phase 102 | Complete |
| PROT-05 | Phase 102 | Complete |
| SESS-03 | Phase 102 | Complete |
| MUX-01 | Phase 103 | Complete |
| MUX-02 | Phase 103 | Complete |
| PROT-01 | Phase 103 | Complete |
| PROT-04 | Phase 103 | Complete |
| MUX-03 | Phase 104 | Complete |
| MUX-04 | Phase 104 | Complete |
| MUX-05 | Phase 104 | Pending |
| MUX-06 | Phase 104 | Pending |
| MUX-07 | Phase 104 | Pending |
| OPS-01 | Phase 105 | Pending |
| OPS-02 | Phase 105 | Pending |
| OPS-03 | Phase 105 | Pending |
| SESS-04 | Phase 105 | Pending |

**Coverage:**
- v3.0.0 requirements: 31 total
- Mapped to phases: 31
- Unmapped: 0

---
*Requirements defined: 2026-04-09*
*Last updated: 2026-04-10 after Phase 103 Plan 01 completion*
