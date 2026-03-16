# Requirements: chromatindb

**Defined:** 2026-03-16
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v0.7.0 Requirements

Requirements for production readiness milestone. Each maps to roadmap phases.

### Cleanup

- [ ] **CLEAN-01**: Tests relocated into db/ directory with CTest discovery preserved (284 tests before = 284 tests after)
- [ ] **CLEAN-02**: Old standalone benchmark binary (chromatindb_bench) removed from build
- [ ] **CLEAN-03**: db/ README updated with current features, old benchmark data removed
- [ ] **CLEAN-04**: Stale artifacts swept and removed (dead code, leftover files from previous milestones)

### Performance

- [ ] **PERF-01**: Redundant SHA3-256 hash + FlatBuffer re-encode eliminated from ingest pipeline (pre-computed hash passed through)
- [ ] **PERF-02**: OQS_SIG context cached (static or thread_local) instead of created/destroyed per verify call
- [ ] **PERF-03**: Dedup check (has_blob) runs before signature verification on sync-received blobs, skipping crypto for already-stored blobs

### Sync Resumption

- [ ] **SYNC-01**: Per-peer per-namespace seq_num cursors tracked during sync rounds
- [ ] **SYNC-02**: Hash-list exchange skipped for namespaces where remote seq_num equals cursor (unchanged since last sync)
- [ ] **SYNC-03**: Sync cursors persisted across node restarts via dedicated libmdbx sub-database
- [ ] **SYNC-04**: Periodic full hash-diff resync as fallback for cursor drift (configurable interval, default every 10th round)

### Namespace Quotas

- [ ] **QUOTA-01**: Per-namespace maximum byte limit configurable and enforced at ingest
- [ ] **QUOTA-02**: Per-namespace maximum blob count limit configurable and enforced at ingest
- [ ] **QUOTA-03**: Namespace usage tracked via materialized aggregate in libmdbx sub-database (O(1) lookup on write path)
- [ ] **QUOTA-04**: Quota exceeded rejection signaled to writing peer with clear error

### Benchmarks

- [ ] **BENCH-01**: Tombstone creation performance scenario added to Docker benchmark suite
- [ ] **BENCH-02**: Tombstone sync propagation latency measured across multi-node topology
- [ ] **BENCH-03**: Tombstone GC/expiry performance measured under load

## Future Requirements

Deferred to v0.8.0+. Tracked but not in current roadmap.

### Performance (Advanced)

- **PERF-04**: Crypto offload to asio::thread_pool for ML-DSA-87 verify (breaks >20 blobs/sec ceiling for 1 MiB)
- **PERF-05**: thread_local buffer reuse for build_signing_input() to reduce allocation pressure

### Sync (Advanced)

- **SYNC-05**: Cursor compaction (prune cursors for peers not seen in N days)

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Multithreaded ingest pipeline | All data structures are single-thread-only by design. Thread pool offload for crypto only (v0.8.0). |
| Hardware crypto acceleration (SHA3-NI, AES-NI) | Requires CPU feature detection + runtime dispatch. ML-DSA-87 verify is the dominant cost, not SHA3. |
| HashML-DSA (pre-hash signing) | Breaking protocol change, liboqs has no separate HashML-DSA API. Adopt if liboqs adds support later. |
| Complex quota policies (tiered, time-based, burst) | YAGNI. Database is intentionally dumb. Relay layer owns sophisticated policies. |
| Quota negotiation protocol | Quotas are node-local config, like max_storage_bytes. No new wire messages. |
| Chunked/streaming blob verification | ML-DSA-87 requires full message. No incremental API in liboqs. |
| Per-blob sync cursors | Excessive granularity. Per-namespace seq_num is the right resumption unit. |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| CLEAN-01 | — | Pending |
| CLEAN-02 | — | Pending |
| CLEAN-03 | — | Pending |
| CLEAN-04 | — | Pending |
| PERF-01 | — | Pending |
| PERF-02 | — | Pending |
| PERF-03 | — | Pending |
| SYNC-01 | — | Pending |
| SYNC-02 | — | Pending |
| SYNC-03 | — | Pending |
| SYNC-04 | — | Pending |
| QUOTA-01 | — | Pending |
| QUOTA-02 | — | Pending |
| QUOTA-03 | — | Pending |
| QUOTA-04 | — | Pending |
| BENCH-01 | — | Pending |
| BENCH-02 | — | Pending |
| BENCH-03 | — | Pending |

**Coverage:**
- v0.7.0 requirements: 18 total
- Mapped to phases: 0
- Unmapped: 18 (pending roadmap creation)

---
*Requirements defined: 2026-03-16*
*Last updated: 2026-03-16 after initial definition*
