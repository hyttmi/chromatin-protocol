# Feature Research

**Domain:** Decentralized post-quantum secure database node (signed blob store with replication)
**Researched:** 2026-03-03 (revised from earlier libcpunkdb research, rescoped to chromatindb daemon)
**Confidence:** MEDIUM (based on training data analysis of IPFS, Hypercore, GunDB, etcd, CockroachDB, BitTorrent, Nostr relay implementations; no web verification available)

**IMPORTANT SCOPE NOTE:** This research replaces the earlier FEATURES.md which was written for "libcpunkdb" -- an embeddable library with HLC, LWW conflict resolution, capability grants, profile namespaces, and encrypted envelopes. The project scope has narrowed to **chromatindb: an intentionally dumb database node daemon**. Application semantics (conflict resolution, messaging, profiles, encrypted envelopes) live in the future Relay layer (Layer 2), NOT in the database node. This file is scoped to Layer 1 only.

## Feature Landscape

### Table Stakes (Users Expect These)

"Users" here means relay operators and developers building Layer 2 on top of chromatindb. A node that lacks any of these is fundamentally broken as a decentralized blob store.

| # | Feature | Why Expected | Complexity | Notes |
|---|---------|--------------|------------|-------|
| 1 | **Cryptographic namespace ownership** -- SHA3-256(pubkey) = namespace, verified on every write | Every comparable system (IPFS, Hypercore, Nostr) has some form of content/author verification. Without this, any peer can forge data and the entire trust model collapses | MEDIUM | Already designed. ML-DSA-87 sig check on every ingest. The pubkey is included in the blob so any node can verify independently |
| 2 | **Blob storage and retrieval** -- store opaque signed blobs keyed by namespace + hash | This IS the product. IPFS stores content-addressed blocks, Hypercore stores append-only entries, etcd stores KV pairs. A storage node that cannot reliably store and retrieve data is not a storage node | LOW | libmdbx with namespace+hash as key. Content-addressed via SHA3-256. Deduplication is free from content addressing |
| 3 | **Sequence index per namespace** -- monotonic seq_num for efficient polling | Every replication system needs a cursor mechanism. etcd has revision numbers, Kafka has offsets, Hypercore has seq numbers, Nostr relays track event timestamps. Without this, clients must re-fetch everything on every poll | LOW | Per-namespace counter assigned by receiving node. Enables "give me everything since seq N" queries |
| 4 | **TTL and automatic expiry** -- blobs expire after TTL seconds, automatic pruning | Without expiry, append-only storage grows unboundedly. IPFS requires manual garbage collection + pinning. Hypercore never deletes. This is a known pain point in every persistent P2P system. TTL makes the system self-cleaning | MEDIUM | 7-day default, TTL=0 for permanent. Expiry index in libmdbx sorted by expiry timestamp. Background pruning thread |
| 5 | **Peer discovery** -- bootstrap nodes + peer exchange | Every P2P system needs initial peer discovery. BitTorrent uses trackers + DHT + PEX, IPFS uses bootstrap + DHT, Hypercore uses DHT. Bootstrap + peer exchange is the simplest viable approach (no DHT complexity) | MEDIUM | Bootstrap list in config. Active peers exchanged periodically. Explicit design choice: no DHT (proven unreliable in previous projects) |
| 6 | **Node-to-node sync** -- hash-list diff, bidirectional | The core replication mechanism. Without sync, nodes are isolated storage silos. IPFS uses Bitswap, Hypercore uses Merkle tree exchange, CockroachDB uses Raft log replication. Hash-list diff is the simplest correct approach | HIGH | Compare namespace hash-lists between peers, exchange missing blobs. Must handle: partial sync, resumption, TTL-expired blob exclusion. This is the most complex table-stakes feature |
| 7 | **PQ-encrypted transport** -- ML-KEM-1024 key exchange + AES-256-GCM channel | Transport encryption is table stakes for any network daemon in 2026. TLS is the baseline; PQ-encryption is the project's raison d'etre. Without this, the "PQ-secure" claim is hollow | HIGH | Full handshake protocol: ML-KEM-1024 key encapsulation, derive AES-256-GCM session key, encrypted framing. Proven patterns from PQCC project |
| 8 | **Write acknowledgement with replication count** -- confirm write, report how many nodes have it | Clients need confirmation that data was accepted and is being replicated. etcd returns write confirmation with quorum status. Without ACKs, writers are blind to whether their data persists | LOW | Immediate ACK on local store, async replication count updates. Simple: "stored locally, replicated to N peers" |
| 9 | **Query interface** -- "give me namespace X since seq Y", "list namespaces" | The read API. Without a query interface, stored data is inaccessible. IPFS has the gateway API, etcd has range queries, Nostr relays have subscription filters. The query set is intentionally minimal (namespace + seq range) | LOW | Two primary queries: (1) fetch blobs by namespace since seq_num, (2) list known namespaces. No rich query language -- the database is intentionally dumb |
| 10 | **Wire format with deterministic encoding** -- FlatBuffers for all messages | Blobs must be signed over a deterministic byte representation. If encoding is non-deterministic, the same logical blob produces different bytes and signatures fail to verify on other nodes. Protocol Buffers are explicitly ruled out for this reason | MEDIUM | FlatBuffers 25.12.19 with deterministic encoding. Wire format covers: blob submission, sync messages, query requests/responses, peer exchange |
| 11 | **Graceful shutdown and crash recovery** -- clean state on restart | Any daemon must survive crashes without data corruption. libmdbx provides ACID transactions and crash safety, but the daemon must handle in-flight operations, partial syncs, and clean connection teardown | LOW | libmdbx handles storage crash safety. Daemon needs signal handling, connection draining, sync state persistence |
| 12 | **Logging and observability** -- structured logging, basic health metrics | Operators need to know what their node is doing. Every production daemon (etcd, CockroachDB, IPFS) ships with logging and metrics. Without this, debugging production issues is impossible | LOW | spdlog for structured logging. At minimum: peer connections, sync events, storage stats, error rates. Metrics endpoint deferred but log everything |
| 13 | **Configuration** -- bootstrap peers, storage path, bind address, TTL defaults | A daemon must be configurable. Hard-coded values are unacceptable for anything beyond a prototype | LOW | JSON config file (nlohmann/json). CLI flags for overrides. Sensible defaults for everything |

### Differentiators (Competitive Advantage)

Features that set chromatindb apart from comparable systems. These are not expected, but they create real value.

| # | Feature | Value Proposition | Complexity | Notes |
|---|---------|-------------------|------------|-------|
| 1 | **Post-quantum cryptography throughout** -- ML-DSA-87 signing, ML-KEM-1024 transport, SHA3-256 hashing | No other decentralized storage system uses PQ crypto. IPFS uses Ed25519/RSA, Hypercore uses Ed25519, Nostr uses secp256k1. chromatindb is future-proof against quantum attacks. This is THE differentiator | Already scoped in table stakes | This is baked into the system at every level, not a bolt-on. The PQ overhead (large signatures: 4627 bytes; large pubkeys: 2592 bytes) is a deliberate tradeoff for quantum resistance |
| 2 | **Ephemeral-by-default data model** -- TTL on all blobs | Every comparable system assumes data permanence. IPFS pins forever, Hypercore appends forever, CockroachDB stores forever. chromatindb's TTL-first design means storage is naturally bounded (write_rate x avg_TTL), nodes self-clean, and privacy improves (data doesn't persist indefinitely) | Already scoped in table stakes | Unique among decentralized storage. The "permanent" option (TTL=0) exists but is opt-in, not default |
| 3 | **Intentionally dumb storage** -- no application semantics in the node | Most comparable systems leak application concerns into the storage layer: IPFS has IPNS/IPLD/DAGs, Hypercore has Hyperbee/Hyperdrive, GunDB has SEA/user system. chromatindb stores signed opaque blobs and nothing else. This makes it a clean foundation for any application layer | LOW (it's the absence of complexity) | The discipline is in what you do NOT build. The node has no knowledge of what blobs contain. This is a feature, not a limitation |
| 4 | **Cryptographic namespace isolation** -- namespaces are mathematically derived, not registered | IPFS uses content addressing (no ownership), Nostr events are signed but stored globally (no namespace isolation), etcd has no namespace concept. chromatindb's SHA3-256(pubkey) namespace model means: no registration authority, no name collisions, cryptographic proof of ownership, and natural data partitioning for sync | Already scoped in table stakes | The elegance is that namespace = hash(pubkey) requires zero coordination between nodes. Any keypair creates a namespace immediately |
| 5 | **Resumable bidirectional sync** -- pick up where you left off | Many P2P systems require full re-sync on reconnect (GunDB, older IPFS Bitswap). Per-peer sync state persistence means reconnecting peers only exchange what changed since last sync. Critical for bandwidth-constrained or intermittent connections | MEDIUM | Requires persisting per-peer sync progress (which namespace, which seq_num). Adds complexity to sync engine but massive efficiency gain |
| 6 | **Simple peer model (no DHT)** -- bootstrap + peer exchange only | IPFS's Kademlia DHT is a major source of complexity and unreliability. BitTorrent's DHT works but adds enormous code. chromatindb's bootstrap + PEX model is radically simpler: connect to known peers, learn about new peers from them. Less resilient than DHT in theory, much more reliable in practice for small-to-medium networks | Already scoped in table stakes | Proven lesson from previous projects. DHT is an anti-feature (see below) |

### Anti-Features (Deliberately NOT Building)

These are features that seem valuable but would damage the project. Each is a conscious decision informed by previous project failures or analysis of comparable systems.

| # | Anti-Feature | Why Requested | Why Problematic | What to Do Instead |
|---|-------------|---------------|-----------------|-------------------|
| 1 | **DHT (Distributed Hash Table)** | Standard approach for P2P peer/content discovery (IPFS Kademlia, BitTorrent Mainline DHT) | Proven unreliable in previous projects (chromatin-protocol, DNA messenger). Requires routing tables, churn handling, NAT traversal, iterative lookups. Adds 5-10x complexity to peer layer. Small networks (<1000 nodes) don't benefit | Bootstrap nodes + peer exchange. Simpler, more reliable for target scale. If the network grows to need DHT, it can be added later without protocol changes |
| 2 | **Application semantics** -- messages, profiles, nicknames, user accounts | Seems natural to add "just a little" app logic to the storage layer | Violates separation of concerns. Every app-level feature in the node makes it harder to use for different applications. GunDB's SEA user system, IPFS's IPLD, OrbitDB's access controllers all couple storage to specific use cases | Keep the node dumb. Application semantics belong in Layer 2 (Relay). The node stores blobs. Period |
| 3 | **Conflict resolution / CRDT / LWW** | Multiple writers to same key need resolution | This is application-layer logic. Different applications need different resolution strategies (LWW, CRDT merge, manual resolution). Baking one strategy into the node limits all applications. The node stores ALL blobs; the application decides what they mean | Store all signed blobs. Let Layer 2 implement whatever conflict resolution it needs. The node has no concept of "conflicting" blobs |
| 4 | **Human-readable namespaces** | Users want "alice" not "a3f8b2c1..." | Requires a naming authority or consensus mechanism (both add massive complexity). Petname/Zooko's triangle is unsolvable at the storage layer | Namespaces are SHA3-256(pubkey). Human-readable mapping is Layer 2/3 concern. Could be implemented as a well-known namespace that maps names to pubkeys |
| 5 | **Rich query language** -- SQL, GraphQL, range queries on blob contents | Operators and developers want to query data | Blobs are opaque. The node has no schema knowledge. Adding a query language means: parsing blob contents (breaks opacity), indexing (storage overhead), schema management (complexity). This is exactly how scope creep kills projects | Two queries: (1) namespace X since seq Y, (2) list namespaces. Applications that need rich queries should build their own indexes in Layer 2 |
| 6 | **Encrypted envelopes / E2E encryption** | Privacy: relay should not see blob contents | This is application-layer encryption. The node stores opaque bytes -- it already cannot interpret contents. Layer 2 can encrypt blob payloads before submitting to the node. Building envelope crypto into the node couples it to specific key exchange patterns | Transport is PQ-encrypted (ML-KEM-1024 + AES-256-GCM). Payload encryption is Layer 2's job. The node never looks inside blobs |
| 7 | **Global consensus** | Consistency guarantees across the network | No shared mutable state to agree on. Each namespace is independently owned by one keypair. CockroachDB/etcd need consensus because multiple writers contend on shared state. chromatindb has single-writer namespaces (the owner's keypair) -- no contention, no consensus needed | Per-namespace ownership via cryptographic proof. Replication is eventual, not strongly consistent. This is simpler AND more correct for the use case |
| 8 | **Capability delegation / access grants** | Let other keys write to your namespace | Adds complexity to the authorization layer: grant format, revocation, timing issues, chain-of-trust validation. This is application semantics. The node's auth model is simple: SHA3(pubkey) == namespace means you can write. That's it | Capability delegation is a Layer 2 feature. If a relay wants to let multiple keys write to a logical "space," it can manage that in its own namespace and the application layer interprets it |
| 9 | **Schema enforcement / typed data** | Ensure blobs conform to expected structure | Blobs are opaque by design. Schema enforcement means the node must understand blob contents, which breaks the entire "intentionally dumb" architecture. Different applications will have different schemas | Blobs are bytes. Applications validate their own data. The node verifies signatures, not contents |
| 10 | **Built-in HTTP/REST API** | Easy integration for web applications | HTTP adds a dependency (HTTP server library), attack surface (HTTP parsing vulnerabilities), and design constraints (request/response doesn't fit streaming sync well). The daemon speaks its own binary protocol over PQ-encrypted TCP | The wire protocol is FlatBuffers over PQ-encrypted TCP. If HTTP access is needed, a separate proxy/gateway (potentially part of Layer 2 Relay) can translate. Keep the node protocol clean |
| 11 | **Automatic NAT traversal / hole punching** | Nodes behind NATs can't receive connections | NAT traversal (STUN/TURN/ICE) is enormously complex and unreliable. IPFS spends significant effort on this. For a server daemon, the assumption is: you have a reachable address, or you connect outbound to bootstrap nodes | Nodes that can't accept inbound connections still work: they connect outbound to peers and sync bidirectionally over those connections. Relay nodes with public IPs serve as meeting points |
| 12 | **Sharding / partitioned storage** | Scale beyond single-node capacity | Sharding adds massive complexity: partition assignment, rebalancing, routing, partial failures. chromatindb nodes store what they choose to replicate. Selective replication by namespace IS the partitioning strategy, emergent rather than coordinated | Nodes choose which namespaces to replicate. Popular namespaces end up on more nodes. This is organic partitioning without coordination overhead |

## Feature Dependencies (Build Order)

```
[PQ Crypto Layer] (ML-DSA-87, ML-KEM-1024, SHA3-256, AES-256-GCM)
    |
    +---> [Blob Format] (FlatBuffers schema, signing, verification)
    |         |
    |         +---> [Storage Engine] (libmdbx, namespace+hash keys, seq index, expiry index)
    |         |         |
    |         |         +---> [TTL Pruning] (background expiry scanner)
    |         |         |
    |         |         +---> [Query Interface] (namespace+seq queries, namespace listing)
    |         |
    |         +---> [Signature Verification] (verify-on-ingest pipeline)
    |
    +---> [PQ Transport] (ML-KEM-1024 handshake, AES-256-GCM framing)
              |
              +---> [Peer Connections] (TCP listener, outbound connections)
                        |
                        +---> [Peer Discovery] (bootstrap, peer exchange)
                        |
                        +---> [Sync Engine] (hash-list diff, bidirectional, resumable)
                        |         |
                        |         +---> [Write ACKs] (local + replication count)
                        |
                        +---> [Wire Protocol] (FlatBuffers messages over PQ-encrypted TCP)

[Configuration] (JSON config, CLI flags) -- independent, needed by everything
[Logging] (spdlog) -- independent, needed by everything
[Daemon Lifecycle] (signal handling, graceful shutdown) -- independent, wraps everything
```

### Dependency Notes

- **PQ Crypto before everything:** Every component depends on hashing (SHA3-256), signing (ML-DSA-87), or encryption (ML-KEM-1024/AES-256-GCM). This is the absolute foundation.
- **Blob Format before Storage:** The storage schema (key layout, indexes) is derived from the blob format. Changing the blob format after data is stored is a breaking change.
- **PQ Transport before Peer Connections:** All node-to-node communication is encrypted. No plaintext mode.
- **Storage before Sync:** The sync engine reads from and writes to the local store. It needs the storage engine operational.
- **Peer Discovery before Sync:** You need peers before you can sync with them.
- **Sync before Write ACKs with replication count:** Replication count comes from sync confirmations.
- **Configuration and Logging are cross-cutting:** Needed by every component from day one.

## MVP Definition

### Launch With (v0.1)

Minimum viable database node -- what's needed to validate the concept of a working PQ-secure decentralized blob store.

- [ ] **PQ Crypto layer** -- ML-DSA-87 sign/verify, SHA3-256, ML-KEM-1024 encaps/decaps, AES-256-GCM. Reuse patterns from PQCC project
- [ ] **Blob format** -- FlatBuffers schema with: namespace, pubkey, data, ttl, timestamp, signature. Deterministic encoding for signing
- [ ] **Signature verification on ingest** -- reject blobs where SHA3(pubkey) != namespace or signature is invalid
- [ ] **Storage engine** -- libmdbx with blob storage (namespace+hash key), sequence index (namespace+seq -> hash), expiry index
- [ ] **TTL expiry** -- background thread scans expiry index, removes expired blobs
- [ ] **Query interface** -- fetch blobs by namespace since seq_num, list known namespaces
- [ ] **PQ-encrypted transport** -- ML-KEM-1024 handshake, AES-256-GCM session, encrypted framing
- [ ] **Peer connections** -- TCP listener for inbound, outbound connections to configured peers
- [ ] **Bootstrap discovery** -- connect to configured bootstrap nodes on startup
- [ ] **Basic sync** -- hash-list diff between connected peers, exchange missing blobs
- [ ] **Write ACK** -- acknowledge blob acceptance (local store confirmation)
- [ ] **Configuration** -- JSON config file for: bind address, storage path, bootstrap peers, default TTL
- [ ] **Logging** -- spdlog structured logging for all operations
- [ ] **Daemon lifecycle** -- signal handling, graceful shutdown, crash recovery via libmdbx ACID

### Add After Validation (v0.2)

Features to add once the core is working and validated with real usage.

- [ ] **Peer exchange (PEX)** -- learn about new peers from connected peers (beyond just bootstrap)
- [ ] **Resumable sync** -- persist per-peer sync state, resume from last known position on reconnect
- [ ] **Write ACK with replication count** -- track and report how many peers have confirmed a blob
- [ ] **Sync fingerprinting** -- xxHash (XXH3) bucket fingerprints for faster sync negotiation before full hash-list exchange
- [ ] **Storage statistics** -- namespace count, blob count, storage size, expiry rate. Expose via query interface
- [ ] **Rate limiting** -- per-peer and per-namespace write rate limits to prevent abuse
- [ ] **Blob size limits** -- configurable maximum blob size, reject oversized writes

### Future Consideration (v1.0+)

Features to defer until the system is proven in production.

- [ ] **Negentropy set reconciliation** -- upgrade hash-list diff to O(diff) sync. Evaluate C++ availability first
- [ ] **Selective namespace replication** -- configure which namespaces a node stores (currently: store everything you receive)
- [ ] **Admin interface** -- runtime peer management, storage inspection, config reload without restart
- [ ] **Metrics endpoint** -- Prometheus-style metrics export for production monitoring
- [ ] **Pluggable storage backends** -- abstract storage interface beyond libmdbx (for testing, alternative deployments)
- [ ] **Multi-listener** -- bind to multiple addresses/ports simultaneously

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority | Phase |
|---------|------------|---------------------|----------|-------|
| PQ Crypto layer | HIGH | MEDIUM (reuse from PQCC) | P1 | v0.1 |
| Blob format (FlatBuffers) | HIGH | MEDIUM | P1 | v0.1 |
| Signature verification | HIGH | LOW | P1 | v0.1 |
| Storage engine (libmdbx) | HIGH | MEDIUM | P1 | v0.1 |
| TTL expiry | HIGH | LOW | P1 | v0.1 |
| Query interface | HIGH | LOW | P1 | v0.1 |
| PQ-encrypted transport | HIGH | HIGH | P1 | v0.1 |
| Peer connections (TCP) | HIGH | MEDIUM | P1 | v0.1 |
| Bootstrap discovery | HIGH | LOW | P1 | v0.1 |
| Basic sync (hash-list diff) | HIGH | HIGH | P1 | v0.1 |
| Write ACK (local) | MEDIUM | LOW | P1 | v0.1 |
| Configuration (JSON) | MEDIUM | LOW | P1 | v0.1 |
| Logging (spdlog) | MEDIUM | LOW | P1 | v0.1 |
| Daemon lifecycle | MEDIUM | LOW | P1 | v0.1 |
| Peer exchange (PEX) | MEDIUM | MEDIUM | P2 | v0.2 |
| Resumable sync | MEDIUM | MEDIUM | P2 | v0.2 |
| Replication count ACK | LOW | MEDIUM | P2 | v0.2 |
| Sync fingerprinting (XXH3) | MEDIUM | MEDIUM | P2 | v0.2 |
| Storage statistics | LOW | LOW | P2 | v0.2 |
| Rate limiting | MEDIUM | LOW | P2 | v0.2 |
| Blob size limits | MEDIUM | LOW | P2 | v0.2 |
| Negentropy sync | MEDIUM | HIGH | P3 | v1.0+ |
| Selective namespace replication | MEDIUM | MEDIUM | P3 | v1.0+ |
| Admin interface | LOW | MEDIUM | P3 | v1.0+ |
| Metrics endpoint | LOW | MEDIUM | P3 | v1.0+ |

## Competitor Feature Analysis

Analysis of how comparable systems handle the features chromatindb needs.

| Feature | IPFS | Hypercore | GunDB | Nostr Relays | etcd | BitTorrent | chromatindb |
|---------|------|-----------|-------|--------------|------|------------|-------------|
| **Data model** | Content-addressed blocks (DAG) | Append-only log per feed | JSON graph | Signed events | Ordered KV pairs | Content-addressed pieces | Signed blobs in namespaces |
| **Identity/ownership** | PeerID (Ed25519/RSA) | Feed keypair (Ed25519) | SEA user system | secp256k1 pubkey | None (cluster auth) | None | ML-DSA-87 pubkey -> namespace |
| **Write verification** | Content hash check | Signature per entry | SEA signature | Signature per event | Raft consensus | Piece hash check | ML-DSA-87 sig + namespace check |
| **Data expiry** | Manual GC + pinning | Never (append-only) | Never (persistent) | Relay-specific policy | TTL per key | Seeding-dependent | TTL on every blob (7d default) |
| **Peer discovery** | Bootstrap + Kademlia DHT | Bootstrap + DHT | WebRTC signaling | Hardcoded relay URLs | Static cluster config | Tracker + DHT + PEX | Bootstrap + PEX |
| **Sync/replication** | Bitswap (want/have) | Merkle tree diff | HAM gossip | REQ/EVENT subscription | Raft log replication | Piece exchange | Hash-list diff |
| **Transport encryption** | TLS 1.3 / Noise | Noise protocol | None (WebRTC DTLS) | WSS (TLS) | TLS | None standard | ML-KEM-1024 + AES-256-GCM |
| **Query model** | CID lookup, IPLD selectors | seq range per feed | Graph traversal | Filter subscriptions | Range queries on keys | Piece/file request | Namespace + seq range |
| **Storage backend** | Badger/FlatFS | Random-access-storage | RAD (radix tree) | Varies (SQLite, etc) | bbolt (B+ tree) | Filesystem | libmdbx |
| **Wire format** | Protobuf + CBOR | Custom binary | JSON | JSON (NIP-01) | Protobuf (gRPC) | Bencoded | FlatBuffers |

### Key Lessons from Comparable Systems

**From IPFS:**
- Content addressing is proven and reliable (adopt: SHA3-256 blob hashing)
- DHT is the biggest source of complexity and reliability issues (avoid: no DHT)
- Manual garbage collection + pinning is a terrible UX (adopt: automatic TTL expiry)
- Bitswap's want/have protocol is effective for exchange (inform: hash-list diff design)
- Multiple storage backends create maintenance burden (avoid: libmdbx only)

**From Hypercore:**
- Per-feed append-only logs with sequence numbers work well (adopt: per-namespace seq_num)
- Merkle tree verification provides integrity guarantees (consider: not needed when every blob is individually signed)
- Never-delete policy causes unbounded growth (avoid: TTL-first design)
- Sparse replication (only sync what you need) is valuable (future: selective namespace replication)

**From GunDB:**
- HAM (Hypothetical Amnesia Machine) conflict resolution is clever but complex (avoid: no conflict resolution in node)
- WebRTC peer-to-peer works but is unreliable for server daemons (avoid: use TCP)
- Offline-first with eventual sync is a good model (adopt: local-first with peer sync)

**From Nostr Relays:**
- Dumb relay model works: relays store events, clients interpret them (adopt: intentionally dumb node)
- Subscription-based querying (REQ with filters) is effective (inform: query interface design)
- No built-in replication between relays is a weakness (differentiate: built-in peer sync)
- WebSocket-only transport limits deployment options (avoid: binary protocol over TCP)
- NIP-77 Negentropy sync is the state of the art for set reconciliation (future: evaluate for v1.0+)

**From etcd:**
- Write acknowledgement with quorum status is valuable (adopt: write ACK with replication count)
- TTL/lease mechanism for key expiry is well-proven (adopt: TTL model)
- Watch/subscribe for changes is useful (future: could add namespace change notifications)

**From BitTorrent:**
- PEX (Peer Exchange) is simple and effective (adopt: for v0.2)
- Tracker + DHT + PEX layered discovery is resilient (adopt simplified: bootstrap + PEX)
- Content-addressed pieces with hash verification work at massive scale (validates: content-addressed blob model)

## Sources

- IPFS documentation and architecture (ipfs.tech/docs) -- MEDIUM confidence (training data, not live-verified)
- Hypercore protocol specification (docs.holepunch.to) -- MEDIUM confidence (training data)
- GunDB documentation (gun.eco/docs) -- LOW confidence (training data, less familiar)
- Nostr NIP specifications (github.com/nostr-protocol/nips) -- MEDIUM confidence (training data, well-studied)
- etcd documentation (etcd.io/docs) -- MEDIUM confidence (training data, well-established system)
- BitTorrent protocol specification (bittorrent.org/beps) -- MEDIUM confidence (training data, mature protocol)
- CockroachDB architecture docs (cockroachlabs.com/docs) -- MEDIUM confidence (training data)
- Project memory and PROJECT.md -- HIGH confidence (direct project context)
- Previous project lessons (chromatin-protocol, DNA messenger, PQCC) -- HIGH confidence (documented in project memory)

---
*Features research for: chromatindb -- decentralized PQ-secure database node (signed blob store with replication)*
*Researched: 2026-03-03*
*Scope: Layer 1 (database node) only. Application semantics are Layer 2 (Relay).*
