# Phase 82: Reconcile-on-Connect & Safety Net - Context

**Gathered:** 2026-04-04
**Status:** Ready for planning

<domain>
## Phase Boundary

Peers catch up on missed blobs via full reconciliation on connect, with a long-interval safety net (600s default) as correctness backstop. Disconnected peer cursors are preserved for 5 minutes for cursor reuse on reconnect. The `sync_interval_seconds` config field is replaced by `safety_net_interval_seconds`.

</domain>

<decisions>
## Implementation Decisions

### Cursor grace period
- **D-01:** Preserve SyncProtocol cursor state per namespace when a peer disconnects. Store in a map keyed by peer pubkey hash with disconnect timestamp. On reconnect within 5 minutes, restore cursors so sync resumes from last position instead of full reconciliation.
- **D-02:** Lazy cleanup on reconnect check. Store disconnect timestamp with cursor state. On reconnect, check if within 5 min — reuse or discard. Periodically scan (e.g. during safety-net cycle) to free stale entries. No per-peer timer.

### Safety net interval
- **D-03:** Sync all peers each cycle. Every 600s, run sync_all_peers() as currently implemented. Simple, complete, long enough that cost is negligible.
- **D-04:** Bypass cooldown for safety net. Safety-net sync always runs regardless of cooldown. Cooldown exists to prevent rapid-fire from aggressive peers, not from the node's own safety net.

### Connect reconciliation order
- **D-05:** Use existing syncing flag to gate BlobNotify. Phase 79's on_blob_ingested already skips BlobNotify to peers with syncing=true (storm suppression). Sync-on-connect naturally gates BlobNotify — no new mechanism needed.
- **D-06:** Keep initiator-only sync-on-connect. Current pattern: initiator sends SyncRequest, responder handles it. No change to the existing pattern.

### Config migration
- **D-07:** Rename in-place, no backward compat. Replace `sync_interval_seconds` with `safety_net_interval_seconds` in config.h. Default 600s. Old configs with the old field name are silently ignored. Pre-MVP: no backward compat needed.

### Claude's Discretion
- Data structure for storing disconnected peer cursor state (map type, what exactly the cursor contains)
- How cursor state is extracted from SyncProtocol on disconnect (snapshot method or direct copy)
- How cursor state is restored on reconnect (SyncProtocol setter or constructor parameter)
- Whether the safety-net loop is the same coroutine as the old sync_timer_loop or a new one
- How stale cursor entries are cleaned up during the periodic scan
- Validation rules for safety_net_interval_seconds (minimum, maximum)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Peer management & sync
- `db/peer/peer_manager.h` — PeerInfo struct (syncing flag), sync_timer_, peers_ deque
- `db/peer/peer_manager.cpp` — on_peer_connected() (line ~314), on_peer_disconnected() (line ~468), sync_timer_loop() (line ~2633), sync_all_peers() (line ~2620), run_sync_with_peer() (line ~1797)

### Sync protocol
- `db/sync/sync_protocol.h` — SyncProtocol class, cursor state, OnBlobIngested callback
- `db/sync/sync_protocol.cpp` — sync session implementation, cursor management

### Config
- `db/config/config.h` — sync_interval_seconds (to be replaced), sync_cooldown_seconds
- `db/config/config.cpp` — JSON parsing for config fields

### Requirements
- `.planning/REQUIREMENTS.md` — MAINT-04, MAINT-05, MAINT-06, MAINT-07

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `sync_timer_loop()` — existing periodic sync coroutine, adapt for 600s safety-net interval
- `sync_all_peers()` — existing method to sync with all connected peers, reuse as-is
- `run_sync_with_peer()` — full reconciliation implementation, reuse as-is
- `PeerInfo.syncing` flag — already gates BlobNotify delivery (Phase 79 D-06/D-07)
- `sync_cooldown_seconds_` — existing cooldown mechanism, safety-net bypasses it

### Established Patterns
- Timer-cancel coroutine pattern for sync_timer_loop
- on_peer_connected initiator-only sync spawn
- PeerInfo deque iteration for sync_all_peers

### Integration Points
- `on_peer_disconnected()` — add cursor state preservation before removing from peers_
- `on_peer_connected()` — check for preserved cursor state, restore if within grace period
- `sync_timer_loop()` — change interval from sync_interval_seconds to safety_net_interval_seconds, add stale cursor cleanup
- `config.h` — rename field, update parsing

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

*Phase: 82-reconcile-on-connect-safety-net*
*Context gathered: 2026-04-04*
