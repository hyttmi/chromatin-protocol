# Project Research Summary

**Project:** chromatindb
**Domain:** Decentralized post-quantum secure signed blob store daemon
**Researched:** 2026-03-03
**Confidence:** HIGH (stack verified via git tags; architecture patterns are mature and stable; features validated against IPFS/Hypercore/Nostr/etcd precedent)

## Executive Summary

chromatindb is a standalone daemon that forms a peer-to-peer network of signed blob stores. Each node holds cryptographically-owned namespaces — namespace = SHA3-256(pubkey) — replicates blobs across peers over post-quantum encrypted transport, and auto-expires data via TTL. The model is close to a Nostr relay (dumb storage, signed data, no app semantics) but with built-in node-to-node replication, PQ crypto throughout, and a TTL-first data model rather than permanent storage. Unlike IPFS or Hypercore there is no DHT — a deliberate constraint from previous project failures (chromatin-protocol and DNA messenger). Bootstrap + peer exchange is simpler, more reliable at the target scale, and entirely sufficient for the use case.

The recommended approach builds bottom-up in strict dependency order: crypto and wire format first (leaf dependencies needed by everything), then storage, then blob ingest logic, then the networking layer (Asio event loop + PQ-encrypted connections), then the peer system (discovery and sync). This order lets each layer be tested independently before the next is added, and keeps the hardest integration work — multi-node sync over PQ-encrypted transport — until after the foundation is solid and tested. The full stack is C++20 with CMake FetchContent: liboqs for all PQ crypto, a small audited AEAD+KDF library (not OpenSSL) for symmetric crypto, libmdbx for storage, FlatBuffers (or hand-packed binary if canonicality issues arise) for wire format, and standalone Asio for async TCP.

The dominant risks are: signing over non-deterministic FlatBuffers bytes (day-one design decision, protocol-breaking to fix), unauthenticated PQ transport (ML-KEM encrypts but does not authenticate — requires post-handshake ML-DSA-87 signed challenge, must be in v0.1), and ML-DSA-87 verification cost (~0.3ms/op) dominating sync throughput (requires a worker thread pool and verification result cache before any multi-peer testing). All three are well-understood, have clear mitigations, and are addressable if caught at the right phase.

## Key Findings

### Recommended Stack

The stack is narrow and well-justified. Every choice has a specific rationale; nothing is included for convenience. C++20 provides the language features needed (Asio coroutines via `co_await`, `std::span`, constexpr) without Boost. liboqs is the only production-grade NIST-compliant PQ crypto library, proven in the PQCC project. OpenSSL is explicitly excluded — symmetric crypto (AES-256-GCM, key derivation from shared secrets) comes from a small, audited AEAD+KDF library yet to be selected. libmdbx provides single-writer/multi-reader MVCC with zero-copy mmap reads and automatic page reclamation (critical for TTL blob pruning), and is strictly better than LMDB for this use case. FlatBuffers is retained for zero-copy deserialization and schema evolution but is flagged as potentially replaceable with hand-packed binary if canonicality causes signing problems. Standalone Asio (not Boost.Asio) is header-only, provides async TCP with C++20 coroutine support, and handles sockets, timers, and signals in a single `io_context`. Always fetch the latest available versions via FetchContent.

**Core technologies:**
- **C++20 / CMake + FetchContent**: Language and build — coroutines, concepts, no Boost. All deps pulled at configure time.
- **liboqs (latest)**: PQ crypto — ML-DSA-87 signing/verification, ML-KEM-1024 key encapsulation, SHA3-256 hashing. No credible alternative.
- **Small audited AEAD+KDF library (TBD, not OpenSSL)**: Symmetric crypto — AES-256-GCM channel encryption, session key derivation from ML-KEM shared secret. Minimal dep surface. Candidates: libsodium, monocypher.
- **libmdbx (latest)**: Embedded storage — MVCC, zero-copy mmap reads, automatic page reclamation. Better than LMDB for TTL-heavy append-mostly workloads.
- **FlatBuffers (latest)**: Wire format — zero-copy deserialization, schema evolution, deterministic encoding (with `ForceDefaults(true)`). Flagged as replaceable with hand-packed binary if issues arise.
- **Standalone Asio (latest)**: Async TCP — C++20 `co_await` coroutines, single `io_context` for sockets, timers, and signals.
- **xxHash/XXH3 (latest)**: Sync fingerprints — fast non-crypto hashing for O(1) "already in sync" check before hash-list exchange.
- **Catch2, spdlog, nlohmann/json (latest)**: Testing, logging, config — standard, proven in previous projects.

### Expected Features

The feature set is intentionally narrow. The node stores, verifies, replicates, and expires signed blobs. It has no knowledge of what blobs contain. Application semantics (messages, profiles, nicknames, conflict resolution) live in Layer 2 (Relay) and are out of scope for the database node.

**Must have (table stakes — v0.1):**
- Namespace model: SHA3-256(pubkey) = namespace, verified on every ingest — trust foundation
- Blob storage with content-addressed dedup (SHA3-256 hash as blob ID)
- ML-DSA-87 signature verification on every blob before storage
- Sequence index per namespace (monotonic seq_num: "give me namespace X since seq Y")
- TTL-based expiry with automatic background pruning (7-day default, TTL=0 permanent)
- PQ-encrypted transport: ML-KEM-1024 key exchange + AEAD session, with ML-DSA-87 mutual authentication
- TCP listener + outbound peer connections via Asio
- Bootstrap peer discovery from JSON config
- Hash-list diff sync (bidirectional) with seq_num incremental exchange
- Write ACK (local confirmation)
- JSON config, spdlog logging, signal handling, graceful shutdown

**Should have (competitive — v0.2):**
- Peer exchange (PEX): learn about peers from peers, beyond just bootstrap
- Resumable sync: persist per-peer sync progress, only exchange new blobs on reconnect
- Write ACK with replication count: report how many peers confirmed a blob
- XXH3 fingerprint fast-path: skip hash-list exchange when already in sync (O(1))
- Rate limiting and blob size limits: per-peer and per-namespace abuse prevention
- Storage statistics: namespace count, blob count, disk usage via query interface

**Defer (v1.0+):**
- Negentropy set reconciliation (O(diff) sync — verify C++ availability before committing)
- Selective namespace replication (nodes choose which namespaces to mirror)
- Admin interface for runtime peer management and config reload
- Prometheus metrics endpoint

**Anti-features (never build in the database node):**
- DHT of any kind — proven unreliable in previous projects
- Application semantics: messages, profiles, nicknames — Layer 2 concern
- Conflict resolution or CRDT — Layer 2 concern
- Human-readable namespaces — Layer 2/3 concern
- Rich query language — blobs are opaque
- Encrypted envelopes — payload encryption is Layer 2's job
- Global consensus — single-writer namespaces need none
- Capability delegation / access grants — Layer 2 concern
- Built-in HTTP/REST API — wrong transport for a binary peer protocol

### Architecture Approach

The architecture is a single-process daemon: one Asio `io_context` on the main thread handles all socket IO, while CPU-heavy work (ML-DSA-87 verification, SHA3 hashing, storage writes) is dispatched to a fixed-size worker thread pool with results posted back via eventfd. This pattern keeps the event loop responsive under burst ingest — ML-DSA-87 verify takes ~0.3ms per op and cannot block the network thread. libmdbx enforces single-writer internally; a write queue accumulates incoming blobs and flushes them in batches to maximize transaction efficiency. Six build phases map directly to the dependency-ordered component hierarchy.

**Major components:**
1. **Crypto Layer** — liboqs + AEAD/KDF wrappers. Pure functions (bytes in, bytes out). Leaf dependency; no network or storage awareness.
2. **Wire Format** — FlatBuffers schemas for Blob, ClientMessage, PeerMessage. Build-time artifact. Defines the wire contract.
3. **Storage Layer** — libmdbx wrapper owning all four sub-databases (blobs, sequence, expiry, peers). Single component controls all transactions. Background expiry scanner runs on timer.
4. **Blob Engine** — Ingest pipeline: namespace verification, signature verification (via worker pool), content-address dedup, seq_num assignment, store, query. No networking awareness.
5. **Networking Layer** — Asio event loop, PQ-encrypted TCP connection (ML-KEM handshake + AEAD + ML-DSA-87 mutual auth), client listener, peer manager (bootstrap, connection pool, reconnect with backoff), worker thread pool.
6. **Sync Engine** — Hash-list diff protocol, bidirectional. Reads per-peer sync state from storage, queries local seq index, sends SYNC_REQUEST / HASH_LIST / WANT / BLOBS. Resumable via persisted per-peer seq_num progress.

**Storage schema (4 sub-databases):**
- `blobs`: key = [namespace:32][hash:32], value = FlatBuffers Blob. Primary store.
- `sequence`: key = [namespace:32][seq_num:8], value = [hash:32]. Per-namespace efficient polling.
- `expiry`: key = [expiry_timestamp:8][hash:32], value = [namespace:32]. Sorted for efficient batch pruning.
- `peers`: key = [peer_id:32][namespace:32], value = [last_synced_seq:8][last_sync_time:8]. Resumable sync state.

**Blob format on wire:**
```
Blob {
  namespace:  [32B]     SHA3-256(pubkey)
  pubkey:     [2592B]   ML-DSA-87 public key
  data:       [bytes]   opaque payload
  ttl:        u32       seconds until expiry (0 = permanent, default 604800)
  timestamp:  u64       wall clock time of creation (unix seconds)
  signature:  [4627B]   ML-DSA-87 sig over (namespace || data || ttl || timestamp)
}
Computed by node: hash = SHA3-256(blob content), seq_num = local monotonic per namespace
```

### Critical Pitfalls

1. **FlatBuffers non-determinism breaks signature verification** — Sign over a canonical byte string (`SHA3-256(namespace || data || ttl || timestamp)` as a fixed-size raw concatenation), NOT over the raw FlatBuffer bytes. The FlatBuffer is the transport envelope that carries the signature; it is not the signed content. Set `ForceDefaults(true)` on every FlatBufferBuilder always. Write a round-trip test: serialize, deserialize, re-serialize, compare bytes. Run in CI across platforms. This is a day-one design decision; retrofitting it requires a protocol version bump and breaks verification of all existing blobs.

2. **PQ transport encrypts but does not authenticate — MITM-trivial without post-handshake auth** — ML-KEM-1024 establishes a shared secret but does not prove who you are talking to. After the key exchange, both sides must sign the session fingerprint (`SHA3-256("chromatindb-v1" || shared_secret)`) with their ML-DSA-87 identity key, and each side verifies the peer's signature against a known or TOFU-pinned key. No plaintext fallback — not even behind a debug flag. Must be in v0.1; adding it later is protocol-breaking.

3. **ML-DSA-87 verification cost stalls sync without a worker pool and cache** — Never verify signatures on the event loop thread. Dispatch to a worker thread pool (sized to `hardware_concurrency`). Add a verification result cache keyed by blob hash: once a blob is verified, never re-verify the same hash (blobs are immutable, content-addressed). Without the cache, syncing 10K blobs takes 3-30 seconds of CPU. The cache is ~50 LOC (in-memory LRU or small libmdbx sub-database).

4. **libmdbx single-writer serialization bottlenecks concurrent ingest** — Never issue one write transaction per blob. Accumulate blobs in a write queue, flush in batches (every 100ms or N blobs). One transaction with 100 puts is an order of magnitude faster than 100 single-put transactions. Design the write pipeline before writing storage code; retrofitting batch writes changes the concurrency model.

5. **Timestamp-based TTL without clock validation enables abuse** — Reject blobs where `timestamp > now + 10 minutes` (prevents future-timestamp bypass of TTL). Apply an expiry grace period when pruning: `expiry_timestamp + 5 minutes < now` (absorbs NTP skew between nodes). Both validations belong in the ingest path from day one.

6. **Permissionless namespace creation enables storage spam** — Anyone can generate keypairs and flood the node with valid-signature blobs. Mitigate with: configurable per-node storage quota (evict nearest-expiry first when approaching limit), per-namespace byte cap, configurable TTL ceiling (reject blobs with TTL above configured maximum), configurable namespace blocklist for node operators.

## Implications for Roadmap

Research is explicit about build order: crypto and wire format are leaf dependencies everything else requires, so they come first. Storage precedes the blob engine. The blob engine must be testable without a network, so it precedes networking. Networking must exist before the peer system. Peer system and multi-node sync are the last and most complex integration. This order is not negotiable — it matches the dependency graph from ARCHITECTURE.md.

### Phase 1: Foundation (Crypto, Wire Format, Config)

**Rationale:** Every other component depends on hashing (SHA3-256), signing (ML-DSA-87), or serialization (FlatBuffers). These have zero dependencies on anything else. Build and test them in isolation, get them right, and then treat them as stable. The canonical signing approach (sign a fixed byte concatenation, not FlatBuffer bytes) must be decided here — it is a protocol-breaking decision to change later.
**Delivers:** liboqs C++ RAII wrappers (sign, verify, hash, kem_encaps, kem_decaps), AEAD+KDF wrappers for AES-256-GCM and key derivation, FlatBuffers schemas (Blob, ClientMessage, PeerMessage) with canonicality-verified blob format, nlohmann/json config parsing, spdlog setup, node identity (keypair + namespace derivation).
**Addresses:** Namespace ownership model, blob format, wire contract, configuration.
**Avoids (pitfall):** FlatBuffers non-determinism — canonical signing approach locked in, round-trip test written.
**Research flag:** AEAD+KDF library selection is a concrete gap. Must choose (libsodium, monocypher, or other) before coding begins.

### Phase 2: Storage Engine

**Rationale:** The blob engine needs storage to be complete, but storage can be tested with raw byte operations before the blob engine exists. libmdbx transaction discipline is subtle — getting it right in isolation prevents hard-to-diagnose bugs later. Per-node quotas and timestamp validation also live here.
**Delivers:** libmdbx wrapper with all 4 sub-databases, typed CRUD for blobs/sequence/expiry/peers, write batch queue, timer-driven TTL expiry scanner, libmdbx geometry configuration (large upper bound), timestamp validation on ingest, per-node storage quota enforcement.
**Addresses:** Blob storage, sequence index, TTL expiry, per-peer sync state, storage spam limits.
**Avoids (pitfalls):** libmdbx write bottleneck (batch writes from the start), stale read transaction anti-pattern (RAII wrappers), geometry misconfiguration, timestamp abuse, storage spam.
**Research flag:** Standard LMDB-style patterns, no research needed.

### Phase 3: Blob Engine

**Rationale:** Core business logic. Can be fully tested without sockets — feed blobs directly in unit tests. This validates the entire ingest pipeline (namespace check, sig verify, dedup, seq assign, store, query) before network complexity is introduced. Verification cache and worker thread pool design are established here.
**Delivers:** Namespace verification (SHA3(pubkey) == namespace check — first validation, before sig verify), ML-DSA-87 signature verification dispatched to worker pool, verification result cache, content-addressed dedup, seq_num assignment, query interface (namespace + seq range, namespace listing).
**Addresses:** Signature verification, dedup, sequence index queries, write ACK.
**Avoids (pitfalls):** Sig verification on event loop thread (worker pool), missing verification cache.
**Research flag:** Standard patterns, no research needed.

### Phase 4: Networking Layer (Event Loop + PQ-Encrypted Connections)

**Rationale:** The peer system requires encrypted connections and the event loop. The transport layer is independently testable (loopback echo, single-connection tests) before peer logic is added. The PQ handshake + mutual authentication is the security-critical component and must be correct before any real peer connections.
**Delivers:** Asio `io_context` event loop, PQ-encrypted TCP connection (ML-KEM-1024 key exchange + AEAD channel + ML-DSA-87 mutual authentication), TCP listener for inbound connections, outbound connection initiation, worker thread pool integration, length-prefix message framing, per-IP connection limits.
**Addresses:** PQ-encrypted transport, mutual authentication, concurrent connections, message framing, graceful shutdown, signal handling.
**Avoids (pitfalls):** Unauthenticated transport (authentication built in from the start), no plaintext fallback, missing message framing.
**Research flag:** Verify Asio C++20 coroutine API (`asio::co_spawn`, `asio::use_awaitable`, strand-based per-connection serialization) against current docs before implementation. MEDIUM confidence from research — the API details were not web-verified.

### Phase 5: Peer System (Discovery + Sync)

**Rationale:** Integrates everything: networking + blob engine + storage + crypto. Multi-node sync is the most complex component and the last to be added. Peer discovery (bootstrap) is simpler and comes first within this phase. Two-node loopback tests become possible here.
**Delivers:** Peer manager (bootstrap from config, connection pool, reconnect with backoff), hash-list diff sync protocol (bidirectional, seq_num incremental — only exchange hashes since last sync checkpoint), per-peer sync state persistence (resumable), XXH3 fingerprint O(1) fast-path (skip hash exchange when fingerprints match).
**Addresses:** Peer discovery, node-to-node sync, resumable sync (core).
**Avoids (pitfalls):** Full hash-list exchange (seq_num incremental from the start), sync scaling, global sync anti-pattern (namespace-scoped, per-peer progress tracking).
**Research flag:** Hash-list diff is well-understood, no research needed. Negentropy C++ availability needs verification before v1.0 upgrade decision — defer to v1.0 planning.

### Phase 6: Integration and Hardening

**Rationale:** Main daemon binary wiring, end-to-end multi-node integration tests, and hardening items (rate limiting, blob size limits, namespace blocklist) that require the full system to be meaningful.
**Delivers:** `main.cpp` wiring all components, two-node loopback integration tests, multi-node sync convergence tests, TTL expiry integration test, rate limiting (per-peer and per-namespace), per-namespace storage quotas, namespace blocklist, peer exchange (v0.2), write ACK with replication count (v0.2).
**Addresses:** Daemon lifecycle, graceful shutdown, storage spam mitigations, abuse prevention, v0.2 features.
**Avoids (pitfalls):** Storage spam (quotas and limits enforced), per-IP connection limits.
**Research flag:** Standard integration and hardening patterns, no research needed.

### Phase Ordering Rationale

- **Dependency-driven bottom-up**: Each phase only adds components that depend on all prior phases. No phase builds something requiring components not yet tested. This matches the explicit build order from ARCHITECTURE.md.
- **Testability at every phase**: Phases 1-3 are fully testable without network sockets. Phase 4 is testable with loopback connections. Phase 5 requires two nodes (localhost). Phase 6 requires the complete daemon.
- **Security-critical paths in early phases**: The canonical signing approach (Phase 1) and transport authentication (Phase 4) are the two highest-cost pitfalls to retrofit. Both are addressed before any complex logic is layered on top.
- **No scope creep**: The anti-features list is long and explicit. Nothing from that list appears in any phase.

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 1:** AEAD+KDF library selection — need to identify a small, audited, FetchContent-compatible library for AES-256-GCM and HKDF/key-derivation that is not OpenSSL. Options to evaluate: libsodium (AES-256-GCM available, HKDF included, widely audited), monocypher (public domain, no deps, ChaCha20-Poly1305 natively, AES-256-GCM not native), or a header-only AEAD library. This is a concrete gap in the current research.
- **Phase 4:** Asio C++20 coroutine API — MEDIUM confidence from research. Verify `asio::co_spawn`, `asio::use_awaitable`, and strand-based per-connection serialization against current Asio docs before coding.

Phases with well-documented patterns (skip dedicated research):
- **Phase 2:** libmdbx patterns are well-documented. Transaction discipline, geometry config, and RAII wrappers are standard LMDB-style usage.
- **Phase 3:** Blob ingest is deterministic logic with known patterns from PQCC.
- **Phase 5 (hash-list diff):** Set difference sync is a well-understood algorithm; implementation is mechanical.
- **Phase 6:** Integration testing and hardening are standard practices.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All library categories verified. liboqs proven in PQCC. libmdbx, FlatBuffers, Asio are mature. One gap: AEAD+KDF library not yet selected — no OpenSSL. |
| Features | MEDIUM-HIGH | Feature set informed by strong analogies (Nostr relay model, Hypercore seq_num, etcd TTL/ACK, BitTorrent PEX). Scope is narrow and well-defined by PROJECT.md constraints. |
| Architecture | HIGH | All patterns described (Asio + worker pool, libmdbx single-writer, hash-list diff) are mature and stable. Build order follows a strict dependency graph. Specific Asio coroutine API details are MEDIUM confidence. |
| Pitfalls | MEDIUM-HIGH | Most pitfalls are well-documented problems in libmdbx/liboqs/FlatBuffers ecosystems. ML-DSA-87 timing (~0.3ms on x86) is an estimate that needs hardware validation. |

**Overall confidence:** HIGH

### Gaps to Address

- **AEAD+KDF library selection**: PROJECT.md explicitly excludes OpenSSL. No specific library is named. Must be decided before Phase 1 coding begins. Candidates: libsodium (widely audited, AES-256-GCM + HKDF, larger footprint), monocypher (public domain, minimal, ChaCha20-Poly1305 natively — evaluate whether ChaCha20 is acceptable in place of AES-256-GCM). A concrete choice with rationale is needed before Phase 1 planning.
- **FlatBuffers vs hand-packed binary**: If the canonical signing approach (sign a fixed-size byte concatenation, not the FlatBuffer bytes) is correctly implemented in Phase 1, FlatBuffers non-determinism becomes irrelevant for correctness. The question then becomes whether FlatBuffers is worth the dependency. Decision can be deferred to Phase 1 implementation — but must be decided then, not later.
- **Asio coroutine API verification**: Training-data confidence on `asio::co_spawn` / `co_await` integration. Verify against current Asio docs before Phase 4.
- **ML-DSA-87 timing on target hardware**: The ~0.3ms figure is a community x86 benchmark. Measure on actual deployment hardware to size the worker thread pool correctly.
- **Negentropy C++ availability**: A C++ implementation exists (github.com/hoytech/negentropy) but maturity and FetchContent compatibility need verification before committing to it for the v1.0 sync upgrade.

## Sources

### Primary (HIGH confidence)
- liboqs, libmdbx, FlatBuffers, Asio, xxHash, Catch2, spdlog, nlohmann/json git repositories — all library versions verified via `git ls-remote --tags` on 2026-03-03
- `.planning/PROJECT.md` — direct project requirements and constraints (including no-OpenSSL constraint, FlatBuffers-as-potentially-replaceable flag)
- PQCC project experience — liboqs integration patterns proven in production, documented in project memory
- ARCHITECTURE.md — libmdbx MVCC semantics from library header/docs; epoll/Asio patterns from Stevens/Kerrisk and Asio documentation; ML-KEM/ML-DSA parameters from NIST FIPS 203/204

### Secondary (MEDIUM confidence)
- IPFS, Hypercore, GunDB, Nostr, etcd, BitTorrent architectures — training data analysis informing feature prioritization and anti-pattern identification
- FlatBuffers deterministic encoding limitations — training data, well-documented community issue
- P2P networking patterns (bootstrap, PEX, hash-list diff) — training data from Bitcoin, Nostr, IPFS documentation
- ML-DSA-87 ~0.3ms verification timing — community benchmarks, needs hardware validation

### Tertiary (LOW confidence)
- Negentropy C++ implementation (hoytech/negentropy) maturity — needs direct verification before v1.0 planning
- AEAD+KDF library landscape (libsodium vs monocypher vs alternatives) — gap in current research, needs dedicated evaluation

---
*Research completed: 2026-03-03*
*Ready for roadmap: yes*
