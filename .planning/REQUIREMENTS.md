# Requirements: chromatindb

**Defined:** 2026-03-19
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v0.9.0 Requirements

Requirements for Connection Resilience & Hardening milestone. Each maps to roadmap phases.

### Connection Resilience

- [x] **CONN-01**: Node auto-reconnects to all outbound peers on disconnect with exponential backoff (1s→60s) and jitter
- [x] **CONN-02**: Node suppresses reconnection attempts to peers that rejected the connection via ACL
- [ ] **CONN-03**: Node detects and disconnects dead peers via inactivity timeout (no messages received within deadline)

### Storage Hardening

- [x] **STOR-01**: Tombstone GC correctly reclaims storage — root cause identified and fixed (or documented if mmap behavior)
- [x] **STOR-02**: Node automatically prunes cursor entries for peers not seen within configurable age threshold
- [x] **STOR-03**: Node performs read-only integrity scan of all sub-databases at startup, logging any inconsistencies
- [ ] **STOR-04**: libmdbx crash recovery verified via kill-9 test scenarios with data integrity checks post-restart
- [ ] **STOR-05**: Delegate writes are correctly counted against the namespace owner's quota

### Operational

- [x] **OPS-01**: Version string injected by CMake at build time (version.h removed, no manual version bumps)
- [x] **OPS-02**: Node rejects invalid config at startup with human-readable error messages (ranges, types, formats)
- [x] **OPS-03**: All tracked metrics counters (rate_limited, peers_connected_total, peers_disconnected_total) emitted in periodic and SIGUSR1 log output
- [x] **OPS-04**: Log output available in structured JSON format for machine parsing
- [x] **OPS-05**: Node can log to rotating file in addition to stdout (configurable path, max size, max files)
- [x] **OPS-06**: All timers (expiry, sync, flush, metrics, pex) cancelled consistently in both stop() and on_shutdown paths

### Documentation

- [ ] **DOCS-01**: README updated with all v0.9.0 features (connection resilience, logging, config validation, storage hardening)
- [ ] **DOCS-02**: Protocol documentation current with v0.8.0 wire changes (reconciliation messages, rate limiting) and v0.9.0 keepalive behavior

## Future Requirements

Deferred to v1.0.0 (integration tests + hardening).

- **INT-01**: Docker-based integration test suite covering multi-node scenarios from db/TESTS.md
- **SAN-01**: ASAN/TSAN/UBSAN sanitizer passes with zero findings
- **REPL-01**: Replication count in WriteAck reflects actual peer replication (currently stubbed to 1)
- **SYNC-01**: max_sync_sessions > 1 support (replace bool syncing with counter)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Prometheus metrics endpoint | Requires HTTP server dependency, violates binary-protocol-only constraint |
| gRPC health check API | Same as above: HTTP dependency, attack surface |
| JSON Schema config validation | New dependency for 20 fields of range checking — hand-written is sufficient |
| Automatic config migration | No previous config format exists; config is pre-1.0 |
| Database compaction command | mmap databases reuse freed pages internally; file size is cosmetic |
| Hot config reload of logging/keepalive | SIGHUP already reloads ACL; restart for other config changes is acceptable pre-1.0 |
| Ping/Pong keepalive sender | Adds wire protocol complexity; receiver-side inactivity timeout is simpler and avoids AEAD nonce desync |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| CONN-01 | Phase 44 | Complete |
| CONN-02 | Phase 44 | Complete |
| CONN-03 | Phase 44 | Pending |
| STOR-01 | Phase 43 | Complete |
| STOR-02 | Phase 43 | Complete |
| STOR-03 | Phase 43 | Complete |
| STOR-04 | Phase 45 | Pending |
| STOR-05 | Phase 45 | Pending |
| OPS-01 | Phase 42 | Complete |
| OPS-02 | Phase 42 | Complete |
| OPS-03 | Phase 43 | Complete |
| OPS-04 | Phase 43 | Complete |
| OPS-05 | Phase 43 | Complete |
| OPS-06 | Phase 42 | Complete |
| DOCS-01 | Phase 45 | Pending |
| DOCS-02 | Phase 45 | Pending |

**Coverage:**
- v0.9.0 requirements: 16 total
- Mapped to phases: 16
- Unmapped: 0

---
*Requirements defined: 2026-03-19*
*Last updated: 2026-03-19 after roadmap creation*
