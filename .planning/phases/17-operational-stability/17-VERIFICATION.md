---
phase: 17-operational-stability
verified: 2026-03-10T17:15:00Z
status: passed
score: 14/14 must-haves verified
re_verification: false
human_verification:
  - test: "Send SIGTERM to a live node and confirm peer list is saved before connections drain"
    expected: "peers.json updated before connections close; exit code 0 on clean drain"
    why_human: "Signal delivery and process lifecycle cannot be verified by grep; requires a running process"
  - test: "Send SIGUSR1 to a live node and inspect log output"
    expected: "Multi-line metrics dump: global counters, per-peer lines, per-namespace lines, framed by === markers"
    why_human: "Log output content verification requires a running node"
  - test: "Crash the node mid-write of peers.json (e.g., SIGKILL during save) and restart"
    expected: "Node starts cleanly, loads last complete peers.json (no partial/corrupt file)"
    why_human: "Crash injection during atomic write requires manual fault injection"
---

# Phase 17: Operational Stability Verification Report

**Phase Goal:** Node survives restarts and crashes without losing peer connections or operational visibility
**Verified:** 2026-03-10T17:15:00Z
**Status:** PASSED
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | SIGTERM triggers PeerManager shutdown callback (save peers) before Server drain begins | VERIFIED | `server.cpp:59` calls `on_shutdown_()` before `asio::co_spawn(...drain...)`. Callback registered in `peer_manager.cpp:107-114` calls `save_persisted_peers()` first |
| 2 | Second SIGTERM force-closes all connections and stops ioc cleanly (no std::_Exit) | VERIFIED | `arm_signal_handler()` in `server.cpp:70-86` handles second signal with `ioc_.stop()`. `grep "std::_Exit" server.cpp` returns only a comment, zero actual calls |
| 3 | Exit code is 0 for clean shutdown, 1 for forced/timeout shutdown | VERIFIED | `server.cpp:77` sets `exit_code_ = 1` on second signal; `server.cpp:331` sets it on drain timeout. `main.cpp:134` returns `pm.exit_code()` |
| 4 | Expiry scan coroutine runs as PeerManager member function with timer-cancel shutdown | VERIFIED | `peer_manager.cpp:971` — `PeerManager::expiry_scan_loop()` member function; cancelled via `expiry_timer_->cancel()` at lines 112 and 143 |
| 5 | Drain timeout is 5 seconds (not configurable) | VERIFIED | `server.cpp:66` — `drain(std::chrono::seconds(5))` hardcoded |
| 6 | Peer list saved atomically via temp+fsync+rename+dir_fsync on every flush | VERIFIED | `peer_manager.cpp:1302-1375` — full POSIX atomic write: `O_WRONLY|O_CREAT|O_TRUNC`, `::write`, `::fsync`, `std::filesystem::rename`, dir `::fsync` |
| 7 | Peer list flushes every 30 seconds via periodic timer coroutine | VERIFIED | `PeerManager::peer_flush_timer_loop()` at `peer_manager.cpp:992`, spawned at `peer_manager.cpp:130`. Timer interval: `seconds(30)` |
| 8 | Corrupt peers.json on startup logs warning and starts with empty list (non-fatal) | VERIFIED | `peer_manager.cpp:1296-1299` — catch block: `spdlog::warn("failed to load persisted peers: {}", e.what())` + `persisted_peers_.clear()` |
| 9 | NodeMetrics struct has all required counters | VERIFIED | `peer_manager.h:61-68` — 6 fields: `ingests`, `rejections`, `syncs`, `rate_limited`, `peers_connected_total`, `peers_disconnected_total` |
| 10 | All counters are plain uint64_t (single io_context thread, no atomics needed) | VERIFIED | `peer_manager.h:62-67` — all `uint64_t`. `grep "std::atomic" peer_manager.h` returns nothing for NodeMetrics |
| 11 | Counters are incremented at correct code paths | VERIFIED | 7 increment sites in `peer_manager.cpp`: line 219 (`peers_connected_total`), 253 (`peers_disconnected_total`), 400 (`ingests`), 402 (`ingests` — duplicates), 404 (`rejections`), 665 and 833 (`syncs` — initiator and responder) |
| 12 | SIGUSR1 triggers multi-line metrics dump (global counters, per-peer, per-namespace) | VERIFIED | `sigusr1_loop()` at `peer_manager.cpp:1424` calls `dump_metrics()`; `dump_metrics()` at line 1433: global `log_metrics_line()`, per-peer loop, per-namespace loop |
| 13 | Periodic 60-second metrics line logged as structured key=value | VERIFIED | `metrics_timer_loop()` at `peer_manager.cpp:1462`, interval `seconds(60)`, calls `log_metrics_line()` which logs `"metrics: connections={} blobs={} storage={:.1f}MiB syncs={} ingests={} rejections={} uptime={}"` |
| 14 | SIGUSR1 handler and metrics timer are cancellable on shutdown | VERIFIED | `sigusr1_signal_.cancel()` in `on_shutdown_` callback (line 111) and `stop()` (line 142); `metrics_timer_loop()` checks `stopping_` flag on every iteration |

**Score:** 14/14 truths verified

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/net/server.h` | `ShutdownCallback`, `set_on_shutdown`, `arm_signal_handler`, `exit_code` | VERIFIED | All present: `ShutdownCallback` typedef (line 25), `set_on_shutdown` (line 53), `exit_code()` (line 56), `arm_signal_handler()` declared (line 78), `on_shutdown_` member (line 97), `exit_code_` member (line 98) |
| `db/net/server.cpp` | Re-arming signal handler, pre-drain callback, clean second-signal handling | VERIFIED | `arm_signal_handler()` (line 69), `stop()` invokes `on_shutdown_` before drain (line 59), second signal: `ioc_.stop()` not `std::_Exit` (line 79) |
| `db/peer/peer_manager.h` | `NodeMetrics` struct, `metrics_` member, `peer_flush_timer_loop`, `sigusr1_signal_`, `metrics_timer_loop`, `start_time_` | VERIFIED | `NodeMetrics` struct at line 61, `sigusr1_signal_` at line 249, `peer_flush_timer_loop` at line 185, `metrics_timer_loop` at line 216, `metrics_` at line 259, `start_time_` at line 260 |
| `db/peer/peer_manager.cpp` | Atomic `save_persisted_peers`, `peer_flush_timer_loop`, counter increments, `sigusr1_loop`, `dump_metrics`, `metrics_timer_loop`, `log_metrics_line` | VERIFIED | All implementations present and substantive. POSIX atomic write at lines 1329-1375. Counter increments at 7 locations. All coroutines as named member functions |
| `db/main.cpp` | Exit code propagated, expiry lambda removed | VERIFIED | `return pm.exit_code()` at line 134. `grep "run_expiry_scan" main.cpp` returns nothing |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `server.cpp` | `peer_manager.cpp` | `on_shutdown_` callback in `Server::stop()` before drain | WIRED | `server.cpp:59`: `if (on_shutdown_) on_shutdown_()` — fires before `asio::co_spawn(drain)` at line 66 |
| `peer_manager.cpp` | `server.cpp` | `expiry_timer_->cancel()` before `server_.stop()` | WIRED | `peer_manager.cpp:143`: `if (expiry_timer_) expiry_timer_->cancel()` then `server_.stop()` at line 144 |
| `main.cpp` | `server.cpp` | `pm.exit_code()` after `ioc.run()` | WIRED | `main.cpp:134`: `return pm.exit_code()` |
| `peer_manager.cpp Data handler` | `NodeMetrics::ingests / rejections` | `++metrics_.ingests` / `++metrics_.rejections` at ingest result | WIRED | Lines 400, 402 (`ingests`), 404 (`rejections`) |
| `peer_manager.cpp sync completion` | `NodeMetrics::syncs` | `++metrics_.syncs` at end of sync | WIRED | Lines 665 (initiator) and 833 (responder) |
| `peer_manager.cpp on_peer_connected` | `NodeMetrics::peers_connected_total` | `++metrics_.peers_connected_total` | WIRED | Line 219 |
| `sigusr1_loop` | `dump_metrics` | SIGUSR1 signal triggers `dump_metrics()` | WIRED | `sigusr1_loop()` at line 1424-1431 calls `dump_metrics()` at line 1429 |
| `metrics_timer_loop` | `log_metrics_line` | 60-second timer triggers `log_metrics_line()` | WIRED | `metrics_timer_loop()` at line 1469 calls `log_metrics_line()` |
| `log_metrics_line` | `storage_.used_bytes()` / `storage_.list_namespaces()` | Live storage queries (not cached counters) | WIRED | `peer_manager.cpp:1474` calls `storage_.used_bytes()`; line 1481 calls `storage_.list_namespaces()` |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| OPS-01 | 17-01 | SIGTERM graceful shutdown: stop accepting, drain, save peer list, bounded timeout | SATISFIED | `server.cpp:stop()` calls `on_shutdown_()` (saves peers) before 5s drain. Acceptor closed. Signal handler re-arms |
| OPS-02 | 17-01 | Expiry scan coroutine is cancellable (not asio::detached with no cancel path) | SATISFIED | `expiry_scan_loop()` is a member coroutine with timer-cancel pattern. Context.md explicitly left cancellation mechanism to Claude's discretion; timer-cancel chosen over `asio::cancellation_signal` — functionally equivalent (immediately cancellable on shutdown) |
| OPS-03 | 17-02 | Peer list saved atomically (temp + fsync + rename + dir fsync) | SATISFIED | `save_persisted_peers()` uses full POSIX atomic write pattern at lines 1329-1375 |
| OPS-04 | 17-02 | Peer list flushes periodically (30s timer) in addition to shutdown flush | SATISFIED | `peer_flush_timer_loop()` spawned in `start()`, fires `save_persisted_peers()` every 30 seconds |
| OPS-05 | 17-02 | NodeMetrics struct tracks blob count, storage used, connections, syncs, ingests, rejections, rate_limited | SATISFIED | All 6 counters in `NodeMetrics` struct. Note: `blob_count` and `storage_used` are live-queried from Storage at log time (correct approach per CONTEXT.md) rather than stored in NodeMetrics |
| OPS-06 | 17-03 | SIGUSR1 dumps current metrics via spdlog (follows sighup_loop coroutine pattern) | SATISFIED | `sigusr1_loop()` is identical pattern to `sighup_loop()`. `dump_metrics()` outputs global counters, per-peer, per-namespace |
| OPS-07 | 17-03 | Metrics logged periodically (60s timer) via spdlog | SATISFIED | `metrics_timer_loop()` fires `log_metrics_line()` every 60 seconds with structured key=value format |

**All 7 requirements accounted for. No orphaned requirements.**

---

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `tests/peer/test_peer_manager.cpp` | 253 | Pre-existing SEGFAULT in "PeerManager storage full signaling" | Warning | Pre-existing from Phase 16-03, logged in `deferred-items.md`, not caused by Phase 17 changes. 272/273 tests pass. |

No TODO/FIXME/placeholder comments or empty implementations found in Phase 17 modified files.

---

### Human Verification Required

#### 1. Live SIGTERM graceful shutdown

**Test:** Start node, connect 1-2 peers, send SIGTERM, inspect log and peers.json
**Expected:** Log shows "signal received, starting graceful shutdown", peers.json written before connections drain, process exits with code 0
**Why human:** Signal delivery and process lifecycle require a running process; file timestamps need manual inspection

#### 2. SIGUSR1 metrics dump output

**Test:** Start node, allow some syncs to accumulate, send `kill -USR1 <pid>`, inspect log
**Expected:** Multi-line dump beginning `=== METRICS DUMP (SIGUSR1) ===`, global `metrics:` line, per-peer entries, per-namespace entries, closing `=== END METRICS DUMP ===`
**Why human:** Log content can only be verified with a running node

#### 3. Atomic write crash safety

**Test:** Inject SIGKILL during `save_persisted_peers()` execution (e.g., via debugger breakpoint after `::write` but before `rename`), then restart
**Expected:** Node starts cleanly — either loads last complete peers.json or starts empty with a warning; never loads a partial/corrupt file
**Why human:** Fault injection during atomic write requires manual or tool-assisted crash injection

---

### Test Results

- **273 total tests** — 272 passed, 1 failed
- **Failing test:** #253 "PeerManager storage full signaling" — SEGFAULT, pre-existing from Phase 16-03, documented in `deferred-items.md`
- **Phase 17 tests added:** 2 new tests (NodeMetrics E2E counter test, NodeMetrics default initialization test) — both pass
- **No regressions** introduced by Phase 17 changes

---

### Gaps Summary

No gaps. All 14 observable truths verified against actual code. All 7 requirements satisfied. All key links wired. Build succeeds. 272/273 tests pass (1 pre-existing SEGFAULT unrelated to Phase 17).

The only notable implementation deviation from OPS-02's wording ("asio::cancellation_signal") is the timer-cancel pattern used instead — this was explicitly left to implementor discretion in CONTEXT.md and is functionally equivalent: the expiry coroutine is immediately cancellable on shutdown with no 60-second wait.

---

_Verified: 2026-03-10T17:15:00Z_
_Verifier: Claude (gsd-verifier)_
