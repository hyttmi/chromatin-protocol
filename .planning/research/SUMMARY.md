# Project Research Summary

**Project:** CPUNK-DB (libcpunkdb)
**Domain:** Decentralized replicated key-value database with post-quantum cryptography
**Researched:** 2026-03-03
**Confidence:** HIGH (crypto stack proven in production; storage, serialization, and sync patterns well-established)

## Executive Summary

CPUNK-DB is a signed, append-only operation log with materialized state and TTL-based expiry, designed as an embeddable C++ library. The system model draws from three proven patterns: event sourcing (operation log is source of truth, state is derived), the Nostr relay model (pubkey-owned data, dumb relays, client-side verification), and Negentropy (range-based set reconciliation for efficient sync). The distinguishing design choices are ephemeral-by-default data (TTL on all non-profile operations) and post-quantum cryptography throughout (ML-DSA-87 for signing, ML-KEM-1024 for encryption). No blockchain, no DHT, no global consensus — namespaces converge independently through deterministic last-write-wins conflict resolution.

The recommended implementation path reuses the crypto stack proven in chromatin/PQCC (liboqs 0.15.0, OpenSSL 3.x, AES-256-GCM) and pairs it with libmdbx for storage (zero-copy mmap reads, automatic page reclamation critical for TTL-pruned logs) and FlatBuffers for wire format (zero-copy, deterministic encoding required for content-addressable signing). The architecture is transport-agnostic by design — the library produces and consumes sync messages; the caller provides the pipe. This keeps the library embeddable and avoids forcing WebSocket or TCP on users.

The primary technical risks are ML-DSA-87 signature verification throughput on mobile (1-3ms per op, 10k ops = 10-30 seconds), HLC clock skew causing silent LWW data loss, and reconciliation ordering bugs causing silent sync failures. All three are well-understood and mitigated through: verification caching, bounded skew rejection on HLC merge, and strict canonical sort ordering for set reconciliation. TTL-first design structurally prevents the append-only log growth problem that kills every comparable system. The MVP should defer Negentropy (start with hash-list diff), encrypted envelopes, anti-spam PoW, and log compaction/snapshots.

## Key Findings

### Recommended Stack

The crypto stack carries over directly from chromatin/PQCC: liboqs 0.15.0 (with the known FetchContent include path fix for generated headers), OpenSSL 3.x for AES-256-GCM, and all algorithms at NIST Category 5 (ML-DSA-87, ML-KEM-1024, SHA3-256). For storage, libmdbx 0.13.11 is recommended over LMDB or RocksDB — its zero-copy mmap reads pair naturally with FlatBuffers zero-copy access, its native CMake support makes FetchContent straightforward, and its automatic page reclamation is essential for append-only logs with TTL-based pruning. FlatBuffers is required (not optional) because deterministic byte representation is needed for content-addressed signing — Protocol Buffers are explicitly ruled out for this reason.

For the sync fingerprint computation, xxHash (XXH3) provides the needed speed for bucket hashing. HLC is approximately 100 lines of C++ from Kulkarni et al. (2014) — too small to be a dependency. Development tooling (Catch2, spdlog, nlohmann/json) carries over from chromatin/PQCC.

**Core technologies:**
- **C++20 / CMake 3.24+**: Language and build system — already proven, FetchContent manages all deps
- **liboqs 0.15.0**: ML-DSA-87, ML-KEM-1024, SHA3-256 — only production-grade PQ library, already used in PQCC
- **OpenSSL 3.x**: AES-256-GCM only — hardware-accelerated, liboqs uses it as backend
- **libmdbx 0.13.11**: Embedded KV storage — zero-copy mmap, ACID, automatic page reclamation for pruned logs
- **FlatBuffers 25.12.19**: Binary serialization — deterministic encoding required for signing; zero-copy pairs with libmdbx
- **xxHash 0.8.3 (XXH3)**: Sync fingerprints — fast non-crypto hashing for range buckets
- **Custom HLC (~100 LOC)**: Hybrid logical clock — not a dependency, implement directly from Kulkarni et al.

### Expected Features

CPUNK-DB v0.1 must ship 12 table-stakes features. The dependency chain is strict: crypto and HLC are the foundation, everything else builds upward through operation log, storage, and namespace/authz before reaching the sync engine and public API.

**Must have (table stakes for v0.1):**
- Cryptographic identity (ML-DSA-87 pubkey owns namespace) — trust foundation
- Signed append-only operation log (SET, DELETE, GRANT, REVOKE) — source of truth
- Local-first storage (full replica of subscribed namespaces via libmdbx) — offline-first requirement
- Deterministic LWW conflict resolution (HLC + hash tiebreak) — convergence guarantee
- Causal ordering via HLC — bounded skew detection built in
- Namespace read/write API (get/set/delete) — basic usability
- Multi-relay connectivity — no single point of censorship
- Operation deduplication (SHA3-256 content-addressed IDs) — free dedup via hash identity
- Signature verification on ingest (reject invalid ops at door) — relay is untrusted
- Capability grants (prefix-scoped, TTL-bounded delegation) — multi-author namespaces
- TTL on all operations (profile/ keys only may use TTL=0) — ephemeral-by-default
- Profile namespace (permanent identity anchor: pubkeys, bio, relay hints) — key discovery

**Should have (differentiators, defer to v0.2):**
- Negentropy range-based set reconciliation (O(diff) sync — start with hash-list diff in v0.1)
- Encrypted envelopes (ML-KEM key wrapping + AES-256-GCM — relay stays blind to values)
- Anti-spam PoW for open-write namespaces (capability-only in v0.1)
- Log compaction via snapshots (TTL expiry keeps storage bounded; snapshots compress further)
- Equivocation detection (LWW handles the data; flagging is application-layer)

**Defer (v2+):**
- Multi-device conflict UI / key-per-device patterns
- Relay discovery and reputation tracking
- Batch operations (multiple SET/DELETE per signed op)
- Federation / cross-deployment sync

### Architecture Approach

CPUNK-DB has eight components arranged in a strict dependency hierarchy. The public API sits on top and orchestrates everything below it. Transport is explicitly not owned by the library — sync messages are produced/consumed as byte blobs; callers connect to relays. This design enables embedding in any context (CLI, daemon, mobile app) without forcing a transport choice. The operation log is the canonical source of truth; the materialized state table (libmdbx sub-database) is a derived view used for fast reads and LWW resolution. Storage uses seven libmdbx sub-databases: operations (sorted namespace→HLC→hash), state (materialized KV), expiry (secondary TTL index), grants, profiles, meta (per-namespace HLC ceiling), and sync_state (per-relay sync progress).

**Major components:**
1. **Crypto Layer** — ML-DSA-87 sign/verify, ML-KEM-1024 encaps/decaps, AES-256-GCM, SHA3-256
2. **HLC** — timestamp generation, merge on receive, bounded skew detection (MAX_SKEW ~5 min)
3. **Operation Log** — create, validate, persist signed operations; content-addressed by SHA3-256 hash; TTL field on every op
4. **Local Storage (libmdbx)** — persist ops and materialized state; expiry index for TTL scanning
5. **Namespace/AuthZ** — verify write permission, check grants, enforce profile namespace rules (TTL=0 only for profile/ keys)
6. **State Engine** — materialize current KV from log; LWW conflict resolution; TTL checks at read time
7. **Sync Engine** — set reconciliation; produce/consume HAVE/WANT/RANGE/OPS messages; skip expired ops
8. **Public API** — orchestrate all components; expose clean C++ interface

### Critical Pitfalls

1. **HLC clock skew causes silent LWW data loss** — On HLC merge, reject remote timestamps more than MAX_SKEW (5 min) ahead of local wall clock. Accept the operation but do not advance local HLC. Track per-namespace HLC ceiling. Must be in Phase 1 — retrofitting skew detection is hard and data loss is permanent.

2. **ML-DSA-87 verification throughput on mobile** — 1-3ms per op on ARM; 10k ops = 10-30 seconds blocking CPU. Mitigate with: verification result cache in libmdbx (verify-once), async background verification pipeline, TTL reducing active op count. Batch verification (check liboqs support) and signed snapshots (future) reduce O(n) to O(1).

3. **Append-only log growth without pruning** — ML-DSA-87 sigs are 4627 bytes each; 10k ops = ~50MB signatures alone. Structurally solved by TTL-first design (storage bounded by write_rate x average_TTL). Reserve SNAPSHOT op_type in format from day one even if deferred.

4. **Reconciliation ordering bugs causing silent sync failure** — Both peers must sort operations identically. Strict canonical sort key: (HLC: uint64, SHA3_hash: bytes[32]). Ambiguity = fingerprint mismatch = ops silently never sync. Use property-based testing: random op sets split across peers, verify convergence.

5. **Capability revocation timing gap** — Revoked grantee can still write to relays that haven't seen the REVOKE. Mitigation: mandatory TTL on all grants (access is time-bounded even without explicit revoke); validate grantee permission against grant state at operation's HLC timestamp (retroactive revocation); short-lived grants with auto-renewal.

## Implications for Roadmap

Research reveals a strict dependency chain that should map directly to phases. Each phase has a clear prerequisite from the phase before it — there is no productive reordering.

### Phase 1: Foundation — Crypto and Clock

**Rationale:** Everything in the system depends on the crypto layer and HLC. These have zero dependencies on each other and zero dependencies on storage or serialization. HLC skew detection must be implemented here — retrofitting later risks permanent data loss bugs.
**Delivers:** Reusable liboqs wrapper (sign, verify, hash, encaps/decaps, AES-GCM) + HLC with bounded skew rejection
**Addresses:** Table stakes #1 (cryptographic identity), #5 (causal ordering)
**Avoids:** Pitfall #1 (HLC clock skew data loss — must be first)
**Notes:** Crypto patterns proven in chromatin/PQCC. Reuse liberally. HLC is ~100 LOC. Phase is fast.

### Phase 2: Core Data — Operation Format, Storage, AuthZ

**Rationale:** Operation format is the schema everything signs and syncs. Once operations are distributed, the format is frozen — version byte and canonical serialization spec must be locked here. libmdbx storage and namespace/AuthZ build on the format.
**Delivers:** FlatBuffers operation schema (with version byte, TTL field, SNAPSHOT type reserved), libmdbx wrapper (7 sub-databases), TTL expiry index and scan, namespace ownership, capability grants with TTL, profile namespace rules
**Addresses:** Table stakes #2 (signed log), #3 (local storage), #8 (deduplication), #9 (verify on ingest), #10 (capability grants), #11 (TTL), #12 (profile namespace)
**Avoids:** Pitfall #2 (verification cache goes here), Pitfall #3 (TTL-first design), Pitfall #5 (grant TTL mandatory from day one), Pitfall #13 (serialization lock-in — version byte now)
**Uses:** FlatBuffers 25.12.19, libmdbx 0.13.11

### Phase 3: State Engine — LWW and Conflict Resolution

**Rationale:** State materialization and conflict resolution require a complete operation log and storage layer. This phase produces fast reads (query materialized state, not log) and deterministic convergence.
**Delivers:** LWW conflict resolution with HLC + hash tiebreak, materialized state table, TTL checks at read time, equivocation detection (two ops same author/seq/namespace, different hash)
**Addresses:** Table stakes #4 (deterministic conflict resolution), #6 (namespace read/write API)
**Avoids:** Pitfall #4 (equivocation — LWW + content-addressing handles naturally), Pitfall #10 (split-brain — document LWW tradeoff, expose losing ops in API)

### Phase 4: Sync Engine — Set Reconciliation

**Rationale:** Sync requires a complete, working local store and state engine. Start with hash-list diff (simpler, correct) and plan Negentropy upgrade in v0.2. Property-based testing critical here — sync bugs are silent data loss.
**Delivers:** Sync message protocol (HAVE/WANT/RANGE/OPS), hash-list diff sync for v0.1, per-relay sync state (resume on reconnect), sync of expired-op exclusion, multi-relay connectivity
**Addresses:** Table stakes #7 (multi-relay), differentiator #3 (Negentropy — v0.2 upgrade path)
**Avoids:** Pitfall #5 (strict canonical sort order from day one), Pitfall #8 (client-server model, sync timeout/fallback), Pitfall #12 (property-based tests, multi-client from day one)

### Phase 5: Public API and Integration Testing

**Rationale:** Public API is the integration point for all components. Integration tests here exercise the full stack end-to-end with real libmdbx instances, real signatures, and simulated multi-relay scenarios.
**Delivers:** Clean C++ library interface (get/set/delete/grant/subscribe/sync_msg), library packaging (libcpunkdb), integration test suite with simulated relays and partitions
**Addresses:** Full feature set from table stakes; validates all pitfall mitigations under realistic conditions

### Phase 6: v0.2 Hardening (Post-MVP)

**Rationale:** Defer complexity that doesn't block correctness. These features improve performance and capability without being required for a functional library.
**Delivers:** Negentropy range-based reconciliation (O(diff) sync), encrypted envelopes (ML-KEM + AES-GCM), anti-spam PoW for open-write namespaces, log compaction via signed snapshots
**Notes:** Negentropy C++ availability must be verified before planning this phase — may require porting from reference implementation.

### Phase Ordering Rationale

- Crypto and HLC before everything (Phase 1) because every subsequent component depends on both, and HLC skew detection cannot be retrofitted.
- Operation format before storage (Phase 2) because libmdbx schema is derived from the format; changing format after signing begins is a breaking change.
- State engine before sync (Phase 3 before Phase 4) because the sync engine's inbound path writes to materialized state — it needs the LWW resolver.
- Hash-list sync before Negentropy (Phase 4 then v0.2) because Negentropy is an optimization on correct sync, not a correctness requirement. Shipping simple-and-correct faster is better than shipping Negentropy late.
- Public API last (Phase 5) because it is the integration point; premature API design locks in internal choices that may need to change.

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 4 (Sync Engine):** Negentropy C++ implementation availability is unverified — may need porting from JavaScript reference. Determine before planning Phase 4 whether to implement Negentropy in v0.1 or confirmed v0.2. Property-based test tooling choices (rapidcheck vs custom) need evaluation.
- **Phase 6 (Encrypted Envelopes):** ML-KEM key wrapping protocol (ephemeral vs static sender key, nonce handling) needs detailed spec. No reference implementation to steal from.

Phases with standard patterns (skip additional research):
- **Phase 1 (Crypto + HLC):** Both are thoroughly documented. liboqs patterns proven in chromatin/PQCC. HLC from published paper. No unknowns.
- **Phase 2 (Operation Format + Storage):** FlatBuffers schema design and libmdbx usage are well-documented. TTL index pattern (sorted by expiry timestamp) is standard.
- **Phase 3 (State Engine):** LWW with HLC is standard distributed systems pattern. No research needed.
- **Phase 5 (Public API):** C++ library API design is standard. No novel decisions.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Crypto stack proven in chromatin/PQCC. libmdbx used by Erigon at scale. FlatBuffers from Google with production usage. All version pins verified. |
| Features | MEDIUM-HIGH | Derived from analysis of Nostr, Automerge, Hypercore, OrbitDB, GUN.js. Feature set is well-motivated. Anti-spam PoW scope and grant rate-limiting fields need detailed spec during Phase 2. |
| Architecture | MEDIUM-HIGH | Component boundaries clear. Storage schema detailed. Data flows specified. One open question: Negentropy C++ availability (unverified — may be JS-only reference). |
| Pitfalls | MEDIUM-HIGH | All 6 critical pitfalls have documented mitigations from distributed systems literature and real incidents. ML-DSA-87 batch verification support in liboqs needs confirmation before Phase 4 optimization work. |

**Overall confidence:** HIGH for v0.1 scope. MEDIUM for v0.2 features (Negentropy port, encrypted envelope protocol design).

### Gaps to Address

- **Negentropy C++ availability**: Before Phase 4 planning, verify whether a C++ implementation exists or if the Rust/JS reference must be ported. This affects whether Negentropy is in v0.1 or v0.2.
- **Default TTL values**: What are sensible defaults for messages, presence, inbox items? Needs product decision before Phase 2 (operation format) — influences storage sizing and UX guidance.
- **Profile namespace key structure**: What keys live under `profile/` besides pubkeys, bio, and relay hints? Define the full profile schema before Phase 2.
- **ML-DSA-87 batch verification in liboqs**: Verify support before Phase 4 optimization work. If not supported, bulk ingest performance path requires a different approach.
- **Anti-spam PoW fields**: If PoW stamps are reserved in the operation format (even for v0.2), the format needs a `pow_nonce` field. Decide in Phase 2 whether to reserve the field.
- **SNAPSHOT op_type value**: Reserve the op_type byte value in Phase 2 even if implementation is deferred. Lock in the constants table early.

## Sources

### Primary (HIGH confidence)
- liboqs 0.15.0 documentation and chromatin/PQCC project direct experience — crypto layer, FetchContent integration, include path fix
- libmdbx documentation (erthink/libmdbx) — storage schema, page reclamation, sub-database model
- Erigon project — libmdbx at scale in production
- FlatBuffers documentation (Google) — deterministic encoding, zero-copy access, schema evolution
- Kulkarni et al. "Logical Physical Clocks and Consistent Snapshots in Globally Distributed Databases" (2014) — HLC design

### Secondary (MEDIUM confidence)
- Negentropy protocol specification (Nostr NIPs) — set reconciliation design; C++ availability unverified
- Nostr NIP-77, event sourcing patterns — relay model, subscription design, anti-spam
- Automerge, Hypercore, OrbitDB documentation — feature comparison, transport-agnostic design patterns

### Tertiary (LOW confidence)
- ML-DSA-87 mobile performance estimates (1-3ms/op on ARM) — from community benchmarks, needs validation on target hardware
- FoundationDB simulation testing approach — referenced as inspiration, not directly applicable

---
*Research completed: 2026-03-03*
*Ready for roadmap: yes*
