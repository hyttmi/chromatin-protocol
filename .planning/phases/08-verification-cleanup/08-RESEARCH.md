# Phase 8: Verification & Cleanup - Research

**Researched:** 2026-03-05
**Domain:** Documentation verification, dead code cleanup, traceability
**Confidence:** HIGH

## Summary

Phase 8 is a documentation and cleanup phase with no new features or libraries. The work consists of three distinct areas: (1) creating missing verification documents for Phases 2 and 5, (2) removing dead handshake code from connection.cpp, and (3) confirming traceability is up-to-date. All work is internal to the project with no external dependencies.

**Primary recommendation:** Two plans -- one for verification docs (both Phase 2 and Phase 5), one for dead code cleanup and traceability finalization. All work is independent and can run in wave 1.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Create 02-VERIFICATION.md for Phase 2 (Storage Engine) -- follows existing format from 01-VERIFICATION.md
- Create 05-VERIFICATION.md for Phase 5 (Peer System) -- must include gap closure phases (6: Sync Receive Side, 7: Peer Discovery)
- Phase 7 has no verification doc either -- include 07-VERIFICATION.md if gaps exist, but Phase 5 verification covering DISC-02 may suffice
- Format: structured frontmatter + Observable Truths table + Required Artifacts, matching existing verification docs exactly
- Remove HandshakeInitiator hs2 at connection.cpp:153 and all associated confused comments (~lines 153-179)
- Scan for other orphaned code in the handshake/connection area but don't do a project-wide dead code hunt
- Keep HandshakeInitiator itself -- it's actively used (hs at line 125)
- REQUIREMENTS.md traceability table is already 32/32 mapped and Complete
- Verify gap closure phases (6, 7) are correctly attributed -- they already appear in the table
- Ensure Phase 8 itself is reflected in ROADMAP.md progress table after completion

### Claude's Discretion
- Exact verification doc wording and evidence citations
- Whether Phase 7 needs its own VERIFICATION.md or is covered by Phase 5's doc
- Any additional minor dead code found during the handshake area scan
- Formatting and organization of traceability updates

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STOR-01 | Node stores signed blobs in libmdbx keyed by namespace + SHA3-256 hash | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| STOR-02 | Node deduplicates blobs by content-addressed SHA3-256 hash | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| STOR-03 | Node maintains per-namespace monotonic sequence index | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| STOR-04 | Node maintains expiry index sorted by expiry timestamp | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| STOR-05 | Node automatically prunes expired blobs via background scan | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| STOR-06 | Blobs have configurable TTL (default 7 days, TTL=0 = permanent) | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| DAEM-04 | Node recovers cleanly from crashes (libmdbx ACID) | 02-VERIFICATION.md must verify this against Phase 2 implementation |
| DISC-01 | Node connects to configured bootstrap nodes on startup | 05-VERIFICATION.md must verify this against Phase 5 implementation |
</phase_requirements>

## Standard Stack

No new libraries or tools required. This phase uses only:
- Existing `.planning/` documentation infrastructure
- Existing source code for verification evidence
- Existing test suite for evidence gathering

## Architecture Patterns

### Verification Document Format

Established pattern from 01-VERIFICATION.md, 03-VERIFICATION.md, 04-VERIFICATION.md, 06-VERIFICATION.md:

```yaml
---
phase: XX-name
verified: ISO-8601-timestamp
status: passed
score: N/N must-haves verified
re_verification: false
---
```

Sections (in order):
1. Phase header (goal, verified date, status)
2. Observable Truths table (mapped from ROADMAP success criteria)
3. Required Artifacts table
4. Key Link Verification table
5. Requirements Coverage table
6. Anti-Patterns Found table
7. Human Verification Required section
8. Test Suite Summary
9. Gap Summary

### Dead Code Identification

The dead code at connection.cpp:153-179 consists of:
- `HandshakeInitiator hs2(identity_);` -- creates a second HandshakeInitiator that is never used
- ~26 lines of stream-of-consciousness comments about framing confusion
- The actual working code resumes at line 180 (`session_keys_ = std::move(temp_keys)`)

The dead code boundary is clean -- lines 153-179 can be removed with no functional impact. Line 180 onward is active code that correctly moves temp_keys into session_keys_.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Verification format | Custom format | Existing VERIFICATION.md pattern | 4 existing examples establish the canonical format |
| Evidence gathering | Manual code reading | grep + test suite output | Automated evidence is reproducible and verifiable |

## Common Pitfalls

### Pitfall 1: Verification Without Evidence
**What goes wrong:** Verification doc states "VERIFIED" without citing specific file:line or test numbers
**Why it happens:** Template-filling without actually checking code
**How to avoid:** Every VERIFIED cell must cite file path + line number + test case name/number
**Warning signs:** Generic citations like "see storage.cpp"

### Pitfall 2: Missing Gap Closure Coverage in Phase 5 Verification
**What goes wrong:** Phase 5 verification only covers original Phase 5 work, misses Phases 6+7 which close Phase 5's gaps
**Why it happens:** Phase 5 success criteria references bootstrap discovery and sync, but the actual implementations were completed in Phases 6 and 7
**How to avoid:** Phase 5 verification must trace through to gap closure phases (6: sync receive side, 7: peer discovery)
**Warning signs:** DISC-01 not verified, DISC-02 not referenced

### Pitfall 3: Dead Code Boundary Error
**What goes wrong:** Removing too much or too little from connection.cpp
**Why it happens:** The dead code is interleaved with comments that could look important
**How to avoid:** Clear boundary: remove lines 153-179 (hs2 + all comment block), keep line 180 onward (session_keys_ move)
**Warning signs:** Compilation failure after removal

## Code Examples

### Verification Truth Example (from 01-VERIFICATION.md)

```markdown
| 1  | Node generates ML-DSA-87 keypair and derives namespace as SHA3-256(pubkey), deterministically | VERIFIED | `src/identity/identity.h` + `identity.cpp`: `generate()` calls `signer_.generate_keypair()` then `crypto::sha3_256(signer_.export_public_key())`. |
```

### Dead Code to Remove (connection.cpp:153-179)

```cpp
HandshakeInitiator hs2(identity_);
// Actually, we can't re-derive keys. Let me restructure.
// The handshake class already encrypts auth messages internally.
// ...26 lines of abandoned design notes...
```

## Open Questions

None. This phase is well-scoped with clear deliverables and established patterns.

## Sources

### Primary (HIGH confidence)
- Existing VERIFICATION.md files (01, 03, 04, 06) -- established format
- connection.cpp source code -- dead code identified at specific lines
- REQUIREMENTS.md -- current traceability state (32/32 mapped)
- ROADMAP.md -- Phase 8 success criteria

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies
- Architecture: HIGH - 4 existing verification docs establish the pattern
- Pitfalls: HIGH - scope is narrow and well-defined

**Research date:** 2026-03-05
**Valid until:** N/A (internal project artifacts)
