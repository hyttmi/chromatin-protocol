# Phase 83: Bidirectional Keepalive - Context

**Gathered:** 2026-04-04
**Status:** Ready for planning

<domain>
## Phase Boundary

Application-level heartbeat: node sends Ping to all TCP peers every 30 seconds, disconnects peers that are silent for 60 seconds (2 missed keepalive cycles). Detects dead TCP connections that OS-level TCP keepalive may miss.

</domain>

<decisions>
## Implementation Decisions

### Keepalive timer placement
- **D-01:** Single PeerManager coroutine. One timer fires every 30s, iterates all TCP peers, sends Ping to each via send_message(). Consistent with sync_timer_loop and expiry_scan_loop patterns. One timer for all peers.

### Silence detection
- **D-02:** Any received message resets the silence timer. Update last-activity timestamp on every decoded message (Pong, Data, BlobNotify, sync, etc.). If the peer is sending any traffic, it's alive. Most forgiving approach.
- **D-03:** last_recv_time_ lives on the Connection object. Updated in message_loop on every decoded message. PeerManager reads it during the keepalive check. Connection owns the data since it owns the message_loop.

### Disconnect behavior
- **D-04:** Immediate TCP close via conn->close(). No Goodbye message (peer is unresponsive anyway). on_peer_disconnected handles cleanup (cursor grace period from Phase 82).

### Claude's Discretion
- Clock type for last_recv_time_ (steady_clock vs system_clock)
- Whether to skip Ping for peers currently in sync (probably not — they're still alive)
- Keepalive interval configurability (hardcode 30s or add config field)
- Log level for keepalive disconnect (info vs warn)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Connection & message loop
- `db/net/connection.h` — Connection class definition
- `db/net/connection.cpp` — message_loop() with Ping/Pong handling (line ~762), send_message() via queue

### Peer management
- `db/peer/peer_manager.h` — PeerInfo struct, timer pointers, coroutine declarations
- `db/peer/peer_manager.cpp` — sync_timer_loop() (line ~2633), expiry_scan_loop() patterns, on_peer_disconnected()

### Requirements
- `.planning/REQUIREMENTS.md` — CONN-01, CONN-02

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `message_loop()` Ping/Pong handling — already receives Ping, replies with Pong via send queue. Pong case currently does nothing ("nothing to do for now").
- `send_message()` — queue-based, safe for concurrent Ping sends from PeerManager coroutine
- Timer-cancel coroutine pattern — used by sync_timer_loop, expiry_scan_loop, peer_flush_timer_loop

### Established Patterns
- PeerManager timer coroutines: `while (!stopping_) { timer; wait; work; }` with cancel pointer
- peers_ deque iteration for broadcast operations (sync_all_peers, on_blob_ingested fan-out)

### Integration Points
- `Connection::message_loop()` — add `last_recv_time_` update on every decoded message
- `PeerManager::start()` — spawn keepalive_loop coroutine
- `PeerManager::stop()` — cancel keepalive timer
- `PeerManager` — new `keepalive_loop()` coroutine

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

*Phase: 83-bidirectional-keepalive*
*Context gathered: 2026-04-04*
