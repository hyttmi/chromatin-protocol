# Phase 78: Documentation & Polish - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-02
**Phase:** 78-documentation-polish
**Areas discussed:** Protocol spec depth, README structure, Tutorial flow
**Mode:** Auto (all areas auto-selected with recommended defaults)

---

## Protocol Spec Depth

| Option | Description | Selected |
|--------|-------------|----------|
| Byte-level format tables | Match existing PROTOCOL.md Offset/Size/Field/Description style | yes |
| High-level description | Conceptual overview without byte offsets | |
| Mixed (concept + bytes) | High-level intro followed by format tables | |

**User's choice:** [auto] Byte-level format tables (matches existing PROTOCOL.md pattern)
**Notes:** Existing PROTOCOL.md is consistently byte-level. Cross-language SDK implementers need exact offsets.

---

## README Structure

| Option | Description | Selected |
|--------|-------------|----------|
| Table-based method listing | New sections matching existing Method/Description/Returns pattern | yes |
| Prose-based API reference | Paragraph descriptions per method | |
| Inline with existing sections | Add encrypted methods to existing Data Operations table | |

**User's choice:** [auto] Table-based method listing under new sections (matches existing API Overview)
**Notes:** Encryption is a distinct capability category. Separate sections keep the README scannable.

---

## Tutorial Flow

| Option | Description | Selected |
|--------|-------------|----------|
| Step-by-step narrative extension | Extend existing tutorial with new sections at end | yes |
| Separate encryption tutorial | New file: docs/encryption-tutorial.md | |
| Cookbook recipes | Short independent examples, not a narrative | |

**User's choice:** [auto] Step-by-step narrative extending existing tutorial (matches getting-started.md pattern)
**Notes:** Single tutorial file is simpler. Encryption is a natural next step after basic blob operations.

---

## Claude's Discretion

- Exact placement of envelope spec within PROTOCOL.md
- Whether to include a high-level concepts intro before byte tables
- Method ordering in README encryption tables
- Whether tutorial shows error handling (NotARecipientError)

## Deferred Ideas

None -- auto-mode stayed within phase scope
