# Project Retrospective

*A living document updated after each milestone. Lessons feed forward into future planning.*

## Milestone: v1.5.0 — Documentation & Distribution

**Shipped:** 2026-03-28
**Phases:** 2 | **Plans:** 4 | **Sessions:** ~1

### What Was Built
- dist/ production deployment kit: hardened systemd units (ProtectSystem=strict, NoNewPrivileges=yes), default JSON configs, sysusers.d/tmpfiles.d, POSIX install.sh with install/uninstall modes
- Full documentation refresh: CMakeLists.txt version bump 1.1.0→1.5.0, README.md v1.5.0, db/README.md with relay section + deployment section + 58 message types + 26 config fields
- PROTOCOL.md systematic verification against peer_manager.cpp encoder source — found and fixed 2 real byte-offset discrepancies (NamespaceListResponse field order, StorageStatusResponse field sizes)

### What Worked
- Smallest milestone (2 phases, 4 plans) — completed in a single session with auto-advance
- Research phase correctly identified all stale documentation values before planning — zero rework during execution
- Parallel execution of plans 69-01 and 69-02 (no file overlap) saved significant time
- PROTOCOL.md verification caught real bugs: NamespaceListResponse had field order swapped, StorageStatusResponse had wrong field size (4 vs 8 bytes for total_blobs)
- All phase verifications passed first time

### What Was Inefficient
- Nothing significant — this was a clean docs-only milestone with no surprises

### Patterns Established
- Documentation verification against source code as a formal phase — catches drift that accumulates over multiple milestones
- dist/ decoupled from CMake build system (no install() targets, no CPack) — simpler, more portable

### Key Lessons
- Protocol documentation drifts even when updated per-phase — systematic source comparison is the only reliable verification
- Small focused milestones (2 phases) execute cleanly in a single auto-advance chain

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~1
- Notable: Second-smallest milestone by phase count; auto-advance chain completed the entire milestone in one session

---

## Milestone: v1.4.0 — Extended Query Suite

**Shipped:** 2026-03-27
**Phases:** 3 | **Plans:** 7 | **Sessions:** ~2

### What Was Built
- 18 new FlatBuffers enum values (types 41-58) for 9 query request/response pairs
- Relay message filter expanded from 20 to 38 client-allowed types across 3 phases
- Storage helpers: count_tombstones() (O(1) via map stat), count_delegations() (cursor prefix scan), list_delegations() (DelegationEntry vector)
- 6 coroutine-IO handlers: NamespaceList (paginated), StorageStatus (44-byte), NamespaceStats (41-byte), MetadataRequest, BatchExistsRequest, DelegationListRequest
- 3 coroutine-IO handlers: BatchReadRequest (size-capped multi-blob fetch), PeerInfoRequest (trust-gated), TimeRangeRequest (timestamp-filtered)
- NodeInfoResponse expanded from 20 to 38 supported_types for SDK capability discovery
- PROTOCOL.md v1.4.0 Query Extensions section with byte-level wire format for all 9 message pairs
- 12+ integration tests covering all query types (found/not-found, pagination, trust gating, batch, range)

### What Worked
- Entire milestone completed in 2 days — 3 phases, 7 plans, 34 commits, +2342 lines across 11 code files
- Layered phase design worked perfectly: schema+relay (wave 1) → handlers+tests (wave 2) → docs (wave 2 parallel)
- Phase 65/66/67 each followed identical structure (schema plan → handler plan) — predictable, fast execution
- Parallel subagent execution for Wave 2 of Phase 67 (67-02 handlers + 67-03 docs) saved significant time
- All 3 phase verifications passed first time — 9/9 must-haves each
- Requirements closure was clean: 14/14 checked off, 1 intentionally dropped with rationale

### What Was Inefficient
- summary-extract tool returned N/A for all summaries — metadata extraction from SUMMARY.md frontmatter failed silently
- Pre-existing flaky E2E peer tests ("closed mode accepts authorized peer and syncs", "closed mode disables PEX discovery") still not migrated to Docker — continues to create noise during regression testing

### Patterns Established
- Trust-gated response pattern: different response detail levels based on connection trust status (PeerInfoRequest)
- Cumulative size cap with truncation flag for batch operations (BatchReadRequest)
- Seq_map scan with timestamp post-filter for time-range queries (cheaper than dedicated time index)
- DelegationEntry struct for typed delegation map results from cursor prefix scan

### Key Lessons
1. 3-phase milestones with consistent schema→handler structure are the sweet spot for query expansion — predictable, parallelizable
2. Dropping QUERY-05 (HealthRequest) early was the right call — NodeInfoResponse already covered it, no wasted effort
3. Wave-2 parallelization of handlers + docs is safe and effective when they touch different files
4. Flaky E2E tests (backlog 999.6) are now consistently the only noise in regression gates — fixing these would make regression gates fully clean

### Cost Observations
- Model mix: 100% quality profile (opus for executors, sonnet for verifier)
- Sessions: ~2 across 2 days
- Notable: Fastest per-plan velocity yet — 7 plans in ~1 session of active work. Pattern reuse from v1.3.0 handlers made execution mechanical.

---

## Milestone: v1.3.0 — Protocol Concurrency & Query Foundation

**Shipped:** 2026-03-26
**Phases:** 4 | **Plans:** 8 | **Sessions:** ~2

### What Was Built
- request_id correlation plumbed through transport envelope, codec, connection, relay, and all dispatch handlers (Phase 61)
- IO-thread transfer for Data/Delete handlers after engine offload — AEAD nonce safety under concurrency (Phase 62)
- ExistsRequest/ExistsResponse (types 37/38) with key-only has_blob() storage lookup (Phase 63)
- NodeInfoRequest/NodeInfoResponse (types 39/40) with version, uptime, peers, storage, 20 supported types (Phase 63)
- PROTOCOL.md wire spec with request_id semantics, new message byte-level formats, 40-entry type table (Phase 64)
- README.md and db/README.md updated with v1.3.0 capabilities, dispatch model, 551 unit tests (Phase 64)

### What Worked
- Entire milestone completed in 2 days — 4 phases, 8 plans, 34 commits, 54 files changed
- Phase 61 plans 02 and 03 were naturally combined into a single commit — correct granularity emerged during execution
- Offload pattern from v0.8.0 (thread pool dispatch) was directly reused for concurrent dispatch — zero new async primitives needed
- has_blob() key-existence check was trivial to add to Storage (MDBX cursor seek without value read) — elegant and efficient
- NodeInfoResponse supported_types list enables SDK capability discovery without version parsing — forward-compatible design
- Documentation phase (64) was pure documentation — no code changes needed, clean milestone boundary

### What Was Inefficient
- Phase 61 plan 03 SUMMARY.md was never created (work combined into plan 02 commit) — paperwork gap
- CONC-01/02/05 checkboxes in REQUIREMENTS.md were never checked off despite Phase 61 being complete — bookkeeping gap
- Phase 61 checkbox in ROADMAP.md showed unchecked `[ ]` despite progress table showing 3/3 Complete — inconsistent state

### Patterns Established
- IO-thread transfer after engine offload: co_await asio::post(ioc_) before send_message/metrics for thread-safe AEAD access
- Dispatch model classification: inline (Subscribe, Ping, etc.), coroutine-IO (Read/List/Stats/Exists/NodeInfo), coroutine-offload-transfer (Data/Delete)
- Binary wire format for rich responses: length-prefixed strings + big-endian integers (NodeInfoResponse)
- Key-only storage lookup: MDBX cursor seek without value read for existence checks

### Key Lessons
1. Combined commits for tightly coupled plans are natural — forcing separate commits for plan-02 and plan-03 of a 3-wave phase adds overhead without value
2. Bookkeeping gaps (unchecked requirements, missing summaries) accumulate when plan execution is fast — the velocity that makes 2-day milestones possible also makes it easy to skip checkboxes
3. Reusing proven patterns (offload→transfer, relay filter update, binary wire format) keeps velocity high — v1.3.0 was mostly composition of existing patterns
4. Documentation-only phases work well as the final phase — no risk of code changes invalidating docs

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~2 across 2 days
- Notable: Documentation phase completed in minutes — pure markdown updates with no code changes

---

## Milestone: v1.2.0 — Relay & Client Protocol

**Shipped:** 2026-03-23
**Phases:** 4 | **Plans:** 8 | **Sessions:** ~1

### What Was Built
- Client protocol wire types: WriteAck, ReadRequest/Response, ListRequest/Response, StatsRequest/Response (types 31-37)
- PQ-authenticated relay binary (chromatindb_relay) with ML-KEM-1024 + ML-DSA-87 mutual auth
- Bidirectional UDS forwarding with default-deny message filter (16 client operation types allowed)
- Shared utility libraries: db/util/hex.h and db/tests/test_helpers.h eliminating 570+ lines of duplication
- Root-level GEMINI.md with Zero Duplication Policy

### What Worked
- Full milestone in 1 day — 4 phases, 8 plans, 44 commits, 60 files (+7,817 / -240)
- Relay architecture (TCP client → relay → UDS → node) cleanly separates untrusted and trusted zones
- SSH-style .key/.pub identity for relay was a familiar infrastructure pattern — zero design debate
- Default-deny message filter was the right security posture — explicitly allow rather than block
- Shared test_helpers.h immediately reduced duplication across 15+ test files

### What Was Inefficient
- No retrospective was written at milestone completion — process gap
- test_helpers.h extraction required touching many test files simultaneously — large but mechanical change

### Patterns Established
- Relay as security boundary: untrusted TCP clients never touch the node directly
- TrustedHello over UDS: one client session = one UDS connection, trust via local socket ownership
- Default-deny message filter: only explicitly listed client operations pass through
- SSH-style identity files: .key/.pub siblings for relay key management
- Zero Duplication Policy: all repeating tasks must use single source of truth

### Key Lessons
1. Relay architecture validates the three-layer design — clean separation between untrusted clients and trusted node
2. UDS as internal transport is ideal for co-located relay+node — no network overhead, OS-level access control
3. Code deduplication phases pay for themselves immediately — 570 lines removed, shared helpers used everywhere
4. Zero Duplication Policy should be enforced from project start, not retrofit

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~1
- Notable: Third consecutive 1-day milestone (v1.0.0, v1.1.0, v1.2.0)

---

## Milestone: v1.0.0 — Database Layer Done

**Shipped:** 2026-03-22
**Phases:** 7 | **Plans:** 23 | **Sessions:** ~8

### What Was Built
- SANITIZER CMake enum (asan/tsan/ubsan) replacing ENABLE_ASAN boolean
- Full suite ASAN/TSAN/UBSAN clean (469 tests, zero findings in db/ code)
- PEX SIGSEGV root cause and fix (AEAD nonce desync from concurrent SyncRejected writes)
- 50-run E2E reliability (release 50/50, TSAN 50/50, ASAN 50/50)
- 54 Docker integration tests across 12 categories (crypto, ACL, DR, DoS, TTL, E2E, stress, fuzz)
- 5-node SIGKILL churn (60 cycles, 30 min), 1000-namespace scaling, protocol fuzzing

### What Worked
- Sanitizer approach caught real bugs: TSAN found data races, ASAN found lifetime issues, UBSAN found annotation bugs in deps
- PEX SIGSEGV root cause analysis was thorough — traced to concurrent SyncRejected writes causing AEAD nonce desync
- Docker integration test infrastructure provides repeatable, isolated testing environment
- 50-run reliability testing gave high confidence in stability

### What Was Inefficient
- 7 phases for what was essentially a quality gate — could potentially have been fewer phases with more plans each
- No accomplishments were recorded in MILESTONES.md at the time — retrospective written retroactively

### Key Lessons
1. Sanitizers should be enabled from the first milestone — retrofitting catches bugs but the earlier they're found, the cheaper they are
2. Concurrent write bugs in AEAD protocols are subtle — the PEX SIGSEGV took significant investigation
3. Docker integration tests are worth the infrastructure investment — 54 tests across 12 categories is comprehensive

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~8 across 2 days
- Notable: Most sessions per milestone since v1.0 — quality/verification work requires iteration

---

## Milestone: v0.9.0 — Connection Resilience & Hardening

**Shipped:** 2026-03-20
**Phases:** 4 | **Plans:** 8 | **Sessions:** ~1

### What Was Built
- CMake version injection replacing stale hardcoded version.h, plus startup config validation with error accumulation
- Consolidated cancel_all_timers() for safe shutdown (7 timers including new cursor compaction + inactivity)
- Multi-sink logging: rotating file + console, JSON structured format, shared sinks vector
- Storage hardening: startup integrity scan (7 sub-databases), cursor compaction (6h), tombstone GC root cause documented
- Auto-reconnect with jittered exponential backoff (1s-60s) for all outbound peers including PEX-discovered
- ACL-aware reconnect suppression: 3 rejections → 600s extended backoff, SIGHUP reset
- Receiver-side inactivity timeout (configurable, default 120s) for dead peer detection
- Crash recovery verified via Docker kill-9 tests; delegation quota enforcement verified (5 Catch2 tests)
- Complete documentation update: README (25 config fields, 8 features) + PROTOCOL.md (SyncRejected, rate limiting, inactivity)

### What Worked
- Entire milestone completed in 1 day — 4 phases, 8 plans, same velocity as v0.8.0
- Phase ordering was optimal: foundation (config/timers) → storage/logging → network (builds on both) → verification/docs (covers all)
- cancel_all_timers() pattern from Phase 42 immediately paid off in Phase 43 (cursor timer) and Phase 44 (inactivity timer) — one line each
- Receiver-side inactivity detection was the right design call: zero wire protocol changes, avoids AEAD nonce desync
- Tombstone GC investigation resolved a long-standing mystery (not a bug, mmap geometry) with minimal effort
- Milestone audit caught two real integration issues (used_data_bytes orphaned, README inaccuracy) — both trivially fixed

### What Was Inefficient
- Pre-existing PEX test SIGSEGV was a distraction during Phase 44 — confirmed pre-existing but cost investigation time
- ROADMAP.md still had v0.9.0 success criteria using "configurable age threshold" for cursor compaction when implementation used connected-set — planning/implementation divergence caught by audit

### Patterns Established
- Value copies across co_await for coroutine safety (reconnect_loop)
- Direct method call for cross-component signaling (notify_acl_rejected, clear_reconnect_state)
- Timestamp update at top of message handler (before rate limiting) to prevent false inactivity disconnects
- Scoped read txn pattern for libmdbx (close txn before methods that open their own)
- Error accumulation pattern for config validation (all failures at once)

### Key Lessons
1. Foundation phases (config, timers, logging) have compounding returns — invest early
2. Receiver-side detection is almost always simpler than sender-side probing for protocol extensions
3. "Not a bug" is a valid and valuable finding — tombstone GC investigation closed a known issue with documentation
4. Planning/implementation divergence happens silently — milestone audit is the last defense
5. One-day milestones remain achievable at 22K+ LOC when building on a stable foundation

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~1 across 1 day
- Notable: Second consecutive 1-day milestone. Average ~19 min/plan — steady velocity maintained.

---

## Milestone: v0.8.0 — Protocol Scalability

**Shipped:** 2026-03-19
**Phases:** 4 | **Plans:** 8 | **Sessions:** ~1

### What Was Built
- Thread pool crypto offload: ML-DSA-87 verify and SHA3-256 hash dispatched to asio::thread_pool with two-dispatch ingest pattern
- Custom XOR-fingerprint range-based set reconciliation: O(diff) sync replacing O(N) hash list exchange (~550 LOC, zero dependencies)
- 3 new wire message types (ReconcileInit=27, ReconcileRanges=28, ReconcileItems=29), HashList=12 removed
- Sync rate limiting: per-peer cooldown, concurrent session limit, universal byte accounting with SyncRejected=30
- Benchmark validation: +116% large-blob throughput, O(diff) confirmed at 1050ms for 10/1000 delta

### What Worked
- Entire milestone completed in a single day — 4 phases, 8 plans, 51 commits
- Phase dependency ordering was correct: thread pool (protocol-agnostic) → reconciliation (largest change) → rate limiting (benefits from reconciliation) → benchmarks (validates stack)
- Two-dispatch ingest pattern was an elegant optimization discovered during implementation — duplicates skip expensive ML-DSA-87 verify entirely
- Custom reconciliation was simpler than expected: ~550 LOC vs estimated 400-500, and zero external dependency headaches
- The "always reconcile, cursor skip only in Phase C" fix was caught immediately by daemon E2E tests — good test coverage from v1.0 paid off

### What Was Inefficient
- ReconcileItems echo loop was a design gap not caught during planning — discovered during integration testing
- Cursor-hit namespace skip was explicitly planned but incorrect for bidirectional sync — the plan had a fundamental correctness bug
- Negentropy was researched and planned before being dropped for custom solution — research time partially wasted

### Patterns Established
- Two-dispatch crypto offload: cheap hash first → dedup gate → expensive verify only for new blobs
- ReconcileItems as protocol termination signal (breaks echo loop)
- has_fingerprint check for convergence detection
- Universal byte accounting placement (top of message handler, not per-type)
- co_spawn wrapping for non-coroutine callbacks that need async operations
- Closed mode in tests to eliminate PEX timeout interference with sync timing

### Key Lessons
1. Custom algorithms can be simpler than integrating external libraries when the domain is well-understood (reconciliation: ~550 LOC custom vs ~1000 LOC dependency + patching)
2. Planning can have correctness bugs — the cursor skip plan was wrong about bidirectional semantics. E2E tests caught it, not the plan review
3. Wire protocol removal (HashList) requires updating all callers immediately — can't leave compilation gaps
4. One-day milestones are achievable when building on a stable foundation with comprehensive test coverage
5. Thread pool offload for stateless crypto is straightforward; the hard part is ensuring stateful AEAD is never accessed from workers

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~1 across 1 day
- Notable: Fastest milestone overall — 4 phases in 1 day. Average ~24 min/plan including research and planning.

---

## Milestone: v0.5.0 — Hardening & Flexibility

**Shipped:** 2026-03-15
**Phases:** 5 | **Plans:** 6 | **Sessions:** ~2

### What Was Built
- Build restructure: db/ as self-contained CMake component with guarded FetchContent pattern
- TTL flexibility: BLOB_TTL_SECONDS removed, writer-controlled TTL, tombstone_map cleanup in expiry scan
- Encryption at rest: ChaCha20-Poly1305 for all stored blob payloads, HKDF-derived keys, auto-generated master key
- Transport optimization: trusted_peers config, lightweight handshake skipping ML-KEM-1024 for localhost/trusted peers
- Mismatch fallback: PQRequired message forces PQ upgrade when trust doesn't match (no connection failure)
- Documentation: README updated for DARE, trusted peers, configurable TTL

### What Worked
- Cleanest milestone yet: 5 phases, 6 plans, 2 days, zero issues
- Each phase was tightly scoped with clear success criteria — no scope creep
- HKDF key derivation pattern reused from transport layer (DARE) — familiar pattern, fast implementation
- TrustCheck lambda chain (PeerManager→Server→Connection) kept layers decoupled
- Mismatch fallback design (TrustedHello→PQRequired) is graceful — no connection failures on trust mismatch

### What Was Inefficient
- No milestone audit performed — all requirements were verified during phase execution, but the formal audit step was skipped
- Phase 26 (documentation) was minimal — only README + version bump. Could have been folded into Phase 25

### Patterns Established
- Guarded FetchContent: `if(NOT TARGET dep) FetchContent_Declare/MakeAvailable endif()` for composable CMake
- Envelope format: `[version][nonce][ciphertext+tag]` for self-describing encrypted storage
- AEAD associated data binding: AD = mdbx key to bind ciphertext to storage location
- Initiator-first trust negotiation: initiator proposes mode, responder accepts or forces upgrade
- Read-before-delete in expiry scan for index cleanup (tombstone_map)

### Key Lessons
1. Self-contained CMake components (db/) enable future reuse without pulling entire project — worth doing early
2. Single HKDF-derived key per node (not per-blob) is simpler and sufficient when AEAD AD binds ciphertext to location
3. Trust negotiation with fallback is better than hard failure — mismatch just upgrades to full PQ
4. Writer-controlled TTL is the right design for a dumb database — policy belongs to the application layer
5. Small milestones (5 phases) ship faster and have fewer integration issues than large ones

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~2 across 2 days
- Notable: Fastest milestone per-phase (~20 min avg) — all phases were focused, no multi-plan sprawl except Phase 25

---

## Milestone: v3.0 — Real-time & Delegation

**Shipped:** 2026-03-08
**Phases:** 4 | **Plans:** 8 | **Sessions:** ~3

### What Was Built
- Blob deletion: owner-signed tombstones (4-byte magic + 32-byte target hash), permanent (TTL=0), sync-propagated
- liboqs algorithm stripping: only ML-DSA-87 + ML-KEM-1024 enabled (BUILD-01)
- Namespace delegation: signed delegation blobs, O(1) indexed verification, write-only delegate access
- Delegation revocation via tombstoning delegation blob (reuses Phase 12 infrastructure)
- Pub/sub notifications: SUBSCRIBE/UNSUBSCRIBE/NOTIFICATION wire messages, connection-scoped subscriptions
- Three notification trigger paths: Data handler, Delete handler, SyncProtocol callback
- README.md with build/config/CLI/scenarios/crypto documentation
- Standalone benchmark binary covering crypto, data path, sync, and network operations

### What Worked
- Phase dependencies perfectly ordered: deletion -> delegation (needs tombstone for revocation) -> pub/sub (needs deletion for tombstone notifications)
- Reusing existing patterns: magic-prefix blob types (tombstone, delegation) fit cleanly into existing ingest pipeline
- Zero engine changes for delegation blob creation — existing pipeline handled it naturally
- Delegation revocation as "just tombstone the delegation blob" was elegant and required zero new mechanisms
- Fastest milestone: 8 plans in 2 days, average ~15 min/plan (vs ~25 min for v1.0)
- liboqs stripping reduced build time significantly

### What Was Inefficient
- Missing VERIFICATION.md for phases 12 and 14 — caught by milestone audit, documentation-only gap
- Phase 15 plans had stale checkboxes in roadmap despite being completed
- Audit found `gaps_found` status that was technically correct but misleading (all code worked, just docs missing)

### Patterns Established
- Magic-prefix blob types: 4-byte magic identifies blob variant (tombstone=0xDEADBEEF, delegation=0xDE1E6A7E)
- Ownership-or-delegation check: two-path verification on ingest hot path
- Delegate restriction guards: explicit checks before signature verification for delegation/tombstone creation
- co_spawn for notification fan-out: one coroutine per subscriber for non-blocking dispatch
- OnBlobIngested callback: SyncProtocol notifies peer manager of sync-received blobs

### Key Lessons
1. liboqs algorithm stripping should have been done in v1.0 — build time was a bottleneck for 2 milestones
2. Reusing existing infrastructure (tombstones for revocation, ingest pipeline for delegation) is faster and less error-prone than new mechanisms
3. Magic-prefix blob types are a clean extensibility pattern — new blob types slot in without protocol changes
4. Three trigger paths for notifications (data/delete/sync) is the right architecture — each has different context
5. Missing verification docs are a process gap, not a quality gap — all code was correct and tested

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~3 across 2 days
- Notable: Phases 13-14 were fastest at ~25 min combined — building directly on Phase 12 patterns

---

## Milestone: v2.0 — Closed Node Model

**Shipped:** 2026-03-07
**Phases:** 3 | **Plans:** 8 | **Sessions:** ~4

### What Was Built
- Source restructure: db/ layout, chromatindb:: namespace (60 files migrated, 155 tests preserved)
- Access control: allowed_keys config with open/closed mode, connection-level ACL gating
- PEX disabled in closed mode at all 4 protocol integration points
- SIGHUP hot-reload with fail-safe validation and immediate peer revocation
- 100 MiB blob support: MAX_BLOB_DATA_SIZE + MAX_FRAME_SIZE as constexpr protocol invariants
- Memory-efficient sync: index-only hash reads from seq_map, one-blob-at-a-time transfer, adaptive timeout

### What Worked
- Phase 9 (restructure) completed in 14 min — git mv + sed, no code changes needed
- AccessControl design (std::set + implicit self-allow) was simple and correct from the start
- Step 0 pattern (cheapest validation first) made oversized blob rejection trivial
- Milestone audit before completion caught zero gaps — phases were well-scoped from requirements
- Building on v1.0 patterns (timer-cancel, sequential sync, snapshot iteration) meant zero new async bugs

### What Was Inefficient
- Phase 11 plans listed "TBD" in roadmap — should have been filled during planning
- Some ROADMAP.md formatting inconsistencies (Phase 11 progress row had wrong column alignment)
- liboqs build time is excessive — builds all algorithms when only ML-DSA-87, ML-KEM-1024, and SHA3-256 are needed

### Patterns Established
- ACL gating at on_peer_connected: after handshake identity is known, before any state is created
- Silent TCP close for security rejections (no protocol-level denial = no information leakage)
- SIGHUP via dedicated coroutine member function (not lambda) to avoid stack-use-after-return
- Step 0 pattern: cheapest validation (integer comparison) before expensive operations (crypto)
- Index-only reads: use seq_map as secondary index to avoid loading blob data for hash collection
- One-blob-at-a-time: bounded memory by sending single blobs instead of bulk transfers

### Key Lessons
1. Source restructure phases are fast and low-risk — git mv preserves history, namespace rename is mechanical
2. Implicit mode (empty allowed_keys = open, non-empty = closed) eliminates config ambiguity — one field, one behavior
3. SIGHUP coroutine lifetime requires dedicated member function, not lambda — compiler-generated coroutine frames can outlive lambda captures
4. Expiry filtering at the receiver is simpler than at the sender — the receiver already has the logic in ingest_blobs
5. liboqs full build is wasteful — next milestone should strip unnecessary algorithm builds

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~4 across 2 days
- Notable: Phase 9 (restructure) was fastest at 14 min — mechanical refactoring with no design decisions

---

## Milestone: v1.0 — MVP

**Shipped:** 2026-03-05
**Phases:** 8 | **Plans:** 21 | **Sessions:** ~8

### What Was Built
- Complete PQ crypto stack (ML-DSA-87, ML-KEM-1024, SHA3-256, ChaCha20-Poly1305, HKDF-SHA256)
- ACID-safe blob storage with libmdbx (4 sub-databases, content-addressed dedup, TTL expiry)
- Blob ingest pipeline with fail-fast namespace/signature verification and write ACKs
- PQ-encrypted TCP transport with ML-KEM key exchange and ML-DSA mutual auth
- Peer system with bootstrap discovery, PEX, and bidirectional hash-list diff sync
- Running daemon with CLI (run/keygen/version), signal handling, graceful shutdown

### What Worked
- Bottom-up dependency ordering: each phase had a stable foundation to build on
- Parallel plan execution within phases cut wall-clock time significantly
- RAII wrappers for crypto eliminated entire classes of memory bugs
- Synchronous SyncProtocol separated from async PeerManager — testable without sockets
- Gap closure phases (6-8) after milestone audit caught real integration holes
- 3-day timeline for 9,449 LOC with full test coverage — high velocity maintained

### What Was Inefficient
- Phase 7 (Peer Discovery) took ~35 min for 2 plans due to coroutine lifetime bugs (co_spawn, pointer invalidation)
- Initial Phase 5 sync was send-only; receive side needed a whole extra phase (Phase 6) — should have been scoped correctly initially
- Peer discovery was listed in v1 requirements (DISC-02) but not initially in the roadmap — audit caught it
- Some SUMMARY files lack requirements-completed frontmatter (minor debt)

### Patterns Established
- Timer-cancel pattern for async message queues (steady_timer + cancel to wake coroutine)
- Sequential sync protocol (Phase A/B/C) to avoid TCP deadlock
- Inline PEX after sync to prevent AEAD nonce desync
- Deque for coroutine-accessed containers (not vector — pointer invalidation on push_back)
- Snapshot iteration: copy connection pointers before iterating across co_await points
- Big-endian uint64 keys for lexicographic == numeric ordering in libmdbx
- Pimpl pattern to isolate libmdbx from headers

### Key Lessons
1. Milestone audits before shipping catch real gaps — the first audit found missing sync receive side and peer discovery
2. Coroutines + async require extreme care with container lifetimes — every co_await is a potential invalidation point
3. Sequential protocols (send all, then receive all) are safer than interleaved bidirectional over a single connection
4. RAII + move semantics eliminate most crypto memory issues — invest in wrappers early
5. FlatBuffers ForceDefaults(true) is non-negotiable for deterministic encoding — discovered on first test failure

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~8 across 3 days
- Notable: Phase 8 (verification/cleanup) completed in ~5 min — minimal new code, just documentation and dead code removal

---

## Milestone: v1.1.0 — Operational Polish & Local Access

**Shipped:** 2026-03-22
**Phases:** 4 | **Plans:** 6 | **Sessions:** ~1

### What Was Built
- Repository cleanup: stale artifacts removed, 23 phase directories archived, CMake version bump to 1.1.0, README/db/README.md updated
- Configurable expiry scan interval (expiry_scan_interval_seconds, min 10s, SIGHUP reloadable)
- Shared sync_reject.h with 8 constexpr reason codes (expanded from 3 hardcoded constants)
- Step 0c timestamp validation on ingest: rejects blobs >1hr future or >30d past before any crypto
- Automatic runtime mdbx compaction: Storage::compact() with live env.copy(compactify=true), close/swap/reopen, 6h default timer, SIGHUP reload, SIGUSR1 metrics
- Unix Domain Socket local access: Connection refactored to generic stream socket, UdsAcceptor class, TrustedHello path, full ACL/rate/quota enforcement, 0660 permissions, stale socket cleanup

### What Worked
- Entire v1.1.0 completed in <1 day — 4 phases, 6 plans, 13 tasks, 4865 insertions
- Full auto-advance pipeline (discuss → plan → execute → verify per phase) ran smoothly for all 4 phases
- Prior patterns (timer-cancel, SIGHUP reload, validate_config, Step 0) made each new feature incremental
- Plan checker caught a real issue (missing create_uds_outbound in file list) before execution — saved a broken build
- TS_AUTO sentinel pattern emerged from Phase 54 and immediately proved useful (57 tests needed valid timestamps after validation was added)
- Compaction open_env() refactor was clean — constructor and compact() share identical open logic

### What Was Inefficient
- Phase 53 (cleanup) had to stage uncommitted changes in phases 47/49 before archiving — leftover from v1.0.0
- Timestamp validation broke 57 existing tests with hardcoded past timestamps — auto-fixed but could have been anticipated in planning
- No backward compatibility consideration needed (no deployed nodes) — simplified all wire format decisions

### Patterns Established
- TS_AUTO sentinel: test helpers use current time when timestamp=0, avoiding hardcoded past timestamps that fail validation
- Shared constexpr header for protocol constants (sync_reject.h) vs anonymous namespace constants
- open_env() factored helper for Storage::Impl — reusable open logic for constructor and compact()
- Generic stream socket in Connection: asio::generic::stream_protocol::socket type-erases TCP and UDS
- Separate UdsAcceptor class (not extending Server) — accept-only, no reconnect logic

### Key Lessons
1. Auto-advance pipeline works well for straightforward milestones — all 4 phases ran end-to-end without manual intervention
2. Plan checker adds real value — the create_uds_outbound file list gap would have caused a compilation failure during execution
3. Validation changes ripple through tests — adding Step 0 validation means updating all test helpers that create blobs with arbitrary timestamps
4. SIGHUP reload pattern is now well-established — 3 config fields reloadable (ACL, expiry interval, compaction interval)
5. UDS as "just another transport" was the right design — reusing TrustedHello and on_peer_connected kept the implementation minimal

### Cost Observations
- Model mix: 100% quality profile (opus for agents)
- Sessions: ~1
- Notable: Fastest milestone by wall-clock time — all 4 phases in a single auto-advance chain

---

## Cross-Milestone Trends

### Process Evolution

| Milestone | Sessions | Phases | Key Change |
|-----------|----------|--------|------------|
| v1.0 | ~8 | 8 | Bottom-up dependency ordering + gap closure via audit |
| v2.0 | ~4 | 3 | Clean milestone audit (zero gaps) — scoping improved from v1.0 lessons |
| v3.0 | ~3 | 4 | Fastest per-plan avg (~15 min) — pattern reuse accelerated delivery |
| v0.4.0 | ~5 | 6 | Production hardening — storage limits, metrics, rate limiting |
| v0.5.0 | ~2 | 5 | Cleanest milestone — DARE, trusted peers, configurable TTL in 2 days |
| v0.6.0 | ~2 | 5 | Docker benchmark infrastructure — end-to-end validation pipeline |
| v0.7.0 | ~2 | 6 | Sync cursors + crypto hot-path — largest protocol evolution |
| v0.8.0 | ~1 | 4 | Fastest milestone (1 day) — custom reconciliation + thread pool offload |
| v0.9.0 | ~1 | 4 | Second 1-day milestone — connection resilience, storage hardening, operational tooling |
| v1.0.0 | ~8 | 7 | Database layer done — sanitizers, 54 Docker integration tests, stress/chaos/fuzz |
| v1.1.0 | ~1 | 4 | Fastest milestone — full auto-advance chain, operational polish + UDS |
| v1.2.0 | ~1 | 4 | Relay architecture — PQ-authenticated gateway, default-deny filter, Zero Duplication Policy |
| v1.3.0 | ~2 | 4 | Concurrent dispatch foundation — request_id, ExistsRequest, NodeInfoRequest, IO-thread transfer |
| v1.4.0 | ~2 | 3 | 18 new query types, relay filter expansion, PROTOCOL.md update |
| v1.5.0 | ~1 | 2 | Docs-only — dist/ kit, full doc refresh, PROTOCOL.md source verification |

### Cumulative Quality

| Milestone | Tests | LOC | Source Files | Requirements |
|-----------|-------|-----|-------------|--------------|
| v1.0 | 155 | 9,449 | 53 | 32/32 |
| v2.0 | 196 | 11,027 | ~60 | 14/14 |
| v3.0 | 255 | 14,152 | ~70 | 16/16 |
| v0.4.0 | 284 | 14,523 | ~70 | 22/22 |
| v0.5.0 | 284 | 17,124 | 66 | 13/13 |
| v0.6.0 | 284 | 17,775 | ~70 | 14/14 |
| v0.7.0 | 313 | 18,000+ | ~75 | 20/20 |
| v0.8.0 | 408 | 22,003 | ~80 | 12/12 |
| v0.9.0 | 408+ | 22,467 | ~85 | 16/16 |
| v1.0.0 | 469 | 22,608 | ~90 | 16/16 |
| v1.1.0 | 500+ | 23,852 | ~95 | 12/12 |
| v1.2.0 | 530+ | 24,000+ | ~100 | 16/16 |
| v1.3.0 | 551+ | 27,469 | ~105 | 12/12 |
| v1.4.0 | 560+ | ~28,000 | ~110 | 14/14 |
| v1.5.0 | 567 | ~29,600 | ~110 | 12/12 |

### Top Lessons (Verified Across Milestones)

1. No DHT — bootstrap + PEX is simpler and works (validated across chromatin-protocol, DNA messenger, and now chromatindb)
2. Milestone audit before shipping catches integration gaps that phase-level verification misses
3. Source restructure is low-risk when done as a dedicated phase — mechanical work, no design decisions
4. Building on established async patterns (timer-cancel, sequential protocol) prevents new concurrency bugs
5. Reusing existing infrastructure (magic-prefix types, ingest pipeline) is faster and less error-prone than new mechanisms
6. Algorithm stripping for liboqs resolved in v3.0 — should have been done earlier (2 milestones of slow builds)
7. Writer-controlled policy (TTL, delegation) keeps the database layer dumb — application logic belongs in higher layers
8. Trust negotiation with graceful fallback is better than hard failure — validated in v0.5.0 transport optimization
9. Custom algorithms beat external dependencies when the domain is well-understood — validated in v0.8.0 reconciliation (~550 LOC vs 1000+ LOC dep)
10. Plans can have correctness bugs — E2E tests catch what plan reviews miss (v0.8.0 cursor skip bug)
11. Foundation phases (config, timers, logging) compound — v0.9.0 Phase 42 patterns used immediately by Phases 43+44
12. Planning/implementation divergence happens silently — milestone audit is the last defense (v0.9.0 README vs cursor compaction)
13. Relay as security boundary cleanly separates untrusted clients from trusted node — validated in v1.2.0
14. Bookkeeping gaps (unchecked requirements, missing summaries) accumulate at high velocity — v1.3.0 had 3 unchecked requirements despite code being shipped
