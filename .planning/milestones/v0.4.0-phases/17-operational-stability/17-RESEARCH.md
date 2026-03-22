# Phase 17: Operational Stability - Research

**Researched:** 2026-03-10
**Domain:** Graceful shutdown, peer persistence, runtime metrics, coroutine cancellation
**Confidence:** HIGH

## Summary

Phase 17 adds three capabilities to an already-operational daemon: (1) graceful shutdown with bounded drain and peer list persistence, (2) runtime metrics counters with periodic and on-demand logging, and (3) cancellable expiry scanning. All the fundamental building blocks already exist in the codebase -- `Server::drain()` handles connection teardown, `save_persisted_peers()` / `load_persisted_peers()` handle peer file I/O, `sighup_loop()` demonstrates the coroutine signal handler pattern, and spdlog is used everywhere. The work is connecting existing pieces, filling gaps between them, and adding a `NodeMetrics` struct with plain `uint64_t` counters at existing code paths.

The dominant technical risk is shutdown ordering: `PeerManager::stop()` currently sets `stopping_` and calls `server_.stop()` but does NOT save the peer list. The SIGTERM handler in `Server::start()` calls `std::_Exit(1)` on second signal, which is a hard kill bypassing all destructors. The expiry scan coroutine in `main.cpp` is `asio::detached` with no cancellation path. All three gaps are well-understood and have clean solutions using existing Asio primitives.

No new dependencies are required. No new wire protocol messages. No new config schema beyond what the requirements specify. The entire phase operates within the existing single-threaded `io_context` model.

**Primary recommendation:** Build in three plans -- (1) shutdown sequencing with peer persistence, (2) NodeMetrics struct with counter placement, (3) SIGUSR1 dump and periodic metrics log. Plans 1 and 2 are independent but plan 3 depends on plan 2.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Periodic 60s log: structured key=value single line via spdlog::info
  - Example: `metrics: connections=3 blobs=1420 storage=84.2MiB syncs=12 ingests=45 rejections=2 uptime=3600`
- SIGUSR1 dump: detailed multi-line report including:
  - Global counters (same as periodic line)
  - Per-peer breakdown: address + namespace prefix (e.g., `127.0.0.1:4200 (ns:a1b2c3d4...)`)
  - Per-namespace blob counts and storage used
- Human-readable storage size with unit suffix (MiB) in log output
- Required counters from OPS-05: blob_count, storage_used, connections, syncs, ingests, rejections, rate_limited
- Additional counters: uptime_seconds, peers_connected_total, peers_disconnected_total
- All counters monotonically increasing since startup (never reset)
- Prometheus-style: rate = delta/time calculated by consumer, not by the node
- On SIGTERM: save peer list FIRST, then drain connections
  - Ensures peer file is written while connection list is most accurate
  - If drain hangs, peer file is already saved
- 2nd signal: skip drain timeout, force-close all connections immediately, but still run clean exit path (destructors, spdlog flush). NOT std::_Exit.
- Exit code 0 for clean shutdown (drain completed), exit code 1 for forced/timeout shutdown
- Drain timeout stays hardcoded at 5s (constexpr, matches TTL-as-invariant philosophy)
- Flush triggers: 30s periodic timer + shutdown flush. No event-triggered writes (no flush on connect/disconnect)
- Atomic write pattern (temp + fsync + rename + dir fsync) implemented inline in peer_manager, not as reusable utility (YAGNI)
- Corrupt peers.json on startup: log warning, start with empty list. Non-fatal -- bootstrap peers still work
- Prune stale entries at startup only (existing behavior), not during periodic flush
- SIGUSR1 handler follows existing sighup_loop() coroutine pattern in PeerManager
- Expiry scanner currently in main.cpp as lambda with asio::detached -- needs to move into a cancellable member coroutine
- Server::drain already exists with goodbye/timeout pattern -- extend, don't rewrite
- existing stopping_ flag in PeerManager should integrate with new shutdown sequence

### Claude's Discretion
- Storage metric source: MDBX stat (ground truth) vs in-memory counter -- pick the right tradeoff
- Expiry coroutine cancellation mechanism (cancellation_signal vs stop token)
- NodeMetrics struct layout and where counters are incremented
- SIGUSR1 per-namespace enumeration strategy (iterate MDBX vs maintain in-memory index)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| OPS-01 | SIGTERM triggers graceful shutdown: stop accepting connections, drain in-flight coroutines, save peer list, bounded timeout | Shutdown sequencing pattern, Server::drain() extension, PeerManager::stop() rewrite, second-signal handling |
| OPS-02 | Expiry scan coroutine is cancellable via asio::cancellation_signal (not asio::detached with no cancel path) | co_spawn + bind_cancellation_slot pattern, move from main.cpp lambda to PeerManager member coroutine |
| OPS-03 | Persistent peer list is saved atomically on clean shutdown (temp + fsync + rename + dir fsync) | Atomic file write pattern, POSIX fsync semantics, save_persisted_peers() rewrite |
| OPS-04 | Persistent peer list flushes periodically (30s timer) in addition to shutdown flush | Timer loop coroutine pattern (same as sync_timer_loop/pex_timer_loop), stopping_ check |
| OPS-05 | NodeMetrics struct tracks blob count, storage used, connections, syncs, ingests, rejections, rate-limited count | NodeMetrics struct design, counter placement at on_peer_message/on_peer_connected/sync completion sites |
| OPS-06 | SIGUSR1 dumps current metrics via spdlog (follows sighup_loop coroutine pattern) | sighup_loop() template, signal_set for SIGUSR1, per-namespace enumeration via list_namespaces() |
| OPS-07 | Metrics logged periodically (60s timer) via spdlog | Timer loop coroutine, structured key=value format, human-readable storage size formatting |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest (FetchContent) | signal_set for SIGUSR1, cancellation_signal for expiry, steady_timer for periodic flush/metrics | Already used for all async I/O; co_spawn cancellation is built-in |
| spdlog | latest (FetchContent) | Metrics output (periodic line, SIGUSR1 dump) | Already used for all logging; no new dependency |
| nlohmann/json | latest (FetchContent) | peers.json serialization (existing) | Already used for peer persistence and config |
| libmdbx | latest (FetchContent) | Storage stats via env.get_info() for storage_used metric | Already used for all storage; used_bytes() already implemented |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| POSIX `<unistd.h>` | system | fsync(), rename() for atomic file write | Peer list atomic persistence on Linux |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Plain uint64_t counters | std::atomic<uint64_t> | Single io_context thread means no data races; atomic adds MFENCE overhead on hot path. Use plain uint64_t. |
| asio::cancellation_signal for expiry | Shared stopping_ flag check in loop | cancellation_signal is the Asio-idiomatic way and triggers error_code on timer wait. Flag checking requires manual polling and doesn't interrupt the 60s sleep. cancellation_signal is better. |
| MDBX stat for storage_used | In-memory running counter | MDBX env.get_info().mi_geo.current is O(1) and authoritative. Running counter drifts after crashes and misses decrement paths (expiry purge). Use MDBX stat. |
| MDBX iteration for per-namespace stats | In-memory namespace index | SIGUSR1 is operator-triggered (rare). list_namespaces() already exists and works. Adding an in-memory index is premature optimization for an infrequent operation. Use list_namespaces(). |

## Architecture Patterns

### Recommended Changes
```
db/
  peer/
    peer_manager.h     # Add: NodeMetrics struct, sigusr1_signal_, metrics timer, expiry coroutine, peer_flush_timer
    peer_manager.cpp   # Add: sigusr1_loop(), metrics_timer_loop(), peer_flush_timer_loop(), expiry_scan_loop()
                       #       atomic save_persisted_peers(), shutdown sequence rewrite
  net/
    server.h           # Add: shutdown_callback_ for PeerManager to hook before drain
    server.cpp         # Modify: second signal handling (no std::_Exit), shutdown callback invocation
  main.cpp             # Modify: remove expiry lambda, adjust shutdown flow
```

### Pattern 1: Shutdown Sequence (OPS-01)

**What:** On SIGTERM, PeerManager saves peer list first, then Server drains connections.

**Current state:** `Server::start()` installs a SIGINT/SIGTERM handler that calls `Server::stop()`. `PeerManager::stop()` sets `stopping_` and calls `server_.stop()`. The signal handler in Server never calls PeerManager::stop() -- it calls its own stop(). The peer list is never saved on shutdown.

**Required change:** PeerManager must own the shutdown orchestration. Server needs a pre-drain callback so PeerManager can save peers before connections start closing.

**Implementation approach:**
```cpp
// In Server: add a pre-shutdown callback
void Server::set_on_shutdown(std::function<void()> cb) {
    on_shutdown_ = std::move(cb);
}

// In Server::stop() -- call before drain:
void Server::stop() {
    if (draining_) return;
    draining_ = true;
    if (on_shutdown_) on_shutdown_();  // PeerManager saves peers here
    acceptor_.close(ec);
    asio::co_spawn(ioc_, drain(std::chrono::seconds(5)), asio::detached);
}

// In PeerManager constructor -- register the callback:
server_.set_on_shutdown([this]() {
    stopping_ = true;
    save_persisted_peers();  // Save while connection list is accurate
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    // Cancel periodic timers by setting stopping_ -- they check this flag
});
```

**Second signal handling:**
```cpp
// Current (BROKEN): std::_Exit(1) -- skips all destructors and spdlog flush
// Fixed: force-close connections, clean exit
signals_.async_wait([this](asio::error_code ec, int sig) {
    if (ec) return;
    if (draining_) {
        spdlog::info("second signal received ({}), forcing shutdown", sig);
        // Force close all remaining connections
        for (auto& conn : connections_) {
            conn->close();
        }
        connections_.clear();
        ioc_.stop();  // At this point drain coroutine is already running; just stop
        return;
    }
    spdlog::info("signal {} received, starting graceful shutdown", sig);
    stop();
});
```

**Key insight:** The second-signal handler must keep the async_wait re-armed. Currently it uses a one-shot lambda. Since we want to handle the SECOND signal too, we need to re-arm after the first:

```cpp
// Re-arm signal handler for second signal
void Server::arm_signal_handler() {
    signals_.async_wait([this](asio::error_code ec, int sig) {
        if (ec) return;
        if (draining_) {
            // Second signal -- force exit
            spdlog::info("second signal ({}), forcing shutdown", sig);
            for (auto& conn : connections_) conn->close();
            connections_.clear();
            spdlog::default_logger()->flush();
            ioc_.stop();
            return;
        }
        spdlog::info("signal {} received, graceful shutdown", sig);
        stop();
        arm_signal_handler();  // Re-arm for second signal
    });
}
```

### Pattern 2: Atomic File Write (OPS-03)

**What:** Write peers.json atomically so a crash mid-write doesn't corrupt the file.

**Current state:** `save_persisted_peers()` opens the file directly with `std::ofstream` and writes. A crash mid-write leaves a truncated/corrupt file.

**Required change:** Write to temp file, fsync, rename over target, fsync directory.

```cpp
void PeerManager::save_persisted_peers() {
    // ... existing prune + cap + json build ...

    auto path = peers_file_path();
    auto tmp_path = path;
    tmp_path += ".tmp";

    try {
        std::filesystem::create_directories(path.parent_path());

        // Write to temp file
        {
            int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                spdlog::warn("failed to open temp peer file: {}", strerror(errno));
                return;
            }
            auto json_str = j.dump(2);
            auto written = ::write(fd, json_str.data(), json_str.size());
            if (written < 0 || static_cast<size_t>(written) != json_str.size()) {
                ::close(fd);
                spdlog::warn("failed to write temp peer file");
                return;
            }
            ::fsync(fd);
            ::close(fd);
        }

        // Atomic rename
        std::filesystem::rename(tmp_path, path);

        // Directory fsync (required on Linux for rename durability)
        int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }

        spdlog::debug("saved {} persisted peers to {}", persisted_peers_.size(), path.string());
    } catch (const std::exception& e) {
        spdlog::warn("failed to save persisted peers: {}", e.what());
        // Clean up temp file on failure
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
}
```

**Why raw POSIX instead of std::ofstream:** `std::ofstream` does not expose `fsync()`. The temp + fsync + rename pattern requires `fsync()` on the file descriptor before rename. Using raw POSIX `open()/write()/fsync()/close()` is the correct approach. Alternative: use `std::ofstream` then convert to fd via platform-specific means, but that's more complex for no benefit.

### Pattern 3: Coroutine Signal Handler (OPS-06, reuse of sighup_loop)

**What:** SIGUSR1 triggers a metrics dump via a coroutine member function.

**Existing pattern to copy:**
```cpp
// Already in PeerManager -- sighup_loop():
void PeerManager::setup_sighup_handler() {
    sighup_signal_.add(SIGHUP);
    asio::co_spawn(ioc_, sighup_loop(), asio::detached);
}

asio::awaitable<void> PeerManager::sighup_loop() {
    while (!stopping_) {
        auto [ec, sig] = co_await sighup_signal_.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        handle_sighup();
    }
}
```

**SIGUSR1 implementation (same pattern):**
```cpp
// In peer_manager.h:
asio::signal_set sigusr1_signal_;  // New member

// In peer_manager.cpp:
void PeerManager::setup_sigusr1_handler() {
    sigusr1_signal_.add(SIGUSR1);
    asio::co_spawn(ioc_, sigusr1_loop(), asio::detached);
}

asio::awaitable<void> PeerManager::sigusr1_loop() {
    while (!stopping_) {
        auto [ec, sig] = co_await sigusr1_signal_.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        dump_metrics();
    }
}
```

**Critical:** This MUST be a member function, NOT a lambda. RETROSPECTIVE.md documents stack-use-after-return with lambda-based signal handlers due to compiler coroutine frame lifetimes.

### Pattern 4: Cancellable Expiry Coroutine (OPS-02)

**What:** Move the expiry scanner from a main.cpp lambda to a PeerManager member coroutine that can be cancelled on shutdown.

**Current state (main.cpp, line 130-143):**
```cpp
asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
    while (true) {
        asio::steady_timer timer(ioc);
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec) co_return;
        auto purged = storage.run_expiry_scan();
        if (purged > 0) {
            spdlog::info("expiry scan: purged {} blobs", purged);
        }
    }
}(), asio::detached);
```

**Problem:** `asio::detached` with no cancellation path. On shutdown, the timer sleeps for up to 60s. The `stopping_` flag is not checked. The coroutine is a lambda (stack-use-after-return risk, per RETROSPECTIVE.md).

**Solution:** Move to PeerManager member coroutine, check `stopping_` after each timer wake:
```cpp
asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;

        auto purged = storage_.run_expiry_scan();
        if (purged > 0) {
            spdlog::info("expiry scan: purged {} blobs", purged);
        }
    }
}
```

**Cancellation at shutdown:** When `PeerManager::stop()` sets `stopping_ = true`, the expiry coroutine will exit at the next timer wake. For immediate cancellation (don't wait up to 60s), keep a pointer to the timer or use `asio::cancellation_signal`:

Option A (timer pointer -- simpler):
```cpp
// Member:
asio::steady_timer* expiry_timer_ = nullptr;

// In expiry_scan_loop:
asio::steady_timer timer(ioc_);
expiry_timer_ = &timer;
timer.expires_after(std::chrono::seconds(60));
auto [ec] = co_await timer.async_wait(...);
expiry_timer_ = nullptr;

// In stop():
if (expiry_timer_) expiry_timer_->cancel();
```

Option B (cancellation_signal -- more idiomatic for Asio):
```cpp
// Member:
asio::cancellation_signal expiry_cancel_;

// Spawn with bound cancellation slot:
asio::co_spawn(ioc_,
    expiry_scan_loop(),
    asio::bind_cancellation_slot(expiry_cancel_.slot(), asio::detached));

// In stop():
expiry_cancel_.emit(asio::cancellation_type::terminal);
```

**Recommendation:** Use Option A (timer pointer). It matches the existing `sync_notify` timer-cancel pattern already used in PeerManager for sync message queues. The pattern is proven in this codebase. `cancellation_signal` requires `this_coro::reset_cancellation_state` inside the coroutine to opt into non-default cancellation types, adding complexity for no practical benefit here.

### Pattern 5: NodeMetrics Struct (OPS-05)

**What:** Lightweight struct with plain `uint64_t` counters, updated inline at existing code paths.

```cpp
// In peer_manager.h (not a separate file -- YAGNI)
struct NodeMetrics {
    // From OPS-05 requirements
    uint64_t ingests = 0;           // Successful blob ingestions (Data messages)
    uint64_t rejections = 0;        // Failed ingestions (validation errors)
    uint64_t syncs = 0;             // Completed sync rounds (initiator + responder)
    uint64_t rate_limited = 0;      // Blobs rejected by rate limiter (Phase 18)

    // Additional counters (from CONTEXT.md decisions)
    uint64_t uptime_seconds = 0;    // Computed at dump time, not incremented
    uint64_t peers_connected_total = 0;
    uint64_t peers_disconnected_total = 0;
};
```

**Why plain uint64_t, not atomic:** The entire PeerManager runs on a single `io_context` thread. There is no concurrent access to these counters. Using `std::atomic<uint64_t>` would add MFENCE overhead on every increment on the ingest hot path for zero correctness benefit. If a cross-thread metrics reader is added in the future, atomics can be retrofitted then.

**Counter types:**
- `blob_count` and `storage_used`: NOT in NodeMetrics. These are queried live from Storage/MDBX at dump time. They are not monotonically increasing (blobs expire, storage shrinks). Storing them as counters would require decrement logic and drift tracking.
- `connections`: NOT a counter -- it's `peers_.size()` at dump time. The `peers_connected_total` / `peers_disconnected_total` are the monotonic counters.

**Counter placement (where to increment):**

| Counter | File | Location | Line Context |
|---------|------|----------|-------------|
| `ingests` | peer_manager.cpp | `on_peer_message` Data handler | After `engine_.ingest()` returns `accepted == true` |
| `rejections` | peer_manager.cpp | `on_peer_message` Data handler | After `engine_.ingest()` returns `accepted == false` |
| `syncs` | peer_manager.cpp | `run_sync_with_peer` and `handle_sync_as_responder` | After sync round completes (at function end, before co_return) |
| `rate_limited` | peer_manager.cpp | Rate limit check in `on_peer_message` | When rate limit fires (Phase 18 -- stub at 0 for now) |
| `peers_connected_total` | peer_manager.cpp | `on_peer_connected` | After adding to `peers_` deque |
| `peers_disconnected_total` | peer_manager.cpp | `on_peer_disconnected` | After removing from `peers_` deque |

**Sync counter note:** `syncs` should count completed sync rounds, including both initiator and responder perspectives. `run_sync_with_peer` (initiator) and `handle_sync_as_responder` (responder) each increment on successful completion. The user can derive sync rate = delta(syncs) / delta(time).

### Pattern 6: Periodic Timer Loop (OPS-04, OPS-07)

**What:** Timer-based periodic tasks for peer list flush (30s) and metrics log (60s).

**Existing pattern to follow (sync_timer_loop):**
```cpp
asio::awaitable<void> PeerManager::sync_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(config_.sync_interval_seconds));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        co_await sync_all_peers();
    }
}
```

**Peer flush timer (30s):**
```cpp
asio::awaitable<void> PeerManager::peer_flush_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        save_persisted_peers();  // Now uses atomic write pattern
    }
}
```

**Metrics timer (60s):**
```cpp
asio::awaitable<void> PeerManager::metrics_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        log_metrics_line();
    }
}
```

### Pattern 7: Metrics Formatting

**Periodic line (compact, single line, structured key=value):**
```cpp
void PeerManager::log_metrics_line() {
    auto storage_bytes = storage_.used_bytes();
    auto storage_mib = static_cast<double>(storage_bytes) / (1024.0 * 1024.0);
    auto uptime = compute_uptime_seconds();

    spdlog::info("metrics: connections={} blobs={} storage={:.1f}MiB "
                 "syncs={} ingests={} rejections={} uptime={}",
                 peers_.size(),
                 count_total_blobs(),  // list_namespaces() iteration
                 storage_mib,
                 metrics_.syncs,
                 metrics_.ingests,
                 metrics_.rejections,
                 uptime);
}
```

**SIGUSR1 dump (detailed, multi-line):**
```cpp
void PeerManager::dump_metrics() {
    spdlog::info("=== METRICS DUMP (SIGUSR1) ===");

    // Global counters (same data as periodic line)
    log_metrics_line();

    // Per-peer breakdown
    for (const auto& peer : peers_) {
        auto ns_hex = to_hex(peer.connection->peer_pubkey(), 4);
        spdlog::info("  peer: {} (ns:{})", peer.address, ns_hex);
    }

    // Per-namespace stats
    auto namespaces = engine_.list_namespaces();
    for (const auto& ns : namespaces) {
        auto ns_hex = to_hex(std::span<const uint8_t>(ns.namespace_id.data(), 4));
        spdlog::info("  namespace: {} blobs={} latest_seq={}",
                     ns_hex, /* blob count */ ns.latest_seq_num, ns.latest_seq_num);
    }

    spdlog::info("=== END METRICS DUMP ===");
}
```

**blob_count challenge:** `list_namespaces()` returns latest_seq_num per namespace but NOT blob count. Seq gaps from expiry mean seq_num != blob count. For accurate blob counts, a full namespace scan would be required. Options:
1. Report latest_seq_num as a proxy (cheap, slightly misleading)
2. Count blobs by iterating blobs_map per namespace (expensive, but SIGUSR1 is rare)
3. Add a `count_blobs(namespace_id)` method to Storage that does a cursor count in a read transaction

**Recommendation:** For the periodic metrics line, report total blob count by summing `latest_seq_num` values (approximate but O(N namespaces), not O(N blobs)). For the SIGUSR1 detailed dump, use `list_namespaces()` which already exists and provides per-namespace latest_seq. A `count_blobs()` method could be added to Storage if exact counts are needed, but that's optimization beyond what OPS-05 requires. The seq_num is a reasonable upper bound and easy to compute.

**Uptime computation:**
```cpp
// Member:
std::chrono::steady_clock::time_point start_time_;

// In start():
start_time_ = std::chrono::steady_clock::now();

// Helper:
uint64_t PeerManager::compute_uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
}
```

### Anti-Patterns to Avoid

- **Lambda signal handlers:** The RETROSPECTIVE.md documents stack-use-after-return when using lambdas for signal handlers that become coroutines. Always use member functions for signal handler coroutines.

- **fsync on every peer event:** Calling `save_persisted_peers()` on every connect/disconnect would block the io_context thread with `fsync()`. Use the 30s periodic timer + shutdown flush instead.

- **Atomic counters in single-threaded context:** `std::atomic<uint64_t>` adds unnecessary MFENCE overhead when only the io_context thread accesses the counters. Use plain `uint64_t`.

- **In-memory storage counter:** Maintaining a running `total_bytes` counter that's incremented on ingest and decremented on expiry/delete will drift after crashes. Always query `storage_.used_bytes()` (which calls `env.get_info().mi_geo.current`) for the authoritative value.

- **std::_Exit on second signal:** Current behavior. This skips destructors, doesn't flush spdlog, doesn't clean up temp files. Replace with clean exit path.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Atomic file write | Custom retry/journal | POSIX temp+fsync+rename+dir_fsync | Well-known pattern; covers crash during write. LWN article validates this as the correct Linux approach. |
| Signal handling in coroutines | Raw signal(), sigaction() | asio::signal_set + member coroutine | Asio signal_set integrates with io_context. Raw signal handlers are async-signal-unsafe and can't interact with coroutines. |
| Coroutine cancellation | Custom flag polling | asio::cancellation_signal or timer-cancel | Asio-native; handles the timer interrupt cleanly via error_code |
| Metrics struct | External metrics library (prometheus-cpp, StatsD) | Plain uint64_t struct + spdlog | PROJECT.md prohibits HTTP/REST. No external metrics endpoint needed. 30 lines vs. a dependency. |
| Storage size formatting | sprintf / manual formatting | fmt via spdlog | spdlog uses fmt; `{:.1f}MiB` formatting is built in |

**Key insight:** Every building block needed for Phase 17 already exists in this codebase or in standard POSIX/Asio APIs. The risk is purely in connecting them correctly, not in implementing new mechanisms.

## Common Pitfalls

### Pitfall 1: Shutdown Blocks for 60s on Expiry Timer
**What goes wrong:** The expiry scan coroutine sleeps for 60 seconds. If shutdown starts mid-sleep, the drain timeout (5s) expires but the expiry coroutine is still sleeping. `ioc_.stop()` is called while the coroutine frame is live.
**Why it happens:** No cancellation path for the expiry timer. `stopping_` is set but the coroutine doesn't check it until the timer fires.
**How to avoid:** Store a pointer to the expiry timer and cancel it in `stop()`. Or use `asio::cancellation_signal`. Either way, the coroutine wakes immediately, checks `stopping_`/error_code, and returns.
**Warning signs:** Shutdown takes much longer than 5 seconds. Logs show "shutdown complete" but process doesn't exit.

### Pitfall 2: Peer List Not Saved If Drain Hangs
**What goes wrong:** If `save_persisted_peers()` is called AFTER `Server::drain()`, and drain blocks (stuck peer, slow goodbye), the peer list is never written.
**Why it happens:** Saving after drain is the wrong order.
**How to avoid:** Decision is locked: save FIRST, then drain. The `on_shutdown_` callback in Server fires before `drain()` starts.
**Warning signs:** peers.json is stale or missing after restart despite peers having been connected.

### Pitfall 3: fsync Blocks io_context Thread
**What goes wrong:** `fsync()` is a blocking syscall. If called on every peer connect/disconnect, it pauses ALL coroutines on the io_context thread.
**Why it happens:** Naive implementation calls save on every event.
**How to avoid:** Decision is locked: 30s periodic timer + shutdown flush only. No event-triggered writes. The 30s flush is acceptable because worst case you lose 30s of peer list updates on crash.
**Warning signs:** Connection latency spikes every time a peer connects or disconnects.

### Pitfall 4: Second Signal Causes std::_Exit
**What goes wrong:** Current code calls `std::_Exit(1)` on second signal. This skips ALL destructors (MDBX might not flush), doesn't flush spdlog (last log lines lost), and doesn't clean up temp files.
**Why it happens:** Quick and dirty implementation for "just kill it".
**How to avoid:** Replace with force-close all connections + `ioc_.stop()`. Destructors still run. spdlog still flushes. Exit code is 1 (forced).
**Warning signs:** After forced shutdown, spdlog output is truncated. MDBX may need recovery on next start.

### Pitfall 5: Counter Overflow
**What goes wrong:** `uint32_t` counter wraps after 4 billion ingests (reachable in high-throughput scenarios).
**Why it happens:** Using too-small integer type.
**How to avoid:** All counters are `uint64_t`. At 10,000 ingests/second, uint64_t overflows after 58 million years.
**Warning signs:** Counter values suddenly drop to near zero.

### Pitfall 6: Race Between SIGUSR1 Dump and Shutdown
**What goes wrong:** `dump_metrics()` iterates `peers_` while `stop()` is clearing connections. The deque is being modified during iteration.
**Why it happens:** Both run on the io_context thread, but `stop()` can modify state between `co_await` points in the dump.
**How to avoid:** `dump_metrics()` is synchronous (no `co_await`). It runs to completion before any other event fires. The risk is if dump_metrics itself contains `co_await` -- which it should NOT. Keep it pure synchronous.
**Warning signs:** Crash during SIGUSR1 dump after SIGTERM sent.

## Code Examples

### Complete Shutdown Sequence
```cpp
// server.cpp -- modified signal handler
void Server::start() {
    // ... existing acceptor setup ...

    // Signal handling with re-arm for second signal
    arm_signal_handler();
}

void Server::arm_signal_handler() {
    signals_.async_wait([this](asio::error_code ec, int sig) {
        if (ec) return;
        if (draining_) {
            // Second signal: force shutdown
            spdlog::info("second signal ({}), forcing shutdown", sig);
            for (auto& conn : connections_) conn->close();
            connections_.clear();
            spdlog::default_logger()->flush();
            ioc_.stop();
            return;
        }
        spdlog::info("signal {} received, graceful shutdown", sig);
        stop();
        arm_signal_handler();  // Re-arm for second signal
    });
}

void Server::stop() {
    if (draining_) return;
    draining_ = true;
    if (on_shutdown_) on_shutdown_();  // PeerManager: save peers, cancel timers
    asio::error_code ec;
    acceptor_.close(ec);
    asio::co_spawn(ioc_, drain(std::chrono::seconds(5)), asio::detached);
}
```

### Atomic Peer File Write (POSIX)
```cpp
#include <fcntl.h>
#include <unistd.h>

void PeerManager::save_persisted_peers() {
    // ... existing prune + cap + JSON build ...

    auto path = peers_file_path();
    auto tmp_path = std::filesystem::path(path.string() + ".tmp");

    try {
        std::filesystem::create_directories(path.parent_path());
        auto json_str = j.dump(2);

        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            spdlog::warn("failed to create temp peer file: {}", strerror(errno));
            return;
        }

        auto written = ::write(fd, json_str.data(), json_str.size());
        if (written < 0 || static_cast<size_t>(written) != json_str.size()) {
            ::close(fd);
            std::filesystem::remove(tmp_path);
            spdlog::warn("failed to write temp peer file");
            return;
        }

        if (::fsync(fd) != 0) {
            ::close(fd);
            std::filesystem::remove(tmp_path);
            spdlog::warn("failed to fsync temp peer file");
            return;
        }
        ::close(fd);

        // Atomic rename
        std::filesystem::rename(tmp_path, path);

        // Directory fsync for rename durability
        int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }

        spdlog::debug("saved {} persisted peers", persisted_peers_.size());
    } catch (const std::exception& e) {
        spdlog::warn("failed to save persisted peers: {}", e.what());
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
}
```

### Human-Readable Storage Size
```cpp
std::string format_storage_size(uint64_t bytes) {
    double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    // spdlog uses fmt internally
    return fmt::format("{:.1f}MiB", mib);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `std::_Exit(1)` on second signal | Force-close + `ioc_.stop()` (clean exit) | This phase | Destructors run, spdlog flushes, MDBX closes cleanly |
| Direct file write for peers.json | Atomic temp+fsync+rename+dir_fsync | This phase | Crash-safe peer persistence |
| No metrics at all | NodeMetrics struct + spdlog dump | This phase | Runtime observability without external tooling |
| Expiry as detached lambda | Member coroutine with cancellation | This phase | Clean shutdown, no stack-use-after-return risk |

## Open Questions

1. **Blob count accuracy in metrics line**
   - What we know: `list_namespaces()` returns `latest_seq_num` per namespace, not blob count. Seq gaps from expiry mean seq_num > actual blob count.
   - What's unclear: Whether the user cares about exact blob count or if seq_num is an acceptable proxy.
   - Recommendation: Use sum of `latest_seq_num` values as the `blobs=` metric for the periodic line. It's an upper bound and O(N namespaces) to compute. For SIGUSR1 detailed dump, also report per-namespace `latest_seq_num`. If exact counts are needed later, add `Storage::count_blobs(namespace_id)` which cursor-counts blobs_map entries.

2. **Exit code propagation**
   - What we know: Decision says exit code 0 for clean, 1 for forced/timeout.
   - What's unclear: How to propagate exit code from drain coroutine (which runs asynchronously) back to `main()` which calls `ioc.run()`.
   - Recommendation: Use a member variable `int exit_code_ = 0` on Server. Set to 1 if drain times out or second signal received. `main()` returns this value after `ioc.run()` completes. Expose via `Server::exit_code()` or pass through PeerManager.

## Sources

### Primary (HIGH confidence -- source code verified)
- `db/peer/peer_manager.h` line 186-220 -- sighup_loop() pattern, stopping_ flag, sighup_signal_ member
- `db/peer/peer_manager.cpp` lines 112-116 -- PeerManager::stop() current implementation
- `db/peer/peer_manager.cpp` lines 847-859 -- sighup_loop() implementation (template for SIGUSR1)
- `db/peer/peer_manager.cpp` lines 1193-1260 -- load/save_persisted_peers() current implementation
- `db/net/server.cpp` lines 51-59 -- SIGINT/SIGTERM handler with std::_Exit(1) on second signal
- `db/net/server.cpp` lines 62-72 -- Server::stop() and drain(5s)
- `db/net/server.cpp` lines 293-323 -- drain() coroutine: goodbye + timer + force close
- `db/main.cpp` lines 130-143 -- expiry scan lambda (asio::detached, no cancel path)
- `db/storage/storage.cpp` lines 693-696 -- used_bytes() via env.get_info().mi_geo.current
- `db/storage/storage.h` lines 105 -- list_namespaces() declaration
- `build/_deps/asio-src/include/asio/co_spawn.hpp` -- co_spawn cancellation architecture
- `build/_deps/asio-src/src/doc/overview/cpp20_coroutines.qbk` lines 176-196 -- cancellation_state documentation

### Secondary (MEDIUM confidence)
- `.planning/research/ARCHITECTURE.md` -- Previous milestone research on shutdown sequencing and metrics
- `.planning/research/SUMMARY.md` -- Validated research confirming no new dependencies, counter type decisions
- `.planning/RETROSPECTIVE.md` -- SIGHUP lambda stack-use-after-return (referenced in MEMORY.md, validates member function approach)
- [LWN: crash-safe atomic file write](https://lwn.net/Articles/457667/) -- temp + fsync + rename + directory fsync required on Linux

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies; all Asio APIs verified in local source tree
- Architecture: HIGH -- every integration point identified at line-level in existing code; patterns copied from working sighup_loop/sync_timer_loop implementations
- Pitfalls: HIGH -- all pitfalls derive from existing codebase bugs (std::_Exit, lambda stack-use-after-return) or documented RETROSPECTIVE.md lessons; not speculative

**Research date:** 2026-03-10
**Valid until:** 2026-04-10 (stable -- no external dependencies or fast-moving APIs)
