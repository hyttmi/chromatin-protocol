# chromatindb

## What This Is

A decentralized, post-quantum secure database node with access control, data-at-rest encryption, and scalable sync. You run chromatindb on a server, it joins a network of other chromatindb nodes, stores signed blobs in cryptographically-owned namespaces, and replicates data across the network via O(diff) set reconciliation. Operators can run open nodes (anyone can connect) or closed nodes (only authorized pubkeys allowed). The system supports blobs up to 100 MiB with configurable per-blob TTL, thread-pool crypto offload, and sync rate limiting. Designed to be technically unstoppable.

The database layer is intentionally dumb — it stores signed blobs, verifies ownership, encrypts at rest, replicates, and expires old data. Application logic (messaging, identity, social) lives in higher layers built on top.

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

- ✓ Real-time pub/sub notifications for namespace changes — v3.0
- ✓ Namespace delegation (owner grants write access to other pubkeys) — v3.0
- ✓ Blob deletion by owner via replicated tombstones — v3.0

- ✓ Global storage limits with configurable max — v0.4.0
- ✓ Disk-full reporting to peers (protocol-level rejection) — v0.4.0
- ✓ Persistent peer list across restarts — v0.4.0
- ✓ Graceful shutdown (clean disconnect, finish in-flight) — v0.4.0
- ✓ Metrics/observability (connections, storage, sync stats) — v0.4.0
- ✓ Rate limiting for abuse prevention — v0.4.0
- ✓ Tombstone index (O(1) indexed lookup) — v0.4.0
- ✓ Namespace-scoped sync — v0.4.0
- ✓ README with usage/interaction samples — v0.4.0

- ✓ Data-at-rest encryption (ChaCha20-Poly1305 for stored blobs) — v0.5.0
- ✓ Lightweight local handshake (skip PQ crypto for trusted/localhost peers) — v0.5.0
- ✓ Configurable TTL (per-blob TTL set by writer, replacing hardcoded 7-day default) — v0.5.0
- ✓ Tombstone TTL (tombstones expire after configurable period, garbage collected) — v0.5.0
- ✓ Build restructure (db/ as self-contained CMake component) — v0.5.0
- ✓ Documentation updates for all v0.5.0 changes — v0.5.0

- ✓ Docker container build with multi-stage Dockerfile and Release binaries — v0.6.0
- ✓ Protocol-compliant load generator with configurable workloads and JSON stats — v0.6.0
- ✓ Multi-node Docker Compose topology with health checks and late-joiner support — v0.6.0
- ✓ Performance benchmark suite: ingest, sync, multi-hop, late-joiner, trusted-vs-PQ — v0.6.0
- ✓ Structured benchmark report with hardware profiling and computed analysis — v0.6.0

- ✓ Sync resumption with per-peer per-namespace cursors for O(new) sync — v0.7.0
- ✓ Hash-then-sign protocol (ML-DSA-87 signs 32-byte SHA3-256 digest, not raw concatenation) — v0.7.0
- ✓ Namespace storage quotas (per-namespace byte/count limits enforced at ingest) — v0.7.0
- ✓ Crypto hot-path optimization (incremental SHA3, dedup-before-verify, OQS_SIG caching) — v0.7.0
- ✓ Deletion benchmarks in Docker suite (tombstone create/sync/GC) — v0.7.0
- ✓ Component self-containment (tests relocated into db/, stale artifacts removed) — v0.7.0

- ✓ Thread pool crypto offload (ML-DSA-87 verify + SHA3-256 hash dispatched to asio::thread_pool) — v0.8.0
- ✓ Custom XOR-fingerprint set reconciliation replacing O(N) hash list exchange with O(diff) — v0.8.0
- ✓ Sync rate limiting (per-peer cooldown, session limit, universal byte accounting) — v0.8.0
- ✓ Benchmark validated: +116% large-blob throughput, O(diff) confirmed — v0.8.0

- ✓ CMake version injection (build-time version from project(VERSION), git hash) — v0.9.0
- ✓ Startup config validation (fail-fast with accumulated error messages) — v0.9.0
- ✓ Timer cleanup (consolidated cancel_all_timers() for shutdown consistency) — v0.9.0
- ✓ Auto-reconnect with jittered exponential backoff (1s-60s) for all outbound peers — v0.9.0
- ✓ ACL-aware reconnect suppression (3 rejections → 600s backoff, SIGHUP reset) — v0.9.0
- ✓ Receiver-side inactivity timeout for dead peer detection (configurable, default 120s) — v0.9.0
- ✓ Rotating file logging with JSON structured format option — v0.9.0
- ✓ Startup integrity scan of all 7 sub-databases — v0.9.0
- ✓ Cursor compaction timer (6h, prunes disconnected peers) — v0.9.0
- ✓ Tombstone GC root cause documented (mmap geometry, not a bug) — v0.9.0
- ✓ Complete metrics output including quota/sync rejection counters — v0.9.0
- ✓ Crash recovery verified via Docker kill-9 test scenarios — v0.9.0
- ✓ Delegation quota enforcement verified (owner-attribution confirmed) — v0.9.0
- ✓ README + protocol docs current with v0.8.0 and v0.9.0 changes — v0.9.0

- ✓ SANITIZER CMake enum (asan/tsan/ubsan) replacing ENABLE_ASAN boolean — v1.0.0
- ✓ ASAN-clean full suite (469 tests, zero findings in db/ code) — v1.0.0
- ✓ TSAN-clean full suite (469 tests, zero data races in db/ code) — v1.0.0
- ✓ UBSAN-clean full suite (469 tests, zero UB findings in db/ code) — v1.0.0
- ✓ PEX SIGSEGV fix (root cause: AEAD nonce desync from concurrent SyncRejected writes) — v1.0.0
- ✓ 50-run E2E reliability validated (release 50/50, TSAN 50/50, ASAN 50/50) — v1.0.0

### Active

See REQUIREMENTS.md for v1.0.0 requirements.

## Current Milestone: v1.0.0 Database Layer Done

**Goal:** Prove chromatindb works correctly under adversarial conditions via Docker-based integration tests, sanitizer passes, and fixing whatever breaks — then open-source.

**Target features:**
- Full integration test suite from db/TESTS.md (~52 system-level tests across 12 categories)
- ASAN/TSAN/UBSAN sanitizer passes on full Catch2 test suite
- Duplicate-connection dedup verification (symmetric 2-node topology)
- Fix all issues discovered by tests and sanitizers
- PEX test SIGSEGV fix (pre-existing, deferred from v0.4.0)

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

Shipped v0.9.0 with 22,467 LOC C++20, 469 tests.
Built across 20 days total: v1.0 (3d), v2.0 (2d), v3.0 (2d), v0.4.0 (5d), v0.5.0 (2d), v0.6.0 (2d), v0.7.0 (2d), v0.8.0 (1d), v0.9.0 (1d).
9 milestones, 45 phases, 88 plans, 171 requirements total.

Tech stack: C++20, CMake, liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256), libsodium (ChaCha20-Poly1305, HKDF-SHA256), libmdbx, FlatBuffers, Standalone Asio (C++20 coroutines, thread_pool), xxHash (XXH3), Catch2, spdlog, nlohmann/json.

Three-layer architecture (building bottom-up):
- **Layer 1 (v0.9.0 SHIPPED): chromatindb** — production-hardened database node with connection resilience, operational tooling, and complete documentation
- **Layer 2 (FUTURE): Relay** — application semantics, owns a namespace
- **Layer 3 (FUTURE): Client** — mobile/desktop app, talks to relay

**Current milestone:** v1.0.0 — integration test suite (Docker-based system tests from db/TESTS.md), sanitizer passes (ASAN/TSAN/UBSAN), fix what breaks, open-source release.

**Benchmark results (v0.8.0, Ryzen 5 5600U, Docker):**
- 1 MiB ingest: 33.1 blobs/sec (+116% over v0.6.0 baseline of 15.3)
- Reconciliation scaling: 1050ms for 10-blob delta on 1000-blob namespace (O(diff) confirmed)
- Small namespace: no regression within 5% threshold

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
| Hash-then-sign (v0.7.0) | ML-DSA-87 signs/verifies the 32-byte SHA3-256 digest, not the raw concatenation. Reduces verification input from ~1 MiB to 32 bytes for large blobs. Pre-MVP breaking change. | Pending — PERF-04 |
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
| Tombstones permanent (TTL=0) | Deleted means deleted forever, no resurrection | ✓ Good — simple semantics |
| Tombstone data = 4-byte magic + 32-byte target hash | Self-verifiable on any node, replicates like blobs | ✓ Good — clean design |
| Delegation via signed blob in owner's namespace | Delegates sign with own key; verification: ownership OR valid delegation | ✓ Good — no protocol changes needed |
| Delegates write-only (no delete) | Deletion is owner-privileged; keeps security model simple | ✓ Good — clear privilege boundary |
| Revocation by tombstoning delegation blob | Reuses existing deletion infrastructure | ✓ Good — zero new mechanisms |
| Delegation index key = namespace:32 + SHA3-256(delegate_pubkey):32 | O(1) btree lookup on write hot-path | ✓ Good — efficient verification |
| Pub/sub: metadata-rich notifications | Namespace + seq + hash + size; subscriber fetches blob if wanted | ✓ Good — pull model, no data push |
| Subscriptions connection-scoped | No persistence across disconnects; relay can re-subscribe | ✓ Good — simple state management |
| No self-exclusion on notifications | Writing peer receives its own notifications | ✓ Good — consistent, simple |
| Single HKDF-derived blob key (not per-blob) | One key per node with context "chromatindb-dare-v1", AEAD AD = mdbx key | ✓ Good — simple, binds ciphertext to storage location |
| Envelope format [version][nonce][ciphertext+tag] | Self-describing, forward-compatible encryption format | ✓ Good — version byte enables future migration |
| Full startup scan for unencrypted data | Pre-release, no migration path needed | ✓ Good — catches misconfiguration |
| Lightweight handshake with mismatch fallback | Initiator proposes TrustedHello, responder replies PQRequired if untrusted | ✓ Good — graceful upgrade, no connection failure |
| TrustCheck lambda chain (PeerManager→Server→Connection) | Runtime trust decision injection without coupling layers | ✓ Good — clean dependency inversion |
| No ENABLE_ASAN/install()/CMAKE_BUILD_TYPE in db/ | YAGNI — sanitizers, install, build type are consumer concerns | ✓ Good — minimal self-contained component |
| SIGUSR1 + log grep for benchmark convergence polling | Uses existing metrics mechanism, no new API needed | ✓ Good — zero code changes to chromatindb |
| Full compose restart between PQ/trusted benchmark runs | SIGHUP reloads config but existing connections keep handshake type | ✓ Good — fair comparison with fresh connections |
| Runtime IP resolution via docker inspect for trusted_peers | Docker DNS names not accepted; IPs resolved at benchmark time | ✓ Good — dynamic, no hardcoded addresses |
| Zero-hash sentinel in seq_map on blob deletion | Preserves seq_num monotonicity for cursor change detection; fix at storage root cause, not cursor symptom | ✓ Good — seq_map entries never deleted, all seq_num consumers see monotonic values |
| Two-dispatch ingest pattern | blob_hash offload first (dedup gate), then build_signing_input+verify bundled. Duplicates skip ML-DSA-87. | ✓ Good — eliminates expensive verify for duplicate blobs |
| Custom XOR-fingerprint reconciliation (not negentropy) | SHA3-256 patching hassle for negentropy; ~550 LOC custom vs 1000+ LOC dep | ✓ Good — zero dependencies, O(diff) confirmed |
| ReconcileItems as final-exchange signal | Breaks ItemList echo loop in network protocol | ✓ Good — deterministic termination |
| Always reconcile, cursor skip only in Phase C | Bidirectional correctness: peer must discover what it is missing | ✓ Good — fixed subtle bidirectional sync bug |
| Universal byte accounting at top of on_peer_message | All messages metered uniformly; differentiated response per type | ✓ Good — sync traffic no longer bypasses rate limiter |
| Pool ref as constructor param / set_pool() for factory | Owned objects get pool in constructor; Connection (factory-created) gets set_pool() | ✓ Good — clean ownership semantics |
| configure_file output to source dir | Generates db/version.h in source tree so existing #include paths work without CMake include dir changes | ✓ Good — zero consumer changes |
| Error accumulation in config validation | validate_config collects all errors in vector, throws single exception — operator sees everything in one restart | ✓ Good — better operator experience |
| Shared sinks vector for logging | All loggers (default + named) use same sink set from init() — consistent file+console output | ✓ Good — no sink drift |
| Tombstone GC is mmap geometry, not a bug | used_bytes() returns mmap file size; freed pages reused internally. used_data_bytes() added for accurate B-tree metric | ✓ Good — documented, accurate metric available |
| Cursor compaction hardcoded 6h, connected-set criterion | YAGNI on config; prune any disconnected peer's cursors, not age-based | ✓ Good — simple, no config complexity |
| Reconnect with value copies across co_await | Prevents dangling refs on Server destruction during coroutine suspension | ✓ Good — fixed subtle lifetime bug |
| Receiver-side inactivity (not Ping sender) | Avoids AEAD nonce desync from bidirectional keepalive messages; zero wire protocol changes | ✓ Good — simpler, safer |
| Timestamp update at top of on_peer_message | Before rate limiting check — prevents false inactivity disconnects during rate-limited sessions | ✓ Good — correct ordering |
| Coroutine params by value (not const ref) | C++ coroutine frames copy the reference, not the value — stack-use-after-scope when caller returns | ✓ Good — caught by ASAN |
| Silent SyncRequest drop when peer syncing | Spawning detached SyncRejected coroutine races with sync initiator's writes, causing AEAD nonce desync | ✓ Good — fixed PEX SIGSEGV and flaky tests |
| recv_sync_msg executor transfer after offload() | offload() resumes on thread_pool thread; must co_await asio::post(ioc_) before accessing io_context-bound state | ✓ Good — caught by TSAN |
| UBSAN nonnull-attribute excluded globally | liboqs/libsodium __nonnull annotations on params that intentionally accept NULL — annotation bugs, not real UB | ✓ Good — scoped exclusion |
| Per-target UBSAN alignment for libmdbx | MDBX intentionally uses misaligned stores in mmap'd pages — safe on x86, technically UB per C11 | ✓ Good — scoped to mdbx only |

---
*Last updated: 2026-03-21 after Phase 46 (Sanitizers & Bug Fix)*
