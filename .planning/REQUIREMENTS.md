# Requirements: chromatindb v1.7.0 Client-Side Encryption

**Defined:** 2026-03-31
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v1.7.0 Requirements

Requirements for client-side PQ envelope encryption. Each maps to roadmap phases.

### Identity

- [x] **IDENT-01**: User can generate an identity with both ML-DSA-87 signing and ML-KEM-1024 encryption keypairs
- [x] **IDENT-02**: User can save and load identity with .key/.pub (signing) and .kem/.kpub (encryption) files
- [x] **IDENT-03**: Identity exposes KEM public key for directory publishing and encryption operations

### Envelope Encryption

- [x] **ENV-01**: SDK encrypts blob data with a random per-blob DEK using ChaCha20-Poly1305
- [x] **ENV-02**: SDK wraps DEK per-recipient via ML-KEM-1024 encapsulation + HKDF-derived KEK
- [x] **ENV-03**: Encrypted blob uses versioned binary format (magic, version, suite, recipient stanzas, AEAD ciphertext)
- [x] **ENV-04**: Envelope header is authenticated as AEAD associated data (prevents stanza substitution)
- [x] **ENV-05**: Recipient can decrypt by finding their stanza (via KEM pubkey hash), decapsulating, and unwrapping DEK
- [x] **ENV-06**: HKDF uses unique domain label "chromatindb-envelope-kek-v1" for key wrapping derivation

### Directory

- [x] **DIR-01**: Admin can create an org directory backed by a namespace they own
- [x] **DIR-02**: User can self-register by publishing a signed UserEntry blob to the directory via delegation
- [x] **DIR-03**: UserEntry contains signing pubkey, KEM pubkey, display name, and ML-DSA-87 signature over KEM pubkey
- [x] **DIR-04**: User can list all registered users in a directory
- [x] **DIR-05**: User can fetch another user's KEM pubkey by display name or pubkey hash
- [x] **DIR-06**: SDK caches directory entries in memory and invalidates via pub/sub notifications

### Groups

- [x] **GRP-01**: Admin can create a named group in the directory with an initial member list
- [x] **GRP-02**: Admin can add or remove members from a group
- [x] **GRP-03**: User can list groups and view group membership
- [x] **GRP-04**: SDK resolves group name to member KEM pubkeys at encrypt-time

### Client Helpers

- [x] **CLI-01**: User can call write_encrypted(data, recipients) to encrypt and store a blob
- [x] **CLI-02**: User can call read_encrypted(blob_hash) to fetch, find stanza, decrypt and return plaintext
- [x] **CLI-03**: User can call write_to_group(data, group_name) to encrypt for all group members
- [x] **CLI-04**: User can call write_encrypted(data) with no recipients to encrypt to self only

### Documentation

- [ ] **DOC-01**: PROTOCOL.md updated with envelope format spec and HKDF label registry
- [ ] **DOC-02**: SDK README updated with encryption API section
- [ ] **DOC-03**: Getting started tutorial updated with encryption workflow example

## Future Requirements

Deferred to future release. Tracked but not in current roadmap.

### Streaming Encryption

- **STREAM-01**: SDK can encrypt blobs >10 MiB using chunked STREAM construction to avoid full in-memory buffering

### Re-encryption

- **REKEY-01**: User can re-encrypt existing blobs for a new recipient set (manual helper for post-revocation cleanup)

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Shared group symmetric key | Requires key rotation protocol on every membership change; per-blob envelope encryption is simpler and equally secure for a blob store |
| Key escrow / admin recovery | Destroys zero-knowledge property — if admin can decrypt, it's not client-side encryption |
| Convergent encryption / client-side dedup | Leaks plaintext equality, classic chosen-plaintext attack |
| Automatic re-encryption on group change | O(blobs x members), scales catastrophically; forward-looking revocation via ACL is sufficient |
| Proxy re-encryption | Academic technique, limited library support, unnecessary when client can re-encrypt directly |
| Certificate authority / PKI hierarchy | Self-certifying keys (SHA3-256(pubkey) = namespace) are sufficient; X.509 adds massive complexity |
| Forward secrecy per blob | Double-ratchet only makes sense for ordered message streams; per-blob random DEK provides key independence |
| Encrypted metadata / blob hashes | Node needs metadata for sync, expiry, queries; only blob data is encrypted (standard model) |
| Passphrase-based encryption | Identity is keypair-based; passphrase protection of secret key files is out of SDK scope |
| Backward-compatible identity loading | Pre-v1.7.0 identities without KEM keypairs not supported; generate new identity |
| C++ node changes | All encryption is pure SDK-side; node remains a zero-knowledge store |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| IDENT-01 | Phase 75 | Complete |
| IDENT-02 | Phase 75 | Complete |
| IDENT-03 | Phase 75 | Complete |
| ENV-01 | Phase 75 | Complete |
| ENV-02 | Phase 75 | Complete |
| ENV-03 | Phase 75 | Complete |
| ENV-04 | Phase 75 | Complete |
| ENV-05 | Phase 75 | Complete |
| ENV-06 | Phase 75 | Complete |
| DIR-01 | Phase 76 | Complete |
| DIR-02 | Phase 76 | Complete |
| DIR-03 | Phase 76 | Complete |
| DIR-04 | Phase 76 | Complete |
| DIR-05 | Phase 76 | Complete |
| DIR-06 | Phase 76 | Complete |
| GRP-01 | Phase 77 | Complete |
| GRP-02 | Phase 77 | Complete |
| GRP-03 | Phase 77 | Complete |
| GRP-04 | Phase 77 | Complete |
| CLI-01 | Phase 77 | Complete |
| CLI-02 | Phase 77 | Complete |
| CLI-03 | Phase 77 | Complete |
| CLI-04 | Phase 77 | Complete |
| DOC-01 | Phase 78 | Pending |
| DOC-02 | Phase 78 | Pending |
| DOC-03 | Phase 78 | Pending |

**Coverage:**
- v1.7.0 requirements: 26 total
- Mapped to phases: 26
- Unmapped: 0

---
*Requirements defined: 2026-03-31*
*Last updated: 2026-03-31 after roadmap creation*
