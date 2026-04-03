# Phase 81: Event-Driven Expiry - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-03
**Phase:** 81-event-driven-expiry
**Areas discussed:** Timer rearm strategy, Integration with ingest, Batch processing, Backward compat

---

## Timer Rearm Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| MDBX cursor query | O(1) sorted secondary index, no in-memory state | ✓ |
| In-memory min-heap | O(log n) but must stay in sync with storage | |
| You decide | | |

**User's choice:** MDBX cursor query

---

| Option | Description | Selected |
|--------|-------------|----------|
| Timer fires, finds nothing, rearms to next | Simple, correct, tiny wasted wakeup | ✓ |
| Cancel and rearm on delete | More responsive but adds complexity to every delete | |
| You decide | | |

**User's choice:** Timer fires, finds nothing, rearms to next

---

## Integration with Ingest

| Option | Description | Selected |
|--------|-------------|----------|
| Check in on_blob_ingested | Reuses Phase 79 unified callback, no new hook | ✓ |
| Separate engine-level hook | Decouples expiry from notification | |
| You decide | | |

**User's choice:** Check in on_blob_ingested

---

| Option | Description | Selected |
|--------|-------------|----------|
| Add timestamp+ttl to callback | Extend signature, update all call sites | |
| Query storage after ingest | Extra read but no signature changes | |
| Pass expiry directly | Compute in ingest path, pass through IngestResult | |
| You decide | Claude picks least invasive approach | ✓ |

**User's choice:** You decide (Claude's discretion)

---

## Batch Processing

| Option | Description | Selected |
|--------|-------------|----------|
| Process all in one scan | Existing run_expiry_scan behavior with precise timing | ✓ |
| One-at-a-time with immediate rearm | More granular but more timer churn | |
| You decide | | |

**User's choice:** Process all in one scan

---

## Backward Compat

| Option | Description | Selected |
|--------|-------------|----------|
| Fully replace periodic scan | Remove periodic timer, event-driven only | ✓ |
| Keep periodic as fallback | Belt and suspenders at longer interval | |
| You decide | | |

**User's choice:** Fully replace periodic scan

---

## Claude's Discretion

- How to get expiry time for rearm check (callback signature, storage query, or IngestResult)
- Storage API for earliest expiry query
- Config handling for now-unused expiry_scan_interval_seconds
- Edge cases: no blobs, TTL=0 blobs

## Deferred Ideas

None
