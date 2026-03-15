# Roadmap: chromatindb

## Milestones

- ✅ **v1.0 MVP** — Phases 1-8 (shipped 2026-03-05)
- ✅ **v2.0 Closed Node Model** — Phases 9-11 (shipped 2026-03-07)
- ✅ **v3.0 Real-time & Delegation** — Phases 12-15 (shipped 2026-03-08)
- ✅ **v0.4.0 Production Readiness** — Phases 16-21 (shipped 2026-03-13)
- **v0.5.0 Hardening & Flexibility** — Phases 22-26 (in progress)

## Phases

<details>
<summary>✅ v1.0 MVP (Phases 1-8) — SHIPPED 2026-03-05</summary>

- [x] Phase 1: Foundation (4/4 plans) — completed 2026-03-03
- [x] Phase 2: Storage Engine (3/3 plans) — completed 2026-03-03
- [x] Phase 3: Blob Engine (2/2 plans) — completed 2026-03-03
- [x] Phase 4: Networking (3/3 plans) — completed 2026-03-04
- [x] Phase 5: Peer System (3/3 plans) — completed 2026-03-04
- [x] Phase 6: Complete Sync Receive Side (2/2 plans) — completed 2026-03-05
- [x] Phase 7: Peer Discovery (2/2 plans) — completed 2026-03-05
- [x] Phase 8: Verification & Cleanup (2/2 plans) — completed 2026-03-05

Full details: [milestones/v1.0-ROADMAP.md](milestones/v1.0-ROADMAP.md)

</details>

<details>
<summary>✅ v2.0 Closed Node Model (Phases 9-11) — SHIPPED 2026-03-07</summary>

- [x] Phase 9: Source Restructure (2/2 plans) — completed 2026-03-06
- [x] Phase 10: Access Control (3/3 plans) — completed 2026-03-06
- [x] Phase 11: Larger Blob Support (3/3 plans) — completed 2026-03-07

Full details: [milestones/v2.0-ROADMAP.md](milestones/v2.0-ROADMAP.md)

</details>

<details>
<summary>✅ v3.0 Real-time & Delegation (Phases 12-15) — SHIPPED 2026-03-08</summary>

- [x] Phase 12: Blob Deletion (2/2 plans) — completed 2026-03-07
- [x] Phase 13: Namespace Delegation (2/2 plans) — completed 2026-03-08
- [x] Phase 14: Pub/Sub Notifications (2/2 plans) — completed 2026-03-08
- [x] Phase 15: Polish & Benchmarks (2/2 plans) — completed 2026-03-08

Full details: [milestones/v3.0-ROADMAP.md](milestones/v3.0-ROADMAP.md)

</details>

<details>
<summary>✅ v0.4.0 Production Readiness (Phases 16-21) — SHIPPED 2026-03-13</summary>

- [x] Phase 16: Storage Foundation (3/3 plans) — completed 2026-03-10
- [x] Phase 17: Operational Stability (3/3 plans) — completed 2026-03-10
- [x] Phase 18: Abuse Prevention & Topology (3/3 plans) — completed 2026-03-12
- [x] Phase 19: Documentation & Release (2/2 plans) — completed 2026-03-12
- [x] Phase 20: Metrics Completeness & Consistency (1/1 plans) — completed 2026-03-13
- [x] Phase 21: Test 260 SEGFAULT Fix (1/1 plans) — completed 2026-03-13

</details>

### v0.5.0 Hardening & Flexibility

- [x] **Phase 22: Build Restructure** - CMakeLists.txt into db/ as self-contained CMake component (completed 2026-03-14)
- [x] **Phase 23: TTL Flexibility** - Remove hardcoded TTL constant, fix tombstone expiry scan (completed 2026-03-14)
- [x] **Phase 24: Encryption at Rest** - ChaCha20-Poly1305 encryption for all stored blob payloads (completed 2026-03-14)
- [x] **Phase 25: Transport Optimization** - Lightweight handshake for localhost and trusted peers (completed 2026-03-15)
- [x] **Phase 26: Documentation & Release** - README updates for all v0.5.0 features (completed 2026-03-15)

## Phase Details

### Phase 22: Build Restructure
**Goal**: db/ directory is a self-contained CMake component that can be built and tested independently
**Depends on**: Phase 21
**Requirements**: BUILD-01
**Success Criteria** (what must be TRUE):
  1. db/ contains its own CMakeLists.txt that declares a library target with all db sources
  2. The root CMakeLists.txt consumes db/ via add_subdirectory and the full project builds and links correctly
  3. All existing tests compile and pass without modification (zero regressions)
**Plans:** 1/1 plans complete
Plans:
- [ ] 22-01-PLAN.md — Move schemas to db/schemas/, create db/CMakeLists.txt, rewrite root to consume via add_subdirectory

### Phase 23: TTL Flexibility
**Goal**: Writers control blob lifetime via TTL in signed data (no hardcoded constant), and expired tombstones are garbage-collected
**Depends on**: Phase 22
**Requirements**: TTL-01, TTL-03, TTL-04, TTL-05
**Success Criteria** (what must be TRUE):
  1. BLOB_TTL_SECONDS constexpr no longer exists — writers set TTL in signed blob data, the node honors it
  2. A blob with TTL=0 is stored permanently and never expired (existing behavior preserved)
  3. Tombstones with TTL>0 are expired by the existing expiry scan and cleaned from all indexes including tombstone_map
  4. Tombstones with TTL=0 remain permanent (existing behavior preserved)
  5. Regular blob expiry is unaffected by the tombstone cleanup changes
**Plans:** 1/1 plans complete
Plans:
- [ ] 23-01-PLAN.md — Remove BLOB_TTL_SECONDS, fix run_expiry_scan to clean tombstone_map for expired tombstones

### Phase 24: Encryption at Rest
**Goal**: All blob payloads stored on disk are encrypted and decrypted transparently
**Depends on**: Phase 23
**Requirements**: EAR-01, EAR-02, EAR-03, EAR-04
**Success Criteria** (what must be TRUE):
  1. Blob data written to libmdbx is ChaCha20-Poly1305 encrypted (raw database reads show ciphertext, not plaintext)
  2. A node-local master key file exists with restricted permissions (0600) and is loaded at startup
  3. Per-blob encryption keys are derived from the master key via HKDF-SHA256 (not stored separately)
  4. Read operations return decrypted plaintext transparently -- callers (sync, query, pub/sub) see no difference
  5. A node started without a master key file auto-generates one on first run
**Plans:** 1/1 plans complete
Plans:
- [x] 24-01-PLAN.md — Create master key module and add ChaCha20-Poly1305 encryption to all Storage read/write paths

### Phase 25: Transport Optimization
**Goal**: Localhost and trusted peer connections complete handshake without PQ crypto overhead
**Depends on**: Phase 22
**Requirements**: TOPT-01, TOPT-02, TOPT-03
**Success Criteria** (what must be TRUE):
  1. A connection from 127.0.0.1 or ::1 completes handshake without ML-KEM-1024 key exchange
  2. Connections from addresses listed in trusted_peers config skip PQ handshake with mutual identity verification
  3. trusted_peers is reloadable via SIGHUP without restarting the node
  4. Non-trusted remote peers still perform the full PQ-encrypted handshake (no regression)
**Plans:** 2/2 plans complete
Plans:
- [x] 25-01-PLAN.md — Config infrastructure (trusted_peers parsing, validation, SIGHUP reload) and transport schema (TrustedHello, PQRequired)
- [x] 25-02-PLAN.md — Lightweight handshake implementation (key derivation, do_handshake branching, mismatch fallback, tests)

### Phase 26: Documentation & Release
**Goal**: Operators can understand and use all v0.5.0 features from the README
**Depends on**: Phase 23, Phase 24, Phase 25
**Requirements**: DOC-05
**Success Criteria** (what must be TRUE):
  1. README documents data-at-rest encryption: master key management, auto-generation, file permissions
  2. README documents trusted_peers config and localhost handshake behavior
  3. README documents configurable TTL: per-blob TTL in signed data, max_ttl operator config, tombstone_ttl
  4. version.h reports 0.5.0 and all tests pass
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 22 -> 23 -> 24 -> 25 -> 26
Note: Phase 25 depends only on Phase 22 (not 23/24), but ordered here for simplicity. Can be parallelized with 23/24 if desired.

| Phase | Milestone | Plans | Status | Completed |
|-------|-----------|-------|--------|-----------|
| 1. Foundation | v1.0 | 4/4 | Complete | 2026-03-03 |
| 2. Storage Engine | v1.0 | 3/3 | Complete | 2026-03-03 |
| 3. Blob Engine | v1.0 | 2/2 | Complete | 2026-03-03 |
| 4. Networking | v1.0 | 3/3 | Complete | 2026-03-04 |
| 5. Peer System | v1.0 | 3/3 | Complete | 2026-03-04 |
| 6. Complete Sync Receive Side | v1.0 | 2/2 | Complete | 2026-03-05 |
| 7. Peer Discovery | v1.0 | 2/2 | Complete | 2026-03-05 |
| 8. Verification & Cleanup | v1.0 | 2/2 | Complete | 2026-03-05 |
| 9. Source Restructure | v2.0 | 2/2 | Complete | 2026-03-06 |
| 10. Access Control | v2.0 | 3/3 | Complete | 2026-03-06 |
| 11. Larger Blob Support | v2.0 | 3/3 | Complete | 2026-03-07 |
| 12. Blob Deletion | v3.0 | 2/2 | Complete | 2026-03-07 |
| 13. Namespace Delegation | v3.0 | 2/2 | Complete | 2026-03-08 |
| 14. Pub/Sub Notifications | v3.0 | 2/2 | Complete | 2026-03-08 |
| 15. Polish & Benchmarks | v3.0 | 2/2 | Complete | 2026-03-08 |
| 16. Storage Foundation | v0.4.0 | 3/3 | Complete | 2026-03-10 |
| 17. Operational Stability | v0.4.0 | 3/3 | Complete | 2026-03-10 |
| 18. Abuse Prevention & Topology | v0.4.0 | 3/3 | Complete | 2026-03-12 |
| 19. Documentation & Release | v0.4.0 | 2/2 | Complete | 2026-03-12 |
| 20. Metrics Completeness & Consistency | v0.4.0 | 1/1 | Complete | 2026-03-13 |
| 21. Test 260 SEGFAULT Fix | v0.4.0 | 1/1 | Complete | 2026-03-13 |
| 22. Build Restructure | v0.5.0 | 1/1 | Complete | 2026-03-14 |
| 23. TTL Flexibility | v0.5.0 | 1/1 | Complete | 2026-03-14 |
| 24. Encryption at Rest | v0.5.0 | 1/1 | Complete | 2026-03-14 |
| 25. Transport Optimization | v0.5.0 | 2/2 | Complete | 2026-03-15 |
| 26. Documentation & Release | 1/1 | Complete    | 2026-03-15 | - |
