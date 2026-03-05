# Project Research Summary

**Project:** chromatindb v2.0
**Domain:** Closed node access control + larger blob support for decentralized PQ-secure database node
**Researched:** 2026-03-05
**Confidence:** HIGH

## Executive Summary

chromatindb v2.0 is a focused extension of the v1.0 PQ-secure blob store daemon. The two core deliverables are a closed node model (pubkey whitelist) and support for blobs up to 100 MiB. Research confirms that both are achievable with zero new dependencies — every existing library handles the v2.0 requirements within its documented limits. The implementation is mechanical changes to a well-understood codebase, not greenfield work. libmdbx supports values up to ~2 GiB, FlatBuffers handles buffers up to ~2 GiB, ChaCha20-Poly1305 supports messages up to ~256 GiB, and the existing 4-byte frame length prefix supports frames up to ~4 GiB. All changes are constant bumps, config additions, and targeted logic changes in existing files.

The recommended approach is three sequential phases: source restructure first (namespace rename + move to /db), then access control, then larger blob support. The ordering is non-negotiable. The namespace rename touches every file mechanically and must precede feature work to avoid merge conflicts. Access control is architecturally simpler and higher-value, so it goes before blob size changes. Blob support has the most subtle changes (sync batching, potential storage architecture pivot) and benefits from a clean, already-restructured codebase.

The primary risk is the storage architecture decision for large blobs: whether to keep blobs inline in libmdbx (simpler code, ACID) or move to external filesystem storage (avoids overflow page fragmentation and write amplification). This decision cascades into the sync protocol and expiry scan design. The secondary risk is the sync protocol memory budget — the current hash collection implementation loads full blob data just to compute hashes, which would cause OOM with 100 MiB blobs. Both must be resolved before any large blob implementation begins.

## Key Findings

### Recommended Stack

No new dependencies are required. The v1.0 stack handles v2.0 requirements natively. All changes are constant bumps and targeted code modifications in existing files. The build process is unchanged.

**Core technologies (unchanged from v1.0):**
- `libmdbx`: blob storage — handles 100 MiB values natively via overflow pages; no changes to storage.cpp API
- `libsodium` (ChaCha20-Poly1305): transport encryption — supports 100 MiB messages with no modification (~256 GiB max)
- `FlatBuffers`: wire format — 32-bit offsets handle 100 MiB blobs; `FlatBufferBuilder` initial size needs dynamic sizing to avoid ~14 reallocation copies for large data
- `liboqs` (ML-DSA-87, ML-KEM-1024): PQ handshake — already exposes `peer_pubkey_` after handshake; access control hooks naturally here
- `nlohmann/json`: config — already parses arrays; `allowed_keys` is a new JSON array field, no new parser needed
- `Asio`: networking — `async_read` handles variable-size payloads without changes; `asio::signal_set` (already in codebase) used for SIGHUP config reload

**Critical constant changes:**
- `MAX_FRAME_SIZE`: 16 MiB -> 110 MiB (100 MiB data + ~7.3 KiB PQ overhead + 16 B AEAD tag + ~10% headroom)
- `FlatBufferBuilder` initial size: dynamic — `std::max(size_t(8192), blob.data.size() + 16384)`
- New: `MAX_BLOB_DATA_SIZE = 100 * 1024 * 1024` (constexpr protocol invariant, like TTL)
- New: `MAX_HASHES_PER_REQUEST` (caps BlobRequest batch size in sync to bound responder memory)

### Expected Features

**Must have (table stakes — v2.0 core):**
- `allowed_keys` config — JSON array of hex-encoded namespace IDs (SHA3-256 of pubkey, 64 hex chars); non-empty = closed mode, empty = open (backward compatible with v1.0)
- Connection-level auth gating — reject unauthorized peers immediately after handshake in `Connection::run()`, before `on_ready` fires; denied peers never enter PeerManager state
- Blob size limit enforcement — `MAX_BLOB_DATA_SIZE` checked as Step 0 in `BlobEngine::ingest()`, before signature verification (cheapest rejection)
- Frame size increase — `MAX_FRAME_SIZE` bumped to 110 MiB to fit 100 MiB blob + protocol overhead
- Sync blob transfer batching — split BlobRequests by `MAX_HASHES_PER_REQUEST` to prevent multi-hundred-MiB single-allocation sync messages; fix hash collection to not load blob data

**Should have (differentiators worth building):**
- SIGHUP config reload — hot-reload `allowed_keys` without daemon restart; use `asio::signal_set` (already in codebase), not raw signal handlers (which would create thread safety issues)
- Disconnect revoked peers — on config reload, iterate connected peers and close connections whose pubkey is no longer in `allowed_keys`; `close_gracefully()` pattern already exists
- Memory-aware blob reception — validate declared frame length against `MAX_BLOB_DATA_SIZE + overhead` before allocating recv buffer; prevents forced 4 GiB allocation from malformed frame headers

**Defer (post-v2.0):**
- Per-peer write restriction (read-only vs read+write) — adds config schema complexity; YAGNI for initial closed model
- Chunked/streaming blob transfer — only necessary at 1+ GiB; ML-DSA-87 requires full data in memory for signing regardless
- Namespace-level ACLs — belongs in Layer 2 (Relay), not the database node
- inotify-based config watching — SIGHUP is the correct Unix convention

### Architecture Approach

The v1.0 architecture has exactly the right hooks for both features. The ML-DSA-87 handshake already sets `peer_pubkey_` before `on_ready` fires, making `Connection::run()` the correct access control check point. BlobEngine's 4-stage ingest pipeline accepts a size check as Step 0 with no structural changes. The sync protocol's existing `pending_responses` counter and BlobTransfer message type support the batching change without wire format modifications.

**Major components and v2.0 changes:**
1. `config::Config` — add `allowed_keys` (vector<string> of hex-encoded namespace IDs), parse from JSON array
2. `net::Connection` — add `AccessPolicy` callback type, check after handshake in `run()` before `on_ready`; if not allowed, call `close()` and `close_cb_`, never fire `on_ready`
3. `net::Server` — propagate access policy to each Connection on creation
4. `peer::PeerManager` — build AccessPolicy lambda from config (O(1) unordered_set lookup); disable PEX when in closed mode (2-line change); handle SIGHUP via `asio::signal_set`; disconnect revoked peers on reload
5. `engine::BlobEngine` — add `IngestError::too_large`, check `data.size()` as Step 0 in `ingest()`
6. `net/framing.h` — bump `MAX_FRAME_SIZE` to 110 MiB
7. `sync/sync_protocol.cpp` — fix hash collection to iterate seq_map cursor directly (no blob data load); send one BlobTransfer per blob (not batch); add `MAX_HASHES_PER_REQUEST` constant

**Source restructure (prerequisite to all features):**
- Move to `db/` directory layout; move `src/`, `tests/`, `schemas/`, `CMakeLists.txt`
- Rename `chromatin::` namespace to `chromatindb::` across all ~73 files + FlatBuffers schemas + regenerate headers
- Must be a single atomic commit, followed by clean build (`rm -rf build`) + full test run (155 tests)

### Critical Pitfalls

1. **Access control at the wrong layer** — Checking `allowed_keys` in `BlobEngine::ingest()` allows unauthorized peers to freely read all data (no access checks on get/list operations) and creates a DoS vector (full ML-KEM handshake CPU cost before rejection). Enforce in `Connection::run()` after `do_handshake()` sets `peer_pubkey_`, before `on_ready` fires. Never add access checks to the engine layer.

2. **Memory exhaustion from sync hash collection** — `SyncProtocol::collect_namespace_hashes()` currently calls `get_blobs_since()` which loads full blob data just to compute 32-byte hashes. With ten 100 MiB blobs, this allocates ~2 GiB. Fix: rewrite to iterate the seq_map cursor directly and read only hash values, never loading blob data. This is a prerequisite that must be completed before bumping the blob size limit.

3. **libmdbx overflow page fragmentation** — 100 MiB values consume ~25,600 contiguous overflow pages. Allocating contiguous pages in a fragmented database is slow; expiry scanning many large blobs in one transaction can stress freelist management. Options: (a) store blob data externally on filesystem using content hash as filename (standard pattern in Git, IPFS, container registries), keeping only metadata in libmdbx; or (b) keep blobs in libmdbx and increase page size to 65536 bytes (16x fewer overflow pages, but requires database migration). This architectural decision must be made before Phase 3 implementation.

4. **Sync timeout too short for large blobs** — The 30-second `SYNC_TIMEOUT` assumes small blobs. At 10 MB/s, three 100 MiB blobs hit the timeout exactly and sync permanently fails. Make the timeout adaptive based on estimated transfer size, or implement per-blob transfer with acknowledgments that create a natural progress heartbeat.

5. **Source restructure build breakage** — CMakeLists.txt has hardcoded source file lists and FlatBuffers output paths. Moving files without updating all references causes linker errors. FlatBuffers schema namespace (`chromatin.wire`) must match C++ namespace or generated headers will be in the wrong namespace. Always do a clean build after restructuring, not incremental.

## Implications for Roadmap

Three phases are clearly implied by the architecture and dependency chain. The ordering reflects hard technical dependencies and risk minimization, not preference.

### Phase 1: Source Restructure
**Rationale:** Mechanical rename touches every file. Doing it first eliminates merge conflicts with all feature work. Zero functional risk — testable immediately against the existing 155-test suite. Must be a single atomic commit.
**Delivers:** `db/` directory layout, `chromatindb::` namespace throughout all source files, regenerated FlatBuffers headers, all 155 tests passing after clean rebuild.
**Avoids:** Pitfall 7 (CMakeLists stale builds from stale objects), Pitfall 11 (namespace cascade causing FlatBuffers schema mismatch)
**Note:** If the namespace rename is not required by the milestone spec, skip it. Phase 1 becomes directory restructure only — much smaller scope.

### Phase 2: Access Control (Closed Node Model)
**Rationale:** Higher-value primary deliverable; lower risk than blob size changes; modifies fewer files; establishes the `AccessPolicy` callback pattern cleanly before sync changes add complexity to PeerManager. Access control and blob support are functionally independent — doing access control first keeps each phase focused.
**Delivers:** `allowed_keys` config field, connection-level gating in `Connection::run()`, PEX disabled in closed mode, SIGHUP reload via `asio::signal_set`, disconnect of revoked peers on reload.
**Implements:** `config::Config` + `net::Connection` + `net::Server` + `peer::PeerManager` changes
**Avoids:** Pitfall 1 (wrong layer), Pitfall 6 (SIGHUP raw signal handler thread safety), Pitfall 8 (TOCTOU — revoked peers disconnected promptly on reload)

### Phase 3: Larger Blob Support
**Rationale:** Depends on Phase 1 for the clean namespace. Contains the highest-risk change (sync batching + storage architecture decision). Benefits from Phase 2 being complete so PeerManager changes don't conflict.
**Delivers:** `MAX_FRAME_SIZE` = 110 MiB, `MAX_BLOB_DATA_SIZE` = 100 MiB constexpr, size check as Step 0 in `BlobEngine::ingest()`, fixed hash collection (seq_map cursor iteration, no blob data load), one-blob-per-transfer sync, adaptive sync timeout, dynamic `FlatBufferBuilder` initial sizing.
**Uses:** libmdbx overflow pages or external filesystem storage (decision required before coding)
**Avoids:** Pitfall 2 (sync OOM from hash collection and batch transfer), Pitfall 3 (libmdbx fragmentation), Pitfall 4 (sync timeout), Pitfall 5 (frame/blob size constant mismatch), Pitfall 9 (missing size validation in ingest)

### Phase Ordering Rationale

- Phase 1 must be first: touching 73+ files for a mechanical rename while simultaneously adding features makes code review impossible and guarantees conflicts.
- Phase 2 before Phase 3: access control modifies the handshake/connection layer; blob support modifies the engine/sync layer. Keeping them separate ensures each is auditable. Access control is also the primary v2.0 goal.
- Phase 3 last: the sync protocol changes have the most subtle correctness requirements (pending_responses counter, one-blob-per-transfer invariant, adaptive timeouts). A clean, tested codebase reduces debugging burden.

### Research Flags

Needs resolution before coding Phase 3:
- **Phase 3 — storage architecture:** Inline libmdbx (simpler code, ACID, ~25,600 overflow pages per 100 MiB blob, fragmentation risk) vs external filesystem (standard blob store pattern, eliminates overflow issues, adds two consistency domains). Must decide at phase planning time. Recommendation: if YAGNI applies, stay in libmdbx with page size increased to 65536; if correctness of expiry and fragmentation concern the operator, use external storage. This is the only open architectural question in the milestone.

Standard patterns (no additional research needed):
- **Phase 1:** Mechanical find-and-replace + CMake path updates. Well-understood.
- **Phase 2:** Connection-gated access control after handshake is an established pattern (Quorum, Tendermint, strfry). All integration points are identified with specific file and function references. Config reload via `asio::signal_set` is already in the codebase.
- **Phase 3 (except storage decision):** Frame size bump, engine size check, sync batching — all changes are identified with specific files, functions, and line-level rationale. No library unknowns.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All constants verified against official docs and v1.0 source. libmdbx ~2 GiB limit, FlatBuffers ~2 GiB limit, ChaCha20 ~256 GiB limit — all confirmed. Zero new dependencies. |
| Features | HIGH | Table stakes are direct analogs of patterns from strfry (pubkey whitelist), Quorum (disconnect after handshake), standard Unix daemons (SIGHUP). Anti-features are well-reasoned exclusions. |
| Architecture | HIGH | Based on direct analysis of v1.0 source with line-level references. Integration points are specific file+function references, not estimates. |
| Pitfalls | HIGH | Critical pitfalls derived from codebase analysis with specific line numbers (e.g., sync_protocol.cpp:26-44, connection.cpp:66-89). Hash collection OOM is a verifiable bug in current code. |

**Overall confidence:** HIGH

### Gaps to Address

- **Storage architecture decision (Phase 3):** Research identifies the tradeoffs clearly but does not prescribe the final answer. The v1.0 YAGNI principle favors staying in libmdbx; correct large-blob engineering favors external storage. Decide explicitly before Phase 3 planning.

- **Memory budget validation:** Peak memory with 100 MiB blobs is ~300-500 MiB per connection (4 copies of blob in flight during ingest). Research recommends documenting 4 GiB RAM minimum for nodes handling large blobs. Validate during Phase 3 integration testing.

- **Namespace rename necessity:** Research flags the `chromatin::` -> `chromatindb::` rename as optional. If not required by milestone spec, skip it. Phase 1 scope drops from ~73 files to directory move + CMakeLists.txt updates only.

## Sources

### Primary (HIGH confidence)
- [libmdbx C API docs](https://libmdbx.dqdkfa.ru/group__c__api.html) — `MDBX_MAXDATASIZE = 0x7FFF0000`, overflow page handling
- [libmdbx GitHub](https://github.com/erthink/libmdbx) — page size defaults, geometry configuration, overflow page issue #192
- [FlatBuffers Internals](https://flatbuffers.dev/internals/) — 32-bit offset limit ~2 GiB; FlatBuffers issues #7537, #7391 confirm limit
- [libsodium ChaCha20-Poly1305 docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305) — IETF variant ~256 GiB per-message limit
- [RFC 7539](https://datatracker.ietf.org/doc/html/rfc7539) — IETF AEAD construction, 32-bit block counter
- [TOCTOU CWE-367](https://cwe.mitre.org/data/definitions/367.html) — revocation timing analysis
- chromatindb v1.0 source code — direct analysis of all 40+ source/header files with line-level references

### Secondary (MEDIUM confidence)
- [strfry Nostr relay](https://github.com/hoytech/strfry) — pubkey whitelist and connection-level gating patterns
- [Quorum p2p auth](https://github.com/ConsenSys/quorum/pull/897/files) — disconnect unauthorized peers after handshake
- [SIGHUP convention](https://blog.devtrovert.com/p/sighup-signal-for-configuration-reloads) — Unix daemon reload pattern
- [Reth libmdbx large value discussion](https://github.com/paradigmxyz/reth/issues/19546) — real-world large value usage (third-party, not official)

### Tertiary (LOW confidence)
- [p2panda access control](https://p2panda.org/2025/07/28/access-control.html) — CRDT-based ACL design, referenced as anti-pattern only

---
*Research completed: 2026-03-05*
*Ready for roadmap: yes*
