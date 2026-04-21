# Phase 125: MVP Documentation Update - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in `125-CONTEXT.md` — this log preserves the alternatives considered.

**Date:** 2026-04-21
**Phase:** 125-docs-update-for-mvp-protocol
**Areas discussed:** Audience & file split, db/ARCHITECTURE.md scope, v4.1.0 feature coverage depth, Inline-comment sweep scope

---

## Audience & file split

### How should content be divided across the 4 (or fewer) files?

| Option | Description | Selected |
|--------|-------------|----------|
| Keep 4 files, sharpen boundaries | README pitch+quickstart; db/PROTOCOL byte-level wire; db/ARCHITECTURE internal; cli/README cdb commands; db/README build/operator quickstart; zero duplication, cross-link instead | ✓ |
| Merge db/README into db/ARCHITECTURE.md | db/README becomes thin pointer; ARCHITECTURE = canonical node-internal doc | |
| Single PROTOCOL + README per binary | Collapse to 3 files; PROTOCOL holds both wire + internals | |

**User's choice:** Keep 4 files, sharpen boundaries.
**Notes:** Each file owns its audience's detail.

### Cross-file linking style

| Option | Description | Selected |
|--------|-------------|----------|
| One-line summary + link to PROTOCOL section | Terse in-doc mention, deep reference lives in owner doc | ✓ |
| Full explanation inline where first needed | Self-sufficient per doc; mild duplication | |
| Terminology glossary in README.md | Central index; readers context-switch to glossary | |

**User's choice:** One-line summary + link.

### cli/README quickstart beyond command reference?

| Option | Description | Selected |
|--------|-------------|----------|
| 5-command hello-world flow | keygen→publish→put→ls→get; ~15 lines | ✓ |
| Command-reference only | No tutorial flow | |
| Full backup+restore worked example | ~40-60 lines, substantive scenario | |

**User's choice:** 5-command hello-world flow.

---

## db/ARCHITECTURE.md scope

### Structural skeleton

| Option | Description | Selected |
|--------|-------------|----------|
| Top-down: Storage → Engine → Net | Data layer first, then BlobEngine, then network; matches debug-unfamiliar-code-path mental model | ✓ |
| Bottom-up: Network → Engine → Storage | Packet-enters-node trace | |
| By concern: Data model, Concurrency, Replication | Conceptual groupings | |

**User's choice:** Top-down.

### Phase 121 strand/concurrency depth

| Option | Description | Selected |
|--------|-------------|----------|
| Dedicated subsection with ASCII threading diagram | ~40 lines; explains writer thread, coroutine safety, STORAGE_THREAD_CHECK; cites Phase 121 TSAN evidence | ✓ |
| One paragraph + link to code | ~10 lines; lean summary | |
| Inline wherever concurrency surfaces | No dedicated section | |

**User's choice:** Dedicated subsection.

### Diagram format

| Option | Description | Selected |
|--------|-------------|----------|
| ASCII only | Matches PROTOCOL.md style; terminal-friendly; zero tooling | ✓ |
| Mermaid where useful, ASCII elsewhere | GitHub renders Mermaid; not terminal-reviewable | |
| Prose only | Handshake + cascade hard to grasp textually | |

**User's choice:** ASCII only.

### Cross-reference to PROTOCOL.md

| Option | Description | Selected |
|--------|-------------|----------|
| Link by section anchor, don't duplicate | Consistent with sharpen-boundaries decision | ✓ |
| Brief inline recap when critical | Mild duplication; self-sufficient per doc | |

**User's choice:** Link by section anchor.

---

## v4.1.0 feature coverage depth

### How deep for each v4.1.0 feature (phases 116-124)?

| Option | Description | Selected |
|--------|-------------|----------|
| Dedicated section per major feature | Each big one gets its own subsection | Initially selected, amended |
| Single "v4.1.0 changes" section | Changelog-style summary, link to PROTOCOL | |
| User-facing only; link to .planning/ for internals | .planning/ as internals detail | ✗ (ruled out) |

**User's choice:** Free-text — "the plans will never land to github... never".
**Notes:** This is a hard constraint: `.planning/` is internal-only, the 4 docs are the complete public surface. Option 3 is structurally impossible.

### (Follow-up) Depth split per feature

| Option | Description | Selected |
|--------|-------------|----------|
| Tiered: deep for load-bearing, brief for small | Deep: chunked, NAME+BOMB, signer_hint/PUBK-first, auto-PUBK+D-05+D-06. Brief: ls filter, configurable constants, peer mgmt, pipelining | ✓ |
| Uniform depth per feature | Seven evenly-sized sections | |
| Claude's Discretion per feature | No up-front policy | |

**User's choice:** Tiered.

### Node-side error-code table (0x07-0x0B) in PROTOCOL.md?

| Option | Description | Selected |
|--------|-------------|----------|
| Full table with codes + trigger conditions + user-facing D-05 wording | ErrorResponse is wire; implementers need wording parity | ✓ |
| Codes + conditions only (no wording) | Separate CLI concern | |
| Skip — leave error handling to implementers | Under-documents real wire surface | |

**User's choice:** Full table with D-05 wording.

---

## Inline-comment sweep scope

### Scope of the SC#6 sweep

| Option | Description | Selected |
|--------|-------------|----------|
| Whole repo: cli/ + db/ + tests/ | Thorough; one commit per area | ✓ |
| Source only (skip tests) | Faster pass | |
| Phase 122-124 touch sites only | Minimal-risk | |

**User's choice:** Whole repo.

### TransportMsgType_Data = 8 in the schema

| Option | Description | Selected |
|--------|-------------|----------|
| Delete from schema + regenerate | Eliminates pre-122 vestige; tests updated to BlobWrite | ✓ |
| Keep as reserved slot | Defensive; stale vocabulary lingers | |
| Leave schema alone, only update comments | Unused enum remains callable | |

**User's choice:** Delete from schema + regenerate.

### Fold the `"(Phase 123)"` help-text leak fix into this phase?

| Option | Description | Selected |
|--------|-------------|----------|
| Yes — fix in this phase | Deferred from 124 specifically for 125 | ✓ |
| No — separate tiny 125.1 | Cleaner scope | |

**User's choice:** Yes.

### Strip `// Phase N` dev-facing code comments?

| Option | Description | Selected |
|--------|-------------|----------|
| Leave them — internal comments are fine | Memory rule scopes to user-visible only; breadcrumb value | |
| Strip them — keep code phaseless | Git log preserves archaeology | |
| Only strip when the phase reference is itself stale | Surgical per SC#6 wording | |

**User's choice:** Free-text — "i think they should be stripped, also i think the code itself has way too comments for some obvious tasks, it should be cleaned really well".
**Notes:** Scope expansion. Beyond SC#6, this becomes a full code-comment hygiene pass across cli/ + db/. Captured as D-13 (phase-N stripping) and D-14 (over-commenting cleanup) in CONTEXT.md. Planner will size as dedicated plan(s).

---

## Claude's Discretion

- Exact doc section ordering within each file (planner picks sensible outline).
- Whether `db/README.md` gets touched at all in this phase or stays largely as-is.
- Whether the code-comment hygiene pass also prunes commented-out code blocks (default: yes — always cruft).

## Deferred Ideas

- Threat model document (post-v1.0.0, separate phase)
- Migration guide (rejected — project hasn't shipped, no backward compat)
- Interactive protocol diagrams (rejected — ASCII only per D-06)
- Public API reference for client libs (premature — only one client exists)
- Top-level README glossary (rejected in favor of per-doc ownership)
