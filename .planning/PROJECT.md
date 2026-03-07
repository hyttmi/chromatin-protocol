# chromatindb

## What This Is

A decentralized, post-quantum secure database node with access control. You run chromatindb on a server, it joins a network of other chromatindb nodes, stores signed blobs in cryptographically-owned namespaces, and replicates data across the network via hash-list diff sync. Operators can run open nodes (anyone can connect) or closed nodes (only authorized pubkeys allowed). The system supports blobs up to 100 MiB and is designed to be technically unstoppable.

The database layer is intentionally dumb — it stores signed blobs, verifies ownership, replicates, and expires old data. Application logic (messaging, identity, social) lives in higher layers built on top.

## Core Value

Any node can receive a signed blob, verify its ownership via cryptographic proof (SHA3-256(pubkey) == namespace + ML-DSA-87 signature), store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## Requirements

### Validated

- ✓ Namespace model: SHA3-256(pubkey) = namespace, verified on every write — v1.0
- ✓ Blob storage: signed blobs stored in libmdbx keyed by namespace + hash — v1.0
- ✓ Signature verification: ML-DSA-87 sig check on every ingest — v1.0
- ✓ Content-addressed dedup: SHA3-256 hash as blob ID — v1.0
- ✓ Sequence index: per-namespace monotonic seq_num for efficient polling — v1.0
- ✓ TTL and expiry: 7-day default, TTL=0 permanent, automatic pruning — v1.0
- ✓ Peer discovery: bootstrap nodes + peer exchange — v1.0
- ✓ Node-to-node sync: hash-list diff, bidirectional — v1.0
- ✓ PQ-encrypted transport: ML-KEM-1024 key exchange + ChaCha20-Poly1305 channel — v1.0
- ✓ Write ACKs: confirmation after local storage — v1.0
- ✓ Wire format: FlatBuffers with deterministic encoding for signing — v1.0
- ✓ Query interface: "give me namespace X since seq_num Y", "list all namespaces" — v1.0
- ✓ Access control: allowed_keys config restricts which pubkeys can connect — v2.0
- ✓ Fully closed node: only authorized pubkeys can read or write — v2.0
- ✓ Larger blob limit: 100 MiB for medium files (documents, images, small archives) — v2.0
- ✓ SIGHUP hot-reload of ACL without restart — v2.0
- ✓ Memory-efficient sync for large blobs (index-only hashes, one-blob-at-a-time) — v2.0

### Active

(None — planning next milestone)

### Out of Scope

- Application semantics (messages, profiles, nicknames) — relay/app layer concern
- Human-readable names — relay/app layer concern
- Client authentication — relay layer concern
- Message routing — relay layer concern
- Conflict resolution / LWW / HLC — relay/app layer concern
- Encrypted envelopes — relay/app layer concern
- DHT or gossip protocol — proven unreliable in previous projects
- Layer 2 (Relay) and Layer 3 (Client) — future work
- HTTP/REST API — adds attack surface and deps, binary protocol over PQ-encrypted TCP only
- NAT traversal / hole punching — server daemon assumes reachable address
- OpenSSL — prefer minimal deps (liboqs + libsodium)
- Chunked/streaming blob transfer — only necessary at 1+ GiB; ML-DSA-87 requires full data for signing
- Per-peer read/write restrictions — YAGNI for current access control model

## Context

Shipped v2.0 with 11,027 LOC C++20, 196 tests.
Built across 5 days total: v1.0 in 3 days (8 phases, 21 plans), v2.0 in 2 days (3 phases, 8 plans).

Tech stack: C++20, CMake, liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256), libsodium (ChaCha20-Poly1305, HKDF-SHA256), libmdbx, FlatBuffers, Standalone Asio (C++20 coroutines), xxHash (XXH3), Catch2, spdlog, nlohmann/json.

Three-layer architecture (building bottom-up):
- **Layer 1 (v2.0 SHIPPED): chromatindb** — database node network with access control
- **Layer 2 (FUTURE): Relay** — application semantics, owns a namespace
- **Layer 3 (FUTURE): Client** — mobile/desktop app, talks to relay

Previous projects inform design:
- **chromatin-protocol**: Kademlia + libmdbx + WebSocket = too complex. No DHT ever again.
- **DNA messenger**: DHT storage unreliable. SQLite-as-cache on client worked.
- **PQCC**: PQ crypto stack proven and production-ready. Reuse directly.

## Constraints

- **Crypto (PQ)**: liboqs — ML-DSA-87 (signing), ML-KEM-1024 (key exchange), SHA3-256 (hashing)
- **Crypto (symmetric)**: libsodium — ChaCha20-Poly1305 (AEAD) + HKDF-SHA256 (KDF). No OpenSSL.
- **Storage**: libmdbx — LMDB-compatible, crash-safe
- **Wire format**: FlatBuffers — deterministic encoding required for signing
- **Language**: C++20, CMake, FetchContent for all dependencies (always use latest available version)
- **Networking**: Standalone Asio with C++20 coroutines
- **Sync fingerprints**: xxHash (XXH3)
- **Testing**: Catch2
- **Logging**: spdlog
- **Config**: nlohmann/json
- **No DHT**: Explicit constraint from lessons learned
- **No OpenSSL**: Prefer minimal deps — liboqs for PQ, libsodium for symmetric
- **No shortcuts**: No inefficient code, no lazy workarounds. Code must be correct and efficient — always.

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| No DHT, use bootstrap + peer exchange | Kademlia proved too complex and unreliable in previous projects | ✓ Good — PEX works cleanly, zero DHT complexity |
| libmdbx over SQLite for node storage | LMDB-style MVCC fits high-throughput concurrent reads; crash-safe | ✓ Good — ACID guarantees proven, pimpl isolates header |
| FlatBuffers over Protobuf | Deterministic encoding needed for signing; zero-copy deserialization | ✓ Good — ForceDefaults(true) makes it work |
| ML-DSA-87 + ML-KEM-1024 (NIST Cat 5) | Maximum PQ security; proven in PQCC project | ✓ Good — large keys (2592B pub, 4896B sec) but acceptable |
| No OpenSSL (prefer minimal deps) | Use liboqs for PQ + libsodium for symmetric | ✓ Good — clean separation, smaller attack surface |
| ChaCha20-Poly1305 over AES-256-GCM | Software-fast, constant-time, no hardware dependency | ✓ Good — used by WireGuard, TLS 1.3 |
| Canonical signing (not raw FlatBuffer) | Sign SHA3-256(namespace\|\|data\|\|ttl\|\|timestamp), independent of wire format | ✓ Good — stable across FlatBuffer changes |
| Database is intentionally dumb | Separation of concerns: db stores blobs, app layer interprets them | ✓ Good — clean layer boundary |
| Standalone Asio with C++20 coroutines | Lightweight async, no Boost dependency | ✓ Good — co_await pattern works well |
| Sequential sync protocol (Phase A/B/C) | Avoids TCP deadlock from bidirectional sends | ✓ Good — simple and reliable |
| Inline PEX after sync | Prevents AEAD nonce desync from concurrent message streams | ✓ Good — serialized per-connection |
| Deque for peers_ container | Prevents pointer invalidation on push_back during coroutine suspension | ✓ Good — fixed nasty coroutine bug |
| Timer-cancel pattern for sync inbox | steady_timer on stack, pointer in PeerInfo, cancel to wake | ✓ Good — clean async message queue |
| TTL as protocol invariant | constexpr, not user-configurable — simplifies protocol | ✓ Good — eliminated config complexity |
| AccessControl with std::set + implicit self-allow | O(log n) lookup; self-allow prevents accidental self-lockout | ✓ Good — simple, correct |
| ACL gating at on_peer_connected | After handshake, before message loop — unauthorized peers never see data | ✓ Good — minimal attack surface |
| Silent TCP close for ACL rejection | No protocol-level denial — no information leakage about access rules | ✓ Good — defense in depth |
| SIGHUP via coroutine member function | Avoids stack-use-after-return with compiler coroutine frames | ✓ Good — safer than lambda approach |
| Implicit closed mode from non-empty allowed_keys | One config field, zero ambiguity — no separate toggle needed | ✓ Good — KISS |
| MAX_BLOB_DATA_SIZE as Step 0 in ingest | Single integer comparison before any crypto — cheapest validation first | ✓ Good — prevents resource waste |
| One-blob-at-a-time sync transfer | Memory bounded: only one blob in flight per connection | ✓ Good — prevents OOM with 100 MiB blobs |
| Expiry filtering deferred to receiver | Sender doesn't need blob data at hash collection time | ✓ Good — simplifies sender, index-only reads |
| MAX_FRAME_SIZE = 110 MiB (10% headroom) | Room for protocol overhead without a second round-trip | ✓ Good — future-proof |

---
*Last updated: 2026-03-07 after v2.0 milestone*
