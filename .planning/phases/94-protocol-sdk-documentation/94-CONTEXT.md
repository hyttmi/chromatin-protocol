# Phase 94: Protocol & SDK Documentation - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Document the v2.1.1 features (delegation revocation, KEM key rotation, group membership revocation) in PROTOCOL.md and the SDK getting-started tutorial. This covers DOC-01 and DOC-02. No code changes — documentation only.

</domain>

<decisions>
## Implementation Decisions

### PROTOCOL.md Structure
- **D-01:** Extend existing sections rather than creating new top-level sections. Revocation goes under "Namespace Delegation" (line 427), key versioning under "Client-Side Envelope Encryption" (line 975), group membership revocation as subsection near existing group content.

### Tutorial Example Depth
- **D-02:** Match existing getting-started style — short focused snippets (5-15 lines each). Delegation revocation, KEM key rotation, and group membership management each get their own section with a minimal working example. No separate full lifecycle script.

### Bug Fix Documentation
- **D-03:** Silent update — document correct behavior only. No "behavioral change" callouts for the Phase 93 fixes (register namespace, group timestamp resolution). Pre-production, no users to notify.

### Claude's Discretion
- Section ordering within the existing PROTOCOL.md sections
- Exact code snippet structure in getting-started (follow existing patterns)
- Whether to add cross-references between PROTOCOL.md sections

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Protocol
- `db/PROTOCOL.md` — Current protocol doc (1147 lines). New content extends existing sections per D-01.
- `db/PROTOCOL.md` line 427 — "Namespace Delegation" section (revocation goes here)
- `db/PROTOCOL.md` line 975 — "Client-Side Envelope Encryption" section (key versioning goes here)

### SDK Tutorial
- `sdk/python/docs/getting-started.md` — Current tutorial (425 lines). New sections match existing style per D-02.

### Implementation References (what to document)
- `.planning/phases/91-sdk-delegation-revocation/91-01-SUMMARY.md` — Delegation revocation implementation
- `.planning/phases/92-kem-key-versioning/92-01-SUMMARY.md` through `92-03-SUMMARY.md` — KEM rotation, UserEntry v2, envelope key ring
- `.planning/phases/93-group-membership-revocation/93-01-SUMMARY.md` — write_to_group refresh
- `.planning/phases/93-group-membership-revocation/93-02-SUMMARY.md` — E2E test + bug fixes

### Source Code (authoritative for behavior)
- `sdk/python/chromatindb/client.py` — write_to_group, write_encrypted, read_encrypted
- `sdk/python/chromatindb/_directory.py` — register, delegate, revoke_delegation, create_group, remove_member, list_delegates
- `sdk/python/chromatindb/_envelope.py` — envelope_encrypt, envelope_decrypt, cipher suites
- `sdk/python/chromatindb/identity.py` — rotate_kem, key_version, _build_kem_ring_map
- `sdk/python/chromatindb/exceptions.py` — NotARecipientError, DirectoryError

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- PROTOCOL.md already has sections for Delegation (line 427), Envelope Encryption (line 975), Message Type Reference (line 664) — extend these
- getting-started.md already has sections for Groups (line 243), Directory (line 201), Error Handling (line 270) — add after these

### Established Patterns
- PROTOCOL.md uses subsections with wire format details, hex examples, and behavioral notes
- getting-started.md uses `## Section` headers with `# comment` style code blocks, minimal explanatory text between code
- Both docs reference specific message types by number and name

### Integration Points
- PROTOCOL.md Message Type Reference table (line 664) may need updates if any new types were added
- getting-started.md "Next Steps" section (line 415) — link new sections from there

</code_context>

<specifics>
## Specific Ideas

No specific requirements — follow established documentation patterns in both files.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 94-protocol-sdk-documentation*
*Context gathered: 2026-04-07*
