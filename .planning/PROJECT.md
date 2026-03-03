# chromatindb

## What This Is

A decentralized, post-quantum secure database node. You run chromatindb on a server, it joins a network of other chromatindb nodes, stores signed blobs in cryptographically-owned namespaces, and replicates data across the network. Anyone can run a node. Anyone can generate a keypair and start writing. The system is designed to be technically unstoppable.

The database layer is intentionally dumb — it stores signed blobs, verifies ownership, replicates, and expires old data. Application logic (messaging, identity, social) lives in higher layers built on top.

## Core Value

Any node can receive a signed blob, verify its ownership via cryptographic proof (SHA3-256(pubkey) == namespace + ML-DSA-87 signature), store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## Requirements

### Validated

(None yet — ship to validate)

### Active

- [ ] Namespace model: SHA3-256(pubkey) = namespace, verified on every write
- [ ] Blob storage: signed blobs stored in libmdbx keyed by namespace + hash
- [ ] Signature verification: ML-DSA-87 sig check on every ingest
- [ ] Content-addressed dedup: SHA3-256 hash as blob ID
- [ ] Sequence index: per-namespace monotonic seq_num for efficient polling
- [ ] TTL and expiry: 7-day default, TTL=0 permanent, automatic pruning
- [ ] Peer discovery: bootstrap nodes + peer exchange (no DHT)
- [ ] Node-to-node sync: hash-list diff, bidirectional, resumable
- [ ] PQ-encrypted transport: ML-KEM-1024 key exchange + AES-256-GCM channel
- [ ] Write ACKs: confirmation with replication count
- [ ] Wire format: FlatBuffers with deterministic encoding for signing
- [ ] Query interface: "give me namespace X since seq_num Y", "give me all namespaces"

### Out of Scope

- Application semantics (messages, profiles, nicknames) — relay/app layer concern
- Human-readable names — relay/app layer concern
- Client authentication — relay layer concern
- Message routing — relay layer concern
- Conflict resolution / LWW / HLC — relay/app layer concern
- Encrypted envelopes — relay/app layer concern
- DHT or gossip protocol — proven unreliable in previous projects
- Layer 2 (Relay) and Layer 3 (Client) — future work

## Context

Three-layer architecture (building bottom-up):
- **Layer 1 (NOW): chromatindb** — database node network
- **Layer 2 (FUTURE): Relay** — application semantics, owns a namespace
- **Layer 3 (FUTURE): Client** — mobile/desktop app, talks to relay

Previous projects inform design:
- **chromatin-protocol**: Kademlia + libmdbx + WebSocket = too complex. No DHT ever again.
- **DNA messenger**: DHT storage unreliable. SQLite-as-cache on client worked.
- **PQCC**: PQ crypto stack proven and production-ready. Reuse directly.

Blob format on the wire:
```
Blob {
  namespace:  [32B]     SHA3-256(pubkey)
  pubkey:     [2592B]   ML-DSA-87 public key
  data:       [bytes]   opaque payload
  ttl:        u32       seconds until expiry (0 = permanent, default 604800)
  timestamp:  u64       wall clock time of creation
  signature:  [4627B]   ML-DSA-87 signature over (namespace || data || ttl || timestamp)
}

Computed:
  hash    = SHA3-256(blob content) — blob ID, dedup key
  seq_num = assigned by receiving node (local ordering per namespace)
```

Storage model (libmdbx):
- **Blobs**: signed data, keyed by namespace + hash
- **Sequence index**: per-namespace monotonic seq_num → blob hash
- **Expiry index**: sorted by expiry timestamp for efficient pruning
- **Peer state**: per-peer sync progress

## Constraints

- **Crypto**: All NIST Category 5 via liboqs — ML-DSA-87 (signing), ML-KEM-1024 (key exchange), SHA3-256 (hashing), AES-256-GCM (symmetric). No OpenSSL — liboqs provides all crypto primitives.
- **Storage**: libmdbx — LMDB-compatible, crash-safe
- **Wire format**: FlatBuffers — deterministic encoding required for signing
- **Language**: C++20, CMake, FetchContent for all dependencies (always use latest available version)
- **Sync fingerprints**: xxHash (XXH3)
- **Testing**: Catch2
- **Logging**: spdlog
- **Config**: nlohmann/json
- **No DHT**: Explicit constraint from lessons learned
- **No OpenSSL**: liboqs has everything built in, avoid unnecessary dependencies

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| No DHT, use bootstrap + peer exchange | Kademlia proved too complex and unreliable in previous projects | — Pending |
| libmdbx over SQLite for node storage | LMDB-style MVCC fits high-throughput concurrent reads; crash-safe | — Pending |
| FlatBuffers over Protobuf | Deterministic encoding needed for signing; zero-copy deserialization | — Pending |
| ML-DSA-87 + ML-KEM-1024 (NIST Cat 5) | Maximum PQ security; proven in PQCC project | — Pending |
| No OpenSSL, liboqs only | liboqs provides AES-256-GCM, SHA3, and all PQ primitives — no need for external crypto dep | — Pending |
| Database is intentionally dumb | Separation of concerns: db stores blobs, app layer interprets them | — Pending |

---
*Last updated: 2026-03-03 after initialization*
