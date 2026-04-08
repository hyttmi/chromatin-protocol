# Phase 96: PeerManager Architecture - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-08
**Phase:** 96-peermanager-architecture
**Areas discussed:** Component boundaries, Decomposition style, Inter-component wiring

---

## Component Boundaries

### How should the ~10 concerns map to components?

| Option | Description | Selected |
|--------|-------------|----------|
| 4 components | Stick with ROADMAP's 4: ConnectionManager, MessageDispatcher, SyncOrchestrator, MetricsCollector. Fold PEX into connection mgmt, blob notify/fetch into sync, signals/persistence/timers into facade. | |
| 6 components | Split further: add PexManager and BlobPushManager as separate components alongside the core 4. | ✓ |
| You decide | Claude picks based on coupling analysis. | |

**User's choice:** 6 components
**Notes:** None

### Where should leftover concerns land?

| Option | Description | Selected |
|--------|-------------|----------|
| Stay in PeerManager facade | Orchestration glue -- coordinate across components. PeerManager stays as wiring layer. | |
| 7th component: NodeLifecycle | Extract signals, timers, persistence, config reload into dedicated lifecycle manager. | |
| Distribute into the 6 | Keepalive → ConnectionManager, expiry → SyncOrchestrator, persistence → PexManager, etc. | ✓ |

**User's choice:** Distribute into the 6
**Notes:** Confirmed mapping: keepalive + strike → ConnectionManager, expiry + cursor compaction → SyncOrchestrator, peer persistence → PexManager, signals + config → facade (thin), metrics → MetricsCollector.

---

## Decomposition Style

### Where should new component files live?

| Option | Description | Selected |
|--------|-------------|----------|
| All in db/peer/ | connection_manager.h/.cpp, message_dispatcher.h/.cpp, etc. Keeps PeerManager's domain together. | ✓ |
| Spread to domain dirs | sync_orchestrator → db/sync/, metrics_collector → db/metrics/, etc. | |
| New db/peer/ subdirs | db/peer/sync/, db/peer/metrics/, etc. Hierarchical but deeper nesting. | |

**User's choice:** All in db/peer/
**Notes:** None

---

## Inter-Component Wiring

### How should components access shared state?

| Option | Description | Selected |
|--------|-------------|----------|
| Reference injection | Each component gets references/pointers to what it needs at construction. Explicit dependencies. | ✓ |
| Shared context struct | PeerContext struct holds all shared state, all components get a reference. | |
| Event bus / callbacks | Components communicate through callbacks/events. No shared state. | |

**User's choice:** Reference injection
**Notes:** None

### Who owns the peers_ deque?

| Option | Description | Selected |
|--------|-------------|----------|
| ConnectionManager owns it | Provides find_peer() and iteration. Natural since connections are its domain. | ✓ |
| PeerManager facade keeps it | Facade retains ownership, passes references down. Components are purely behavioral. | |
| You decide | Claude picks based on coupling analysis. | |

**User's choice:** ConnectionManager owns it
**Notes:** None

### How should cross-component calls work?

| Option | Description | Selected |
|--------|-------------|----------|
| Direct method calls | Components hold references to each other. Simple, explicit, zero overhead. | ✓ |
| Thin callback interface | Components register std::function callbacks. More decoupled but more boilerplate. | |
| You decide | Claude picks minimal coupling approach. | |

**User's choice:** Direct method calls
**Notes:** None

---

## Claude's Discretion

- Exact constructor signatures for each component
- Internal method organization within each component
- How to layer components to avoid circular include dependencies
- Whether PeerManager facade owns components by value or unique_ptr
- How config reload propagates to components

## Deferred Ideas

None -- discussion stayed within phase scope
