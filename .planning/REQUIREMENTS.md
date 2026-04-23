# Requirements: chromatindb v4.2.0

**Defined:** 2026-04-22
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v4.2.0 Requirements

Requirements for Storage Efficiency + Configurable Blob Cap. Shrink blob and frame limits to mdbx-efficient sizes, promote the blob cap to the operator's `config.json`, publish it via `NodeInfoResponse` + Prometheus so remote verification is possible, and teach the CLI + sync protocol to handle divergent caps cleanly.

### Blob Cap (BLOB)

- [ ] **BLOB-01**: Replace hardcoded `MAX_BLOB_DATA_SIZE` in `db/net/framing.h` with `Config::blob_max_bytes`; default 4 MiB (4 × 1024 × 1024)
- [ ] **BLOB-02**: Config bounds validation at startup — `blob_max_bytes` must be in `[1 MiB, 64 MiB]`; out-of-range values fail config load with clear error
- [ ] **BLOB-03**: `blob_max_bytes` is SIGHUP-reloadable; new writes honor new cap; existing stored blobs remain readable when cap is lowered (no migration, no deletion)
- [ ] **BLOB-04**: Ingest rejection of oversized blob continues to use existing `oversized_blob` IngestResult; error message reflects the actual current cap (not a baked-in constant)

### Frame Size (FRAME)

- [ ] **FRAME-01**: Lower `MAX_FRAME_SIZE` from 110 MiB to **2 MiB** in both `db/net/framing.h` and `cli/src/connection.cpp`
- [ ] **FRAME-02**: `MAX_FRAME_SIZE` remains a fixed protocol constant (not configurable); `static_assert` documents the relationship `MAX_FRAME_SIZE ≈ 2 × STREAMING_THRESHOLD + AEAD margin`

### NodeInfoResponse Extensions (NODEINFO)

- [ ] **NODEINFO-01**: `NodeInfoResponse` wire format adds `max_blob_data_bytes` (u64 BE)
- [ ] **NODEINFO-02**: `NodeInfoResponse` wire format adds `max_frame_bytes` (u32 BE)
- [ ] **NODEINFO-03**: `NodeInfoResponse` wire format adds `rate_limit_bytes_per_sec` (u64 BE) — renamed + retyped from the original (u32 BE) spec per Phase 127 CONTEXT.md D-03; exposes the value the node actually enforces (`config.rate_limit_bytes_per_sec`) instead of a ghost metric the node does not track
- [ ] **NODEINFO-04**: `NodeInfoResponse` wire format adds `max_subscriptions_per_connection` (u32 BE)

### Prometheus Config Gauges (METRICS)

- [ ] **METRICS-01**: Every numeric field in the `Config` struct is exposed as a `chromatindb_config_<field_name>` gauge under the existing `/metrics` endpoint
- [ ] **METRICS-02**: Gauges reflect live values — after SIGHUP reload, the new scrape returns updated values without node restart
- [x] **METRICS-03**: New counter `chromatindb_sync_skipped_oversized_total{peer=...}` increments once per sync-filter skip for operator visibility

### Sync Cap Divergence (SYNC)

- [x] **SYNC-01**: Peer handshake capability record carries the peer's advertised `max_blob_data_bytes`; stored per-connection in `PeerInfo`
- [x] **SYNC-02**: Sync announce-side filter — blobs whose `blob.data.size() > peer.advertised_blob_cap` are omitted from the set-reconciliation announce to that peer
- [x] **SYNC-03**: Filter applies uniformly to PULL reconciliation announce, PUSH `BlobNotify` fan-out, and direct `BlobFetch` response paths
- [x] **SYNC-04**: Each skip increments `chromatindb_sync_skipped_oversized_total{peer=...}` so operators can see partial-replication situations

### CLI Auto-tuning (CLI)

- [ ] **CLI-01**: CLI reads `max_blob_data_bytes` from `NodeInfoResponse` on connect; caches for the session
- [ ] **CLI-02**: `CHUNK_SIZE_BYTES_DEFAULT` derived from the cached server cap at connect time (replaces hardcoded 16 MiB in `cli/src/wire.h`)
- [ ] **CLI-03**: `CHUNK_THRESHOLD_BYTES` derived from the server cap — files `≥` server cap trigger CDAT/CPAR chunking (replaces hardcoded 400 MiB in `cli/src/chunked.h`)
- [ ] **CLI-04**: Manifest validator `CHUNK_SIZE_BYTES_MAX` bounded by server cap (replaces hardcoded 256 MiB); manifests that declare a chunk size exceeding the cap are rejected
- [ ] **CLI-05**: `MAX_CHUNKS` policy decision finalized in discuss-phase — either retain 65536 (256 GiB file ceiling at 4 MiB default) or grow to `1 << 20` (~4 TiB ceiling)

### Pre-shrink Audit (AUDIT)

- [x] **AUDIT-01**: Inventory every non-chunked single-frame response type (`BatchReadResponse`, `DelegationListResponse`, `NamespaceListResponse`, `PeerInfoResponse`, `NodeInfoResponse`, `TimeRangeResponse`, `StorageStatusResponse`, `NamespaceStatsResponse`) and document its worst-case payload size at existing request-level caps — **reframed per CONTEXT.md D-01..D-03 to a send-path inventory; zero bypass sites found (D-09); table in 126-SUMMARY.md §"Send-Path Inventory"**
- [x] **AUDIT-02**: Any response whose worst-case payload exceeds 2 MiB after AEAD + framing overhead either gets its request-level cap lowered or is moved to the streaming path; unit tests gate the 2 MiB invariant going forward — **satisfied structurally: send_message auto-streams >= STREAMING_THRESHOLD; runtime assert + static_assert on both db/ and cli/ sides; two round-trip [connection][streaming] Catch2 tests landed**

### Documentation Reconciliation (DOCS)

- [ ] **DOCS-01**: PROJECT.md blob-limit statement corrected — current reads "Larger blob limit: 100 MiB" (validated v2.0) but `MAX_BLOB_DATA_SIZE` is actually 500 MiB; update Validated requirement line and add the new v4.2.0 default
- [ ] **DOCS-02**: PROTOCOL.md frame + blob sections rewritten with `MAX_FRAME_SIZE=2 MiB`, `MAX_BLOB_DATA_SIZE=config.blob_max_bytes (default 4 MiB)`, and the rationale
- [ ] **DOCS-03**: PROTOCOL.md `NodeInfoResponse` wire format section extended with the four new fields, in byte-exact layout
- [ ] **DOCS-04**: PROTOCOL.md gains a "Sync Cap Divergence" subsection under the sync protocol spec, documenting the announce-side filter rule and its implications for writers
- [ ] **DOCS-05**: README.md config field table adds `blob_max_bytes` row with bounds + SIGHUP-reload note
- [ ] **DOCS-06**: README.md Prometheus section documents the `chromatindb_config_*` gauge family and the `chromatindb_sync_skipped_oversized_total` counter
- [ ] **DOCS-07**: cli/README.md chunking section updated — CLI auto-tunes chunk size from the server's advertised cap; explain the user-visible behavior (no `--chunk-size` flag; any file ≥ server cap is auto-chunked)
- [ ] **DOCS-08**: db/ARCHITECTURE.md Step-0 validation table row for `MAX_BLOB_DATA_SIZE` updated to reflect configurable cap

### Verification (VERI)

- [ ] **VERI-01**: Unit tests for `blob_max_bytes` config load, bounds validation, and SIGHUP reload
- [ ] **VERI-02**: Unit tests for `NodeInfoResponse` encode/decode covering the four new fields
- [x] **VERI-03**: Unit tests for the sync announce-filter logic (peer cap smaller / larger / equal / zero)
- [ ] **VERI-04**: Unit tests for `chromatindb_config_*` gauge emission on `/metrics` scrape
- [x] **VERI-05**: Integration test — 2-node topology with divergent caps (A=1 MiB, B=8 MiB); write a 6 MiB blob to B; assert A receives nothing for that blob; assert `chromatindb_sync_skipped_oversized_total{peer=A}` increments; assert that sub-cap blobs still replicate normally
- [ ] **VERI-06**: CLI integration test against the local node — connect, receive `max_blob_data_bytes` in NodeInfoResponse, auto-tune chunking, put+get a 64 MiB file and verify SHA3-256 round-trip

## Future Requirements

Deferred beyond v4.2.0.

### Tuning Evidence
- **BENCH-01**: Benchmark suite comparing `[1, 2, 4, 8, 16] MiB` blob caps for ingest throughput, sync throughput, and mdbx file-size growth under churn
- **BENCH-02**: Operator tuning guide documenting when to raise (`storage-heavy, high-core-count`) or lower (`memory-constrained, mobile-backed`) the cap

### Cap-aware Client Policy
- **CLIPOL-01**: CLI flag to override auto-tuned chunk size (`--chunk-size`) for advanced users
- **CLIPOL-02**: CLI discovers minimum cap across all known peers and chunks to that floor when `--broad-replication` is set

### Adaptive Behavior
- **ADAPT-01**: Adaptive cap — node shrinks `blob_max_bytes` under memory pressure; SIGHUP reload triggered automatically

## Out of Scope

| Feature | Reason |
|---------|--------|
| Backward compat with pre-v4.2.0 `NodeInfoResponse` wire format | Pre-MVP, no deployed ecosystem to preserve |
| Migration / deletion of existing oversized blobs when cap is lowered | Lowered cap only affects new writes; old blobs stay readable until TTL expiry |
| Renegotiating peer cap mid-session | Cap is a session-constant snapshot at handshake; SIGHUP on a peer takes effect on next reconnect |
| Sub-MiB caps | `STREAMING_THRESHOLD = 1 MiB` is the natural floor; caps below this serve no purpose |
| Caps > 64 MiB | Retains the "predictable per-blob memory working set" property; raise later with evidence if needed |
| Live remote config mutation (e.g. an admin RPC to change `blob_max_bytes`) | SIGHUP + `/metrics` verification is enough for MVP operations |
| String-valued Config fields as Prometheus gauges | Gauges are numeric; string fields (`bind_address`, `log_level`, paths) stay out of `/metrics` |
| Client-side policy that rejects writes exceeding any peer's cap | Writer chooses; database stays dumb |

## Traceability

Filled by the roadmapper after phase decomposition.

| Requirement | Phase | Status |
|-------------|-------|--------|
| AUDIT-01    | 126   | Complete |
| AUDIT-02    | 126   | Complete |
| NODEINFO-01 | 127   | Pending |
| NODEINFO-02 | 127   | Pending |
| NODEINFO-03 | 127   | Pending |
| NODEINFO-04 | 127   | Pending |
| VERI-02     | 127   | Pending |
| BLOB-01     | 128   | Pending |
| BLOB-02     | 128   | Pending |
| BLOB-03     | 128   | Pending |
| BLOB-04     | 128   | Pending |
| FRAME-01    | 128   | Pending |
| FRAME-02    | 128   | Pending |
| METRICS-01  | 128   | Pending |
| METRICS-02  | 128   | Pending |
| VERI-01     | 128   | Pending |
| VERI-04     | 128   | Pending |
| SYNC-01     | 129   | Complete |
| SYNC-02     | 129   | Complete |
| SYNC-03     | 129   | Complete |
| SYNC-04     | 129   | Complete |
| METRICS-03  | 129   | Complete |
| VERI-03     | 129   | Complete |
| VERI-05     | 129   | Complete |
| CLI-01      | 130   | Pending |
| CLI-02      | 130   | Pending |
| CLI-03      | 130   | Pending |
| CLI-04      | 130   | Pending |
| CLI-05      | 130   | Pending |
| VERI-06     | 130   | Pending |
| DOCS-01     | 131   | Pending |
| DOCS-02     | 131   | Pending |
| DOCS-03     | 131   | Pending |
| DOCS-04     | 131   | Pending |
| DOCS-05     | 131   | Pending |
| DOCS-06     | 131   | Pending |
| DOCS-07     | 131   | Pending |
| DOCS-08     | 131   | Pending |

**Coverage:**
- v4.2.0 requirements: 38 total
- Mapped to phases: 38 (all mapped)
- Unmapped: 0

**Phase totals:**
- Phase 126 (Pre-shrink Audit): 2 requirements (AUDIT-01, AUDIT-02)
- Phase 127 (NodeInfoResponse Capability Extensions): 5 requirements (NODEINFO-01..04, VERI-02)
- Phase 128 (Configurable Blob Cap + Frame Shrink + Config Gauges): 10 requirements (BLOB-01..04, FRAME-01..02, METRICS-01..02, VERI-01, VERI-04)
- Phase 129 (Sync Cap Divergence): 7 requirements (SYNC-01..04, METRICS-03, VERI-03, VERI-05)
- Phase 130 (CLI Auto-tuning): 6 requirements (CLI-01..05, VERI-06)
- Phase 131 (Documentation Reconciliation): 8 requirements (DOCS-01..08)
- Total: 2 + 5 + 10 + 7 + 6 + 8 = 38 ✓

---
*Requirements defined: 2026-04-22*
*Last updated: 2026-04-22 — roadmap decomposition complete (6 phases, 126-131, 100% mapped)*
