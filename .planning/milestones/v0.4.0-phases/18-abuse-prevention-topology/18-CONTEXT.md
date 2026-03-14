# Phase 18: Abuse Prevention & Topology - Context

**Gathered:** 2026-03-11
**Status:** Ready for planning

<domain>
## Phase Boundary

Open nodes resist write-flooding abuse via per-connection rate limiting, and operators control which namespaces the node replicates via a configurable filter. Requirements: PROT-01 through PROT-06.

</domain>

<decisions>
## Implementation Decisions

### Rate limit defaults & granularity
- Unit: bytes per second (total payload throughput), not message count
- Scope: per-connection only (one token bucket per connected peer)
- Default rate and burst values: Claude's discretion based on protocol characteristics

### Strike integration on rate exceed
- Rate limit exceeded = immediate disconnect via close_gracefully() — no strike system involvement
- Strike system remains separate for validation failures (bad signatures, malformed messages)
- Reconnect allowed immediately — stateless, no cooldown ban
- Metrics counter and log detail level: Claude's discretion

### Hot-reload scope
- Rate limit parameters (bytes/sec, burst) are SIGHUP-reloadable — follows allowed_keys pattern
- sync_namespaces is SIGHUP-reloadable — existing sync sessions complete, new syncs use updated filter
- Blobs in now-filtered namespaces: kept in storage but not synced (safe, reversible)
- Namespace filter applies to both sync Phase A AND ingest (Data/Delete messages) — consistent operator model: "this node doesn't handle that namespace"

### Namespace filter config format
- Format: 64-char hex hashes, consistent with allowed_keys
- Model: whitelist only — sync_namespaces lists what TO replicate, empty = replicate all (PROT-06, mirrors implicit closed mode from allowed_keys)
- Reject behavior for filtered namespace blobs: Claude's discretion (silent drop vs error response)
- Startup validation: Claude's discretion (follow validate_allowed_keys pattern or not)

### Claude's Discretion
- Default rate_limit_bytes_per_sec value
- Default rate_limit_burst value (burst factor)
- rate_limited metric semantics (disconnection events vs dropped messages)
- Log verbosity for rate-limit disconnects
- Reject behavior when peer sends blob to filtered namespace
- Startup validation of sync_namespaces entries

</decisions>

<specifics>
## Specific Ideas

- Immediate disconnect matches the "no backpressure delay" requirement literally — don't slow the peer, cut them
- The allowed_keys implicit-closed-mode pattern (non-empty list = closed) should be replicated exactly for sync_namespaces
- Filtering at ingest prevents storing blobs that would never replicate — no wasted storage

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `record_strike()` in peer_manager.cpp: existing strike system, rate limiting is separate but the disconnect pattern (close_gracefully) is reusable
- `NodeMetrics::rate_limited`: stub counter already exists at 0, ready to be wired
- `validate_allowed_keys()` in config.h: validation pattern for hex string arrays, reusable for sync_namespaces
- `reload_config()` + SIGHUP handler: established hot-reload pattern for adding new reloadable fields

### Established Patterns
- PeerInfo struct holds per-connection state (strike_count, peer_is_full) — token bucket fields go here
- Config struct uses simple types (uint64_t, vector<string>) — rate limit and namespace fields follow this
- on_peer_message() routes Data/Delete messages — rate limit check goes before processing (Step 0 pattern: cheapest check first)
- Single io_context thread — no concurrency issues with token bucket state

### Integration Points
- on_peer_message(): rate limit check before Data/Delete processing
- engine_.list_namespaces() in sync Phase A: filter before encode_namespace_list()
- reload_config(): add rate limit param reload and sync_namespaces reload
- Config::load_config(): parse new JSON fields
- PeerInfo: add token bucket fields

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 18-abuse-prevention-topology*
*Context gathered: 2026-03-11*
