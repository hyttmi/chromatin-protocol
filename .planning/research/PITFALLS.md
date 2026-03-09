# Domain Pitfalls

**Domain:** Production readiness features on a C++20 coroutine-based P2P blob store
**Project:** chromatindb v0.4.0
**Researched:** 2026-03-08
**Confidence:** HIGH (verified against actual source code and official documentation)

---

## Critical Pitfalls

Mistakes that cause data loss, hard-to-diagnose production failures, or require rewrites.

---

### Pitfall 1: MDBX_MAP_FULL Crashes Before the Logical Storage Limit Fires

**What goes wrong:** libmdbx's environment has a `size_upper` geometry limit set at `env_open`
time (error code `MDBX_MAP_FULL`, -30792). If the logical storage quota (`max_storage_bytes`
config) is set too close to the physical mapsize limit, the mdbx write transaction can fail with
`MDBX_MAP_FULL` before the application's quota check triggers. The blob ingest path returns
`StoreResult::Error` — but this is indistinguishable from any other storage error. The protocol
sends no specific rejection; the peer cannot tell "node is full" from "storage is broken."

**Why it happens:** Two separate limits exist: the logical quota the operator sets and the
physical mmap upper bound. If they are close to equal, or if the mapsize is never configured
large enough, the physical limit can be hit first under burst write load.

**Consequences:** Blob data is silently dropped with a generic error. Disk-full reporting
(the explicit v0.4.0 feature) never fires because the error is swallowed before reaching the
reporting path. The node appears healthy in logs but is discarding all ingest.

**Prevention:**
- Set libmdbx `size_upper` to at least 2x the configured `max_storage_bytes`. The physical
  limit should be unreachable under normal operation.
- Add a distinct `StoreResult::Status::StorageFull` enum value alongside `Stored`, `Duplicate`,
  and `Error`. Map `MDBX_MAP_FULL` to `StorageFull` explicitly inside `Storage::store_blob()`.
  This lets `BlobEngine::ingest()` and the protocol layer send a specific rejection.
- Add `IngestError::storage_full` to the engine's error enum and a corresponding
  `TransportMsgType::StorageFull` wire message so peers receive an actionable signal.

**Detection:** Write a Catch2 test that fills storage to logical quota and verifies the returned
`StoreResult::Status` is `StorageFull`, not `Error`. Also verify the process does not crash.

**Phase:** Storage limits + disk-full reporting (first feature phase).

---

### Pitfall 2: Storage Limit Check Is Stale After a co_await Suspension

**What goes wrong:** A coroutine checks `used_bytes_ < max_storage_bytes_` before suspending
(e.g., waiting on the sync inbox timer, or reading from the connection). Another coroutine runs
during the suspension and writes blobs until the limit is reached. When the first coroutine
resumes, it calls `storage_.store_blob()` without re-checking — writing past the limit.

**Why it happens:** The Asio `io_context` is single-threaded, so there are no data races in the
C++ memory model sense. But every `co_await` is a preemption point where other coroutines run.
A limit check before a `co_await` and an action after it are NOT atomic. This is the same class
of bug as the v1.0 deque-invalidation fix and the v2.0 SIGHUP lambda issue.

**Consequences:** Blobs are accepted beyond the configured limit. The node fills disk. The
disk-full reporting feature fires too late — at the OS write level, not the protocol boundary.

**Prevention:**
- Perform the storage limit check inside `BlobEngine::ingest()`, immediately before calling
  `storage_.store_blob()`. The engine is fully synchronous (no `co_await`), so there is no
  suspension between check and store. This is the only correct placement.
- Do NOT check the limit in the coroutine body before dispatching to the engine. Do NOT cache
  the byte count as a coroutine-local variable across any `co_await`.
- Follow the existing Step 0 pattern: the limit check is the new Step 0, before the existing
  `oversized_blob` check.

**Detection:** Write a test with two concurrently running ingest coroutines that together exceed
the limit by one blob. Verify the final stored byte count never exceeds `max_storage_bytes`.

**Phase:** Storage limits enforcement (BlobEngine::ingest integration).

---

### Pitfall 3: Storage Byte Counter Drifts From Reality

**What goes wrong:** A running counter `used_bytes_` is incremented on `StoreResult::Stored` and
must be decremented on every path that removes blob data. If any removal path is missed, the
counter reports more storage used than reality, causing premature "full" rejections. If
incremented on `StoreResult::Duplicate`, it double-counts. On restart, if the counter is not
recomputed from the database, it starts at zero — making the first restart accept unlimited
blobs until the next expiry scan recalibrates.

**The four decrement paths that must all be handled:**
1. `run_expiry_scan()` — deletes blobs whose TTL has elapsed.
2. `delete_blob()` in `BlobEngine` — tombstone creation calls `delete_blob_data()` on the target
   blob before storing the tombstone blob.
3. `has_blob()` returning duplicate — must NOT increment.
4. Node restart — the counter is zero; must be recomputed from a full DB scan before any ingest
   is accepted.

**Consequences:** Over-restrictive node rejects valid blobs prematurely. Or over-permissive node
accepts blobs past the limit. Both make the storage limit feature incorrect.

**Prevention:**
- Add a `compute_used_bytes()` method to `Storage` that scans all blobs and sums their data
  sizes. Call it once during `Storage` construction to initialize `used_bytes_`.
- Funnel all counter mutations through a single private `Storage::adjust_used_bytes(int64_t
  delta)` method. No ad-hoc incrementing at call sites.
- The tombstone creation path: decrement for the target blob that was deleted, then increment
  for the tombstone blob itself (tombstones are 36 bytes, so the net change is `tombstone_size -
  deleted_blob_size`).
- Add a Catch2 test that runs random store/delete/expire cycles and asserts that `used_bytes()`
  matches a fresh `compute_used_bytes()` scan at the end of each cycle.

**Phase:** Storage limits implementation.

---

### Pitfall 4: peers.json Corrupted on Crash During In-Place Write

**What goes wrong:** `save_persisted_peers()` writes JSON to `peers.json` directly (open, write,
close). If the process is killed (SIGKILL, OOM kill, power loss) between the open and close, the
file is left truncated or partially written. On the next startup, `load_persisted_peers()` calls
`nlohmann::json::parse()`, which throws on malformed input. If the exception is not caught, the
node crashes at startup. If it is caught but the peer list is discarded, the node starts with
zero peers and must rediscover from bootstrap nodes.

**Why it happens:** File writes are not atomic. Even if `write()` fits in one syscall, the
kernel can write partial pages to disk before `close()`. The existing `PeerManager` already has
`save_persisted_peers()` and `load_persisted_peers()` stubs — the crash-safety concern applies
specifically to how the write is implemented.

**Consequences:** Node cannot start after unclean shutdown without manual intervention, or it
starts with no peer knowledge and must re-bootstrap (slow, increases bootstrap peer load).

**Prevention:** Use the write-to-temp + fsync + rename pattern, which is atomic on all POSIX
filesystems:
```cpp
// 1. Write JSON to peers.json.tmp
// 2. fsync(tmp_fd) — flush data to stable storage
// 3. close(tmp_fd)
// 4. rename("peers.json.tmp", "peers.json") — atomic directory entry swap
// 5. open(parent_dir) + fsync(dir_fd) — durably commit the rename
```
The directory fsync (step 5) is required on Linux to guarantee the rename survives a power loss.
Without it, the rename may be lost on ext4/xfs even though the data is on disk.

**Detection:** Write a test that truncates `peers.json` to simulate a partial write, then
verifies the node starts successfully and loads an empty peer list (not crashes).

**Phase:** Persistent peer list implementation.

---

### Pitfall 5: Graceful Shutdown Blocks for 120 Seconds Per Stuck Sync Coroutine

**What goes wrong:** `run_sync_with_peer()` uses `recv_sync_msg()` with `BLOB_TRANSFER_TIMEOUT`
(120 seconds). If shutdown is signalled (SIGTERM) while this coroutine is suspended waiting for
a peer that has gone silent, the coroutine does not know about the shutdown signal. It waits the
full 120 seconds, then times out and returns. With 32 peers potentially in various stages of
sync, this can block clean exit for minutes. Systemd's `TimeoutStopSec` then fires a SIGKILL,
bypassing all cleanup.

**Why it happens:** The `stopping_` flag (already a member of `PeerManager`) is set during
`stop()`, but it is not checked inside `recv_sync_msg()` or used to cancel the `sync_notify`
timer. The timer-cancel pattern wakes coroutines when messages arrive, but no mechanism wakes
them when `stopping_` becomes true.

**Consequences:** Slow shutdown (10+ seconds for each stuck peer). Operator sees daemon hang.
If Systemd kills with SIGKILL, in-flight DB writes may be interrupted (libmdbx is crash-safe;
the DB will be consistent, but unsaved peer data may be lost).

**Prevention:**
- On `stop()` entry, iterate all `peers_` entries and cancel their `sync_notify` timers via
  `peer.sync_notify->cancel()` (the timer pointer is already in `PeerInfo`). This wakes all
  blocked `recv_sync_msg()` calls immediately.
- Inside `recv_sync_msg()`, check `stopping_` immediately after the timer fires and return
  `std::nullopt` if true.
- Set a hard maximum shutdown time (e.g., 3 seconds) using an `asio::steady_timer` that calls
  `io_context::stop()` as a backstop.

**Detection:** Test: start a sync with an unresponsive mock peer, then call `stop()`. Measure
elapsed time to `io_context::run()` returning. Must be under 1 second, not 120 seconds.

**Phase:** Graceful shutdown implementation.

---

### Pitfall 6: io_context Destruction With Live Coroutine Frames Causes Use-After-Free

**What goes wrong:** `io_context::stop()` signals the event loop to exit but does NOT wait for
outstanding coroutine frames to complete. If the `io_context` object (or objects associated with
it: `steady_timer`, `tcp::acceptor`, `signal_set`) is destroyed while any coroutine holds a
reference to it — through a captured `this` pointer, a timer, or a socket — the coroutine frame
contains a dangling reference. Resuming the coroutine (which may happen during the `io_context`
destructor's cleanup path) causes use-after-free.

**Why it happens:** This is the same class of bug as the SIGHUP lambda / stack-use-after-return
issue fixed in v2.0. `co_spawn`-ed coroutines are tied to the executor; their frames hold
references to the objects they use. The destructor order matters.

**Consequences:** Intermittent ASAN violations or segfaults at shutdown. These are often
dismissed as "only happens at exit" but indicate real memory safety bugs that can mask data
corruption under race conditions.

**Prevention:**
- Do NOT call `io_context::stop()` to initiate shutdown. Instead, use cancellation: cancel all
  timers and close all sockets (via `server_.stop()`). The `io_context` will return from `run()`
  naturally when all coroutines co_return.
- Destruction order must be: cancel timers → server stop → `io_context::run()` returns →
  destroy all objects. The `PeerManager` destructor must cancel all outstanding async operations
  before the `io_context` is destroyed.
- Run shutdown path under ASAN on every CI run to catch lifetime violations.

**Detection:** Run the full test suite with ASAN enabled. Any `heap-use-after-free` in a
destructor or timer callback at shutdown indicates a coroutine outlived its executor.

**Phase:** Graceful shutdown implementation.

---

## Moderate Pitfalls

---

### Pitfall 7: Raw PeerInfo* Pointer Dangling After co_await in Rate Limit Path

**What goes wrong:** Rate limiting will likely add a time-based backpressure path (e.g., delay
processing a message from an abusive peer using `co_await asio::steady_timer`). The natural
implementation retrieves `PeerInfo* peer = find_peer(conn)` before the `co_await`, then uses
`peer->strike_count` after resuming. During the `co_await` delay, the peer disconnects and
`on_peer_disconnected()` erases its entry from `peers_` (the deque). The `PeerInfo*` is now
dangling. Accessing `peer->strike_count` after resumption is use-after-free.

**Why it happens:** The existing `peers_` is a `std::deque<PeerInfo>`. A deque does NOT
invalidate pointers/references on push_back, but DOES invalidate them on erase (from either end
or middle). `on_peer_disconnected()` erases the peer's entry, invalidating any raw pointer into
the deque.

**Prevention:**
- Do NOT store raw `PeerInfo*` across any `co_await`. Re-lookup via `find_peer(conn)` after
  every suspension and handle `nullptr` (peer disconnected during delay).
- Strongly prefer disconnecting abusive peers immediately (adding a strike) over introducing
  backpressure `co_await` delays. The existing strike system already handles this: exceed
  `STRIKE_THRESHOLD` → disconnect. Extend it for rate limit violations.
- If a delay is truly required, check `stopping_` and re-run `find_peer()` after every
  timer expiry before touching any `PeerInfo` fields.

**Phase:** Rate limiting implementation.

---

### Pitfall 8: Tombstone Index Write Is Not Atomic With the Tombstone Blob Write

**What goes wrong:** The tombstone index fix converts `has_tombstone_for()` from an O(n) scan
to an O(1) lookup using a new libmdbx sub-database keyed by `[namespace:32][target_hash:32]`.
If the tombstone blob is written in one libmdbx write transaction and the index entry is written
in a separate transaction, a crash between the two commits leaves them out of sync: the blob
exists but the index misses it, or vice versa. A missing index entry causes `has_tombstone_for()`
to return false, allowing a deleted blob to be re-ingested during sync — violating the "tombstones
are permanent" invariant.

**Why it happens:** libmdbx is ACID per-transaction, not across transactions. The existing
`delegation_index` in v3.0 correctly writes both the delegation blob and its index entry in a
single transaction — copy that pattern exactly.

**Consequences:** Deleted blobs can be re-ingested if the index is incomplete. The
"tombstones permanent" guarantee is violated.

**Prevention:**
- Write both the tombstone blob entry in `blobs_map` AND the tombstone index entry in a single
  libmdbx write transaction. One commit, both present or neither.
- Add a startup integrity check: scan all tombstone blobs and verify each has an index entry,
  rebuilding missing entries if found. This makes the system self-healing after any corrupt state.
- Add a Catch2 test that inserts a tombstone and verifies `has_tombstone_for()` returns true in
  O(1) (call count must be one btree lookup, not a namespace scan).

**Phase:** Tombstone index optimization.

---

### Pitfall 9: Namespace-Scoped Sync Applies Filter at Phase C (Blob Transfer) Instead of Phase A (Namespace List)

**What goes wrong:** The sync protocol phases are: A) exchange namespace lists, B) exchange hash
lists per namespace, C) transfer missing blobs. If namespace-scoped sync is implemented by
filtering blob requests at Phase C but sending the full namespace list at Phase A, the peer sees
ALL namespace IDs the local node holds — even those it should not sync. This leaks namespace
existence to unauthorized peers. On closed nodes, namespace IDs are sensitive (they are hashes
of owner pubkeys).

**Why it happens:** The natural implementation reflex is to add a filter at the point where
blobs are sent (Phase C), because that is where data is transferred. But information about which
namespaces exist is already shared in Phase A.

**Prevention:**
- Filter the namespace list BEFORE encoding in Phase A. `SyncProtocol::encode_namespace_list()`
  takes a `std::vector<storage::NamespaceInfo>` — filter this vector before calling encode.
- The per-connection namespace allowlist (which namespaces this peer syncs) must be established
  before Phase A begins. Encode it in `PeerInfo` as a `std::set<std::array<uint8_t, 32>>` (the
  same pattern as `subscribed_namespaces` for pub/sub).
- Store the sync namespace filter exclusively in `PeerInfo`. On disconnect, cleanup is automatic
  when the `PeerInfo` entry is erased. Do NOT introduce a separate map keyed by connection
  pointer — it will leak.

**Phase:** Namespace-scoped sync implementation.

---

### Pitfall 10: Metrics Counters Use std::atomic on the Single-Threaded io_context Path

**What goes wrong:** The temptation is to make all metrics counters `std::atomic<uint64_t>` for
"thread safety." But the entire `PeerManager` and `BlobEngine` ingest path runs on a single
`io_context` thread. `std::atomic` with `memory_order_seq_cst` (the default on x86) inserts
full memory fences (`MFENCE` / `LOCK` prefix). On the hot blob ingest path (executed for every
incoming blob from every peer), this adds unnecessary CPU overhead and false cache-line sharing
between adjacent counters.

**Why it happens:** Cargo-culting "shared state needs atomics" into a single-threaded async
context where the shared state is in fact never accessed from multiple threads simultaneously.

**Prevention:**
- Use plain `uint64_t` for all counters updated exclusively on the `io_context` thread:
  connection count, bytes stored, blobs ingested, sync rounds, strikes issued.
- Only use `std::atomic` if the counter is read from a genuinely different thread (e.g., a
  dedicated HTTP metrics endpoint thread running on a separate thread pool).
- If multiple counters are in the same struct, use `alignas(64)` to prevent false sharing
  between hot-path counters and cold-path ones — but only after profiling confirms the overhead.
- Recommended: expose metrics via a periodic log line on a timer (same io_context thread) or
  write to a stats file. Both access counters on the io_context thread — no atomics needed.

**Phase:** Metrics/observability implementation.

---

### Pitfall 11: Persistent Peer File Written on Every Disconnect Causes Disk I/O Spike

**What goes wrong:** If `save_persisted_peers()` is called inside `on_peer_disconnected()`, a
network partition that causes 32 peers to disconnect in rapid succession triggers 32 sequential
JSON serializations + fsync calls. Each fsync on a busy server takes 10–100 ms. The `io_context`
is blocked during each fsync (it is a synchronous call inside a non-coroutine method), stalling
all other async operations for seconds.

**Why it happens:** Correctness-first instinct: write immediately on any change so the file is
always fresh. This is correct for safety but wrong for frequency.

**Prevention:**
- Write `peers.json` on a periodic flush timer (e.g., every 30 seconds), not on every
  disconnect. Loss of 30 seconds of peer updates across an unclean exit is acceptable — the peer
  list is a hint for reconnect speed, not safety-critical data.
- Always write on clean shutdown (the SIGTERM path in graceful shutdown) for maximum freshness.
- The periodic timer and the shutdown path both use the same `save_persisted_peers()` function
  with the atomic write pattern (Pitfall 4).
- Do NOT call `fsync` inside `on_peer_disconnected()`. That callback runs on the io_context
  thread and blocks it for the duration of the system call.

**Phase:** Persistent peer list implementation.

---

### Pitfall 12: Rate Limiter Blocks the io_context via Synchronous Sleep

**What goes wrong:** A rate limiter that calls `std::this_thread::sleep_for()` inside a message
handler or coroutine to enforce backpressure blocks the entire `io_context` thread. During the
sleep, no other peer's messages are processed, no sync timers fire, no heartbeats run. The rate
limiter intended to protect the node from abuse instead makes it unresponsive to all peers.

**Why it happens:** Translating a synchronous rate limiter design (token bucket with sleep) into
async code without converting the sleep to `co_await asio::steady_timer`.

**Consequences:** Complete io_context stall for the duration of the sleep. All connected peers
experience a "dead" node. This is effectively a self-inflicted DoS.

**Prevention:**
- Never call any blocking `sleep_*` function in the io_context thread context.
- Rate-limit by disconnecting abusive peers using the existing strike system, not by sleeping.
  Adding a strike and disconnecting is instantaneous (no suspension).
- If soft rate limiting is needed (throttle writes without disconnect), use a token bucket
  whose refill and block both use `co_await asio::steady_timer`. But note that this introduces
  the dangling `PeerInfo*` risk described in Pitfall 7.
- Simplest correct implementation: bytes-per-second counter per `PeerInfo`, reset on a
  periodic timer. If a peer exceeds the limit, add a strike. After `STRIKE_THRESHOLD` strikes,
  disconnect. No sleeping needed.

**Phase:** Rate limiting implementation.

---

## Minor Pitfalls

---

### Pitfall 13: Disk-Full Protocol Rejection Uses Generic Error Message Type

**What goes wrong:** When the node rejects an ingest due to storage limit, it must send a
protocol-level signal the peer can act on. If this reuses a generic `Error` frame (or sends no
response), the peer cannot distinguish "node is full — stop sending here" from "your blob is
invalid — fix it." A peer that treats the response as a transient error will retry indefinitely,
wasting both its bandwidth and the full node's CPU.

**Prevention:**
- Define `TransportMsgType::StorageFull` as a distinct wire message type.
- On receipt of `StorageFull`, peers should back off writes to that node and not retry the same
  blob until the node signals availability again (or until a configurable backoff expires).
- Add this to the FlatBuffers transport schema in a backward-compatible way (new enum value).

**Phase:** Disk-full reporting implementation.

---

### Pitfall 14: Tombstone Index Breaks the Existing Expiry Path

**What goes wrong:** `run_expiry_scan()` currently deletes blobs from `blobs_map` and
`expiry_map`, leaving `seq_map` intact (gaps are expected). With a tombstone index sub-database,
if an expired tombstone blob is deleted from `blobs_map` by the expiry scanner, but its index
entry in the tombstone sub-database is NOT deleted in the same transaction, the index points to
a blob that no longer exists. `has_tombstone_for()` would then return false (finding the index
entry is not enough — it must verify the target blob still exists), or worse, it would continue
blocking future ingests of the target blob hash even though the tombstone itself was expired.

**Prevention:**
- Tombstones have `TTL=0` (permanent). The expiry scanner must NOT delete them. Verify
  `run_expiry_scan()` skips blobs with `TTL=0` (check the existing expiry path in storage.cpp).
- If tombstones can be expired (a future design change), the expiry scanner must also delete
  the tombstone index entry in the same transaction as the blob deletion.
- Write a test that verifies tombstone blobs survive an expiry scan run.

**Phase:** Tombstone index optimization.

---

### Pitfall 15: Metrics Overflow From Undersized Counter Types

**What goes wrong:** Using `uint32_t` for cumulative counters (total blobs ingested, total bytes
written) overflows after ~4 billion blobs or ~4 GiB respectively. A busy node hitting 4 billion
blob ingests wraps the counter silently, producing nonsensical metrics.

**Prevention:**
- Use `uint64_t` for ALL cumulative counters in the metrics system. `uint32_t` is acceptable
  for per-sync-round stats that reset each cycle (e.g., `SyncStats::blobs_sent`).
- Never use `uint32_t` for an always-incrementing counter unless the wrap behavior is documented
  and intentional.

**Phase:** Metrics implementation.

---

### Pitfall 16: Version Bump in version.h Breaks Tests That Assert Version String

**What goes wrong:** Bumping `version.h` to `0.4.0` breaks any Catch2 test that asserts the
version string or checks the version header value. Minor disruption, but it creates CI noise
that obscures real failures if done at the wrong time.

**Prevention:** Update `version.h` as the final step after all feature phases pass. Before
bumping, search for version string assertions: `grep -r "0.3.0\|version" tests/`.

**Phase:** Version bump (final phase).

---

## Phase-Specific Warnings

| Phase | Pitfall | Severity | Mitigation |
|---|---|---|---|
| Storage limits | MDBX_MAP_FULL before logical quota (Pitfall 1) | Critical | mapsize upper = 2x logical limit; distinct StorageFull status |
| Storage limits | Stale limit check after co_await (Pitfall 2) | Critical | Check inside BlobEngine::ingest(), not in coroutine body |
| Storage limits | Counter drift from missed decrement paths (Pitfall 3) | Critical | Single adjust_used_bytes() method; startup recompute; invariant test |
| Disk-full reporting | Generic error indistinguishable from invalid blob (Pitfall 13) | Moderate | New TransportMsgType::StorageFull wire message |
| Persistent peer list | Crash during write corrupts peers.json (Pitfall 4) | Critical | write-to-temp + fsync + rename; directory fsync required |
| Persistent peer list | fsync on every disconnect stalls io_context (Pitfall 11) | Moderate | Periodic timer flush; flush on clean shutdown only |
| Graceful shutdown | Stuck sync blocks exit for 120s per peer (Pitfall 5) | Critical | Cancel all sync_notify timers on stop(); check stopping_ in recv_sync_msg() |
| Graceful shutdown | io_context destroyed with live coroutine frames (Pitfall 6) | Critical | Cancel timers + server stop; let io_context drain naturally |
| Metrics | Atomic overhead on single-threaded hot path (Pitfall 10) | Moderate | Plain uint64_t on io_context thread; atomic only for cross-thread reads |
| Metrics | uint32_t overflow on cumulative counters (Pitfall 15) | Minor | uint64_t for all cumulative metrics |
| Rate limiting | Raw PeerInfo* dangling after co_await delay (Pitfall 7) | Critical | Re-lookup find_peer() after every suspension; prefer strike+disconnect |
| Rate limiting | Blocking sleep stalls io_context (Pitfall 12) | Critical | co_await steady_timer or strike+disconnect only |
| Tombstone index | Non-atomic index + blob write (Pitfall 8) | Critical | Single transaction for both; copy delegation_index pattern |
| Tombstone index | Expiry scanner deletes tombstone blob leaving orphan index (Pitfall 14) | Moderate | Verify TTL=0 blobs are skipped by expiry scanner |
| Namespace-scoped sync | Filter at Phase C leaks namespace IDs (Pitfall 9) | Moderate | Filter namespace list before Phase A encode |
| Namespace-scoped sync | Filter state in separate map leaks on disconnect (Pitfall 9) | Minor | Store sync filter in PeerInfo only |
| Version bump | Version assertion failures in tests (Pitfall 16) | Minor | Bump as final step after all tests pass |

---

## Sources

- libmdbx MDBX_MAP_FULL issue: [erthink/libmdbx#136](https://github.com/erthink/libmdbx/issues/136)
- Erigon MDBX_MAP_FULL in production: [ledgerwatch/erigon#7666](https://github.com/ledgerwatch/erigon/issues/7666)
- Crash-safe atomic file write (LWN): [lwn.net/Articles/457667](https://lwn.net/Articles/457667/)
- Asio coroutine cancellation object lifetime: [chriskohlhoff/asio#1574](https://github.com/chriskohlhoff/asio/issues/1574)
- Asio graceful shutdown task_group pattern: [cppalliance.org Q2 2024](https://cppalliance.org/mohammad/2024/07/10/MohammadsQ2Update.html)
- False sharing on atomic counters: [travisdowns.github.io concurrency costs](https://travisdowns.github.io/blog/2020/07/06/concurrency-costs.html)
- The concurrency trap (atomic counter stalling a pipeline): [redixhumayun.github.io](https://redixhumayun.github.io/concurrency/2025/06/05/the-concurrency-trap-how-an-atomic-counter-stalled-a-pipeline.html)
- Tendermint dirty peer state on disconnect: [tendermint/tendermint#3304](https://github.com/tendermint/tendermint/issues/3304)
- chromatindb source: `db/peer/peer_manager.h`, `db/storage/storage.h`, `db/engine/engine.h`, `db/sync/sync_protocol.h`
- chromatindb RETROSPECTIVE.md: `.planning/RETROSPECTIVE.md`

---
*Pitfalls research for: chromatindb v0.4.0 — production readiness features*
*Researched: 2026-03-08*
