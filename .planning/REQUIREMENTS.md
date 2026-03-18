# Requirements: chromatindb

**Defined:** 2026-03-18
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v1.0.0 Requirements

Requirements for the "database layer is done" release. Each maps to roadmap phases.

### Thread Pool Crypto Offload

- [ ] **PERF-06**: ML-DSA-87 signature verification dispatched to asio::thread_pool (event loop does not block during verify)
- [ ] **PERF-07**: SHA3-256 content hash dispatched to asio::thread_pool (event loop does not block during hash)
- [ ] **PERF-08**: Connection-scoped AEAD state never accessed from thread pool workers (nonce safety by design)
- [ ] **PERF-09**: Thread pool worker count configurable at startup (default: hardware_concurrency)

### Cursor Compaction

- [ ] **SYNC-05**: Stale peer cursors automatically pruned based on configurable retention period (SIGHUP-reloadable)

### Connection Resilience

- [ ] **CONN-01**: Node automatically reconnects to configured/bootstrap peers after disconnection
- [ ] **CONN-02**: Reconnection uses exponential backoff with configurable max interval
- [ ] **CONN-03**: Reconnection suppressed for peers that sent ACL rejection (no retry against peers that won't accept us)

### Benchmark Validation

- [ ] **BENCH-04**: Full Docker benchmark suite re-run with thread pool offload enabled
- [ ] **BENCH-05**: 1 MiB blob throughput compared against v0.6.0 baseline (15.3 blobs/sec) with improvement quantified
- [ ] **BENCH-06**: Small/medium blob throughput confirmed with no regression from thread pool overhead

## Future Requirements

Deferred beyond v1.0.0. Tracked but not in current roadmap.

### Performance (Advanced)

- **PERF-10**: Adaptive thread pool sizing based on load (scale workers up/down)
- **PERF-11**: Per-blob size threshold for thread pool dispatch (skip offload for small blobs where dispatch overhead > crypto cost)

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Multithreaded ingest pipeline | Data structures are single-thread-only by design. Only stateless crypto ops offloaded. |
| Hardware crypto acceleration (SHA3-NI) | Requires CPU feature detection + runtime dispatch. Thread pool offload is the higher-value fix. |
| Connection pooling / multiplexing | One connection per peer is sufficient. Retry with backoff handles disconnects. |
| Peer reputation / scoring | YAGNI. ACL rejection suppression is sufficient for v1.0.0. |
| Adaptive backoff based on network conditions | Exponential backoff with max cap is sufficient. Relay layer can implement smarter strategies. |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| PERF-06 | — | Pending |
| PERF-07 | — | Pending |
| PERF-08 | — | Pending |
| PERF-09 | — | Pending |
| SYNC-05 | — | Pending |
| CONN-01 | — | Pending |
| CONN-02 | — | Pending |
| CONN-03 | — | Pending |
| BENCH-04 | — | Pending |
| BENCH-05 | — | Pending |
| BENCH-06 | — | Pending |

**Coverage:**
- v1.0.0 requirements: 11 total
- Mapped to phases: 0
- Unmapped: 11

---
*Requirements defined: 2026-03-18*
*Last updated: 2026-03-18 after initial definition*
