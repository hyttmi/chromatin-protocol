# Project Research Summary

**Project:** chromatindb v0.4.0 — Production Readiness
**Domain:** Production hardening of a C++20 P2P post-quantum blob store daemon
**Researched:** 2026-03-08
**Confidence:** HIGH

## Executive Summary

chromatindb v0.4.0 is a pure hardening milestone: no new protocols, no new dependencies, no architectural shifts. The existing 14,152 LOC codebase already contains nearly every primitive needed — libmdbx storage stats, graceful drain coroutines, peer persistence machinery, spdlog, and the strike system are all in place. The work is connecting existing pieces, filling the gaps between them, and adding three narrow new constructs: a fifth mdbx sub-database for tombstone indexing, a `NodeMetrics` struct with plain `uint64_t` counters, and a token bucket per `PeerInfo`. All 8 features total approximately 370 lines of new code.

The recommended implementation order is correctness first, then operations, then topology control. The tombstone index (currently O(n) on every ingest) ships first because it is on the hot path for all subsequent features. Storage limit enforcement comes before disk-full reporting, which produces the error code that triggers the wire message. Graceful shutdown and persistent peer persistence are tightly coupled at `PeerManager::stop()` and build as a single phase. Metrics and rate limiting are independent and follow in any order.

The dominant risk category is coroutine concurrency: every `co_await` in the Asio model is a preemption point where container state can change. The three highest-cost pitfalls are: (1) a stale storage limit check that fires in a coroutine body instead of inside the synchronous `BlobEngine::ingest()`; (2) `io_context` destroyed with live coroutine frames causing use-after-free at shutdown; and (3) a raw `PeerInfo*` going dangling across a `co_await` in the rate limit path. All three are prevented by patterns already documented in the project's RETROSPECTIVE.md.

## Key Findings

### Recommended Stack

No new dependencies. Every v0.4.0 feature is achievable with the existing stack. See `STACK.md` for full detail.

**Core technologies and their v0.4.0 roles:**
- **libmdbx** — add 5th sub-database (`tombstone_map`); expose storage stats via `env.get_info()` (verified in `build/_deps/libmdbx-src/mdbx.h++` line 5486). `MDBX_MAX_DBI` default is 32768; current usage is 4 DBIs; adding a 5th is trivial.
- **Asio (C++20 coroutines)** — expiry coroutine cancellation via `asio::cancellation_signal`; SIGUSR1 metrics dump follows existing `sighup_loop()` member-function coroutine pattern exactly.
- **spdlog** — periodic metrics log line on 60-second timer; no Prometheus, no HTTP server, no external metrics endpoint needed.
- **nlohmann/json** — three new config fields: `max_storage_bytes`, `rate_limit_bytes_per_sec`, `sync_namespaces`.
- **`std::chrono::steady_clock`** — token bucket timing for per-connection rate limiting (30 lines, no library).

**What NOT to add:** prometheus-cpp (adds HTTP server + protobuf), StatsD client (UDP socket dependency), any rate limiting library (token bucket is 30 lines of code). PROJECT.md explicitly prohibits HTTP/REST API.

### Expected Features

**Must have (table stakes — all 6 blocking for production deployment):**
- **Tombstone index** — O(1) `has_tombstone_for()` via new `tombstone_map` mdbx DBI. Current O(n) scan over all blobs in a namespace is unacceptable at scale and sits on the ingest hot path.
- **Global storage limit** — `max_storage_bytes` config field, `Storage::used_bytes()` via libmdbx stats, `IngestError::storage_full`. Prevents node from filling disk and crashing the host.
- **Disk-full rejection** — New `StorageFull = 23` wire message type (additive, backward compatible). Peers receive actionable signal instead of a silent drop or generic error.
- **Graceful shutdown** — SIGTERM drains coroutines cleanly. Primary gap: `PeerManager::stop()` is not called from the signal path; sync coroutines do not check `stopping_`; the expiry scan coroutine is `asio::detached` with no cancellation path.
- **Metrics/observability** — `NodeMetrics` struct (plain `uint64_t` — single io_context thread, not atomic), SIGUSR1 dump via spdlog, 60-second periodic log line.
- **Rate limiting** — Token bucket per `PeerInfo` (`write_tokens` + `last_token_refill_us`). Applies to `Data`/`Delete` messages only, not sync `BlobTransfer`. Feeds the existing strike system.

**Should have (differentiator, medium complexity):**
- **Namespace-scoped sync** — `sync_namespaces` config filter applied at Phase A namespace list assembly (before sending to peer), not at Phase C blob transfer. Zero protocol changes — local initiator filter only. Prevents leaking namespace IDs for filtered namespaces.
- **README in db/** — Operator documentation for config schema, startup, wire protocol overview.

**Defer (anti-features, explicitly out of scope):**
- Per-namespace storage quota — wrong layer; application concern for the relay, not the database node
- HTTP metrics endpoint — violates PROJECT.md "No HTTP/REST API" constraint
- Persistent rate limit state — rate limits are connection-scoped by design; reset on reconnect is correct
- Write-ahead rate limit with queuing — reject immediately, peer retries; queuing adds memory pressure for no gain

**Already shipped (NOT new for v0.4.0):**
- Persistent peer list — `peers.json`, `load_persisted_peers`, `save_persisted_peers` already exist in `peer_manager.cpp`. The gap is wiring `save_persisted_peers()` into `stop()` and using atomic file write (temp + fsync + rename).

### Architecture Approach

All 8 features integrate within the existing three-layer architecture without any subsystem-level changes. `peer_manager.cpp` is the central integration point for the majority of features; `storage.cpp` and `engine.h` handle storage-side concerns. No new coroutine primitives, no new networking, no new protocol handshakes.

**Major components and their v0.4.0 changes:**
1. **Storage** (`db/storage/`) — add `tombstone_map` 5th DBI (mirrors `delegation_map` pattern exactly); add `used_bytes()` / `storage_stats()` methods; populate tombstone index in the same transaction as the tombstone blob write.
2. **BlobEngine** (`db/engine/`) — add `storage_full` to `IngestError`; insert capacity check as new Step 0 (before namespace derivation and signature verification) inside the synchronous `ingest()` call, not in the coroutine body.
3. **PeerManager** (`db/peer/`) — token bucket fields in `PeerInfo`; `NodeMetrics` member; SIGUSR1 signal set (follows `sighup_loop` pattern); fix `stop()` to: set `stopping_` → call `save_persisted_peers()` → cancel all timers → call `server_.stop()`; namespace filter set built from config at startup.
4. **Config** (`db/config/`) — three new fields: `max_storage_bytes`, `rate_limit_bytes_per_sec` + `rate_limit_burst`, `sync_namespaces`.
5. **Wire protocol** (`db/wire/transport.fbs`) — add `StorageFull = 23` to `TransportMsgType` enum. Additive; old nodes log-and-skip unknown types in `on_peer_message`.
6. **main.cpp** — wire `asio::cancellation_signal` for expiry scan coroutine; verify `PeerManager::stop()` is called from SIGTERM path.

### Critical Pitfalls

1. **Storage limit check stale after co_await** — The check MUST live inside `BlobEngine::ingest()` which is fully synchronous. Any check in the coroutine body before dispatching to the engine is not atomic with the write. Another coroutine can write past the limit during any `co_await` suspension.

2. **MDBX_MAP_FULL before logical quota fires** — libmdbx's physical `size_upper` geometry limit can be hit before the application quota if they are too close. Set `size_upper` to at least 2x `max_storage_bytes`. Also map `MDBX_MAP_FULL` explicitly to `StoreResult::StorageFull` inside `store_blob()` so the error reaches the wire protocol correctly instead of becoming a silent generic error.

3. **Graceful shutdown blocks 120s per stuck sync peer** — `recv_sync_msg()` has `BLOB_TRANSFER_TIMEOUT` (120 seconds). On `stop()`, cancel all `peer.sync_notify` timers immediately to wake blocked coroutines. Check `stopping_` immediately after every timer expiry. Set a hard 3-second backstop timer as a fallback.

4. **io_context destroyed with live coroutine frames** — Do NOT call `io_context::stop()` to initiate shutdown. Cancel timers and close sockets via `server_.stop()`; let the io_context drain naturally when all coroutines `co_return`. Destructor order must be: cancel timers → server stop → `ioc.run()` returns → destroy objects.

5. **Non-atomic tombstone index + blob write** — Both the `blobs_map` entry and the `tombstone_map` index entry must be written in a single libmdbx transaction. Copy the `delegation_map` pattern exactly — it already writes the delegation blob and its index in one transaction.

6. **Raw `PeerInfo*` dangling across co_await** — Never store a raw `PeerInfo*` across any suspension point. Re-lookup via `find_peer(conn)` after every `co_await`. Prefer the strike+disconnect approach (instantaneous, no suspension) over backpressure `co_await` delays.

7. **peers.json corrupted on crash** — Use write-to-temp + fsync + rename. Directory fsync is required on Linux for the rename to survive power loss. Do NOT call `fsync` inside `on_peer_disconnected()` — it blocks the io_context thread. Use a 30-second periodic flush timer plus a flush on clean shutdown.

## Implications for Roadmap

Based on combined research, the natural phase structure is 4 phases covering 10 plans with a clear dependency ordering:

### Phase A: Storage Foundation

**Rationale:** Tombstone index is the highest-priority correctness fix — on the hot path for every ingest, affects all subsequent features. Storage limits and disk-full reporting share `IngestError::storage_full` and are two sides of one feature. All three ship first because downstream phases (metrics, rate limiting) call `engine_.ingest()` and require these paths to be correct. No other features depend on the tombstone index being slow; all features depend on storage limits being correct.

**Delivers:** O(1) tombstone lookups replacing the O(n) scan, bounded disk usage enforced at the protocol boundary, wire-level storage full signal to peers.

**Addresses:** Tombstone index, global storage limit, disk-full rejection.

**Avoids:** MDBX_MAP_FULL before quota fires (set `size_upper` = 2x limit), stale limit check after co_await (check inside `ingest()` only), non-atomic index + blob write (single transaction; copy delegation_map).

**Plans:**
- PLAN-01: Tombstone index (`tombstone_map` DBI + startup migration scan + `has_tombstone_for` O(1) replacement)
- PLAN-02: Storage limits (`Storage::used_bytes()`, `IngestError::storage_full`, `Config::max_storage_bytes`, Step 0 capacity check in `ingest()`)
- PLAN-03: Disk-full reporting (`StorageFull = 23` wire message, `peer_is_full` in `PeerInfo`, send response on `storage_full` in `on_peer_message`)

### Phase B: Operational Stability

**Rationale:** Graceful shutdown and persistent peer list are tightly coupled at `PeerManager::stop()` — both require modifying the same function. Building them as one phase avoids implementing `stop()` twice. Metrics logically follows: it needs storage stats from Phase A for the used-bytes log line and should be in place before rate limiting so drop counters are wired from the start.

**Delivers:** Clean SIGTERM behavior with bounded shutdown time, peer list persistence across restarts, runtime observability via SIGUSR1 dump and periodic log line.

**Addresses:** Graceful shutdown, persistent peer list (wiring gap), metrics logging.

**Avoids:** Shutdown blocking 120s per peer (cancel `sync_notify` timers in `stop()`), io_context lifetime violation (cancel then drain, not stop()), peers.json corruption (temp + fsync + rename + dir fsync), fsync on every disconnect (periodic 30s timer flush), uint32_t counter overflow (use uint64_t throughout), atomic overhead on hot path (plain uint64_t not atomic on single-threaded io_context).

**Plans:**
- PLAN-04: Graceful shutdown (`stop()` sequence: `stopping_` → save peers → cancel all timers → `server_.stop()`; expiry coroutine cancellation signal in `main.cpp`)
- PLAN-05: Persistent peer list wiring (atomic file write pattern, 30s flush timer, flush on clean shutdown)
- PLAN-06: Metrics (`NodeMetrics` struct, counter placement at event sites, SIGUSR1 dump coroutine following `sighup_loop` pattern)

### Phase C: Abuse Prevention and Topology Control

**Rationale:** Rate limiting depends on metrics being in place (for `blobs_rate_limited` counter) and is independent of namespace-scoped sync. Both are lower-risk than Phase A/B features and build after the system is stable.

**Delivers:** Per-connection write rate limiting preventing abuse on open nodes, operator control over which namespaces replicate to which peers.

**Addresses:** Rate limiting, namespace-scoped sync.

**Avoids:** Raw `PeerInfo*` dangling after co_await (re-lookup after every suspension; prefer strike+disconnect), blocking sleep stalling io_context (never `sleep_for` in coroutine), rate limiting sync traffic (applies to Data/Delete only, not BlobTransfer), namespace filter at Phase C leaking namespace IDs (filter at Phase A namespace list assembly, not at blob transfer time).

**Plans:**
- PLAN-07: Rate limiting (token bucket in `PeerInfo`, config fields, strike integration, metrics counter)
- PLAN-08: Namespace-scoped sync (config `sync_namespaces`, `std::set` built at startup, filter in `run_sync_with_peer` before Phase A namespace list encoding)

### Phase D: Documentation and Release

**Rationale:** README documents what exists after all features are stable and tested. Version bump is last so version-string assertions in tests do not generate CI noise during development.

**Delivers:** Operator documentation, version 0.4.0 tag-ready binary.

**Addresses:** README in `db/`, version bump.

**Avoids:** Version bump breaking test assertions (search for version string assertions first, then bump as final step).

**Plans:**
- PLAN-09: README (`db/README.md`: config schema, startup, wire protocol overview, example flows)
- PLAN-10: Version bump (`version.h` → 0.4.0, after all features pass all tests)

### Phase Ordering Rationale

- Phase A before Phase B: Storage limit enforcement is correct before observability is wired. `Storage::used_bytes()` from Phase A is required by the metrics log line in Phase B.
- Phase B before Phase C: Metrics must be in place before rate limiting so `blobs_rate_limited` is wired at feature-build time. Graceful shutdown must be correct before adding more signal handlers (SIGUSR1 in Phase B metrics).
- Phase C before Phase D: All features must be stable before the README documents them and the version is bumped.
- Tombstone index first within Phase A: It is on the ingest hot path, has no dependencies on other v0.4.0 features, is the smallest isolated change in the milestone, and tests cleanly in isolation before touching storage limits.

### Research Flags

All phases have well-documented patterns from direct codebase inspection. No phase requires `/gsd:research-phase` during planning. Specific notes:

- **Phase A:** Implementation details fully specified in STACK.md and ARCHITECTURE.md. libmdbx API verified against actual headers. The `delegation_map` write pattern in `storage.cpp` is the exact template for `tombstone_map`.
- **Phase B:** Shutdown sequence specified in ARCHITECTURE.md with gaps identified. `sighup_loop()` in `peer_manager.cpp` is the exact template for `sigusr1_loop()`. Atomic file write is textbook POSIX.
- **Phase C:** Token bucket algorithm is specified completely and fits in 30 lines. Namespace filter is a 3-line change to `run_sync_with_peer`. No unknowns.
- **Phase D:** No research needed. Documentation and `version.h` edit only.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All claims verified against actual headers in `build/_deps/`. Zero new dependencies confirmed. libmdbx API for storage stats verified at specific lines. |
| Features | HIGH | Existing codebase inspected directly. Persistent peer list confirmed already shipped at source level. Complexity estimates are line-level. All anti-features justified against specific PROJECT.md constraints. |
| Architecture | HIGH | Every integration point specified by file, method, and where relevant, line number. All patterns (delegation_map, sighup_loop, deque, Step 0 ordering) are existing code being extended, not invented. |
| Pitfalls | HIGH | Critical pitfalls derived from project RETROSPECTIVE.md, libmdbx issue tracker, and Asio coroutine lifetime documentation. Not speculative — all grounded in concrete failure modes. |

**Overall confidence:** HIGH

### Gaps to Address

- **Counter drift vs. direct libmdbx query:** STACK.md recommends querying libmdbx stats directly. PITFALLS.md identifies that an in-memory running counter drifts after crashes and misses decrement paths. Resolution is clear: always query libmdbx `env.get_info()`. PLAN-02 scope must explicitly include startup recomputation (one `compute_used_bytes()` call before accepting any ingests) rather than assuming the counter starts at zero.

- **io_context drain vs. hard stop():** ARCHITECTURE.md suggests `io_context::stop()` as a backstop after 5 seconds. PITFALLS.md warns against calling `io_context::stop()` while coroutines hold live references. These are compatible: prefer natural drain; `io_context::stop()` fires only after the configurable hard timeout if natural drain has not completed. PLAN-04 must document this as the fallback, not the primary mechanism.

- **Metrics counter type:** STACK.md drafted `NodeMetrics` with `std::atomic<uint64_t>`. PITFALLS.md recommends plain `uint64_t` on the single-threaded io_context path to avoid MFENCE overhead on the hot ingest path. Resolution: use plain `uint64_t` for all counters updated on the io_context thread; document that atomics would be needed only if a cross-thread reader is added in the future.

## Sources

### Primary (HIGH confidence — source code verified)
- `db/storage/storage.h` / `storage.cpp` — Storage API, sub-database structure, `has_tombstone_for` O(n) scan, `delegation_map` write pattern
- `db/engine/engine.h` — `IngestError` enum, ingest pipeline, Step 0 ordering
- `db/peer/peer_manager.h` / `peer_manager.cpp` — `PeerInfo` struct, persistence infrastructure, `sighup_loop` pattern, `stopping_` flag
- `db/net/server.h` / `server.cpp` — `drain()` coroutine, SIGINT/SIGTERM handling
- `db/wire/transport_generated.h` — `TransportMsgType` enum (current max: `Notification = 22`)
- `db/main.cpp` — expiry scan coroutine (`asio::detached`, no cancellation path in current code)
- `build/_deps/libmdbx-src/mdbx.h` lines 2808-2886 — `MDBX_envinfo` struct
- `build/_deps/libmdbx-src/mdbx.h++` lines 5486-5495 — `env::get_info()` C++ wrapper
- `.planning/RETROSPECTIVE.md` — SIGHUP lambda stack-use-after-return, deque invalidation at co_await

### Secondary (MEDIUM confidence)
- [CockroachDB node shutdown](https://www.cockroachlabs.com/docs/dev/node-shutdown) — drain sequence: stop accepting → drain in-flight → disconnect
- [Nostr NIP-11](https://nips.nostr.com/11) — storage capacity advertisement patterns
- [LWN: crash-safe atomic file write](https://lwn.net/Articles/457667/) — temp + fsync + rename + directory fsync required on Linux
- [libmdbx MDBX_MAP_FULL issue #136](https://github.com/erthink/libmdbx/issues/136) — confirmed production failure mode
- [Token bucket algorithm reference](https://intronetworks.cs.luc.edu/current/html/tokenbucket.html) — algorithm correctness
- [travisdowns.github.io: concurrency costs](https://travisdowns.github.io/blog/2020/07/06/concurrency-costs.html) — atomic fence overhead on hot paths
- [Asio coroutine cancellation: chriskohlhoff/asio#1574](https://github.com/chriskohlhoff/asio/issues/1574) — object lifetime with co_spawn
- [PouchDB filtered replication](https://pouchdb.com/2015/04/05/filtered-replication.html) — namespace-scoped sync pattern
- [IPFS Kubo storage quota #972](https://github.com/ipfs/kubo/issues/972) — StorageMax config, mi_used_pgno tracking

---
*Research completed: 2026-03-08*
*Ready for roadmap: yes*
