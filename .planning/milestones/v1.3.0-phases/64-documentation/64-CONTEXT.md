# Phase 64: Documentation - Context

**Gathered:** 2026-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

Update PROTOCOL.md, README.md, and db/README.md to reflect all v1.3.0 changes: request_id transport semantics, concurrent dispatch model, ExistsRequest/ExistsResponse, and NodeInfoRequest/NodeInfoResponse. No code changes — documentation only.

</domain>

<decisions>
## Implementation Decisions

### request_id in PROTOCOL.md
- **D-01:** Update the TransportMessage schema section once with `request_id: uint32` — do NOT annotate every individual message section. request_id lives in the transport envelope, not in payloads.
- **D-02:** State the three semantics clearly: client-assigned, node-echoed, per-connection scope. Server-initiated messages (Notification) use request_id=0.
- **D-03:** Add a one-sentence note in the transport overview that the node may process requests concurrently and responses may arrive out of order, correlated by request_id.

### Concurrent dispatch placement
- **D-04:** Keep dispatch model OUT of PROTOCOL.md. PROTOCOL.md is a wire format spec for client developers — they don't need to know which thread handles their request.
- **D-05:** Put the inline-vs-offload categorization in db/README.md's architecture/features section, aimed at node operators and contributors.

### Root README.md scope
- **D-06:** Keep root README.md as a thin pointer to db/README.md. Bump the version string to v1.3.0. No feature bullets or duplicated content.

### New message types in PROTOCOL.md
- **D-07:** Add ExistsRequest/ExistsResponse and NodeInfoRequest/NodeInfoResponse sections following the same structure as existing client protocol sections (ReadRequest, StatsRequest, etc.) — byte-level wire format tables.
- **D-08:** Update the message type reference table from 36 to 40 entries.

### db/README.md updates
- **D-09:** Add feature entries for: concurrent request pipelining (request_id), blob existence check (ExistsRequest), and node capability discovery (NodeInfoRequest).
- **D-10:** Update the wire protocol summary paragraph (currently says "36 message types" — now 40).
- **D-11:** Update test count if it changed after v1.3.0 phases.

### Claude's Discretion
- Exact wording and paragraph flow
- Whether ExistsRequest/ExistsResponse and NodeInfoRequest/NodeInfoResponse get one combined section or two separate sections in PROTOCOL.md
- Ordering of new content within existing document structure
- How to phrase concurrent-response semantics for client developers

</decisions>

<specifics>
## Specific Ideas

- request_id explanation should emphasize the pipelining use case: "send multiple requests without waiting, match responses by request_id"
- Follow the exact wire format table style already used for ReadRequest, ListRequest, StatsRequest sections in PROTOCOL.md

</specifics>

<canonical_refs>
## Canonical References

### Target documents
- `db/PROTOCOL.md` — Wire protocol walkthrough (currently covers types 0-36, needs 37-40 + request_id)
- `db/README.md` — Full project documentation (features, config, usage)
- `README.md` — Root pointer (version string update only)

### Requirements
- `.planning/REQUIREMENTS.md` §DOCS-01 — PROTOCOL.md: request_id, dispatch model, new message types
- `.planning/REQUIREMENTS.md` §DOCS-02 — README.md: v1.3.0 capabilities
- `.planning/REQUIREMENTS.md` §DOCS-03 — db/README.md: concurrent dispatch, new types, request_id

### Source of truth for new features
- `.planning/phases/61-transport-foundation/61-CONTEXT.md` — request_id decisions (D-01 through D-10)
- `.planning/phases/63-query-extensions/63-CONTEXT.md` — ExistsRequest/NodeInfoRequest decisions (D-01 through D-05)
- `db/schemas/transport.fbs` — Current FlatBuffers schema with all 40 types
- `db/peer/peer_manager.cpp` — Handler implementations (wire format, dispatch model)
- `db/version.h` — Version constants used in NodeInfoResponse

### Prior documentation patterns
- `db/PROTOCOL.md` §Client Protocol — ReadRequest/ListRequest/StatsRequest sections establish the format for new message type documentation
- `db/README.md` §Features — Feature entry style and depth for new entries

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- Existing PROTOCOL.md client protocol section: template for ExistsRequest/NodeInfoRequest documentation
- Existing message type reference table: extend with 4 new rows
- db/README.md features list: append 3 new entries following established pattern

### Established Patterns
- Wire format tables: `| Field | Offset | Size | Encoding | Description |` format
- Feature entries: bold name + em-dash + one-paragraph description
- Message type table: `| Value | Name | Description |` with concise one-line descriptions

### Integration Points
- TransportMessage schema display in PROTOCOL.md transport layer section (add request_id field)
- Message type reference table at bottom of PROTOCOL.md (add types 37-40)
- db/README.md wire protocol summary paragraph (update type count)
- db/README.md test count (verify current count)
- Root README.md version string line

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 64-documentation*
*Context gathered: 2026-03-26*
