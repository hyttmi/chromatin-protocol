# Phase 40: Sync Rate Limiting - Context

**Gathered:** 2026-03-19
**Status:** Ready for planning

<domain>
## Phase Boundary

Metered sync requests per peer to prevent resource exhaustion via repeated sync initiation. Closes the abuse vector where sync messages bypass all existing rate limiting (Phase 18 token bucket only covers Data/Delete). Requirements: RATE-01, RATE-02, RATE-03.

</domain>

<decisions>
## Implementation Decisions

### Rejection behavior (RATE-01, RATE-03)
- Sync cooldown and concurrent session violations: reject and keep connection alive (NOT disconnect)
- New wire message type SyncRejected (type 30) with reason byte (cooldown, session limit, byte rate)
- Peer can still send Data/Delete after sync rejection — only sync is refused
- Rejection is metering, NOT misbehavior — does not feed into strike system
- Separate metrics counter for sync rejections (not reused from rate_limited)

### Sync byte accounting (RATE-02)
- Sync message bytes count against the existing per-peer token bucket (shared with Data/Delete)
- Both initiator and responder account sync bytes against the bucket (symmetric enforcement)
- Byte-rate exceed behavior during sync: Claude's discretion (disconnect like Phase 18, or reject-and-continue)

### Mid-sync interruption
- Check byte budget at namespace boundaries — current namespace reconciliation completes fully, but no new namespace starts if budget exceeded
- When sync is cut short due to byte budget, send SyncComplete early (not SyncRejected) — peer sees a shorter sync, remaining namespaces sync next round
- Responder mid-sync cutoff: stop responding silently, let initiator hit SYNC_TIMEOUT (30s). No mid-stream SyncRejected complexity.

### Config defaults
- Sync cooldown: 30 seconds default (allows sync twice per sync interval)
- Max concurrent sync sessions: 1 (keep serial, matches existing syncing bool pattern)
- Sync rate limiting enabled by default (cooldown=30, max_sessions=1) — this is closing an abuse vector, not an opt-in feature
- Cooldown=0 disables sync cooldown (operator override)
- All sync rate limit params SIGHUP-reloadable (follows Phase 18 pattern)

### Node-local policy (NOT protocol-affecting)
- Sync rate limits are node-local operational config, same as rate_limit_bytes_per_sec, max_peers, max_storage_bytes
- Different nodes having different cooldowns is expected and correct — each node enforces its own tolerance for incoming sync requests
- SyncRejected is a soft signal ("not now"), not a protocol incompatibility
- Follows locked decision: "No protocol-affecting config options"

### Claude's Discretion
- Byte-rate exceed during sync: disconnect (Phase 18 pattern) or reject-and-continue
- SyncRejected payload structure beyond reason byte
- Exact metrics counter naming and log verbosity
- PeerInfo fields for tracking cooldown timestamps and session count

</decisions>

<specifics>
## Specific Ideas

- The sync cooldown is about incoming sync INITIATION frequency. A peer that gets rejected just waits for next sync round — no harm done.
- Max sessions = 1 means the existing `syncing` bool essentially becomes the enforcement mechanism, just with a proper rejection message instead of silently ignoring.
- SyncRejected as a new message type follows the v0.8.0 pattern of adding wire message types (ReconcileInit=27, ReconcileRanges=28, ReconcileItems=29, now SyncRejected=30).

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `try_consume_tokens()` (peer_manager.cpp:39): Token bucket implementation, reusable for sync byte accounting — just call it for sync messages too
- `PeerInfo::bucket_tokens/bucket_last_refill` (peer_manager.h:59-60): Shared bucket fields, sync bytes feed into same bucket
- `PeerInfo::syncing` (peer_manager.h:50): Boolean already enforces serial sync — extend to proper session tracking with rejection
- `NodeMetrics::rate_limited` (peer_manager.h:69): Existing counter for data rate limiting — add parallel counter for sync rejections
- `reload_config()` (peer_manager.cpp:1526): SIGHUP reload infrastructure, add sync rate limit param reload

### Established Patterns
- Rate limit check at on_peer_message() before processing (Step 0 pattern) — extend to sync message types
- Token bucket per-peer in PeerInfo struct — add cooldown timestamp field
- Config struct simple types — add sync_cooldown_seconds, max_sync_sessions
- SIGHUP reload for rate params — same pattern for new fields

### Integration Points
- `on_peer_message()` (peer_manager.cpp:444): Currently skips rate check for sync messages — add sync byte accounting here
- `handle_sync_as_responder()` (peer_manager.cpp:1033): Check cooldown before starting responder sync
- `run_sync_with_peer()` (peer_manager.cpp:631): Check byte budget at namespace loop boundaries
- `Config` struct (config.h): Add sync_cooldown_seconds, max_sync_sessions fields
- `transport.fbs`: Add SyncRejected=30 message type
- `PeerInfo`: Add last_sync_time field for cooldown tracking

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 40-sync-rate-limiting*
*Context gathered: 2026-03-19*
