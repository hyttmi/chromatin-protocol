# Phase 96: PeerManager Architecture - Context

**Gathered:** 2026-04-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Split the 4187-line PeerManager god object (`db/peer/peer_manager.cpp`) into 6 focused components with clear responsibilities. PeerManager public API remains unchanged -- it becomes a thin facade that delegates to components. No new features, no behavioral changes, no wire protocol changes.

</domain>

<decisions>
## Implementation Decisions

### Component Boundaries (6 components)
- **D-01:** ConnectionManager -- connection lifecycle (on_peer_connected, on_peer_disconnected, should_accept_connection, announce_and_sync), keepalive loop, strike system. **Owns the `peers_` deque.**
- **D-02:** MessageDispatcher -- the on_peer_message routing switch and route_sync_message. Receives messages and delegates to the appropriate component.
- **D-03:** SyncOrchestrator -- sync protocol (run_sync_with_peer, handle_sync_as_responder, recv_sync_msg, sync_all_peers, sync_timer_loop), expiry scanning (expiry_scan_loop), cursor compaction (cursor_compaction_loop), storage compaction (compaction_loop).
- **D-04:** PexManager -- peer exchange protocol (pex_timer_loop, run_pex_with_peer, handle_pex_as_responder, handle_peer_list_response, request_peers_from_all), peer persistence (load/save/update_persisted_peer, peer_flush_timer_loop).
- **D-05:** BlobPushManager -- blob notify/fetch (on_blob_notify, handle_blob_fetch, handle_blob_fetch_response), on_blob_ingested fan-out (BlobNotify to peers + Notification to subscribers). Owns `pending_fetches_`.
- **D-06:** MetricsCollector -- Prometheus /metrics HTTP endpoint (metrics_accept_loop, metrics_handle_connection, start/stop_metrics_listener), periodic metrics logging (metrics_timer_loop, log_metrics_line), SIGUSR1 dump (dump_metrics). Owns `metrics_` counters.

### Leftover Distribution
- **D-07:** No 7th component. Remaining concerns distributed into the 6:
  - Keepalive + strike system → ConnectionManager
  - Expiry + cursor compaction + storage compaction → SyncOrchestrator
  - Peer persistence → PexManager
  - Signals (SIGHUP/SIGUSR1) + config reload → PeerManager facade (thin orchestration glue)
  - Metrics dump + Prometheus → MetricsCollector

### Decomposition Style
- **D-08:** All new component files live in `db/peer/`: connection_manager.h/.cpp, message_dispatcher.h/.cpp, sync_orchestrator.h/.cpp, pex_manager.h/.cpp, blob_push_manager.h/.cpp, metrics_collector.h/.cpp.
- **D-09:** PeerManager facade stays at `db/peer/peer_manager.h/.cpp` with unchanged public API. All callers (main.cpp, tests) continue using PeerManager directly.

### Inter-Component Wiring
- **D-10:** Reference injection at construction. Each component receives references/pointers to its dependencies. No shared context struct, no event bus.
- **D-11:** ConnectionManager owns `peers_` (the `std::deque<std::unique_ptr<PeerInfo>>`). Other components that need peer access get a reference to ConnectionManager.
- **D-12:** Direct method calls between components where cross-component interaction is needed (e.g., BlobPushManager calls SyncOrchestrator to check sync state). Circular dependencies avoided by careful layering.
- **D-13:** MetricsCollector owns `NodeMetrics`. Components that increment counters get a `NodeMetrics&`.

### Claude's Discretion
- Exact constructor signatures for each component
- Internal method organization within each component
- How to layer components to avoid circular include dependencies (forward declarations, interface abstractions if needed)
- Whether PeerManager facade owns components by value or unique_ptr
- How config reload propagates to components (direct call vs observer)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Source Files (decomposition targets)
- `db/peer/peer_manager.h` -- 391 lines, class declaration with all 46 methods and 50+ member variables to distribute
- `db/peer/peer_manager.cpp` -- 4187 lines, all method implementations to split across 6 components
- `db/peer/sync_reject.h` -- existing peer-directory file, sync rejection enum

### Dependencies (components will need these)
- `db/net/connection.h` -- Connection::Ptr, message callbacks
- `db/net/server.h` -- Server class (stays in PeerManager facade)
- `db/net/uds_acceptor.h` -- UDS acceptor (stays in PeerManager facade)
- `db/sync/sync_protocol.h` -- SyncProtocol (moves to SyncOrchestrator)
- `db/engine/engine.h` -- BlobEngine (used by SyncOrchestrator, BlobPushManager)
- `db/storage/storage.h` -- Storage (used by SyncOrchestrator, BlobPushManager)
- `db/config/config.h` -- Config struct
- `db/acl/access_control.h` -- AccessControl (used by ConnectionManager for auth)

### Phase 95 Utilities (reuse in new components)
- `db/util/endian.h` -- BE read/write helpers
- `db/util/blob_helpers.h` -- namespace/hash extraction
- `db/net/auth_helpers.h` -- auth payload encode/decode
- `db/crypto/verify_helpers.h` -- verify_with_offload

### Requirements
- `.planning/REQUIREMENTS.md` -- ARCH-01

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/peer/sync_reject.h` -- existing file in peer directory, establishes pattern for peer-domain headers
- Phase 95 utility headers -- shared helpers already extracted, components should use these
- `db/tests/test_helpers.h` -- shared test utilities (TempDir, make_signed_blob, listening_address)

### Established Patterns
- Namespace `chromatindb::peer` for all peer-domain code
- `#pragma once` header guards
- Timer-cancel pattern: `asio::steady_timer*` member, `cancel()` in stop, `co_await` in loop
- `net::Connection::Ptr` (shared_ptr) for connection ownership
- `std::deque<std::unique_ptr<PeerInfo>>` for heap-stable peer storage
- `find_peer()` re-lookup after every co_await (coroutine safety pattern from post-Phase 93 bugfix)

### Integration Points
- `main.cpp` constructs PeerManager and calls start()/stop() -- unchanged
- ~30 test files reference PeerManager directly -- public API unchanged means no test rewrites for callers
- Engine callbacks (on_blob_ingested) route through PeerManager -- facade delegates to BlobPushManager
- Server callbacks (on_peer_connected/disconnected) -- facade delegates to ConnectionManager

### Method Count by Component (from codebase scan)
- ConnectionManager: ~8 methods (~400 lines)
- MessageDispatcher: ~3 methods (~1200 lines including the big switch)
- SyncOrchestrator: ~10 methods (~1700 lines)
- PexManager: ~8 methods (~350 lines)
- BlobPushManager: ~5 methods (~350 lines)
- MetricsCollector: ~8 methods (~300 lines)
- PeerManager facade: ~8 methods (~200 lines -- start, stop, config reload, signal setup)

</code_context>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches for the decomposition.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 96-peermanager-architecture*
*Context gathered: 2026-04-08*
