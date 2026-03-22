# Phase 34: Sync Resumption - Context

**Gathered:** 2026-03-17
**Status:** Ready for planning

<domain>
## Phase Boundary

Per-peer per-namespace cursors that transform sync cost from O(total_blobs) to O(new_blobs). Cursors track seq_num per namespace per peer, skip hash exchange for unchanged namespaces, persist across restarts, and fall back to periodic full resync. Requirements: SYNC-01, SYNC-02, SYNC-03, SYNC-04.

</domain>

<decisions>
## Implementation Decisions

### Peer identity for cursors
- Cursors keyed by SHA3-256(peer_pubkey) (32 bytes), not by address — survives IP changes, port changes, reconnects
- Flat composite key in single libmdbx sub-database: `[peer_pubkey_hash:32][namespace:32] -> [seq_num:8][round_count:4]`
- Consistent with existing sub-db patterns (blobs, delegation, tombstone)
- On-demand reads from libmdbx during sync round (no in-memory cursor cache in PeerInfo)
- Round counter persisted alongside seq_num to survive restarts — avoids unnecessary full resync after restart

### Full resync triggers
- **Periodic (SYNC-04):** Every Nth round, configurable via `full_resync_interval` in config.json, default 10, SIGHUP-reloadable
- **SIGHUP config reload:** Reset all round counters to force next sync with each peer to be full. Do NOT wipe cursor seq_nums — if full resync confirms they're still valid, no work wasted
- **Large time gap:** If time since last sync with a peer exceeds a threshold, force full resync. Claude's discretion on the threshold value
- **Cursor mismatch detection:** During cursor-based sync, if remote seq_num < our stored cursor for that namespace, reset that namespace's cursor only (per-namespace, not per-peer). Auto-recover via full diff for that namespace

### Cursor reset behavior
- Startup cleanup scan: on node start, scan all cursor entries, compare peer_pubkey_hashes against connected + persisted peers (peers.json), delete entries for unknown peer hashes
- No GC-to-cursor coupling: tombstone GC and blob expiry do NOT trigger cursor adjustments. Cursors track seq_num, gaps from GC are handled by existing get_blobs_by_seq skip pattern. Periodic full resync catches edge cases
- Key rotation: old pubkey's cursors cleaned up by startup scan. New pubkey starts fresh (full sync on first round)

### Observability
- Two-level logging: info-level summary line (extend existing sync log with cursor stats) + debug-level per-namespace lines
- Info line format includes: skipped-via-cursor count, full-diff count, round N/M
- NodeMetrics gets three new counters: `cursor_hits`, `cursor_misses`, `full_resyncs` — visible in SIGUSR1 dump and periodic metrics
- Log levels: info for periodic full resync (expected), warn for event-triggered full resync (SIGHUP, time gap, mismatch)
- Startup cursor cleanup: info-level summary ("removed N entries for M unknown peers"), no per-entry detail

### Claude's Discretion
- Time gap threshold for full resync trigger (reasonable default, configurable)
- Wire protocol changes needed to support cursor-based sync (new message types vs extending existing)
- Exact cursor value encoding in libmdbx (endianness, packing)
- How cursor-based sync integrates into existing Phase A/B/C flow in run_sync_with_peer

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Storage::get_blobs_by_seq(ns, since_seq)`: Already supports querying blobs since a seq_num — natural fit for cursor-based retrieval
- `Storage::list_namespaces()`: Returns NamespaceInfo with `latest_seq_num` — can compare against stored cursor
- `SyncProtocol::encode/decode_namespace_list()`: Already encodes namespace + seq_num on wire — cursor comparison info already in the protocol
- libmdbx sub-database pattern: Five existing sub-dbs (blobs, sequence, expiry, delegation, tombstone) — adding a sixth (cursors) follows established pattern
- SIGHUP handler in PeerManager: Already wired for config reload — can add round counter reset
- `PersistedPeer` / `persisted_peers_` / peers.json: Existing peer persistence for startup cursor cleanup cross-reference

### Established Patterns
- Flat composite keys: `[namespace:32][hash:32]` (blobs), `[namespace:32][delegate_pk_hash:32]` (delegation) — cursor key `[peer_hash:32][ns:32]` is consistent
- Timer-cancel pattern for async loops — existing sync_timer_loop, expiry_scan_loop
- SIGHUP reload: `handle_sighup()` → `reload_config()` — extend to reset round counters
- NodeMetrics: plain uint64_t counters, monotonically increasing, single io_context thread
- pimpl in Storage: cursor sub-db lives inside Storage::Impl

### Integration Points
- `PeerManager::run_sync_with_peer()`: Main sync orchestration coroutine — needs cursor lookup/update logic
- `PeerManager::handle_sync_as_responder()`: Responder side of sync — may also need cursor awareness
- `Storage::Impl` constructor: Open the new cursor sub-database alongside existing five
- `PeerManager::handle_sighup()`: Add round counter reset
- `PeerManager::start()`: Add startup cursor cleanup scan

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

*Phase: 34-sync-resumption*
*Context gathered: 2026-03-17*
