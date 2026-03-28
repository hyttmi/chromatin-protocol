# Requirements: chromatindb

**Defined:** 2026-03-28
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v1.5.0 Requirements

Requirements for v1.5.0 Documentation & Distribution. Each maps to roadmap phases.

### Distribution

- [x] **DIST-01**: dist/ contains hardened systemd unit file for chromatindb node (Type=simple, ProtectSystem=strict)
- [x] **DIST-02**: dist/ contains hardened systemd unit file for chromatindb_relay (Type=simple, ProtectSystem=strict)
- [x] **DIST-03**: dist/ contains default JSON config for chromatindb node with sane production defaults
- [x] **DIST-04**: dist/ contains default JSON config for chromatindb_relay with sane production defaults
- [x] **DIST-05**: dist/ contains sysusers.d config to create chromatindb system user/group
- [x] **DIST-06**: dist/ contains tmpfiles.d config for data, log, and config directories
- [x] **DIST-07**: dist/ contains install.sh that deploys all artifacts to FHS-standard locations

### Documentation

- [ ] **DOCS-01**: README.md updated with current test/LOC/type counts and v1.4.0 features
- [ ] **DOCS-02**: README.md includes relay deployment section
- [ ] **DOCS-03**: README.md includes dist/ deployment instructions
- [ ] **DOCS-04**: PROTOCOL.md verified against source for all 58 message types with correct byte offsets
- [ ] **DOCS-05**: db/README.md updated with current state

## Future Requirements

### Client SDK

- **SDK-01**: Python SDK for connecting to relay
- **SDK-02**: CLI tool for admin operations (quota check, list blobs, etc.)

### Performance

- **PERF-01**: Performance benchmarks for Relay layer

## Out of Scope

| Feature | Reason |
|---------|--------|
| .deb/.rpm packaging | Premature — project has minimal users; install.sh sufficient |
| logrotate config | spdlog handles rotation internally via rotating_file_sink; external logrotate causes log loss with copytruncate |
| sd_notify / Type=notify | Requires libsystemd dependency; Type=simple sufficient |
| Man pages | Docs-as-code in README/PROTOCOL.md is sufficient for current audience |
| Docker Compose production template | Existing benchmark compose works; production compose is a different concern |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| DIST-01 | Phase 68 | Complete |
| DIST-02 | Phase 68 | Complete |
| DIST-03 | Phase 68 | Complete |
| DIST-04 | Phase 68 | Complete |
| DIST-05 | Phase 68 | Complete |
| DIST-06 | Phase 68 | Complete |
| DIST-07 | Phase 68 | Complete |
| DOCS-01 | Phase 69 | Pending |
| DOCS-02 | Phase 69 | Pending |
| DOCS-03 | Phase 69 | Pending |
| DOCS-04 | Phase 69 | Pending |
| DOCS-05 | Phase 69 | Pending |

**Coverage:**
- v1.5.0 requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-03-28*
*Last updated: 2026-03-28 after roadmap creation*
