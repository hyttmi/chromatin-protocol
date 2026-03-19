# Domain Pitfalls: v0.9.0 Connection Resilience & Hardening

**Domain:** Connection lifecycle, crash recovery, logging, config validation for chromatindb (C++20 async daemon)
**Researched:** 2026-03-19
**Stack context:** Standalone Asio (C++20 coroutines, single io_context thread + thread_pool), libmdbx (MMAP/ACID), spdlog (logging), nlohmann/json (config)

## Critical Pitfalls

Mistakes that cause crashes, data corruption, or production outages.

### Pitfall 1: Reconnect Storm After Network Partition

**What goes wrong:** Network partition heals. All N peers reconnect simultaneously. Each reconnect triggers a full sync. N^2 sync messages flood the network. CPU spikes from ML-DSA-87 verification. Timeouts cascade. Peers disconnect again. Cycle repeats.

**Why it happens:** Fixed reconnect delay without jitter. All peers use the same backoff schedule, so they all wake up at the same time.

**Consequences:** Sustained network instability. Sync rate limiter (v0.8.0) mitigates but doesn't prevent the initial burst. CPU-bound crypto verification blocks the io_context.

**Prevention:**
- Add random jitter (0-25% of delay) to exponential backoff.
- Stagger initial reconnect: if daemon starts with 10 bootstrap peers, don't connect to all simultaneously. Add 0-2s random delay per peer on startup.
- Sync rate limiter (already exists) throttles sync after connect, but connection establishment itself needs staggering.

**Detection:** Log "connected to N peers within 1 second" warnings. Monitor sync rejection rate.

### Pitfall 2: Coroutine Lifetime vs Timer Lifetime

**What goes wrong:** A `steady_timer` is allocated on the coroutine stack. The coroutine is cancelled (e.g., shutdown). The timer's completion handler runs after the coroutine frame is destroyed. Stack-use-after-return.

**Why it happens:** `asio::co_spawn(ioc, coroutine, asio::detached)` -- the detached token means nobody holds the coroutine alive. If the io_context stops while the coroutine is suspended on the timer, the timer is cancelled, but the cancellation handler may reference dead stack variables.

**Consequences:** ASAN: stack-use-after-return. UBSAN: undefined behavior. In production: intermittent crash on shutdown.

**Prevention:**
- Use `asio::as_tuple(asio::use_awaitable)` for timer waits. Check the error_code for `operation_aborted`. Do NOT access any local variables after getting `operation_aborted` -- just `co_return`.
- Cancel all timers in `on_shutdown()` before calling `ioc.stop()`.
- The existing SIGHUP coroutine uses a member function (not lambda) specifically to avoid this. Follow the same pattern for reconnect and keepalive coroutines.

**Detection:** ASAN with detect_stack_use_after_return=1. TSAN for race conditions. Both are v1.0.0 scope but should be kept in mind.

### Pitfall 3: libmdbx Stale Reader Slots After Crash

**What goes wrong:** Process crashes (kill -9) while a read transaction is open. libmdbx reserves a reader slot in the shared lock table. On restart, the stale reader slot blocks write transactions from reclaiming pages. Database grows without bound.

**Why it happens:** libmdbx uses a shared-memory lock file (`.mdbx-lck`) to track active readers. A crashed process leaves its reader slot marked as active.

**Consequences:** Database file grows. Eventually hits disk limits. Write performance degrades as B-tree depth increases.

**Prevention:**
- libmdbx automatically clears stale reader slots on `mdbx_env_open()` for single-process usage. The reader table uses PIDs, and dead PIDs are detected.
- Verify this works correctly in the crash recovery audit: kill -9 during read transaction, restart, confirm no stale readers.
- If using multi-process access (we don't): need `mdbx_reader_check()`. Not applicable to chromatindb (single process).

**Detection:** `mdbx_env_info()` reports reader count. Log it at startup. If reader count > 0 on fresh start, stale readers exist.

### Pitfall 4: File Logging Creates Unwritable Path

**What goes wrong:** Config specifies `"log_file": "/var/log/chromatindb/node.log"`. Directory doesn't exist. spdlog throws at initialization. Daemon crashes before any log output. Operator sees no error message.

**Why it happens:** `rotating_file_sink_mt` constructor throws `spdlog::spdlog_ex` if it cannot create/open the file.

**Consequences:** Silent startup failure. No logs to debug why.

**Prevention:**
- In `validate_config()`: check that the parent directory of `log_file` exists (or can be created). `std::filesystem::create_directories()` for the parent path.
- Catch `spdlog_ex` during `logging::init()`. If file sink fails, fall back to console-only and log a warning to stderr.
- Never let file sink failure be fatal. Console logging must always work.

**Detection:** Startup log message: "File logging enabled: /path/to/file.log" or "File logging failed, using console only: [reason]".

### Pitfall 5: ACL-Rejected Peer Reconnect Infinite Loop

**What goes wrong:** Peer A is in our `bootstrap_peers`. Remote peer B has us in `allowed_keys` initially. B removes our key. A reconnects, handshake succeeds (TCP + PQ crypto), but B immediately closes the connection (ACL gating). A backs off, reconnects, gets closed again. Forever.

**Why it happens:** ACL rejection is indistinguishable from a network error at the TCP level -- the remote just closes the connection. The reconnect loop doesn't know the difference between "network flap" and "permanently rejected".

**Consequences:** Wasted resources. Log spam. The peer will never accept us until they re-add our key.

**Prevention:**
- Track "connection succeeded but immediately closed" pattern. If TCP connect + handshake succeed but the connection closes within 5 seconds with zero messages exchanged, classify as "likely ACL rejection."
- After 3 consecutive ACL-pattern disconnects: extend backoff to 10+ minutes.
- Reset ACL-backoff on SIGHUP (operator may have asked remote to re-add the key).

**Detection:** Log pattern: "peer {} connected but closed within {}s, {} consecutive times (possible ACL rejection)".

## Moderate Pitfalls

### Pitfall 6: JSON Log Format Breaks on Unescaped Content

**What goes wrong:** Log message contains user-controlled data (peer address, error message from exception) with quotes or newlines. JSON pattern produces invalid JSON. Log aggregator rejects the line.

**Prevention:**
- All log messages use programmer-controlled format strings. Peer addresses are IP:port (safe). Namespace hashes are hex (safe). Error messages from stdlib are ASCII.
- Audit: ensure no `spdlog::info("{}", user_string)` where `user_string` could contain quotes. In chromatindb, the only external input in logs is IP addresses and hex hashes -- both safe.
- If paranoid: wrap the message formatter to escape quotes in `%v`. But this adds overhead and is likely unnecessary.

### Pitfall 7: Config Validation Rejects Valid Docker Compose Config

**What goes wrong:** Validation rule is too strict. Example: `sync_interval_seconds >= 5` rejects the benchmark config that uses `sync_interval_seconds: 2` for fast convergence testing.

**Prevention:**
- Validation ranges must accommodate testing/benchmarking scenarios.
- Use warnings (not errors) for "unusual but valid" values. Example: `sync_interval_seconds < 5` logs a warning but doesn't reject.
- Critical rejections only for values that would cause crashes or undefined behavior (e.g., port=0, max_peers=0).

### Pitfall 8: Version Injection Breaks Clean Builds

**What goes wrong:** `execute_process(git describe)` fails in Docker build or tarball source (no `.git` directory). CMake configure fails. Build broken.

**Prevention:**
- `ERROR_QUIET` on the execute_process call.
- Fallback: `if(NOT CHROMATINDB_GIT_HASH) set(CHROMATINDB_GIT_HASH "unknown") endif()`.
- The version number itself comes from `project(VERSION X.Y.Z)`, not git. Git hash is supplementary.

### Pitfall 9: Cursor Compaction Deletes Active Peer's Cursors

**What goes wrong:** Compaction timer fires. Peer A hasn't synced in 3601 seconds (just over the 3600s stale threshold). But peer A is still connected -- just idle. Cursors deleted. Next sync does a full reconciliation instead of incremental.

**Prevention:**
- Never delete cursors for currently connected peers, regardless of staleness.
- Compaction checks: `cursor_stale_seconds` threshold AND peer NOT in active connections list.
- The `known_peer_hashes` parameter to `cleanup_stale_cursors()` already provides this mechanism -- pass currently connected peer hashes.

### Pitfall 10: Tombstone GC "Fix" That Isn't a Bug

**What goes wrong:** Developer spends days investigating why libmdbx database file doesn't shrink after tombstone GC. Concludes the code is broken. Adds complex compaction logic. The actual answer: mmap databases don't shrink files. Freed pages are reused for new writes.

**Prevention:**
- First: measure `used_bytes()` (via `mdbx_env_info()`), not file size. If `used_bytes()` decreases after GC but file size doesn't, it's working correctly.
- Second: verify tombstone blobs actually have TTL > 0 and expiry entries in `expiry_map`. If tombstones are created with TTL=0 (permanent), they will never be GC'd -- by design.
- Third: if both are correct (used_bytes decreases, tombstones have TTL > 0), document and close the "issue."
- Only if `used_bytes()` doesn't decrease: then there's a real bug in the expiry scan or delete_blob_data path.

## Minor Pitfalls

### Pitfall 11: Log Rotation File Permissions in Docker

**What goes wrong:** Docker container runs as non-root user. Log file created with wrong permissions. On rotation, spdlog can't create the new file. Logging stops silently.

**Prevention:** Ensure log directory is writable by the container user. Docker: `--user $(id -u):$(id -g)` or pre-create the log directory with correct ownership.

### Pitfall 12: Config Validation Error Before Logging Init

**What goes wrong:** `validate_config()` throws before `logging::init()` is called. Error goes to stderr as an unformatted exception message. Not catastrophic, but ugly.

**Prevention:** Order in `cmd_run()`: (1) parse config, (2) validate config, (3) init logging. Validation errors go to stderr via the exception handler. This is acceptable -- the daemon hasn't started yet, stderr is the right place. The exception message should be self-contained.

### Pitfall 13: Reconnect to Self

**What goes wrong:** Operator misconfigures `bootstrap_peers` to include the node's own address. Node connects to itself. Handshake succeeds (same identity). Sync loop syncs with self (no-op). Wastes a connection slot.

**Prevention:** In `connect_to_peer()`: after handshake, compare remote namespace with self namespace. If equal, log warning and close. Already partially handled by PEX dedup but should be explicit.

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Mitigation |
|-------------|---------------|------------|
| CMake version injection | Git describe fails in Docker/tarball builds | ERROR_QUIET + "unknown" fallback |
| Config validation | Over-strict validation rejects valid benchmark configs | Warn (don't reject) for unusual-but-valid values |
| File logging | Unwritable log path = silent startup failure | Validate path in config, fall back to console on failure |
| Structured logging | JSON broken by unescaped content | Audit: only hex/IP data in log messages, no user strings |
| Keepalive timeout | Timeout too aggressive during large blob sync | Timeout > 2x max expected transfer time for 100 MiB blob |
| Auto-reconnect | Reconnect storm after partition heals | Jitter + staggered startup |
| ACL-aware reconnect | Infinite loop on permanently rejected peer | Track pattern, extend backoff after 3 consecutive rejects |
| Cursor compaction | Deleting active peer's cursors | Check connected peers before compacting |
| Tombstone GC | Misdiagnosing mmap file behavior as a bug | Check used_bytes() not file size |
| Startup integrity scan | Scan blocks startup for minutes on large databases | Optional --skip-integrity-check flag |
| libmdbx crash audit | Stale reader slots after kill -9 | Verify auto-cleanup on env_open() |
| Timer cleanup | Coroutine stack-use-after-return on shutdown | Cancel all timers before ioc.stop() |
| Delegation quota | Delegate writes bypass owner quota check | Verify ingest resolves delegate->owner for quota accounting |

## Sources

- [libmdbx README - Reader Slots](https://github.com/erthink/libmdbx/blob/master/README.md)
- [Asio Timeouts and Cancellation](https://cppalliance.org/asio/2023/01/02/Asio201Timeouts.html)
- [spdlog Sinks](https://github.com/gabime/spdlog/wiki/Sinks)
- [spdlog JSON Logging](https://github.com/gabime/spdlog/issues/1797)
