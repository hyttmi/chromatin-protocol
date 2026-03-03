# Features Research

**Domain:** Decentralized replicated key-value database with post-quantum cryptography
**Researched:** 2026-03-03
**Confidence:** MEDIUM-HIGH (based on analysis of Nostr, Automerge, Yjs, Hypercore, OrbitDB, GUN.js)

## Table Stakes

Features that must exist or the system is unusable as a decentralized KV store.

| # | Feature | Complexity | Why Table Stakes | Dependencies |
|---|---------|-----------|-----------------|--------------|
| 1 | **Cryptographic identity** — namespace owned by ML-DSA-87 public key, all writes signed | Medium | Without this, any relay can forge data. This IS the trust model | Crypto layer |
| 2 | **Signed append-only operation log** — SET, DELETE as immutable signed entries | Medium | Source of truth. Without signed ops, no integrity on untrusted relays | Identity, crypto |
| 3 | **Local-first storage** — full replica of subscribed namespaces on device | Medium | Offline-first is a core promise. No local storage = no offline capability | Storage engine (libmdbx) |
| 4 | **Deterministic conflict resolution** — all peers converge to same state | High | Without deterministic resolution, replicas diverge permanently. LWW with HLC + hash tiebreak | HLC, operation log |
| 5 | **Causal ordering (HLC)** — hybrid logical clock for operation timestamps | Medium | Wall-clock correlation for UX + causal guarantees. Bounded skew detection built in | None (standalone ~100 LOC) |
| 6 | **Namespace read/write** — get/set/delete keys within a namespace | Low | The basic API. Everything else builds on this | Storage, operation log |
| 7 | **Multi-relay connectivity** — connect to N relays simultaneously | Medium | Single relay = single point of censorship/failure | Transport interface |
| 8 | **Operation deduplication** — content-addressed ops (SHA3-256 hash as ID) | Low | Without dedup, multi-relay sync creates duplicates. Hash-based ID gives free dedup | Hashing |
| 9 | **Signature verification on ingest** — reject invalid ops at the door | Medium | Accepting unverified ops poisons the local store. Verify-on-ingest is the firewall | Crypto layer |
| 10 | **Capability grants** — owner can authorize other keys to write | High | Without delegation, only the owner can write. Inbox, shared namespaces, multi-device all need grants | Identity, operation log |
| 11 | **TTL on operations** — all operations expire (except profile namespace) | Medium | System is ephemeral by design. Without TTL: unbounded storage growth, no natural pruning, relay costs explode. TTL=0 reserved for profile keys only | Operation format, storage |
| 12 | **Profile namespace** — permanent identity anchor with pubkeys, bio, relay hints | Medium | Everything starts with key discovery. Profile is what you look up to find someone's signing key, encryption key, preferred relays. Must not expire | Identity, operation format |

## Differentiators

Features that set CPUNK-DB apart from existing systems.

| # | Feature | Complexity | What It Enables | Comparable Systems |
|---|---------|-----------|----------------|-------------------|
| 1 | **Post-quantum cryptography** — ML-DSA-87 signatures, ML-KEM-1024 encryption | Medium | Future-proof against quantum attacks. No other decentralized DB uses PQ crypto | None — unique |
| 2 | **Ephemeral-by-default data model** — TTL on all non-profile operations | Low | Self-cleaning storage, bounded relay costs, natural privacy (data doesn't persist forever). Unique among decentralized DBs which all assume permanence | None — Nostr/Hypercore/OrbitDB are all permanent-by-default |
| 3 | **Range-based set reconciliation** — Negentropy-style efficient sync | High | O(diff) sync instead of O(n) full-state exchange. Critical for mobile bandwidth | Nostr (Negentropy NIP-77), Hypercore (Merkle tree sync) |
| 4 | **Encrypted envelopes** — ML-KEM + AES-256-GCM per-recipient encryption | Medium | Payloads encrypted at rest on untrusted relays. Relay sees metadata but not values | None built-in with PQ crypto |
| 5 | **Scoped capability delegation** — grants with prefix scope, TTL, rate limits | High | Fine-grained access: "write to keys starting with `inbox/` for 24h, max 100 ops" | OrbitDB has basic access controllers. Nostr has none |
| 6 | **Transport-agnostic design** — library produces/consumes sync messages, never opens sockets | Low | Embeddable in any app. Caller owns transport | Automerge (transport-agnostic). Nostr is WebSocket-only |
| 7 | **Anti-spam for open-write namespaces** — PoW stamps for inbox writes from strangers | Medium | Inbox-style messaging without centralized moderation | Nostr has NIP-13 PoW. No other decentralized DB has built-in anti-spam |
| 8 | **Namespace-scoped replication** — subscribe to specific namespaces, not everything | Low | Mobile devices only sync what they care about. No global state | Nostr (subscription filters), Hypercore (per-feed) |
| 9 | **Deterministic state snapshots** — materialize current KV state from operation log | Medium | Fast reads without replaying entire log. Snapshot hash verifiable against log | Automerge (document snapshots) |
| 10 | **Equivocation detection** — detect conflicting ops for same sequence from same author | Medium | Defense against fork attacks. Peers can flag and distrust equivocating authors | No comparable system detects this explicitly |
| 11 | **Log compaction via TTL + snapshots** — expired ops pruned, snapshots preserve state | High | Storage bounded naturally by TTL. Snapshots allow pruning old ops while preserving current state | Hypercore (no compaction ever). Kafka has log compaction |

## Anti-Features

Things to deliberately NOT build.

| # | Anti-Feature | Why Not | What To Do Instead |
|---|-------------|---------|-------------------|
| 1 | **Global consensus** | No shared state to agree on. Each namespace is independently owned | Per-namespace convergence via deterministic conflict resolution |
| 2 | **Blockchain / chain of blocks** | Ordering from HLC + causal links. No PoW/PoS needed | Signed operation log with content-addressed IDs |
| 3 | **DHT routing** | DHT requires routing tables, churn handling, NAT traversal. Relays solve this simpler | Relay-based pub/sub with multi-relay redundancy |
| 4 | **Human-readable namespaces** | Name registration requires central authority or naming consensus (both ruled out) | Namespaces are SHA3-256(pubkey). Human names are application-layer mapping |
| 5 | **Rich query language** | SQL/GraphQL over a KV store adds enormous complexity | Key prefix scans. Applications build their own indexes |
| 6 | **Full CRDT merge** | Automerge-style CRDTs require complex per-type merge functions. LWW covers 90% of cases | LWW for single values, add-wins for sets. Apps handle complex merges |
| 7 | **Relay trust / reputation** | Adds protocol complexity. Relays are fungible | Connect to multiple relays. Client-side quality tracking |
| 8 | **Built-in payments** | Payment rails add massive scope. PoW is simpler for anti-spam | PoW stamps for strangers, capability tokens for contacts |
| 9 | **Access control lists (ACL)** | ACLs require central authority to evaluate. Capabilities are bearer tokens — local verification | Capability-based grants signed by namespace owner |
| 10 | **Schema enforcement** | Values are opaque blobs. Schema is application concern | Applications validate their own data. CPUNK-DB stores bytes |

## Feature Dependencies (Build Order)

```
Crypto Layer (ML-DSA, ML-KEM, SHA3, AES-GCM)
    ↓
HLC (Hybrid Logical Clock)
    ↓
Operation Log (signed ops, content-addressed, TTL field)
    ↓
├── Local Storage (libmdbx, TTL-based expiry scanning)
├── Namespace/AuthZ (ownership, grants, profile namespace)
│       ↓
│   State Engine (materialize KV, LWW conflict resolution)
│       ↓
│   Encrypted Envelopes (ML-KEM per-recipient encryption)
│       ↓
└── Sync Engine (Negentropy set reconciliation, skip expired ops)
        ↓
    Public API (get/set/delete/subscribe/sync)
```

## MVP Recommendation

Ship first:
1. Crypto layer (reuse patterns from PQCC)
2. HLC implementation
3. Operation format with TTL field + signing/verification
4. Profile namespace (permanent, TTL=0)
5. libmdbx local storage with TTL-based expiry
6. Namespace ownership + basic grants
7. LWW conflict resolution
8. Simple sync (hash-list diff — upgrade to Negentropy later)
9. Public C++ API

Defer to v0.2: Negentropy sync, encrypted envelopes, anti-spam PoW, multi-device, snapshots/compaction, equivocation detection.

## Competitive Feature Matrix

| Feature | CPUNK-DB | Nostr | Automerge | Hypercore | OrbitDB | GUN.js |
|---------|----------|-------|-----------|-----------|---------|--------|
| Signed operations | ML-DSA-87 (PQ) | secp256k1 | None | Ed25519 | None (IPFS) | SEA |
| Data model | Ephemeral (TTL) | Permanent | Permanent | Permanent | Permanent | Permanent |
| Conflict resolution | LWW (HLC) | Replaceable events | CRDT merge | Append-only | CRDT | LWW (HAM) |
| Sync protocol | Negentropy | Negentropy (NIP-77) | Custom | Merkle tree | IPFS bitswap | WebRTC gossip |
| Encryption | ML-KEM + AES-GCM | NIP-44 (XChaCha20) | None | None | None | AES |
| Access control | Capability grants | None | None | Owner-only | OrbitDB ACL | None |
| Offline-first | Yes | No (relay-dependent) | Yes | Yes | Partial | Yes |
| Transport | Agnostic | WebSocket only | Agnostic | Custom | libp2p | WebRTC |
| Anti-spam | PoW + capabilities | NIP-13 PoW | N/A | N/A | N/A | None |
| Identity | Profile namespace | kind:0 event | N/A | Feed keypair | DID | SEA user |

## Steal / Don't Steal

**From Nostr:** Steal Negentropy sync, event-based architecture, relay subscription model. Don't steal the lack of access control or WebSocket coupling.

**From Automerge:** Steal transport-agnostic design, offline-first focus, document snapshots. Don't steal CRDT complexity — LWW is sufficient.

**From Hypercore:** Steal append-only log integrity, Merkle tree verification. Don't steal the no-compaction-ever approach or custom transport.

**From OrbitDB:** Steal database-per-namespace with pluggable access control. Don't steal the IPFS dependency or slow sync.

---
*Features research for: decentralized replicated KV database with PQ crypto*
*Researched: 2026-03-03*
