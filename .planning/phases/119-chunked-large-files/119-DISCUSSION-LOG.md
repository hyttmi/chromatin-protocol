# Phase 119: Chunked Large Files - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in 119-CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-19
**Phase:** 119-chunked-large-files
**Areas discussed:** Chunk size + threshold, CPAR manifest format + truncation prevention, Client UX (auto-detect vs explicit), Deletion cascade (client vs node-side)

---

## Chunk size + threshold

### Chunk size

| Option | Description | Selected |
|--------|-------------|----------|
| 16 MiB (Recommended) | 1 GiB file = 64 chunks = ~2 KiB manifest; per-chunk overhead <0.01%; 8-in-flight fits in ~128 MiB RAM | ✓ |
| 1 MiB | Matches STREAMING_THRESHOLD; 1 GiB = 1024 chunks; huge manifest | |
| 64 MiB | Fewer chunks; 8 in flight = up to 512 MiB RAM | |
| User-tunable via config | `chunk_size_bytes` key; YAGNI until someone needs it | |

**User's choice:** 16 MiB (recommended)

### Chunk threshold

| Option | Description | Selected |
|--------|-------------|----------|
| Always chunk ≥ 400 MiB (Recommended) | Safely under 500 MiB hard cap with envelope headroom; no flag | ✓ |
| Always chunk ≥ 64 MiB | Earlier chunking; more noise for mid-size files | |
| Flag-driven: --chunked / --no-chunked | User controls; more knobs | |
| Always chunk regardless of size | One code path; wasteful for small files | |

**User's choice:** Always chunk ≥ 400 MiB (recommended)

---

## CPAR manifest format + truncation prevention

### Manifest format

| Option | Description | Selected |
|--------|-------------|----------|
| FlatBuffers schema (Recommended) | New `Manifest` table alongside `Blob`; auto-codec; forward-compat | ✓ |
| Plain binary TLV | Hand-rolled; smaller; no schema evolution story | |
| JSON-in-blob | Human-readable; bigger; clashes with FlatBuffers-everywhere stance | |

**User's choice:** FlatBuffers schema (recommended)

### Integrity mechanism

| Option | Description | Selected |
|--------|-------------|----------|
| segment_count + plaintext_sha3 (Recommended) | CHUNK-05 minimum + full plaintext hash; catches truncation + substitution | ✓ |
| Just segment_count | Bare CHUNK-05; detects truncation, not per-chunk substitution | |
| Merkle root of chunk hashes | Over-engineered; manifest is already signed and holds hashes | |

**User's choice:** segment_count + plaintext_sha3 (recommended)

---

## Client UX (auto-detect vs explicit)

### put behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Auto-detect from file size (Recommended) | stat the file; ≥ 400 MiB → chunked; no flag | ✓ |
| Explicit --chunked flag required | User opts in; errors if ≥ 500 MiB without flag | |
| Auto-detect + --no-chunked escape hatch | Auto by default; flag to force single-blob | |

**User's choice:** Auto-detect (recommended)

### get behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Transparent reassembly (Recommended) | Detect CPAR prefix; pipeline chunk reads; write via pwrite | ✓ |
| Require --assemble flag | Without flag, saves raw manifest; surprising | |

**User's choice:** Transparent reassembly (recommended)

### Progress reporting

| Option | Description | Selected |
|--------|-------------|----------|
| One `chunk N/total saved` line per chunk (Recommended) | Matches Phase 120 style; --quiet suppresses; arrival order | ✓ |
| Progress bar (percentage / chunk count) | Needs TTY detection, CR handling | |
| Silent until done, then one summary line | A 20-min upload with no feedback feels broken | |

**User's choice:** Per-chunk line (recommended)

---

## Deletion cascade (client vs node-side)

### Top-level principle

| Option | Description | Selected |
|--------|-------------|----------|
| Client-side (Recommended) | CLI walks manifest, tombstones chunks, simple, no node changes | ✓ |
| Node-side automatic cascade | Node would need to decrypt manifest — privacy leak or weakens encryption | |
| Client-side tombstone-first | Manifest gone first, chunks after; orphan risk | |

**User's choice:** Client-side — with explicit note:
> "this is a bit of a concern here. However, the node itself is generally a dumb database so technically developer who writes cli or whatever communicating with the database should handle this by itself"

### Tombstone order

| Option | Description | Selected |
|--------|-------------|----------|
| Chunks first, then manifest last (Recommended) | Idempotent retry — manifest exists, retry finishes cleanup | ✓ |
| Manifest first, then chunks | Creates true orphans if cdb dies mid-rm | |

**User's choice:** Chunks first, manifest last (recommended)

### Orphan policy

| Option | Description | Selected |
|--------|-------------|----------|
| Accept: retry rm is idempotent (Recommended) | Re-running rm on the same manifest completes cleanup | ✓ |
| Add `cdb gc` subcommand later | Defer; build only if orphans become real problem | |
| Build `cdb gc` now as part of Phase 119 | Overbuild until someone hits the problem | |

**User's choice:** Accept + retry (recommended)

---

## Claude's Discretion

- Exact FlatBuffers schema layout for the `Manifest` table (field ordering, nested `ChunkRef` vs flat array, explicit `version` field).
- CPAR 4-byte magic value ("CPAR" ASCII assumed unless a conflict turns up).
- Per-chunk retry policy on transient failures during upload/download.
- Where the new schema file lives (`cli/schemas/` vs shared `schemas/`).

## Deferred Ideas

- `cdb gc` subcommand — build only if orphans become operational.
- Resume support for partial uploads/downloads.
- Parallel chunk signing.
- Reshare on chunked blobs.
- `cdb put --chunk-size` flag.
- Progress-bar UI.
- Node-side cascade delete (explicitly rejected on privacy grounds).
