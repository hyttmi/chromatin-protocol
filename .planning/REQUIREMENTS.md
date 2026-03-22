# Requirements: chromatindb

**Defined:** 2026-03-22
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v1.1.0 Requirements

Requirements for v1.1.0 Operational Polish & Local Access. Each maps to roadmap phases.

### Compaction

- [ ] **COMP-01**: Node operator can trigger runtime mdbx compaction automatically for long-running nodes

### Local Access

- [ ] **UDS-01**: Local process can read/write blobs via Unix Domain Socket without TCP+PQ overhead

### Operational Hardening

- [x] **OPS-01**: Node operator can configure expiry scan interval via config field (replacing hardcoded 60s)
- [ ] **OPS-02**: Node rejects blobs with timestamps too far in the future or past on ingest
- [x] **OPS-03**: SyncRejected messages include human-readable reason strings for operator debugging

### Release & Cleanup

- [x] **REL-01**: Git repository has a v1.0.0 release tag on the shipped commit
- [x] **REL-02**: Stale bash tests (deploy/test-crash-recovery.sh) and design docs (db/TESTS.md) removed
- [x] **REL-03**: Stale .planning/milestones/v1.0.0-* deferred docs cleaned up
- [x] **REL-04**: CMake project version bumped to 1.1.0

### Documentation

- [x] **DOCS-01**: db/README.md reflects v1.0.0 state (sanitizers, 469 tests, Docker integration, stress/chaos/fuzz)
- [x] **DOCS-02**: README.md aligned with v1.0.0 shipped state
- [ ] **DOCS-03**: db/PROTOCOL.md updated with sync reject reason strings

## v2 Requirements

None — future features TBD after v1.1.0.

## Out of Scope

| Feature | Reason |
|---------|--------|
| HTTP/REST API | Adds attack surface and deps — binary protocol over PQ-encrypted TCP only |
| Per-blob encryption keys | Single HKDF-derived key per node is sufficient |
| Chunked/streaming blob transfer | Only necessary at 1+ GiB |
| Layer 2 (Relay) or Layer 3 (Client) | Future work, separate milestone |
| NAT traversal | Server daemon assumes reachable address |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| COMP-01 | Phase 55 | Pending |
| UDS-01 | Phase 56 | Pending |
| OPS-01 | Phase 54 | Complete |
| OPS-02 | Phase 54 | Pending |
| OPS-03 | Phase 54 | Complete |
| REL-01 | Phase 53 | Complete |
| REL-02 | Phase 53 | Complete |
| REL-03 | Phase 53 | Complete |
| REL-04 | Phase 53 | Complete |
| DOCS-01 | Phase 53 | Complete |
| DOCS-02 | Phase 53 | Complete |
| DOCS-03 | Phase 54 | Pending |

**Coverage:**
- v1.1.0 requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-03-22*
*Last updated: 2026-03-22 after roadmap creation*
