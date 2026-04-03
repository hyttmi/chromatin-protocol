# Phase 81: Event-Driven Expiry - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Replace the periodic 60-second expiry scan timer with a precise next-expiry timer that fires at exactly the right second. On every blob ingest, check if the new blob's expiry is earlier than the current timer target and rearm if so. After each scan, rearm to the next earliest expiry from storage. Fully replaces the periodic scan — no fallback timer.

</domain>

<decisions>
## Implementation Decisions

### Timer rearm strategy
- **D-01:** Use MDBX cursor query to determine the next earliest expiry time. O(1) with a sorted secondary index on expiry time. No in-memory state to maintain — storage is the source of truth.
- **D-02:** If a blob is deleted/purged before its expiry timer fires, the timer fires anyway, finds nothing expired, and rearms to the next earliest. Simple, correct, tiny wasted wakeup. No need to cancel/rearm on delete.

### Integration with ingest
- **D-03:** Check in on_blob_ingested (Phase 79 unified callback). After on_blob_ingested fires, compute the new blob's expiry and compare to the current timer target. If earlier, cancel and rearm. Reuses existing hook, no new callback needed.
- **D-04:** (Claude's Discretion) How to get the expiry time — extend on_blob_ingested signature, query storage, or pass through IngestResult. Claude picks the least invasive approach.

### Batch processing
- **D-05:** Process all expired blobs in one scan. When the timer fires, `run_expiry_scan()` purges ALL expired blobs (existing behavior). After the scan, query for next earliest expiry and rearm. Existing batch behavior with precise timing.

### Backward compat
- **D-06:** Fully replace the periodic scan. Remove the periodic timer (`expiry_scan_loop()`) entirely. On startup, query storage for earliest expiry and set the initial timer. The `expiry_scan_interval_seconds` config option becomes unused.

### Claude's Discretion
- Storage API for querying earliest expiry time (new method on Storage or cursor-based)
- Whether `expiry_scan_interval_seconds` config is removed or deprecated (kept in struct but ignored)
- Timer state tracking (simple `uint64_t next_expiry_target_` vs more complex)
- Edge case: no blobs in storage (no timer armed, arm on first ingest)
- Edge case: all blobs have TTL=0 (never expire) — timer stays disarmed

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Expiry scanning
- `db/peer/peer_manager.cpp` — `expiry_scan_loop()` (line ~2873): current periodic timer implementation
- `db/peer/peer_manager.h` — `expiry_timer_` pointer, `expiry_scan_interval_seconds_` member

### Storage
- `db/storage/storage.h` — `run_expiry_scan()` declaration
- `db/storage/storage.cpp` — `run_expiry_scan()` implementation, MDBX cursor patterns

### Engine & ingest callback
- `db/peer/peer_manager.cpp` — `on_blob_ingested()` (line ~2926): unified callback where expiry check hooks in
- `db/engine/engine.h` — `IngestResult`, `ingest()` signature

### Config
- `db/config/config.h` — `expiry_scan_interval_seconds` (line 36 area)

### Requirements
- `.planning/REQUIREMENTS.md` — MAINT-01, MAINT-02, MAINT-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `expiry_scan_loop()` — current coroutine structure (steady_timer + cancel pattern) can be adapted for event-driven timer
- `run_expiry_scan()` — existing purge logic, reuse as-is for the scan step
- `expiry_timer_` pointer — existing cancellation mechanism for the timer
- `on_blob_ingested()` — unified callback hook point (Phase 79)

### Established Patterns
- Timer-cancel wakeup pattern: `steady_timer` + `async_wait` + `cancel()` to rearm — used across 8+ coroutine loops in PeerManager
- Member coroutine with `stopping_` flag for clean shutdown
- MDBX cursor iteration for ordered traversal (used in sync protocol for seq-ordered reads)

### Integration Points
- `expiry_scan_loop()` — replace entirely with event-driven loop
- `on_blob_ingested()` — add expiry check + rearm logic after notification fan-out
- `Storage` — add method to query earliest expiry time (MDBX cursor on expiry-sorted data)
- `start()` — initial timer arm from storage query on startup

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 81-event-driven-expiry*
*Context gathered: 2026-04-03*
