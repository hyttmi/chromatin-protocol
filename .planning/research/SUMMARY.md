# Project Research Summary

**Project:** chromatindb
**Domain:** Decentralized PQ-secure database node — query API expansion (v1.4.0 Extended Query Suite)
**Researched:** 2026-03-26
**Confidence:** HIGH

## Executive Summary

The v1.4.0 milestone is a pure application-layer feature expansion onto a stable, production-hardened foundation. Thirteen shipped milestones and 64 phases of prior work mean the entire stack, storage schema, dispatch model, wire format conventions, and relay filter architecture are locked and proven. The research confirms that all 10 new query types (the milestone listed 11, but BlobMetadata and Metadata are duplicates and must be merged into one MetadataRequest/MetadataResponse pair) can be built using existing libmdbx cursor operations, existing in-memory state reads, and the coroutine-IO dispatch pattern established in Phase 62. No new dependencies, no new sub-databases for the core feature set, and no new async primitives are needed anywhere in this milestone.

The recommended approach is to implement in three phases ordered by complexity: simple node-level queries first (Health, NamespaceList, NamespaceStats, StorageStatus — zero storage API changes required), then blob-level queries second (MetadataRequest, BatchExists, DelegationList — minor new Storage methods, no complex wire formats), then batch and range queries last (BatchRead, PeerInfo, TimeRange — complex variable-length response encoding, privacy policy decisions, and the timestamp-scan tradeoff). This ordering is dependency-driven: simpler phases validate the four-component integration pattern before the harder features add storage complexity, and TimeRange is intentionally last because it requires the largest design decision: scan-and-filter via seq_map (O(N) decrypts, acceptable for v1.4.0 volumes) versus a new timestamp sub-database (O(log N) queries but adds write-path complexity — YAGNI).

The primary risks are operational, not conceptual. Every phase that adds message types must update four locations: transport.fbs enum, message_filter.cpp in the relay, NodeInfoResponse supported_types array, and PROTOCOL.md. Missing any one of these produces silent failures visible only through specific test paths (relay path vs UDS path). The AEAD nonce desync risk from incorrect handler dispatch is the most severe failure mode — it causes data corruption rather than clear errors — and has been the root cause of production bugs in prior milestones (Phase 50 PEX SIGSEGV). All 10 new handlers must be classified as coroutine-IO and must never call thread-pool offload paths.

## Key Findings

### Recommended Stack

No stack changes whatsoever. The existing stack fully covers all 10 new query types. The research validates this against every new type: libmdbx cursor `lower_bound()` scans power time-range and list queries; `has_blob()` loops in a single MDBX read transaction cover batch existence; `get_blob()` loops cover batch read; `mdbx::txn::get_map_stat()` returns aggregate tombstone and delegation counts in O(1); the in-memory `peers_` deque in PeerManager covers PeerInfo; a status byte is sufficient for Health. All wire encoding uses the existing big-endian integer helpers (`encode_be_u64`, `encode_be_u32`). The FlatBuffers enum extension from 40 types (v1.3.0) to 60 types (v1.4.0) is a standard backward-compatible operation already done multiple times in this codebase.

**Core technologies (unchanged, no additions):**
- libmdbx v0.13.11 — ACID storage with 7 sub-databases; cursor scans power all new list and range queries
- liboqs / libsodium — PQ crypto and DARE encryption; metadata queries must fully decrypt blobs (AEAD is all-or-nothing; there is no way to read metadata without decrypting the full blob)
- FlatBuffers v25.2.10 — enum extension from types 0-40 to 0-60; backwards-compatible; regenerate transport_generated.h
- Standalone Asio 1.38.0 — coroutine-IO dispatch; all new handlers use the same co_spawn on ioc_ pattern from Phase 62
- C++20 / CMake — language and build system; no FetchContent changes needed

See `.planning/research/STACK.md` for per-query-type analysis, integration point details, and the full "what NOT to add" rationale.

### Expected Features

The feature analysis identified 11 items in the milestone spec, but BlobMetadata and Metadata are identical operations (return blob fields without the data payload, identified by namespace+hash) and must be merged into a single MetadataRequest/MetadataResponse pair. The resulting 10 message type pairs occupy enum slots 41-60 in transport.fbs, with the duplicate slot reserved.

**Must have (table stakes):**
- Health — any deployable daemon needs liveness/readiness probes; orchestrators (Docker, K8s, systemd) require this
- MetadataRequest — completes the Read/Exists/Metadata trio that is universally expected in a blob store API
- NamespaceList — NodeInfoResponse reports namespace_count but clients have no way to enumerate the actual namespace IDs
- NamespaceStats — per-namespace quota usage, tombstone count, and delegation count; needed for monitoring dashboards
- StorageStatus — disk usage, quota headroom, and tombstone totals at the node level; needed for capacity planning

**Should have (differentiators):**
- BatchExists — reduces O(N) round-trips to O(1) for sync verification and cache freshness checks; high-value, low-complexity
- BatchRead — batch fetch for small-blob workloads where per-round-trip latency dominates over bandwidth
- DelegationList — exposes who has write access to a namespace; essential for delegation management UIs and security audits

**Defer to v1.5.0 (acceptable scope reduction if needed):**
- PeerInfo — useful for operational dashboards but requires trust-gating policy decisions about IP address exposure
- TimeRange — standard query type but requires either O(N) decrypt scans (v1.4.0 approach) or a new timestamp sub-database; highest complexity in the set

See `.planning/research/FEATURES.md` for full wire format specifications, payload byte layouts, and per-feature implementation notes.

### Architecture Approach

Every new query type integrates via the same four-component pattern established across all prior milestones: add enum values to transport.fbs, add Storage methods where needed, add dispatch cases in `PeerManager::on_peer_message()`, and add relay filter entries in `message_filter.cpp`. No changes to Connection, Engine, Server, Handshake, Sync, Reconciliation, PEX, Config, Identity, or Crypto. The entire new work is confined to storage.h/.cpp, peer_manager.cpp, message_filter.cpp, transport.fbs, and PROTOCOL.md.

All 10 new handlers are classified as coroutine-IO dispatch (co_spawn on ioc_). None require the coroutine-offload-transfer pattern because none call the crypto thread pool — they are read-only queries using synchronous MDBX read transactions. The co_spawn is required solely because `send_message()` is an awaitable that must run on the IO thread for AEAD nonce serialization.

**Major components and their v1.4.0 scope:**
1. Storage (storage.h/.cpp) — add 5 new methods: `get_blob_metadata()`, `batch_has_blobs()`, `batch_get_blobs()`, `list_delegations()`, `tombstone_count()`; all use existing MDBX cursor patterns
2. PeerManager (peer_manager.cpp) — add 10 dispatch cases in `on_peer_message()`, all following the coroutine-IO template from Phase 62/63
3. Relay filter (relay/core/message_filter.cpp) — add 20 new enum values (10 request + 10 response) to `is_client_allowed()`
4. FlatBuffers schema (transport.fbs) — add enum values 41-60 to TransportMsgType; regenerate transport_generated.h
5. PROTOCOL.md — document byte-level wire format for all 10 new request/response pairs

See `.planning/research/ARCHITECTURE.md` for per-type dispatch classification table, new Storage method signatures, and complete wire format specifications.

### Critical Pitfalls

1. **Batch response frame overflow (BatchRead)** — aggregate response exceeds MAX_FRAME_SIZE (110 MiB) when no cumulative size cap is enforced. Prevent by: capping BatchRead at 16-32 blobs, enforcing a cumulative size limit with a `partial` flag in the response wire format (must be in the format from the start — adding it later is a breaking change), and writing a test with blobs whose aggregate size approaches the frame limit.

2. **AEAD nonce desync from incorrect dispatch classification** — `send_counter_` in Connection is a non-atomic uint64_t; concurrent sends from multiple coroutines on different threads cause nonce races and AEAD decryption failures at the remote end. This produced the Phase 50 PEX SIGSEGV. Prevent by: classifying all 10 new handlers as coroutine-IO, never adding thread pool offload to read-only query handlers, running TSAN tests with pipelined concurrent requests per connection.

3. **Relay filter missing new types** — missing a case in `message_filter.cpp` causes the relay to silently disconnect clients who send the new type; the node works fine over UDS, masking the bug in single-node tests. Prevent by: treating relay filter update as a per-phase mandatory checklist item, and explicitly testing each new type through the relay path (not just UDS).

4. **NamespaceList unbounded scan** — exposing the existing `list_namespaces()` without pagination is a DoS vector for nodes with many namespaces; adding pagination after deployment is a breaking wire format change. Prevent by: designing the pagination cursor (after_namespace:32 + limit:4) into the request format before any implementation begins.

5. **Health handler blocking the IO thread** — calling any storage cursor method from a health handler adds latency under write transaction contention; calling a write transaction causes health to hang during compaction. Liveness must be zero-IO. Readiness may use `used_bytes()` (O(1) mmap stat, no cursor) but must not open cursors, scan namespaces, or write.

See `.planning/research/PITFALLS.md` for additional integration gotchas, security mistakes to avoid, and a per-feature "looks done but isn't" checklist.

## Implications for Roadmap

Based on combined research, the feature ordering from FEATURES.md aligns exactly with the dependency and complexity analysis from ARCHITECTURE.md and PITFALLS.md. Three implementation phases are recommended within the v1.4.0 milestone.

### Phase 65: Simple Node-Level Queries (Health, NamespaceList, NamespaceStats, StorageStatus)

**Rationale:** These four queries require zero new Storage methods — they reuse existing `list_namespaces()`, `get_namespace_quota()`, `effective_quota()`, `used_bytes()`, and `used_data_bytes()`. They cover the highest-priority table stakes features (Health is a deployment blocker). Starting here validates the four-component integration pattern (schema + handler + relay filter + PROTOCOL.md) before adding storage complexity.

**Delivers:** Complete liveness/readiness probe support; full namespace enumeration and storage visibility for operators; foundation pattern that all subsequent phases copy.

**Addresses:** Health, NamespaceList, NamespaceStats, StorageStatus from the features list.

**Avoids:** Health blocking IO (Pitfall 6 in PITFALLS.md) — design as inline or minimal-IO from the start. NamespaceList unbounded scan (Pitfall 7) — pagination cursor in wire format before any code is written. Relay filter omission (Pitfall 4) — establish checklist discipline here.

**Research flag:** Standard patterns from Phase 62/63. Skip `/gsd:research-phase`.

---

### Phase 66: Blob-Level Queries (MetadataRequest, BatchExists, DelegationList)

**Rationale:** These require minor new Storage methods but no complex wire formats or policy decisions. MetadataRequest closes the obvious gap in the Read/Exists API. BatchExists is the simplest batch operation (fixed-size response: count + N result bytes). DelegationList is a cursor prefix scan over existing delegation_map following the exact `list_namespaces()` pattern. Grouping these together allows the new Storage methods to be written and tested together before the more complex batch operations.

**Delivers:** Complete single-blob query API (exists/metadata/read); O(1) round-trip batch existence check; delegation visibility for security audits.

**Addresses:** MetadataRequest (must-have), BatchExists (should-have), DelegationList (should-have).

**Avoids:** DelegationList snapshot inconsistency (Pitfall 5) — implement as a single-txn cursor scan, not N separate `has_valid_delegation()` calls. MetadataRequest seq_num gap — the plan must decide whether to include seq_num in the response (requires reverse lookup or seq_map scan) or omit it; this decision must be made before writing the wire format.

**Research flag:** Standard patterns. Skip `/gsd:research-phase`.

---

### Phase 67: Batch and Range Queries (BatchRead, PeerInfo, TimeRange)

**Rationale:** These are the most complex features in the milestone. BatchRead requires variable-length response encoding with cumulative size tracking and a truncation flag — the wire format must be locked before any implementation begins. PeerInfo requires a trust-gating policy decision (what to expose to untrusted vs trusted connections). TimeRange requires the explicit decision between seq_map scan-and-filter (O(N) decrypts, accepted for v1.4.0) and a new timestamp sub-database (O(log N), YAGNI). Placing these last means the simpler patterns are exercised and the dispatch model is well-understood.

**Delivers:** Batch blob fetch with partial-result support; detailed peer topology data; time-window blob queries.

**Addresses:** BatchRead (should-have), PeerInfo (defer candidate), TimeRange (defer candidate).

**Avoids:** Frame size overflow (Pitfall 1) — BatchRead cumulative size cap and partial flag must be in the wire format spec, not added during implementation. AEAD nonce desync (Pitfall 2) — TSAN pipelined-request tests required. TimeRange full-scan performance (Pitfall 3) — enforce request limit (e.g., 100 results) and document O(N) behavior in PROTOCOL.md.

**Research flag:** BatchRead and TimeRange benefit from explicit design review during plan writing. PeerInfo trust-gating policy must be documented before implementation. If schedule is tight, both PeerInfo and TimeRange can be explicitly deferred to v1.5.0 without breaking the rest of the milestone.

---

### Phase Ordering Rationale

- Phase 65 comes first because it covers deployment-blocking features (Health) and establishes the four-component integration checklist used by all subsequent phases.
- Phase 66 comes second because it adds Storage API methods that Phase 67 may need to reference, and the patterns are simpler.
- Phase 67 comes last because it contains the features with the most design decisions; deferring PeerInfo and TimeRange to v1.5.0 is explicitly acceptable if needed.
- TimeRange is the very last feature within Phase 67 because the timestamp-scan decision (seq_map proxy vs new sub-database) has the largest blast radius if changed after implementation starts.
- All phases share the same four-component integration pattern; per-phase relay filter and PROTOCOL.md updates must be treated as required, not optional.

### Research Flags

Phases needing design attention during plan writing:
- **Phase 67 (BatchRead):** Wire format must include `partial` flag and cumulative size tracking from day one. Lock the response format before writing any implementation code.
- **Phase 67 (TimeRange):** Decision on seq_map scan-and-filter vs timestamp index must be documented in the plan. Recommendation from all three research files: use scan-and-filter with a 100-result limit for v1.4.0; defer timestamp index to v2+.
- **Phase 67 (PeerInfo):** Trust-gating policy must be documented. Recommendation: allow through relay, return reduced data (count only, no IP addresses) for untrusted connections.
- **Phase 66 (MetadataRequest):** Decide whether seq_num is included in MetadataResponse; if yes, document the lookup strategy (scan seq_map — O(N) — or accept absence of seq_num).

Phases with well-documented standard patterns (no additional research needed):
- **Phase 65:** All four queries reuse existing storage methods and coroutine-IO dispatch from Phase 62/63.
- **Phase 66:** DelegationList and BatchExists cursor/loop patterns are direct copies of `list_namespaces()` and `has_blob()` patterns already in the codebase.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Verified through direct codebase analysis of 12+ libmdbx cursor uses, 6 existing handler implementations, FlatBuffers enum history (30 -> 40 -> 60 types), and DARE encryption model |
| Features | HIGH | Derived from protocol gap analysis cross-referenced with existing wire spec in PROTOCOL.md; BlobMetadata/Metadata merge decision is clear and unambiguous |
| Architecture | HIGH | All dispatch classifications and component boundaries derived from direct source reading of peer_manager.cpp, storage.cpp, and connection.cpp; no inferences from external docs |
| Pitfalls | HIGH | Derived from codebase analysis plus documented failure modes from prior milestones (Phase 50 PEX SIGSEGV, Phase 62 IO-thread transfer pattern); not speculative |

**Overall confidence:** HIGH

### Gaps to Address

- **TimeRange timestamp units:** Confirm whether blob.timestamp is stored in microseconds or seconds at the storage layer before finalizing the TimeRange wire format. PITFALLS.md flags this as a potential confusion point. Resolve by checking the FlatBuffer schema and store_blob() during Phase 67 plan writing.

- **MetadataRequest seq_num inclusion:** No reverse index from blob_hash to seq_num currently exists. The Phase 66 plan must decide: scan seq_map to find seq_num (O(N)), add a new reverse index, or omit seq_num from MetadataResponse. FEATURES.md notes this explicitly as the only non-trivial part of MetadataRequest.

- **BlobMetadata vs Metadata final confirmation:** Both research files recommend merging into a single type pair. The roadmap plan must confirm this merge and reserve the duplicate type IDs as `Reserved` in transport.fbs to maintain contiguous enum numbering.

- **PeerInfo trust-gating policy:** A policy decision, not a code ambiguity. Must be documented before Phase 67 implementation begins. The recommendation (expose count for untrusted, full data for trusted/UDS) is documented in FEATURES.md but must be approved as the final design.

## Sources

### Primary (HIGH confidence)

- `db/storage/storage.cpp` — cursor patterns, DARE encryption model, delegation_map/tombstone_map key structure, `list_namespaces()` and `integrity_scan()` patterns
- `db/peer/peer_manager.cpp` lines 523-538 — dispatch model classification comment block, ExistsRequest/NodeInfoRequest handler patterns from Phase 62/63
- `db/net/connection.cpp` line 155 — `send_counter_++` in `send_encrypted()`, non-atomic nonce serialization
- `db/net/framing.h` — MAX_FRAME_SIZE = 110 MiB
- `relay/core/message_filter.cpp` — explicit allow-list switch structure confirming per-type registration requirement
- `db/PROTOCOL.md` — existing wire format specifications (types 1-40) confirming big-endian, no-padding conventions
- `.planning/RETROSPECTIVE.md` — PEX SIGSEGV root cause (v1.0.0), IO-thread transfer pattern (v1.3.0), dispatch model classification

### Secondary (MEDIUM confidence)

- Kubernetes liveness/readiness probe semantics — health check design (two-probe model; liveness = no storage, readiness = lightweight probe)
- API Design Patterns Ch. 18 (Manning) — batch operation response patterns and partial-result conventions
- libmdbx v0.13.11 documentation — MVCC snapshot isolation guarantees confirming safe concurrent cursor reads without explicit locking

---
*Research completed: 2026-03-26*
*Ready for roadmap: yes*
