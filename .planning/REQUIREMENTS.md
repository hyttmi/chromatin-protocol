# Requirements: chromatindb

**Defined:** 2026-03-16
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v0.7.0 Requirements

Requirements for production readiness milestone. Each maps to roadmap phases.

### Cleanup

- [x] **CLEAN-01**: Tests relocated into db/ directory with CTest discovery preserved (313 tests before = 313 tests after)
- [ ] **CLEAN-02**: Old standalone benchmark binary (chromatindb_bench) removed from build
- [x] **CLEAN-03**: db/ README updated with current features, old benchmark data removed
- [x] **CLEAN-04**: Stale artifacts swept and removed (dead code, leftover files from previous milestones)

### Performance

- [x] **PERF-01**: Redundant SHA3-256 hash + FlatBuffer re-encode eliminated from ingest pipeline (pre-computed hash passed through)
- [x] **PERF-02**: OQS_SIG context cached (static or thread_local) instead of created/destroyed per verify call
- [x] **PERF-03**: Dedup check (has_blob) runs before signature verification on sync-received blobs, skipping crypto for already-stored blobs
- [x] **PERF-04**: Hash-then-sign protocol change — sign/verify over SHA3-256(namespace||data||ttl||timestamp) (32 bytes) instead of raw concatenation (~1 MiB for large blobs). Breaking change, acceptable pre-MVP.
- [x] **PERF-05**: Sync receive path copy reduction — eliminate redundant .assign() copies in TransportCodec::decode and wire::decode_blob by passing encoded FlatBuffer bytes through to storage without intermediate decode/re-encode

### Sync Resumption

- [x] **SYNC-01**: Per-peer per-namespace seq_num cursors tracked during sync rounds
- [x] **SYNC-02**: Hash-list exchange skipped for namespaces where remote seq_num equals cursor (unchanged since last sync)
- [x] **SYNC-03**: Sync cursors persisted across node restarts via dedicated libmdbx sub-database
- [x] **SYNC-04**: Periodic full hash-diff resync as fallback for cursor drift (configurable interval, default every 10th round)

### Namespace Quotas

- [x] **QUOTA-01**: Per-namespace maximum byte limit configurable and enforced at ingest
- [x] **QUOTA-02**: Per-namespace maximum blob count limit configurable and enforced at ingest
- [x] **QUOTA-03**: Namespace usage tracked via materialized aggregate in libmdbx sub-database (O(1) lookup on write path)
- [x] **QUOTA-04**: Quota exceeded rejection signaled to writing peer with clear error

### Benchmarks

- [x] **BENCH-01**: Tombstone creation performance scenario added to Docker benchmark suite
- [x] **BENCH-02**: Tombstone sync propagation latency measured across multi-node topology
- [x] **BENCH-03**: Tombstone GC/expiry performance measured under load

## Future Requirements

Deferred to v0.8.0+. Tracked but not in current roadmap.

### Performance (Advanced)

- **PERF-06**: Crypto offload to asio::thread_pool for ML-DSA-87 verify (breaks >20 blobs/sec ceiling for 1 MiB)

### Sync (Advanced)

- **SYNC-05**: Cursor compaction (prune cursors for peers not seen in N days)

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Multithreaded ingest pipeline | All data structures are single-thread-only by design. Thread pool offload for crypto only (v0.8.0). |
| Hardware crypto acceleration (SHA3-NI, AES-NI) | Requires CPU feature detection + runtime dispatch. ML-DSA-87 verify is the dominant cost, not SHA3. |
| HashML-DSA (FIPS 204 pre-hash mode) | liboqs has no separate HashML-DSA API. PERF-04 achieves the same effect (SHA3-256 pre-hash before sign/verify) without needing the formal HashML-DSA mode. |
| Complex quota policies (tiered, time-based, burst) | YAGNI. Database is intentionally dumb. Relay layer owns sophisticated policies. |
| Quota negotiation protocol | Quotas are node-local config, like max_storage_bytes. No new wire messages. |
| Chunked/streaming blob verification | ML-DSA-87 requires full message. No incremental API in liboqs. |
| Per-blob sync cursors | Excessive granularity. Per-namespace seq_num is the right resumption unit. |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| CLEAN-01 | Phase 32 | Complete |
| CLEAN-02 | Phase 37 | Pending |
| CLEAN-03 | Phase 37 | Complete |
| CLEAN-04 | Phase 37 | Complete |
| PERF-01 | Phase 33 | Complete |
| PERF-02 | Phase 33 | Complete |
| PERF-03 | Phase 33 | Complete |
| PERF-04 | Phase 33 | Complete |
| PERF-05 | Phase 33 | Complete |
| SYNC-01 | Phase 34 | Complete |
| SYNC-02 | Phase 34 | Complete |
| SYNC-03 | Phase 34 | Complete |
| SYNC-04 | Phase 34 | Complete |
| QUOTA-01 | Phase 35 | Complete |
| QUOTA-02 | Phase 35 | Complete |
| QUOTA-03 | Phase 35 | Complete |
| QUOTA-04 | Phase 35 | Complete |
| BENCH-01 | Phase 36 | Complete |
| BENCH-02 | Phase 36 | Complete |
| BENCH-03 | Phase 36 | Complete |

**Coverage:**
- v0.7.0 requirements: 20 total
- Mapped to phases: 20
- Unmapped: 0

---
*Requirements defined: 2026-03-16*
*Last updated: 2026-03-17 — added PERF-04 (hash-then-sign), PERF-05 (sync copy reduction), removed future PERF-05 (obsoleted by hash-then-sign)*
