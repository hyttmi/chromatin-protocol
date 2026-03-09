# Feature Landscape

**Domain:** Production readiness hardening for decentralized PQ-secure database node daemon
**Researched:** 2026-03-08
**Milestone:** v0.4.0

## Context: What Already Exists

Before categorizing new features, the following are shipped and NOT re-implemented:

- Signed blob storage with namespace ownership (SHA3-256(pubkey) = namespace)
- PQ-encrypted transport (ML-KEM-1024 + ChaCha20-Poly1305)
- Hash-list diff sync (bidirectional, one-blob-at-a-time)
- ACL (allowed_keys, open/closed mode, SIGHUP reload)
- Delegation (write-only delegates via signed delegation blobs)
- Pub/sub notifications (connection-scoped subscriptions)
- Blob deletion via replicated tombstones
- TTL/expiry (7-day default, TTL=0 permanent)
- 100 MiB blob support
- PEX peer discovery
- Persistent peer list (peers.json via `load_persisted_peers` / `save_persisted_peers`)
- Strike system for misbehaving peers

The `PersistedPeer` struct and `peers.json` machinery already exist in `peer_manager.h`. Persistent peer list is NOT a new feature for v0.4.0 — it is already shipped.

---

## Table Stakes

Features that are baseline expectations for a production node daemon. Missing any of these makes the system feel unfinished or unsafe to deploy.

| Feature | Why Expected | Complexity | Dependencies on Existing | Notes |
|---------|--------------|------------|--------------------------|-------|
| **Global storage limit with enforcement** | Every production storage system has a quota. Without a cap, a node fills its disk and crashes the entire host. IPFS Kubo has `StorageMax` config. Nostr strfry uses LMDB `mapsize`. Production daemons always have this. | MED | `storage::Storage`, `config::Config`, `engine::BlobEngine::ingest()`, `engine::BlobEngine::delete_blob()` | Add `max_storage_bytes` to Config (default: unlimited, 0 = no limit). `Storage` exposes `used_bytes()` via `mdbx_env_info` (stat.mi_used_pgno * page_size). Engine checks before ingest. Return `IngestError::storage_full` when exceeded. |
| **Disk-full protocol rejection** | Peers must receive a clear rejection, not a silent connection drop or partial write. Standard P2P pattern: nodes signal capacity limits explicitly so peers can route around them. Nostr NIP-11 defines `max_content_length` and `retention` capacity limits. | LOW | `engine::IngestError`, `wire/transport.fbs` (new `StorageFull` msg type or reuse `Goodbye`), `peer_manager.cpp` | On `IngestError::storage_full`, send `WriteNack` with `storage_full` reason to the writing peer. For inbound sync blobs, skip ingest and log. No protocol change needed if we reuse existing Nack path — just add a new rejection reason code. |
| **Graceful shutdown** | SIGTERM must drain in-flight work cleanly. Operators expect that killing the daemon does not corrupt the database or leave partial sync state. cockroachdb, gRPC, every Unix daemon standard: stop accepting, finish in-flight, close connections with Goodbye, flush storage. | MED | `PeerManager::stop()`, `net::Server`, `asio::io_context`, `storage::Storage` destructor, `main.cpp` | `stop()` already exists on PeerManager. Graceful shutdown extends it: (1) stop accepting new connections, (2) send Goodbye to all peers, (3) cancel sync timers, (4) wait for active coroutines to finish (bounded timeout, e.g. 10s), (5) close storage. The `stopping_` flag already exists. |
| **Tombstone index (O(1) has_tombstone_for)** | `has_tombstone_for()` currently scans all blobs in a namespace linearly (O(n)). Called on every ingest to prevent resurrection of deleted blobs. With high-write namespaces this becomes a performance bottleneck. Fixed-cost lookup is required for production correctness. | MED | `storage::Storage::has_tombstone_for()`, `storage.cpp` (Storage::Impl), libmdbx sub-database | Add a `tombstone_map` sub-database in libmdbx: key = `[namespace:32][target_hash:32]`, value = empty (or timestamp). `has_tombstone_for` becomes a btree cursor seek = O(log n) btree, effectively O(1) amortized. Backfill on open: scan existing blobs for tombstone magic, populate index. |
| **Operational metrics logging** | Operators need visibility into what the node is doing. How many peers connected? How many blobs stored? Sync stats? Rate-limited attempts? Disk usage? Without this, production debugging is blind. Standard pattern: periodic log line with key counters (spdlog already in use). | LOW | `spdlog` (already dep), `PeerManager`, `storage::Storage`, `engine::BlobEngine` | No new dependency needed. Add a `Metrics` struct with atomic counters: `blobs_stored`, `blobs_rejected`, `bytes_stored`, `peers_connected`, `sync_rounds`, `rate_limited_count`. Log via spdlog on a 60s timer (or at SIGHUP). Extend `SyncStats` struct already in `sync_protocol.h`. |
| **Rate limiting for write abuse** | Open nodes are exposed to write floods. Without per-connection rate limiting, a single peer can saturate disk I/O and starve sync. Nostr relays (strfry, nostr-relay Python) universally implement rate limiting. Token bucket is the standard algorithm (constant memory, handles bursts). | MED | `peer_manager.cpp` (`on_peer_message()`), `config::Config` | Token bucket per connection: `max_writes_per_second` (default: 10), `burst_size` (default: 50). Track in `PeerInfo` struct (two uint64 fields: `rate_tokens`, `rate_last_refill_ns`). Check before processing `Data`/`Delete` messages. On limit: send Nack with `rate_limited` reason, increment strike. No new dependencies — stdlib chrono for timing. |
| **README with usage and interaction samples** | A deployed daemon needs documentation for operators. What config fields exist? How do you connect a client? What does the wire protocol look like at a high level? | LOW | None | Write `db/README.md` with: node startup, config JSON schema, how to connect, example wire flow. |

---

## Differentiators

Features that add real operational value beyond minimum. Not strictly required, but make the system notably better to run in production.

| Feature | Value Proposition | Complexity | Dependencies on Existing | Notes |
|---------|-------------------|------------|--------------------------|-------|
| **Namespace-scoped sync** | Filter which namespaces sync with which peers. Allows a node to act as a partial mirror: "I replicate namespaces A and B, not C." Useful for tiered storage (low-disk node mirrors only priority namespaces). Analogous to Nostr relay `retention` spec (NIP-11) and PouchDB filtered replication. | HIGH | `sync::SyncProtocol`, `PeerManager::run_sync_with_peer()`, `peer_manager.cpp`, `config::Config` | Two approaches: (a) per-peer config (which namespaces to sync with which address), or (b) global namespace filter (which namespaces this node stores at all). Option (b) is simpler and YAGNI-aligned. Add `namespace_filter` to Config: if non-empty, only namespaces in the list are stored and synced. Check at ingest time (before crypto verification) and at sync namespace list assembly. |
| **Storage usage in metrics** | Report current disk usage (bytes used, percentage of limit, number of blobs) as part of the periodic metrics log. Operators need to know when to increase limits or expand storage before hitting the wall. | LOW | `storage::Storage` (needs `used_bytes()` method), `Metrics` struct | Add `Storage::used_bytes()` using `mdbx_env_info()` stat fields. Include in the 60s metrics log line. Feeds the storage-full enforcement logic as well (dual use). |
| **Rate limit config exposure** | Make rate limiting parameters configurable rather than hardcoded. `max_writes_per_second` and `burst_size` in the JSON config. Different deployments (closed node with 5 trusted peers vs open node with 100 unknown peers) need different policies. | LOW | `config::Config`, rate limiter in `PeerInfo` | Single-field addition to Config. Default values are safe for both open and closed mode. |
| **Graceful shutdown timeout config** | Let operators tune the drain timeout (default 10s). Some deployments have large in-flight blobs (100 MiB) that need more time to finish. Others want fast restart. | LOW | `config::Config`, graceful shutdown logic in `main.cpp` | Add `shutdown_timeout_seconds` to Config (default: 10). Used in the shutdown wait loop. |

---

## Anti-Features

Features to explicitly NOT build for v0.4.0. Either wrong layer, wrong time, or add complexity exceeding value.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| **Per-namespace storage quota** | Different quotas per namespace adds config complexity (per-namespace config map) and tracking overhead (separate counter per namespace). The global limit covers the actual problem: running out of disk. Namespace-level quotas are an application layer concern (relay decides how much of its namespace to use). | Global storage limit with namespace-scoped sync to control which namespaces are stored at all. |
| **HTTP metrics endpoint (Prometheus scrape)** | Adding an HTTP listener means a new socket, a new dependency (HTTP server or raw socket code), a new attack surface, and diverges from the "binary PQ protocol only" constraint in PROJECT.md. | spdlog periodic log lines. Operators scrape logs. If Prometheus is needed, add a log exporter in the relay layer — not the database node. |
| **Persistent rate limit state** | Storing rate limit buckets across restarts adds serialization/deserialization for per-connection ephemeral state. Rate limits reset on reconnect naturally — this is correct and expected behavior. | Token buckets are connection-scoped, initialized on connect, reset on disconnect. Same model as pub/sub subscriptions. |
| **Write-ahead rate limit with queuing** | A leaky-bucket queue that delays writes instead of rejecting them. Adds memory pressure (blob queue), latency, and complexity. On a busy open node, a full queue is indistinguishable from disk-full. | Reject immediately with a Nack. Peer retries. This is the correct behavior for a storage node. |
| **Dynamic rate limit adjustment** | Auto-tuning rate limits based on peer behavior or disk pressure. This is ML-level complexity for a storage daemon. | Static config. Operator adjusts if needed. |
| **Filesystem-level storage (ext4/btrfs quotas)** | Using OS-level quotas instead of application-level enforcement means libmdbx writes fail at the OS level with ENOSPC, which is crash-unsafe for LMDB-style MVCC (mid-transaction ENOSPC = corruption risk). | Application-level check in `engine::BlobEngine::ingest()` before any write: if `used_bytes() >= max_storage_bytes`, reject with `storage_full`. |
| **Streaming metrics (event-based observability)** | Emitting structured events per blob ingest/delete/sync to an external system (OpenTelemetry, InfluxDB) adds dependencies, configuration, and network I/O on the hot path. | Periodic counter log. Counters accumulate, snapshot every 60s. Cheap, zero-dependency. |
| **Namespace-scoped rate limits** | Different write rates per namespace adds a map lookup per write. The strike system already handles persistent abuse (N strikes = disconnect). Rate limiting is per-connection, not per-namespace. | Per-connection token bucket. Strikes handle namespace-specific spammers. |

---

## Feature Dependencies

```
Storage::used_bytes()
    -> Global storage limit check (engine::BlobEngine::ingest needs the value)
    -> Disk-full protocol rejection (IngestError::storage_full triggers rejection)
    -> Storage usage in metrics (dual-use)

Global storage limit
    -> Disk-full rejection (two sides of the same feature)

Tombstone index (tombstone_map sub-db)
    -> Storage::has_tombstone_for() O(1) (replaces O(n) scan)
    -> Must backfill on first open from existing blobs (migration)

Graceful shutdown
    -> PeerManager::stop() extension (already exists, extend drain logic)
    -> Goodbye on all peers before close (existing Goodbye message type)

Metrics struct
    -> Storage usage (needs used_bytes())
    -> Rate limiter (contributes rate_limited_count)
    -> Sync stats (SyncStats already exists, aggregate into Metrics)

Rate limiting (PeerInfo token bucket)
    -> Per-connection: lives in PeerInfo (no external dep)
    -> Feeds strike system (rate limit excess = strike)

Namespace-scoped sync
    -> Config namespace_filter
    -> Engine ingest (early rejection for filtered namespaces)
    -> SyncProtocol::collect_namespace_hashes (skip filtered namespaces during hash collection)
    -> PeerManager: filter NamespaceList before sending to peers
```

---

## MVP Recommendation

**Priority 1 -- Correctness and Safety (must ship):**
1. **Tombstone index** -- O(n) `has_tombstone_for` is a correctness-adjacent performance bug. Called on every ingest. Fix first because it affects the hot path for all subsequent features (storage limit enforcement also calls ingest).
2. **Global storage limit + disk-full rejection** -- Prevents node from crashing the host. Requires `Storage::used_bytes()` first.
3. **Graceful shutdown** -- SIGTERM safety. Operators must be able to restart the daemon without risking database corruption.

**Priority 2 -- Operations (high value, low risk):**
4. **Metrics logging** -- Operational visibility. No dependency on P1 features. Can be built in parallel. Uses existing spdlog, existing SyncStats struct.
5. **Rate limiting** -- Abuse prevention for open nodes. Simple token bucket in PeerInfo. Feeds strike system.

**Priority 3 -- Topology control (differentiator, MED complexity):**
6. **Namespace-scoped sync** -- Allows partial mirrors. More complex protocol interaction. Build after P1+P2 are stable.

**Documentation:**
7. **README in db/** -- Write after features are finalized. No code risk.
8. **Version bump to 0.4.0** -- Last step.

**Defer or descope:**
- Per-namespace storage quota: wrong layer, YAGNI
- HTTP metrics endpoint: violates protocol constraints
- Everything in anti-features

---

## Implementation Notes

### Tombstone Index Migration

The tombstone index is a net-new libmdbx sub-database (`tombstone_map`). On first open after upgrading, `Storage::open()` must scan all existing blobs for tombstone magic (`0xDEADCAFE` + 32-byte target hash) and backfill the index. This is a one-time O(total blobs) migration that runs on startup. It is fast (index-only reads) and idempotent (can safely re-run). Key = `[namespace:32][target_hash:32]`, value = empty or single byte. The existing `has_tombstone_for` API signature is unchanged — only the implementation changes.

### Storage Used Bytes via libmdbx

`mdbx_env_info()` returns `MDBX_envinfo` with fields:
- `mi_geo.current` = current database geometry (file size)
- `mi_used_pgno` = number of pages in use
- `page_size` = page size in bytes

`used_bytes = mi_used_pgno * page_size` gives the amount of storage actually used by data (not file size, which includes free list pages). This is the correct number to compare against `max_storage_bytes` because it reflects actual data on disk, not pre-allocated geometry.

The check in `engine::BlobEngine::ingest()`:
```
// Step 0 (before structural checks): storage limit
if (max_storage_bytes_ > 0 && storage_.used_bytes() >= max_storage_bytes_) {
    return IngestResult::rejection(IngestError::storage_full, "node storage limit reached");
}
```

This check is cheap (single libmdbx stat call, no disk I/O) and belongs before signature verification. Consistent with the "Step 0: cheapest validation first" decision from v1.0.

### Rate Limiter: Token Bucket in PeerInfo

No new struct needed. Add two fields to `PeerInfo`:
- `uint64_t rate_tokens = BURST_SIZE` — current token count (scaled by 1000 to avoid float)
- `uint64_t rate_last_refill_ns = 0` — timestamp of last token refill

Refill on every message: `elapsed_ns = now - last_refill_ns; tokens += elapsed * rate_per_ns; clamp to BURST`. On write message: if `tokens < 1000`, reject with Nack + strike. Otherwise `tokens -= 1000`.

This is pure stdlib chrono arithmetic. Zero allocation. O(1). Correct for a coroutine-based system because `on_peer_message()` is single-threaded per connection.

### Graceful Shutdown Sequence

Current `PeerManager::stop()` cancels timers. Extend to:
1. Set `stopping_ = true` (already exists, prevents new sync rounds)
2. Stop `net::Server` accept loop (no new connections)
3. Send `Goodbye` to all connected peers (iterate `peers_`, `conn->send(Goodbye)`)
4. Wait up to `shutdown_timeout_seconds` for coroutines to finish
5. `io_context.stop()` if timeout exceeded
6. `Storage` destructor handles MDBX close (already RAII)

The existing `Goodbye` message type (TransportMsgType_Goodbye = 7) handles clean peer notification. No new wire messages needed.

### Namespace-Scoped Sync

Config field: `std::vector<std::string> namespace_filter` (hex-encoded namespace IDs, 64 chars each). If empty = replicate all (default, backward compatible).

Three enforcement points:
1. **Engine ingest**: if `namespace_filter` non-empty and namespace not in filter, return `IngestError::namespace_not_accepted` (new error code). Peers learn quickly not to send blobs for filtered namespaces via consistent rejection.
2. **SyncProtocol: NamespaceList assembly**: filter out non-accepted namespaces from the list sent to peers in Phase A of sync. Peers only request hashes for namespaces we advertise.
3. **SyncProtocol: hash collection**: already namespace-scoped. No change needed.

The filter is checked at ingest (fast: set lookup) and at NamespaceList time (also fast: filter once per sync round). No changes to the wire protocol — the existing NamespaceList message carries what we choose to send.

### Metrics Struct

```cpp
struct Metrics {
    std::atomic<uint64_t> blobs_stored{0};
    std::atomic<uint64_t> blobs_rejected{0};
    std::atomic<uint64_t> blobs_rate_limited{0};
    std::atomic<uint64_t> sync_rounds_completed{0};
    std::atomic<uint64_t> peers_connected_total{0};
    // Point-in-time (read from PeerManager/Storage on log)
    // peers_currently_connected: peers_.size()
    // bytes_used: storage_.used_bytes()
};
```

Logged every 60s via spdlog at INFO level:
```
[metrics] peers=4 blobs_stored=1423 blobs_rejected=12 rate_limited=3 sync_rounds=87 used_bytes=48MB/10GB
```

Atomic counters because metrics are updated from the coroutine thread but could be read from a signal handler (SIGHUP). In practice, the system is single-threaded on `io_context`, but atomics are cheap insurance.

---

## Complexity Assessment

| Feature | Lines of Code (est.) | Risk | Touches |
|---------|---------------------|------|---------|
| Tombstone index (new MDBX sub-db + migration) | ~80 | MED | storage.cpp (Impl), storage.h |
| Storage::used_bytes() | ~15 | LOW | storage.cpp, storage.h |
| Global storage limit + IngestError::storage_full | ~30 | LOW | engine.h, engine.cpp, config.h |
| Disk-full rejection (Nack reason) | ~20 | LOW | peer_manager.cpp |
| Graceful shutdown (drain + timeout) | ~50 | MED | peer_manager.cpp, main.cpp, config.h |
| Metrics struct + 60s log timer | ~60 | LOW | new metrics.h, peer_manager.cpp |
| Rate limiting (token bucket in PeerInfo) | ~50 | MED | peer_manager.h (PeerInfo), peer_manager.cpp |
| Namespace-scoped sync | ~60 | MED | config.h, engine.cpp, sync_protocol.cpp, peer_manager.cpp |
| README in db/ | ~100 lines doc | LOW | db/README.md |
| Version bump | ~5 | LOW | version.h |
| **Total (all features)** | **~370** | | |

Risk notes:
- Tombstone index migration is the highest-risk change: modifies the on-disk database schema. Requires testing with existing populated databases. Must be idempotent.
- Graceful shutdown coroutine drain has subtle timing: must not cancel timers that are mid-operation. Use `stopping_` flag + bounded wait rather than hard cancel.
- Namespace-scoped sync changes NamespaceList assembly: must not break existing open-mode behavior (empty filter = send all namespaces, as today).

---

## Sources

- [Nostr NIP-11 Relay Information Document](https://nips.nostr.com/11) — capacity limits, max_content_length, retention specs; establishes pattern for protocol-level capability advertisement
- [IPFS Kubo storage quota discussion](https://github.com/ipfs/kubo/issues/972) — StorageMax config, mi_used_pgno pattern for tracking usage
- [nostr-relay rate limiting docs](https://code.pobblelabs.org/nostr_relay/doc/tip/docs/rate_limits.md) — per-IP and per-pubkey token bucket configuration
- [CockroachDB node shutdown](https://www.cockroachlabs.com/docs/dev/node-shutdown) — authoritative reference for distributed node drain sequence: stop accepting -> drain in-flight -> disconnect
- [Graceful shutdown in distributed systems (GeeksForGeeks)](https://www.geeksforgeeks.org/system-design/graceful-shutdown-in-distributed-systems-and-microservices/) — standard pattern: signal -> stop accepting -> drain -> close
- [Token bucket algorithm](https://intronetworks.cs.luc.edu/current/html/tokenbucket.html) — algorithm reference for per-connection rate limiting
- [Bitcoin Core peers.dat](https://raghavsood.com/blog/2018/05/20/demystifying-peers-dat/) — reference for persistent peer list format (chromatindb already has peers.json implemented)
- [PouchDB filtered replication](https://pouchdb.com/2015/04/05/filtered-replication.html) — namespace/topic-scoped sync pattern
- [prometheus-cpp-lite](https://github.com/biaks/prometheus-cpp-lite) — header-only metrics (NOT recommended for chromatindb; spdlog log lines are sufficient and zero-dependency)
- Existing codebase: `storage.h` (StoreResult, Storage API), `engine.h` (IngestError enum), `peer_manager.h` (PeerInfo, PersistedPeer, stopping_ flag), `sync_protocol.h` (SyncStats struct)
