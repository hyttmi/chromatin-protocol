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

- [ ] **FEAT-01**: Source exclusion for notifications — relay tracks which client wrote a blob, suppresses echo notification to that client
- [ ] **FEAT-02**: Relay-side max blob size limit (configurable, separate from node's 100 MiB)
- [ ] **FEAT-03**: Health check endpoint (HTTP GET /health returns 200 when relay+UDS connected)

## Backlog Requirements

### Error Response (Phase 999.2)

- [x] **ERR-01**: Node sends ErrorResponse(63) with error_code and original_type for all client-facing request failures instead of silent drop
- [x] **ERR-02**: Error codes are categorical (malformed_payload, unknown_type, decode_failed, validation_failed, internal_error) with no internal state leaked
- [x] **ERR-03**: NodeInfoRequest supported_types includes type 63 and PROTOCOL.md documents the ErrorResponse wire format
- [x] **ERR-04**: Relay translates ErrorResponse(63) binary to JSON with human-readable code and type names
- [x] **ERR-05**: Relay type_registry, message_filter, json_schema, and translator all handle ErrorResponse
- [x] **ERR-06**: Prometheus error_responses_total counter tracks error responses sent by the node

### Request Timeout (Phase 999.3)

- [ ] **TIMEOUT-01**: PendingRequest stores original_type byte, register_request accepts type parameter
- [ ] **TIMEOUT-02**: purge_stale accepts optional callback invoked per stale entry with full PendingRequest
- [ ] **TIMEOUT-03**: ERROR_TIMEOUT (0x06) defined in error_codes.h and mapped in relay translator
- [ ] **TIMEOUT-04**: RelayConfig has request_timeout_seconds field (default 10, 0=disabled, SIGHUP-reloadable)
- [ ] **TIMEOUT-05**: Cleanup loop interval is dynamic: timeout/2 when enabled, 60s when disabled
- [ ] **TIMEOUT-06**: Stale requests receive JSON error {type:error, code:timeout, original_type:<name>} before cleanup
- [ ] **TIMEOUT-07**: Prometheus errors_total and request_timeouts_total counters increment on timeout

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
| FEAT-01 | Phase 109 | Pending |
| FEAT-02 | Phase 109 | Pending |
| FEAT-03 | Phase 109 | Pending |
| ERR-01 | Phase 999.2 | Complete |
| ERR-02 | Phase 999.2 | Complete |
| ERR-03 | Phase 999.2 | Complete |
| ERR-04 | Phase 999.2 | Complete |
| ERR-05 | Phase 999.2 | Complete |
| ERR-06 | Phase 999.2 | Complete |
| TIMEOUT-01 | Phase 999.3 | Pending |
| TIMEOUT-02 | Phase 999.3 | Pending |
| TIMEOUT-03 | Phase 999.3 | Pending |
| TIMEOUT-04 | Phase 999.3 | Pending |
| TIMEOUT-05 | Phase 999.3 | Pending |
| TIMEOUT-06 | Phase 999.3 | Pending |
| TIMEOUT-07 | Phase 999.3 | Pending |

**Coverage:**
- v3.1.0 requirements: 14 total
- Backlog requirements: 13 total (Phase 999.2: 6, Phase 999.3: 7)
- Mapped to phases: 27
- Unmapped: 0

---
*Requirements defined: 2026-04-10*
*Backlog requirements added: 2026-04-11*
