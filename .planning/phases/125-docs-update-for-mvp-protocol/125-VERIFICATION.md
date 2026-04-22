---
phase: 125-docs-update-for-mvp-protocol
verified: 2026-04-22T07:45:00Z
status: passed
score: 6/6
overrides_applied: 0
re_verification:
  previous_status: none
  previous_score: null
  gaps_closed: []
  gaps_remaining: []
  regressions: []
---

# Phase 125: MVP Documentation Update — Verification Report

**Phase Goal:** Rewrite PROTOCOL.md, README.md, cli/README.md, and db/ARCHITECTURE.md to describe the final v1 wire format, signing canonical form, storage model, and user workflows. Any doc that contradicts the shipping protocol is a bug.

**Verified:** 2026-04-22T07:45:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths (ROADMAP Success Criteria)

| # | Truth (SC) | Status | Evidence |
|---|-----------|--------|----------|
| 1 | PROTOCOL.md fully documents post-122 blob wire format, signing canonical form, namespace derivation, PUBK-first, signer_hint, owner_pubkeys DBI, NAME + BOMB magics | VERIFIED | `db/PROTOCOL.md` (1622 lines). Blob Schema at L245, Canonical Signing Input at L263, Namespace Derivation at L312, signer_hint Semantics at L328, owner_pubkeys DBI at L340, PUBK Blob Format at L357, PUBK-First Invariant at L386, NAME Blob Format at L411, BOMB Blob Format at L440. `signer_hint` appears 26 times; NAME magic `0x4E414D45` and BOMB magic `0x424F4D42` both present; error codes 0x07–0x0B present with verbatim CLI wording matching `cli/tests/test_wire.cpp [error_decoder]` literals byte-for-byte |
| 2 | PROTOCOL.md documents v4.1.0 features (blob type indexing, ListRequest/ListResponse, chunked CDAT/CPAR, request pipelining) | VERIFIED | ListRequest / ListResponse at L1017; `request_id` documented (pipelining depth noted); CDAT/CPAR referenced (4 mentions); Chunked Transport Framing at L51; BlobWrite=64 envelope documented in §Sending a Blob |
| 3 | README.md describes all v4.1.0 features at user/operator level incl. config fields + peer management | VERIFIED | `README.md` (106 lines). ML-DSA-87, libmdbx, chunked, peer all present. `db/README.md` (503 lines) contains all 5 Phase-118 knobs (`blob_transfer_timeout`, `sync_timeout`, `pex_interval`, `strike_threshold`, `strike_cooldown`) and 3 subcommands (`add-peer`, `remove-peer`, `list-peers`). "62 message types" stale count removed from all 5 public docs |
| 4 | cli/README.md covers every `cdb` subcommand: keygen, publish, put (--name), get (by hash/name), rm (batched), ls (filters), contact/group, chunked, pipelining | VERIFIED | `cli/README.md` (288 lines). All 11 subcommands present (keygen, publish, put, get, rm, ls, contact, delegate, revoke, reshare, info). Flags `--name`, `--type`, `--replace`, `--host`, `--node` present. Hello World quickstart (5-command D-03) present. Deep sections: Mutable Names + Batched Deletion/CPAR Cascade. Brief sections: Chunked Large Files + Request Pipelining + Auto-PUBK + First-Write Errors. Cross-links to PROTOCOL.md#bomb-blob-format, #name-blob-format, #pubk-first-invariant, #errorresponse-type-63, #chunked-transport-framing, #transport-layer — all resolve |
| 5 | db/ARCHITECTURE.md exists; documents 8 DBIs, ingest pipeline, sync, strand model | VERIFIED | `db/ARCHITECTURE.md` exists (879 lines, NEW file). All 8 DBIs tabulated (blobs, sequence, expiry, delegation, tombstone, cursor, quota, owner_pubkeys). Top-level structure Overview → Storage Layer → Engine Layer → Net Layer → Configuration and Subsystems (D-04 top-down). Strand model subsection with 17 STORAGE_THREAD_CHECK/strand references (D-05). 11-step ingest pipeline table. BOMB cascade subsection with BlobWrite=64 wire-path rule. 25 cross-links to PROTOCOL.md anchors, all 20 unique anchors resolve to real headings |
| 6 | Inline code comments referencing removed fields (`namespace_id` in wire format, per-blob pubkey embedding, old signing input shape) removed or updated | VERIFIED | Source scan clean: `BlobData.namespace_id\b` = 0 hits, `BlobData.pubkey\b` = 0 hits, `MsgType::Data\b` = 0 hits, `namespace_id \|\| data \|\| ttl \|\| timestamp` = 0 hits across `cli/src/` and `db/` (`*.cpp`/`*.h`). Additionally: `TransportMsgType_Data = 8` deleted from `db/schemas/transport.fbs`; `db/wire/transport_generated.h` regenerated via flatc (no `TransportMsgType_Data` remains); 16 test-site references migrated to `TransportMsgType_BlobWrite`; memo comment at `message_dispatcher.cpp:1388` deleted; 313 `// Phase N` breadcrumbs stripped (0 remain, only preserved survivor is `SECTION("... (Phase 115)")` Catch2 identifier — intentional per D-13 scope) |

**Score:** 6/6 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|---------|--------|---------|
| `db/PROTOCOL.md` | Byte-level wire protocol spec, post-122/123/124 accurate | VERIFIED | 1622 lines; contains signer_hint (26×), BlobWrite, BlobWriteBody, PUBK-first, NAME/BOMB magics, full error-code table 0x01-0x0B with verbatim CLI wording |
| `README.md` | Top-level project pitch + v4.1.0 features + doc cross-links | VERIFIED | 106 lines (~1 screen per D-01); cross-links to PROTOCOL.md, ARCHITECTURE.md, db/README.md, cli/README.md present; ML-DSA-87, libmdbx, chunked, peer all present |
| `cli/README.md` | cdb command reference + hello-world + v4.1.0 features | VERIFIED | 288 lines; Hello World 5-command quickstart; all 11 subcommands; deep sections for Mutable Names + Batched Deletion/CPAR; brief sections for Chunked Files, Pipelining, Auto-PUBK; verbatim 0x07 CLI wording present |
| `db/ARCHITECTURE.md` | Internal implementation doc (NEW) | VERIFIED | 879 lines; Storage → Engine → Net top-down per D-04; 8 DBIs; strand model with ASCII diagram; 11-step ingest pipeline; BOMB cascade; 25 cross-links to PROTOCOL.md |
| `db/README.md` | Operator guide with Phase 118 knobs + subcommands | VERIFIED | 503 lines; 5 Phase-118 knobs present; add-peer/remove-peer/list-peers present; ARCHITECTURE.md cross-link present; "62 message types" stale count removed |
| `db/schemas/transport.fbs` | TransportMsgType enum without Data = 8 | VERIFIED | Inspected lines 5-70: enum jumps from `Goodbye = 7` directly to `SyncRequest = 9`; `BlobWrite = 64` present as final entry |
| `db/wire/transport_generated.h` | flatc-regenerated header without TransportMsgType_Data | VERIFIED | 134 `TransportMsgType_` references, none are `TransportMsgType_Data`; header shows `TransportMsgType_MAX = TransportMsgType_BlobWrite` and `TransportMsgType_None, TransportMsgType_Goodbye, TransportMsgType_SyncRequest, TransportMsgType_BlobWrite` in Names/Values tables |
| `cli/src/main.cpp` | User-visible help text free of (Phase N) leaks | VERIFIED | Line 619 reads `"batched BOMB tombstone. Exit 2 if no targets given.\n"` — no `(Phase 123)` token. Repo-wide grep for `"[^"]*\(Phase [0-9]+\)[^"]*"` in cli/src/ returns 0 hits |

### Key Link Verification

| From | To | Via | Status | Details |
|------|-----|-----|--------|---------|
| `db/PROTOCOL.md` | `db/schemas/blob.fbs` | Prose references 5-field Blob shape matching `.fbs` ground truth | WIRED | Blob Schema §L245 matches blob.fbs (signer_hint, data, ttl, timestamp, signature) |
| `db/PROTOCOL.md` | `db/schemas/transport.fbs` | Message Type Reference matches TransportMsgType enum | WIRED | `BlobWrite = 64` row in Message Type Reference; `Data = 8` marked DELETED with external-client guidance |
| `db/PROTOCOL.md` | `cli/src/error_decoder.cpp` via `[error_decoder]` test | Error-code table wording byte-identical to CLI literals | WIRED | All 5 strings (0x07-0x0B) in PROTOCOL.md §ErrorResponse match the REQUIRE-equality literals in `cli/tests/test_wire.cpp:673-694` verbatim |
| `db/ARCHITECTURE.md` | `db/PROTOCOL.md` anchors | 25 cross-links by section anchor (D-02/D-07 zero duplication) | WIRED | All 20 unique anchors resolve to real headings (verified against PROTOCOL.md heading table) |
| `cli/README.md` | `db/PROTOCOL.md` anchors | User-guide links to wire spec for detail | WIRED | 6 anchors: #bomb-blob-format, #chunked-transport-framing, #errorresponse-type-63, #name-blob-format, #pubk-first-invariant, #transport-layer — all resolve |
| `README.md` | All 4 other public docs | Documentation section cross-links | WIRED | PROTOCOL.md, ARCHITECTURE.md, db/README.md, cli/README.md all referenced |
| `db/schemas/transport.fbs` | `db/wire/transport_generated.h` | flatc --cpp --gen-object-api regeneration | WIRED | Confirmed diff shape: enum row removed, `EnumValuesTransportMsgType[65] → [64]`, value list entry removed, name table slot `"Data" → ""` |
| `db/README.md` | `db/ARCHITECTURE.md` | Operator doc cross-link to implementation detail | WIRED | `grep -q "ARCHITECTURE\.md" db/README.md` passes |

### Data-Flow Trace (Level 4)

N/A — Phase 125 is documentation-only (plus one schema-regen + test-site migration + comment hygiene). No artifacts render dynamic runtime data; all verification is static code/doc content.

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| Test baseline preserved post-schema-regen (cli) | `./cli/build/tests/cli_tests --reporter compact` | 197614 assertions / 98 cases (user-confirmed 2026-04-22) | PASS |
| Test baseline preserved post-schema-regen (node peer) | `./build/db/db/chromatindb_tests "[peer]" --reporter compact` | 506 assertions / 77 cases (user-confirmed 2026-04-22) | PASS |
| Both binaries compile cleanly post-comment-hygiene | `cmake --build cli/build --target cdb; cmake --build build --target chromatindb` | Both built cleanly per 125-05-SUMMARY.md | PASS |
| D-13 repo-wide breadcrumb grep | `grep -rEq "// Phase [0-9]+\|/// Phase [0-9]+" cli/src/ cli/tests/ db/ --include="*.cpp" --include="*.h" --include="*.fbs"` | 0 hits (confirmed by live grep) | PASS |
| D-12 user-string leak grep | `grep -rEq "\"[^\"]*\(Phase [0-9]+\)[^\"]*\"" cli/src/` | 0 hits (confirmed) | PASS |
| D-11 schema regen propagated | `grep -q "TransportMsgType_Data" db/wire/transport_generated.h` | 0 hits (confirmed) | PASS |
| Preservation of load-bearing WHY | `grep -q "CONC-04" db/engine/engine.cpp && grep -qi "AEAD nonce" db/net/connection.cpp && grep -qi "mmap" db/storage/storage.cpp && grep -q "ForceDefaults" cli/src/wire.cpp` | All 4 PRESENT (confirmed) | PASS |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| DOCS-01 | 125-01, 125-04, 125-05 | PROTOCOL.md updated with blob type indexing wire format | SATISFIED | PROTOCOL.md L245-1622 documents post-122 5-field Blob, blob_type indexing at L1017, ListRequest/ListResponse types 33-34 documented; REQUIREMENTS.md:59 marked complete 2026-04-22 |
| DOCS-02 | 125-01, 125-04, 125-05 | PROTOCOL.md updated with new ListRequest/ListResponse format | SATISFIED | ListRequest §L1017 documents TYPE-01..TYPE-04 type-filter; PIPE-01..PIPE-03 pipelining with default depth 8; REQUIREMENTS.md:60 marked complete 2026-04-22 |
| DOCS-03 | 125-03, 125-05 | README.md updated with all new node config fields + peer management | SATISFIED | db/README.md:503 documents 5 Phase-118 knobs (blob_transfer_timeout, sync_timeout, pex_interval, strike_threshold, strike_cooldown) and 3 subcommands (add-peer, remove-peer, list-peers); top-level README.md:106 updated with v4.1.0 feature bullets |
| DOCS-04 | 125-02, 125-05 | cli/README.md updated with groups, import, chunking, pipelining | SATISFIED | cli/README.md:288 has Hello World, Mutable Names, Batched Deletion/CPAR, Chunked Large Files, Request Pipelining, Auto-PUBK sections; all subcommands in Commands table including contact/group management |

Cross-referenced against REQUIREMENTS.md:59-62: all 4 DOCS requirements explicitly marked complete (2026-04-22) and mapped to Phase 125 Plan 01 in the requirement index table (REQUIREMENTS.md:124-127).

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `db/tests/net/test_framing.cpp` | 187 | `SECTION("MAX_BLOB_DATA_SIZE is 500 MiB (Phase 115)")` | Info | Intentional survivor — Catch2 SECTION label is an identifier (user-visible test output), not a comment. Explicitly carved out by D-13 scope per 125-05-SUMMARY.md auto-fix #2. Not a blocker. |
| `db/PROTOCOL.md` | 310, 1122 | References to `TransportMsgType_Data = 8 … DELETED` | Info | Intentional deprecation notice for external client implementers. Explicitly documented design per 125-01 key-decisions ("Data=8 row retained as DELETED to prevent wrong-path implementation by external clients"). Not a blocker. |

No blockers or warnings found. No TODO/FIXME/placeholder comments introduced. No commented-out code blocks (D-14 sweep confirmed clean per 125-05-SUMMARY.md).

### Human Verification Required

None. All truths are verifiable programmatically via grep/file inspection. The behavioral tests that would require human judgement (UI appearance, real-time behavior, external service integration) do not apply to a documentation-only phase. The one runtime-behavioral concern — that the schema regen in Plan 04 preserves test suite behavior — was addressed by user running both test binaries and confirming baselines (cli_tests 197614/98, chromatindb_tests [peer] 506/77) at 2026-04-22.

### Deferred Items

None. All 6 ROADMAP Success Criteria satisfied in-phase. No items routed to later phases.

### Gaps Summary

No gaps. All 6 ROADMAP Success Criteria verified against the codebase:

- **SC#1** (PROTOCOL.md post-122 wire format): verified by heading presence, 26 `signer_hint` occurrences, NAME + BOMB magics with invariants, full 0x01-0x0B error-code table with CLI-verbatim wording
- **SC#2** (PROTOCOL.md v4.1.0 features): verified by ListRequest/ListResponse documentation, CDAT/CPAR references, request_id pipelining notes, chunked transport framing section
- **SC#3** (README.md + db/README.md operator-level): verified by 5 Phase-118 config knobs documented, 3 peer-management subcommands, "62 message types" removed from all 5 docs
- **SC#4** (cli/README.md every subcommand): verified by presence of all 11 `cdb` subcommands, Hello World D-03 quickstart, deep sections for mutable names + batched deletion/CPAR cascade, brief sections for chunked files + pipelining + auto-PUBK
- **SC#5** (db/ARCHITECTURE.md exists): verified — new 879-line file with top-down Storage → Engine → Net structure, all 8 DBIs, strand model with ASCII diagram (D-05), 11-step ingest pipeline, 25 cross-links to PROTOCOL.md (all anchors resolve)
- **SC#6** (inline comments for removed fields): verified — all four D-10 absence greps return zero hits; additionally `TransportMsgType_Data = 8` removed from schema + generated header + 16 test sites (D-11), `(Phase 123)` token removed from cli/src/main.cpp:619 (D-12), 313 `// Phase N` breadcrumbs stripped across 49 files (D-13), D-14 obvious-comment pass completed on 12 high-density files. All load-bearing WHY annotations (CONC-04, AEAD nonce, MDBX mmap, ForceDefaults, 11-step ingest markers, 20+ requirement IDs) preserved.

Scope-expansion beyond literal SC#6 (CONTEXT D-13 + D-14) was executed as user-directed and documented in 125-05-SUMMARY.md. User confirmed full test suite still green post-hygiene-pass (2026-04-22 close-out).

---

*Verified: 2026-04-22T07:45:00Z*
*Verifier: Claude (gsd-verifier)*
