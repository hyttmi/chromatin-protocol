# Requirements: chromatindb Relay v3.1.0 Live Hardening

**Defined:** 2026-04-10
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v3.1.0 Requirements

Requirements for Relay Live Hardening. Each maps to roadmap phases.

### Bug Fixes

- [x] **FIX-01**: binary_to_json succeeds for all compound response types (NodeInfoResponse, StatsResponse, etc.) against live node data
- [x] **FIX-02**: All std::visit + coroutine lambda patterns in relay/ audited and replaced with get_if/get branching

### End-to-End Verification

- [ ] **E2E-01**: All 38 relay-allowed message types translate correctly through relay→node→relay with live node
- [ ] **E2E-02**: Subscribe/Unsubscribe/Notification fan-out works end-to-end with live blob writes
- [ ] **E2E-03**: Rate limiting enforces messages/sec limit and disconnects on sustained violation
- [ ] **E2E-04**: SIGHUP reloads TLS, ACL, rate limit, and metrics_bind without restart
- [ ] **E2E-05**: SIGTERM drains send queues and sends close frames before exit

### Performance

- [ ] **PERF-01**: Relay throughput benchmark — messages/sec at 1, 10, 100 concurrent clients
- [ ] **PERF-02**: Relay latency overhead — relay vs direct UDS for same operation
- [ ] **PERF-03**: Large blob throughput — PDF-size (1-10 MiB), X-ray/DICOM-size (50-100 MiB) write+read through relay
- [ ] **PERF-04**: Mixed workload — concurrent small metadata queries + large blob transfers

### New Features

- [ ] **FEAT-01**: Source exclusion for notifications — relay tracks which client wrote a blob, suppresses echo notification to that client
- [ ] **FEAT-02**: Relay-side max blob size limit (configurable, separate from node's 100 MiB)
- [ ] **FEAT-03**: Health check endpoint (HTTP GET /health returns 200 when relay+UDS connected)

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
| E2E-01 | Phase 107 | Pending |
| E2E-02 | Phase 108 | Pending |
| E2E-03 | Phase 108 | Pending |
| E2E-04 | Phase 108 | Pending |
| E2E-05 | Phase 108 | Pending |
| PERF-01 | Phase 110 | Pending |
| PERF-02 | Phase 110 | Pending |
| PERF-03 | Phase 110 | Pending |
| PERF-04 | Phase 110 | Pending |
| FEAT-01 | Phase 109 | Pending |
| FEAT-02 | Phase 109 | Pending |
| FEAT-03 | Phase 109 | Pending |

**Coverage:**
- v3.1.0 requirements: 14 total
- Mapped to phases: 14
- Unmapped: 0

---
*Requirements defined: 2026-04-10*
