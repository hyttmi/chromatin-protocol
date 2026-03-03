# CPUNK-DB

## What This Is

A C/C++ library (libcpunkdb) providing a decentralized, post-quantum secure key-value database. The architecture is Telegram's model with the trust arrow reversed: clients are the source of truth, relays are untrusted caches that store, index, and serve signed operations. Supports messaging (encrypted inboxes), social feeds (public namespaces), identity (permanent profiles with unique nicknames and wallet addresses), and any other namespaced key-value data. All data is ephemeral by default (7-day TTL) except permanent profile namespaces.

## Core Value

Any device can write to its own namespace, go offline, reconnect to any relay, and converge to the correct state — with every operation cryptographically verified and no trust in infrastructure.

## Requirements

### Validated

(None yet — ship to validate)

### Active

- [ ] Namespaced key-value data owned by ML-DSA-87 public key identities
- [ ] Signed append-only operation log (SET, DELETE, GRANT, REVOKE, REGISTER)
- [ ] Deterministic conflict resolution (LWW with HLC + hash tiebreak for data, FWW for names)
- [ ] Offline-first: full local storage (libmdbx), sync when connected
- [ ] 7-day default TTL on all operations (configurable per-op, ephemeral by default)
- [ ] Permanent profile namespace (TTL=0): pubkeys, bio, nickname, wallet addresses, relay hints
- [ ] Unique immutable nicknames (FWW + relay-enforced registration)
- [ ] Namespace ownership with capability/grant model (scoped write permissions with TTL)
- [ ] Encrypted payload envelopes (ML-KEM-1024 + AES-256-GCM) — v1
- [ ] Sync protocol logic (transport-agnostic, hash-list diff for v1, Negentropy upgrade path)
- [ ] Multi-relay connectivity (no single point of failure/censorship)
- [ ] Hybrid crypto model: library owns signing/verification/algorithms, app provides key material
- [ ] Wallet address storage in profile (derived from same seed as ML-DSA-87, derivation is app-layer)
- [ ] Anti-spam for inbox: capability-only for v1, PoW for strangers in v2

### Out of Scope

- Relay implementation — separate project (thin service over PostgreSQL/CouchDB/etc.)
- Transport layer — library produces sync messages, caller handles WebSocket/TCP/etc.
- Key derivation from seed phrase — app-layer concern, library stores resulting pubkeys
- Application-layer UI — no messaging UI, social feed UI, or chat protocols
- Blockchain — not needed; FWW + relay enforcement handles name uniqueness
- DHT routing — relays replace DHT entirely
- Payment rails — anti-spam uses PoW, not payments
- Schema enforcement — values are opaque blobs, apps validate their own data

## Context

Previous attempt (chromatin-protocol P2P messenger) used DHT-based store-and-forward. Learned that DHT doesn't scale for messaging. CPUNK-DB uses Telegram's architecture pattern (client-server sync with sequence-based diffs) but reverses the trust: clients sign everything, relays are replaceable untrusted caches.

The model: each user has a local database (libcpunkdb on device). 1-2 relay servers replicate/index user databases. Relays sync with each other. Users sync their local DB to/from relays. Relays index profiles for name lookup and discovery. Relays enforce name registration uniqueness.

Existing crypto experience from chromatin/PQCC project: ML-DSA-87, ML-KEM-1024, AES-256-GCM, SHA3-256 — all via liboqs 0.15.0. NIST Category 5 throughout.

Target: implementable by a small team in 3-6 months. C/C++20, CMake, liboqs via FetchContent.

## Constraints

- **Crypto**: ML-DSA-87 (4627-byte sigs), ML-KEM-1024, AES-256-GCM, SHA3-256 via liboqs 0.15.0
- **Language**: C/C++20, CMake build system
- **Storage**: libmdbx 0.13.11 (client), existing DB for relay (PostgreSQL/CouchDB/etc.)
- **Serialization**: FlatBuffers 25.12.19 (deterministic encoding required for content-addressed signing)
- **Mobile**: Battery-friendly — minimal round trips, efficient sync, no background polling
- **NAT**: Outbound connections to relays (no port forwarding)
- **Relay trust**: Zero for data integrity (everything signed). Trusted for name registration ordering only
- **TTL**: 7-day default. Profile keys (TTL=0) are the only permanent data
- **Signatures**: 4627 bytes per operation — design must account for this overhead

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Library, not daemon | Embeddable in any app. Daemon wrapper trivial to add | — Pending |
| Telegram architecture, trust reversed | Client = source of truth, relay = cache. Proven sync model, decentralized trust | — Pending |
| Relay is separate project | Thin service over existing DB. libcpunkdb defines sync protocol, relay handles transport/storage | — Pending |
| HLC for ordering | Wall-clock correlation + causal guarantees. Bounded skew detection (MAX_SKEW ~5min) | — Pending |
| LWW for data, FWW for names | LWW: deterministic, simple for single-owner namespaces. FWW: first-come-first-served for nickname registration | — Pending |
| 7-day default TTL | Ephemeral by default. Self-cleaning storage. Solves log growth, deletion semantics, relay costs | — Pending |
| Permanent profiles (TTL=0) | Identity anchor: pubkeys, bio, nickname, wallets, relay hints. Must not expire | — Pending |
| Unique immutable nicknames | Set once, never changed. Relay-enforced FWW for uniqueness. Like ENS but without blockchain | — Pending |
| Hybrid crypto model | Library owns sign/verify/encrypt/decrypt. App provides key material. Library guarantees nothing goes in unsigned | — Pending |
| Key derivation is app-layer | Library stores pubkeys in profile. Seed-to-keypair derivation (ML-DSA, ML-KEM, BTC, ETH) is app's job | — Pending |
| Encrypted envelopes in v1 | ML-KEM + AES-GCM from day one. Required if anyone builds messaging on top | — Pending |
| No blockchain | FWW + relay enforcement sufficient for name uniqueness. Raft sequencer upgrade path if needed | — Pending |
| No DHT | Relays replace DHT. Simpler, faster, NAT-friendly | — Pending |
| libmdbx over LMDB | Native CMake, automatic page reclamation for TTL-pruned logs, better diagnostics | — Pending |
| FlatBuffers over Protobuf | Deterministic encoding required for content-addressed signing. Zero-copy pairs with libmdbx | — Pending |

---
*Last updated: 2026-03-03 after deep questioning*
