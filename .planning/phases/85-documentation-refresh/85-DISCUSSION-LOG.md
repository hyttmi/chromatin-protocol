# Phase 85: Documentation Refresh - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-05
**Phase:** 85-documentation-refresh
**Areas discussed:** Audience & longevity, PROTOCOL.md structure, Sync model narrative, SDK example depth

---

## Audience & Longevity

| Option | Description | Selected |
|--------|-------------|----------|
| Just me | Future-you reference. Technical, terse, assumes deep context | |
| Me + future contributors | Technical but self-contained. Someone with crypto/networking background could onboard | |
| Public-facing | Written for external developers who might use chromatindb | ✓ |

**User's choice:** Public-facing
**Notes:** None

### Polish Level

| Option | Description | Selected |
|--------|-------------|----------|
| Ship-quality | Accurate, well-structured, readable by external devs. Not marketing-polished but technically complete | ✓ |
| Comprehensive reference | Deep technical detail, edge cases, multiple examples per feature | |
| Minimal accurate | Correct info, minimal prose | |

**User's choice:** Ship-quality

### Tone

| Option | Description | Selected |
|--------|-------------|----------|
| RFC-style technical | PROTOCOL.md stays formal/spec-like. README and SDK docs warmer | |
| Consistent developer-friendly | All docs approachable developer tone. PROTOCOL.md uses MUST/SHOULD with explanatory prose | ✓ |

**User's choice:** Consistent developer-friendly

### History References

| Option | Description | Selected |
|--------|-------------|----------|
| v2.0.0 only | No backward references. Docs describe current behavior | ✓ |
| Brief migration notes | Short 'Changed in v2.0.0' callouts | |

**User's choice:** v2.0.0 only

---

## PROTOCOL.md Structure

| Option | Description | Selected |
|--------|-------------|----------|
| Add to existing sections | Insert new message types into existing 'Message Types' section | |
| New 'Event-Driven Sync' section | Group all v2.0.0 additions into dedicated section | |
| Full restructure | Reorganize whole doc around connection lifecycle: handshake, sync, push, keepalive | ✓ |

**User's choice:** Full restructure

### Diagrams

| Option | Description | Selected |
|--------|-------------|----------|
| Mermaid diagrams | Mermaid sequence/state diagrams. Renders on GitHub, easy to maintain | ✓ |
| ASCII art | Plain text diagrams. Works everywhere | |
| No diagrams | Prose-only descriptions | |

**User's choice:** Mermaid diagrams

### Relay Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, relay section | Dedicated subsection on relay message filtering | |
| No, relay has own docs | Keep PROTOCOL.md focused on wire protocol only | ✓ |

**User's choice:** No, relay has own docs

---

## Sync Model Narrative

### README Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Full project overview | What chromatindb is, how sync works, build/run, links. ~100-200 lines | |
| Landing page + links | Brief pitch + links. ~50 lines | |
| Comprehensive guide | Everything in one doc. 500+ lines | |

**User's choice:** Full project overview

### Quick-start

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, build + run | CMake build instructions + minimal config. Clone to running in 5 min | ✓ |
| Yes, Docker only | Docker run command for instant start | |
| No, link to docs | Link to separate guide | |

**User's choice:** Yes, build + run

### Sync Explanation

| Option | Description | Selected |
|--------|-------------|----------|
| Conceptual + diagram | 2-3 paragraphs + Mermaid sequence diagram | |
| Bullet points | Quick bullet list, links to PROTOCOL.md | |
| Architecture section | Dedicated section with node diagram, sync flow, keepalive lifecycle | ✓ |

**User's choice:** Architecture section

---

## SDK Example Depth

### SDK README

| Option | Description | Selected |
|--------|-------------|----------|
| API reference + example | Document connect() params, ConnectionState, wait_connected(). One runnable example | ✓ |
| Full patterns guide | Multiple examples: basic, catch-up, disable, monitoring | |
| Minimal API docs | Just params and types | |

**User's choice:** API reference + example

### Getting-Started Tutorial

| Option | Description | Selected |
|--------|-------------|----------|
| New section | Add 'Connection Resilience' section. ~50-80 lines | ✓ |
| Weave into existing | Integrate into existing flow. No separate section | |
| Separate advanced guide | New advanced-patterns.md doc | |

**User's choice:** New section

---

## Claude's Discretion

- Exact section ordering within restructured PROTOCOL.md
- Mermaid diagram style choices
- How much existing PROTOCOL.md content to preserve vs rewrite
- README heading hierarchy

## Deferred Ideas

None
