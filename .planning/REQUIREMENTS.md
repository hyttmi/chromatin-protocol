# Requirements: chromatindb

**Defined:** 2026-03-20
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v1.0.0 Requirements

Requirements for the "database layer is done" open-source release. Each maps to roadmap phases.

### Cryptographic Integrity & Transport

- [x] **CRYPT-01**: Docker test verifies immutable content addressing -- blob hash matches independent SHA3-256(namespace||data||ttl||timestamp) recomputation on synced peer
- [x] **CRYPT-02**: Docker test verifies author non-repudiation -- ML-DSA-87 signature on blob from Node A independently verified on Node B using blob's pubkey
- [x] **CRYPT-03**: Docker test verifies payload tamper-proofing -- single bit flip in data.mdb causes AEAD authentication failure, node refuses to serve corrupted blob
- [x] **CRYPT-04**: Docker test verifies forward secrecy -- captured PQ handshake traffic cannot be decrypted using long-term ML-DSA-87 identity keys
- [x] **CRYPT-05**: Docker test verifies MITM rejection -- injected node substituting KEM keys causes session fingerprint mismatch and handshake failure on both sides
- [x] **CRYPT-06**: Docker test verifies trusted peer bypass -- lightweight handshake skips KEM but still verifies ML-DSA-87 identity; wrong identity key rejected even if IP is in trusted_peers

### Access Control

- [x] **ACL-01**: Docker test verifies closed-garden enforcement -- unauthorized node disconnected after handshake, no application-layer messages exchanged (packet capture verification)
- [x] **ACL-02**: Docker test verifies namespace sovereignty -- write with non-owning key and no delegation rejected immediately, no data written to storage
- [x] **ACL-03**: Docker test verifies delegation write -- delegate can write to owner's namespace on any cluster node; delegate cannot delete (write-only)
- [x] **ACL-04**: Docker test verifies revocation propagation -- tombstoned delegation blob syncs to all peers, revoked delegate's subsequent writes immediately rejected
- [x] **ACL-05**: Docker test verifies SIGHUP ACL reload -- newly added key can connect without restart; removed key's active connection dropped

### Disaster Recovery & Forensics

- [x] **DR-01**: Docker test verifies DARE forensics -- hex inspection of data.mdb shows no plaintext blob content, namespace IDs, or public keys; all data high-entropy
- [x] **DR-02**: Docker test verifies master key dependency -- daemon refuses to open database without master.key
- [x] **DR-03**: Docker test verifies master key rotation (negative) -- Node B cannot read Node A's data.mdb using Node B's master.key
- [x] **DR-04**: Docker test verifies data directory migration -- full data_dir copy to new machine resumes operation (connections, blob serving, sync, cursor state)
- [x] **DR-05**: Docker test verifies crash recovery -- kill -9 during active sync, restart recovers without data loss, MDBX transactions intact, cursors allow resumption

### Network Resilience

- [x] **NET-01**: Docker test verifies eventual consistency -- 5-node mesh, network partition via iptables, heal, all nodes converge to identical blob set via XOR fingerprint verification
- [x] **NET-02**: Docker test verifies split-brain writes -- 4-node cluster partitioned into halves, different blobs written to each half, heal results in union of all blobs
- [x] **NET-03**: Docker test verifies large blob integrity -- blobs at 1K, 100K, 1M, 10M, 100M sizes synced across cluster, hash verified on each peer
- [x] **NET-04**: Docker test verifies sync cursor resumption -- node stopped/restarted syncs only new blobs (verified via wire traffic), not full re-sync
- [x] **NET-05**: Docker test verifies sync recovery after crash -- kill -9 during reconciliation, restart completes sync without manual intervention, no duplicates or missing blobs
- [x] **NET-06**: Docker test verifies late-joiner at scale -- new node catches up to 10,000 blobs across multiple namespaces, blob counts and fingerprints match

### Resource Exhaustion & DoS

- [x] **DOS-01**: Docker test verifies write rate limiting -- flooding peer disconnected, other peers continue operating normally
- [x] **DOS-02**: Docker test verifies sync rate limiting -- excess sync initiations rejected with SyncRejected, byte-rate accounting includes sync traffic
- [x] **DOS-03**: Docker test verifies concurrent session limit -- excess sync sessions rejected, existing sessions complete normally
- [x] **DOS-04**: Docker test verifies storage full signaling -- StorageFull broadcast to all peers, peers stop pushing data, accepts data again after space freed
- [x] **DOS-05**: Docker test verifies namespace quota enforcement -- writes rejected after quota hit, other namespaces unaffected
- [x] **DOS-06**: Docker test verifies thread pool under load -- saturated ML-DSA-87 verifications don't starve event loop; new connections accepted, health checks pass

### Operational Signaling

- [x] **OPS-01**: Docker test verifies SIGHUP config reload -- changed rate_limit_bytes_per_sec applies immediately; changed allowed_keys disconnects disallowed peers
- [x] **OPS-02**: Docker test verifies SIGUSR1 metrics dump -- output includes all expected fields (peers, blobs, storage, syncs, ingests, rejections, rate_limited, cursor_hits, cursor_misses, full_resyncs, uptime)
- [x] **OPS-03**: Docker test verifies SIGTERM graceful shutdown -- in-flight MDBX transactions complete/abort cleanly, restart shows no corruption

### Tombstone & TTL Lifecycle

- [ ] **TTL-01**: Docker test verifies tombstone propagation -- 100 blobs deleted via tombstones, all peers stop serving original data after sync
- [ ] **TTL-02**: Docker test verifies tombstone TTL expiry -- tombstones with TTL=60 expire and are GC'd, tombstone_map entries removed, storage decreases
- [ ] **TTL-03**: Docker test verifies TTL=0 permanent blobs -- blobs with TTL=0 never expired or collected regardless of age
- [ ] **TTL-04**: Docker test verifies delegate cannot delete -- delegate with valid delegation blob has tombstone rejected; only namespace owner can delete

### Set Reconciliation Protocol

- [x] **RECON-01**: Docker test verifies O(diff) scaling -- 10 new blobs on 10,000-blob namespace, wire traffic proportional to ~10 blobs not ~10,000
- [x] **RECON-02**: Docker test verifies empty namespace skip -- identical namespaces on two nodes, sync detects zero differences via cursor check, no ReconcileInit sent
- [x] **RECON-03**: Docker test verifies version byte forward compat -- ReconcileInit with unknown version byte rejected gracefully, no crash or state corruption
- [x] **RECON-04**: Docker test verifies large difference set -- Node A has 5000 blobs, Node B has 0, reconciliation transfers all 5000 with no duplicates or missed blobs

### E2E Messaging Primitives

- [x] **E2E-01**: Docker test verifies async message delivery -- blob written while recipient disconnected, recipient reconnects, receives blob via sync + pub/sub notification
- [ ] **E2E-02**: Docker test verifies history backfill -- relay node joins namespace after 1000 messages, reconciliation backfills all blobs with no gaps, sequence numbers monotonic
- [x] **E2E-03**: Docker test verifies delete for everyone -- tombstone propagates to all cluster nodes within one sync interval, no node serves original blob
- [x] **E2E-04**: Docker test verifies multi-namespace isolation -- two namespaces on same cluster, messages never cross namespaces, verified via fingerprint comparison

### Stress & Chaos

- [ ] **STRESS-01**: Docker test verifies long-running stability (4h) -- 3-node cluster, continuous ingest at 10 blobs/sec mixed sizes, memory bounded, blob counts consistent, no crashes
- [ ] **STRESS-02**: Docker test verifies peer churn -- 5-node cluster, random node kill/restart every 30s for 30 minutes, all nodes converge to identical blob set
- [ ] **STRESS-03**: Docker test verifies rapid namespace creation -- 1000 namespaces with 10 blobs each, all sync correctly, cursor storage bounded
- [ ] **STRESS-04**: Docker test verifies concurrent everything -- 4 nodes simultaneously ingesting, syncing, tombstoning, SIGHUP reloading, SIGUSR1 metrics; no deadlocks, crashes, or corruption

### Sanitizer & Security

- [x] **SAN-01**: Full Catch2 test suite passes under AddressSanitizer (ASAN) with zero findings
- [x] **SAN-02**: Full Catch2 test suite passes under ThreadSanitizer (TSAN) with zero data races
- [x] **SAN-03**: Full Catch2 test suite passes under UndefinedBehaviorSanitizer (UBSAN) with zero findings
- [ ] **SAN-04**: Docker test verifies protocol fuzzing -- malformed FlatBuffers, truncated frames, invalid crypto payloads, garbage bytes handled gracefully; no crashes or memory corruption
- [ ] **SAN-05**: Docker test verifies handshake fuzzing -- malformed messages at each PQ handshake stage cause clean disconnect, no crashes

### Connection Topology

- [x] **TOPO-01**: Docker test verifies duplicate-connection dedup -- two nodes configured as each other's peer, simultaneous connect produces exactly one logical connection, sync works correctly

### Bug Fixes

- [x] **FIX-01**: PEX test SIGSEGV (test_daemon.cpp:296) fixed -- coroutine lifetime during teardown resolved, "three nodes: peer discovery via PEX" test passes reliably

## Future Requirements

Deferred beyond v1.0.0.

### Advanced Testing

- **TEST-01**: CI/CD pipeline integration (GitHub Actions) for automated test runs on every PR
- **TEST-02**: Nightly stress/chaos test runs with trend tracking
- **TEST-03**: Coverage reporting and minimum coverage thresholds

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| New protocol features | Database layer feature-complete. v1.0.0 is testing/hardening only. |
| Performance optimization | v0.8.0 delivered +116% throughput. Optimize only if tests reveal regressions. |
| CI/CD pipeline setup | Test infrastructure is local Docker. CI integration is post-release. |
| Relay (Layer 2) development | Separate milestone after v1.0.0 open-source release. |
| Documentation rewrite | README/PROTOCOL.md already current as of v0.9.0. |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| SAN-01 | Phase 46 | Complete |
| SAN-02 | Phase 46 | Complete |
| SAN-03 | Phase 46 | Complete |
| FIX-01 | Phase 46 | Complete |
| CRYPT-01 | Phase 47 | Complete |
| CRYPT-02 | Phase 47 | Complete |
| CRYPT-03 | Phase 47 | Complete |
| CRYPT-04 | Phase 47 | Complete |
| CRYPT-05 | Phase 47 | Complete |
| CRYPT-06 | Phase 47 | Complete |
| ACL-01 | Phase 48 | Complete |
| ACL-02 | Phase 48 | Complete |
| ACL-03 | Phase 48 | Complete |
| ACL-04 | Phase 48 | Complete |
| ACL-05 | Phase 48 | Complete |
| TOPO-01 | Phase 48 | Complete |
| NET-01 | Phase 49 | Complete |
| NET-02 | Phase 49 | Complete |
| NET-03 | Phase 49 | Complete |
| NET-04 | Phase 49 | Complete |
| NET-05 | Phase 49 | Complete |
| NET-06 | Phase 49 | Complete |
| RECON-01 | Phase 49 | Complete |
| RECON-02 | Phase 49 | Complete |
| RECON-03 | Phase 49 | Complete |
| RECON-04 | Phase 49 | Complete |
| OPS-01 | Phase 50 | Complete |
| OPS-02 | Phase 50 | Complete |
| OPS-03 | Phase 50 | Complete |
| DR-01 | Phase 50 | Complete |
| DR-02 | Phase 50 | Complete |
| DR-03 | Phase 50 | Complete |
| DR-04 | Phase 50 | Complete |
| DR-05 | Phase 50 | Complete |
| DOS-01 | Phase 50 | Complete |
| DOS-02 | Phase 50 | Complete |
| DOS-03 | Phase 50 | Complete |
| DOS-04 | Phase 50 | Complete |
| DOS-05 | Phase 50 | Complete |
| DOS-06 | Phase 50 | Complete |
| TTL-01 | Phase 51 | Pending |
| TTL-02 | Phase 51 | Pending |
| TTL-03 | Phase 51 | Pending |
| TTL-04 | Phase 51 | Pending |
| E2E-01 | Phase 51 | Complete |
| E2E-02 | Phase 51 | Pending |
| E2E-03 | Phase 51 | Complete |
| E2E-04 | Phase 51 | Complete |
| STRESS-01 | Phase 52 | Pending |
| STRESS-02 | Phase 52 | Pending |
| STRESS-03 | Phase 52 | Pending |
| STRESS-04 | Phase 52 | Pending |
| SAN-04 | Phase 52 | Pending |
| SAN-05 | Phase 52 | Pending |

**Coverage:**
- v1.0.0 requirements: 54 total
- Mapped to phases: 54
- Unmapped: 0

---
*Requirements defined: 2026-03-20*
*Last updated: 2026-03-20 after roadmap creation*
