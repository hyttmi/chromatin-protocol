# Requirements: chromatindb Relay v3.1.0 Live Hardening

**Defined:** 2026-04-10
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v3.1.0 Requirements

Requirements for Relay Live Hardening. Each maps to roadmap phases.

### Bug Fixes

- [x] **FIX-01**: binary_to_json succeeds for all compound response types (NodeInfoResponse, StatsResponse, etc.) against live node data
- [x] **FIX-02**: All std::visit + coroutine lambda patterns in relay/ audited and replaced with get_if/get branching

### End-to-End Verification

- [x] **E2E-01**: All 38 relay-allowed message types translate correctly through relay→node→relay with live node
- [x] **E2E-02**: Subscribe/Unsubscribe/Notification fan-out works end-to-end with live blob writes
- [x] **E2E-03**: Rate limiting enforces messages/sec limit and disconnects on sustained violation
- [x] **E2E-04**: SIGHUP reloads TLS, ACL, rate limit, and metrics_bind without restart
- [x] **E2E-05**: SIGTERM drains send queues and sends close frames before exit

### Performance

- [ ] **PERF-01**: Relay throughput benchmark — messages/sec at 1, 10, 100 concurrent clients
- [ ] **PERF-02**: Relay latency overhead — relay vs direct UDS for same operation
- [ ] **PERF-03**: Large blob throughput — PDF-size (1-10 MiB), X-ray/DICOM-size (50-100 MiB) write+read through relay
- [ ] **PERF-04**: Mixed workload — concurrent small metadata queries + large blob transfers

### New Features

- [x] **FEAT-01**: Source exclusion for notifications — relay tracks which client wrote a blob, suppresses echo notification to that client
- [x] **FEAT-02**: Relay-side max blob size limit (configurable, separate from node's 100 MiB)
- [x] **FEAT-03**: Health check endpoint (HTTP GET /health returns 200 when relay+UDS connected)

## Backlog Requirements

### Error Response (Phase 999.2)

- [x] **ERR-01**: Node sends ErrorResponse(63) with error_code and original_type for all client-facing request failures instead of silent drop
- [x] **ERR-02**: Error codes are categorical (malformed_payload, unknown_type, decode_failed, validation_failed, internal_error) with no internal state leaked
- [x] **ERR-03**: NodeInfoRequest supported_types includes type 63 and PROTOCOL.md documents the ErrorResponse wire format
- [x] **ERR-04**: Relay translates ErrorResponse(63) binary to JSON with human-readable code and type names
- [x] **ERR-05**: Relay type_registry, message_filter, json_schema, and translator all handle ErrorResponse
- [x] **ERR-06**: Prometheus error_responses_total counter tracks error responses sent by the node

### Request Timeout (Phase 999.3)

- [x] **TIMEOUT-01**: PendingRequest stores original_type byte, register_request accepts type parameter
- [x] **TIMEOUT-02**: purge_stale accepts optional callback invoked per stale entry with full PendingRequest
- [x] **TIMEOUT-03**: ERROR_TIMEOUT (0x06) defined in error_codes.h and mapped in relay translator
- [x] **TIMEOUT-04**: RelayConfig has request_timeout_seconds field (default 10, 0=disabled, SIGHUP-reloadable)
- [x] **TIMEOUT-05**: Cleanup loop interval is dynamic: timeout/2 when enabled, 60s when disabled
- [x] **TIMEOUT-06**: Stale requests receive JSON error {type:error, code:timeout, original_type:<name>} before cleanup
- [x] **TIMEOUT-07**: Prometheus errors_total and request_timeouts_total counters increment on timeout

### Binary WS Frame Cleanup (Phase 999.5)

- [x] **WSTEXT-01**: send_binary() removed from WsSession — no binary WS frame send method exists
- [x] **WSTEXT-02**: is_binary_response() removed from translator.h — no binary response type detection exists
- [x] **WSTEXT-03**: route_response() in uds_multiplexer.cpp calls send_json() unconditionally for all response types
- [x] **WSTEXT-04**: write_frame() in ws_session.cpp uses OPCODE_TEXT directly with no binary marker prefix detection

### Endianness Standardization (Phase 999.7)

- [x] **BE-01**: build_signing_input() in db/wire/codec.cpp encodes ttl as BE uint32 and timestamp as BE uint64 using store_u32_be/store_u64_be from db/util/endian.h
- [x] **BE-02**: encode_auth_payload() in db/net/auth_helpers.h writes pubkey_size as BE uint32
- [x] **BE-03**: decode_auth_payload() in db/net/auth_helpers.h reads pubkey_size as BE uint32
- [x] **BE-04**: PROTOCOL.md documents all-BE wire format with zero LE references (ttl_be32, timestamp_be64, pubkey_size BE)
- [x] **BE-05**: Relay UDS multiplexer and relay_uds_tap tool encode/decode auth payload pubkey_size as BE, matching node
- [x] **BE-06**: tools/relay_test_helpers.h build_signing_input() encodes ttl/timestamp as BE, and zero LE references remain in db/, relay/, tools/ source files

### Database Layer Chunking (Phase 999.8)

- [x] **CHUNK-01**: Manifest format encode/decode with CHNK magic prefix (0x43484E4B), BE chunk_count, ordered chunk hashes
- [x] **CHUNK-02**: is_manifest() helper detects manifest blobs by magic prefix, following is_tombstone/is_delegation pattern
- [x] **CHUNK-03**: store_blobs_atomic() stores N pre-computed blobs in a single MDBX write transaction with dedup, capacity, and quota enforcement
- [x] **CHUNK-04**: store_chunked() engine method splits data into 1 MiB chunks, signs each via callback, creates manifest, stores all atomically, returns manifest blob_hash
- [x] **CHUNK-05**: read_chunked() engine method reads manifest, fetches all chunks in order, returns reassembled data or error with chunks_found/chunks_expected
- [x] **CHUNK-06**: PROTOCOL.md documents CHNK manifest magic prefix in the magic prefix registry

### HTTP Transport (Phase 999.9)

- [x] **HTTP-01**: HTTP/1.1 request parser handles method, path, query, headers, Content-Length, Connection keep-alive
- [x] **HTTP-02**: HttpResponse builder serializes JSON, binary, and error responses with correct Content-Type and Content-Length
- [x] **HTTP-03**: Coroutine-based HTTP server accepts TLS/plain connections on configured bind address with connection cap
- [x] **HTTP-04**: Session tokens are opaque RAND_bytes(32) hex, stored in TokenStore with create/lookup/remove/reap_idle
- [x] **HTTP-05**: Challenge-response auth: POST /auth/challenge returns nonce, POST /auth/verify with ML-DSA-87 signature returns bearer token
- [x] **HTTP-06**: Bearer token auth middleware rejects unauthenticated requests with 401
- [ ] **HTTP-07**: POST /blob accepts raw binary FlatBuffer body (application/octet-stream), returns JSON WriteAck
- [ ] **HTTP-08**: GET /blob/{namespace}/{hash} returns raw binary FlatBuffer response (application/octet-stream), 404 if not found
- [ ] **HTTP-09**: DELETE /blob/{namespace}/{hash} accepts raw binary tombstone body, returns JSON DeleteAck
- [x] **HTTP-10**: GET /list/{namespace} with query params returns JSON blob list via translator
- [x] **HTTP-11**: GET /stats/{namespace} returns JSON namespace statistics via translator
- [x] **HTTP-12**: UdsMultiplexer decoupled from ws::SessionManager via SessionDispatch callback interface
- [x] **HTTP-13**: ResponsePromise awaitable bridges async UDS responses to synchronous HTTP handler coroutines
- [ ] **HTTP-14**: POST /batch/read accepts JSON body, returns JSON with base64-encoded blobs via translator
- [x] **HTTP-15**: GET /exists/{namespace}/{hash} returns JSON {exists: bool} via translator
- [x] **HTTP-16**: GET /node-info returns JSON node info via translator
- [x] **HTTP-17**: GET /peer-info returns JSON peer info via translator
- [x] **HTTP-18**: GET /storage-status returns JSON storage status via translator
- [x] **HTTP-19**: GET /metadata/{namespace}/{hash} returns JSON blob metadata via translator
- [x] **HTTP-20**: GET /delegations/{namespace} returns JSON delegation list via translator
- [x] **HTTP-21**: GET /time-range/{namespace} with query params returns JSON time range results via translator
- [ ] **HTTP-22**: POST /subscribe adds namespaces to session subscription set via SubscriptionTracker
- [ ] **HTTP-23**: POST /unsubscribe removes namespaces from session subscription set
- [ ] **HTTP-24**: GET /events?token=<token> returns SSE text/event-stream with notification events
- [ ] **HTTP-25**: SSE heartbeats sent every 30s; disconnect triggers subscription cleanup
- [ ] **HTTP-26**: relay_main.cpp creates HttpServer + TokenStore instead of WsAcceptor + SessionManager
- [ ] **HTTP-27**: /metrics and /health served by main HTTP server (MetricsCollector accept loop removed)
- [ ] **HTTP-28**: SIGHUP reloads TLS, ACL, rate limit, request timeout, max blob size with HTTP transport
- [ ] **HTTP-29**: All WebSocket code deleted: ws_frame, ws_handshake, ws_session, ws_acceptor, session_manager
- [ ] **HTTP-30**: No source file in relay/ includes or references any ws/ header or WS class
- [x] **HTTP-31**: GET /namespace-stats/{namespace} returns JSON per-namespace stats via translator

## Future Requirements

### Post-v3.1.0

None yet.

## Out of Scope

| Feature | Reason |
|---------|--------|
| Node code changes | db/ is frozen — relay-only milestone |
| Multi-node relay | One relay talks to one node. Scale by running more relays. |
| WebSocket compression | Encrypted payloads are incompressible by design |
| Docker integration tests | Local node+relay testing is sufficient for this milestone |
| Python SDK rebuild | Old SDK deleted; any WebSocket client works with relay v2 |
| HTTP/2 | Not needed pre-MVP, HTTP/1.1 is sufficient |
| gRPC | Too heavy for this use case |
| GraphQL | Inappropriate for binary blob operations |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| FIX-01 | Phase 106 | Complete |
| FIX-02 | Phase 106 | Complete |
| E2E-01 | Phase 107 | Complete |
| E2E-02 | Phase 108 | Complete |
| E2E-03 | Phase 108 | Complete |
| E2E-04 | Phase 108 | Complete |
| E2E-05 | Phase 108 | Complete |
| PERF-01 | Phase 110 | Pending |
| PERF-02 | Phase 110 | Pending |
| PERF-03 | Phase 110 | Pending |
| PERF-04 | Phase 110 | Pending |
| FEAT-01 | Phase 109 | Complete |
| FEAT-02 | Phase 109 | Complete |
| FEAT-03 | Phase 109 | Complete |
| ERR-01 | Phase 999.2 | Complete |
| ERR-02 | Phase 999.2 | Complete |
| ERR-03 | Phase 999.2 | Complete |
| ERR-04 | Phase 999.2 | Complete |
| ERR-05 | Phase 999.2 | Complete |
| ERR-06 | Phase 999.2 | Complete |
| TIMEOUT-01 | Phase 999.3 | Complete |
| TIMEOUT-02 | Phase 999.3 | Complete |
| TIMEOUT-03 | Phase 999.3 | Complete |
| TIMEOUT-04 | Phase 999.3 | Complete |
| TIMEOUT-05 | Phase 999.3 | Complete |
| TIMEOUT-06 | Phase 999.3 | Complete |
| TIMEOUT-07 | Phase 999.3 | Complete |
| WSTEXT-01 | Phase 999.5 | Complete |
| WSTEXT-02 | Phase 999.5 | Complete |
| WSTEXT-03 | Phase 999.5 | Complete |
| WSTEXT-04 | Phase 999.5 | Complete |
| BE-01 | Phase 999.7 | Complete |
| BE-02 | Phase 999.7 | Complete |
| BE-03 | Phase 999.7 | Complete |
| BE-04 | Phase 999.7 | Complete |
| BE-05 | Phase 999.7 | Complete |
| BE-06 | Phase 999.7 | Complete |
| CHUNK-01 | Phase 999.8 | Complete |
| CHUNK-02 | Phase 999.8 | Complete |
| CHUNK-03 | Phase 999.8 | Complete |
| CHUNK-04 | Phase 999.8 | Complete |
| CHUNK-05 | Phase 999.8 | Complete |
| CHUNK-06 | Phase 999.8 | Complete |
| HTTP-01 | Phase 999.9 | Complete |
| HTTP-02 | Phase 999.9 | Complete |
| HTTP-03 | Phase 999.9 | Complete |
| HTTP-04 | Phase 999.9 | Complete |
| HTTP-05 | Phase 999.9 | Complete |
| HTTP-06 | Phase 999.9 | Complete |
| HTTP-07 | Phase 999.9 | Pending |
| HTTP-08 | Phase 999.9 | Pending |
| HTTP-09 | Phase 999.9 | Pending |
| HTTP-10 | Phase 999.9 | Complete |
| HTTP-11 | Phase 999.9 | Complete |
| HTTP-12 | Phase 999.9 | Complete |
| HTTP-13 | Phase 999.9 | Complete |
| HTTP-14 | Phase 999.9 | Pending |
| HTTP-15 | Phase 999.9 | Complete |
| HTTP-16 | Phase 999.9 | Complete |
| HTTP-17 | Phase 999.9 | Complete |
| HTTP-18 | Phase 999.9 | Complete |
| HTTP-19 | Phase 999.9 | Complete |
| HTTP-20 | Phase 999.9 | Complete |
| HTTP-21 | Phase 999.9 | Complete |
| HTTP-22 | Phase 999.9 | Pending |
| HTTP-23 | Phase 999.9 | Pending |
| HTTP-24 | Phase 999.9 | Pending |
| HTTP-25 | Phase 999.9 | Pending |
| HTTP-26 | Phase 999.9 | Pending |
| HTTP-27 | Phase 999.9 | Pending |
| HTTP-28 | Phase 999.9 | Pending |
| HTTP-29 | Phase 999.9 | Pending |
| HTTP-30 | Phase 999.9 | Pending |
| HTTP-31 | Phase 999.9 | Complete |

**Coverage:**
- v3.1.0 requirements: 14 total
- Backlog requirements: 60 total (Phase 999.2: 6, Phase 999.3: 7, Phase 999.5: 4, Phase 999.7: 6, Phase 999.8: 6, Phase 999.9: 31)
- Mapped to phases: 74
- Unmapped: 0

---
*Requirements defined: 2026-04-10*
*Backlog requirements added: 2026-04-11*
*Endianness requirements added: 2026-04-12*
*Binary WS frame cleanup requirements added: 2026-04-12*
*Chunking requirements added: 2026-04-12*
*HTTP transport requirements added: 2026-04-13*
