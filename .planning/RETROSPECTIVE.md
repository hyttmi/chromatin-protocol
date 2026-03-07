# Project Retrospective

*A living document updated after each milestone. Lessons feed forward into future planning.*

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

## Cross-Milestone Trends

### Process Evolution

| Milestone | Sessions | Phases | Key Change |
|-----------|----------|--------|------------|
| v1.0 | ~8 | 8 | Bottom-up dependency ordering + gap closure via audit |
| v2.0 | ~4 | 3 | Clean milestone audit (zero gaps) — scoping improved from v1.0 lessons |

### Cumulative Quality

| Milestone | Tests | LOC | Source Files | Requirements |
|-----------|-------|-----|-------------|--------------|
| v1.0 | 155 | 9,449 | 53 | 32/32 |
| v2.0 | 196 | 11,027 | ~60 | 14/14 |

### Top Lessons (Verified Across Milestones)

1. No DHT — bootstrap + PEX is simpler and works (validated across chromatin-protocol, DNA messenger, and now chromatindb)
2. Milestone audit before shipping catches integration gaps that phase-level verification misses
3. Source restructure is low-risk when done as a dedicated phase — mechanical work, no design decisions
4. Building on established async patterns (timer-cancel, sequential protocol) prevents new concurrency bugs
5. liboqs build time is a real bottleneck — strip unnecessary algorithms in next milestone
