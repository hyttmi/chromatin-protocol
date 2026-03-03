# CPUNK-DB

## What This Is

A C/C++ library (libcpunkdb) providing a fully decentralized, post-quantum secure key-value database. Namespaced data is owned by cryptographic identities, modified through signed append-only operations, and replicated across untrusted relays. It is a DHT alternative — no global consensus, no blockchain, no mandatory coordination. Applications (messaging, social, file sync, etc.) build on top.

## Core Value

Any device can write to its own namespace, go offline, reconnect to any relay, and converge to the correct state — with every operation cryptographically verified and no trust in infrastructure.

## Requirements

### Validated

(None yet — ship to validate)

### Active

- [ ] Namespaced key-value data owned by ML-DSA-87 public key identities
- [ ] Signed append-only operation log (SET, DELETE, GRANT, REVOKE)
- [ ] Deterministic conflict resolution (HLC + hash tiebreak, LWW per key)
- [ ] Offline-first: full local storage, sync when connected
- [ ] Range-based set reconciliation for efficient sync (not per-item lookups)
- [ ] Namespace ownership with capability/grant model (scoped write permissions)
- [ ] Encrypted payload envelopes (ML-KEM-1024 + AES-256-GCM)
- [ ] Anti-spam model for inbox-style open-write namespaces
- [ ] Multi-device identity support (same identity across devices)
- [ ] Sync protocol logic (algorithm and message types, transport-agnostic)

### Out of Scope

- Relay implementation — separate project, CPUNK-DB defines sync protocol but not the server
- Transport layer — no built-in WebSocket/TCP; library provides sync messages, caller handles I/O
- Application-layer features — no messaging UI, social feeds, or chat protocols
- Human-readable naming — identities are key fingerprints, naming is an application concern
- Blockchain or global consensus — by design
- DHT routing — relays replace DHT; no Kademlia, no routing tables
- Key discovery/exchange — application layer handles introductions
- Payment rails — if anti-spam needs payments, that's external

## Context

Previous attempt (chromatin-protocol P2P messenger) used DHT-based store-and-forward. Learned that DHT doesn't scale for messaging — high latency, unreliable delivery, NAT traversal pain. CPUNK-DB replaces DHT with relay-based replication: relays are dumb pipes that store and forward signed operations. All integrity comes from signatures and hashes, never from relay trust.

Existing crypto experience from chromatin/PQCC project: ML-DSA-87 (signing), ML-KEM-1024 (key exchange), AES-256-GCM (encryption), SHA3-256 (hashing) — all via liboqs. NIST Category 5 throughout.

Target: implementable by a small team in 3-6 months. C/C++20, CMake, liboqs via FetchContent.

## Constraints

- **Crypto**: ML-DSA-87 signatures (4627 bytes), ML-KEM-1024 key exchange, AES-256-GCM, SHA3-256 — all NIST PQ standards via liboqs
- **Language**: C/C++20 with CMake build system
- **Dependencies**: liboqs (FetchContent), minimal others — library must be embeddable
- **Mobile**: Design must be battery-friendly — minimal round trips, efficient sync, no background polling
- **NAT**: Works behind NAT via outbound connections to relays (no port forwarding)
- **Relay trust**: Zero — relays can drop, reorder, delay, or duplicate. They cannot forge, modify, or suppress specific operations without detection
- **Signature size**: ML-DSA-87 signatures are 4627 bytes — operations are chunky, design must account for this

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Library, not daemon | Embeddable in any app. Daemon wrapper is trivial to add later | — Pending |
| Relay protocol is separate project | Clean separation of concerns. CPUNK-DB defines sync algorithm, relay handles transport/storage | — Pending |
| HLC (Hybrid Logical Clock) for ordering | Wall-clock correlation for UX + causal ordering guarantees. Better than pure Lamport (no time info) or vector clocks (grow with writers) | — Pending |
| LWW (Last-Writer-Wins) conflict resolution | Deterministic, simple, works well for single-owner namespaces. Sets use add-wins semantics | — Pending |
| No blockchain | No global consensus needed. Operations are valid if signed by authorized key. Conflict resolution is local | — Pending |
| No DHT | Relays replace DHT for discovery and replication. Simpler, faster, NAT-friendly | — Pending |
| ML-DSA-87 / ML-KEM-1024 (NIST Cat 5) | Maximum PQ security. Larger signatures acceptable for database operations (not chat messages) | — Pending |
| Namespace = SHA3-256(owner_pubkey) | Collision-resistant, no central registry, self-certifying | — Pending |

---
*Last updated: 2026-03-03 after initialization*
