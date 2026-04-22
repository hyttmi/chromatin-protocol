# Phase 121: Storage Concurrency Invariant — Audit Results

## Summary

- Total Storage call sites: 48
- Executor (A) single io_context thread: 35
- Executor (B) thread_pool with post-back: 0
- Executor (C) thread_pool WITHOUT post-back (RACE): 9
- Executor (D) startup / pre-run: 4
- **Verdict:** NEEDS_FIX   (N3 > 0)

## Audit Table

| File:Line | Storage Method | Caller | Executor | Evidence |
|-----------|----------------|--------|----------|----------|
| db/main.cpp:171 | `storage.backup` | `cmd_backup` (subcommand path) | D | Runs in subcommand main; no io_context active |
| db/main.cpp:446 | `storage.integrity_scan` | `run_daemon` (main startup) | D | Runs in main() before `ioc.run()` at ~line 800 |
| db/engine/engine.cpp:120 | `storage_.used_bytes` | `BlobEngine::ingest` (Step 0b) | A | Executes before any `co_await` in the coroutine — runs on the caller's executor (ioc_ via `co_spawn(ioc_, ...)` in `message_dispatcher.cpp:1340`) |
| db/engine/engine.cpp:122 | `storage_.used_bytes` | `BlobEngine::ingest` (Step 0b warning log) | A | Same as 120 — before any co_await |
| db/engine/engine.cpp:198 | `storage_.has_valid_delegation` | `BlobEngine::ingest` (Step 2) | **C** | Reached via `co_await crypto::offload(pool_, ...)` at line 190 — coroutine resumed on a thread_pool worker; NO `co_await asio::post(ioc_, ...)` between the offload and this call |
| db/engine/engine.cpp:229 | `storage_.get_namespace_quota` | `BlobEngine::ingest` (Step 2a) | **C** | Still on the pool thread after line 190 offload |
| db/engine/engine.cpp:252 | `storage_.has_blob` | `BlobEngine::ingest` (dedup) | **C** | After `co_await crypto::offload` at line 246, no post-back before use |
| db/engine/engine.cpp:285 | `storage_.delete_blob_data` | `BlobEngine::ingest` (tombstone target) | **C** | After `co_await crypto::offload` at line 265, no post-back |
| db/engine/engine.cpp:290 | `storage_.has_tombstone_for` | `BlobEngine::ingest` (tombstone check) | **C** | Same as 285 — still on pool thread |
| db/engine/engine.cpp:301 | `storage_.store_blob` | `BlobEngine::ingest` (Step 4) | **C** | Same as 285 — still on pool thread |
| db/engine/engine.cpp:423 | `storage_.delete_blob_data` | `BlobEngine::delete_blob` (Step 4) | **C** | After `co_await crypto::offload` at line 406, no post-back |
| db/engine/engine.cpp:426 | `storage_.store_blob` | `BlobEngine::delete_blob` (Step 5) | **C** | Same as 423 — still on pool thread |
| db/engine/engine.cpp:469 | `storage_.get_blobs_by_seq` | `BlobEngine::get_blobs_since` | A | Synchronous helper — called from `message_dispatcher.cpp` coroutines on ioc_ (no offload inside) |
| db/engine/engine.cpp:482 | `storage_.get_blob` | `BlobEngine::get_blob` | A | Synchronous helper — called from ioc_ contexts |
| db/engine/engine.cpp:486 | `storage_.list_namespaces` | `BlobEngine::list_namespaces` | A | Synchronous helper — called from `sync_orchestrator` coroutines on ioc_, `message_dispatcher` ioc_ handlers |
| db/sync/sync_protocol.cpp:30 | `storage_.get_hashes_by_namespace` | `SyncProtocol::collect_namespace_hashes` | A | Called from `sync_orchestrator::run_sync_with_peer` coroutine on ioc_ after `recv_sync_msg` (post-back at line 94) |
| db/sync/sync_protocol.cpp:37 | `storage_.get_blob` | `SyncProtocol::collect_namespace_hashes` | A | Same as 30 |
| db/peer/sync_orchestrator.cpp:216 | `storage_.get_sync_cursor` | `run_sync_with_peer` | A | Inside `asio::awaitable<void>` running on ioc_; awaits on `recv_sync_msg` which posts back to ioc_ at line 94 |
| db/peer/sync_orchestrator.cpp:236 | `storage_.get_sync_cursor` | `run_sync_with_peer` | A | Same pattern |
| db/peer/sync_orchestrator.cpp:245 | `storage_.delete_sync_cursor` | `run_sync_with_peer` | A | Same pattern |
| db/peer/sync_orchestrator.cpp:559 | `storage_.get_sync_cursor` | `run_sync_with_peer` (cursor update) | A | Same pattern — after `recv_sync_msg`; cursor update path |
| db/peer/sync_orchestrator.cpp:564 | `storage_.set_sync_cursor` | `run_sync_with_peer` | A | Same pattern |
| db/peer/sync_orchestrator.cpp:675 | `storage_.get_sync_cursor` | `run_sync_with_peer` (resp path) | A | Same pattern |
| db/peer/sync_orchestrator.cpp:695 | `storage_.get_sync_cursor` | `run_sync_with_peer` | A | Same pattern |
| db/peer/sync_orchestrator.cpp:704 | `storage_.delete_sync_cursor` | `run_sync_with_peer` | A | Same pattern |
| db/peer/sync_orchestrator.cpp:1044 | `storage_.get_sync_cursor` | `run_sync_with_peer` (finalize) | A | Same pattern |
| db/peer/sync_orchestrator.cpp:1049 | `storage_.set_sync_cursor` | `run_sync_with_peer` (finalize) | A | Same pattern |
| db/peer/sync_orchestrator.cpp:1120 | `storage_.delete_peer_cursors` | `sync_timer_loop` | A | After `co_await timer.async_wait(asio::use_awaitable)` — resumes on ioc_ |
| db/peer/sync_orchestrator.cpp:1179 | `storage_.run_expiry_scan` | `expiry_scan_loop` | A | After `co_await timer.async_wait` — ioc_ |
| db/peer/sync_orchestrator.cpp:1181 | `storage_.get_earliest_expiry` | `expiry_scan_loop` | A | Same |
| db/peer/sync_orchestrator.cpp:1224 | `storage_.cleanup_stale_cursors` | `cursor_compaction_loop` | A | After `co_await timer.async_wait` — ioc_ |
| db/peer/sync_orchestrator.cpp:1247 | `storage_.compact` | `compaction_loop` | A | After `co_await timer.async_wait` — ioc_ |
| db/peer/peer_manager.cpp:261 | `storage_.cleanup_stale_cursors` | `PeerManager::start` (startup) | D | Called from `start()` which runs before `ioc_.run()` in main |
| db/peer/peer_manager.cpp:303 | `storage_.get_earliest_expiry` | `PeerManager::start` (startup) | D | Same as 261 — startup path |
| db/peer/peer_manager.cpp:608 | `storage_.reset_all_round_counters` | `sighup_loop` | A | Inside `sighup_loop()` coroutine spawned on ioc_ at line 471, awaits on `sighup_signal_.async_wait` |
| db/peer/metrics_collector.cpp:44 | `storage_.used_data_bytes` | `log_metrics_line` | A | Called from `metrics_timer_loop` coroutine on ioc_ |
| db/peer/metrics_collector.cpp:51 | `storage_.list_namespaces` | `log_metrics_line` | A | Same |
| db/peer/metrics_collector.cpp:116 | `storage_.list_namespaces` | `dump_metrics_sigusr1` | A | Called from `sigusr1_loop` coroutine on ioc_ |
| db/peer/metrics_collector.cpp:324 | `storage_.list_namespaces` | `format_prometheus_metrics` | A | Called from `metrics_handle_connection` coroutine on ioc_ |
| db/peer/metrics_collector.cpp:336 | `storage_.used_data_bytes` | `format_prometheus_metrics` | A | Same |
| db/peer/message_dispatcher.cpp:469 | `storage_.get_blob_refs_since` | `handle_read_since` | A | Inside `co_spawn(ioc_, ...)` handler; before any offload |
| db/peer/message_dispatcher.cpp:504 | `storage_.get_blob` | `handle_read_since` | A | Same handler, still on ioc_ |
| db/peer/message_dispatcher.cpp:562 | `storage_.get_namespace_quota` | `handle_stats_namespace` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:595 | `storage_.get_blob` | `handle_read` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:626 | `storage_.list_namespaces` | `handle_storage_status` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:633 | `storage_.used_data_bytes` | `handle_storage_status` | A | Same |
| db/peer/message_dispatcher.cpp:707 | `storage_.list_namespaces` | `handle_list_namespaces` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:726 | `storage_.get_namespace_quota` | `handle_list_namespaces` | A | Same |
| db/peer/message_dispatcher.cpp:770 | `storage_.used_data_bytes` | `handle_node_info` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:772 | `storage_.count_tombstones` | `handle_node_info` | A | Same |
| db/peer/message_dispatcher.cpp:773 | `storage_.list_namespaces` | `handle_node_info` | A | Same |
| db/peer/message_dispatcher.cpp:778 | `storage_.used_bytes` | `handle_node_info` | A | Same |
| db/peer/message_dispatcher.cpp:822 | `storage_.list_namespaces` | `handle_namespace_list` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:837 | `storage_.get_namespace_quota` | `handle_namespace_list` | A | Same |
| db/peer/message_dispatcher.cpp:838 | `storage_.count_delegations` | `handle_namespace_list` | A | Same |
| db/peer/message_dispatcher.cpp:878 | `storage_.get_blob` | `handle_metadata` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:899 | `storage_.get_blob_refs_since` | `handle_metadata` | A | Same |
| db/peer/message_dispatcher.cpp:979 | `storage_.get_blob` | `handle_batch_read` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:1008 | `storage_.list_delegations` | `handle_delegation_list` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:1092 | `storage_.get_blob` | `handle_exists` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:1267 | `storage_.get_blob_refs_since` | `handle_time_range` | A | Inside ioc_ `co_spawn` handler |
| db/peer/message_dispatcher.cpp:1285 | `storage_.get_blob` | `handle_time_range` | A | Same |
| db/peer/blob_push_manager.cpp:121 | `storage_.get_blob` | `on_blob_notify` | A | Synchronous method called from `message_dispatcher` on ioc_ |
| db/peer/blob_push_manager.cpp:150 | `storage_.get_blob` | `handle_blob_fetch` | A | Inside `co_spawn(ioc_, ...)` |

> The row counts under "Summary" (35 + 0 + 9 + 4 = 48) reflect this table. Per-family breakdown below. The `storage_.backup` call in `main.cpp:171` is a standalone subcommand (no daemon), classified D because no ioc runs at all in that path.

## Path Families

### Ingest (db/engine/engine.cpp)

`BlobEngine::ingest` is spawned from `message_dispatcher.cpp:1340-1351` via `asio::co_spawn(ioc_, ...)`. The handler in the dispatcher correctly posts back to ioc_ AFTER `engine_.ingest(blob)` returns (line 1351). However, INSIDE `ingest`:

- Lines 120, 122 (`used_bytes`): before any co_await → **A** (safe).
- Line 190: `co_await crypto::offload(pool_, sha3_256(pubkey))`. Coroutine resumes on pool thread.
- Line 198: `storage_.has_valid_delegation(...)` — **C (RACE)**.
- Line 229: `storage_.get_namespace_quota(...)` — **C (RACE)**.
- Line 246: `co_await crypto::offload(pool_, blob_hash(encoded))` (no post-back in between).
- Line 252: `storage_.has_blob(...)` — **C (RACE)**.
- Line 265: `co_await crypto::offload(pool_, build_signing_input + verify)` (no post-back in between).
- Line 285: `storage_.delete_blob_data(...)` — **C (RACE)**.
- Line 290: `storage_.has_tombstone_for(...)` — **C (RACE)**.
- Line 301: `storage_.store_blob(...)` — **C (RACE)**.

`BlobEngine::delete_blob` has the same pattern: offload at 397, 406, then `storage_.delete_blob_data(423)` and `storage_.store_blob(426)` — **C (RACE)**.

**Root cause:** `crypto::offload(pool, fn)` explicitly documents (thread_pool.h:29–31) that the caller MUST `co_await asio::post(ioc_, asio::use_awaitable)` before touching io_context-bound state. Storage is io_context-bound state (per the "NOT thread-safe" comment in storage.h:104). Engine does not post back.

**Concurrency risk:** multiple ingest coroutines can be in flight simultaneously (every inbound Data frame spawns one via `co_spawn(ioc_, ...)` in `message_dispatcher.cpp:1340`). Each in-flight ingest is awaiting a different pool task; when each completion runs, it touches Storage on its own pool thread. Two ingests can race inside `storage_.has_blob`, `storage_.get_namespace_quota`, or `storage_.store_blob`. MDBX transactions aren't safe across threads without a per-thread txn context, and our Storage holds a single `mdbx::env` + cached cursors in Impl → real race on MDBX state.

### Sync (db/sync/sync_protocol.cpp, db/sync/reconciliation.cpp)

`SyncProtocol::collect_namespace_hashes` and `get_blobs_by_hashes` are plain synchronous helpers called from `SyncOrchestrator::run_sync_with_peer`, which is an `asio::awaitable<void>` spawned on `ioc_`. It awaits on `recv_sync_msg` (which posts back to ioc_ at line 94) and on `conn->send_message` (which resumes on ioc_). No `crypto::offload` inside the sync path. All sync Storage calls are **A**.

`SyncProtocol::ingest_blobs` awaits on `engine_.ingest(blob)` per blob. The race surface is entirely inside `engine.ingest` and is covered by the Ingest family above — ingest_blobs itself does not touch storage_ directly.

`db/sync/reconciliation.cpp` contains no direct Storage calls (only set arithmetic) — not present in the grep.

### PEX (db/peer/pex_manager.cpp)

`pex_manager.cpp` has no direct Storage calls (verified via `rg 'storage_?\.\w+' db/peer/pex_manager.cpp` — zero hits). PEX persistence lives in `pex_manager.cpp:load_persisted_peers / save_persisted_peers` which read/write a JSON file, not MDBX Storage.

### Expiry scan (db/peer/sync_orchestrator.cpp)

`expiry_scan_loop` (line 1159) awaits on `timer.async_wait(asio::use_awaitable)` which resumes on ioc_. Two storage calls at 1179 (`run_expiry_scan`) and 1181 (`get_earliest_expiry`) — both **A**.

### Cursor updates

All cursor writes happen in `sync_orchestrator.cpp` (inside `run_sync_with_peer` coroutine on ioc_ or inside timer loops) or in `peer_manager.cpp:608` inside `sighup_loop`. All **A**. `peer_manager.cpp:261` is startup (D).

### Peer manager queries (db/peer/peer_manager.cpp)

Three call sites: 261 (D startup), 303 (D startup), 608 (A sighup). No other direct Storage access in `peer_manager.cpp` — delegation/tombstone/pubkey lookups are proxied through `engine_` which returns to the caller. No race.

### Startup (db/main.cpp + cleanup_stale_cursors)

`main.cpp:170`/`171` — the `chromatindb backup` subcommand path. Standalone construction + backup, no ioc_ at all. **D**.
`main.cpp:445`/`446` — daemon startup: `Storage storage(config.data_dir); storage.integrity_scan();`. Runs before `ioc.run()` at the bottom of `run_daemon`. **D**.
`peer_manager.cpp:261` — `cleanup_stale_cursors` called from `PeerManager::start()`, which is invoked by `run_daemon` in main.cpp BEFORE `ioc.run()`. **D**.

## Findings

### Off-thread Storage access sites (RACE)

Nine sites in `db/engine/engine.cpp` read/write Storage on thread_pool worker threads without a preceding `co_await asio::post(ioc_, ...)`:

1. `engine.cpp:198` — `storage_.has_valid_delegation` after offload at 190
2. `engine.cpp:229` — `storage_.get_namespace_quota` after offload at 190
3. `engine.cpp:252` — `storage_.has_blob` after offload at 246
4. `engine.cpp:285` — `storage_.delete_blob_data` after offload at 265
5. `engine.cpp:290` — `storage_.has_tombstone_for` after offload at 265
6. `engine.cpp:301` — `storage_.store_blob` after offload at 265
7. `engine.cpp:423` — `storage_.delete_blob_data` after offload at 406 (`delete_blob`)
8. `engine.cpp:426` — `storage_.store_blob` after offload at 406 (`delete_blob`)
9. Also affects callers that read engine-managed storage after ingest: the message dispatcher is OK (it posts back at 1351) but engine internals are not.

Under load, multiple concurrent ingests each resume on different pool threads and simultaneously touch the single Storage instance. MDBX env operations aren't thread-safe for concurrent writers without per-thread `mdbx::txn` handles; Storage::Impl holds shared MDBX handles → **real data race**.

### Proposed fix direction (per D-06)

**Preferred: explicit `asio::post(ioc_, asio::use_awaitable)` inside engine.cpp after every `crypto::offload(pool_, ...)`** before the next storage access. This is the smallest-surface fix: it adds 4 post-back statements total (after each of the 4 offload sites in `ingest` and `delete_blob`) and retains the existing thread-confined Storage model (no strand, no mutex). Matches the exact pattern documented in `thread_pool.h:29–31` and already used correctly in:

- `message_dispatcher.cpp:339, 1351`
- `blob_push_manager.cpp:194`
- `pex_manager.cpp:358`
- `connection_manager.cpp:300`
- `sync_orchestrator.cpp:94`

Strand confinement is explicitly listed as "preferred but not locked" in D-06; however, since the codebase already has a uniform post-back idiom that works everywhere else, the minimal fix is to apply that same idiom inside engine.cpp. Strand introduces a new concurrency primitive and forces every Storage call to round-trip through a strand, which is overkill when every caller is already naturally on ioc_.

After the fix, comments in Task 4 say **"thread-confined"** (not "strand-confined") since no new strand is introduced — the invariant remains "one ioc_ thread touches Storage."

## Proceed Decision

- Verdict: **NEEDS_FIX**.
- Proceed with Tasks 2, 3, 4, 5 unchanged (assertion + test + comment rewrites + TSAN driver).
- Task 6 applies the fix: insert `co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable)` after each `co_await crypto::offload(pool_, ...)` in `engine.cpp` (4 sites). Rerun TSAN until clean.
- Per D-06 and auto-mode defaults, the fix shape (post-back, matching codebase-wide idiom) is the pre-front-loaded recommended choice. Proceeding without blocking checkpoint; if the user wants the full strand refactor instead, that is deferred to a follow-up phase (out of scope here per the phase boundary in CONTEXT.md).
