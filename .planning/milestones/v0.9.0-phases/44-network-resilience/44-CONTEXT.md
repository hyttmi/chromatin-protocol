# Phase 44: Network Resilience - Context

**Gathered:** 2026-03-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Node maintains persistent connectivity to its peer network -- automatically reconnecting on disconnect, suppressing reconnection to ACL-rejecting peers, and detecting dead peers via inactivity timeout. Three requirements: CONN-01, CONN-02, CONN-03.

</domain>

<decisions>
## Implementation Decisions

### Claude's Discretion

User delegated all decisions for this infrastructure phase. Claude has full flexibility on:

**Auto-reconnect with backoff (CONN-01):**
- Extend existing `Server::reconnect_loop()` with random jitter (currently missing)
- Backoff range: 1s to 60s exponential with jitter (per success criteria)
- Scope: all outbound peers (bootstrap AND discovered) -- CONN-01 says "outbound peers" not "bootstrap only"
- Current `connect_once()` for discovered peers needs reconnect capability
- Fix the `handshake_ok` bug: ACL rejection in `on_peer_connected` fires after `on_ready` sets `handshake_ok = true`, causing delay reset to 1s (tight retry loop)

**ACL-aware reconnection suppression (CONN-02):**
- Detection heuristic: connects, handshakes successfully, disconnects quickly with zero application messages = ACL rejection pattern
- Track rejection count per address; after threshold (Claude decides count), enter extended 600s backoff
- SIGHUP resets suppression state (already reloads ACL -- extend to clear rejection counters)
- ACL rejection detection lives in Server layer (it owns the reconnect loop) with signal from PeerManager (it does the ACL check)

**Inactivity timeout (CONN-03):**
- Receiver-side only (NOT Ping sender) -- avoids adding wire protocol messages and AEAD nonce desync
- Track last-message-received timestamp per connection
- Periodic check or per-connection timer (Claude decides mechanism)
- Configurable via config field (Claude decides field name and default)
- Applies to all connections (inbound and outbound) -- a dead peer is dead regardless of direction
- New config field validated by existing `validate_config()`
- New timer added to `cancel_all_timers()` following Phase 42 pattern

</decisions>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches. User trusts Claude's judgment on all implementation details for this infrastructure phase.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Server::reconnect_loop()`: Existing exponential backoff (1s→60s), needs jitter addition and ACL awareness
- `Server::connect_once()`: Discovered peer connector, needs reconnect wrapping
- `PeerManager::on_peer_connected()`: ACL check at line 274-283, fires conn->close() on rejection
- `PeerManager::cancel_all_timers()`: Phase 42 consolidation, ready for new timer additions
- `PeerManager::on_peer_message()`: Universal byte accounting entry point -- could track last-message time here
- `db/config/config.cpp:validate_config()`: Phase 42 validation framework for new config fields

### Established Patterns
- Timer ownership: `asio::steady_timer*` members, nullable, cancel via `->cancel()`
- Timer-cancel pattern for async loops (expiry, sync, pex, flush, metrics, cursor_compaction)
- Config loading: `nlohmann/json j.value()` with defaults, then `validate_config()`
- `on_ready` callback for post-handshake setup
- `on_close` callback for disconnect handling
- Coroutine-based loops with `asio::co_spawn(ioc_, ..., asio::detached)`

### Integration Points
- `Server::reconnect_loop()`: Refactor for jitter + ACL awareness
- `Server::connect_once()`: Add reconnect path for discovered peers
- `Server::connect_to_peer()`: Fix handshake_ok/ACL interaction
- `PeerManager::on_peer_connected()`: Signal ACL rejection back to Server for suppression tracking
- `PeerManager::on_peer_message()`: Track last-message timestamp for inactivity detection
- `PeerManager::run()`: Spawn inactivity check timer alongside existing timers
- `PeerManager::cancel_all_timers()`: Add inactivity timer
- `db/config/config.h`: Add inactivity timeout config field
- `db/config/config.cpp`: Validate new field in `validate_config()`

</code_context>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 44-network-resilience*
*Context gathered: 2026-03-20*
