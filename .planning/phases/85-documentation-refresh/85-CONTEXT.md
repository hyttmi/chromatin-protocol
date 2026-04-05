# Phase 85: Documentation Refresh - Context

**Gathered:** 2026-04-05
**Status:** Ready for planning

<domain>
## Phase Boundary

Update all project documentation to accurately describe the v2.0.0 event-driven sync model, new wire types (BlobNotify, BlobFetch, BlobFetchResponse), bidirectional keepalive, and SDK auto-reconnect behavior. Four documents: PROTOCOL.md, README.md, SDK README, and getting-started tutorial.

</domain>

<decisions>
## Implementation Decisions

### Audience & Tone
- **D-01:** Docs are public-facing, written for external developers who might use chromatindb
- **D-02:** Ship-quality polish — accurate, well-structured, readable by external devs. Not marketing-polished but technically complete and clear
- **D-03:** Consistent developer-friendly tone across all docs. PROTOCOL.md uses MUST/SHOULD but with explanatory prose
- **D-04:** v2.0.0 only — no backward references to v1.x timer-based model. Docs describe current behavior

### PROTOCOL.md Structure
- **D-05:** Full restructure around connection lifecycle: handshake, sync, push notifications, targeted fetch, keepalive. Not just adding sections to existing layout
- **D-06:** Mermaid sequence/state diagrams for push-then-fetch flow and keepalive lifecycle. Renders on GitHub, easy to maintain
- **D-07:** Relay message filtering is NOT in PROTOCOL.md scope — relay has its own docs. PROTOCOL.md covers the wire protocol only

### Root README.md
- **D-08:** Full project overview (~100-200 lines): what chromatindb is, architecture section with node diagram + sync flow + keepalive lifecycle, build/run instructions, links to detailed docs
- **D-09:** Quick-start section: CMake build instructions + minimal config to start a single node. Clone to running in 5 minutes
- **D-10:** Dedicated architecture section explaining push→fetch→safety-net sync model with Mermaid diagrams

### SDK Documentation
- **D-11:** SDK README gets API reference + one runnable example: document connect() params (auto_reconnect, on_disconnect, on_reconnect), ConnectionState enum, wait_connected()
- **D-12:** Getting-started tutorial gets new "Connection Resilience" section (~50-80 lines) after existing content: auto-reconnect setup, reconnect callback, catch-up pattern

### Claude's Discretion
- Exact ordering of sections within restructured PROTOCOL.md
- Specific Mermaid diagram style choices
- How much of the existing PROTOCOL.md content to preserve vs rewrite during restructure
- README section ordering and heading hierarchy

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Wire Protocol (source of truth for byte formats)
- `db/PROTOCOL.md` — Current 964-line protocol spec, needs full restructure
- `db/src/protocol/message_types.h` — Canonical message type enum (types 59-61 for push, existing types)

### Existing Documentation (to update)
- `README.md` — Current 15-line root README, needs expansion to full project overview
- `sdk/python/README.md` — Current 119-line SDK README, needs auto-reconnect API docs
- `sdk/python/docs/getting-started.md` — Current 302-line tutorial, needs connection resilience section

### Implementation References (what happened in v2.0.0)
- `.planning/phases/79-send-queue-push-notifications/79-CONTEXT.md` — BlobNotify design decisions
- `.planning/phases/80-targeted-blob-fetch/80-CONTEXT.md` — BlobFetch/BlobFetchResponse design
- `.planning/phases/83-bidirectional-keepalive/83-CONTEXT.md` — Ping/Pong keepalive decisions
- `.planning/phases/84-sdk-auto-reconnect/84-CONTEXT.md` — SDK auto-reconnect design decisions

### Phase Summaries (what was built)
- `.planning/phases/79-send-queue-push-notifications/` — SUMMARY files for push notification implementation details
- `.planning/phases/80-targeted-blob-fetch/` — SUMMARY files for targeted fetch implementation
- `.planning/phases/81-event-driven-expiry/` — SUMMARY files for event-driven expiry
- `.planning/phases/82-reconcile-on-connect-safety-net/` — SUMMARY files for reconcile-on-connect
- `.planning/phases/83-bidirectional-keepalive/` — SUMMARY files for keepalive implementation
- `.planning/phases/84-sdk-auto-reconnect/` — SUMMARY files for SDK auto-reconnect

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/PROTOCOL.md` — existing protocol spec structure provides the skeleton for restructure
- `sdk/python/chromatindb/_reconnect.py` — ConnectionState enum, backoff_delay, callback types (source of truth for SDK API docs)
- `sdk/python/chromatindb/client.py` — connect() signature with auto_reconnect params (source of truth for API reference)

### Established Patterns
- PROTOCOL.md uses byte-level wire format tables (e.g., `[type:1][length:4 BE][payload:N]`)
- SDK README uses docstring-style API reference with parameter tables
- Getting-started tutorial uses step-by-step narrative with code blocks

### Integration Points
- README.md links to PROTOCOL.md and SDK docs
- SDK README links to getting-started tutorial
- PROTOCOL.md is referenced by SDK implementation code

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches within the decisions above.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 85-documentation-refresh*
*Context gathered: 2026-04-05*
