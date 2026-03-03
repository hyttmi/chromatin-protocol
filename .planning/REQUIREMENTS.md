# Requirements: chromatindb

**Defined:** 2026-03-03
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.

## v1 Requirements

Requirements for initial release (v0.1). Each maps to roadmap phases.

### Crypto

- [ ] **CRYP-01**: Node can generate ML-DSA-87 keypairs for signing/verification
- [ ] **CRYP-02**: Node can perform ML-KEM-1024 key encapsulation/decapsulation for PQ key exchange
- [ ] **CRYP-03**: Node can compute SHA3-256 hashes for namespace derivation and blob IDs
- [ ] **CRYP-04**: Node can perform ChaCha20-Poly1305 encryption/decryption for transport channel (via libsodium)

### Namespace

- [ ] **NSPC-01**: Namespace is derived as SHA3-256(pubkey) with no registration or authority
- [ ] **NSPC-02**: Node verifies SHA3-256(pubkey) == claimed namespace on every write, rejects mismatches
- [ ] **NSPC-03**: Node verifies ML-DSA-87 signature over (namespace || data || ttl || timestamp) on every write, rejects invalid

### Storage

- [ ] **STOR-01**: Node stores signed blobs in libmdbx keyed by namespace + SHA3-256 hash
- [ ] **STOR-02**: Node deduplicates blobs by content-addressed SHA3-256 hash
- [ ] **STOR-03**: Node maintains per-namespace monotonic sequence index (seq_num -> blob hash)
- [ ] **STOR-04**: Node maintains expiry index sorted by expiry timestamp
- [ ] **STOR-05**: Node automatically prunes expired blobs (TTL elapsed) via background scan
- [ ] **STOR-06**: Blobs have configurable TTL (default 7 days / 604800s, TTL=0 = permanent)

### Wire Format

- [ ] **WIRE-01**: All messages use FlatBuffers with deterministic encoding
- [ ] **WIRE-02**: Blob wire format includes: namespace (32B), pubkey (2592B), data (variable), ttl (u32), timestamp (u64), signature (4627B)

### Transport

- [ ] **TRNS-01**: Node-to-node connections use ML-KEM-1024 key exchange to establish shared secret
- [ ] **TRNS-02**: After key exchange, channel is encrypted with AES-256-GCM
- [ ] **TRNS-03**: Transport includes mutual authentication via ML-DSA-87 signed key exchange
- [ ] **TRNS-04**: Node listens for inbound TCP connections on configurable bind address
- [ ] **TRNS-05**: Node makes outbound TCP connections to configured peers

### Discovery

- [ ] **DISC-01**: Node connects to hardcoded/configured bootstrap nodes on startup
- [ ] **DISC-02**: Node receives peer lists from bootstrap nodes and connects to discovered peers

### Sync

- [ ] **SYNC-01**: Nodes exchange blob hash lists to identify missing blobs (hash-list diff)
- [ ] **SYNC-02**: Sync is bidirectional -- both nodes end up with the union of their data
- [ ] **SYNC-03**: Sync skips expired blobs (don't replicate dead data)

### Query

- [ ] **QURY-01**: Client can request "give me namespace X since seq_num Y" and receive matching blobs
- [ ] **QURY-02**: Client can request "list all namespaces" and receive namespace list

### ACK

- [ ] **ACKW-01**: Node acknowledges blob acceptance after local storage (write ACK)

### Daemon

- [ ] **DAEM-01**: Node reads configuration from JSON file (bind address, storage path, bootstrap peers, default TTL)
- [ ] **DAEM-02**: Node logs all operations via structured logging (spdlog)
- [ ] **DAEM-03**: Node handles signals for graceful shutdown (drain connections, flush storage)
- [ ] **DAEM-04**: Node recovers cleanly from crashes (libmdbx ACID guarantees)

## v2 Requirements

Deferred to future release. Tracked but not in current roadmap.

### Discovery v2

- **DISC-03**: Node learns about new peers from connected peers (peer exchange / PEX)

### Sync v2

- **SYNC-04**: Sync is resumable -- per-peer sync state persisted, resume from last position on reconnect
- **SYNC-05**: Sync uses XXH3 bucket fingerprints for faster negotiation before full hash-list exchange

### ACK v2

- **ACKW-02**: Write ACK includes replication count (how many peers have confirmed the blob)

### Operations v2

- **OPER-01**: Node enforces per-peer and per-namespace write rate limits
- **OPER-02**: Node enforces configurable maximum blob size
- **OPER-03**: Node exposes storage statistics (namespace count, blob count, storage size, expiry rate) via query interface

## Out of Scope

| Feature | Reason |
|---------|--------|
| Application semantics (messages, profiles, nicknames) | Layer 2 Relay concern -- database is intentionally dumb |
| Human-readable namespaces | Layer 2/3 concern -- requires naming authority or consensus |
| Client authentication | Layer 2 Relay concern |
| Message routing | Layer 2 Relay concern |
| Conflict resolution / LWW / HLC / CRDTs | Layer 2/app concern -- different apps need different strategies |
| Encrypted envelopes / E2E encryption | Layer 2/app concern -- node stores opaque blobs, transport is already PQ-encrypted |
| DHT or gossip protocol | Proven unreliable in chromatin-protocol and DNA messenger |
| Global consensus | No shared mutable state -- single-writer namespaces eliminate contention |
| Capability delegation / access grants | Layer 2 concern -- node auth is simply SHA3(pubkey) == namespace |
| Schema enforcement / typed data | Blobs are opaque by design |
| HTTP/REST API | Adds attack surface and deps -- binary protocol over PQ-encrypted TCP only |
| NAT traversal / hole punching | Server daemon assumes reachable address; outbound-only nodes still work |
| Sharding / partitioned storage | Organic via selective namespace replication (future) |
| OpenSSL | Prefer minimal deps -- liboqs for PQ + small audited AEAD+KDF lib for symmetric |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| CRYP-01 | Phase 1: Foundation | Pending |
| CRYP-02 | Phase 1: Foundation | Pending |
| CRYP-03 | Phase 1: Foundation | Pending |
| CRYP-04 | Phase 1: Foundation | Pending |
| WIRE-01 | Phase 1: Foundation | Pending |
| WIRE-02 | Phase 1: Foundation | Pending |
| NSPC-01 | Phase 1: Foundation | Pending |
| DAEM-01 | Phase 1: Foundation | Pending |
| DAEM-02 | Phase 1: Foundation | Pending |
| STOR-01 | Phase 2: Storage Engine | Pending |
| STOR-02 | Phase 2: Storage Engine | Pending |
| STOR-03 | Phase 2: Storage Engine | Pending |
| STOR-04 | Phase 2: Storage Engine | Pending |
| STOR-05 | Phase 2: Storage Engine | Pending |
| STOR-06 | Phase 2: Storage Engine | Pending |
| DAEM-04 | Phase 2: Storage Engine | Pending |
| NSPC-02 | Phase 3: Blob Engine | Pending |
| NSPC-03 | Phase 3: Blob Engine | Pending |
| QURY-01 | Phase 3: Blob Engine | Pending |
| QURY-02 | Phase 3: Blob Engine | Pending |
| ACKW-01 | Phase 3: Blob Engine | Pending |
| TRNS-01 | Phase 4: Networking | Pending |
| TRNS-02 | Phase 4: Networking | Pending |
| TRNS-03 | Phase 4: Networking | Pending |
| TRNS-04 | Phase 4: Networking | Pending |
| TRNS-05 | Phase 4: Networking | Pending |
| DAEM-03 | Phase 4: Networking | Pending |
| DISC-01 | Phase 5: Peer System | Pending |
| DISC-02 | Phase 5: Peer System | Pending |
| SYNC-01 | Phase 5: Peer System | Pending |
| SYNC-02 | Phase 5: Peer System | Pending |
| SYNC-03 | Phase 5: Peer System | Pending |

**Coverage:**
- v1 requirements: 32 total
- Mapped to phases: 32
- Unmapped: 0

---
*Requirements defined: 2026-03-03*
*Last updated: 2026-03-03 after roadmap creation*
