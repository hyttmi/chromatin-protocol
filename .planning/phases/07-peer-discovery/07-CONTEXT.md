# Phase 7: Peer Discovery - Context

**Gathered:** 2026-03-05
**Status:** Ready for planning

<domain>
## Phase Boundary

Nodes discover peers beyond their bootstrap list via peer exchange protocol. After connecting to a bootstrap peer, a node requests and receives a peer list, then connects to discovered peers it doesn't already know about. This phase adds peer exchange messages, discovery logic, and peer persistence. It does NOT add DHT, mDNS, or any other discovery mechanism — pure peer exchange only.

</domain>

<decisions>
## Implementation Decisions

### Exchange trigger & timing
- Request peers on connect (after handshake) AND periodically on a 5-minute timer
- Separate message flow — new PeerExchange request/response message types in transport.fbs, independent of sync protocol
- Ask ALL connected peers for their peer lists, not just bootstrap peers

### Peer list content
- Share address only (host:port) — peer identity is discovered during handshake
- Share a random subset of known peers per exchange (not the full list)
- Only share peers the node has actually connected to and authenticated (verified peers only)

### Connection policy
- Only bootstrap peers get reconnect loops with exponential backoff (existing behavior)
- Discovered peers are ephemeral — if they disconnect, re-discover them through future peer exchanges
- No reconnect retry for discovered peers

### Peer persistence
- Persist discovered peer addresses to disk so they survive node restarts
- On restart, connect to bootstrap peers AND previously-known persisted peers

### Claude's Discretion
- Connection strategy for newly discovered peers (immediate vs throttled, subset selection)
- Dedup approach (by address, by pubkey after handshake, or both)
- Whether to split max_peers into inbound/outbound limits or keep single pool
- Filtering logic when building peer list responses (excluding requester, self, etc.)
- Persistence format and location (JSON file vs libmdbx, in data_dir)
- Pruning strategy for stale persisted peers (failed connection count vs TTL-based)
- Capacity limit for persisted peer list
- Max peers to share per exchange response

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `PeerManager` (src/peer/peer_manager.h): Already tracks peers in `vector<PeerInfo>` with address, pubkey, bootstrap flag, strike count. Natural home for discovery logic.
- `PeerManager::sync_timer_loop()`: Existing periodic timer pattern using `asio::steady_timer`. Peer exchange timer can follow the same pattern.
- `Server::connect_to_peer()` / `Server::reconnect_loop()`: Existing outbound connection logic. Discovery will need to call connect_to_peer for new addresses.
- `TransportCodec` + `transport.fbs`: FlatBuffers transport with `TransportMsgType` enum. New PeerExchange types (16, 17+) will be added here.
- `config::Config::bootstrap_peers` / `max_peers`: Existing config fields for peer limits and bootstrap list.

### Established Patterns
- Single io_context thread — PeerManager is NOT thread-safe, all operations run on one thread
- Coroutine-based async (asio::awaitable, co_spawn with detached)
- Message routing through `on_peer_message()` dispatch by TransportMsgType
- Timer-cancel pattern for sync message queue (`sync_notify` pointer + `recv_sync_msg`)
- Strike system for misbehaving peers (record_strike → disconnect at threshold)

### Integration Points
- `PeerManager::on_peer_connected()`: Add peer exchange request after handshake (initiator side)
- `PeerManager::on_peer_message()`: Add dispatch for new PeerExchange message types
- `transport.fbs`: Add PeerListRequest/PeerListResponse (or similar) to TransportMsgType enum
- `config::Config`: May need new fields (peer_exchange_interval, max_persisted_peers) or can use hardcoded constants
- `Server`: May need public `connect_to_peer()` or new method for PeerManager to trigger outbound connections to discovered addresses

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 07-peer-discovery*
*Context gathered: 2026-03-05*
