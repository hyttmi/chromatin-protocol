# Phase 125: MVP Documentation Update - Context

**Gathered:** 2026-04-21
**Status:** Ready for planning

<domain>
## Phase Boundary

Rewrite the 4 core shipping-public docs (`README.md`, `db/PROTOCOL.md`,
`cli/README.md`, new `db/ARCHITECTURE.md`) to describe the final v1 wire format,
signing canonical form, storage model, and user workflows — accurate to the
post-phase-124 codebase. Plus a code-comment hygiene pass: remove stale
references to removed fields, strip `// Phase N` historical breadcrumbs, and
tighten over-commenting on obvious code.

**HARD CONSTRAINT:** `.planning/` never ships to GitHub. The 4 public docs are
the complete public surface — no doc may defer detail to a phase artifact.

</domain>

<decisions>
## Implementation Decisions

### File structure and audience split

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
  restate. Example: `cli/README.md` mentions the PUBK-first rule in one line and
  links to `db/PROTOCOL.md#pubk-first-rule` for wire detail.
- **D-03:** Add a minimal 5-command "hello world" quickstart to `cli/README.md`
  (keygen → publish → put → ls → get, ~15 lines) so a new reader sees the
  day-one loop without piecing it together from the command reference.

### db/ARCHITECTURE.md structure (new file)

- **D-04:** Top-down skeleton: `Storage → Engine → Net`. Data layer first
  (8 DBIs, libmdbx model, ACID + strand-confinement), then `BlobEngine` (ingest
  pipeline, validation, PUBK-first, signer_hint checks, BOMB cascade), then
  network stack (handshake, sync protocol, PEX). Matches the mental model of
  someone debugging an unfamiliar code path.
- **D-05:** Phase 121's single-storage-thread strand model gets a dedicated
  subsection (~40 lines) with an ASCII threading diagram showing the writer
  thread, async funnel from I/O coroutines, `co_await` as an iterator-
  invalidation point, and `STORAGE_THREAD_CHECK` discipline. Reference the TSAN
  ship-gate evidence from Phase 121 `VERIFICATION.md`.
- **D-06:** ASCII diagrams only (no Mermaid). Matches `db/PROTOCOL.md`'s
  existing style, keeps the whole repo reviewable in a terminal, no extra
  tooling.
- **D-07:** Cross-references to `db/PROTOCOL.md` link by section anchor — never
  duplicate wire-level content. ARCHITECTURE stays implementation-level;
  PROTOCOL stays byte-level.

### v4.1.0 feature coverage depth

- **D-08:** Tiered depth per feature. **Deep sections (each substantive,
  self-contained):** chunked CDAT/CPAR (Phase 119), NAME + BOMB (Phase 123),
  `signer_hint` + PUBK-first (Phase 122), auto-PUBK + D-05 error decoder +
  D-06 BOMB cascade (Phase 124). **Brief sections (1-3 paragraphs each):**
  blob type indexing / `ls --type` (Phase 117), configurable constants + peer
  management (Phase 118), request pipelining (Phase 120). Every feature
  documented; depth proportional to external-reader need.
- **D-09:** Full error-code table in `db/PROTOCOL.md` covering `ErrorResponse`
  codes `0x07–0x0B`. Columns: code, canonical name, trigger condition,
  user-facing wording emitted by `cdb` (D-05 strings verbatim). External
  implementers need wording parity. Source: verified output from the `[error_decoder]`
  TEST_CASE in `cli/tests/test_wire.cpp` (the literal-equality unit test).

### SC#6 inline-comment sweep + code hygiene pass

- **D-10:** Sweep the whole repo (`cli/ + db/ + tests/`). Find and fix every
  inline comment referring to REMOVED pre-122 artifacts: `BlobData.namespace_id`
  (the field, not the 32-byte identifier — those are still legitimate),
  `BlobData.pubkey` (the embedded-per-blob field), `MsgType::Data = 8`, the
  old signing-input shape. One commit per area (cli, db, tests) for reviewable
  history.
- **D-11:** Delete `TransportMsgType_Data = 8` from `db/schemas/transport.fbs`,
  regenerate `transport_generated.h`, update the remaining test-only references
  (`test_protocol.cpp`, `test_framing.cpp`, `test_connection.cpp`) to use
  `TransportMsgType_BlobWrite` or remove the test cases if they only exercised
  the pre-122 dispatcher branch. Remove the `// TransportMsgType_Data
  direct-write branch was DELETED` memo at `db/peer/message_dispatcher.cpp:1388`
  since the enum value itself is gone.
- **D-12:** Fix the pre-existing `"(Phase 123)"` help-text leak at
  `cli/src/main.cpp:619`. Replace `"batched BOMB tombstone (Phase 123). Exit 2
  if no targets given.\n"` with the same content sans the `(Phase 123)` token.
  Deferred from phase 124 specifically for this phase.
- **D-13:** **Scope expansion (user-directed):** Beyond SC#6, strip `// Phase N`
  historical breadcrumbs from source code everywhere — both in comments that
  reference removed code (SC#6 literal scope) and in comments that simply
  historically cite the introducing phase (e.g., `// Phase 123 helper: ...`).
  Git log preserves archaeology; in-source phase numbers rot. Planner SHOULD
  size this as its own plan within phase 125.
- **D-14:** **Scope expansion (user-directed):** Code-comment hygiene pass —
  remove comments that explain obvious code. Per `CLAUDE.md`: "Default to
  writing no comments. Only add one when the WHY is non-obvious: a hidden
  constraint, a subtle invariant, a workaround for a specific bug." This is
  retrofit work across `cli/src/` and `db/`. Planner MUST decide the policy —
  e.g., pass over each file, prune comments that don't carry load, preserve
  non-obvious WHY. Likely a dedicated plan with its own test-suite smoke pass
  (no behavior change, but re-run full tests after to catch accidental code
  deletion).

### Scope boundary

- **D-15:** No new diagrams beyond ASCII. No migration guide (out of scope —
  this phase is reference documentation of the shipping state). No CHANGELOG
  restructuring. If a v4.1.0 feature doesn't already have good phase-artifact
  source material (SUMMARY/E2E), the docs writer reads the code directly
  (source of truth) and asks the user only if meaning is genuinely ambiguous.

### Claude's Discretion

- Exact doc section ordering within each file (planner picks a sensible
  outline; reader-flow matters more than strict adherence).
- Whether `db/README.md` gets touched at all in this phase or stays frozen —
  it's the existing build/operator quickstart and may need only minor
  edits to stay consistent with the new ARCHITECTURE.md.
- Whether the code-comment hygiene pass (D-14) also prunes commented-out code
  blocks. Default: yes, commented-out code is always cruft; delete on sight.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Current source-of-truth docs (to be rewritten/extended)
- `README.md` — top-level project pitch (97 lines today)
- `db/PROTOCOL.md` — byte-level wire protocol (1386 lines today)
- `db/README.md` — build + operator quickstart (477 lines today)
- `cli/README.md` — `cdb` command reference (152 lines today)

### Artifacts providing authoritative content for the rewrite
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/` — `signer_hint` schema, PUBK-first rationale
- `.planning/phases/123-tombstone-batching-and-name-tagged-overwrite/` — NAME + BOMB design, magics, D-15 same-second tiebreak
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-E2E.md` — live-verified descriptions of PUBK-first, auto-PUBK,
  D-05 error decoding, D-06 BOMB cascade (excellent raw material for PROTOCOL.md + ARCHITECTURE.md)
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/deferred-items.md` — records the `"(Phase 123)"` leak fix target
- `.planning/phases/121-storage-concurrency-invariant/121-VERIFICATION.md` — TSAN ship-gate evidence for the single-writer-thread model documented in D-05
- `.planning/phases/119-chunked-large-files/` — CDAT/CPAR chunked transport design
- `.planning/phases/120-request-pipelining/` — pipelined request/response design
- `.planning/phases/118-configurable-constants-peer-management/` — peer-management CLI commands + config
- `.planning/phases/117-blob-type-indexing-ls-filtering/` — type filter magic + `ls --type` surface

### Project-level
- `.planning/PROJECT.md` — product vision + constraints
- `.planning/REQUIREMENTS.md` — DOCS-01..DOCS-04 acceptance criteria
- `CLAUDE.md` — "default to no comments" policy informing D-14

### Memory constraints
- `feedback_no_phase_leaks_in_user_strings.md` — user-visible `cdb` output must not contain GSD phase numbers (scope for D-12)
- `feedback_no_duplicate_code.md` — applies to docs too: no content duplication between the four files (D-02)
- `feedback_delegate_tests_to_user.md` — orchestrator-level build/test runs belong to the user; docs phase must not trigger full rebuilds needlessly

### Node-side artifacts referenced by docs
- `db/schemas/blob.fbs` — ground truth for `Blob` FlatBuffer vtable layout (PROTOCOL §Wire format)
- `db/schemas/transport.fbs` — ground truth for `BlobWriteBody`, `TransportMsgType` enum (D-11 deletes `Data = 8` here)
- `db/wire/transport_generated.h` — regenerated artifact after D-11
- `cli/src/tests/test_wire.cpp` `[error_decoder]` TEST_CASE — authoritative D-05 wording for PROTOCOL error-code table (D-09)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable assets
- `db/PROTOCOL.md` already uses ASCII diagrams for byte layouts — new `db/ARCHITECTURE.md` reuses that style.
- Phase 121 already proved the strand/concurrency model via TSAN; the
  `VERIFICATION.md` there has the exact evidence format to cite in D-05's
  ARCHITECTURE subsection.
- `cli/tests/test_wire.cpp` `[error_decoder]` TEST_CASE has the literal D-05
  strings (REQUIRE equality checks) — copy-paste source for D-09's table.

### Established patterns
- Existing docs are plain markdown with fenced code blocks. No MDX, no Mermaid.
  Keeps the repo grep-able and diff-friendly (consistent with the ASCII-only
  decision D-06).
- Code comments already lean toward over-explaining (the user's observation for
  D-14). `feedback_no_duplicate_code.md` + `CLAUDE.md`'s no-comments default
  together imply the hygiene pass is a natural cleanup rather than a risky
  rewrite.

### Integration points
- `cli/src/main.cpp:619` — the one pre-existing user-string phase leak to fix
  (D-12). Lives in the help-text switch for `cdb rm`.
- `db/schemas/transport.fbs` (re-generation step for D-11) — check that the
  generated header is checked in (vs generated at build time); if checked in,
  commit the regenerated file explicitly.
- No new binary targets needed; no CMakeLists.txt changes expected.

</code_context>

<specifics>
## Specific Ideas

- User-visible strings must stay phase-number-free (reiterating MEMORY feedback).
- The `[error_decoder]` TEST_CASE's string literals ARE the PROTOCOL.md error
  table. Keep them byte-identical.
- Phase 124's 124-E2E.md retrospective includes the "cross-reference Uptime
  against rebuild timestamp" lesson — relevant operator knowledge, probably
  worth a sentence in `db/README.md`'s deployment section (Claude's discretion
  whether to include).
- ASCII diagrams in ARCHITECTURE.md should model db/PROTOCOL.md's current style
  (ASCII box art, 80-column safe).

</specifics>

<deferred>
## Deferred Ideas

- **Threat model document** — explicit STRIDE doc for the shipping product. Out
  of scope for 125; would be its own phase post-v1.0.0.
- **Migration guide (pre-122 → post-122)** — not needed because the project
  has never shipped; backward compat is NOT a goal (`feedback_no_backward_compat.md`).
- **Interactive protocol diagrams** — Mermaid/excalidraw tempting but rejected
  in D-06.
- **Public API reference for client libraries** — only one client exists
  (`cdb`), so this is premature. Revisit if/when a second client ships.
- **Glossary in top-level README.md** — considered and rejected in favor of
  per-doc ownership (D-01 + D-02).

### Reviewed Todos (not folded)
None — the single open TODO (`node-reconnect-loop-to-ephemeral-client-ports.md`)
is already resolved (commit `04255ec0`) and lives in `.planning/todos/completed/`.

</deferred>

---

*Phase: 125-docs-update-for-mvp-protocol*
*Context gathered: 2026-04-21*
