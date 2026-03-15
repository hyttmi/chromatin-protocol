# Requirements: chromatindb

**Defined:** 2026-03-14
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v0.5.0 Requirements

Requirements for v0.5.0 Hardening & Flexibility. Each maps to roadmap phases.

### Encryption at Rest

- [x] **EAR-01**: Blob payloads are encrypted with ChaCha20-Poly1305 before writing to libmdbx
- [x] **EAR-02**: Encryption key is derived from a node-local master key via HKDF-SHA256 (one key per node, not per blob)
- [x] **EAR-03**: Master key is stored in a separate file (not in the database) with restricted file permissions
- [x] **EAR-04**: Encrypted blobs are decrypted transparently on read — callers see plaintext

### Transport Optimization

- [x] **TOPT-01**: Connections from localhost (127.0.0.1/::1) or configured trusted_peers skip ML-KEM-1024 handshake
- [x] **TOPT-02**: Trusted peer connections use a pre-shared key or null-encryption mode with mutual identity verification
- [x] **TOPT-03**: trusted_peers is a config option (list of addresses) reloadable via SIGHUP

### TTL Flexibility

- [x] **TTL-01**: Blob TTL is set by the writer (included in signed blob data), not a hardcoded constant
- [x] **TTL-03**: TTL=0 remains valid and means permanent (no expiry)
- [x] **TTL-04**: Tombstone TTL is writer-controlled (set in signed tombstone data) — tombstones with TTL>0 expire naturally
- [x] **TTL-05**: Expired tombstones are garbage collected by the existing expiry scan, including tombstone_map cleanup

### Build & Documentation

- [x] **BUILD-01**: CMakeLists.txt restructured so db/ is a self-contained CMake component
- [ ] **DOC-05**: README updated to document DARE, trusted peers, configurable TTL, and tombstone expiry

## Future Requirements

### Deferred

- **PKI/Certificate-based auth** -- useful at 50+ nodes, current allowed_keys sufficient for now
- **Time-limited delegation** -- delegation blobs with TTL for auto-expiring trust
- **Multi-signature (M-of-N) blobs** -- no use case yet
- **Write-through ingestion** -- sync already propagates, failure window tiny
- **RBAC/ABAC** -- enterprise feature, not needed for censorship-resistant infra
- **HSM/TPM integration** -- aspirational, no hardware available
- **OIDC/SAML identity bridging** -- enterprise feature, wrong fit

## Out of Scope

| Feature | Reason |
|---------|--------|
| PKI/Certificate infrastructure | YAGNI — flat allowed_keys works at current scale |
| HSM/TPM key storage | Aspirational — no hardware, adds major dependency |
| OIDC/SAML identity bridging | Enterprise feature, wrong fit for censorship-resistant protocol |
| RBAC/ABAC authorization | Enterprise middleware, not database layer |
| Multi-signature blobs | No use case yet |
| Audit trail namespaces | Compliance logging contradicts censorship resistance |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| EAR-01 | Phase 24 | Complete |
| EAR-02 | Phase 24 | Complete |
| EAR-03 | Phase 24 | Complete |
| EAR-04 | Phase 24 | Complete |
| TOPT-01 | Phase 25 | Complete |
| TOPT-02 | Phase 25 | Complete |
| TOPT-03 | Phase 25 | Complete |
| TTL-01 | Phase 23 | Complete |
| TTL-03 | Phase 23 | Complete |
| TTL-04 | Phase 23 | Complete |
| TTL-05 | Phase 23 | Complete |
| BUILD-01 | Phase 22 | Complete |
| DOC-05 | Phase 26 | Pending |

**Coverage:**
- v0.5.0 requirements: 13 total
- Mapped to phases: 13
- Unmapped: 0

---
*Requirements defined: 2026-03-14*
*Last updated: 2026-03-15 after Phase 25 completion*
