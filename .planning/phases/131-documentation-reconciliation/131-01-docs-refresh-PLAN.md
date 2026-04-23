---
plan: 131-01-docs-refresh
phase: 131
type: execute
wave: 1
depends_on: []
files_modified:
  - .planning/PROJECT.md
  - db/PROTOCOL.md
  - db/README.md
  - README.md
  - cli/README.md
  - db/ARCHITECTURE.md
autonomous: true
requirements: [DOCS-01, DOCS-02, DOCS-03, DOCS-04, DOCS-05, DOCS-06, DOCS-07, DOCS-08]
must_haves:
  truths:
    - "Zero remaining references to `MAX_BLOB_DATA_SIZE = 500 MiB` or `MAX_FRAME_SIZE = 110 MiB` across PROTOCOL.md / db/README.md / README.md / cli/README.md / ARCHITECTURE.md"
    - "Every cited constant matches live code (executor greps live framing.h + config.h before accepting each cited value)"
    - "NodeInfoResponse section in PROTOCOL.md includes the 4 new fields with byte-exact offsets matching 127-CONTEXT.md §interfaces"
    - "PROTOCOL.md gains a dedicated 'Sync Cap Divergence' subsection (DOCS-04) with content per CONTEXT.md D-06"
    - "README config-table + Prometheus-section additions cite the 24 `chromatindb_config_*` gauges and the labeled `chromatindb_sync_skipped_oversized_total` counter"
    - "cli/README.md chunking section states: auto-tunes from server cap on connect, no --chunk-size flag, files > cap get CDAT/CPAR chunked"
    - "db/ARCHITECTURE.md MAX_BLOB_DATA_SIZE row reflects configurable cap (operator tunable [1 MiB, 64 MiB], default 4 MiB)"
  artifacts:
    - path: "db/PROTOCOL.md"
      provides: "DOCS-02/03/04 — frame + blob sections + NodeInfoResponse wire format + sync cap divergence subsection"
    - path: "README.md (or db/README.md — executor decides)"
      provides: "DOCS-05/06 — blob_max_bytes config row + Prometheus section additions"
    - path: "cli/README.md"
      provides: "DOCS-07 — auto-tuning chunking section"
    - path: "db/ARCHITECTURE.md"
      provides: "DOCS-08 — MAX_BLOB_DATA_SIZE validation row updated for configurable cap"
    - path: ".planning/PROJECT.md"
      provides: "DOCS-01 — corrected blob limit validated line"
---

<objective>
Reconcile all shipping docs with the v4.2.0 surface. No code changes. Docs-only phase; gate is "every cited number matches live code, no stale references survive".
</objective>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: DOCS-01 — PROJECT.md blob-limit line correction</name>
  <files>.planning/PROJECT.md</files>
  <read_first>
    - .planning/PROJECT.md (full — find the "Larger blob limit: 100 MiB" validated line in the v2.0 requirements block)
  </read_first>
  <action>
    Find the line in PROJECT.md that reads:
    ```
    - ✓ Larger blob limit: 100 MiB for medium files (documents, images, small archives) — v2.0
    ```
    (or similar — it's in the Validated requirements block, tagged v2.0)

    Replace with TWO lines that correctly reflect history:
    ```
    - ✓ Larger blob limit introduced: `MAX_BLOB_DATA_SIZE` raised to 500 MiB — v2.0
    - ✓ Blob limit made operator-tunable: `Config::blob_max_bytes` default 4 MiB, bounds [1 MiB, 64 MiB], SIGHUP-reloadable; replaces the 500 MiB hardcoded ceiling with `MAX_BLOB_DATA_HARD_CEILING = 64 MiB` — v4.2.0 Phase 128
    ```

    If the v4.2.0 Phase 128 bullet already exists in the Validated block (from a prior session), keep the existing v4.2.0 line AS-IS and only fix the incorrect v2.0 line (the "100 MiB" claim). Do NOT duplicate.

    No other edits to PROJECT.md in this task.
  </action>
  <verify>
    <automated>
      grep -c '100 MiB' .planning/PROJECT.md
      # ^ should be 0 (or if it remains for some other context, verify contextually)
      grep -c '500 MiB.*v2\.0\|MAX_BLOB_DATA_HARD_CEILING.*v4\.2\.0' .planning/PROJECT.md
      # ^ >= 1 each
    </automated>
  </verify>
  <acceptance_criteria>
    - The incorrect "100 MiB" claim removed.
    - Historical v2.0 fact preserved (500 MiB ceiling at that milestone).
    - v4.2.0 configurable-cap fact present (already added in Phase 128 close-out; idempotent if present).
  </acceptance_criteria>
  <done>
    DOCS-01 closed. PROJECT.md no longer claims a wrong historical blob limit.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 2: DOCS-02/03/04 — PROTOCOL.md rewrite (frame + blob + NodeInfoResponse + sync cap divergence)</name>
  <files>db/PROTOCOL.md</files>
  <read_first>
    - db/PROTOCOL.md (full file — identify all sections touching frame size, blob size, NodeInfoResponse, and sync)
    - db/net/framing.h (authoritative constants)
    - db/config/config.h (blob_max_bytes default + bounds)
    - db/peer/message_dispatcher.cpp:660-740 (NodeInfoResponse encoder for byte offsets)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md §interfaces (byte-layout table source)
    - .planning/phases/129-sync-cap-divergence/129-CONTEXT.md (DOCS-04 content source)
    - .planning/phases/129-sync-cap-divergence/129-02-SUMMARY.md (3 filter sites authoritative)
  </read_first>
  <action>
    **DOCS-02 (frame + blob sections):**
    Find PROTOCOL.md sections citing `MAX_FRAME_SIZE` or `MAX_BLOB_DATA_SIZE`. Rewrite those sections with:
    - `MAX_FRAME_SIZE = 2 MiB` (protocol invariant, pinned by paired `static_assert`s relating it to `STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN`).
    - Rationale: the frame is sized to fit exactly one streaming sub-frame plus AEAD envelope margin; it is NOT a per-blob cap.
    - `MAX_BLOB_DATA_SIZE` is NO LONGER a constant — it's `Config::blob_max_bytes` at runtime, default 4 MiB, operator-tunable in `[1 MiB, 64 MiB]` via `config.json` with SIGHUP hot-reload.
    - `MAX_BLOB_DATA_HARD_CEILING = 64 MiB` is the protocol upper bound (build-time invariant; operators cannot exceed it).

    Delete any historical claim that the frame is ~110 MiB. Delete claims that blobs are hard-capped at 500 MiB.

    **DOCS-03 (NodeInfoResponse wire format):**
    Find the existing NodeInfoResponse section in PROTOCOL.md (probably in a "Wire Messages" chapter). Update it to show the full post-v4.2.0 byte layout — copy the table from `127-CONTEXT.md §interfaces` verbatim (adjust formatting to match surrounding PROTOCOL.md table style):

    ```
    [version_len:1][version:version_len bytes]
    [uptime:8 BE uint64]
    [peer_count:4 BE uint32]
    [namespace_count:4 BE uint32]
    [total_blobs:8 BE uint64]
    [storage_used:8 BE uint64]
    [storage_max:8 BE uint64]
    [max_blob_data_bytes:8 BE uint64]              (advertises Config::blob_max_bytes)
    [max_frame_bytes:4 BE uint32]                  (advertises MAX_FRAME_SIZE)
    [rate_limit_bytes_per_sec:8 BE uint64]         (advertises Config::rate_limit_bytes_per_sec)
    [max_subscriptions_per_connection:4 BE uint32] (advertises Config::max_subscriptions_per_connection)
    [types_count:1 uint8]
    [supported_types:types_count bytes]
    ```

    Cite the consumer: "peers snapshot `max_blob_data_bytes` into `PeerInfo::advertised_blob_cap` at handshake (see Sync Cap Divergence below); `cdb` caches the value for the session (chunk-size auto-tuning)."

    **DOCS-04 (Sync Cap Divergence subsection):**
    Add a new subsection under the sync protocol chapter titled "Sync Cap Divergence" (3-5 paragraphs per CONTEXT.md D-06):

    1. Problem statement: peers may run with different `blob_max_bytes` caps. Without coordination, a larger-cap peer would offer blobs the smaller-cap peer cannot accept.
    2. Mechanism: each peer snapshots the remote's `max_blob_data_bytes` from `NodeInfoResponse` at handshake, storing it in `PeerInfo::advertised_blob_cap`. The snapshot is session-constant (no mid-session renegotiation); reconnect picks up new caps.
    3. Filter sites (all sync-out paths):
       - PULL set-reconciliation announce — blobs exceeding peer cap are omitted from the fingerprint set.
       - PUSH `BlobNotify` fan-out — blobs exceeding peer cap are not announced.
       - `BlobFetch` response — requests for oversized blobs return not-available.
    4. Operator visibility: `chromatindb_sync_skipped_oversized_total{peer="<address>"}` counter increments once per skip. Scrape `/metrics` to see partial-replication.
    5. Cap unknown (pre-v4.2.0 peer, `advertised_blob_cap == 0`): filter does NOT skip — conservative fallback preserves replication; the receiving peer's Phase 128 ingest still enforces its own cap.

    Cross-reference: "the CLI-side consumer (cdb auto-tunes chunk size from the advertised cap on connect) is documented in cli/README.md §Chunking."
  </action>
  <verify>
    <automated>
      # Stale citations gone
      grep -c 'MAX_BLOB_DATA_SIZE = 500\|500 MiB.*blob\|110 MiB.*frame\|MAX_FRAME_SIZE = 110' db/PROTOCOL.md
      # ^ must be 0
      # New facts present
      grep -c 'MAX_FRAME_SIZE.*2 MiB\|2 \* 1024 \* 1024\|2097152' db/PROTOCOL.md
      # ^ >= 1
      grep -c 'MAX_BLOB_DATA_HARD_CEILING\|blob_max_bytes.*4 MiB\|advertised_blob_cap\|max_blob_data_bytes' db/PROTOCOL.md
      # ^ >= 3 (across DOCS-02, DOCS-03, DOCS-04 coverage)
      grep -c 'Sync Cap Divergence\|chromatindb_sync_skipped_oversized_total' db/PROTOCOL.md
      # ^ >= 2
    </automated>
  </verify>
  <acceptance_criteria>
    - Frame section cites 2 MiB, not 110 MiB.
    - Blob section cites `Config::blob_max_bytes` default 4 MiB with [1 MiB, 64 MiB] bounds; ceiling `MAX_BLOB_DATA_HARD_CEILING = 64 MiB`; no surviving 500 MiB claim.
    - NodeInfoResponse section shows all 4 new fields in a byte-layout table.
    - "Sync Cap Divergence" subsection exists with the 5-point content.
  </acceptance_criteria>
  <done>
    DOCS-02, DOCS-03, DOCS-04 closed. PROTOCOL.md reflects v4.2.0 wire + sync surface.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 3: DOCS-05/06 — README config table + Prometheus section</name>
  <files>README.md, db/README.md</files>
  <read_first>
    - README.md (full — find config tables + Prometheus/metrics section)
    - db/README.md (full — same scan)
    - db/peer/metrics_collector.cpp (authoritative list of 24 config gauges + HELP text)
    - db/config/config.h (blob_max_bytes field with its comment)
  </read_first>
  <action>
    Determine which README (`README.md` or `db/README.md`) is the operator-facing one. Typically `db/README.md` is the node README with config docs; root `README.md` is the project overview. Grep for "Config" table or "Prometheus" section — whichever has them is the target.

    **DOCS-05 (config field table):**
    Add a row to the config table for `blob_max_bytes`:
    ```
    | blob_max_bytes | uint64 | 4194304 (4 MiB) | Max size of blob data payload, in bytes. Bounds: [1048576, 67108864] (1–64 MiB). SIGHUP-reloadable. |
    ```
    Match the existing table's column order and formatting.

    **DOCS-06 (Prometheus section):**
    Add or extend a subsection covering:

    1. **`chromatindb_config_*` gauge family.** One gauge per numeric Config field (24 gauges total). Naming is 1:1 with struct field names (e.g., `chromatindb_config_blob_max_bytes`, `chromatindb_config_max_peers`). Gauges reflect live values — a SIGHUP reload is followed by scrapes that return updated values without node restart. List 3-5 representative gauge names as examples; don't enumerate all 24.

    2. **`chromatindb_sync_skipped_oversized_total{peer="..."}`.** Labeled counter; increments once per blob a sync-out path skipped because the destination peer's advertised `max_blob_data_bytes` is smaller than the blob size. Scrape this to see partial-replication situations across cap-divergent peers (see PROTOCOL.md §Sync Cap Divergence).

    If `db/README.md` is the operator README, do both edits there. Leave root `README.md` alone unless it duplicates config/metrics content.
  </action>
  <verify>
    <automated>
      # config row added
      grep -c 'blob_max_bytes.*4194304\|blob_max_bytes.*4 MiB' db/README.md README.md 2>/dev/null
      # ^ >= 1 total (whichever file is the right target)
      grep -c 'chromatindb_config_\|chromatindb_sync_skipped_oversized_total' db/README.md README.md 2>/dev/null
      # ^ >= 2 (at least one mention of each gauge family)
    </automated>
  </verify>
  <acceptance_criteria>
    - Config table has a `blob_max_bytes` row with default + bounds + SIGHUP note.
    - Prometheus section documents both the config gauge family AND the sync-skip labeled counter.
  </acceptance_criteria>
  <done>
    DOCS-05 + DOCS-06 closed.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 4: DOCS-07 — cli/README.md chunking section rewrite</name>
  <files>cli/README.md</files>
  <read_first>
    - cli/README.md (full — find chunking / large-file section)
    - .planning/phases/130-cli-auto-tuning/130-01-SUMMARY.md (CLI behavior authoritative)
    - cli/src/connection.cpp (session_blob_cap_ seeding path)
    - cli/src/chunked.cpp (threshold + chunk-size derivation)
  </read_first>
  <action>
    Find the chunking section in `cli/README.md` (look for "chunk", "CDAT", "CPAR", or "large file"). Rewrite to state:

    1. `cdb` reads the server's advertised `max_blob_data_bytes` from `NodeInfoResponse` once per connect and caches it for the session.
    2. There is no `--chunk-size` flag. Chunking is fully auto-tuned: files ≤ server cap upload as a single blob; files > server cap get split into CDAT chunks + a CPAR manifest, with `chunk_size = server_cap`.
    3. MAX_CHUNKS = 65536 bounds the maximum chunked file: 256 GiB at the 4 MiB default cap, 4 TiB at the 64 MiB hard ceiling.
    4. If the server is pre-v4.2.0 (NodeInfoResponse omits `max_blob_data_bytes`), `cdb` refuses to connect rather than silently defaulting — operator must upgrade the node.

    Delete any surviving mention of `CHUNK_SIZE_BYTES_DEFAULT = 16 MiB`, `CHUNK_SIZE_BYTES_MAX = 256 MiB`, `CHUNK_THRESHOLD_BYTES = 400 MiB`, or `--chunk-size` flag.

    Cross-reference PROTOCOL.md §NodeInfoResponse and §Sync Cap Divergence.
  </action>
  <verify>
    <automated>
      grep -c 'auto-tune\|session cap\|max_blob_data_bytes' cli/README.md
      # ^ >= 1
      grep -c 'CHUNK_SIZE_BYTES_DEFAULT\|CHUNK_SIZE_BYTES_MAX\|CHUNK_THRESHOLD_BYTES\|--chunk-size' cli/README.md
      # ^ must be 0
    </automated>
  </verify>
  <acceptance_criteria>
    - Chunking section describes auto-tune from server cap.
    - All 3 deleted constants + the --chunk-size flag are absent.
    - Hard-fail behavior for pre-v4.2.0 node documented.
  </acceptance_criteria>
  <done>
    DOCS-07 closed.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 5: DOCS-08 — ARCHITECTURE.md MAX_BLOB_DATA_SIZE row</name>
  <files>db/ARCHITECTURE.md</files>
  <read_first>
    - db/ARCHITECTURE.md (full — find Step-0 validation table and the MAX_BLOB_DATA_SIZE row)
  </read_first>
  <action>
    Find the Step-0 (or equivalent "ingest pre-validation") table in ARCHITECTURE.md that lists `MAX_BLOB_DATA_SIZE` as a bound. Update that row:

    Before (likely): `MAX_BLOB_DATA_SIZE | 500 MiB | Hard protocol ceiling | ...`
    After: `Config::blob_max_bytes | 1 MiB – 64 MiB (default 4 MiB) | Operator-tunable via config.json, SIGHUP-reloadable; hard protocol ceiling MAX_BLOB_DATA_HARD_CEILING = 64 MiB | ...`

    Match the existing table's formatting and column count. Do not restructure the surrounding content.
  </action>
  <verify>
    <automated>
      grep -c 'MAX_BLOB_DATA_SIZE.*500 MiB' db/ARCHITECTURE.md
      # ^ must be 0 (stale claim gone)
      grep -c 'blob_max_bytes\|MAX_BLOB_DATA_HARD_CEILING' db/ARCHITECTURE.md
      # ^ >= 1
    </automated>
  </verify>
  <acceptance_criteria>
    - Step-0 table row reflects configurable cap with default + bounds + hard ceiling.
    - No surviving `MAX_BLOB_DATA_SIZE = 500 MiB` claim.
  </acceptance_criteria>
  <done>
    DOCS-08 closed.
  </done>
</task>

</tasks>

<verification>
- Docs-only phase; no code compile / no test run required.
- Gate: zero stale references across the 5 updated shipping docs (PROTOCOL.md, db/README.md and/or README.md, cli/README.md, ARCHITECTURE.md) to `MAX_BLOB_DATA_SIZE = 500 MiB`, `MAX_FRAME_SIZE = 110 MiB`, `CHUNK_SIZE_BYTES_*`, `CHUNK_THRESHOLD_BYTES`, or `--chunk-size` flag.
- Every cited constant's value matches live code at HEAD.
</verification>
