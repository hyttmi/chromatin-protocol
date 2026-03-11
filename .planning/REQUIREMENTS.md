# Requirements: chromatindb

**Defined:** 2026-03-08
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v0.4.0 Requirements

Requirements for v0.4.0 Production Readiness. Each maps to roadmap phases.

### Storage & Data Integrity

- [x] **STOR-01**: Tombstone lookups use O(1) indexed check via dedicated mdbx sub-database instead of O(n) namespace scan
- [x] **STOR-02**: Operator can configure a global storage limit (max_storage_bytes) that prevents the node from exceeding disk capacity
- [x] **STOR-03**: Storage limit check runs as Step 0 inside synchronous ingest() before any crypto operations
- [x] **STOR-04**: Node sends StorageFull wire message to peers when rejecting a blob due to capacity
- [x] **STOR-05**: Peers receiving StorageFull set a peer_is_full flag and suppress sync pushes to that peer

### Operations

- [x] **OPS-01**: SIGTERM triggers graceful shutdown: stop accepting connections, drain in-flight coroutines, save peer list, bounded timeout
- [x] **OPS-02**: Expiry scan coroutine is cancellable via asio::cancellation_signal (not asio::detached with no cancel path)
- [x] **OPS-03**: Persistent peer list is saved atomically on clean shutdown (temp + fsync + rename + dir fsync)
- [x] **OPS-04**: Persistent peer list flushes periodically (30s timer) in addition to shutdown flush
- [x] **OPS-05**: NodeMetrics struct tracks blob count, storage used, connections, syncs, ingests, rejections, rate-limited count
- [x] **OPS-06**: SIGUSR1 dumps current metrics via spdlog (follows sighup_loop coroutine pattern)
- [x] **OPS-07**: Metrics logged periodically (60s timer) via spdlog

### Protocol & Abuse Prevention

- [x] **PROT-01**: Per-connection token bucket rate limiter applies to Data/Delete messages (not sync BlobTransfer)
- [x] **PROT-02**: Rate limit exceeded triggers strike system (immediate disconnect, no backpressure delay)
- [x] **PROT-03**: Rate limit parameters configurable (rate_limit_bytes_per_sec, rate_limit_burst)
- [x] **PROT-04**: Operator can configure sync_namespaces to filter which namespaces the node replicates
- [x] **PROT-05**: Namespace filter applied at sync Phase A (namespace list assembly), not at blob transfer time
- [x] **PROT-06**: Empty sync_namespaces means replicate all (backward compatible default)

### Documentation

- [ ] **DOC-01**: README.md moved from repo root to db/ (chromatindb is the product, not the repo)
- [ ] **DOC-02**: README documents config schema, startup, wire protocol overview, and deployment scenarios
- [ ] **DOC-03**: Interaction samples file showing how to connect to and use the database programmatically
- [ ] **DOC-04**: version.h updated to 0.4.0 after all features pass tests

## Future Requirements

### Deferred

- **Per-namespace storage quota** -- relay/application layer concern, not database node
- **HTTP metrics endpoint** -- violates "No HTTP/REST API" constraint
- **Persistent rate limit state** -- connection-scoped by design; reset on reconnect is correct
- **Write-ahead rate limit queuing** -- reject immediately; queuing adds memory pressure for no gain

## Out of Scope

| Feature | Reason |
|---------|--------|
| Per-namespace storage quota | Wrong layer -- relay/app concern |
| HTTP/Prometheus metrics endpoint | Violates PROJECT.md "No HTTP/REST API" |
| Rate limit backpressure/queuing | Reject + disconnect is simpler and correct |
| Persistent rate limit state across reconnects | Connection-scoped by design |
| Per-peer namespace filter negotiation | YAGNI -- global filter sufficient for v0.4.0 |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| STOR-01 | Phase 16 | Complete |
| STOR-02 | Phase 16 | Complete |
| STOR-03 | Phase 16 | Complete |
| STOR-04 | Phase 16 | Complete |
| STOR-05 | Phase 16 | Complete |
| OPS-01 | Phase 17 | Complete |
| OPS-02 | Phase 17 | Complete |
| OPS-03 | Phase 17 | Complete |
| OPS-04 | Phase 17 | Complete |
| OPS-05 | Phase 17 | Complete |
| OPS-06 | Phase 17 | Complete |
| OPS-07 | Phase 17 | Complete |
| PROT-01 | Phase 18 | Complete |
| PROT-02 | Phase 18 | Complete |
| PROT-03 | Phase 18 | Complete |
| PROT-04 | Phase 18 | Complete |
| PROT-05 | Phase 18 | Complete |
| PROT-06 | Phase 18 | Complete |
| DOC-01 | Phase 19 | Pending |
| DOC-02 | Phase 19 | Pending |
| DOC-03 | Phase 19 | Pending |
| DOC-04 | Phase 19 | Pending |

**Coverage:**
- v0.4.0 requirements: 22 total
- Mapped to phases: 22
- Unmapped: 0

---
*Requirements defined: 2026-03-08*
*Last updated: 2026-03-09 after roadmap creation*
