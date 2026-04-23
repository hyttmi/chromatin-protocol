# Phase 131: Documentation Reconciliation — Context

**Gathered:** 2026-04-23 (inline, lean execution, final v4.2.0 phase)
**Status:** Ready for planning

<domain>
## Phase Boundary

Close the v4.2.0 documentation drift: PROTOCOL.md, README.md, cli/README.md, ARCHITECTURE.md, and PROJECT.md all reflect the v4.2.0 shipping surface (operator-configurable blob cap, 2 MiB frame invariant, NodeInfoResponse capability extensions, sync cap divergence, CLI auto-tuning).

**In scope:**
- PROJECT.md "Larger blob limit: 100 MiB" line corrected (DOCS-01) — it was always wrong (MAX_BLOB_DATA_SIZE was 500 MiB, not 100), and is now configurable 1–64 MiB.
- PROTOCOL.md frame + blob sections rewritten for 2 MiB frame + configurable blob cap (DOCS-02).
- PROTOCOL.md NodeInfoResponse wire format extended with the 4 new fields at byte-exact offsets (DOCS-03).
- PROTOCOL.md new "Sync Cap Divergence" subsection (DOCS-04).
- README.md config field table gains `blob_max_bytes` row (DOCS-05).
- README.md Prometheus section gains `chromatindb_config_*` + `chromatindb_sync_skipped_oversized_total` docs (DOCS-06).
- cli/README.md chunking section rewritten for auto-tuning behavior (DOCS-07).
- db/ARCHITECTURE.md MAX_BLOB_DATA_SIZE row updated for configurable cap (DOCS-08).

**Out of scope:**
- Code changes — this is docs-only.
- README.md or ARCHITECTURE.md structural rewrites beyond the enumerated sections.
- Migrating old cli/BACKLOG.md references (that file is an internal backlog, not shipping docs; leave it until someone naturally touches it).

</domain>

<decisions>
## Implementation Decisions

### Source-of-truth anchors
- **D-01:** Every number in the docs MUST match the live code. For each edit, executor verifies against:
  - `db/net/framing.h` — `MAX_FRAME_SIZE`, `MAX_BLOB_DATA_HARD_CEILING`, `STREAMING_THRESHOLD`, `TRANSPORT_ENVELOPE_MARGIN`
  - `db/config/config.h` — `Config::blob_max_bytes` default (4 MiB), `validate_config` bounds `[1 MiB, 64 MiB]`
  - `db/peer/message_dispatcher.cpp` — NodeInfoResponse encoder (byte offsets for DOCS-03)
  - `db/peer/metrics_collector.cpp` — gauge names (DOCS-06)
  - `cli/src/chunked.cpp` — session-cap-derived chunking (DOCS-07)

### Style
- **D-02:** Match existing document voice per file. PROTOCOL.md is spec-formal; READMEs are operator-friendly; ARCHITECTURE.md is developer-contextual. Do not rewrite style, only update facts.
- **D-03:** Tables stay tables. Bullet lists stay bullet lists. Where an existing row needs updating, update the row — don't restructure the surrounding table.
- **D-04:** When deleting obsolete content, delete cleanly — do NOT leave "previously this was X" historical notes. Git history is the audit trail.

### Specific wording for the 4 new NodeInfoResponse fields (DOCS-03)
- **D-05:** Reuse the byte-layout table from `.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md` §"interfaces" — same exact format. Operators reading PROTOCOL.md should see identical markings as developers reading CONTEXT.md.

### Sync Cap Divergence subsection (DOCS-04)
- **D-06:** 4-5 paragraphs max. Cover: (a) problem statement (nodes with divergent caps), (b) PeerInfo snapshot at handshake, (c) the 3 filter sites (announce, BlobNotify fan-out, BlobFetch response), (d) operator consequence (oversized blobs don't replicate to smaller-cap peers; counter visible in /metrics), (e) recovery pattern (reconnect picks up new cap — cap is session-constant).

### CLI auto-tuning (DOCS-07)
- **D-07:** Explicitly state: no `--chunk-size` flag, no operator decision. CLI reads server cap on connect; files ≤ cap go as a single blob; files > cap get CDAT/CPAR chunked with chunk_size == server cap.

### No executable verification, no build gate
- **D-08:** Docs don't compile. The gate is: no markdown linting errors (simple grep), no surviving references to the old constants (`MAX_BLOB_DATA_SIZE = 500 MiB`, `MAX_FRAME_SIZE = 110 MiB`) in the 5 updated files.

### Claude's Discretion
- Exact wording of each updated paragraph.
- Whether to collapse or keep distinct multi-paragraph sections if the existing doc has one giant paragraph to split.
- Cross-references between docs (e.g., "see PROTOCOL.md §X") — add where useful, don't force.

</decisions>

<canonical_refs>
## Canonical References

### Project-level
- `.planning/REQUIREMENTS.md` — DOCS-01..08 (authoritative)
- `.planning/ROADMAP.md` §"Phase 131"
- `.planning/PROJECT.md` — the file this phase partially updates (DOCS-01)

### Phase-level carryover (source of truth for every number)
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md` §interfaces (DOCS-03 byte layout)
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md` (encoder authoritative)
- `.planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-CONTEXT.md` (D-01..D-17 for DOCS-02/05/06/08)
- `.planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-04-SUMMARY.md` (24 gauge names for DOCS-06)
- `.planning/phases/129-sync-cap-divergence/129-CONTEXT.md` (DOCS-04 content source)
- `.planning/phases/129-sync-cap-divergence/129-02-SUMMARY.md` (3 filter sites authoritative for DOCS-04)
- `.planning/phases/130-cli-auto-tuning/130-CONTEXT.md` (DOCS-07 content source)
- `.planning/phases/130-cli-auto-tuning/130-01-SUMMARY.md` (CLI behavior authoritative for DOCS-07)

### Codebase anchors (executor must cross-reference these when updating)
- `db/net/framing.h` — every current constant the docs must cite
- `db/config/config.h` — `blob_max_bytes` default + SIGHUP-reloadable note
- `db/peer/message_dispatcher.cpp:660-740` — NodeInfoResponse encoder (byte offsets)
- `db/peer/metrics_collector.cpp` — gauge emission (names + HELP text to cite in DOCS-06)
- `db/peer/peer_types.h` — PeerInfo::advertised_blob_cap (for DOCS-04 reference)
- `cli/src/connection.cpp` + `cli/src/chunked.cpp` — auto-tune behavior (for DOCS-07)

### Files to modify
- `.planning/PROJECT.md` (DOCS-01 — blob limit line)
- `db/PROTOCOL.md` (DOCS-02, DOCS-03, DOCS-04)
- `db/README.md` OR root `README.md` (DOCS-05, DOCS-06 — executor decides which is the "operator README" based on content; use grep to check which one has Config tables / Prometheus section)
- `cli/README.md` (DOCS-07)
- `db/ARCHITECTURE.md` (DOCS-08)

</canonical_refs>

<deferred>
## Deferred Ideas

- **cli/BACKLOG.md cleanup** — internal file, not shipping docs. Update when someone naturally touches it.
- **Cross-version migration guide** — no backward compat policy means we don't need one, but if v5.0 ever introduces migration, a MIGRATING.md would be its home.
- **Operator runbook for post-SIGHUP scrape verification** — a concrete "how do I confirm my cap change landed" recipe. Useful but separable.
- **Protocol diff changelog** — a PROTOCOL.md-CHANGES.md listing wire-format deltas per milestone. Interesting but not yet asked for.

</deferred>
