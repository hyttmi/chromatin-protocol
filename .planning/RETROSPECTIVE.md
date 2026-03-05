# Project Retrospective

*A living document updated after each milestone. Lessons feed forward into future planning.*

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

### Cumulative Quality

| Milestone | Tests | Assertions | Source Files |
|-----------|-------|------------|-------------|
| v1.0 | 155 | 586 | 53 |

### Top Lessons (Verified Across Milestones)

1. No DHT — bootstrap + PEX is simpler and works (validated across chromatin-protocol, DNA messenger, and now chromatindb)
2. Milestone audit before shipping catches integration gaps that phase-level verification misses
