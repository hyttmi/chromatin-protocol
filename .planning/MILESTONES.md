# Milestones

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
