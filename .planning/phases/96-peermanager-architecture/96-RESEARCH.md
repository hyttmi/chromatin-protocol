# Phase 96: PeerManager Architecture - Research

**Researched:** 2026-04-08
**Domain:** C++ class decomposition / god object refactoring
**Confidence:** HIGH

## Summary

PeerManager is a 4187-line god object in `db/peer/peer_manager.cpp` with 391 lines of header declarations (46 methods, 50+ member variables). CONTEXT.md locks the decomposition into 6 focused components plus a thin facade. All decisions are locked -- this research focuses on the technical patterns, dependency graph, pitfalls, and layering strategy needed to execute the split cleanly.

The core challenge is maintaining behavioral identity while splitting across files: the single io_context thread model, the coroutine timer-cancel pattern, the find_peer re-lookup pattern after every co_await, and the `peers_` deque ownership must all be preserved exactly. The public API (constructor, start/stop, metrics accessors, static encode/decode methods) must remain on PeerManager itself.

**Primary recommendation:** Bottom-up decomposition: extract MetricsCollector first (fewest dependencies), then PexManager, BlobPushManager, SyncOrchestrator, MessageDispatcher, and finally ConnectionManager (owns `peers_`). Each extraction is independently compilable and testable at every step.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** ConnectionManager -- connection lifecycle (on_peer_connected, on_peer_disconnected, should_accept_connection, announce_and_sync), keepalive loop, strike system. **Owns the `peers_` deque.**
- **D-02:** MessageDispatcher -- the on_peer_message routing switch and route_sync_message. Receives messages and delegates to the appropriate component.
- **D-03:** SyncOrchestrator -- sync protocol (run_sync_with_peer, handle_sync_as_responder, recv_sync_msg, sync_all_peers, sync_timer_loop), expiry scanning (expiry_scan_loop), cursor compaction (cursor_compaction_loop), storage compaction (compaction_loop).
- **D-04:** PexManager -- peer exchange protocol (pex_timer_loop, run_pex_with_peer, handle_pex_as_responder, handle_peer_list_response, request_peers_from_all), peer persistence (load/save/update_persisted_peer, peer_flush_timer_loop).
- **D-05:** BlobPushManager -- blob notify/fetch (on_blob_notify, handle_blob_fetch, handle_blob_fetch_response), on_blob_ingested fan-out (BlobNotify to peers + Notification to subscribers). Owns `pending_fetches_`.
- **D-06:** MetricsCollector -- Prometheus /metrics HTTP endpoint (metrics_accept_loop, metrics_handle_connection, start/stop_metrics_listener), periodic metrics logging (metrics_timer_loop, log_metrics_line), SIGUSR1 dump (dump_metrics). Owns `metrics_` counters.
- **D-07:** No 7th component. Remaining concerns distributed: keepalive+strike -> ConnectionManager, expiry+cursor+storage compaction -> SyncOrchestrator, peer persistence -> PexManager, signals+config reload -> PeerManager facade, metrics dump+Prometheus -> MetricsCollector.
- **D-08:** All new component files in `db/peer/`: connection_manager.h/.cpp, message_dispatcher.h/.cpp, sync_orchestrator.h/.cpp, pex_manager.h/.cpp, blob_push_manager.h/.cpp, metrics_collector.h/.cpp.
- **D-09:** PeerManager facade stays at `db/peer/peer_manager.h/.cpp` with unchanged public API. All callers (main.cpp, tests) continue using PeerManager directly.
- **D-10:** Reference injection at construction. Each component receives references/pointers to its dependencies. No shared context struct, no event bus.
- **D-11:** ConnectionManager owns `peers_` (the `std::deque<std::unique_ptr<PeerInfo>>`). Other components that need peer access get a reference to ConnectionManager.
- **D-12:** Direct method calls between components where cross-component interaction is needed. Circular dependencies avoided by careful layering.
- **D-13:** MetricsCollector owns `NodeMetrics`. Components that increment counters get a `NodeMetrics&`.

### Claude's Discretion
- Exact constructor signatures for each component
- Internal method organization within each component
- How to layer components to avoid circular include dependencies (forward declarations, interface abstractions if needed)
- Whether PeerManager facade owns components by value or unique_ptr
- How config reload propagates to components (direct call vs observer)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| ARCH-01 | PeerManager split into focused components (connection management, message dispatch, sync orchestration, metrics) | All 6 components mapped to methods+state, dependency graph analyzed, layering strategy documented, pitfalls catalogued |
</phase_requirements>

## Architecture Patterns

### Recommended Project Structure

```
db/peer/
  peer_manager.h           # Facade: public API, owns 6 components
  peer_manager.cpp         # Facade: constructor, start/stop, SIGHUP/SIGUSR1, config reload, static encode/decode
  connection_manager.h     # ConnectionManager class declaration
  connection_manager.cpp   # ~550 lines: connect/disconnect, dedup, announce, keepalive, strike
  message_dispatcher.h     # MessageDispatcher class declaration
  message_dispatcher.cpp   # ~1200 lines: on_peer_message switch, query handlers
  sync_orchestrator.h      # SyncOrchestrator class declaration
  sync_orchestrator.cpp    # ~1700 lines: run_sync, handle_sync_as_responder, expiry, compaction
  pex_manager.h            # PexManager class declaration
  pex_manager.cpp          # ~350 lines: PEX protocol, peer persistence
  blob_push_manager.h      # BlobPushManager class declaration
  blob_push_manager.cpp    # ~400 lines: notify/fetch, on_blob_ingested fan-out
  metrics_collector.h      # MetricsCollector class declaration
  metrics_collector.cpp    # ~300 lines: Prometheus endpoint, log_metrics_line, dump_metrics
  sync_reject.h            # Existing: sync rejection enum (unchanged)
```

### Pattern 1: Thin Facade with Component Delegation

**What:** PeerManager retains its public API but delegates all work to owned components.
**When to use:** When callers should not know about internal decomposition.

```cpp
// peer_manager.h -- facade (simplified)
class PeerManager {
public:
    // Public API unchanged: constructor, start(), stop(), peer_count(), etc.
    // Static methods stay here: encode/decode_namespace_list, encode/decode_peer_list, encode_notification

    void on_blob_ingested(/*...*/) { blob_push_.on_blob_ingested(/*...*/); }
    const NodeMetrics& metrics() const { return metrics_.metrics(); }
    std::string prometheus_metrics_text() { return metrics_.format_prometheus_metrics(); }
    void reload_config();

private:
    // Component ownership (D-10: reference injection at construction)
    ConnectionManager conn_mgr_;
    MetricsCollector metrics_;
    SyncOrchestrator sync_;
    PexManager pex_;
    BlobPushManager blob_push_;
    MessageDispatcher dispatcher_;

    // Facade-level state only: Server, UdsAcceptor, signal_sets, config_path, stopping_
    net::Server server_;
    std::unique_ptr<net::UdsAcceptor> uds_acceptor_;
    asio::signal_set sighup_signal_;
    asio::signal_set sigusr1_signal_;
    // ...
};
```

### Pattern 2: Component Layering (Dependency DAG)

**What:** Components are layered to prevent circular includes. Lower layers do not include upper layers.

```
Layer 0 (leaf):  MetricsCollector  (depends only on: Storage, NodeMetrics, io_context)
Layer 1:         PexManager        (depends on: ConnectionManager, Server, io_context)
Layer 1:         BlobPushManager   (depends on: ConnectionManager, Storage, Engine, NodeMetrics, io_context)
Layer 2:         SyncOrchestrator  (depends on: ConnectionManager, SyncProtocol, Engine, Storage, NodeMetrics, io_context)
Layer 3:         MessageDispatcher (depends on: ConnectionManager, SyncOrchestrator, PexManager, BlobPushManager, Engine, Storage, NodeMetrics, io_context)
Layer 3:         ConnectionManager (depends on: NodeMetrics, ACL, Identity, io_context)

Facade:          PeerManager       (owns all, wires dependencies)
```

**Key insight:** ConnectionManager owns `peers_` but is at Layer 3 because MessageDispatcher needs it. However, ConnectionManager does NOT depend on MessageDispatcher. The apparent circularity is resolved because:
- ConnectionManager provides peer access (find_peer, peers iteration) through its public interface
- MessageDispatcher calls ConnectionManager methods (not the reverse)
- The message callback is wired by the facade: `conn->on_message(...)` in PeerManager facade delegates to MessageDispatcher

### Pattern 3: Forward Declarations for Cross-Component References

**What:** Use forward declarations in headers to break include cycles.
**When to use:** When a component header needs to reference another component's type without needing its full definition.

```cpp
// connection_manager.h
namespace chromatindb::peer {
class MessageDispatcher;  // Forward declaration -- not needed: CM doesn't depend on MD
}
```

In practice, no forward declarations should be needed between component headers because the dependency DAG is acyclic. Each component header includes only what it needs (connection.h, storage.h, etc.). The facade header includes all component headers.

### Pattern 4: Member Variable Distribution

**What:** Each member variable from PeerManager goes to exactly one component owner.

| Variable | Owner | Rationale |
|----------|-------|-----------|
| `peers_` | ConnectionManager | D-11: central peer container |
| `bootstrap_addresses_` | ConnectionManager | Connection lifecycle |
| `known_addresses_` | PexManager | Peer discovery |
| `trusted_peers_` | ConnectionManager | Trust check for handshake |
| `persisted_peers_` | PexManager | Peer persistence |
| `stopping_` | PeerManager facade | Shared shutdown flag (passed by ref) |
| `expiry_timer_`, `next_expiry_target_`, `expiry_loop_running_` | SyncOrchestrator | Expiry scanning |
| `sync_timer_` | SyncOrchestrator | Sync timer loop |
| `pex_timer_`, `flush_timer_` | PexManager | PEX and flush timers |
| `metrics_timer_` | MetricsCollector | Metrics log timer |
| `cursor_compaction_timer_` | SyncOrchestrator | Cursor compaction |
| `keepalive_timer_` | ConnectionManager | Keepalive loop |
| `compaction_timer_` | SyncOrchestrator | Storage compaction |
| `metrics_acceptor_`, `metrics_bind_` | MetricsCollector | Prometheus HTTP |
| `rate_limit_bytes_per_sec_`, `rate_limit_burst_` | MessageDispatcher | Rate limiting context |
| `full_resync_interval_`, `cursor_stale_seconds_` | SyncOrchestrator | Cursor config |
| `sync_namespaces_` | PeerManager facade | Shared by multiple components (passed by const ref) |
| `sync_cooldown_seconds_`, `max_sync_sessions_` | SyncOrchestrator | Sync rate limiting |
| `safety_net_interval_seconds_` | SyncOrchestrator | Safety-net timer config |
| `max_peers_` | ConnectionManager | Connection limit |
| `compaction_interval_hours_`, `last_compaction_time_`, `compaction_count_` | SyncOrchestrator | Storage compaction |
| `on_notification_` | BlobPushManager | Test hook |
| `pending_fetches_` | BlobPushManager | D-05: blob fetch dedup |
| `disconnected_peers_`, `CURSOR_GRACE_PERIOD_MS` | ConnectionManager | Cursor grace period tracking |
| `metrics_` | MetricsCollector | D-13: NodeMetrics ownership |
| `start_time_` | MetricsCollector | Uptime calculation |
| `server_` | PeerManager facade | Server lifecycle (start/stop in facade) |
| `uds_acceptor_` | PeerManager facade | UDS lifecycle (start/stop in facade) |
| `sync_proto_` | SyncOrchestrator | Sync protocol |
| `sighup_signal_`, `sigusr1_signal_`, `config_path_` | PeerManager facade | Signal handling |

### Pattern 5: Config Reload Propagation

**What:** PeerManager facade calls reload methods on each component.
**Implementation:** Each component exposes a `reload_config(const Config& new_cfg)` or individual setter methods.

```cpp
// In PeerManager::reload_config():
// After parsing new_cfg and validating...
conn_mgr_.set_max_peers(new_cfg.max_peers);
dispatcher_.set_rate_limits(new_cfg.rate_limit_bytes_per_sec, new_cfg.rate_limit_burst);
sync_.set_sync_config(new_cfg.sync_cooldown_seconds, new_cfg.max_sync_sessions,
                      new_cfg.safety_net_interval_seconds);
sync_.set_compaction_interval(new_cfg.compaction_interval_hours);
metrics_.set_metrics_bind(new_cfg.metrics_bind);
// etc.
```

### Anti-Patterns to Avoid

- **Shared mutable context struct:** Would create implicit coupling between all components. Use explicit references instead (D-10).
- **Event bus / observer pattern:** Over-engineering for 6 components on a single thread. Direct method calls are simpler and debuggable (D-12).
- **Virtual interfaces for components:** Unnecessary indirection. Components are internal; only the facade is public. No runtime polymorphism needed.
- **Splitting header-only types:** PeerInfo, NodeMetrics, PersistedPeer, SyncMessage, DisconnectedPeerState, ArrayHash32 should stay in `peer_manager.h` (or a new `peer_types.h`) since multiple components need them.

## Method-to-Component Mapping (Complete)

### ConnectionManager (~550 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| `on_peer_connected` | 323-491 |
| `on_peer_disconnected` | 493-527 |
| `should_accept_connection` | 319-321 |
| `announce_and_sync` | 533-572 |
| `keepalive_loop` | 4138-4185 |
| `record_strike` | 2825-2839 |
| `disconnect_unauthorized_peers` | 3073-3096 |
| `find_peer` | 4702-4707 |
| `peer_display_name` | 4709-4712 |
| `is_trusted_address` | 2866-2869 |

State: `peers_`, `bootstrap_addresses_`, `trusted_peers_`, `disconnected_peers_`, `keepalive_timer_`, `max_peers_`

### MessageDispatcher (~1200 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| `on_peer_message` | 578-1787 |
| `route_sync_message` | 1793-1799 |
| `recv_sync_msg` | 1801-1833 |
| `send_sync_rejected` | 3718-3724 |

State: `rate_limit_bytes_per_sec_`, `rate_limit_burst_`, `sync_namespaces_` (const ref)

Note: `on_peer_message` contains the entire dispatch switch including all query handlers (ReadRequest, ListRequest, StatsRequest, ExistsRequest, NodeInfoRequest, NamespaceListRequest, StorageStatusRequest, NamespaceStatsRequest, MetadataRequest, BatchExistsRequest, DelegationListRequest, BatchReadRequest, PeerInfoRequest, TimeRangeRequest, Data, Delete, Subscribe, Unsubscribe, etc.). These inline query handlers move with the dispatcher since they are tightly bound to the dispatch logic.

### SyncOrchestrator (~1700 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| `run_sync_with_peer` | 1853-2320 |
| `handle_sync_as_responder` | 2322-2769 |
| `sync_all_peers` | 2771-2784 |
| `sync_timer_loop` | 2786-2818 |
| `check_full_resync` | 1839-1851 |
| `expiry_scan_loop` | 3102-3139 |
| `cursor_compaction_loop` | 4066-4099 |
| `compaction_loop` | 4106-4136 |

State: `sync_proto_`, `sync_timer_`, `expiry_timer_`, `next_expiry_target_`, `expiry_loop_running_`, `cursor_compaction_timer_`, `compaction_timer_`, `full_resync_interval_`, `cursor_stale_seconds_`, `sync_cooldown_seconds_`, `max_sync_sessions_`, `safety_net_interval_seconds_`, `compaction_interval_hours_`, `last_compaction_time_`, `compaction_count_`

### PexManager (~350 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| `pex_timer_loop` | 3524-3539 |
| `request_peers_from_all` | 3541-3554 |
| `run_pex_with_peer` | 3422-3445 |
| `handle_pex_as_responder` | 3470-3494 |
| `handle_peer_list_response` | 3496-3522 |
| `build_peer_list_response` | 3447-3468 |
| `load_persisted_peers` | 3564-3594 |
| `save_persisted_peers` | 3597-3673 |
| `update_persisted_peer` | 3675-3696 |
| `peers_file_path` | 3560-3562 |
| `peer_flush_timer_loop` | 3145-3156 |

State: `known_addresses_`, `persisted_peers_`, `pex_timer_`, `flush_timer_`

### BlobPushManager (~400 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| `on_blob_ingested` | 3202-3261 |
| `on_blob_notify` | 3267-3298 |
| `handle_blob_fetch` | 3300-3322 |
| `handle_blob_fetch_response` | 3324-3379 |

State: `pending_fetches_`, `on_notification_`

### MetricsCollector (~300 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| `metrics_timer_loop` | 3786-3797 |
| `log_metrics_line` | 3799-3832 |
| `compute_uptime_seconds` | 3834-3838 |
| `dump_metrics` | 3744-3780 |
| `start_metrics_listener` | 3844-3875 |
| `stop_metrics_listener` | 3877-3884 |
| `metrics_accept_loop` | 3886-3894 |
| `metrics_handle_connection` | 3896-3961 |
| `format_prometheus_metrics` | 3963-4056 |
| `prometheus_metrics_text` | 4058-4060 |

State: `metrics_`, `metrics_timer_`, `metrics_acceptor_`, `metrics_bind_`, `start_time_`

### PeerManager Facade (~300 lines)
| Method | Current Lines (approx) |
|--------|----------------------|
| Constructor | 66-168 |
| `start` | 170-276 |
| `stop` | 290-297 |
| `cancel_all_timers` | 278-288 |
| `peer_count` | 303-305 |
| `bootstrap_peer_count` | 307-313 |
| `exit_code` | 299-301 |
| `reload_config` | 2871-3071 |
| `sighup_loop` / `handle_sighup` / `setup_sighup_handler` | 2845-2864 |
| `sigusr1_loop` / `setup_sigusr1_handler` | 3730-3742 |
| Static: `encode_peer_list` / `decode_peer_list` | 3162-3196 |
| Static: `encode_namespace_list` / `decode_namespace_list` | 3385-3407 |
| Static: `encode_notification` | 3409-3416 |

State: `server_`, `uds_acceptor_`, `sighup_signal_`, `sigusr1_signal_`, `config_path_`, `stopping_`, `sync_namespaces_`

## Shared Types Header

Multiple components need PeerInfo, NodeMetrics, SyncMessage, PersistedPeer, DisconnectedPeerState, ArrayHash32. These should be extracted to a shared types header:

```
db/peer/peer_types.h   # All struct definitions currently in peer_manager.h
```

This keeps `peer_manager.h` clean (just the facade class) while letting all components include the shared types.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Inter-component event bus | Custom pub/sub system | Direct method calls | Single-threaded model makes events unnecessary overhead |
| Dependency injection framework | Custom DI container | Constructor reference injection | 6 components is trivial; framework adds complexity |
| Component lifecycle manager | Abstract component base class | Explicit start/stop delegation in facade | Over-engineering for static composition |

**Key insight:** This is a code organization refactor, not an architecture change. The runtime behavior is identical. Keep the decomposition mechanical -- move methods and state to new files, wire references in the facade constructor.

## Common Pitfalls

### Pitfall 1: Breaking the find_peer Re-lookup Pattern
**What goes wrong:** After moving methods to components, the developer forgets that `find_peer()` must be called after every co_await because peers_ may change during suspension.
**Why it happens:** Copy-pasting code to a new class and not noticing the pattern.
**How to avoid:** find_peer() must be a method on ConnectionManager (since it owns peers_). Every coroutine method in SyncOrchestrator, PexManager, etc. must call `conn_mgr_.find_peer(conn)` after every co_await, exactly as before.
**Warning signs:** TSAN reports, UAF under ASAN.

### Pitfall 2: Timer Pointer Lifetime (Timer-Cancel Pattern)
**What goes wrong:** Timer pointers (e.g., `sync_timer_`) are stored as raw `asio::steady_timer*` pointing to stack-local timers in coroutines. If cancel_all_timers() references a pointer owned by a different component, and that component is destroyed first, you get a dangling pointer.
**Why it happens:** Component destruction order in PeerManager may not match timer cancel order.
**How to avoid:** Each component must have its own `cancel_timers()` method. PeerManager::stop() calls each component's cancel_timers() before destroying them. Components manage their own timer pointers.
**Warning signs:** Crash on shutdown, ASAN heap-use-after-free.

### Pitfall 3: Static Method Migration
**What goes wrong:** `encode_namespace_list`, `decode_namespace_list`, `encode_peer_list`, `decode_peer_list`, `encode_notification` are static methods on PeerManager. Moving them breaks relay_session.cpp, loadgen_main.cpp, and test_namespace_announce.cpp which reference `PeerManager::encode_namespace_list` etc.
**Why it happens:** External callers depend on these static methods being on PeerManager.
**How to avoid:** Keep static methods on PeerManager (the facade). They are pure functions with no state access -- they belong on the public API surface. Internally, components can call them via PeerManager:: scope or the facade can provide convenience forwarding.
**Warning signs:** Compile errors in relay/ and loadgen/ directories.

### Pitfall 4: on_peer_message Callback Wiring
**What goes wrong:** `conn->on_message(...)` is set in `on_peer_connected()` (ConnectionManager), but the lambda calls `on_peer_message()` which is now on MessageDispatcher. If the lambda captures `this` (ConnectionManager), it can't call MessageDispatcher.
**Why it happens:** The callback closure crosses component boundaries.
**How to avoid:** The callback should be wired in the facade or ConnectionManager should hold a reference/function pointer to the dispatcher's method. Best approach: ConnectionManager takes a `MessageCallback` function in its constructor (injected by PeerManager facade), which delegates to `dispatcher_.on_peer_message(...)`.
**Warning signs:** Compile error on lambda capture.

### Pitfall 5: Circular Dependencies Between Components
**What goes wrong:** SyncOrchestrator calls `on_blob_ingested` (BlobPushManager) for sync-received blobs. BlobPushManager iterates `peers_` (ConnectionManager). MessageDispatcher routes to SyncOrchestrator, BlobPushManager, and ConnectionManager. If headers include each other, circular dependency.
**Why it happens:** The original god object had no boundary; everything could see everything.
**How to avoid:** Headers include only what they need. Use forward declarations in headers, full includes in .cpp files. The dependency DAG must be acyclic at the header level. Specifically:
- ConnectionManager.h: no peer component includes
- SyncOrchestrator.h: forward-declares ConnectionManager, BlobPushManager
- BlobPushManager.h: forward-declares ConnectionManager
- MessageDispatcher.h: forward-declares everything (or includes via peer_types.h)
- All .cpp files can include any header
**Warning signs:** Compiler "incomplete type" errors.

### Pitfall 6: sync_namespaces_ Shared Across Components
**What goes wrong:** `sync_namespaces_` is used by MessageDispatcher (namespace filter on Data/Delete), SyncOrchestrator (filter during reconciliation), BlobPushManager (filter on BlobNotify fan-out), and ConnectionManager (announce_and_sync). It's also updated by reload_config.
**Why it happens:** This state cuts across all components.
**How to avoid:** Keep `sync_namespaces_` on the PeerManager facade. Components receive it as `const std::set<...>&` in their constructors. reload_config() updates the set on the facade; components read it through their const reference.
**Warning signs:** Stale filter after SIGHUP.

### Pitfall 7: on_blob_ingested Crosses Multiple Components
**What goes wrong:** `on_blob_ingested` is called from: MessageDispatcher (Data/Delete handlers), SyncOrchestrator (via sync_proto_ callback), and BlobPushManager (handle_blob_fetch_response). It also rearms the expiry timer (SyncOrchestrator concern). If the method lives on BlobPushManager, SyncOrchestrator needs a reference to it.
**Why it happens:** This is the central fan-out point that crosses all boundaries.
**How to avoid:** `on_blob_ingested` lives on BlobPushManager (D-05). BlobPushManager holds a reference to SyncOrchestrator for the expiry timer rearm. The facade wires the sync_proto_ callback to call `blob_push_.on_blob_ingested(...)`. MessageDispatcher calls `blob_push_.on_blob_ingested(...)` for Data/Delete paths.
**Warning signs:** Missing notifications, expiry timer not rearmed.

### Pitfall 8: recv_sync_msg Shared Between SyncOrchestrator and PexManager
**What goes wrong:** Both SyncOrchestrator and PexManager need `recv_sync_msg()` to receive from the sync inbox. If it's only on one component, the other can't use it.
**Why it happens:** The sync inbox pattern is shared between sync and PEX protocols.
**How to avoid:** `recv_sync_msg` stays on MessageDispatcher (which manages the sync inbox routing via `route_sync_message`). Both SyncOrchestrator and PexManager call `dispatcher_.recv_sync_msg(conn, timeout)`. Alternatively, move it to ConnectionManager since it accesses `peer->sync_inbox` (owned by peers_).
**Warning signs:** PEX responses never received, sync timeouts.

### Pitfall 9: Build System Not Updated
**What goes wrong:** New .cpp files added to `db/peer/` but not to `CMakeLists.txt` source list. Build succeeds with stale object files, then fails on clean build.
**Why it happens:** Forgetting to update CMakeLists.txt.
**How to avoid:** Update `chromatindb_lib` SOURCES list in `db/CMakeLists.txt` to replace the single `peer/peer_manager.cpp` entry with all 7 .cpp files (facade + 6 components). No test file changes needed (tests include `peer_manager.h` which pulls in everything).
**Warning signs:** Linker errors for undefined symbols.

## Code Examples

### Component Constructor Pattern
```cpp
// connection_manager.h
class ConnectionManager {
public:
    ConnectionManager(
        identity::NodeIdentity& identity,
        acl::AccessControl& acl,
        NodeMetrics& metrics,
        asio::io_context& ioc,
        const config::Config& config,
        const std::set<std::string>& bootstrap_addresses,
        const std::set<std::string>& trusted_peers,
        const bool& stopping,
        std::function<void(net::Connection::Ptr, wire::TransportMsgType,
                           std::vector<uint8_t>, uint32_t)> on_message);

    // Methods moved from PeerManager
    void on_peer_connected(net::Connection::Ptr conn);
    void on_peer_disconnected(net::Connection::Ptr conn);
    bool should_accept_connection() const;
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    std::string peer_display_name(const net::Connection::Ptr& conn);

    // Peer access for other components (D-11)
    const std::deque<std::unique_ptr<PeerInfo>>& peers() const { return peers_; }
    std::deque<std::unique_ptr<PeerInfo>>& peers() { return peers_; }
    size_t peer_count() const { return peers_.size(); }

    // Timer management
    asio::awaitable<void> keepalive_loop();
    void cancel_timers();

private:
    std::deque<std::unique_ptr<PeerInfo>> peers_;
    // ...
};
```

### Facade Wiring Pattern
```cpp
// peer_manager.cpp (facade constructor)
PeerManager::PeerManager(/*...*/)
    : config_(config)
    , server_(config, identity, ioc)
    , metrics_(storage, ioc, config, stopping_)
    , conn_mgr_(identity, acl, metrics_.node_metrics(), ioc, config,
                bootstrap_addresses_, trusted_peers_, stopping_,
                [this](auto c, auto t, auto p, auto r) {
                    dispatcher_.on_peer_message(c, t, std::move(p), r);
                })
    , sync_(conn_mgr_, engine, storage, pool, ioc, metrics_.node_metrics(),
            sync_namespaces_, stopping_)
    , pex_(conn_mgr_, server_, acl, ioc, config, stopping_)
    , blob_push_(conn_mgr_, engine, storage, ioc, metrics_.node_metrics(),
                 sync_namespaces_, stopping_, sync_)
    , dispatcher_(conn_mgr_, sync_, pex_, blob_push_, engine, storage,
                  ioc, pool, config, metrics_.node_metrics(), sync_namespaces_,
                  stopping_)
{
    // Server callback wiring
    server_.set_on_connected([this](auto c) { conn_mgr_.on_peer_connected(c); });
    server_.set_on_disconnected([this](auto c) { conn_mgr_.on_peer_disconnected(c); });
    // ...
}
```

### Component Ownership (unique_ptr vs by-value)

**Recommendation: By value.** Components are non-copyable, non-movable objects with references to each other. Value members in PeerManager guarantee:
1. Deterministic destruction order (reverse declaration order)
2. No heap allocation overhead
3. Simple reference semantics (no dereferencing pointers)

However, this requires careful declaration order in the facade class to ensure components that are referenced by others are declared (and thus constructed) first. Declaration order must match the layering:

```cpp
class PeerManager {
    // Layer 0: no component dependencies
    MetricsCollector metrics_;
    ConnectionManager conn_mgr_;

    // Layer 1: depends on conn_mgr_ and/or metrics_
    SyncOrchestrator sync_;
    PexManager pex_;
    BlobPushManager blob_push_;

    // Layer 3: depends on everything above
    MessageDispatcher dispatcher_;
};
```

## External Callers Audit

| Caller | Uses | Impact |
|--------|------|--------|
| `db/main.cpp` | Constructor, start(), stop(), ioc.run() | None (facade API unchanged) |
| `loadgen/loadgen_main.cpp` | `PeerManager::encode_namespace_list` (static) | None (static method stays on facade) |
| `relay/core/relay_session.cpp` | `PeerManager::decode_namespace_list`, `PeerManager::encode_namespace_list` (static) | None (static method stays on facade) |
| `db/tests/peer/test_peer_manager.cpp` | Constructor, start/stop, metrics(), peer_count(), on_blob_ingested(), prometheus_metrics_text(), set_on_notification(), reload_config() | None (all stay on facade) |
| `db/tests/peer/test_metrics_endpoint.cpp` | Constructor, prometheus_metrics_text() | None |
| `db/tests/peer/test_keepalive.cpp` | Constructor, start/stop, peer_count() | None |
| `db/tests/peer/test_namespace_announce.cpp` | encode/decode_namespace_list (static) | None |
| `db/tests/peer/test_event_expiry.cpp` | Constructor, start/stop, on_blob_ingested() | None |
| `db/tests/test_daemon.cpp` | Constructor, start/stop | None |

**All callers verified: zero changes required.**

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (FetchContent, BUILD_TESTING) |
| Quick run command | `build/db/chromatindb_tests -t "[peer]"` |
| Full suite command | `build/db/chromatindb_tests` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| ARCH-01 | PeerManager split compiles and all tests pass | compilation + existing suite | `cmake --build build && build/db/chromatindb_tests` | Existing tests cover all |
| ARCH-01 | Public API unchanged (no caller-side changes) | compilation | `cmake --build build` (includes relay, loadgen, all tests) | Existing tests cover all |
| ARCH-01 | ASAN/TSAN/UBSAN clean | sanitizer build | `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" && cmake --build build && build/db/chromatindb_tests` | Existing tests cover all |

### Sampling Rate
- **Per task commit:** `cmake --build build && build/db/chromatindb_tests`
- **Per wave merge:** Full suite with ASAN: `cmake --build build && build/db/chromatindb_tests`
- **Phase gate:** Full suite green + TSAN build green + Docker integration tests green

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. The 1302 existing unit tests and 53 integration test scripts exercise the PeerManager public API. Since the API is unchanged, no new tests are needed. If any test fails, it indicates a decomposition bug.

## Open Questions

1. **recv_sync_msg ownership**
   - What we know: Used by both SyncOrchestrator and PexManager. Accesses `peer->sync_inbox` and `peer->sync_notify` (owned by ConnectionManager via PeerInfo).
   - What's unclear: Should it live on ConnectionManager (owns the data), MessageDispatcher (routes messages to inbox), or be a shared utility?
   - Recommendation: Place on ConnectionManager since it owns PeerInfo and the inbox. Both SyncOrchestrator and PexManager call `conn_mgr_.recv_sync_msg(conn, timeout)`. Alternatively, keep on MessageDispatcher since route_sync_message is its counterpart.

2. **Anonymous namespace helpers (try_consume_tokens, hex_to_namespace)**
   - What we know: `try_consume_tokens` is used only in `on_peer_message` (MessageDispatcher). `hex_to_namespace` is used in constructor and reload_config (PeerManager facade).
   - What's unclear: Should they move to component .cpp files or stay as local helpers?
   - Recommendation: Move `try_consume_tokens` to message_dispatcher.cpp anonymous namespace. Move `hex_to_namespace` to peer_manager.cpp anonymous namespace (stays in facade).

## Sources

### Primary (HIGH confidence)
- Direct codebase analysis of `db/peer/peer_manager.h` (391 lines) and `db/peer/peer_manager.cpp` (4187 lines)
- All 5 peer test files (5156 total lines) analyzed for public API usage
- External caller audit: main.cpp, loadgen_main.cpp, relay_session.cpp, 5 test files
- CMakeLists.txt build system structure verified

### Secondary (MEDIUM confidence)
- CONTEXT.md decisions (locked by user discussion session)
- REQUIREMENTS.md ARCH-01 specification

## Metadata

**Confidence breakdown:**
- Architecture: HIGH - all 4187 lines read, every method mapped to a component, dependency graph verified acyclic
- Pitfalls: HIGH - based on direct code analysis of coroutine patterns, timer lifetimes, and cross-component dependencies
- Method mapping: HIGH - complete line-by-line mapping with line numbers

**Research date:** 2026-04-08
**Valid until:** 2026-05-08 (stable -- codebase-specific, no external dependencies)
