# Milestones

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
