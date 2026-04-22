---
phase: 125-docs-update-for-mvp-protocol
plan: 02
subsystem: docs
tags: [docs, readme, cli-user-guide, pubk-first, bomb, name, cpar, chunked, pipelining]

# Dependency graph
requires:
  - phase: 125-docs-update-for-mvp-protocol
    plan: 01
    provides: "db/PROTOCOL.md anchors for cross-linking (#pubk-first-invariant, #name-blob-format, #bomb-blob-format, #errorresponse-type-63, #chunked-transport-framing, #transport-layer)"
  - phase: 124-cli-adaptation-to-new-mvp-protocol
    provides: "D-05 error-decoder wording (error_decoder.cpp 0x07-0x0B), D-06 CPAR cascade behavior, auto-PUBK on all owner-write commands"
  - phase: 123-tombstone-batching-and-name-tagged-overwrite
    provides: "NAME magic, BOMB magic, --name / --replace semantics, D-15 1-second tiebreak quirk"
provides:
  - "Public README + cdb user guide refreshed against post-122/123/124 codebase"
  - "Five new cli/README sections users need: Hello World, Mutable Names, Batched Deletion + CPAR Cascade, Chunked Large Files, Request Pipelining, Auto-PUBK + First-Write Errors"
  - "Cross-reference set from cli/README into PROTOCOL.md anchors; no duplication of byte-level content (D-02)"
affects: [125-03]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Anchor-based cross-referencing from the user-facing guide (cli/README) into the byte-level spec (db/PROTOCOL.md); D-02 zero-duplication enforced by linking rather than restating"
    - "Verbatim CLI error-wording quoted once in cli/README (auto-PUBK + owned-by-different-key) with explicit note that [error_decoder] TEST_CASE guards drift"
    - "Command-table rows carry in-tree flags verified against cli/src/commands.h + cli/src/main.cpp argv parsing (source of truth over memory)"

key-files:
  created:
    - .planning/phases/125-docs-update-for-mvp-protocol/125-02-SUMMARY.md
  modified:
    - README.md
    - cli/README.md

key-decisions:
  - "README stays light-refresh (~106 lines, single screen) per D-01; cli/README absorbs the substantive rewrite (152 -> 288 lines) per D-08"
  - "cli/README cross-links to PROTOCOL.md#errorresponse-type-63 (the actual anchor) rather than the plan-specified #errorresponse — Plan 01 landed the heading as 'ErrorResponse (Type 63)' and GitHub slugifies the full text"
  - "No --chunk-size flag documented: the CLI does not expose one (CHUNK_SIZE_BYTES_DEFAULT=16 MiB in cli/src/wire.h is compile-time); documenting a non-existent flag would mislead readers. Deviation from plan's Task 2.C — in-scope per the 'verify; omit if not exposed' guard"
  - "Config-file section rewritten around default_node + named-nodes shape (verified against cli/src/main.cpp:184-250). The prior prose's {host, port} form is still honoured by the binary but is no longer the documented default"

requirements-completed:
  - DOCS-04

# Metrics
duration: "4m"
completed: 2026-04-22
---

# Phase 125 Plan 02: README.md + cli/README.md v4.1.0 Refresh — Summary

**Refreshed the two shipping-public user-facing docs (`README.md` top-level pitch and `cli/README.md` cdb user guide) against the post-122/123/124 codebase: added a 5-command hello-world, deep sections for mutable names and batched deletion/CPAR cascade, brief sections for chunked large files, request pipelining, and auto-PUBK first-write errors; all with cross-links into `db/PROTOCOL.md` anchors and zero byte-level content duplication.**

## Performance

- **Duration:** ~4 min wall-clock (2026-04-22T03:02:24Z → 03:07:03Z)
- **Tasks:** 4 (all autonomous, no checkpoints)
- **Files modified:** 2 (`README.md`: 97 → 106 lines; `cli/README.md`: 152 → 288 lines; +145 lines net)

## Accomplishments

- **`README.md` (+9 lines net):** Feature bullets now surface all v4.1.0 shipping capabilities (PUBK-first registration, mutable names, batched deletion, chunked large files, request pipelining, blob type indexing, peer management). Stale "62 message types" text replaced with a schema-directory pointer. Documentation section now cross-links all four public docs (PROTOCOL.md, ARCHITECTURE.md forward-ref, db/README.md, cli/README.md) with one-line audience descriptors per D-01. "Building Your Own Client" updated to describe the `BlobWrite = 64` / `BlobWriteBody{target_namespace, blob}` envelope and the 5-field Blob shape.
- **`cli/README.md` (+136 lines net):** Six new sections and two overhauled sections:
  - **Hello World (new, per D-03)**: 5-command quickstart, ~15 lines, with inline cross-link to PUBK-First Invariant and the Auto-PUBK section below.
  - **Config File (rewritten)**: Now documents `default_node` + named-nodes map per `cli/src/main.cpp:184-250` (the legacy `{host, port}` form is still honoured by the binary, noted in a sentence).
  - **Connection (extended)**: Explicit host-resolution precedence list (CLI flag → `--node` → `default_node` → legacy → built-in port 4200).
  - **Commands table (rewritten)**: Every `cdb` subcommand from `cli/src/commands.h` listed with per-command flag surface. `put`'s `--share/--ttl/--stdin/--name/--replace`, `get`'s by-hash / by-name / `--all --from` modes, `ls`'s `--raw/--type`, `rm`'s single-vs-batched branches, `delegate/revoke` target forms (contact | `@group` | pubkey-file) all present.
  - **Global Flags table (rewritten)**: Matches `ConnectOpts` in `cli/src/main.cpp:74-80` plus `--node` / `--host`. Noted that flags can appear in any position.
  - **Mutable Names (new, DEEP per D-08)**: `put --name` / `get <name>` / `--replace` semantics, resolver rules (timestamp DESC, blob_hash DESC tiebreak), D-15 1-second tiebreak quirk boxed as a Known Quirk subsection, the chunked-file + `--name` rejection documented.
  - **Batched Deletion and CPAR Cascade (new, DEEP per D-08)**: Single-vs-batched `rm` with example output, the CPAR → CDAT cascade (live-verified 48-chunk example from Phase 124 E2E), and the three user-visible BOMB error codes (`0x09`, `0x0A`, `0x0B`) with verbatim decoder wording from `cli/src/error_decoder.cpp:47-52`.
  - **Chunked Large Files (new, BRIEF per D-08)**: 16 MiB CDAT + CPAR manifest, 400 MiB threshold, 1 TiB cap, no `--chunk-size` flag (explicitly noted to prevent doc-to-binary drift).
  - **Request Pipelining (new, BRIEF per D-08)**: `Connection::kPipelineDepth = 8`, no user-visible flag, links to PROTOCOL.md#transport-layer for the `request_id` correlation field.
  - **Auto-PUBK and First-Write Errors (new, BRIEF per D-08)**: Verbatim `0x07` wording (`"namespace not yet initialized on node <host>. Auto-PUBK failed; try running 'cdb publish' first."`) and `0x08` wording (`"namespace <ns-short> is owned by a different key on node <host>. Cannot write."`) quoted. Full 0x07–0x0B table deferred to PROTOCOL.md per D-02; the [`error_decoder`] TEST_CASE is called out as the drift-detector.
  - **`publish` description (rewritten)**: Mentions PUBK-first relation with cross-link; notes auto-PUBK fallback.

## Task Commits

Each task was committed atomically against `master`:

1. **Task 1: README.md refresh** — `339ca0a9` (docs) — Feature bullets for v4.1.0 shipping features; Documentation section with 4-file cross-links; dropped "62 message types"; Building-Your-Own-Client updated for BlobWrite=64.
2. **Task 2: cli/README hello-world + commands + flags** — `9a1bca17` (docs) — Hello World section, rewritten Commands table + Global Flags table against `cli/src/commands.h` and `ConnectOpts`, rewritten Config File + Connection sections, publish description.
3. **Task 3: cli/README deep sections (names + batched rm)** — `6aff293b` (docs) — Mutable Names + Batched Deletion sections with examples and verbatim 0x09/0x0A/0x0B error wording; 4 anchor cross-links to PROTOCOL.md.
4. **Task 4: cli/README brief sections + README 'chunked' case-fix** — `5f93842e` (docs) — Chunked Large Files + Request Pipelining + Auto-PUBK sections; README bullet reworded for lowercase "chunked" (VALIDATION.md D1 grep is case-sensitive).

**Plan metadata commit:** (pending — final commit after this SUMMARY write + STATE/ROADMAP updates.)

## Files Created/Modified

- `README.md` — 97 → 106 lines (+9 net). Feature bullets, Documentation section, Building-Your-Own-Client, Quick Start pointer to cli/README#hello-world.
- `cli/README.md` — 152 → 288 lines (+136 net). Six new sections + two overhauled sections + extended Commands/Global Flags tables.
- `.planning/phases/125-docs-update-for-mvp-protocol/125-02-SUMMARY.md` (this file).

## Cross-Reference Inventory

cli/README.md links to the following PROTOCOL.md anchors (all established by Plan 01):

| Anchor | Referenced From |
|--------|-----------------|
| `#pubk-first-invariant` | Hello World, Commands table (publish row) |
| `#name-blob-format` | Mutable Names |
| `#bomb-blob-format` | Batched Deletion and CPAR Cascade |
| `#errorresponse-type-63` | Batched Deletion, Auto-PUBK and First-Write Errors |
| `#chunked-transport-framing` | Chunked Large Files |
| `#transport-layer` | Request Pipelining |
| `#storing-a-blob` | README.md Building-Your-Own-Client |

README.md links to the four public docs (`db/PROTOCOL.md`, `db/ARCHITECTURE.md` forward-ref, `db/README.md`, `cli/README.md`) plus `db/schemas/` and `cli/README.md#hello-world`.

## Validation Grep Results (VALIDATION.md D1/D2)

**D1 presence greps (ALL PASS):**

- `grep -qE "ml-dsa-87|ML-DSA-87" README.md` → PASS
- `grep -q "libmdbx" README.md` → PASS
- `grep -q "chunked" README.md` → PASS (case-sensitive; lowercase added in Task 4)
- `grep -q "peer" README.md` → PASS
- Every cdb subcommand in cli/README.md (`keygen`, `publish`, `put`, `get`, `rm`, `ls`, `contact`, `delegate`, `revoke`, `reshare`, `info`) → PASS
- `--name`, `--type`, `--replace` flag mentions in cli/README.md → PASS
- Hello World section heading in cli/README.md → PASS

**D2 absence greps (ALL PASS):**

- `! grep -qF "62 message types" README.md cli/README.md` → PASS
- `! grep -qE "blob\.namespace_id\s*=" README.md cli/README.md` → PASS
- `! grep -qE "blob\.pubkey\.assign" README.md cli/README.md` → PASS
- `! grep -qE "MsgType::Data\b" README.md cli/README.md` → PASS
- `! grep -qE '"\(Phase [0-9]+\)"' README.md cli/README.md` → PASS (no user-string phase tokens)
- `! grep -qiE "Phase [0-9]+ added|Phase [0-9]+ introduced" README.md cli/README.md` → PASS (no phase-number leaks in prose)

## Decisions Made

Decisions are enumerated in frontmatter `key-decisions`. The four load-bearing ones:

- **README stays light-refresh per D-01.** ~1-screen top-level pitch. All deep user-guide content lives in cli/README.md. 9-line net growth; no rewrite of Crypto Stack, Two-Node Sync, Dependencies, License.
- **Anchor fix: `#errorresponse-type-63`, not `#errorresponse`.** Plan 01 landed the PROTOCOL.md heading as "ErrorResponse (Type 63)" and GitHub's slugifier produces `errorresponse-type-63`. Used the actual anchor; the plan's spec string was a placeholder.
- **No `--chunk-size` flag documented.** Verified against `cli/src/main.cpp:382-443` (put parser) — no `--chunk-size` case exists. Chunk size is a compile-time constant in `cli/src/wire.h:301`. Documenting a non-existent flag per the plan's literal text (Task 2.C) would have misled readers; the plan itself explicitly permitted omission ("if exposed in `cli/src/main.cpp` — verify; omit if not exposed"). Noted explicitly in the Chunked Large Files section so readers see the fixed value.
- **Config File shape rewritten.** The old prose documented `{"host": "...", "port": ...}`; the binary prioritizes `default_node` + `nodes` map. Both still work (legacy fallback in `main.cpp:184-250`). Documented the modern shape as primary; noted legacy briefly.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Documented anchor `#errorresponse-type-63`, not `#errorresponse`**

- **Found during:** Task 2 + Task 3 cross-link drafting.
- **Issue:** The plan's `<interfaces>` block and success criteria reference `PROTOCOL.md#errorresponse`, but Plan 01's landed heading "ErrorResponse (Type 63)" slugifies to `errorresponse-type-63`.
- **Fix:** Used the actual slug in every cross-link. The VALIDATION.md plan-level success criteria uses regex `PROTOCOL\.md#` which matches both; no VALIDATION breakage.
- **Files modified:** `cli/README.md` (3 link sites).
- **Commit:** folded into `6aff293b` (Task 3) and `5f93842e` (Task 4).

**2. [Rule 1 - Bug] README lowercase "chunked" missing after Task 1**

- **Found during:** Post-Task-4 VALIDATION.md D1 grep sweep.
- **Issue:** README feature bullet used "Chunked large files" (capital C); VALIDATION.md grep is `grep -q "chunked" README.md` (case-sensitive).
- **Fix:** Reworded the bullet's tail from "reassembly on download is transparent" to "chunked reassembly on download is transparent". Semantic content unchanged.
- **Files modified:** `README.md`.
- **Commit:** folded into `5f93842e` (Task 4) alongside the brief cli/README sections.

**3. [Rule 2 - Missing critical functionality] Documented host-resolution precedence explicitly**

- **Found during:** Task 2 Connection section rewrite.
- **Issue:** Plan's Task 2.E spec covered the prose but not the 5-step precedence list that `cli/src/main.cpp:180-250` actually implements. A reader trying to debug which host `cdb` is contacting needs the full ordered list.
- **Fix:** Added explicit numbered precedence: (1) `--host`/positional, (2) `--node` → named nodes map, (3) `default_node`, (4) legacy `host`/`port`, (5) built-in port 4200 + explicit `--uds` carve-out.
- **Files modified:** `cli/README.md`.
- **Commit:** folded into `9a1bca17` (Task 2).

### Omissions vs. plan (all within plan's explicit permission-to-omit)

- `--chunk-size` flag — plan's Task 2.C bullet explicitly allowed omission if not exposed. Confirmed not exposed in `cli/src/main.cpp` put parser; omitted and called out explicitly in the Chunked Large Files section.

**Total deviations:** 3 auto-fixed (all Rule 1/2/3), 0 architectural.
**Impact on plan:** None — plan executed to spec intent; deviations tighten the output.

## Issues Encountered

- **PreToolUse Read-before-edit reminders** on `README.md` and `cli/README.md` after initial reads in the same session. Treated as non-blocking hooks; the Write/Edit tools accepted the operations and the file state tracker confirmed successful updates. No data loss.

## User Setup Required

None — documentation-only change.

## Next Phase Readiness

- **Plan 03 (db/ARCHITECTURE.md) is unblocked.** Anchor set in PROTOCOL.md already exists; Plan 03 can cross-link to the same anchors this plan uses, with zero content overlap (ARCHITECTURE is implementation-level; PROTOCOL is byte-level; this plan's cli/README + README are user-level).
- **Forward-reference flag:** README.md links to `db/ARCHITECTURE.md`, which does not yet exist. Plan 03 satisfies that link. If Plan 03 is delayed, the link 404s gracefully on GitHub rendering — no runtime impact.
- **Plan 04 (D-11 flatc regen + Data=8 enum removal)** and **Plan 05 (comment hygiene)** are orthogonal to this plan's output. No cross-dependencies.

## Self-Check: PASSED

- `README.md` exists on disk (106 lines).
- `cli/README.md` exists on disk (288 lines).
- `.planning/phases/125-docs-update-for-mvp-protocol/125-02-SUMMARY.md` exists on disk (this file).
- All four task commits (`339ca0a9`, `9a1bca17`, `6aff293b`, `5f93842e`) verified present in `git log --oneline`.
- VALIDATION.md D1 presence greps all PASS (listed above).
- VALIDATION.md D2 absence greps all PASS (listed above).

---
*Phase: 125-docs-update-for-mvp-protocol*
*Completed: 2026-04-22*
