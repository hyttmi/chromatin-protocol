# Milestones

## v2.1.0 Compression, Filtering & Observability (Shipped: 2026-04-06)

**Phases completed:** 5 phases, 11 plans, 22 tasks

**Key accomplishments:**

- SyncNamespaceAnnounce (type 62) protocol: peers exchange replication scope after handshake, BlobNotify and reconciliation filtered by namespace intersection, SIGHUP triggers re-announce
- max_peers becomes SIGHUP-reloadable via mutable member, with graceful over-limit drain and warning log
- Two Docker E2E tests: namespace-filtered BlobNotify with 3-node topology proving blobs do not replicate to non-subscribing peers, and max_peers SIGHUP hot reload proving D-12 no-mass-disconnect guarantee
- Brotli compression in SDK envelope encryption with suite=0x02, 256-byte threshold, expansion fallback, and 100 MiB decompression bomb protection
- Rewrite Phase 87 docs from wire compression to SDK envelope compression: ROADMAP.md scope pivot, COMP-01..04 SDK-only requirements, PROTOCOL.md suite=0x02 cipher suite registry
- Per-client namespace subscription tracking with 256-cap enforcement and Notification filtering by subscription match in relay handle_node_message
- Three-state relay session lifecycle with jittered backoff UDS reconnection and subscription replay after reconnect
- Multi-relay failover in Python SDK: connect() accepts relay list, __aenter__ rotates on failure, _reconnect_loop cycles through relays with backoff between full cycles
- Migrated 26 connect() call sites across test and doc files to multi-relay signature, added Multi-Relay Failover documentation sections with current_relay and 4-arg on_reconnect examples
- HTTP /metrics endpoint exposing 16 Prometheus metrics (11 counters + 5 gauges) with SIGHUP-reloadable bind address, zero new dependencies
- PROTOCOL.md updated with SyncNamespaceAnnounce type 62 wire format, BlobNotify namespace filtering, and Prometheus /metrics operational section; README/SDK docs refreshed with v2.1.0 features (observability, compression, multi-relay, auto-reconnect)

---

## v1.7.0 Client-Side Encryption (Shipped: 2026-04-02)

**Phases completed:** 4 phases, 8 plans, 15 tasks

**Key accomplishments:**

- ML-KEM-1024 encryption keypair added to SDK Identity with 4-file persistence, from_public_keys() constructor, and NotARecipientError/MalformedEnvelopeError exceptions
- PQ multi-recipient envelope encryption with KEM-then-Wrap pattern, sorted stanzas, AEAD AD binding, and 31 comprehensive tests
- UserEntry binary codec with ML-DSA-87 KEM cross-key binding, DirectoryEntry dataclass, and DirectoryError exception
- Directory class with admin delegation, user self-registration, cached O(1) lookups, and pub/sub invalidation via drain-and-requeue
- Named group CRUD with GRPE binary codec, 5 Directory methods, and cache extension with latest-timestamp-wins resolution
- Three ChromatinClient convenience methods (write_encrypted, read_encrypted, write_to_group) composing envelope encryption with blob storage, plus GroupEntry re-exports
- Byte-level Client-Side Envelope Encryption section added to PROTOCOL.md with binary format tables, HKDF label registry (4 labels), KEM-then-Wrap pattern, AEAD parameters, and decryption steps
- Encryption API tables in README and end-to-end encryption workflow tutorial covering identity, directory, registration, groups, and encrypted I/O

---

## v1.6.0 Python SDK (Shipped: 2026-03-31)

**Phases completed:** 5 phases, 14 plans, 24 tasks

**Key accomplishments:**

- Pip-installable chromatindb package with 11-class exception hierarchy, FlatBuffers codegen for all 58 message types, and 17 validation tests
- C++ binary producing JSON known-answer vectors for SHA3-256, HKDF-SHA256, ChaCha20-Poly1305, build_signing_input, ML-DSA-87, and namespace derivation
- All crypto primitives (SHA3-256, HKDF, AEAD, signing input) and ML-DSA-87 identity management validated byte-identical to C++ via test vectors, with FlatBuffers wire helpers
- Length-prefixed frame IO with ChaCha20-Poly1305 AEAD encryption, counter-based nonce management, and 20 unit tests
- ML-KEM-1024 PQ handshake initiator with HKDF session key derivation, background reader transport with request_id dispatch, and ChromatinClient async context manager with 21 unit tests
- Frozen result dataclasses and binary codec for all 5 core data operations (write/read/delete/list/exists) with 44 unit tests
- 5 async blob lifecycle methods (write/read/delete/list/exists) on ChromatinClient with 31 unit tests using mock transport
- 9 integration tests verify all 5 data operations (write, read, delete, list, exists) plus pagination and lifecycle against live KVM relay at 192.168.1.200:4201
- 13 frozen dataclasses and 20 binary encode/decode functions for all 10 query types plus 3 pub/sub message types, with full TDD unit test coverage
- 10 async query methods, subscribe/unsubscribe/notifications, and auto-cleanup wired into ChromatinClient with full TDD unit test coverage
- 11 integration tests proving all 10 query types plus pub/sub lifecycle work end-to-end against live KVM relay at 192.168.1.200:4201
- PyPI-ready packaging metadata, SDK README with 19-method API table, and 187-line getting started tutorial covering identity through pub/sub
- Fixed HKDF salt discrepancy (empty salt, not SHA3-256(pubkeys)) and added SDK Client Notes section with 6 protocol implementation gotchas

---

## v1.5.0 Documentation & Distribution (Shipped: 2026-03-28)

**Phases completed:** 2 phases, 4 plans, 8 tasks

**Key accomplishments:**

- Six dist/ files for bare-metal deployment: hardened systemd units, minimal JSON configs, sysusers.d user/group, tmpfiles.d directory structure
- POSIX install.sh with install/uninstall modes, FHS deployment of binaries + configs + systemd units, identity key generation, and config preservation on reinstall
- Version bump to v1.5.0 across CMakeLists.txt and README.md, plus full db/README.md refresh with relay architecture, deployment kit, 58 message types, and 9 v1.4.0 query features
- Verified all 58 message types in PROTOCOL.md against peer_manager.cpp encoder source, fixed 2 byte-offset discrepancies in NamespaceListResponse and StorageStatusResponse

---

## v1.4.0 Extended Query Suite (Shipped: 2026-03-27)

**Phases completed:** 3 phases, 7 plans, 14 tasks

**Key accomplishments:**

- FlatBuffers types 41-46 for node-level queries, O(1) tombstone count and cursor-scanned delegation count in Storage, relay filter expanded to 26 client types
- Three new coroutine-IO handlers for NamespaceList (paginated), StorageStatus (44-byte global stats), and NamespaceStats (41-byte per-namespace stats) with full integration tests
- 6 new FlatBuffers enum types (47-52), Storage::list_delegations() with cursor prefix scan, relay filter expanded to 32 client types
- MetadataRequest, BatchExistsRequest, DelegationListRequest handlers with binary wire protocol and 3 integration tests covering found/not-found, batch existence, and delegation listing
- 6 new FlatBuffers enum values (53-58) for BatchRead/PeerInfo/TimeRange, relay filter expanded from 32 to 38 types, NodeInfoResponse backfilled with all v1.4.0 types (41-58)
- Three coroutine-IO handlers for BatchRead (size-capped multi-blob fetch), PeerInfo (trust-gated peer topology), and TimeRange (timestamp-filtered blob query) with 3 integration tests (29 assertions)
- PROTOCOL.md updated with byte-level wire format for all 9 v1.4.0 query pairs (types 41-58), all 14 requirements marked complete

---

## v1.3.0 Protocol Concurrency & Query Foundation (Shipped: 2026-03-26)

**Phases:** 4 (61-64) | **Plans:** 8 | **Tasks:** 10
**Velocity:** 34 commits | 54 files changed (+4,810 / -835)
**Requirements:** 12/12
**Timeline:** 2 days (2026-03-24 → 2026-03-26)

**Key accomplishments:**

- **Request correlation:** request_id plumbed through transport envelope, codec, connection, relay, and all dispatch handlers
- **Concurrent dispatch:** IO-thread transfer for Data/Delete handlers after engine offload, preventing AEAD nonce desync
- **Blob existence check:** ExistsRequest/ExistsResponse (types 37/38) with key-only has_blob() lookup
- **Node capability discovery:** NodeInfoRequest/NodeInfoResponse (types 39/40) with version, peers, storage, 20 supported types
- **Wire spec:** PROTOCOL.md updated with request_id semantics, new message wire formats, 40-entry type table
- **Documentation:** README.md and db/README.md updated with v1.3.0 capabilities and dispatch model

**Archive:** [v1.3.0-ROADMAP.md](milestones/v1.3.0-ROADMAP.md) | [v1.3.0-REQUIREMENTS.md](milestones/v1.3.0-REQUIREMENTS.md)

---

## v1.2.0 Relay & Client Protocol (Shipped: 2026-03-23)

**Phases:** 4 (57-60) | **Plans:** 8 | **Tasks:** 16
**Velocity:** 44 commits | 60 files changed (+7,817 / -240)
**Requirements:** 16/16
**Timeline:** 1 day (2026-03-23)

**Key accomplishments:**

- **Client Protocol Suite:** Added WriteAck, Read, List, and Stats wire types (31-37), enabling clients to query and interact with the database without being a full peer.
- **PQ-Authenticated Relay:** Launched chromatindb_relay, a standalone binary providing a post-quantum secure gateway via TCP with mutual authentication (ML-KEM-1024 + ML-DSA-87).
- **Trusted UDS Bridge:** Established a secure "arm's length" connection between the relay and node using Unix Domain Sockets and the TrustedHello handshake.
- **Architectural Cleanup:** Consolidated all hex encoding and test helper logic into two shared header-only libraries (db/util/hex.h and db/tests/test_helpers.h).
- **Policy Enforcement:** Created root-level GEMINI.md mandating a Zero Duplication Policy for all future protocol development.

**Archive:** [v1.2.0-ROADMAP.md](milestones/v1.2.0-ROADMAP.md) | [v1.2.0-REQUIREMENTS.md](milestones/v1.2.0-REQUIREMENTS.md)

---

## v1.1.0 Operational Polish & Local Access (Shipped: 2026-03-22)

**Phases completed:** 4 phases, 6 plans, 13 tasks

**Key accomplishments:**

- Stale artifact removal, 23-phase milestone archival (v0.4.0/v0.7.0/v0.8.0/v1.0.0), CMake version bump to 1.1.0, and documentation updates with Testing section and version line
- Configurable expiry scan interval with SIGHUP hot-reload and shared constexpr sync rejection header with 8 reason codes
- Step 0c timestamp validation rejecting blobs >1hr future or >30d past before any crypto, with 8-code sync rejection protocol documented
- Automatic mdbx compaction on configurable timer (default 6h) with live copy, file swap, SIGHUP reload, and SIGUSR1 metrics
- Config uds_path field, Connection refactored to generic stream socket, and UdsAcceptor for Unix domain socket listening
- UDS acceptor wired into PeerManager with startup logging, 9 new tests, and PROTOCOL.md documentation

---

## v1.0.0 Database Layer Done (Shipped: 2026-03-22)

**Phases completed:** 7 phases, 23 plans, 0 tasks

**Key accomplishments:**

- (none recorded)

---

## v0.9.0 Connection Resilience & Hardening (Shipped: 2026-03-20)

**Phases:** 4 (42-45) | **Plans:** 8 | **LOC:** 22,467 C++ (+464)
**Tests:** 408+ tests | **Requirements:** 16/16
**Timeline:** 1 day (2026-03-20)
**Git range:** 57 files changed, 7,007 insertions, 184 deletions

**Key accomplishments:**

- CMake version injection (configure_file template) replacing stale hardcoded version.h
- Startup config validation with error accumulation (all failures reported at once)
- Production-grade logging: rotating file sink, JSON structured format, multi-sink architecture
- Storage hardening: startup integrity scan (7 sub-databases), cursor compaction timer (6h), tombstone GC root cause documented (mmap geometry)
- Auto-reconnect with jittered exponential backoff (1s-60s) for all outbound peers including PEX-discovered
- ACL-aware reconnect suppression (3 rejections → 600s extended backoff, SIGHUP reset)
- Receiver-side inactivity timeout for dead peer detection (configurable, default 120s)
- Crash recovery verified via Docker kill-9 test scenarios with data integrity checks
- Delegation quota enforcement verified (5 Catch2 tests proving owner-attribution)
- Complete documentation update: README (25 config fields, 8 new features) + PROTOCOL.md (SyncRejected, rate limiting, inactivity detection)

**Tech debt:**

- Pre-existing PEX test SIGSEGV (test_daemon.cpp:296) — coroutine lifetime during teardown, deferred to v1.0.0

**Archive:** [v0.9.0-ROADMAP.md](milestones/v0.9.0-ROADMAP.md) | [v0.9.0-REQUIREMENTS.md](milestones/v0.9.0-REQUIREMENTS.md)

---

## v0.8.0 Protocol Scalability (Shipped: 2026-03-19)

**Phases:** 4 (38-41) | **Plans:** 8 | **Commits:** 51 | **LOC:** 22,003 C++ (+8,844)
**Tests:** 408 tests | **Requirements:** 12/12
**Timeline:** 1 day (2026-03-19)
**Git range:** 74 files changed, 10,871 insertions, 2,027 deletions

**Key accomplishments:**

- Thread pool crypto offload: ML-DSA-87 verify and SHA3-256 hash dispatched to asio::thread_pool via two-dispatch ingest pattern
- Custom XOR-fingerprint set reconciliation: O(diff) sync replacing O(N) hash list exchange (~550 LOC, zero external dependencies)
- 3 new wire messages (ReconcileInit=27, ReconcileRanges=28, ReconcileItems=29), HashList=12 removed
- Sync rate limiting: per-peer cooldown, session limit, and universal byte accounting with SyncRejected=30 wire type
- Benchmark validated: 1 MiB ingest 33.1 blobs/sec (+116% over v0.6.0 baseline), O(diff) confirmed (1050ms for 10 blobs on 1000-blob namespace)

**Tech debt:**

- max_sync_sessions_ config values > 1 have no effect (boolean syncing flag, effective max=1). By design, field for future expansion.

**Archive:** [v0.8.0-ROADMAP.md](milestones/v0.8.0-ROADMAP.md) | [v0.8.0-REQUIREMENTS.md](milestones/v0.8.0-REQUIREMENTS.md)

---

## v0.7.0 Production Readiness (Shipped: 2026-03-18)

**Phases:** 6 (32-37) | **Plans:** 12 | **LOC:** 18,000+ C++
**Tests:** 313+ tests | **Requirements:** 20/20
**Timeline:** 2 days (2026-03-17 -> 2026-03-18)

**Key accomplishments:**

- Sync resumption with per-peer per-namespace cursors for O(new) sync
- Hash-then-sign protocol (ML-DSA-87 signs 32-byte SHA3-256 digest, not raw concatenation)
- Namespace storage quotas (per-namespace byte/count limits enforced at ingest)
- Crypto hot-path optimization (incremental SHA3, dedup-before-verify, OQS_SIG caching)
- Deletion benchmarks in Docker suite (tombstone create/sync/GC)
- Component self-containment (tests relocated into db/, stale artifacts removed)

**Known issue:**

- Sync protocol has O(N) hash list exchange flaw — breaks at ~3.4M blobs/namespace. Addressed in v0.8.0.
- Sync traffic bypasses rate limiting. Addressed in v0.8.0.

**Archive:** [v0.7.0-ROADMAP.md](milestones/v0.7.0-ROADMAP.md) | [v0.7.0-REQUIREMENTS.md](milestones/v0.7.0-REQUIREMENTS.md)

---

## v0.6.0 Real-World Validation (Shipped: 2026-03-16)

**Phases:** 5 (27-31) | **Plans:** 6 | **Files:** 40 modified (+6,451 lines) | **LOC:** 17,775 C++
**Requirements:** 14/14 (DOCK-01/02, LOAD-01-04, PERF-01-05, OBS-01-03)
**Timeline:** 2 days (2026-03-15 -> 2026-03-16)

**Key accomplishments:**

- Multi-stage Dockerfile with debian:bookworm-slim runtime, BuildKit cache, non-root user
- Protocol-compliant C++ load generator (chromatindb_loadgen) with timer-driven scheduling, mixed sizes, JSON stats
- Docker Compose 3-node chain topology with health checks, named volumes, late-joiner profile
- 5-scenario benchmark suite: ingest (3 sizes), sync/multi-hop latency, late-joiner catch-up, trusted-vs-PQ comparison
- Structured markdown report with hardware profiling, computed analysis, and combined JSON summary
- `run-benchmark.sh` — 961-line end-to-end automation pipeline with `--report-only` regeneration

**Benchmark baseline (Ryzen 5 5600U, Docker):**

- 1K blobs: 50.2 blobs/sec, p50=22ms
- 100K blobs: 50.2 blobs/sec, 4.9 MiB/sec
- 1M blobs: 15.3 blobs/sec, p99=9.4s (CPU-bound — crypto bottleneck)
- PQ vs trusted overhead: <1% (negligible)
- Late-joiner catch-up: 1.0s for 200 blobs

**Known issue:**

- Large blob (1M) crypto throughput is CPU-bound at 96% (sync verification). Must address before 1.0.0.

**Tech debt carried forward:**

- `--entrypoint` fix for loadgen in Docker (committed post-execution)
- jq `-s` slurp fix for docker stats parsing (committed post-execution)

**Archive:** [v0.6.0-ROADMAP.md](milestones/v0.6.0-ROADMAP.md) | [v0.6.0-REQUIREMENTS.md](milestones/v0.6.0-REQUIREMENTS.md)

---

## v0.5.0 Hardening & Flexibility (Shipped: 2026-03-15)

**Phases:** 5 (22-26) | **Plans:** 6 | **Commits:** 32 | **LOC:** 17,124 C++ (+2,601)
**Tests:** 284 tests | **Requirements:** 13/13
**Timeline:** 2 days (2026-03-14 -> 2026-03-15)

**Key accomplishments:**

- Build restructured — db/ is a self-contained CMake component with guarded FetchContent for composable builds
- Writer-controlled TTL replaces hardcoded 7-day constant; tombstone_map cleanup in expiry scan
- ChaCha20-Poly1305 encryption at rest for all stored blob payloads with HKDF-derived keys and auto-generated master key
- Lightweight handshake for localhost/trusted peers skips ML-KEM-1024 with mismatch fallback to full PQ
- README documents all v0.5.0 features (DARE, trusted peers, configurable TTL)

**Tech debt carried forward:**

- has_tombstone_for still uses O(n) namespace scan (from v3.0 — deletion is rare)
- No milestone audit performed (all requirements verified during phase execution)

**Archive:** [v0.5.0-ROADMAP.md](milestones/v0.5.0-ROADMAP.md) | [v0.5.0-REQUIREMENTS.md](milestones/v0.5.0-REQUIREMENTS.md)

---

## v3.0 Real-time & Delegation (Shipped: 2026-03-08)

**Phases:** 4 (12-15) | **Plans:** 8 | **Commits:** 31 | **LOC:** 14,152 C++ (+6,591)
**Tests:** 255 tests (59 new) | **Requirements:** 16/16
**Timeline:** 2 days (2026-03-07 -> 2026-03-08)

**Key accomplishments:**

- Tombstone-based blob deletion with sync propagation and tombstone-before-blob rejection
- liboqs algorithm stripping (ML-DSA-87 + ML-KEM-1024 only) for faster builds
- Signed delegation blobs for multi-writer namespaces with O(1) indexed verification
- Real-time pub/sub notifications with connection-scoped subscriptions and tombstone events
- Comprehensive README with build, config, CLI, deployment scenarios, and crypto stack docs
- Standalone benchmark binary with crypto, data path, sync, handshake, and notification benchmarks

**Known gaps (documentation-only):**

- Missing VERIFICATION.md for phases 12 and 14 (all code works, all tests pass)

**Tech debt carried forward:**

- has_tombstone_for uses O(n) namespace scan (deletion is rare)
- No combined delegation + notification E2E test (code correct, untested as composed scenario)

**Archive:** [v3.0-ROADMAP.md](milestones/v3.0-ROADMAP.md) | [v3.0-REQUIREMENTS.md](milestones/v3.0-REQUIREMENTS.md)

---

## v2.0 Closed Node Model (Shipped: 2026-03-07)

**Phases:** 3 (9-11) | **Plans:** 8 | **Commits:** 38 | **LOC:** 11,027 C++
**Tests:** 196 tests (41 new) | **Requirements:** 14/14
**Timeline:** 2 days (2026-03-05 -> 2026-03-07)

**Key accomplishments:**

- Source restructured to db/ layout with chromatindb:: namespace (60 files, 155 tests preserved)
- Closed node model with allowed_keys ACL gating at connection level (open/closed mode)
- PEX disabled in closed mode at all 4 protocol points
- SIGHUP hot-reload of allowed_keys with immediate peer revocation
- 100 MiB blob support with Step 0 oversized rejection before crypto
- Memory-efficient sync: index-only hash reads + one-blob-at-a-time transfer with adaptive timeout

**Tech debt carried forward:**

- Expired blob hashes included in sync hash lists (documented intentional trade-off)
- Wire protocol strings "chromatin-init-to-resp-v1" retained (protocol identifiers, not namespace refs)

**Archive:** [v2.0-ROADMAP.md](milestones/v2.0-ROADMAP.md) | [v2.0-REQUIREMENTS.md](milestones/v2.0-REQUIREMENTS.md)

---

## v1.0 MVP (Shipped: 2026-03-05)

**Phases:** 8 | **Plans:** 21 | **Commits:** 80 | **LOC:** 9,449 C++
**Tests:** 155 tests, 586 assertions | **Requirements:** 32/32
**Timeline:** 3 days (2026-03-03 -> 2026-03-05)
**Git range:** `490d2bc..6553b9b`

**Key accomplishments:**

- Post-quantum crypto stack with RAII wrappers (ML-DSA-87, ML-KEM-1024, SHA3-256, ChaCha20-Poly1305, HKDF-SHA256)
- ACID-safe storage engine with libmdbx (blob CRUD, content-addressed dedup, sequence indexing, TTL expiry)
- Blob ingest pipeline with fail-fast validation, namespace ownership verification, and write ACKs
- PQ-encrypted transport with ML-KEM-1024 key exchange, ML-DSA-87 mutual auth, and ChaCha20-Poly1305 encrypted framing
- Full peer system: daemon CLI, bootstrap discovery, peer exchange (PEX), bidirectional hash-list diff sync
- Complete verification: all 32 requirements satisfied, all 5 E2E flows validated

**Tech debt carried forward:**

- Phase 4/5 SUMMARYs lack requirements-completed frontmatter
- Dead Config::storage_path field (parsed but never consumed)
- No standalone verification docs for Phases 7/8

**Archive:** [v1.0-ROADMAP.md](milestones/v1.0-ROADMAP.md) | [v1.0-REQUIREMENTS.md](milestones/v1.0-REQUIREMENTS.md)

---
