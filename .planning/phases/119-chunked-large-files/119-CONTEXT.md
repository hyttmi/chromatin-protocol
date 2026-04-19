# Phase 119: Chunked Large Files - Context

**Gathered:** 2026-04-19
**Status:** Ready for planning

<domain>
## Phase Boundary

Enable uploading and downloading files larger than the 500 MiB per-blob cap by
splitting them client-side into many CDAT chunk blobs plus a CPAR manifest blob
that lists the chunks in order. The node remains a dumb content-addressed
store: it knows how to accept and serve any signed blob regardless of whether
it's a CDAT chunk, a CPAR manifest, or a regular CENV blob. All
chunking/reassembly/deletion-cascade logic lives in the CLI.

Two concrete capabilities delivered:
1. `cdb put <largefile>` transparently handles files ≥ 400 MiB by producing N
   CDAT chunks + one CPAR manifest, all pipelined over a single connection.
2. `cdb get <manifest_hash>` transparently reassembles when it detects the
   CPAR prefix, pipelining chunk fetches and writing to disk in arrival order.

**Out of scope:**
- Node-side cascade deletion (dumb-DB principle — node doesn't decrypt or
  interpret manifest contents to tombstone chunks).
- `cdb gc` garbage-collection subcommand for orphaned chunks (tracked as
  deferred; add only if real-world orphans show up).
- Resume support for partial uploads/downloads (deferred; retry-from-scratch
  is fine for Phase 119).
- Per-chunk parallel signing on multiple threads (single-threaded signing
  fits inside the 8-in-flight pipelining window).
- New progress-bar UI (per-chunk line is sufficient).

</domain>

<decisions>
## Implementation Decisions

### Chunk sizing and threshold

- **D-01:** CDAT chunk size = **16 MiB** of *plaintext* per chunk. Sweet spot:
  a 1 GiB file produces 64 chunks (~2 KiB manifest of hashes), per-chunk
  overhead (Blob header + ML-DSA-87 signature + AEAD tag) stays well under
  0.01%, and 8-in-flight pipelining fits in ~128 MiB working memory.
- **D-02:** Auto-chunk threshold = file **≥ 400 MiB** triggers CDAT+CPAR path.
  Sits safely under the 500 MiB hard cap with envelope overhead room. Smaller
  files stay on the existing single-blob path unchanged. No flag — the
  decision is made by stat'ing the input file.

### CPAR manifest format

- **D-03:** CPAR is a **new FlatBuffers table** `Manifest` defined in
  `cli/schemas/` (or wherever the Blob schema lives), consistent with the
  existing Blob wire format. Minimum fields:
  - `chunk_size_bytes : uint32` — plaintext bytes per chunk (D-01 value;
    carried in the manifest for forward-compat, not hard-coded at read time)
  - `segment_count : uint32` — number of CDAT chunks (CHUNK-05)
  - `total_plaintext_bytes : uint64` — full original file size
  - `plaintext_sha3 : [uint8:32]` — SHA3-256 of the full reassembled plaintext
    (integrity check at reassembly; CHUNK-05 hardening beyond bare count)
  - `chunk_hashes : [[uint8:32]]` — ordered list of CDAT blob hashes
- **D-04:** Integrity guarantees:
  1. **Truncation:** `segment_count` binds the expected chunk count; manifest
     is ML-DSA-87 signed by the owner so attacker can't silently drop entries.
  2. **Per-chunk authenticity:** each CDAT chunk is a signed Blob — existing
     signature check catches substitution.
  3. **Whole-file integrity:** after reassembly, CLI computes SHA3-256 over
     the concatenated plaintext and compares to `plaintext_sha3` in the
     manifest. Mismatch = abort + delete partial output file. Closes the gap
     where an attacker-controlled node could serve a valid-but-different
     chunk for the same rid (unlikely given per-chunk signature, but
     defense-in-depth and a real "envelope v2" upgrade).

### Client UX

- **D-05:** `cdb put` auto-detects. File size is checked by `stat(2)` before
  any transfer begins. Files < 400 MiB take the single-blob code path
  untouched. Files ≥ 400 MiB fan out to CDAT+CPAR. No new flags on `cdb put`.
- **D-06:** `cdb get <hash>` auto-reassembles. After decrypting the blob's
  payload, the CLI checks the 4-byte type prefix. If CPAR, it parses the
  FlatBuffers manifest, pipelines ReadRequests for all `chunk_hashes`, and
  writes them to the output file at `chunk_index * chunk_size_bytes` offset
  as they arrive (pwrite). If CENV (or any non-CPAR type), today's code path
  runs unchanged. No `--assemble` flag.
- **D-07:** Progress = one `chunk N/total saved` line to stderr per completed
  chunk, arrival order (not request order — matches Phase 120 D-08 convention).
  `--quiet` suppresses the per-chunk lines. No progress bar (no curses
  dependency, no TTY detection complexity). Final line on success:
  `saved: <filename> (<total_size>, <chunks> chunks)`.

### Deletion cascade

- **D-08:** Client-side cascade only. `cdb rm <hash>` fetches the blob,
  checks its type prefix; if CPAR, it parses the chunk list and tombstones
  each chunk before tombstoning the manifest itself. The node has no
  awareness that chunks belong to a manifest — it just sees N+1 tombstone
  writes from the owner's identity. Matches the "node = dumb content store"
  ethos.
- **D-09:** Delete order = **chunks first, manifest last**. If cdb crashes
  mid-rm, the manifest still exists, so re-running `cdb rm <manifest_hash>`
  finishes the cleanup idempotently (tombstoning an already-tombstoned chunk
  is a no-op on the node side). No data loss, no unreachable orphans —
  just a partial cleanup that completes on retry.
- **D-10:** Orphan handling is **accept + retry**. No `cdb gc` subcommand
  this phase. If the pattern becomes a real problem in practice, add a GC
  tool as its own phase (tracked in Deferred Ideas below).

### Carried forward from Phase 120 (pipelining)

- Pipelining depth = 8 (PIPE-03). Chunk upload uses `Connection::send_async`
  + `conn.recv()` arrival-order drain with per-batch `rid → chunk_index` map,
  same shape as `cmd::put` / `cmd::get`.
- Single-sender, single-reader invariant is preserved: chunking adds no new
  thread, strand, or coroutine — still synchronous pump.
- Arrival order ≠ chunk index — download writes with `pwrite` at
  `chunk_index * chunk_size_bytes` to accommodate out-of-order arrivals.

### Streaming from disk (CHUNK-01 no-full-memory-buffering)

- **D-11:** Upload reads from disk incrementally. Reference approach:
  `fread()` a 16 MiB plaintext chunk from the input file, envelope-encrypt it
  into a CDAT Blob, hand to `send_async`. Working memory = 8 × 16 MiB ≈ 128
  MiB for in-flight buffers + the current read buffer. No `mmap`, no whole-
  file read. Planner can refine if a memory-arena pattern fits better.
- **D-12:** Download writes to disk incrementally. Each CDAT chunk, once
  decrypted, is `pwrite`d at its correct offset. Output file is `open(O_WRONLY|
  O_CREAT)` up front and truncated to `total_plaintext_bytes` after all
  chunks land (or left sparse if the filesystem supports it). On failure
  mid-download, the partial file is `unlink`'d — don't leave half-files.

### Claude's Discretion

- Exact FlatBuffers schema layout for the new `Manifest` table (field
  ordering, whether to use a nested `ChunkRef` table vs a flat `[[uint8:32]]`
  array, whether to add an explicit `version` field for future evolution).
  Planner picks the ergonomic choice; the REQUIRED semantic fields are in
  D-03.
- CPAR 4-byte magic value. "CPAR" as ASCII is the obvious choice and already
  implied by the phase name; locked unless planner finds a conflict.
- Retry policy on a single-chunk transient failure during upload/download
  (today there's none — one bad chunk fails the whole batch). Planner can
  add a small retry budget per chunk if it's a natural fit.
- Atomicity on a mid-download failure: whether to `unlink` the partial file
  immediately or leave it with a `.partial` suffix. D-12 says unlink; any
  alternate pattern needs a clear justification.
- Where the `Manifest` FlatBuffers schema lives (cli/schemas/ vs a shared
  schemas/ dir) — picker's choice, follow existing convention for Blob.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Requirements & roadmap
- `.planning/REQUIREMENTS.md` — CHUNK-01 through CHUNK-05 (the acceptance
  criteria this phase must satisfy).
- `.planning/ROADMAP.md` — Phase 119 row and success criteria.

### Prior-phase contracts to preserve
- `.planning/phases/120-request-pipelining/120-CONTEXT.md` — D-01..D-09 of
  pipelining (depth, single-sender/reader, arrival-order drain, batch-local
  rid map). Phase 119's chunk fan-out drops directly onto these primitives.
- `.planning/phases/120-request-pipelining/120-01-SUMMARY.md` — Connection
  surface (`send_async`, `recv_for`, `kPipelineDepth`, `pending_replies_`).
- `.planning/phases/120-request-pipelining/120-02-SUMMARY.md` — the
  `cmd::put` / `cmd::get` two-phase pipelined pattern that chunk upload/
  download will reuse.
- `.planning/phases/117-blob-type-indexing-ls-filtering/117-CONTEXT.md` —
  4-byte blob_type convention (CENV, PUBK, TOMB, DLGT, CDAT); the new CPAR
  type slots into the same mechanism.

### Existing code this phase extends
- `cli/src/connection.h`, `cli/src/connection.cpp` — `send_async` / `recv`
  transport, `send_chunked` (transport-level chunking at 1 MiB sub-frames,
  NOT the same thing as CDAT+CPAR blob-level chunking introduced here).
- `cli/src/envelope.cpp` — existing per-blob AEAD envelope with segment_count
  for intra-blob AEAD segments; this phase adds a DIFFERENT segment concept
  (inter-blob manifest segments). Name disambiguation matters — don't
  conflate "envelope segment" (AEAD) with "manifest segment" (chunk).
- `cli/src/wire.h` — `CDAT_MAGIC`, `extract_blob_type`,
  `is_infrastructure_type` (add `CPAR_MAGIC`, include CPAR in
  infrastructure list).
- `cli/src/commands.cpp` — `cmd::put`, `cmd::get`, `cmd::rm` are the three
  that get the chunked code path.
- `db/schemas/` — FlatBuffers schemas for existing Blob type; CPAR
  `Manifest` table lands in the same style.
- `db/storage/storage.cpp` — node storage already indexes blob_type; CPAR
  requires no server-side changes (just flows through the existing ingest).

### Threat model precedents
- `.planning/phases/120-request-pipelining/120-01-PLAN.md` §threat_model —
  T-120-01..T-120-07 cover the pipelining surface. Phase 119 inherits all
  of them unchanged and adds T-119-01..N for the chunking surface (partial
  upload leaves orphaned CDAT, mid-download tampering, manifest
  substitution, etc. — planner's threat_model expands on these).

### Project rules
- `CLAUDE.md` / project memory — no copy-paste of utilities (helpers go into
  shared headers), `-j$(nproc)` for builds, no `|| true` to suppress errors,
  no backward-compat scaffolding (protocol changes are fine since the node
  is actively developed).

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable assets

- **`Connection::send_async` + `recv` pump** (Phase 120) — the
  chunk upload/download both drop onto these primitives directly; no new
  transport layer needed.
- **`encode_blob` / `decode_blob`** — each CDAT chunk is a regular signed
  Blob, so the existing encoder/decoder handles the chunk wire format. CPAR
  manifest is a NEW blob type but also a signed Blob with a new payload
  schema.
- **`extract_blob_type`** in `cli/src/wire.h` — already classifies CENV /
  PUBK / TOMB / DLGT / CDAT. Extend with CPAR.
- **`envelope::encrypt` / `envelope::decrypt`** — per-chunk envelope
  encryption uses the existing recipient-set mechanism unchanged. Each CDAT
  chunk has its own recipients (same set as the parent manifest would have).
- **`parse_hex` / `to_hex`** — used by the manifest's chunk-hash list.
- **FlatBuffers code generation** — `Manifest` table compiles alongside
  `Blob` via the existing `flatc` invocation in CMake.

### Established patterns

- **4-byte blob type magic** (`CENV`, `PUBK`, `TOMB`, `DLGT`, `CDAT`) —
  CPAR fits this pattern directly. Must update `extract_blob_type_name` and
  `is_infrastructure_type` in `cli/src/wire.h`, plus the `--type` filter
  acceptance in `cli/src/main.cpp`.
- **Per-batch rid → index map + greedy-fill up to `kPipelineDepth`** (Phase
  120-02 two-phase shape) — exact pattern reuses for chunk upload and
  chunk download.
- **`++errors; continue;` per-item error path** — chunk errors should follow
  the same pattern: one bad chunk doesn't sink the batch, but the whole
  reassembly DOES sink if any chunk is unrecoverable (no partial file).
- **`opts.quiet` gating** — per-chunk progress lines must honor it, same as
  existing `saved:` lines.

### Integration points

- `cmd::put` in `cli/src/commands.cpp` — add a size-check branch at the top
  of the per-file loop. Files < 400 MiB → existing code. Files ≥ 400 MiB →
  new `put_chunked` helper.
- `cmd::get` — after decrypting the response, check type prefix. If CPAR,
  hand to new `get_chunked` helper that walks the manifest.
- `cmd::rm` — after fetching the blob to delete, check type. If CPAR, hand
  to new `rm_chunked` helper that cascades.
- `cli/src/wire.h` — add `CPAR_MAGIC`, update `extract_blob_type_name`,
  `is_infrastructure_type`.
- `cli/src/main.cpp` — extend `--type` acceptance to include CPAR.

</code_context>

<specifics>
## Specific Ideas

- **Manifest is itself a signed Blob.** Don't invent a separate manifest
  wire format — it's a Blob whose `data` field is a FlatBuffers-encoded
  `Manifest` table, whose first 4 bytes are the CPAR magic. ML-DSA-87
  signature over the same fields as any other blob (namespace || data ||
  ttl || timestamp). Node never inspects manifest contents; it just ingests.
- **Chunks live in the same namespace as the manifest.** The manifest owner
  also owns the chunks. No cross-namespace reference.
- **Chunks are encrypted.** Each CDAT chunk is envelope-encrypted to the same
  recipient set as the parent manifest. Recipients for a chunked upload is
  decided ONCE at `cdb put` time and applied uniformly to every CDAT + the
  manifest. Reshare on a manifest is future work (deferred).
- **Per-chunk error = per-batch error.** Within the chunked upload/download,
  "one bad chunk fails the whole file" is the right default. The per-item
  resilience pattern from Phase 120-02 applies at the BATCH level (one file
  fails, others in the same `cdb put a b c` still succeed), not at the
  CHUNK level. A file either arrives whole or not at all.
- **The 400 MiB threshold is checked at the CLI.** The node will happily
  ingest a 499 MiB single blob if someone hand-builds one, because the node
  cap is 500 MiB. The 400 MiB CLI threshold gives envelope-overhead headroom
  so the CLI never accidentally hits a node rejection because the encrypted
  blob went over 500 MiB.

</specifics>

<deferred>
## Deferred Ideas

- **`cdb gc` subcommand** to find and tombstone CDAT chunks not referenced
  by any live CPAR manifest in the owner's namespace. Only build this when
  orphaned chunks become a real operational problem.
- **Resume support for partial uploads/downloads** — retry picks up from the
  first missing chunk instead of starting over. Meaningful for truly huge
  files (10s of GiB); not needed for the initial ship. Would require
  storing a small client-side progress file or querying the node for which
  chunks of the manifest are already present.
- **Parallel chunk signing on a worker pool** — today all chunk signatures
  run on the caller thread. For 10+ GiB files, signing could become a
  bottleneck. Not profiled yet; defer until someone shows it matters.
- **Re-share on a chunked blob** — today `cdb reshare` operates on a single
  blob. For a manifest, re-share needs to walk the chunk list and re-
  envelope each one with the new recipient set, then rewrite the manifest.
  Out of scope for Phase 119.
- **`cdb put --chunk-size N`** flag for operator-tunable chunk size — YAGNI
  for the first ship. Add only when a concrete use case appears.
- **Progress bar UI** for TTY sessions — per-chunk line is enough for now.
- **Node-side cascade delete** — would require the node to either decrypt
  the manifest (privacy leak) or accept unencrypted chunk-hash lists in
  CPAR blobs (weakens the encryption story). Decided NO; client cascade is
  the right layer.

</deferred>

---

*Phase: 119-chunked-large-files*
*Context gathered: 2026-04-19*
