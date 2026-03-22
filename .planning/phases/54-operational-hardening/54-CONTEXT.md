# Phase 54: Operational Hardening - Context

**Gathered:** 2026-03-22
**Status:** Ready for planning

<domain>
## Phase Boundary

Node operators get finer control over GC timing, protection against malformed timestamps, and actionable error messages on sync rejection. Three features: configurable expiry scan interval, timestamp validation on ingest, and human-readable SyncRejected reason strings. Plus PROTOCOL.md documentation update.

</domain>

<decisions>
## Implementation Decisions

### SyncRejected wire format
- Keep byte enum approach (no string on the wire). Receiver maps byte to human-readable string locally via shared header constant.
- No backward compatibility needed -- no deployed nodes yet. Free to restructure the existing reason bytes.
- Full expansion of reason codes: cooldown, session_limit, byte_rate, storage_full, quota_exceeded, namespace_not_found, blob_too_large, timestamp_rejected (8 total).
- Shared constexpr mapping in a header -- both sender and receiver use the same byte-to-string table.
- Logging: receiver logs at debug level (it chose to reject), initiator logs at info level (actionable for operator).

### Timestamp rejection behavior
- Hardcoded thresholds: 1 hour in the future, 30 days in the past. NOT configurable -- per key decision, config is for node-local operational concerns, not protocol behavior.
- Step 0 placement: timestamp validation is a cheap integer compare, happens before any crypto work (SHA3, ML-DSA). Fits existing Step 0 pattern.
- Reject with reason: ingest result includes rejection reason string (e.g. "timestamp too far in future"). Writer gets actionable feedback.
- Applies to both direct ingest AND sync ingest -- malformed timestamps rejected regardless of source.
- timestamp_rejected reason code added to SyncRejected enum for sync-path rejections.

### Expiry scan configuration
- New config field: `expiry_scan_interval_seconds` (uint32_t)
- Default: 60 seconds (matches current hardcoded behavior -- zero behavior change)
- Minimum: 10 seconds (enforced by validate_config)
- No maximum constraint
- SIGHUP hot-reload: yes -- cancel current expiry timer, restart with new interval. Consistent with existing SIGHUP behavior for ACL/trusted_peers.

### Claude's Discretion
- Exact validate_config error messages for new fields
- Whether to add a "timestamp_rejected" counter to metrics dump
- Test structure and organization for new validation paths
- PROTOCOL.md formatting and section organization

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `validate_config()` in `db/config/config.cpp:223` -- error accumulation pattern, add new field validation here
- `expiry_scan_loop()` in `db/peer/peer_manager.cpp:1824` -- timer-cancel pattern, replace hardcoded `std::chrono::seconds(60)` with config value
- `send_sync_rejected()` in `db/peer/peer_manager.cpp:2281` -- sends 1-byte payload, expand reason codes here
- Rejection handling in `db/peer/peer_manager.cpp:804-810` -- receiver-side byte-to-string mapping already exists (cooldown/session_limit/byte_rate)
- `SYNC_REJECT_*` constants in `db/peer/peer_manager.cpp:70-72` -- move to shared header, expand enum

### Established Patterns
- Config fields: add to `Config` struct in `config.h`, parse in `from_json`, validate in `validate_config`, log at startup in `main.cpp`
- SIGHUP reload: `reload_config()` in PeerManager already handles ACL/trusted_peers -- extend for expiry interval
- Timer-cancel: `expiry_timer_` pointer pattern already in place for clean shutdown and reload
- Step 0 validation: `MAX_BLOB_DATA_SIZE` check pattern in ingest -- replicate for timestamp

### Integration Points
- `db/config/config.h` -- add `expiry_scan_interval_seconds` field
- `db/peer/peer_manager.cpp` -- consume new config field in expiry_scan_loop, extend SIGHUP reload
- `db/engine/blob_engine.cpp` (or wherever ingest lives) -- add timestamp validation as Step 0
- `db/schemas/transport.fbs` -- no FlatBuffers change needed (reason stays as raw byte in payload)
- `db/PROTOCOL.md` -- document reason codes and timestamp validation

</code_context>

<specifics>
## Specific Ideas

- No backward compatibility constraints -- no one runs the node yet, even at v1.0.0. Free to restructure wire format and reason codes.
- Remember this context when updating documentation at the end of the milestone.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 54-operational-hardening*
*Context gathered: 2026-03-22*
