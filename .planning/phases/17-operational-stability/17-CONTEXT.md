# Phase 17: Operational Stability - Context

**Gathered:** 2026-03-10
**Status:** Ready for planning

<domain>
## Phase Boundary

Node survives restarts and crashes without losing peer connections or operational visibility. Three concerns: graceful shutdown (SIGTERM handling, coroutine cancellation, bounded drain), persistent peer list (atomic writes, periodic flush), and runtime metrics (counters, periodic logging, SIGUSR1 dump). No new wire protocol messages, no new config schema beyond what requirements specify.

</domain>

<decisions>
## Implementation Decisions

### Metrics log format
- Periodic 60s log: structured key=value single line via spdlog::info
  - Example: `metrics: connections=3 blobs=1420 storage=84.2MiB syncs=12 ingests=45 rejections=2 uptime=3600`
- SIGUSR1 dump: detailed multi-line report including:
  - Global counters (same as periodic line)
  - Per-peer breakdown: address + namespace prefix (e.g., `127.0.0.1:4200 (ns:a1b2c3d4...)`)
  - Per-namespace blob counts and storage used
- Human-readable storage size with unit suffix (MiB) in log output

### Counter set and semantics
- Required counters from OPS-05: blob_count, storage_used, connections, syncs, ingests, rejections, rate_limited
- Additional counters: uptime_seconds, peers_connected_total, peers_disconnected_total
- All counters monotonically increasing since startup (never reset)
- Prometheus-style: rate = delta/time calculated by consumer, not by the node

### Shutdown sequencing
- On SIGTERM: save peer list FIRST, then drain connections
  - Ensures peer file is written while connection list is most accurate
  - If drain hangs, peer file is already saved
- 2nd signal: skip drain timeout, force-close all connections immediately, but still run clean exit path (destructors, spdlog flush). NOT std::_Exit.
- Exit code 0 for clean shutdown (drain completed), exit code 1 for forced/timeout shutdown
- Drain timeout stays hardcoded at 5s (constexpr, matches TTL-as-invariant philosophy)

### Peer list persistence
- Flush triggers: 30s periodic timer + shutdown flush. No event-triggered writes (no flush on connect/disconnect)
- Atomic write pattern (temp + fsync + rename + dir fsync) implemented inline in peer_manager, not as reusable utility (YAGNI)
- Corrupt peers.json on startup: log warning, start with empty list. Non-fatal — bootstrap peers still work
- Prune stale entries at startup only (existing behavior), not during periodic flush

### Claude's Discretion
- Storage metric source: MDBX stat (ground truth) vs in-memory counter — pick the right tradeoff
- Expiry coroutine cancellation mechanism (cancellation_signal vs stop token)
- NodeMetrics struct layout and where counters are incremented
- SIGUSR1 per-namespace enumeration strategy (iterate MDBX vs maintain in-memory index)

</decisions>

<specifics>
## Specific Ideas

- SIGUSR1 handler follows existing sighup_loop() coroutine pattern in PeerManager
- Expiry scanner currently in main.cpp as lambda with asio::detached — needs to move into a cancellable member coroutine
- Server::drain already exists with goodbye/timeout pattern — extend, don't rewrite
- existing stopping_ flag in PeerManager should integrate with new shutdown sequence

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `PeerManager::sighup_loop()` — coroutine signal handler pattern, reuse for SIGUSR1
- `PeerManager::save_persisted_peers()` / `load_persisted_peers()` — existing peer file I/O, extend with atomic write
- `Server::drain(timeout)` — existing drain coroutine with goodbye + timeout + force close
- `PeerManager::stopping_` flag — already exists, wire into shutdown sequence
- spdlog already used everywhere — no new logging dependency

### Established Patterns
- Signal handlers as coroutine member functions (not lambdas — stack-use-after-return per MEMORY.md)
- Timer-cancel pattern for async message queues (sync_inbox)
- Deque for coroutine-accessed containers
- Step 0 pattern: cheapest validation before expensive ops

### Integration Points
- `main.cpp:cmd_run()` — expiry scanner lambda needs to become a PeerManager member coroutine
- `Server::stop()` — PeerManager::stop() must call save_persisted_peers() before Server::stop()
- `Server::signals_` — currently handles SIGINT/SIGTERM, needs coordination with PeerManager shutdown
- `PeerInfo` struct — potential location for per-connection metric counters
- `BlobEngine::ingest()` / `Storage` — counter increment points for ingests/rejections

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 17-operational-stability*
*Context gathered: 2026-03-10*
