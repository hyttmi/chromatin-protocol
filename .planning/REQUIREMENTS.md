# Requirements: chromatindb

**Defined:** 2026-03-19
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v0.8.0 Requirements

Requirements for protocol scalability. Fixes fundamental sync protocol flaw, hardens against sync abuse, offloads CPU-bound crypto.

### Set Reconciliation

- [x] **SYNC-06**: Custom XOR-fingerprint range-based reconciliation module built with no external dependency
- [x] **SYNC-07**: Per-namespace reconciliation using custom range-based protocol replaces full hash list exchange (O(differences) not O(total_blobs))
- [x] **SYNC-08**: Existing sync cursors coexist with reconciliation (unchanged namespaces skipped via cursor, reconciliation runs for all namespaces but cursor-hit namespaces skip blob requests)
- [x] **SYNC-09**: Reconciliation wire messages include version byte for forward compatibility
- [ ] **SYNC-10**: Docker benchmark confirms O(diff) improvement over hash-list baseline and no regression for small namespaces

### Sync Rate Limiting

- [ ] **RATE-01**: Sync initiation frequency limited per peer (configurable cooldown)
- [ ] **RATE-02**: Sync messages included in per-peer byte-rate token bucket (extends existing rate limiter)
- [ ] **RATE-03**: Concurrent sync sessions limited per peer (configurable max)

### Thread Pool Crypto Offload

- [x] **PERF-06**: ML-DSA-87 signature verification dispatched to asio::thread_pool (event loop does not block during verify)
- [x] **PERF-07**: SHA3-256 content hash dispatched to asio::thread_pool (event loop does not block during hash)
- [x] **PERF-08**: Connection-scoped AEAD state never accessed from thread pool workers (nonce safety by design)
- [x] **PERF-09**: Thread pool worker count configurable at startup (default: hardware_concurrency)

## Future Requirements

Deferred beyond v0.8.0. Tracked but not in current roadmap.

### Performance (Advanced)

- **PERF-10**: Adaptive thread pool sizing based on load (scale workers up/down)
- **PERF-11**: Per-blob size threshold for thread pool dispatch (skip offload for small blobs where dispatch overhead > crypto cost)

### Cursor Compaction (deferred from v1.0.0)

- **SYNC-05**: Stale peer cursors automatically pruned based on configurable retention period (SIGHUP-reloadable)

### Connection Resilience (deferred from v1.0.0)

- **CONN-01**: Node automatically reconnects to configured/bootstrap peers after disconnection
- **CONN-02**: Reconnection uses exponential backoff with configurable max interval
- **CONN-03**: Reconnection suppressed for peers that sent ACL rejection

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Persistent Merkle tree | Write amplification on every blob ingest. In-memory sorted vectors per sync avoids this. |
| IBLT (Invertible Bloom Lookup Table) | Capacity estimation failures require fallback. Range-based reconciliation is deterministic. |
| Negentropy (external library) | SHA3-256 patching hassle. Custom XOR-fingerprint reconciliation is simpler (~500 LOC) with zero dependency. |
| Multithreaded ingest pipeline | Data structures are single-thread-only by design. Only stateless crypto ops offloaded. |
| Hardware crypto acceleration (SHA3-NI) | Requires CPU feature detection + runtime dispatch. Thread pool offload is the higher-value fix. |
| Peer reputation / scoring | YAGNI. Rate limiting + ACL is sufficient. |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| SYNC-06 | Phase 39 | Complete |
| SYNC-07 | Phase 39 | Complete |
| SYNC-08 | Phase 39 | Complete |
| SYNC-09 | Phase 39 | Complete |
| SYNC-10 | Phase 41 | Pending |
| RATE-01 | Phase 40 | Pending |
| RATE-02 | Phase 40 | Pending |
| RATE-03 | Phase 40 | Pending |
| PERF-06 | Phase 38 | Complete |
| PERF-07 | Phase 38 | Complete |
| PERF-08 | Phase 38 | Complete |
| PERF-09 | Phase 38 | Complete |

**Coverage:**
- v0.8.0 requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-03-19*
*Last updated: 2026-03-19 after v0.8.0 roadmap creation*
