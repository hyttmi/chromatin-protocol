# Phase 127: NodeInfoResponse Capability Extensions — Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-22
**Phase:** 127-nodeinforesponse-capability-extensions
**Areas discussed:** Rate-limit field mismatch, Field ordering in wire format, CLI rendering in `cdb info`, VERI-02 test scope

---

## Rate-limit field mismatch

| Option | Description | Selected |
|--------|-------------|----------|
| Rename to rate_limit_bytes_per_sec u64 BE | Match the actual enforced config field. Exposes the real value the node rate-limits on. Symmetric with Phase 128's chromatindb_config_* Prometheus gauge. REQ-deviation: NODEINFO-03 wire field name + type changed. u32 is too small for bytes/sec anyway. | ✓ |
| Keep messages_per_second u32 BE, stub to 0 | Honor REQUIREMENTS.md verbatim. Field carries 0 (= not advertised). Future work would add messages/sec tracking. Pre-MVP acceptable but ships a stub wire field. | |
| Expose bytes/sec under messages name | Wire field named 'messages_per_second' but populated with bytes/sec value. Operators reading the wire field get wrong units. Listed for completeness only. | |

**User's choice:** Rename to rate_limit_bytes_per_sec (u64 BE)
**Notes:** Claude surfaced the semantic mismatch during codebase scout (`grep` found `config.rate_limit_bytes_per_sec` already present). D-03 in CONTEXT.md formalizes the REQ-deviation. REQUIREMENTS.md NODEINFO-03 will be updated during Phase 127 plan execution.

---

## Field ordering in wire format

| Option | Description | Selected |
|--------|-------------|----------|
| Insert BEFORE [types_count][supported_types] | Layout: ...storage_max:8BE \| max_blob_data_bytes:8BE \| max_frame_bytes:4BE \| rate_limit_bytes_per_sec:8BE \| max_subscriptions_per_connection:4BE \| types_count:1 \| supported_types:N. All fixed fields contiguous, variable section last. | ✓ |
| Append AFTER supported_types | Layout: [existing...types_count:1][supported_types:N][4 new fields]. Variable section ends up in middle. Decoder must read types then more fixed. Awkward but works. | |
| Append at very end but swap so new fields go first | Hybrid — add the 4 new fields after types/supported with them still trailing. Same as append but confirming we don't reorder existing fields at all. | |

**User's choice:** Insert before types_count
**Notes:** Protocol-breaking is free here (entire v4.2.0 is breaking). Keeping fixed-fields-then-variable pattern simplifies decoder and future extensions.

---

## CLI rendering in `cdb info`

| Option | Description | Selected |
|--------|-------------|----------|
| Print all 4 caps | cdb info adds lines for Max blob, Max frame, Rate limit, Max subs. Humanized byte sizes via humanize_bytes. Matches "operators can remotely verify caps" purpose. | ✓ |
| Print only blob + frame caps | Only the two size caps that affect blob writes. Rate-limit and subscriptions treated as operator-internal. Leaner output. | |
| Print none — internal cache only | CLI reads all 4 on connect for Phase 130 auto-tune, doesn't display any. Operators would read /metrics instead. Loses human-facing verification surface. | |

**User's choice:** Print all 4 caps
**Notes:** 0-values for rate_limit and max_subs render as "unlimited" per D-07 (convention matches existing Quota handling).

---

## VERI-02 test scope

| Option | Description | Selected |
|--------|-------------|----------|
| Integration round-trip in test_peer_manager.cpp | Extend existing `NodeInfoRequest returns version and node state` TEST_CASE at test_peer_manager.cpp:2773. Fire a real NodeInfoRequest through the real dispatcher, assert all 4 new fields decode + boundary checks (0, UINT32_MAX, UINT64_MAX). Catches dispatcher wiring + encoder together. | ✓ |
| Pure encoder + CLI decoder unit tests | Two separate unit tests: (1) node encoder test feeding fake Config, asserting exact byte layout. (2) CLI decoder test feeding canned bytes, asserting parsed values. No live dispatcher. Fast, focused, but doesn't exercise real request→response path. | |
| Both — unit + integration | Unit tests for encoder/decoder edge cases (boundary values) + one integration test proving the real dispatcher wires the right Config fields. More coverage, more lines. | |

**User's choice:** Integration round-trip in test_peer_manager.cpp
**Notes:** Single TEST_CASE extension is the minimum-coverage path that still catches dispatcher wiring bugs. Claude will include a wire-size assertion (resp payload grew by exactly 24 bytes) per D-10 to catch encoder miscalculation.

---

## Claude's Discretion

- Exact phrasing of new `cdb info` output lines (keep column-aligned).
- Whether to use a test helper vs inline boundary values in each TEST_CASE.
- Use existing `store_u64_be` / `store_u32_be` helpers (not really discretion — required by `feedback_no_duplicate_code.md`).
- Naming of extended TEST_CASE — keeps `[peer][nodeinfo]` tags.

## Deferred Ideas

- `rate_limit_messages_per_second` as a separate distinct metric — post-MVP if operators ever want per-message rate distinct from bytes/sec.
- Pre-handshake capability ping — out of MVP scope; requires new pre-auth message type.
- PROTOCOL.md wire-table visual ASCII diagram — Phase 131 can decide.
