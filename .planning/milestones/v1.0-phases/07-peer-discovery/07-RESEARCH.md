# Phase 7: Peer Discovery - Research

**Researched:** 2026-03-05
**Domain:** Peer exchange protocol, peer persistence, connection policy
**Confidence:** HIGH

## Summary

Phase 7 adds peer exchange (PEX) to chromatindb so nodes can discover peers beyond their bootstrap list. The implementation adds two new FlatBuffers message types (PeerListRequest/PeerListResponse), a periodic exchange timer in PeerManager, peer persistence to a JSON file in data_dir, and connection logic for discovered peers. The codebase already has all the patterns needed: timer loops (sync_timer_loop), message dispatch (on_peer_message), connection callbacks (on_peer_connected), and FlatBuffers transport encoding.

**Primary recommendation:** Add PEX as a self-contained extension to PeerManager with new message types in transport.fbs. Follow the existing sync_timer_loop pattern for periodic exchange, use a simple JSON file for persistence, and keep discovered peers ephemeral (no reconnect loops).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Request peers on connect (after handshake) AND periodically on a 5-minute timer
- Separate message flow -- new PeerExchange request/response message types in transport.fbs, independent of sync protocol
- Ask ALL connected peers for their peer lists, not just bootstrap peers
- Share address only (host:port) -- peer identity is discovered during handshake
- Share a random subset of known peers per exchange (not the full list)
- Only share peers the node has actually connected to and authenticated (verified peers only)
- Only bootstrap peers get reconnect loops with exponential backoff (existing behavior)
- Discovered peers are ephemeral -- if they disconnect, re-discover them through future peer exchanges
- No reconnect retry for discovered peers
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

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| DISC-02 | Node receives peer lists from bootstrap nodes and connects to discovered peers | PeerExchange request/response messages, peer exchange timer, connect_to_discovered_peer logic, peer persistence for restarts |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | Timers, async IO, coroutines | Already in use for all networking |
| FlatBuffers | latest | PeerListRequest/PeerListResponse wire format | Already used for all transport messages |
| nlohmann/json | latest | Persisted peers file format | Already a project dependency, simple for small data |
| spdlog | latest | Structured logging | Already in use throughout |

### Supporting
No new libraries needed. Everything builds on existing deps.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| JSON for peer persistence | libmdbx table | Overkill for a small list of addresses; JSON is human-readable and debuggable |
| FlatBuffers for PEX messages | Binary encoding (like sync) | FlatBuffers matches the transport layer pattern; sync uses binary because it predated the transport codec integration |

## Architecture Patterns

### Recommended Additions
```
src/
├── peer/
│   ├── peer_manager.h     # Add PEX methods + PeerPersistence
│   ├── peer_manager.cpp   # Add PEX implementation
│   └── (no new files -- keep it simple)
schemas/
└── transport.fbs          # Add PeerListRequest = 16, PeerListResponse = 17
tests/
├── peer/
│   └── test_peer_manager.cpp  # Unit tests for PEX logic
└── test_daemon.cpp            # E2E test: 3-node peer discovery
```

### Pattern 1: Timer-Based Periodic Exchange
**What:** A new `pex_timer_loop()` coroutine, identical pattern to existing `sync_timer_loop()`.
**When to use:** Periodic peer exchange every 5 minutes.
**Example:**
```cpp
asio::awaitable<void> PeerManager::pex_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(PEX_INTERVAL_SEC));
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        co_await request_peers_from_all();
    }
}
```

### Pattern 2: Message Dispatch Extension
**What:** Add PeerListRequest/PeerListResponse handling in `on_peer_message()`.
**When to use:** When receiving PEX messages from peers.
**Example:**
```cpp
// In on_peer_message():
if (type == wire::TransportMsgType_PeerListRequest) {
    auto response = build_peer_list_response(conn);
    conn->send_message(wire::TransportMsgType_PeerListResponse, response);
    return;
}
if (type == wire::TransportMsgType_PeerListResponse) {
    handle_peer_list_response(conn, std::move(payload));
    return;
}
```

### Pattern 3: Fire-and-Forget Outbound Connection
**What:** Discovered peers connect once without reconnect loops. Call a new method that connects, handshakes, but does NOT enter reconnect_loop on failure.
**When to use:** Connecting to newly discovered peer addresses.
**Key insight:** Server::connect_to_peer() currently falls into reconnect_loop on failure. Discovered peers need a separate path that tries once and drops.

### Anti-Patterns to Avoid
- **Reconnect loops for discovered peers:** Only bootstrap peers get reconnect. Discovered peers are ephemeral.
- **Sharing unverified peers:** Only share peers we have successfully handshaked with (authenticated). Never share addresses we only heard about from other peers.
- **Full peer list sharing:** Always send a random subset to prevent topology mapping attacks.
- **Blocking main thread with persistence IO:** Write peer file asynchronously or on a separate strand if needed. In practice, writing a small JSON file is fast enough to do synchronously on the io_context thread.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Random subset selection | Custom Fisher-Yates | `std::shuffle` + `std::min` truncation | Standard, correct, tested |
| JSON persistence | Custom file format | nlohmann/json dump/parse | Already a dependency, handles edge cases |
| Address dedup | Complex pubkey-based dedup | Simple address-string set comparison | Addresses are unique per peer listen endpoint; pubkey dedup can happen post-handshake as secondary check |

## Common Pitfalls

### Pitfall 1: Port Mismatch Between Listen and Ephemeral Ports
**What goes wrong:** When peer A connects outbound to peer B, A sees B's listen port in the bootstrap config, but B sees A's ephemeral port. If A shares B's address, it should share B's listen address, not the ephemeral connection port.
**Why it happens:** TCP connections use ephemeral source ports. The remote_address() of a connection shows the ephemeral port, not the listen port.
**How to avoid:** Track each peer's *listen address* separately from their connection address. For bootstrap peers, this is the config address. For discovered peers, this is the address from the PeerListResponse. For inbound connections, the listen address is unknown unless the peer announces it.
**Warning signs:** Peer lists contain ephemeral ports that other nodes can't connect to.

### Pitfall 2: Self-Connection
**What goes wrong:** A node discovers its own address in a peer list and tries to connect to itself.
**Why it happens:** When node A shares its peer list with node B, node A's own address might be in the list from another peer's perspective.
**How to avoid:** Filter out self when processing discovered peers. Compare against own bind_address. Also reject connections where peer_pubkey matches our own pubkey (post-handshake check).

### Pitfall 3: Duplicate Connections
**What goes wrong:** Two nodes simultaneously discover each other and both initiate connections, resulting in duplicate connections.
**Why it happens:** Peer exchange happens periodically and independently on each node.
**How to avoid:** Before connecting to a discovered peer, check if we already have a connection to that address. Also check connected peer pubkeys post-handshake and drop duplicates.

### Pitfall 4: Connection Storm
**What goes wrong:** Node discovers 50 new peers and tries to connect to all simultaneously, overwhelming the network or hitting file descriptor limits.
**Why it happens:** Naive "connect to all discovered" logic.
**How to avoid:** Connect to a limited batch of discovered peers per exchange round (e.g., 3-5 at a time). Respect max_peers limit before initiating any connections.

### Pitfall 5: Stale Persisted Peers
**What goes wrong:** Persisted peer file grows unbounded with addresses that no longer work, causing slow startup as the node tries to connect to many dead addresses.
**Why it happens:** No pruning of failed addresses.
**How to avoid:** Track connection failure count per persisted address. Remove after N consecutive failures (e.g., 3). Cap total persisted peers (e.g., 100).

### Pitfall 6: Server::connect_to_peer Enters Reconnect Loop
**What goes wrong:** Using the existing `Server::connect_to_peer()` for discovered peers causes them to get infinite reconnect loops (bootstrap behavior).
**Why it happens:** `connect_to_peer()` calls `reconnect_loop()` on failure/disconnect.
**How to avoid:** Need a separate connection path for discovered peers -- either a new `Server::connect_once()` method or handle the logic directly in PeerManager.

## Code Examples

### Peer List Wire Encoding (FlatBuffers transport.fbs)
```flatbuffers
// Add to TransportMsgType enum:
PeerListRequest = 16,
PeerListResponse = 17
```

The payload for PeerListRequest is empty (no body needed).
The payload for PeerListResponse is a simple address list encoding:
```
[uint16: count][for each: uint16 addr_len, utf8 addr_bytes]
```
This matches the binary encoding style used for sync payloads (big-endian, simple, no FlatBuffers for payloads).

### Peer Persistence JSON Format
```json
{
  "peers": [
    {"address": "192.168.1.10:4200", "last_seen": 1741190400, "fail_count": 0},
    {"address": "10.0.0.5:4200", "last_seen": 1741186800, "fail_count": 1}
  ]
}
```
File location: `{data_dir}/peers.json`

### Random Subset Selection
```cpp
std::vector<std::string> select_random_peers(
    const std::vector<std::string>& all_peers, size_t max_count)
{
    auto result = all_peers;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(result.begin(), result.end(), gen);
    if (result.size() > max_count) result.resize(max_count);
    return result;
}
```

### Connect-Once Pattern for Discovered Peers
```cpp
// New Server method (or PeerManager-level logic):
asio::awaitable<void> Server::connect_once(const std::string& address) {
    auto [host, port] = parse_address(address);
    asio::ip::tcp::resolver resolver(ioc_);
    auto [ec_resolve, endpoints] = co_await resolver.async_resolve(host, port, use_nothrow);
    if (ec_resolve) co_return;  // Give up, no reconnect

    asio::ip::tcp::socket socket(ioc_);
    auto [ec_connect, ep] = co_await asio::async_connect(socket, endpoints, use_nothrow);
    if (ec_connect) co_return;  // Give up, no reconnect

    auto conn = Connection::create_outbound(std::move(socket), identity_);
    connections_.push_back(conn);
    conn->on_close([this](Connection::Ptr c, bool) {
        remove_connection(c);
        if (on_disconnected_) on_disconnected_(c);
    });
    conn->on_ready([this](Connection::Ptr c) {
        if (on_connected_) on_connected_(c);
    });
    co_await conn->run();
    // Connection ended -- no reconnect for discovered peers
}
```

## Discretion Recommendations

### Connection Strategy
**Recommendation:** Connect to up to 3 discovered peers per exchange round, immediate (no throttle/delay). Check `peers_.size() < config_.max_peers` before each connection attempt.
**Rationale:** 3 is enough to grow the network without overwhelming. The max_peers cap naturally limits growth.

### Dedup Approach
**Recommendation:** Primary dedup by address string (before connecting). Secondary dedup by pubkey (post-handshake, in on_peer_connected). If a newly connected peer has the same pubkey as an existing peer, close the newer connection.
**Rationale:** Address dedup is cheap and catches most cases. Pubkey dedup catches edge cases (same node, different address).

### max_peers Pool
**Recommendation:** Keep single pool. Do NOT split into inbound/outbound limits.
**Rationale:** YAGNI. The single max_peers limit already works. Splitting adds complexity for no immediate benefit.

### Filtering Logic for Responses
**Recommendation:** When building a PeerListResponse, exclude: (1) the requester's address, (2) our own bind_address. Include only peers with `is_authenticated() == true`.
**Rationale:** Don't tell a peer about itself. Don't share our own address (they already know it). Only share verified peers.

### Persistence Format
**Recommendation:** JSON file at `{data_dir}/peers.json` using nlohmann/json.
**Rationale:** Human-readable, debuggable, uses existing dependency. The peer list is small (max ~100 entries).

### Pruning Strategy
**Recommendation:** Track `fail_count` per persisted peer. Increment on connection failure, reset to 0 on successful connection. Remove when `fail_count >= 3`. Cap persisted list at 100 entries (drop oldest-last_seen when full).
**Rationale:** Simple, predictable. 3 consecutive failures is enough to determine a peer is gone.

### Persisted Peer Capacity
**Recommendation:** 100 peers max.
**Rationale:** More than enough for network discovery. Small enough to iterate quickly.

### Max Peers Per Exchange
**Recommendation:** Share up to 8 peers per PeerListResponse.
**Rationale:** Enough to learn new peers, not enough to map the full topology.

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Full peer list sharing | Random subset (gossip-style) | Standard practice | Prevents topology mapping |
| DHT for discovery | Bootstrap + PEX | Project decision | Simpler, proven reliable |

## Open Questions

1. **Listen address announcement for inbound peers**
   - What we know: For outbound connections, we know the peer's listen address (it's the address we connected to). For inbound connections, we don't know the peer's listen address.
   - What's unclear: Should peers announce their listen address during PEX?
   - Recommendation: For v1, only share listen addresses of peers we connected outbound to (we know their listen address). Inbound-only peers won't be shared in PEX responses. This is safe and simple. A future PeerAnnounce message could solve this.

## Sources

### Primary (HIGH confidence)
- Existing codebase analysis: peer_manager.h/.cpp, server.h/.cpp, connection.h, transport.fbs
- CONTEXT.md user decisions from discuss-phase

### Secondary (MEDIUM confidence)
- BitTorrent PEX (BEP 11) - established peer exchange protocol patterns
- libp2p peer exchange - subset sharing and verified-only peer lists

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - all libraries already in project
- Architecture: HIGH - all patterns already exist in codebase (timers, dispatch, connections)
- Pitfalls: HIGH - derived from analysis of existing connect_to_peer/reconnect_loop code

**Research date:** 2026-03-05
**Valid until:** 2026-04-05 (stable domain, no fast-moving deps)
