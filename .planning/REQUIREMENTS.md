# Requirements: v2.1.1 Revocation & Key Lifecycle

## ACL Revocation

- [ ] **REV-01**: Owner can revoke a delegate's write access via SDK `revoke_delegate()` method (tombstones the delegation blob)
- [ ] **REV-02**: Node rejects writes from revoked delegates immediately for connected peers and after sync propagation for disconnected peers

## Key Versioning

- [x] **KEY-01**: Owner can rotate KEM keypair via `Identity.rotate_kem()`; old secret keys retained in identity key ring for backward decryption
- [ ] **KEY-02**: Directory tracks key version history; UserEntry v2 includes `key_version` field; `resolve_recipient()` returns latest KEM public key
- [ ] **KEY-03**: `write_encrypted()` uses recipient's latest KEM public key; `read_encrypted()` falls back to older keys via `pk_hash` matching

## Group Membership

- [ ] **GRP-01**: Owner can remove a member from a group; future `write_to_group()` excludes removed member from recipient stanzas
- [ ] **GRP-02**: Group write forces directory cache refresh to ensure removed member's exclusion is reflected

## Documentation

- [ ] **DOC-01**: PROTOCOL.md documents revocation mechanism, key versioning format (UserEntry v2), and group membership revocation behavior
- [ ] **DOC-02**: SDK docs and getting-started tutorial updated with revocation, key rotation, and group membership management examples

## Future Requirements

- Re-encryption of old data on key rotation (deferred — only if compliance use case demands it)
- Permanent ban list / revocation list (deferred — tombstone-based revocation sufficient for now)
- Proxy re-encryption (explicitly rejected — unnecessary complexity)

## Out of Scope

- C++ node changes — delegation revocation already works via tombstones + delegation_map index removal
- New wire types — existing tombstone mechanism is sufficient
- Distributed revocation consensus — eventually-consistent propagation via existing sync is acceptable
- Node-enforced encryption — nodes store opaque blobs, encryption is client-side only

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| REV-01 | Phase 91 | Pending |
| REV-02 | Phase 91 | Pending |
| KEY-01 | Phase 92 | Complete |
| KEY-02 | Phase 92 | Pending |
| KEY-03 | Phase 92 | Pending |
| GRP-01 | Phase 93 | Pending |
| GRP-02 | Phase 93 | Pending |
| DOC-01 | Phase 94 | Pending |
| DOC-02 | Phase 94 | Pending |

**Coverage:**
- v2.1.1 requirements: 9 total
- Mapped to phases: 9/9 (100%)
- Unmapped: 0
