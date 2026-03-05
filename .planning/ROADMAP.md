# Roadmap: chromatindb

## Overview

chromatindb builds bottom-up along its dependency graph: crypto primitives and wire format first (leaf dependencies everything else needs), then the storage engine (libmdbx with all four sub-databases), then the blob engine (ingest pipeline testable without sockets), then the PQ-encrypted networking layer (Asio event loop + ML-KEM handshake + mutual auth), and finally the peer system (bootstrap discovery + hash-list diff sync). Each phase delivers a testable, independently verifiable layer. Phases 1-3 require no network. Phase 4 tests with loopback. Phase 5 integrates multi-node sync and delivers the running daemon.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [x] **Phase 1: Foundation** - Crypto primitives, wire format schemas, config, logging, and node identity
- [x] **Phase 2: Storage Engine** - libmdbx wrapper with all four sub-databases, batch writes, TTL expiry, and crash recovery
- [x] **Phase 3: Blob Engine** - Ingest pipeline (verify, dedup, store), query interface, and write ACKs
- [x] **Phase 4: Networking** - Asio event loop, PQ-encrypted transport with mutual auth, signal handling
- [x] **Phase 5: Peer System** - Bootstrap discovery, hash-list diff sync, and daemon integration
- [x] **Phase 6: Complete Sync Receive Side** - Wire up receive-side sync orchestration in PeerManager (gap closure)
- [ ] **Phase 7: Peer Discovery** - Peer exchange protocol so nodes discover peers beyond bootstrap (gap closure)
- [ ] **Phase 8: Verification & Cleanup** - Missing verification docs, traceability updates, dead code cleanup (gap closure)

## Phase Details

### Phase 1: Foundation
**Goal**: Node has all cryptographic primitives, a canonical wire format, configuration loading, structured logging, and can derive its identity (keypair + namespace)
**Depends on**: Nothing (first phase)
**Requirements**: CRYP-01, CRYP-02, CRYP-03, CRYP-04, WIRE-01, WIRE-02, NSPC-01, DAEM-01, DAEM-02
**Success Criteria** (what must be TRUE):
  1. Node generates an ML-DSA-87 keypair and derives its namespace as SHA3-256(pubkey), and the derivation is deterministic and reproducible
  2. Node performs ML-KEM-1024 encapsulation/decapsulation producing a shared secret, and AES-256-GCM encrypt/decrypt round-trips correctly using that secret
  3. A blob serialized to FlatBuffers, deserialized, and re-serialized produces identical bytes (canonicality verified), and the signed content (namespace || data || ttl || timestamp) is a fixed-size concatenation independent of FlatBuffer encoding
  4. Node loads configuration from a JSON file (bind address, storage path, bootstrap peers) and logs startup via spdlog. TTL is a protocol constant (7-day), not configurable.
**Plans**: 4 plans

Plans:
- [x] 01-01: CMake scaffold + crypto RAII wrappers (ML-DSA-87, ML-KEM-1024, SHA3-256, ChaCha20-Poly1305, HKDF-SHA256)
- [x] 01-02: FlatBuffers wire format + canonical signing codec
- [x] 01-03: Config loading, structured logging, and node identity
- [x] 01-04: Gap closure -- Make TTL a protocol constant (not user-configurable)

### Phase 2: Storage Engine
**Goal**: Node can persistently store, retrieve, deduplicate, index, and expire blobs using libmdbx with crash-safe ACID guarantees
**Depends on**: Phase 1
**Requirements**: STOR-01, STOR-02, STOR-03, STOR-04, STOR-05, STOR-06, DAEM-04
**Success Criteria** (what must be TRUE):
  1. A blob stored by namespace + SHA3-256 hash can be retrieved, and storing the same blob twice results in exactly one entry (content-addressed dedup)
  2. Each blob stored in a namespace receives a monotonically increasing seq_num, and blobs can be retrieved by namespace + seq_num range
  3. Blobs with elapsed TTL are automatically pruned by the background expiry scanner, while TTL=0 blobs persist indefinitely
  4. After a simulated crash (kill -9), the node restarts and all committed data is intact with no corruption (libmdbx ACID)
**Plans**: 3 plans

Plans:
- [x] 02-01: CMake + libmdbx + Storage class with store/retrieve/dedup + crash recovery
- [x] 02-02: Sequence indexing + range queries + expiry index verification
- [x] 02-03: TTL expiry scanner with injectable clock

### Phase 3: Blob Engine
**Goal**: Node can accept blobs from any source, verify namespace ownership and signature, deduplicate, assign sequence numbers, acknowledge writes, and answer queries -- all without network dependencies
**Depends on**: Phase 2
**Requirements**: NSPC-02, NSPC-03, QURY-01, QURY-02, ACKW-01
**Success Criteria** (what must be TRUE):
  1. A blob with SHA3-256(pubkey) != claimed namespace is rejected, and a blob with an invalid ML-DSA-87 signature is rejected, before any storage occurs
  2. A valid blob is ingested (deduped, seq_num assigned, stored) and the caller receives a write ACK confirming acceptance
  3. Querying "namespace X since seq_num Y" returns exactly the blobs stored in that namespace after seq_num Y, in order
  4. Querying "list all namespaces" returns every namespace that has at least one stored blob
**Plans**: 2 plans

Plans:
- [x] 03-01: Storage StoreResult extension + BlobEngine ingest pipeline with fail-fast validation and write ACKs
- [x] 03-02: BlobEngine query methods (get_blobs_since, list_namespaces, get_blob) with comprehensive tests

### Phase 4: Networking
**Goal**: Node can accept inbound and make outbound TCP connections over a PQ-encrypted, mutually authenticated channel, with async IO and graceful shutdown
**Depends on**: Phase 3
**Requirements**: TRNS-01, TRNS-02, TRNS-03, TRNS-04, TRNS-05, DAEM-03
**Success Criteria** (what must be TRUE):
  1. Two nodes complete an ML-KEM-1024 key exchange and establish an AES-256-GCM encrypted channel, and all subsequent messages are encrypted (no plaintext fallback)
  2. After key exchange, both nodes perform ML-DSA-87 mutual authentication by signing the session fingerprint, and a node with an invalid identity is rejected
  3. Node listens on a configurable bind address for inbound connections and initiates outbound connections to configured peer addresses
  4. Node handles SIGTERM/SIGINT by draining active connections and flushing storage before exiting cleanly
**Plans**: 3 plans

Plans:
- [x] 04-01: Asio + transport schema + encrypted framing layer
- [x] 04-02: PQ handshake with ML-KEM key exchange and ML-DSA mutual auth
- [x] 04-03: TCP connection lifecycle, server, and graceful shutdown

### Phase 5: Peer System
**Goal**: Nodes discover each other via bootstrap, synchronize their blob stores bidirectionally via hash-list diff, and operate as a running daemon
**Depends on**: Phase 4
**Requirements**: DISC-01, DISC-02, SYNC-01, SYNC-02, SYNC-03
**Success Criteria** (what must be TRUE):
  1. Node connects to configured bootstrap nodes on startup and receives a peer list, then connects to discovered peers
  2. Two nodes with different blob sets exchange hash lists and end up with the union of both sets (bidirectional sync)
  3. Expired blobs are not replicated during sync (dead data stays dead)
  4. A complete chromatindb daemon starts from config, joins the network, accepts blobs, replicates them to peers, and answers queries -- end to end
**Plans**: 3 plans

Plans:
- [x] 05-01: Sync protocol + hash-list diff + config extensions
- [x] 05-02: PeerManager with sync orchestration
- [x] 05-03: Daemon CLI with E2E tests

### Phase 6: Complete Sync Receive Side
**Goal**: PeerManager orchestrates the full sync loop — receive peer's namespace/hash lists, compute diff, request missing blobs, receive and ingest them — completing bidirectional sync
**Depends on**: Phase 5
**Requirements**: SYNC-01, SYNC-02, SYNC-03
**Gap Closure**: Closes sync receive-side gaps from audit
**Success Criteria** (what must be TRUE):
  1. PeerManager receives and parses peer's NamespaceList and HashList messages
  2. PeerManager calls SyncProtocol::diff_hashes() to identify missing blobs and sends BlobRequest
  3. PeerManager handles incoming BlobTransfer messages and calls SyncProtocol::ingest_blobs()
  4. Two nodes with different blob sets end up with the union of both sets after a sync cycle
**Plans**: 2 plans

Plans:
- [x] 06-01: Sync message queue + full bidirectional sync flow in PeerManager
- [x] 06-02: Strengthen E2E sync tests with strict blob verification

### Phase 7: Peer Discovery
**Goal**: Nodes discover peers beyond their bootstrap list via peer exchange, expanding network connectivity
**Depends on**: Phase 5
**Requirements**: DISC-02
**Gap Closure**: Closes peer discovery gap from audit
**Success Criteria** (what must be TRUE):
  1. After connecting to a bootstrap peer, node requests and receives a peer list
  2. Node connects to discovered peers that it doesn't already know about
  3. Node responds to peer list requests from other nodes

### Phase 8: Verification & Cleanup
**Goal**: Close all verification gaps, update traceability, and clean up tech debt identified by milestone audit
**Depends on**: Phase 6, Phase 7
**Requirements**: STOR-01, STOR-02, STOR-03, STOR-04, STOR-05, STOR-06, DAEM-04, DISC-01
**Gap Closure**: Closes verification gaps and tech debt from audit
**Success Criteria** (what must be TRUE):
  1. 02-VERIFICATION.md exists and confirms all Phase 2 requirements satisfied
  2. 05-VERIFICATION.md exists and confirms all Phase 5 requirements satisfied (including gap closure phases)
  3. Dead handshake code removed from connection.cpp (HandshakeInitiator hs2, orphaned API)
  4. REQUIREMENTS.md traceability table fully up-to-date with all phases complete

## Progress

**Execution Order:**
Phases execute in numeric order: 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 8

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Foundation | 4/4 | Complete | 2026-03-03 |
| 2. Storage Engine | 3/3 | Complete | 2026-03-03 |
| 3. Blob Engine | 2/2 | Complete | 2026-03-03 |
| 4. Networking | 3/3 | Complete | 2026-03-04 |
| 5. Peer System | 3/3 | Complete | 2026-03-04 |
| 6. Complete Sync Receive Side | 2/2 | Complete | 2026-03-05 |
| 7. Peer Discovery | 0/? | Pending | - |
| 8. Verification & Cleanup | 0/? | Pending | - |
