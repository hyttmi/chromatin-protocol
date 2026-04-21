# Phase 125: MVP Documentation Update — Research

**Researched:** 2026-04-21
**Domain:** Documentation rewrite + source-tree code-comment hygiene
**Confidence:** HIGH (all claims grounded in the live repo; no library lookups needed)

## Summary

Phase 125 is a pure content-and-hygiene phase: four public `.md` files must be
rewritten (one created from scratch) to match the post-phase-124 shipping state,
and the repo's inline comments must be scrubbed of stale field references and
historical phase-number breadcrumbs. No behavior changes, no new wire types, no
new binaries.

The authoritative content for the rewrite is already captured in-tree: the
post-122 schemas (`db/schemas/*.fbs`), the post-123 magics + ingest invariants
(`db/wire/codec.h`, `db/engine/engine.cpp`), the post-124 CLI helpers
(`cli/src/wire.h`, `cli/src/commands.cpp`, `cli/src/error_decoder.cpp`), and the
phase-artifact corpus under `.planning/phases/116..124/`. The risk surface is
(a) content drift between the four files (mitigated by the zero-duplication
rule D-02), (b) missing a phase-122/123/124 feature in the coverage matrix, and
(c) accidentally deleting code during the D-14 comment-hygiene pass.

**Primary recommendation:** Break Phase 125 into 5 plans aligned to the natural
scope boundaries: (1) `PROTOCOL.md` rewrite (largest, highest-traffic,
byte-level); (2) `README.md` + `cli/README.md` rewrites (user-facing, parallel-
safe with 1); (3) new `db/ARCHITECTURE.md` + `db/README.md` delta (internal
implementation doc, depends on 1 for cross-link anchors); (4) SC#6 pre-122
vestige cleanup — schema, tests, `main.cpp:619` leak, `message_dispatcher.cpp`
memo; (5) D-13 + D-14 comment-hygiene sweep (last — runs after the schema
cleanup so the comment pass isn't stepping on stale code). Plans 1-3 touch
only `.md`; plans 4-5 touch `.cpp`/`.h`/`.fbs` + tests.

## User Constraints (from CONTEXT.md)

### Locked Decisions

#### File structure and audience split
- **D-01:** Keep 4 distinct files with sharpened audience boundaries:
  - `README.md` — project pitch + quickstart (~1 screen)
  - `db/PROTOCOL.md` — byte-level wire format for external client implementers
  - `db/ARCHITECTURE.md` (NEW) — internal implementation: DBIs, strand model,
    sync engine, ingest pipeline
  - `cli/README.md` — user-facing `cdb` command reference
  - `db/README.md` — stays as a thin build + operator quickstart (not a content
    duplication of ARCHITECTURE; just the "how to run it" angle)
- **D-02:** Zero content duplication between the four files. Each file owns its
  audience's detail; other docs **cross-link** by section anchor rather than
  restate.
- **D-03:** Add a minimal 5-command "hello world" quickstart to `cli/README.md`
  (keygen → publish → put → ls → get, ~15 lines).

#### db/ARCHITECTURE.md structure (new file)
- **D-04:** Top-down skeleton: `Storage → Engine → Net`. Data layer first (8
  DBIs, libmdbx model, ACID + strand-confinement), then `BlobEngine` (ingest
  pipeline, validation, PUBK-first, signer_hint checks, BOMB cascade), then
  network stack (handshake, sync protocol, PEX).
- **D-05:** Phase 121's single-storage-thread strand model gets a dedicated
  subsection (~40 lines) with an ASCII threading diagram showing the writer
  thread, async funnel from I/O coroutines, `co_await` as an iterator-
  invalidation point, and `STORAGE_THREAD_CHECK` discipline. Reference the TSAN
  ship-gate evidence from Phase 121 `VERIFICATION.md`.
- **D-06:** ASCII diagrams only (no Mermaid).
- **D-07:** Cross-references to `db/PROTOCOL.md` link by section anchor — never
  duplicate wire-level content.

#### v4.1.0 feature coverage depth
- **D-08:** Tiered depth per feature.
  - **Deep sections:** chunked CDAT/CPAR (Phase 119), NAME + BOMB (Phase 123),
    `signer_hint` + PUBK-first (Phase 122), auto-PUBK + D-05 error decoder +
    D-06 BOMB cascade (Phase 124).
  - **Brief sections (1-3 paragraphs):** blob type indexing / `ls --type` (Phase 117),
    configurable constants + peer management (Phase 118), request pipelining
    (Phase 120).
- **D-09:** Full error-code table in `db/PROTOCOL.md` covering `ErrorResponse`
  codes `0x07–0x0B`. Source: verified output from the `[error_decoder]`
  TEST_CASE in `cli/tests/test_wire.cpp`.

#### SC#6 inline-comment sweep + code hygiene pass
- **D-10:** Sweep the whole repo. Remove inline comments referring to REMOVED
  pre-122 artifacts: `BlobData.namespace_id`, `BlobData.pubkey` (embedded-per-
  blob field), `MsgType::Data = 8`, the old signing-input shape. One commit
  per area (cli, db, tests) for reviewable history.
- **D-11:** Delete `TransportMsgType_Data = 8` from `db/schemas/transport.fbs`,
  regenerate `transport_generated.h`, update/remove remaining test-only
  references (`test_protocol.cpp`, `test_framing.cpp`, `test_connection.cpp`).
  Remove the `// TransportMsgType_Data direct-write branch was DELETED` memo
  at `db/peer/message_dispatcher.cpp:1388`.
- **D-12:** Fix the pre-existing `"(Phase 123)"` help-text leak at
  `cli/src/main.cpp:619`.
- **D-13:** **Scope expansion** — strip `// Phase N` historical breadcrumbs from
  source code everywhere. Planner SHOULD size this as its own plan.
- **D-14:** **Scope expansion** — code-comment hygiene pass removing comments
  that explain obvious code. Per CLAUDE.md policy (see below). Default: also
  delete commented-out code blocks.

#### Scope boundary
- **D-15:** No new diagrams beyond ASCII. No migration guide (out of scope).
  No CHANGELOG restructuring. If a v4.1.0 feature lacks good phase-artifact
  source material, read the code directly and ask the user only if meaning is
  genuinely ambiguous.

### Claude's Discretion
- Exact doc section ordering within each file.
- Whether `db/README.md` gets touched at all or stays frozen.
- Whether the D-14 hygiene pass also prunes commented-out code blocks. Default: yes.

### Deferred Ideas (OUT OF SCOPE)
- Threat model document (STRIDE) — post-v1.0.0 phase.
- Migration guide (pre-122 → post-122) — `feedback_no_backward_compat.md`.
- Interactive protocol diagrams (Mermaid/excalidraw) — rejected in D-06.
- Public API reference for client libraries — only `cdb` exists.
- Glossary in top-level `README.md` — rejected in favor of per-doc ownership.

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DOCS-01 | PROTOCOL.md updated with blob type indexing wire format | §1 coverage matrix rows for `ListRequest flags`, `ListResponse 44-byte entry`; Phase 117 artifacts |
| DOCS-02 | PROTOCOL.md updated with new ListRequest/ListResponse format | Same as DOCS-01; already partly in current PROTOCOL.md at lines 788-812 (needs post-122 context weave) |
| DOCS-03 | README.md updated with all new node config fields and peer management | Phase 118 artifacts; existing `db/README.md` Configuration section is the baseline to extend |
| DOCS-04 | cli/README.md updated with groups, import, chunking, pipelining | Phase 116/119/120 artifacts + `cli/src/commands.h` subcommand list (verified below) |

Beyond the REQUIREMENTS.md IDs, Phase 125's ROADMAP.md Success Criteria 1-6
are the real acceptance set — they cover post-122/123/124 content that
predates REQUIREMENTS.md being locked. Both must be satisfied.

## Project Constraints (relevant directives)

The repo does NOT contain a `CLAUDE.md` file at the root (CONTEXT.md references
it as a policy source, but the policies live in the user's memory). The
binding directives inherited from user memory and `.planning/` files:

- **"Default to writing no comments."** Only add a comment when the WHY is
  non-obvious: a hidden constraint, a subtle invariant, a workaround for a
  specific bug. This is the policy the D-14 sweep enforces retroactively.
- **No backward compat** on either binary. Deleting pre-122 code/symbols is
  encouraged, not deferred.
- **No phase leaks in user-visible strings.** `cdb` help/errors never say
  "Phase 123".
- **Zero duplication.** Applies to docs too (D-02): no content is restated
  across the four files.
- **No commented-out code.** Delete on sight — this is why the D-14 pass is
  safe and encouraged to also remove comment-blocks that wrap dead code.
- **`.planning/` never ships to GitHub.** Every fact the external reader needs
  must land in one of the four public `.md` files — no deferring to a phase
  artifact.

## 1. Content-coverage matrix

Per-doc section inventory. Source-code columns are absolute paths (relative to
repo root). Depth per D-08.

### 1a. `db/PROTOCOL.md` (byte-level wire spec, external client implementers)

| Section | Source phase | Source artifacts | Source code | Depth |
|---|---|---|---|---|
| Transport framing + AEAD params | pre-122 | (unchanged) | `db/net/framing.cpp`, `db/net/connection.cpp` | keep |
| Chunked transport framing (0x01, 1 MiB sub-frames, sentinel) | 115 (pre-v4.1) | 115 artifacts | `db/net/connection.cpp:834-972` | keep |
| PQ handshake + HKDF labels | pre-122 | (unchanged) | `db/net/handshake.cpp` | keep |
| Lightweight handshake | pre-122 | (unchanged) | `db/net/handshake.cpp` | keep |
| Role signalling (PEER/CLIENT/reserved) | 999.1 | (archived in ROADMAP) | `db/net/role.h`, `db/net/connection.cpp` | keep |
| UDS transport | pre-122 | (unchanged) | `db/net/uds_acceptor.cpp` | keep |
| **Blob schema (post-122: 5 fields)** | 122 | `122-01-SUMMARY.md`, `122-03-SUMMARY.md`, `122-PATTERNS.md` | `db/schemas/blob.fbs` (5 fields: signer_hint, data, ttl, timestamp, signature) | **REWRITE — deep** |
| **Canonical signing input (post-122)** | 122 | `122-03-SUMMARY.md` | `cli/src/wire.cpp` `build_signing_input` (`SHA3-256(target_namespace \|\| data \|\| ttl_be32 \|\| timestamp_be64)`) | **REWRITE — deep** |
| **Namespace derivation (SHA3 of signing pubkey, NOT per-blob)** | 122 | `122-04-SUMMARY.md` | `db/engine/engine.cpp` ingest path, `cli/src/identity.cpp` | **NEW — deep** |
| **signer_hint semantics (32 bytes, resolved via owner_pubkeys)** | 122 | `122-02-SUMMARY.md`, `122-PATTERNS.md` | `db/storage/storage.h:108` (owner_pubkeys DBI), `cli/src/wire.h:139-145` | **NEW — deep** |
| **PUBK-first invariant (node-enforced at ingest)** | 122 | `122-04-SUMMARY.md`, `122-CONTEXT.md` SC#4 | `db/engine/engine.cpp:175-200` (verify path), `db/tests/engine/test_pubk_first.cpp` | **NEW — deep** |
| **owner_pubkeys DBI (8th DBI)** | 122 | `122-02-SUMMARY.md`, `122-07-SUMMARY.md` | `db/storage/storage.h:108`, `db/storage/storage.cpp:1386+` | **NEW — deep** |
| **PUBK blob format (4-byte magic + signing_pk + kem_pk = 4164 bytes)** | 122/124 | `122-01-SUMMARY.md`, `124-RESEARCH.md` | `cli/src/wire.h:278-291` | **NEW — deep** |
| **BlobWrite envelope (MsgType=64, BlobWriteBody{target_namespace, Blob})** | 122 | `122-05-SUMMARY.md`, `122-PATTERNS.md` | `db/schemas/transport.fbs:83-87`, `cli/src/wire.cpp` `encode_blob_write_body` | **NEW — deep** |
| **NAME magic (0x4E414D45, mutable-name pointer)** | 123 | `123-01-SUMMARY.md`, `123-02-SUMMARY.md`, `123-RESEARCH.md` | `db/wire/codec.h:160`, `cli/src/wire.h:354`, `cli/src/wire.cpp:382+` | **NEW — deep** |
| **BOMB magic (0x424F4D42, batched tombstone; ttl=0 invariant)** | 123 | `123-03-SUMMARY.md`, `123-04-SUMMARY.md` | `db/wire/codec.h:191`, `db/engine/engine.cpp:161,311,398`, `cli/src/wire.h:357` | **NEW — deep** |
| **BOMB same-second tiebreak (D-15: ts DESC, blob_hash DESC)** | 123 | `123-CONTEXT.md` D-15, STATE.md FLAG | `db/wire/codec.cpp` (NAME resolver) — **flagged as known quirk** | brief |
| **Error codes 0x07–0x0B (full table, verbatim CLI wording)** | 124 | `124-04-SUMMARY.md`, `124-05-SUMMARY.md` | `cli/src/error_decoder.cpp`, `cli/tests/test_wire.cpp:658-716` `[error_decoder]` | **NEW — deep**; see §5 |
| TTL enforcement (saturating, tombstone ttl=0, query filters) | pre-122 hardened | (unchanged) | `db/engine/engine.cpp:140-173`, `db/wire/codec.h` | keep |
| Sync protocol (Phase A/B/C, reconciliation, per-blob ns prefix) | 122 (wire tweak) | `122-05-SUMMARY.md`, `122-PATTERNS.md` Pitfall #3 | `db/sync/sync_protocol.cpp:242,263` | **UPDATE — deep** (wire format changed: per-blob 32-byte ns prefix in BlobTransfer) |
| SyncNamespaceAnnounce | pre-122 | (unchanged) | `db/peer/message_dispatcher.cpp` | keep |
| ErrorResponse (existing codes 0x01-0x06) | pre-122 | (unchanged) | `db/peer/error_codes.h` | keep (extend table with 0x07-0x0B from D-09) |
| BlobNotify / BlobFetch / BlobFetchResponse | pre-122 | (unchanged) | `db/peer/message_dispatcher.cpp` | keep |
| Blob deletion via tombstone | pre-v4.1 | (unchanged) | `db/wire/codec.h:93` | UPDATE — DeleteAck/Delete=17 path still exists post-122 (node emits DeleteAck regardless of payload shape per STATE.md Phase 124 decision) |
| Namespace delegation + revocation | pre-v4.1 | (unchanged) | `db/engine/engine.cpp` delegation verify | keep |
| Pub/Sub (Subscribe/Unsubscribe/Notification) | pre-v4.1 | (unchanged) | `db/peer/message_dispatcher.cpp` | keep |
| PEX | pre-v4.1 | (unchanged) | `db/peer/pex_manager.cpp` | keep |
| Storage/quota signaling | pre-v4.1 | (unchanged) | — | keep |
| **SyncRejected reason codes** | pre-v4.1 | (unchanged) | `db/peer/sync_reject.h` | keep (already accurate) |
| **Blob type indexing (ListRequest flags, type_filter, 44-byte entries)** | 117 | `117-01-SUMMARY.md`, `117-RESEARCH.md` | `db/storage/storage.h` seq_map extension, `db/peer/message_dispatcher.cpp` ListRequest handler | **UPDATE — brief per D-08** (already present at PROTOCOL.md:788-812, needs cross-link + consistency check) |
| **Request pipelining (request_id correlation, default depth=8)** | 120 | `120-01-SUMMARY.md`, `120-02-SUMMARY.md` | `db/wire/transport.fbs` request_id field, `cli/src/pipeline_pump.h` | **UPDATE — brief per D-08** (already present at PROTOCOL.md:39-47; add depth=8 default) |
| Client query types (ReadRequest, ListRequest, ExistsRequest, etc.) | v1.3.0-v1.4.0 | (unchanged) | `db/peer/message_dispatcher.cpp` | keep |
| MetadataRequest/Response (signer_hint format change) | 122 | `122-07-SUMMARY.md` (MetadataRequest format change) | `db/peer/message_dispatcher.cpp` MetadataResponse handler | **UPDATE — brief** (pubkey field semantics changed post-122) |
| TimeRangeRequest/Response | pre-v4.1 | (unchanged) | — | keep |
| Message Type Reference table (0..64) | 122, 124 | — | `db/schemas/transport.fbs:5-71` | **UPDATE** — BlobWrite=64 added; Data=8 marked DELETED (and per D-11 the row will be deleted post-schema-regen) |
| Client envelope encryption (CENV) | pre-v4.1 | (unchanged) | `cli/src/envelope.cpp` | keep |
| HKDF label registry | pre-v4.1 | (unchanged) | — | keep |
| Prometheus /metrics endpoint | pre-v4.1 | (unchanged) | `db/peer/metrics_collector.cpp` | keep |

**Current `db/PROTOCOL.md` staleness hotspots** (must be corrected):
- Line 245-256: `Blob` schema shows 6 fields including `namespace_id` and
  `pubkey`. **Post-122 shape is 5 fields** (`signer_hint` replaces both).
  This is the single largest stale block.
- Line 262-272: canonical signing input still references `namespace_id`
  parameter name. Byte output identical (see `cli/src/wire.h:158-165`
  comment), but parameter doc must say `target_namespace`.
- Line 274-285: "Sending a Data Message" uses `type = Data (8)`. **Post-122
  path is `BlobWrite = 64` with `BlobWriteBody` envelope.**
- Line 591-608: Tombstone + Delegation sections describe pre-122 framing
  (`Data (8)`). Tombstones now go under `Delete=17` with `BlobWriteBody`
  envelope shape (STATE.md Phase 124 decision); delegations go under
  `BlobWrite=64` with `BlobWriteBody`. Needs rewrite.
- Line 891: `Data` at position 8 stays in the Message Type Reference as
  "Blob storage: FlatBuffer-encoded Blob payload" — must mark DELETED /
  remove per D-11.
- No mention of `BlobWrite=64` anywhere in current PROTOCOL.md.
- No mention of NAME magic, BOMB magic, PUBK magic, owner_pubkeys DBI,
  signer_hint, PUBK-first invariant.
- No error-code table for 0x07–0x0B.

### 1b. `README.md` (top-level project pitch + minimal quickstart)

Current file is 97 lines (single-screen). Scope is already tight per D-01.
Coverage matrix kept short because this doc is intentionally thin.

| Section | Source phase | Source artifacts | Source code | Depth |
|---|---|---|---|---|
| What This Is (PQ-secure blob store, signed, replicated) | all | — | — | keep |
| Feature bullet list | all | — | — | UPDATE (adjust wording if needed) |
| Documentation cross-links (point at PROTOCOL, ARCHITECTURE, cli/README, db/README) | — | — | — | **UPDATE** (add ARCHITECTURE.md link; current line 22 references 62 message types — post-122 it's 63 with BlobWrite=64 added; verify count) |
| Quick Start (build + keygen + run) | — | — | `db/main.cpp` | keep |
| Two-Node Sync example | — | — | — | keep |
| Building Your Own Client (points at PROTOCOL.md + schemas/) | 122 | — | `db/schemas/` | UPDATE (note: 5-field Blob schema + BlobWrite=64) |
| Crypto stack table | — | — | — | keep |
| Dependencies + License | — | — | — | keep |

Per D-01, this file stays one-screen. No feature-by-feature v4.1.0 coverage here.

### 1c. `cli/README.md` (cdb command reference + hello-world quickstart)

Current file 152 lines. Per D-03, add a 5-command hello-world block.

**Canonical cdb subcommand list** (verified from `cli/src/commands.h`):

| Subcommand | Entry point | v4.1.0 feature | Depth |
|---|---|---|---|
| `keygen` | `cmd::keygen` | pre-v4.1 | brief |
| `whoami` | `cmd::whoami` | pre-v4.1 | brief |
| `export-key` | `cmd::export_key` | pre-v4.1 | brief |
| `publish` | `cmd::publish` | 122 (emits PUBK blob in new format) | **UPDATE — note PUBK-first relation** |
| `put <file>...` | `cmd::put` | 116 (--share @group), 119 (chunked auto-split at 400 MiB), 123 (--name, --replace) | **DEEP (--name, --replace, --share, --ttl)** |
| `put --name <name> <file>` | `cmd::put` | 123 | deep |
| `get <hash>...` | `cmd::get` | 119 (CPAR auto-reassembly), 120 (pipelined) | **DEEP (hash + name forms)** |
| `get <name>` | `cmd::get_by_name` | 123 | deep |
| `get --all --from <contact>` | `cmd::get` | 116 | brief |
| `rm <hash>` | `cmd::rm` | pre-v4.1 (single-target tombstone) | brief |
| `rm <hash>...` (multi) | `cmd::rm_batch` | 123 (BOMB batched), 124 D-06 (CPAR cascade) | **DEEP (batching + chunked cascade)** |
| `reshare <hash>` | `cmd::reshare` | pre-v4.1 | brief |
| `ls` / `ls --raw` / `ls --type <TYPE>` | `cmd::ls` | 117 (type filter + hide-by-default) | **BRIEF per D-08** (type filter is v4.1.0 deep-enough already in PROTOCOL.md) |
| `exists <hash>` | `cmd::exists` | pre-v4.1 | brief |
| `info` | `cmd::info` | pre-v4.1 | brief |
| `stats` | `cmd::stats` | pre-v4.1 | brief |
| `delegate <target>` | `cmd::delegate` | pre-v4.1 | brief |
| `revoke <target>` | `cmd::revoke` | pre-v4.1 | brief |
| `delegations` | `cmd::delegations` | pre-v4.1 | brief |
| `contact add/rm/list` | `cmd::contact_*` | 116 | brief |
| `contact import/export` | `cmd::contact_import`/`export` | 116 | brief |
| `group create/add/rm/list` | `cmd::group_*` | 116 | **BRIEF** (keep current coverage) |

Additional deep sections needed per D-08:
- **Hello-world quickstart** (D-03): keygen → publish → put → ls → get, ~15 lines
- **Auto-PUBK + first-write flow** (Phase 124): when the CLI transparently
  emits PUBK before the user's first write; what error user sees if
  auto-PUBK fails (code 0x07 wording from §5).
- **Chunked upload / download** (Phase 119): 400 MiB threshold,
  `--chunk-size`, CPAR reassembly, retry behavior.
- **Pipelining** (Phase 120): what it does transparently (multi-blob get
  goes over one connection, depth=8), operator-visible only as "fast".
- **Global flags**: `--host`, `--node`, `--uds`, `--port`, `-v`/`-q`,
  `--identity`. Verified from `main.cpp:74-80` and the ConnectOpts struct.

**Current cli/README.md staleness / gaps:**
- No mention of `--name` / `get <name>` workflow (Phase 123).
- No mention of batched `rm <h1> <h2> <h3>` / BOMB (Phase 123).
- No mention of chunked files / CPAR reassembly (Phase 119).
- No mention of auto-PUBK or PUBK-first (Phase 122/124).
- No mention of `ls --type` (Phase 117) — current doc says "`ls` List blobs in namespace" only.
- No mention of the config-file resolver (`~/.cdb/config.json` `default_node`,
  named nodes) — only `host: ...` is shown.
- `publish` described as "Publish pubkey to node for contact discovery" —
  post-122 it's load-bearing (PUBK-first invariant requires it).
- No hello-world quickstart per D-03.

### 1d. `db/ARCHITECTURE.md` (NEW — internal implementation doc)

See §4 for the full proposed skeleton.

| Section (top-level) | Source phase | Source artifacts | Source code | Depth |
|---|---|---|---|---|
| Storage layer — 8 DBIs, libmdbx geometry, ACID | 122 (added owner_pubkeys DBI) | `122-02-SUMMARY.md`, `121-VERIFICATION.md` | `db/storage/storage.h:95-108`, `db/storage/storage.cpp` | deep |
| Storage — strand model + STORAGE_THREAD_CHECK (D-05) | 121 | `121-VERIFICATION.md`, `121-TSAN-RESULTS.md` | `db/storage/thread_check.h`, `db/engine/engine.cpp:245,265,349,378,547,584` | **D-05: dedicated ~40-line subsection** |
| Engine — ingest pipeline (11 steps) | 122, 123 | `122-04-SUMMARY.md`, `123-03-SUMMARY.md` | `db/engine/engine.cpp` ingest method | deep |
| Engine — PUBK-first enforcement | 122 | `122-04-SUMMARY.md` | `db/engine/engine.cpp` | deep |
| Engine — signer_hint resolution (owner vs delegate path) | 122 | `122-02-SUMMARY.md`, `122-04-SUMMARY.md` | `db/engine/engine.cpp` verify path | deep |
| Engine — BOMB cascade side-effect (per-target tombstone emit) | 123 | `123-03-SUMMARY.md`, `123-04-SUMMARY.md` | `db/engine/engine.cpp:398+` | deep |
| Engine — thread-pool crypto offload + executor transfer | pre-v4.1 hardened | `121-VERIFICATION.md` | `db/engine/engine.cpp:245,265,...`, `db/crypto/thread_pool.h` | deep |
| Net — handshake state machine (PQ + lightweight fallback) | pre-v4.1 | (unchanged) | `db/net/handshake.cpp`, `db/net/connection.cpp` | brief (link to PROTOCOL) |
| Net — AEAD framing, chunked sub-frame pump | 115 | (unchanged) | `db/net/framing.cpp`, `db/net/connection.cpp:834-972` | brief (link to PROTOCOL) |
| Net — role signalling classification | 999.1 | ROADMAP archived | `db/net/role.h` | brief |
| Peer — PeerManager 6-component decomposition | v2.2.0 | — | `db/peer/*.cpp` | brief |
| Peer — sync orchestrator + cursor model | pre-v4.1 | (unchanged) | `db/peer/sync_orchestrator.cpp` | brief |
| Peer — BlobNotify/BlobFetch push path | pre-v4.1 | — | `db/peer/blob_push_manager.cpp` | brief |
| Peer — PEX + reconnect + inactivity | pre-v4.1 | — | `db/peer/pex_manager.cpp`, `db/peer/connection_manager.cpp` | brief |
| **Configurable constants (5 new knobs)** | 118 | `118-01-SUMMARY.md` | `db/config/config.h:51` Sync/peer tuning | **brief per D-08** |
| **Peer management subcommands (add-peer/remove-peer/list-peers)** | 118 | `118-02-SUMMARY.md` | `db/main.cpp` subcommand dispatch | **brief per D-08** |
| Identity + key lifecycle | pre-v4.1 | — | `db/identity/identity.cpp` | brief |
| Metrics collection + /metrics endpoint | pre-v4.1 | — | `db/peer/metrics_collector.cpp` | brief |
| ACL (closed mode, SIGHUP reload) | pre-v4.1 | — | `db/acl/access_control.cpp` | brief |

### 1e. Cross-reference matrix (D-02 zero-duplication enforcement)

Per D-02, each fact lives in exactly one file. Reader traversal path:

| Fact | Owner doc | Linkers |
|---|---|---|
| Blob wire format (5 fields) | PROTOCOL.md §Blob schema | ARCHITECTURE.md §Engine ingest → link; cli/README.md (no direct link, too deep) |
| Canonical signing input | PROTOCOL.md §Canonical Signing Input | ARCHITECTURE.md §Engine verify → link |
| PUBK-first invariant (protocol rule) | PROTOCOL.md §signer_hint / PUBK-first | ARCHITECTURE.md §Engine PUBK-first (implementation); cli/README.md §publish (one sentence + link) |
| owner_pubkeys DBI layout | PROTOCOL.md brief mention; ARCHITECTURE.md §Storage DBIs owns impl detail | — |
| BOMB + NAME wire format | PROTOCOL.md §BOMB, §NAME | ARCHITECTURE.md §Engine BOMB cascade → link; cli/README.md §rm, §put --name (one-liner + link) |
| Error codes 0x07–0x0B | PROTOCOL.md §ErrorResponse (full table) | cli/README.md (no link — table is internal; user-visible wording is already in CLI) |
| Strand model + STORAGE_THREAD_CHECK | ARCHITECTURE.md §Storage strand model | PROTOCOL.md: no reference (implementation-only) |
| Config fields | db/README.md (existing Configuration section) | ARCHITECTURE.md references by field name |
| cdb subcommands | cli/README.md (owner) | — |
| Chunked file format (CDAT/CPAR) | PROTOCOL.md §Chunked Large Files (needs new subsection under §Storing a Blob) | cli/README.md §put (brief); ARCHITECTURE.md (no reference) |
| Request pipelining (request_id) | PROTOCOL.md §Transport Layer (already present) | cli/README.md §get (one-liner "multi-hash gets pipeline transparently") |

## 2. Existing doc state audit

### 2a. `README.md`
- **Lines:** 97
- **Last touched:** `e92b6ab9 docs: rewrite README for open-source release (database only)` — pre-v4.1.0
- **Keep:** Intro paragraph, Crypto stack table, Dependencies list, Build example, Two-Node Sync example.
- **Rewrite:** Feature bullet list (add v4.1.0 items), Documentation cross-link (add ARCHITECTURE.md), "62 message types" count (→ 63 post-122, or drop the number entirely).
- **Delete:** Nothing substantive.
- **Stale content:** "62 message types" count at line 22. No PUBK / NAME / BOMB mentions. No chunked files / contact groups / peer management mentions in the bullets. No link to cli/README or ARCHITECTURE.
- **Cross-references present:** `db/PROTOCOL.md`, `db/README.md`, `db/schemas/`. **Missing:** `cli/README.md`, `db/ARCHITECTURE.md` (doesn't exist yet).

### 2b. `db/PROTOCOL.md`
- **Lines:** 1386
- **Last touched:** `f038faee proto: drop separate git_hash field from NodeInfoResponse` (post-role-signalling, pre-122)
- **Keep:** Transport layer (§1-79), PQ/lightweight handshake (§80-180), Role Signalling (§181-213), UDS (§161-180), TTL Enforcement (§287-350), Sync Phase A/B/C (§476-583 — but Phase C needs per-blob ns-prefix note per 122-PATTERNS Pitfall #3), BlobNotify/BlobFetch, SyncNamespaceAnnounce, PEX, Storage/Quota/SyncRejected/Rate-limit, Client query types (ReadRequest, ListRequest, …, TimeRangeRequest), AEAD Nonce Counters note, FlatBuffers Determinism note, ML-DSA-87 Non-determinism note, Client-Side Envelope Encryption, HKDF Label Registry, Prometheus Metrics.
- **Rewrite:**
  - Blob Schema (lines 243-256) — 6 fields → 5 fields.
  - Canonical Signing Input (lines 258-272) — rename parameter to `target_namespace`; byte output unchanged but documentation must reflect the new semantic.
  - "Sending a Data Message" (lines 274-285) — switch to `BlobWrite=64` + `BlobWriteBody` envelope; retain `Delete=17` path for tombstones; add `BlobWriteBody{target_namespace:32, blob:Blob}` wire shape.
  - Blob Deletion (lines 586-597) — tombstone `Delete=17` now wraps `BlobWriteBody`.
  - Namespace Delegation (lines 599-624) — delegation blob now sent via `BlobWrite=64`.
  - MetadataResponse pubkey field (line 1034) — semantic changed: was author pubkey, now `signer_hint` (32 bytes). Check actual response format in `db/peer/message_dispatcher.cpp` and reconcile.
  - Message Type Reference (lines 891-946) — add row 64 `BlobWrite`; mark row 8 `Data` DELETED; verify no other rows need post-122 rewording.
- **Add (new sections):**
  - Namespace derivation (SHA3 of signing pubkey, NOT per-blob)
  - signer_hint semantics
  - owner_pubkeys DBI (byte-level public API — external impls need it to understand MetadataResponse and the verify flow)
  - PUBK blob format (magic + signing_pk + kem_pk = 4164 bytes)
  - PUBK-first invariant
  - BlobWrite envelope (MsgType=64, BlobWriteBody)
  - NAME magic + semantics
  - BOMB magic + semantics + ttl=0 invariant
  - D-15 same-second tiebreak note (brief, flag as STATE.md FLAG)
  - Error codes 0x07–0x0B (table, with literal CLI wording — §5)
- **Delete:** None outright, but mark old schema deprecated pre-rewrite.
- **Cross-references present:** internal anchors, Mermaid diagrams (D-06: keep or convert to ASCII — the existing doc mixes both; D-06 says ASCII only for ARCHITECTURE but this PROTOCOL.md file already has Mermaid at lines 228-239, 379-393. Claude's discretion call whether to strip, but D-06 literally applies to ARCHITECTURE not PROTOCOL. **Recommendation: leave PROTOCOL's existing Mermaid as-is** per feedback-no-duplication of scope; D-06 is explicitly scoped to ARCHITECTURE.md).

### 2c. `db/README.md`
- **Lines:** 477
- **Last touched:** `c857a14c chore(100-01): scrub stale relay/SDK references from build, docs, dist` + `e92b6ab9 docs: rewrite README for open-source release (database only)` — pre-v4.1.0
- **Keep:** Crypto stack table, Building section, Sanitizer Builds section, Testing sections, Usage section (keygen, run, backup, version), Signals section, Wire Protocol section (update count + new message types), Scenarios (Single Node, Two-Node Sync, Closed Mode, Rate-Limited, Trusted Local Peers, Logging, Resilient Node), Deployment section, Features section.
- **Rewrite:**
  - Architecture paragraph (§22-27) — add mention of post-122 signer_hint model and PUBK-first invariant (one sentence each, defer to ARCHITECTURE.md for detail per D-02).
  - Configuration section (§122-189) — ADD the 5 new Phase-118 config knobs (`blob_transfer_timeout`, `sync_timeout`, `pex_interval`, `strike_threshold`, `strike_cooldown`).
  - Wire Protocol paragraph (§202-208) — "62 message types" → 63 (or drop the number).
  - Usage section — add `chromatindb add-peer/remove-peer/list-peers` subcommands (Phase 118).
  - Features section — add: Blob Type Indexing (117), Chunked Transport (119), Request Pipelining (120), Storage Concurrency Invariant (121), post-122 signing model. All BRIEF per D-08.
- **Add (new sections):**
  - Brief cross-link to `ARCHITECTURE.md` ("For implementation details (strand model, DBI layout, ingest pipeline), see ARCHITECTURE.md").
  - Brief cross-link to `cli/README.md` for the cdb user.
- **Delete:** None.
- **Stale content:** Wire Protocol paragraph says "62 message types" (post-122 is 63 with BlobWrite=64 added; post-D-11 cleanup, Data=8 is removed from the enum but BlobWrite=64 adds one — net still 63). Configuration section misses 5 Phase 118 knobs. Features section misses 116/117/118/119/120/122/123 items entirely.
- **Scope decision (Claude's Discretion per CONTEXT.md):** db/README.md DOES need touch. It's the operator-facing doc and the Phase 118 knobs + new subcommands genuinely belong here. Recommendation: treat it as part of Plan 3 (ARCHITECTURE + db/README delta).

### 2d. `cli/README.md`
- **Lines:** 152
- **Last touched:** `870a3d89 docs(116-02): add Contact Groups and Bulk Import/Export sections to README` — Phase 116
- **Keep:** Introduction paragraph, Build quickstart, Config File example, Contacts section, Contact Groups section, Bulk Import/Export section, Commands table (extend), Global Flags table, Encryption section, Dependencies.
- **Rewrite:**
  - Quick Start examples (lines 6-24) — still valid but could benefit from hello-world sequencing per D-03.
  - Commands table (lines 95-117) — add `put --name`, `get <name>`, `rm <h> <h>...` (multi-target), `ls --type`, `ls --raw`. Update `publish` description to mention PUBK-first context.
  - Global Flags table (lines 118-126) — add `--node`, `--host`, `--chunk-size` (if exposed), `-v`/`-q` verbose/quiet pair.
  - Connection section (lines 128-130) — update default port + mention config-file `default_node` resolver.
- **Add (new sections):**
  - **Hello-world quickstart** (D-03, ~15 lines): keygen → publish → put hello.txt → ls → get <hash>
  - **Chunked large files** (brief, Phase 119): 400 MiB auto-chunking, CPAR manifest reassembly on get, `cdb rm` of manifest cascades to all CDAT chunks.
  - **Request pipelining** (brief, Phase 120): multi-hash get is transparent; one connection, depth=8.
  - **Mutable names** (deep per D-08, Phase 123): `cdb put --name <n> <file>`, `cdb put --name <n> --replace <file>`, `cdb get <name>`, local `name_cache.json` resolution with NAME-blob enumeration fallback.
  - **Batched deletion** (deep per D-08, Phase 123): `cdb rm <h1> <h2> <h3>...` emits one BOMB. `cdb rm <cpar_hash>` cascades to all CDAT children per Phase 124 D-06.
  - **Auto-PUBK + PUBK-first error** (brief, Phase 124): user runs `publish` first; if skipped, first write sees error 0x07 with the auto-PUBK-failure message. Link to PROTOCOL.md error table only by anchor (no body duplication per D-02).
- **Delete:** None.
- **Stale content:** See §1c list above.
- **Cross-references present:** None today. **Missing:** `db/PROTOCOL.md#error-codes`, `db/PROTOCOL.md#signer-hint`.

### 2e. `db/ARCHITECTURE.md`
- **Does not exist.** Creation is the sole D-04 deliverable. Skeleton at §4.

## 3. Code-comment cleanup scope

### 3a. `// Phase N` breadcrumb inventory (D-13)

**Total count** (across `.cpp`, `.h`, `.fbs`): **313 occurrences across 49 files** (grep `// Phase [0-9]+`). Extending to all comment styles (including `/// Phase N`, `// ... Phase N ...`): **452 occurrences across 67 files**.

**Per-file breakdown (exact `// Phase [0-9]+` pattern, top offenders):**

| File | Hits | Comment character |
|---|---|---|
| `db/tests/engine/test_engine.cpp` | 78 | Test annotations — likely safe to strip |
| `db/tests/peer/test_peer_manager.cpp` | 77 | Test annotations |
| `cli/src/commands.cpp` | 19 | Mix of historical breadcrumbs and load-bearing invariants — see sample below |
| `db/tests/storage/test_storage.cpp` | 15 | Test annotations |
| `db/tests/config/test_config.cpp` | 9 | Test annotations |
| `db/peer/sync_orchestrator.cpp` | 8 | Implementation breadcrumbs (Phase 122 Pitfall #3, Phase 115 etc.) |
| `db/tests/test_helpers.h` | 5 | Phase tags on helper fns (e.g. `Phase 123 D-03: build a properly signed NAME blob`) |
| `db/peer/message_dispatcher.cpp` | 5 | Breadcrumbs in dispatcher routing |
| `db/peer/error_codes.h` | 5 | Phase tags on error-code constants |

**Spot-check sample from `cli/src/commands.cpp`:**

| Line | Comment | Classification |
|---|---|---|
| 519 | `// Phase 123: Resolve a NAME to its current winner's target content_hash` | **STRIP — D-13**: historical breadcrumb, text reads fine without |
| 530 | `// Phase 123 helper: build a signed NAME blob for a (name, target_content_hash)` | **STRIP** — same |
| 573 | `// Phase 124 Rule-1 fix (plan 05): BOMBs are structurally regular blobs` | **STRIP and rewrite** — the WHY ("BOMBs are structurally regular blobs, so MsgType=BlobWrite not Delete") is load-bearing per STATE.md Phase 124 decision, but the "Phase 124 Rule-1 fix (plan 05)" prefix is not. Keep the WHY, drop the prefix. |
| 658 | `// Phase 123 Step 0 (--replace only): look up the prior NAME binding BEFORE` | **STRIP prefix, keep WHY** — the "look up prior NAME BEFORE" is useful |
| 690 | `// Phase 119 / D-14: reject files above the hard 1 TiB cap up-front,` | **STRIP prefix** — keep "reject files above 1 TiB cap up-front" |
| 1209 | `// Phase 119 / CHUNK-04 + Phase 124 D-06: classify the target before` | **STRIP prefix** — keep the classification WHY |

Mechanical rule for D-13: **delete "Phase N" and "Phase N / X-Y" tokens from
comments; leave the surrounding sentence intact if it carries a WHY; delete
the whole comment if the "Phase N..." IS the whole comment body.** Decision
for each case is local to that comment — most are mechanical strips, a few
need preserved load-bearing content.

**Test files** are easier: the "Phase N" token in test annotations is
decorative (documents which phase introduced the test). Safe to strip across
the board. Test-case NAMES stay untouched (Catch2 tag `[phase122]` etc., if
any, are out of scope — D-13 is comment cleanup, not tag cleanup).

### 3b. D-14 over-commenting scope

No exact grep exists; policy is "does this comment carry load-bearing WHY".

**Sampling methodology** (for the planner's D-14 plan):

1. **Passes:** one file at a time.
2. **File set (MVP — 10-15 files covers ~40% of repo LOC):**
   - `cli/src/commands.cpp` (2929 lines, ~14% comment density — 418 comment lines). Top candidate — mix of phase breadcrumbs, implementation notes, and genuinely load-bearing invariants.
   - `cli/src/chunked.cpp` (747 lines, 15%)
   - `cli/src/connection.cpp` (798, 14%)
   - `cli/src/wire.cpp` (555, 12%)
   - `cli/src/wire.h` (432 lines, ~20+% — many `///` doc comments)
   - `db/engine/engine.cpp` (666, 18% — high density, mostly Step 0a/b/c/d/e annotations which ARE load-bearing)
   - `db/storage/storage.cpp` (1988, 10%)
   - `db/storage/storage.h` (402, high-density doc comments)
   - `db/peer/message_dispatcher.cpp` (1520, 2% — already lean)
   - `db/net/connection.cpp` (1101, 11%)
   - `db/sync/sync_protocol.cpp` (308, 11%)
   - `db/wire/codec.cpp` (272, 13%)

**Decision policy for each comment:**

- **Keep:** comments that explain non-obvious WHY — a hidden invariant, a
  subtle AEAD-nonce ordering constraint, a libmdbx-specific quirk, a TSAN
  finding that shaped the code, a protocol-level rejection reason.
- **Strip:** comments that re-state the next line in English. Example:
  `// increment counter` above `counter_++;`.
- **Strip:** comments that cite a phase number but nothing else. These are
  pure history.
- **Delete:** commented-out code blocks. Per user directive (CONTEXT.md
  Claude's Discretion), commented-out code is always cruft.
- **Keep-then-rewrite:** comments that carry load-bearing WHY but lead with
  a phase prefix ("Phase 122: …"). Strip the prefix; keep the substance.

**Estimated magnitude:** Given the 10-18% comment density and the sample
above, I estimate **50-70% of commented lines are load-bearing** (they
explain non-obvious invariants, protocol constraints, thread-safety notes).
The hygiene pass is likely to remove **~20-30% of comment lines** in the
high-density files (dropping phase prefixes, dead breadcrumbs, and any
commented-out code).

**Risk assessment — comments that LOOK obvious but aren't:**

- **Strand/threading assertions:** `// CONC-04 (Phase 121): post back to ioc_ before Storage access.` looks like a breadcrumb but cites a TSAN ship-gate finding. Keep the WHY, strip the `(Phase 121)` prefix.
- **AEAD nonce order invariants:** multiple comments in `db/net/connection.cpp` cite "pre-existing Phase 84 fix: per-connection send queue prevents AEAD nonce desync." Keep — this is the historical root cause of a crash class.
- **BOMB ttl=0 invariant:** `// Phase 123 D-13(1): BOMB is permanent like a single tombstone, exempt from max_ttl.` This is a protocol-level invariant, not a breadcrumb. Keep the WHY, strip the prefix, ideally add a reference anchor to PROTOCOL.md.
- **MDBX mmap quirks:** comments about `used_data_bytes` vs `used_bytes` geometry differences. Keep — someone will try to "simplify" this and break the metrics.
- **FlatBuffer non-determinism:** comments in `cli/src/wire.cpp` about `ForceDefaults(true)`. Keep.

**Guardrail for D-14:** **Re-run the full Catch2 suite after the pass** —
accidental code deletion during comment cleanup is the primary risk. The
MEMORY constraint `feedback_delegate_tests_to_user.md` means orchestrator-
level full rebuilds are the user's job; planner should ensure the user knows
a full test run is required post-D-14.

### 3c. Pre-122 vestige cleanup (SC#6 literal scope)

**Comments referencing removed `BlobData.namespace_id`:**
- `cli/src/wire.h:158-165` contains "byte output IDENTICAL to pre-rename (Phase 122 D-01 invariant)" — the comment correctly documents the rename. Keep but strip phase prefix.
- No active code references to `BlobData.namespace_id` (the field) remain on the hot path — grep confirms the only hits are in `.planning/` phase artifacts and this RESEARCH doc itself.

**Comments referencing removed `BlobData.pubkey`:**
- Grep for `BlobData.pubkey` / `.pubkey` on `BlobData` struct — **no active references.** The post-122 field is `signer_hint`, and the D-14 pass should check that no lingering doc comments still mention "the 2592-byte embedded pubkey."

**`MsgType::Data = 8` references:**
- Grep confirms `MsgType::Data` has **zero functional hits** in `cli/src/*` (verified in `124-03-SUMMARY.md:204`: `grep -rn "MsgType::Data" cli/src/ returns 0 lines phase-wide`).
- `TransportMsgType_Data` still appears in:
  - `db/wire/transport_generated.h:38` (generated file — will be regenerated after D-11)
  - `db/wire/transport_generated.h:109` (EnumNamesTransportMsgType table — same file)
  - `db/tests/net/test_connection.cpp` (9 sites, lines 55, 166, 200, 308, 340, 498, 524, 582, 646, 930, 938 — 11 hits)
  - `db/tests/net/test_framing.cpp` (lines 222, 234 — 2 hits)
  - `db/tests/net/test_protocol.cpp` (lines 73, 76, 265 — 3 hits)
  - `db/peer/message_dispatcher.cpp:1388` (comment only — the memo)

**D-11 execution path:**
1. Delete `Data = 8` row from `db/schemas/transport.fbs:14`. Post-deletion, enum values shift (but FlatBuffers enum values are explicit numerics, so gaps are fine — no cascade). **Verify: does flatc require a `deprecated` attribute or does literal deletion work?** For Catch2-generated FlatBuffer accessors, the enum value is referenced by name (`TransportMsgType_Data`), so deleting it causes a compile error at the test sites — this is the intended breakage.
2. Regenerate `db/wire/transport_generated.h` via `flatc --cpp db/schemas/transport.fbs`. **Plan must specify the flatc binary path** — typically `build/_deps/flatbuffers-build/flatc` after CMake FetchContent. Hand-editing the generated header is forbidden.
3. Update `db/tests/net/test_protocol.cpp`, `test_framing.cpp`, `test_connection.cpp` call sites:
   - Migrate to `TransportMsgType_BlobWrite` where the test is exercising the data-write path.
   - Delete test cases that ONLY exercised the pre-122 dispatcher branch (check each case individually).
4. Delete the `// TransportMsgType_Data direct-write branch was DELETED — no backward compat.` comment at `db/peer/message_dispatcher.cpp:1388`.

**Old signing-input shape references:**
- Grep for `namespace_id || data || ttl || timestamp` — hits are in `.planning/` artifacts only (historical). Source code uses `build_signing_input` now and the parameter was renamed to `target_namespace` (per `cli/src/wire.h:158-165` comment). **No code-comment cleanup needed** for the old shape; the rename is complete.

## 4. `db/ARCHITECTURE.md` skeleton proposal

Top-down per D-04: Storage → Engine → Net. All diagrams ASCII per D-06. Target
~700-800 lines total.

```
# chromatindb — Architecture

## Overview (~30 lines)
 - One-paragraph problem framing
 - Three-tier structure: Storage (libmdbx) + Engine (validation, ingest, cascade) + Net (peer wire)
 - Concurrency model pointer: all storage access via io_context strand (see §Storage Strand Model)
 - Cross-link to PROTOCOL.md for wire format (D-07)

## Storage Layer (~250 lines)

### 8 Sub-databases (~100 lines)
 - ASCII table (80-col safe) listing each DBI: name, key schema, value schema, owner phase, size characteristics
 - blobs, sequence, expiry, delegation, tombstone, cursor, quota, owner_pubkeys
 - Per-DBI: access patterns (hot read vs. write), max_maps = 10 (after Phase 122)
 - Link to storage.h:95-108 for the canonical source

### Transaction Model (~40 lines)
 - libmdbx MVCC: read txns don't block writes; single writer at a time
 - Write-path example (pseudocode): open write txn → mutate DBIs → commit → notify
 - Error mapping: MDBX_MAP_FULL → StorageFull signal

### Storage Strand Model (D-05 — ~40 lines)
 - ASCII diagram:
    io_context (single-threaded owner)
          │
          ▼
    ┌────────────────┐   co_await asio::post(ioc_, …)
    │  Storage::*    │ ◄─────── crypto thread pool workers
    └────────────────┘          (verify, SHA3 — heavy)
          │
          ▼
    STORAGE_THREAD_CHECK() → assert(tid == owner_tid)
 - Why: libmdbx write txns aren't thread-safe; co_await suspends can resume on ANY thread
   from the thread pool. All Storage public methods begin with STORAGE_THREAD_CHECK(),
   which captures owner tid on first call and asserts on every subsequent entry.
 - Evidence: Phase 121 TSAN ship-gate (`.planning/phases/121-storage-concurrency-invariant/121-VERIFICATION.md`), concurrent-ingest stress test confirmed zero TSAN findings.
 - Rule for new contributors: offload compute to thread_pool, but post back to ioc_ before touching Storage. See engine.cpp:245 for the canonical pattern.

### Encryption at Rest (~30 lines)
 - ChaCha20-Poly1305 envelope, HKDF label `chromatindb-dare-v1`
 - Master key at data_dir/master.key (0600 permissions)
 - Link to PROTOCOL.md §HKDF Label Registry

### Expiry + Quotas (~40 lines)
 - Expiry DBI keyed by expiry_ts_be
 - Periodic scanner, evict-on-query behavior
 - Per-namespace quota DBI, global default + overrides

## Engine Layer (~250 lines)

### Ingest Pipeline (~80 lines)
 - ASCII flow diagram: network recv → engine.ingest() → [11 steps] → storage.put_blob() → ack
 - Steps: 0a TTL shape, 0b timestamp window, 0c already-expired, 0d tombstone ttl=0, 0e max_ttl, 1 structural, 2 dedup gate, 3 SHA3(pubkey) → ns derivation, 4 signer_hint resolution (owner vs delegate), 4.5 PUBK-first gate + PUBK register, 5 signature verify, 6 storage write + side-effects
 - Source: db/engine/engine.cpp (666 lines — this section summarizes, not duplicates)

### PUBK-First Invariant (~30 lines)
 - Rule: on every ingest, if owner_pubkeys has no row for target_namespace, incoming blob MUST be a PUBK magic or the write is rejected (error 0x07).
 - Applies to: direct writes, delegate writes, replicated writes from sync path.
 - Test coverage: test_pubk_first.cpp (direct), test_pubk_first_sync.cpp (sync path), test_pubk_first_tsan.cpp (concurrent registration race)
 - Link to PROTOCOL.md §PUBK-First for the external-facing wire rule.

### signer_hint Resolution (~30 lines)
 - Owner path: lookup signer_hint in owner_pubkeys DBI → get signing pk → verify
 - Delegate path: lookup (target_namespace, signer_hint) in delegation DBI → get delegate's signing pk → verify
 - Cached lookup: owner_pubkeys is a hot read; no cache needed today (blob_hash dedup short-circuits before signer_hint lookup on duplicates)

### BOMB Cascade Side-Effect (~50 lines)
 - BOMB = batched tombstone. On ingest, engine emits one single-target tombstone
   blob per target in the BOMB payload (D-05 cascade).
 - Permanence invariant: BOMB ttl=0, same as single tombstone. Enforced at Step 0e.
 - Delegate writers cannot emit BOMB (engine rejects at line 311 — error 0x0B).
 - Malformed BOMB rejection: error 0x0A.
 - Post-Plan-05 bug fix note: BOMBs ship under MsgType::BlobWrite, not Delete,
   because the node's Delete dispatcher rejects non-tombstone payloads. See STATE.md
   Phase 124 Plan 05 "Rule-1 fix" for the root cause.

### Thread Pool Crypto Offload (~40 lines)
 - ML-DSA-87 verify ~5-10 ms; SHA3 ~sub-ms. Both offloaded to crypto_thread_pool.
 - Pattern: offload() in thread pool → co_await asio::post(ioc_) → Storage access.
 - Hardware-concurrency sized by default (config: worker_threads=0 → auto).

### Ingest Error Codes (~20 lines)
 - Cross-link to PROTOCOL.md §ErrorResponse (owns the full table)
 - Implementation reference: db/peer/error_codes.h

## Net Layer (~200 lines)

### Handshake State Machine (~40 lines)
 - Two-path: PQ (ML-KEM-1024 + ML-DSA-87) vs lightweight (ML-DSA-87 only over shared nonces)
 - Fallback: lightweight → PQRequired signal → initiator retries as PQ
 - Role byte in AuthSignature (1 byte post-999.1): PEER or CLIENT, reserved 0x02-0x04
 - Cross-link to PROTOCOL.md §PQ Handshake, §Role Signalling (D-07)

### AEAD Framing + Chunked Sub-Frames (~40 lines)
 - Frame format: 4-byte BE length + AEAD ciphertext
 - Chunked: 0x01 flag byte, 14-byte header, 1 MiB data sub-frames, 0-byte sentinel
 - Per-connection send queue prevents AEAD nonce desync (pre-existing v2.0 hardening — cite here as a subtle invariant future contributors will trip over)
 - Cross-link to PROTOCOL.md §Chunked Transport Framing

### PeerManager Decomposition (~40 lines)
 - 6 components from v2.2.0: ConnectionManager, MessageDispatcher, SyncOrchestrator, MetricsCollector, PexManager, BlobPushManager
 - Wiring: reference injection at construction; facade via PeerManager preserves old public API

### Sync Orchestration (~40 lines)
 - Phase A/B/C: per-namespace exchange → reconciliation → blob transfer
 - Cursor DBI tracks per-peer per-namespace seq progress
 - Safety-net reconciliation every N rounds (config: safety_net_interval_seconds)
 - BlobNotify/BlobFetch is the primary path; sync is the backstop
 - Per-blob target_namespace prefix in BlobTransfer (Phase 122 Pitfall #3)

### PEX + Reconnect + Inactivity (~40 lines)
 - Inline PEX after sync completes (serialized per-connection — avoids nonce desync)
 - Reconnect with jittered exp backoff + ACL-aware extended backoff
 - Receiver-side inactivity timeout (configurable)

## Configuration + Subsystems (~80 lines)

### Configurable Constants (Phase 118) (~20 lines, BRIEF per D-08)
 - 5 knobs added: blob_transfer_timeout, sync_timeout, pex_interval, strike_threshold, strike_cooldown
 - All SIGHUP-reloadable where safe; validated on load with range checks
 - Source: db/config/config.h:51 and db/README.md Configuration reference

### Peer Management Subcommands (Phase 118) (~15 lines, BRIEF)
 - chromatindb add-peer / remove-peer / list-peers edit config.json + emit SIGHUP
 - Pidfile under data_dir — source of truth for SIGHUP target

### Identity Management (~15 lines)
 - ML-DSA-87 keypair in data_dir/node.key + node.pub
 - Namespace = SHA3-256(signing_pk) — derived on load

### Metrics + Observability (~15 lines)
 - NodeMetrics strand-confined (v2.2.0 — no atomics)
 - Prometheus /metrics endpoint (optional, SIGHUP reloadable bind address)
 - SIGUSR1 dumps snapshot to log

### ACL Model (~15 lines)
 - allowed_peer_keys, allowed_client_keys: empty = open, non-empty = closed
 - Role-routed (post-999.1): AuthSignature role byte picks the list
 - SIGHUP reload disconnects unauthorized peers
```

**Total estimate:** ~750 lines fits within D-01's "manageable" size. If any
section grows beyond its budget the overflow goes to PROTOCOL.md (byte-level)
or db/README.md (operator-level) per the D-02 ownership rules.

## 5. Error-code table source (D-09)

Extracted verbatim from `cli/tests/test_wire.cpp:658-716` `[error_decoder]`
TEST_CASE. These strings are asserted under `REQUIRE(call(0xXX) == "…")`
literal equality — changing the wording breaks the test. The CLI enforces
that these strings NEVER leak internal tokens (`PUBK_FIRST_VIOLATION`,
`PUBK_MISMATCH`) or phase numbers to the user (SC-124-7).

| Code | Canonical name | Trigger condition | User-facing wording (byte-identical to [error_decoder] TEST_CASE) |
|---|---|---|---|
| 0x07 | `ERROR_PUBK_FIRST_VIOLATION` | Ingest would violate PUBK-first: target namespace has no owner_pubkeys row and the incoming blob is not a PUBK blob. Node rejects per Phase 122 SC#4. | `Error: namespace not yet initialized on node {host}. Auto-PUBK failed; try running 'cdb publish' first.` |
| 0x08 | `ERROR_PUBK_MISMATCH` | signer_hint matches an owner_pubkeys row but the resolved signing pubkey doesn't match the claimed identity (cross-namespace race, or forged signer_hint). Live-trigger infeasible (SHA3-256 collision needed); covered by unit test. | `Error: namespace {ns_short} is owned by a different key on node {host}. Cannot write.` where `ns_short = to_hex(ns_hint[0..8])` (16 lowercase hex chars) |
| 0x09 | `ERROR_BOMB_TTL_NONZERO` | BOMB blob submitted with ttl != 0. Engine rejects at Step 0e per Phase 123 D-13(1). | `Error: batch deletion rejected (BOMB must be permanent).` |
| 0x0A | `ERROR_BOMB_MALFORMED` | BOMB payload fails structural decode (magic mismatch, count overflow, or target list truncation). | `Error: batch deletion rejected (malformed BOMB payload).` |
| 0x0B | `ERROR_BOMB_DELEGATE_NOT_ALLOWED` | Delegate identity attempted to emit a BOMB. Node engine rejects at line 311 per Phase 123 D-12. | `Error: delegates cannot perform batch deletion on this node.` |

**Additional strings also in this TEST_CASE (defensive paths):**

| Trigger | Wording |
|---|---|
| Short-read ErrorResponse payload (<2 bytes) | `Error: node returned malformed response` |
| Unknown code (not 0x01-0x0B) | `Error: node rejected request (code 0x{XX})` (uppercase hex) |

**For PROTOCOL.md Plan 1 task:** copy this table into the `ErrorResponse`
section and extend the existing 0x01-0x06 table at
`db/PROTOCOL.md:459-467`. User-facing wording is the external reader's
compatibility contract — implementations of alternate clients should
match the wording so operator eyeballs see consistent strings across
tools.

## 6. Plan breakdown recommendation

Five plans. Parallelization note: Plans 1, 2 can run concurrently (pure
markdown, no shared files). Plan 3 depends on Plan 1 for cross-link anchors
(ARCHITECTURE.md links into PROTOCOL.md by section — must target real anchors
that Plan 1 created). Plan 4 depends on nothing and touches source. Plan 5
(D-14 comment hygiene) MUST run LAST because it touches the same source
files that Plan 4 edited — lock-stepping prevents merge conflicts.

### Plan 1 — db/PROTOCOL.md full rewrite

**Why its own plan:** Highest-traffic public doc, byte-level rigor required,
largest content volume (1386 lines → ~1700+ lines post-rewrite). External
client implementers' only reference. Touches only one file.

**Files touched:** `db/PROTOCOL.md`.

**Parallelism:** Can run in parallel with Plan 2. Independent from Plans 3-5.

**Tasks (rough):**
1. Rewrite Blob schema + canonical signing input + Sending a Data Message
   sections (SC#1 load-bearing).
2. Add new sections: namespace derivation, signer_hint, PUBK-first,
   owner_pubkeys DBI public shape, PUBK blob format, BlobWrite envelope.
3. Add NAME + BOMB sections.
4. Extend ErrorResponse code table with 0x07-0x0B (verbatim per §5).
5. Update sync Phase C for per-blob ns prefix (Phase 122 Pitfall #3).
6. Update Message Type Reference: add BlobWrite=64, mark Data=8 deprecated
   (or delete after Plan 4 lands D-11).
7. Update MetadataResponse pubkey field semantic (now signer_hint).
8. Update ListRequest/ListResponse notes for type filter (already partly
   present — reconcile with post-122 context).
9. Cross-link ARCHITECTURE.md anchors (forward references that Plan 3
   will target).

### Plan 2 — README.md + cli/README.md rewrites

**Why its own plan:** User-facing docs, distinct audience (pitch + user-
command reference), natural grouping. Both touch what the external operator
and end user see first.

**Files touched:** `README.md`, `cli/README.md`.

**Parallelism:** Can run in parallel with Plan 1. Depends on no other plan.

**Tasks (rough):**
1. `README.md`: refresh feature bullet list for v4.1.0, fix "62 message types"
   count, add cross-link to ARCHITECTURE.md + cli/README.md.
2. `cli/README.md`: add hello-world quickstart (D-03).
3. Add `put --name` / `get <name>` / `--replace` deep section (D-08).
4. Add `rm <h1> <h2> <h3>...` batched + CPAR cascade deep section (D-08).
5. Add chunked files brief section (D-08 brief).
6. Add pipelining brief section (D-08 brief).
7. Add auto-PUBK + first-write-error brief section referencing PROTOCOL.md
   error-code anchor (D-02 link, not content duplication).
8. Extend Commands table + Global Flags table (verified subcommand list in §1c).
9. Update `publish` description to explain PUBK-first relation.
10. Update Connection section with `--node` + config-file `default_node`.

### Plan 3 — db/ARCHITECTURE.md creation + db/README.md delta

**Why its own plan:** New file + tight edits to the operator doc. Logically
coupled because ARCHITECTURE.md and db/README.md both target the
operator/implementer audience and must not duplicate (D-02). Depends on
Plan 1 for cross-link anchors.

**Files touched:** `db/ARCHITECTURE.md` (new), `db/README.md`.

**Parallelism:** Sequence after Plan 1 completes (for anchor stability).
Can run in parallel with Plan 2.

**Tasks (rough):**
1. Create `db/ARCHITECTURE.md` per skeleton in §4. ~750 lines.
2. Update `db/README.md` Architecture paragraph to reference ARCHITECTURE.md
   and add one sentence each on post-122 signing model + PUBK-first.
3. Add 5 new Phase-118 config knobs to db/README.md Configuration section.
4. Add Phase-118 subcommands (`add-peer`, `remove-peer`, `list-peers`) to
   Usage section.
5. Update "62 message types" → "63" (or drop the number).
6. Add brief Features bullets for 117/119/120/121/122/123/124.

### Plan 4 — SC#6 pre-122 vestige cleanup + main.cpp:619 leak fix + dispatcher memo

**Why its own plan:** Touches source code (not docs). Well-defined scope:
4 specific things to delete. Can run independently of the doc plans because
it doesn't affect doc content. Runs BEFORE Plan 5 because Plan 5's comment-
hygiene pass will step on the same files.

**Files touched:** `cli/src/main.cpp`, `db/schemas/transport.fbs`,
`db/wire/transport_generated.h` (regenerated), `db/peer/message_dispatcher.cpp`,
`db/tests/net/test_protocol.cpp`, `db/tests/net/test_framing.cpp`,
`db/tests/net/test_connection.cpp`. ~7 files.

**Parallelism:** Sequence after Plans 1-3 if there's any chance the doc plans
will reference transport.fbs enum positions (they shouldn't, but safe
ordering). Independent from Plan 5 in parallelism terms except Plan 5 runs
strictly after.

**Tasks (rough):**
1. D-12: fix `cli/src/main.cpp:619` — replace `"batched BOMB tombstone (Phase
   123). Exit 2 if no targets given.\n"` with `"batched BOMB tombstone. Exit
   2 if no targets given.\n"`.
2. D-11: delete `Data = 8` row from `db/schemas/transport.fbs`.
3. D-11: regenerate `db/wire/transport_generated.h` via flatc.
4. D-11: migrate or delete the 16 test-site references to
   `TransportMsgType_Data` (11 in test_connection.cpp, 2 in test_framing.cpp,
   3 in test_protocol.cpp).
5. D-11: delete the `// TransportMsgType_Data direct-write branch was
   DELETED — no backward compat.` comment at
   `db/peer/message_dispatcher.cpp:1388`.
6. D-10: final scan for any lingering pre-122 field references in comments
   (`BlobData.namespace_id`, `BlobData.pubkey`, old signing input shape).
7. **Full test-suite run after** (the user's job per
   feedback_delegate_tests_to_user, but plan must flag it).

### Plan 5 — D-13 `// Phase N` strip + D-14 comment-hygiene pass

**Why its own plan:** Largest-scope, highest-risk (accidental code
deletion), and must run LAST because it touches every source file. Natural
to group D-13 and D-14 because the per-file read-decide-edit loop is the
same.

**Files touched:** 40-60 source + test files across `cli/src/`, `cli/tests/`,
`db/` subsystems.

**Parallelism:** Strictly serial, last.

**Tasks (rough):**
1. Commit per area: `cli/src/`, `cli/tests/`, `db/engine/`, `db/storage/`,
   `db/net/`, `db/peer/`, `db/sync/`, `db/wire/`, `db/crypto/`, `db/config/`,
   `db/tests/`. ~10-12 commits.
2. Per file: strip `// Phase N` prefixes; drop phase-only breadcrumbs;
   remove commented-out code blocks; leave load-bearing WHYs.
3. Scope cap: stick to the CLAUDE.md policy — if you find yourself rewriting
   comments to explain what code does, STOP (that's rewriting, not cleaning).
4. **Full test-suite run after** — accidental-deletion detection.
5. Build under sanitizer (ASAN at minimum) to surface any silent code drops.

## 7. Risks and pitfalls

### Doc-writing risks

- **D-02 violation (content duplication).** Easy to reflex-copy the same
  paragraph into multiple docs. Rule: if two docs want to say the same
  thing, one owns it and the other links. Enforcement: each plan's
  verification step should grep for large string overlaps between the 4
  target files.

- **"Doc says X, code says Y" — code wins.** Multiple places where this
  research found the doc and code drifted already: current `db/PROTOCOL.md`
  Blob schema still has 6 fields; code has 5. Current `db/README.md` claims
  "62 message types"; schema has 63. When the planner authors a task, the
  TASK must cite the source-code line the content is derived from (e.g.
  "blob shape per `db/schemas/blob.fbs`"). No assumed content from memory.

- **Anchor instability between plans.** Plan 3 depends on Plan 1's
  PROTOCOL.md anchors. If Plan 1 renames a section after Plan 3 lands,
  ARCHITECTURE.md links break. Mitigation: Plan 1 finishes before Plan 3
  starts the link-writing portion (inside Plan 3, anchors can be the LAST
  edit).

- **D-08 over-coverage.** The "deep" items (chunked/NAME/BOMB/signer_hint/
  auto-PUBK/error codes/BOMB cascade) expand naturally. Planner should set
  approximate line budgets per section to keep each doc reviewable.

- **`62 message types` count drift.** Both README.md and db/README.md cite
  this count. Post-D-11 the enum has 63 entries (0, 1-63, 64 — skipping 8
  = 63 live types). Recommendation: drop the number entirely from both docs
  and link to the schema for ground truth.

- **PROTOCOL.md has Mermaid diagrams already.** D-06 "ASCII only" applies
  to the NEW ARCHITECTURE.md. PROTOCOL.md's existing Mermaid at lines
  228-239 and 379-393 is not in scope for D-06. Verify with user if the
  planner wants to strip those too — not mandated by CONTEXT.md.

### Code-hygiene risks

- **D-14 accidental code deletion.** Commenting discipline of `// foo`
  vs `//foo` vs `//  foo` varies; a naive regex-strip can swallow the
  next line. **Mitigation:** manual review of every file, full test suite
  run after the pass.

- **D-14 scope creep.** User said "cleaned really well" but this is still
  Phase 125 scope. If the planner finds a file that needs fundamental
  restructuring (not comment pruning), log it as a deferred item and skip.
  Plan 5 is NOT a code-quality rewrite phase.

- **D-11 flatc regeneration.** Hand-editing `transport_generated.h` is
  forbidden. Plan 4 MUST specify the exact flatc invocation. Check at
  CMake time whether the generated header is checked in vs. generated at
  build time — `git ls-files db/wire/transport_generated.h` confirms it's
  checked in (git-tracked), so the regeneration must be committed.

- **D-11 test-site fallout.** `TransportMsgType_Data` appears at ~16 test
  sites. Some tests are genuinely exercising the old pre-122 branch and
  should be DELETED; some can be migrated to `TransportMsgType_BlobWrite`.
  Each site needs an individual call. Plan 4 should enumerate each site
  with the deletion-vs-migration decision before execution.

- **Commented-out code detection.** D-14 task default per user: delete on
  sight. Easy to miss when the comment block is formatted as a docstring
  (`/// ...`). Grep patterns: leading `//` followed by valid C++ tokens
  (semicolon, braces, function calls), ≥3 lines in a row. Manual review
  catches the remainder.

- **Regression risk from D-14.** Primary mitigation: full Catch2 suite
  green after the pass. Secondary: ASAN run catches any silent UB from
  lost code. Tertiary: the user's live-node E2E (running cdb against
  `192.168.1.73`) catches runtime regressions — per
  `feedback_delegate_tests_to_user`, this is the user's responsibility to
  re-run after Plan 5 completes.

### Scope risks

- **Additional doc targets discovered during research.** The user's CONTEXT.md
  scoped to 4 files. During this research I noted that
  `cli/BACKLOG.md` exists but is internal (like `.planning/`) — confirmed not
  public-facing. `dist/` has no README — correct per D-15 (no new docs). No
  other public-facing doc drift found.

- **`db/README.md` scope ambiguity.** CONTEXT.md D-01 says db/README.md
  "stays as a thin build + operator quickstart" AND Claude's Discretion says
  "Whether db/README.md gets touched at all in this phase or stays frozen."
  This research recommends TOUCHING db/README.md because Phase 118's 5 new
  config knobs + `add-peer`/`remove-peer`/`list-peers` subcommands genuinely
  belong there (the operator reads this file to configure the node).
  Treat as part of Plan 3.

- **Mermaid in PROTOCOL.md.** D-06 scopes "ASCII only" to ARCHITECTURE.md.
  PROTOCOL.md has existing Mermaid. Research recommendation: leave it alone —
  ripping it out is out of scope per CONTEXT.md D-15 (no CHANGELOG-style
  restructuring). If the planner or user wants uniformity, they can add it
  as a deferred item.

## Runtime State Inventory

Phase 125 is a docs + code-comment phase. No stored data is renamed, no live
services are reconfigured, no OS-level registrations embed phase numbers. The
only runtime-state-adjacent concern is:

| Category | Items Found | Action Required |
|----------|-------------|------------------|
| Stored data | None — docs + comments only. | — |
| Live service config | None. | — |
| OS-registered state | None. | — |
| Secrets/env vars | None — no secrets touched. | — |
| Build artifacts | `db/wire/transport_generated.h` is checked into git; after D-11 it must be regenerated via flatc and the regeneration committed. `build/` is gitignored — local stale builds must be re-cmake'd to pick up schema changes but that's standard workflow. | Plan 4 commits the regenerated header. Users run `cmake --build -j$(nproc)` (per user preference — never `--parallel`). |

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| flatc (FlatBuffers compiler) | Plan 4 (D-11) — regenerate transport_generated.h | ✓ (via CMake FetchContent after `cmake ..`) | built from in-tree FetchContent | Hand-editing generated headers is FORBIDDEN. If FetchContent hasn't built yet, user must `cmake ..` first. |
| git grep / ripgrep | Plan 5 scope scans (`// Phase N` enumeration) | ✓ assumed (used throughout this research) | — | — |
| Catch2 suite | Post-Plan-4 and Post-Plan-5 regression detection | ✓ already in-tree | — | — |

No missing dependencies block execution. No network calls needed — all content
sources are in-repo.

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Catch2 (already present in both `cli/tests/` and `db/tests/`) |
| Config file | `cli/tests/CMakeLists.txt` + `db/tests/CMakeLists.txt` |
| Quick run command | `cd build && ctest -j$(nproc) --output-on-failure` |
| Full suite command | Same — there is no "fast subset" distinction; full Catch2 is fast enough |
| Phase gate | Full suite green before declaring Plan 4 or Plan 5 complete |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DOCS-01 | PROTOCOL.md documents blob type indexing | manual review | — (prose) | N/A |
| DOCS-02 | PROTOCOL.md documents new ListRequest/ListResponse | manual review | — | N/A |
| DOCS-03 | README.md documents new node config + peer management | manual review | — | N/A |
| DOCS-04 | cli/README.md documents groups, import, chunking, pipelining | manual review | — | N/A |
| SC#1 (roadmap) | PROTOCOL.md documents post-122 wire format | manual review + `grep 'signer_hint' db/PROTOCOL.md` returns ≥5 hits; `grep 'BlobWrite' db/PROTOCOL.md` ≥3 hits; `grep 'owner_pubkeys' db/PROTOCOL.md` ≥2 hits; `grep 'PUBK-first' db/PROTOCOL.md` ≥2 hits | grep | N/A |
| SC#2 | PROTOCOL.md documents v4.1.0 features | `grep 'chunked\|CDAT\|CPAR' db/PROTOCOL.md` ≥3 hits; `grep 'ListRequest' db/PROTOCOL.md` ≥2 hits; `grep 'request_id' db/PROTOCOL.md` ≥2 hits | grep | N/A |
| SC#3 | README.md documents v4.1.0 features | `grep 'add-peer\|remove-peer' db/README.md` ≥2 hits; `grep 'blob_transfer_timeout\|strike_threshold' db/README.md` ≥2 hits | grep | N/A |
| SC#4 | cli/README.md covers every subcommand | checklist against `cli/src/commands.h` | manual | N/A |
| SC#5 | db/ARCHITECTURE.md exists with 8 DBIs + ingest + sync + strand | `test -f db/ARCHITECTURE.md && grep -c 'owner_pubkeys\|strand\|ingest' db/ARCHITECTURE.md` ≥3 hits | test -f + grep | N/A |
| SC#6 | No pre-122 field references in comments | `! grep -rE 'BlobData\.(namespace_id\|pubkey)' cli/src/ db/` returns empty; `! grep 'TransportMsgType_Data' db/ --include=*.cpp --include=*.h` returns empty (except generated header removed); `! grep '(Phase [0-9]+)' cli/src/main.cpp` returns 0 hits | grep | N/A |
| D-13 scope | No `// Phase N` breadcrumbs in source | `! grep -rE '// Phase [0-9]+' cli/src/ db/` returns 0 hits (test files still have tags — Catch2 `[phase122]` tags are NOT in scope; only `// Phase N` comments are) | grep | N/A |
| D-14 scope | No commented-out code blocks | manual per-file review; unit tests green after pass | Catch2 + manual | N/A |

### Sampling Rate

- **Per plan commit:** no code behavior changes in Plans 1-3 → no tests needed. Plan 4 + Plan 5: `ctest -j$(nproc)` after each commit area.
- **Per phase gate:** Full Catch2 suite green before `/gsd-verify-work`.
- **Live-node E2E:** Not required for Plans 1-3 (docs). Plan 4 and Plan 5 should re-run the Phase 124 live-node matrix against `192.168.1.73` since the schema/dispatcher/comments touch code paths that E2E exercised — but this is delegated to the user per `feedback_delegate_tests_to_user`.

### Wave 0 Gaps

- None. All test infrastructure exists:
  - `cli/tests/test_wire.cpp` has the `[error_decoder]` TEST_CASE — already running, already literal-equality.
  - `db/tests/` has full Catch2 coverage for schema, engine, storage, net, sync, peer.
  - Catch2 framework, CMake targets, test helpers all present.
- Post-D-11 fallout: some `db/tests/net/test_*.cpp` tests will require migration or deletion. Plan 4 handles this inside the plan itself — not a Wave 0 pre-requisite.

## Security Domain

`.planning/config.json` doesn't exist in this repo (no `/gsd` config scaffold
— checked manually). `security_enforcement` is absent, so by the default it
applies. However, Phase 125 is documentation + comment hygiene. No new code
paths, no new user inputs, no new auth surfaces, no new network endpoints.

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | no | (no auth changes) |
| V3 Session Management | no | (no session changes) |
| V4 Access Control | no | (no ACL changes) |
| V5 Input Validation | no | (no new inputs) |
| V6 Cryptography | no | (no crypto changes; doc-only) |

### Known Threat Patterns

None relevant to Phase 125. The documentation could THEORETICALLY describe
the protocol incorrectly (e.g. claim a nonce value that's wrong), but:

- The [error_decoder] TEST_CASE enforces CLI wording exactness — PROTOCOL.md's
  error table must match these literals, so the test catches doc-to-code drift
  for error wording.
- PROTOCOL.md claims about Blob schema / BlobWriteBody / BOMB format are
  derived directly from `db/schemas/*.fbs` and `db/wire/codec.h`. Misquoting
  would be caught by any external implementer who writes a compatible client
  (of which there are currently zero, per `feedback_no_python_sdk`).

One secondary concern: PROTOCOL.md is the spec by which OTHERS implement
chromatindb clients. If PROTOCOL.md omits the PUBK-first invariant, a
third-party implementation might miss it and produce blobs that get
rejected. Plan 1 has an explicit "add PUBK-first section" task for exactly
this reason — the documentation gap IS the security-adjacent risk.

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | `db/PROTOCOL.md` already-present Mermaid diagrams (lines 228-239, 379-393) stay as-is under D-06 scope | §2b, §7 | Low — if user wants them stripped, add as a task in Plan 1 |
| A2 | `db/README.md` should be touched in this phase (add Phase 118 knobs + subcommands) | §2c, §6 Plan 3 | Low — CONTEXT.md Claude's Discretion allows either; user can scope-down to leave frozen |
| A3 | Post-D-11 `transport_generated.h` regeneration uses in-tree flatc built via CMake FetchContent | §3c, §7 | Low — verified by file presence + standard FlatBuffers workflow |
| A4 | D-14 estimated scope: 10-15 files covers ~40% of repo LOC for the first pass | §3b | Low — if repo is cleaner than expected, plan finishes early; if dirtier, planner extends scope or splits across commits |
| A5 | Catch2 `[phaseXXX]` tags on TEST_CASEs are NOT in scope for D-13 (comments only; tags are identifiers) | §3a | Low — CONTEXT.md D-13 explicitly says "// Phase N historical breadcrumbs"; tags are not comments |
| A6 | MetadataResponse `pubkey` field post-122 is `signer_hint` (32 bytes) not the full 2592-byte pubkey | §1a, §2b | Medium — must be verified against `db/peer/message_dispatcher.cpp` MetadataResponse handler before Plan 1 task lands. If wrong, PROTOCOL.md update to this section is wrong. Verification: planner reads the handler source as first action of Plan 1 task 7. |
| A7 | The "(Phase 123)" leak at `cli/src/main.cpp:619` is the only user-visible string leak — grep for `\(Phase [0-9]+\)` across `cli/src/` confirms | §3c | Verified via grep — no other hits in `cli/src/*`. If user-visible string generation is built dynamically elsewhere, could miss one. |

All claims about wire-format byte shapes, enum values, magic constants, and
error wording are VERIFIED against the source code or [error_decoder]
TEST_CASE. Claims about scope sizing (~50-70% comment retention estimate) are
informed estimates; the planner can refine during execution.

## Open Questions

1. **MetadataResponse pubkey field semantic** (Assumption A6)
   - What we know: post-122 schema has `signer_hint:32` not inline pubkey; MetadataResponse at PROTOCOL.md:1034 still says "pubkey_len + pubkey bytes" (variable).
   - What's unclear: Did the MetadataResponse keep its variable-length pubkey output (by resolving signer_hint to full pubkey server-side for convenience)? Or did it switch to a fixed 32-byte signer_hint output?
   - Recommendation: Plan 1 first action: read `db/peer/message_dispatcher.cpp` MetadataResponse handler; whichever shape the code emits is canonical. Update PROTOCOL.md line 1034 to match.

2. **Uptime/rebuild-timestamp operator guidance in db/README.md**
   - What we know: Phase 124 Plan 05 retrospective noted the home-daemon-stale-process root cause ("cross-reference Uptime against rebuild timestamp, not just Version" — STATE.md).
   - What's unclear: Should this operator knowledge land in `db/README.md`'s Deployment section? CONTEXT.md specifics note says it "probably worth a sentence, Claude's discretion."
   - Recommendation: one-line addition to Deployment section in Plan 3 ("Note: after binary upgrade, confirm daemon process was restarted — `info` reads the running process image, not the on-disk binary"). Low cost, high operator value.

3. **D-13 scope boundary on `// CONC-04 (Phase 121)` comments**
   - What we know: comments like `// CONC-04 (Phase 121): post back to ioc_ before Storage access.` cite both a requirement ID (load-bearing) and a phase number (pure history).
   - What's unclear: Should D-13 strip just "(Phase 121)" (leaving "CONC-04:"), or should requirement IDs also go (they're another historical artifact)?
   - Recommendation: strip only "(Phase N)" / " Phase N " tokens; leave requirement IDs (CONC-04, D-05, SC#4) because they're semantically meaningful references to design decisions. Can be revisited if user wants more aggressive.

## Sources

### Primary (HIGH confidence)

- `cli/src/wire.h` — post-122 BlobData shape (signer_hint:32), MsgType enum (BlobWrite=64, Data=8 absent), NAME/BOMB/PUBK/CDAT/CPAR magics, type_label + is_hidden_type helpers, chunked constants
- `cli/src/commands.h` — canonical cdb subcommand list (verified)
- `cli/src/main.cpp:619` — the "(Phase 123)" leak (D-12 target, verified)
- `cli/tests/test_wire.cpp:658-716` — `[error_decoder]` TEST_CASE with literal D-05 error strings (the ground truth for D-09)
- `db/schemas/blob.fbs` — 5-field post-122 Blob
- `db/schemas/transport.fbs` — BlobWrite=64 added, Data=8 still present (D-11 target), BlobWriteBody
- `db/storage/storage.h:95-121` — 8 DBIs listed, strand-confinement contract, STORAGE_THREAD_CHECK citation
- `db/engine/engine.cpp` — ingest pipeline step numbering, BOMB ttl=0 invariant (line 161), delegate-BOMB reject (line 311), BOMB cascade (line 398)
- `db/wire/codec.h` — NAME_MAGIC (line 160), BOMB_MAGIC (line 191), predicates (is_pubkey_blob, is_tombstone, is_delegation, is_bomb)
- `db/peer/message_dispatcher.cpp:1388` — memo to delete (D-11)
- `.planning/phases/125-docs-update-for-mvp-protocol/125-CONTEXT.md` — 15 locked decisions (D-01..D-15)
- `.planning/phases/122-*/*-SUMMARY.md` — 7 plans' summaries (signer_hint, PUBK-first, owner_pubkeys)
- `.planning/phases/123-*/*-SUMMARY.md` — NAME + BOMB design evidence
- `.planning/phases/124-*/124-E2E.md` + `124-05-SUMMARY.md` — Phase 124 live-E2E evidence (auto-PUBK, error codes, BOMB cascade)
- `.planning/phases/121-*/121-VERIFICATION.md` — TSAN ship-gate evidence cited by D-05
- `.planning/STATE.md` — Phase 124 plan-by-plan decisions, the Phase 123 D-15 FLAG (same-second BOMB tiebreak), home-daemon-stale-process retrospective
- `.planning/ROADMAP.md` — Phase 125 entry with SC#1-SC#6
- `.planning/PROJECT.md` — product vision, 60+ Key Decisions for context

### Secondary (MEDIUM confidence — inferred from code reading)

- `db/ARCHITECTURE.md` ~750-line skeleton in §4 — inferred by scaling the section counts against code reality (8 DBIs worth 100 lines, 11-step ingest 80 lines, etc.)
- D-14 "50-70% of comment lines are load-bearing" estimate — extrapolated from the 8-file sample with the engine.cpp / storage.cpp high-density cases

### Tertiary (LOW confidence — none)

None — every claim in this research is grounded in a file in this repo.

## Metadata

**Confidence breakdown:**

- Content-coverage matrix: HIGH — every row sourced to a specific file/line in the repo
- Existing doc audit: HIGH — line counts via `wc -l`, last-touch via `git log`, stale content grepped
- Code-comment cleanup inventory: HIGH for count (grep-verified); MEDIUM for per-file judgment (samples confirm the pattern, full pass will surface corner cases)
- ARCHITECTURE.md skeleton: HIGH for contents (source-derived); MEDIUM for line-count sizing (informed estimate)
- Error-code table: HIGH — verbatim from the [error_decoder] TEST_CASE
- Plan breakdown: HIGH for structure (natural scope boundaries); MEDIUM for file counts (Plan 5 touches 40-60 files but the exact list only emerges during execution)
- Risks: HIGH — the D-02 duplication risk and D-14 accidental-deletion risk are real and named

**Research date:** 2026-04-21
**Valid until:** 2026-05-15 (3 weeks — repo is slow-moving but Phase 125 execution itself will shift state; regenerate if Phase 125 is started after this date)

## RESEARCH COMPLETE
