---
plan: 131-01-docs-refresh
phase: 131
type: execute
wave: 1
status: complete
completed: 2026-04-24
requirements-completed: [DOCS-01, DOCS-02, DOCS-03, DOCS-04, DOCS-05, DOCS-06, DOCS-07, DOCS-08]
files-modified:
  - .planning/PROJECT.md
  - db/PROTOCOL.md
  - db/README.md
  - cli/README.md
  - db/ARCHITECTURE.md
---

# Plan 131-01 Summary: Documentation Reconciliation

All 8 DOCS-* requirements closed. Completed across two sessions — the initial executor hit the weekly token limit after landing only DOCS-01 (single-line PROJECT.md Validated fix); the remainder (DOCS-02..08) was completed inline by the orchestrator after the weekly reset.

## Deliverables

### DOCS-01 — PROJECT.md blob limit correction
- Fixed Validated requirements row (committed by initial executor in `7ba5c1b5`).
- Additionally corrected the "What This Is" opening paragraph (line 5) — previously read "blobs up to 100 MiB", now describes the operator-tunable `blob_max_bytes` with CDAT/CPAR chunking for larger files.

### DOCS-02 — PROTOCOL.md frame + blob sections
- Frame-size line: `110 MiB` → `MAX_FRAME_SIZE = 2 MiB` with rationale (paired `static_assert`s relating to `STREAMING_THRESHOLD` + `TRANSPORT_ENVELOPE_MARGIN`) + explicit note that frame is NOT a per-blob cap.
- Chunked Transport Framing blurb: `up to 500 MiB` → `Config::blob_max_bytes`, tunable [1 MiB, 64 MiB], hard ceiling `MAX_BLOB_DATA_HARD_CEILING = 64 MiB`.
- Blob schema comment (line 252): `max 500 MiB` → `Config::blob_max_bytes` with default + bounds.
- NAME payload footer: updated to reference configurable cap.

### DOCS-03 — PROTOCOL.md NodeInfoResponse wire format
- Table extended with 4 new rows: `max_blob_data_bytes` (u64), `max_frame_bytes` (u32), `rate_limit_bytes_per_sec` (u64), `max_subscriptions_per_connection` (u32) — inserted between `storage_max` and `types_count`, matching Phase 127 encoder.
- Every row includes the v4.2.0 marker and the consumer (cdb session-cache / peer advertised_blob_cap).
- Added a post-v4.2.0 consumer paragraph explaining the two downstream paths + protocol-break notice.

### DOCS-04 — PROTOCOL.md Sync Cap Divergence subsection
- New subsection added under `## Sync Protocol`, positioned after `### SyncNamespaceAnnounce` and before `### ErrorResponse`.
- 6 paragraphs cover: cap-snapshot at handshake, the 3 filter sites (PULL announce, BlobNotify fan-out, BlobFetch response), operator visibility via the labeled Prometheus counter, cap-unknown conservative fallback, and the cap-change operator workflow (SIGHUP + reconnect).
- Cross-references to `§NodeInfoResponse` and cli/README.md.

### DOCS-05 — db/README.md config row
- `blob_max_bytes` added to the `config.json` JSON block + bullet documentation (default 4 MiB, bounds 1–64 MiB, SIGHUP-reloadable).
- SIGHUP line (Signals section) extended to list `blob_max_bytes`.
- Wire Protocol section: `maximum frame size is 110 MiB` → `MAX_FRAME_SIZE = 2 MiB` with correct rationale.

### DOCS-06 — db/README.md Prometheus `/metrics` section
- Brand-new top-level section added between `## Signals` and `## Wire Protocol`.
- Documents: `metrics_bind` config, scrape example, runtime counters, gauges, the `chromatindb_config_*` gauge family (v4.2.0, 1:1 mirror of numeric Config fields), and `chromatindb_sync_skipped_oversized_total{peer=...}` (v4.2.0 cap-divergence visibility).
- Cross-references PROTOCOL.md §Sync Cap Divergence.
- Explicit no-auth/no-TLS note with reverse-proxy recommendation.

### DOCS-07 — cli/README.md Chunked Large Files section
- Rewritten from the ground up to describe auto-tuning: chunk_size = server cap, no `--chunk-size` flag, explicit hard-fail on pre-v4.2.0 node.
- File-size ceiling documented: `MAX_CHUNKS × server_cap` (256 GiB at 4 MiB default, 4 TiB at 64 MiB hard ceiling).
- New paragraph on cap divergence across peered nodes, cross-referencing PROTOCOL.md §Sync Cap Divergence + the skip counter.
- Three adjacent stale references to "400 MiB" chunking threshold (lines 139, 156, 230) migrated to "server's advertised blob cap" phrasing.

### DOCS-08 — db/ARCHITECTURE.md Step-0 validation row
- Row updated: `blob.data.size() <= MAX_BLOB_DATA_SIZE` → `blob.data.size() <= blob_max_bytes_` with the full operator/protocol-invariant context inline. Line pointer updated from `engine.cpp:109` to `engine.cpp:112` to match Phase 128 wiring.

## Verification (grep-based gates)

| Check | Result |
|-------|--------|
| Stale `500 MiB.*blob` refs across 4 shipping docs | **0 matches** |
| Stale `110 MiB.*frame` / `115343360` refs | **0 matches** |
| Stale `CHUNK_SIZE_BYTES_*` / `CHUNK_THRESHOLD_BYTES` in cli/README.md | 0 literal refs (2 false-positive matches in grep: "16 MiB-cap node" is an example, "no `--chunk-size` flag" is an explicit denial — both correct) |
| "Sync Cap Divergence" section exists in PROTOCOL.md | 2 occurrences (heading + cross-ref) |
| `chromatindb_config_*` + `chromatindb_sync_skipped_oversized_total` in db/README.md | Both documented |
| `blob_max_bytes` config row present in db/README.md | 5 occurrences (JSON example, bullet, SIGHUP line, Prometheus section, cross-ref) |

## Not run

- `cmake --build` — docs-only phase, no code touched.
- Test suite — docs-only phase.
- Link checker — no markdown linter configured; manual review pass.

## Files changed

- `.planning/PROJECT.md` (+1/−1 "What This Is", +1/−1 Validated row from prior commit)
- `db/PROTOCOL.md` (+16/−3 across frame, blob schema, NAME payload, NodeInfoResponse table, Sync Cap Divergence new section)
- `db/README.md` (+26/−2 across config JSON, config bullets, SIGHUP, Wire Protocol frame line, new Prometheus section)
- `cli/README.md` (+13/−7 across chunking section, NAME constraint, CPAR cascade example, put command table row)
- `db/ARCHITECTURE.md` (+1/−1 Step-0 row)

## Closes v4.2.0 milestone

Phase 131 is the final phase of milestone v4.2.0. All 5 code phases (126–130) shipped; this phase closes the doc-drift that accumulated during them. User-delegated UATs VERI-05 (Phase 129 2-node cap divergence) and VERI-06 (Phase 130 64 MiB CLI round-trip) remain pending user execution but do not block milestone completion.
