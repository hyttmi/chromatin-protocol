# Phase 44: Network Resilience - Research

**Researched:** 2026-03-20
**Domain:** Asio-based TCP reconnection, ACL-aware backoff, receiver-side inactivity timeout
**Confidence:** HIGH

## Summary

Phase 44 adds three tightly-coupled network resilience features to the chromatindb daemon: auto-reconnect with jitter (CONN-01), ACL-aware reconnection suppression (CONN-02), and receiver-side inactivity timeout (CONN-03). All three features build on existing infrastructure -- `Server::reconnect_loop()` already has exponential backoff (1s-60s), `PeerManager::on_peer_connected()` already does ACL checks, and `PeerManager::on_peer_message()` is the natural point to track last-message timestamps.

The key technical challenge is the `handshake_ok` bug: when a peer connects, handshakes successfully, but then gets ACL-rejected in `on_peer_connected()`, the `on_ready` callback has already fired (setting `handshake_ok = true`), which causes `reconnect_loop` to reset `delay_sec` to 1 on line 254-257 -- creating a tight retry loop. This must be fixed as part of CONN-02, and the fix requires a signaling path from PeerManager back to Server to communicate "this was an ACL rejection, not a normal disconnect."

**Primary recommendation:** Extend `Server::reconnect_loop()` with jitter and ACL rejection tracking. Add an `acl_rejected` callback from PeerManager to Server. Add a per-connection `last_message_time` field in PeerInfo with a periodic sweep timer in PeerManager.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
None -- all decisions delegated to Claude's discretion.

### Claude's Discretion
User delegated all decisions for this infrastructure phase. Claude has full flexibility on:

**Auto-reconnect with backoff (CONN-01):**
- Extend existing `Server::reconnect_loop()` with random jitter (currently missing)
- Backoff range: 1s to 60s exponential with jitter (per success criteria)
- Scope: all outbound peers (bootstrap AND discovered) -- CONN-01 says "outbound peers" not "bootstrap only"
- Current `connect_once()` for discovered peers needs reconnect capability
- Fix the `handshake_ok` bug: ACL rejection in `on_peer_connected` fires after `on_ready` sets `handshake_ok = true`, causing delay reset to 1s (tight retry loop)

**ACL-aware reconnection suppression (CONN-02):**
- Detection heuristic: connects, handshakes successfully, disconnects quickly with zero application messages = ACL rejection pattern
- Track rejection count per address; after threshold (Claude decides count), enter extended 600s backoff
- SIGHUP resets suppression state (already reloads ACL -- extend to clear rejection counters)
- ACL rejection detection lives in Server layer (it owns the reconnect loop) with signal from PeerManager (it does the ACL check)

**Inactivity timeout (CONN-03):**
- Receiver-side only (NOT Ping sender) -- avoids adding wire protocol messages and AEAD nonce desync
- Track last-message-received timestamp per connection
- Periodic check or per-connection timer (Claude decides mechanism)
- Configurable via config field (Claude decides field name and default)
- Applies to all connections (inbound and outbound) -- a dead peer is dead regardless of direction
- New config field validated by existing `validate_config()`
- New timer added to `cancel_all_timers()` following Phase 42 pattern

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| CONN-01 | Node auto-reconnects to all outbound peers on disconnect with exponential backoff (1s-60s) and jitter | Extend `Server::reconnect_loop()` with `std::uniform_int_distribution` jitter. Convert `connect_once()` discovered peers to use reconnect path. Fix `handshake_ok` bug. |
| CONN-02 | Node suppresses reconnection attempts to peers that rejected the connection via ACL | Add per-address rejection counter in Server. Signal from PeerManager ACL rejection back to Server. Extended 600s backoff after threshold. SIGHUP resets counters. |
| CONN-03 | Node detects and disconnects dead peers via inactivity timeout (no messages received within deadline) | Add `last_message_time` to PeerInfo. Update in `on_peer_message()` and `on_peer_connected()`. Periodic sweep timer. New `inactivity_timeout_seconds` config field. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest via FetchContent | Timer, coroutines, TCP | Already in project, no alternatives needed |
| spdlog | latest via FetchContent | Structured logging | Already in project |
| nlohmann/json | latest via FetchContent | Config parsing | Already in project |

### Supporting
No new dependencies. All features use existing stack:
- `asio::steady_timer` for inactivity sweep
- `std::chrono::steady_clock` for timestamp tracking
- `<random>` (already included in peer_manager.h) for jitter calculation
- `std::uniform_int_distribution` for jitter range

**Installation:** None required. Zero new dependencies.

## Architecture Patterns

### Recommended Project Structure
No new files needed. All changes in existing files:
```
db/
├── net/
│   └── server.h/.cpp          # reconnect_loop jitter, ACL rejection tracking, discovered peer reconnect
├── peer/
│   └── peer_manager.h/.cpp    # ACL rejection signal, inactivity timer, last_message_time tracking
└── config/
    └── config.h/.cpp          # inactivity_timeout_seconds field + validation
```

### Pattern 1: Timer-Cancel Pattern (existing, reuse)
**What:** Each periodic coroutine creates a local `asio::steady_timer`, stores its address in a nullable member pointer, and nulls it after the wait completes. `cancel_all_timers()` cancels all live timers.
**When to use:** Any new periodic loop in PeerManager.
**Example (from existing code):**
```cpp
asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        expiry_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        expiry_timer_ = nullptr;
        if (ec || stopping_) co_return;
        // ... do work ...
    }
}
```

### Pattern 2: Exponential Backoff with Jitter
**What:** Current `reconnect_loop` doubles delay on failure (1s, 2s, 4s, ... 60s) but has NO jitter. Adding jitter prevents thundering herd when multiple peers reconnect simultaneously.
**Implementation approach:** After computing exponential delay, apply uniform random jitter in range [0, delay/2]. This gives effective range [delay, delay*1.5] which spreads reconnection attempts without violating the 1s-60s bounds.
**Example:**
```cpp
// Existing: delay_sec = std::min(delay_sec * 2, max_delay);
// New: add jitter
std::uniform_int_distribution<int> jitter_dist(0, delay_sec / 2);
int jittered_delay = delay_sec + jitter_dist(rng);
```

### Pattern 3: Callback Signal for ACL Rejection
**What:** PeerManager needs to signal Server that a connection was ACL-rejected (not just disconnected). Server uses this to track rejection counts per address.
**Implementation approach:** Add a new callback on Server (`set_on_acl_rejected(callback)`) that PeerManager fires from `on_peer_connected()` when ACL check fails, BEFORE calling `conn->close()`. Server's reconnect loop tracks rejection count per address and escalates backoff after threshold.
**Why not use on_disconnected:** The `on_disconnected` callback fires for ALL disconnects (normal, timeout, ACL). We need to distinguish ACL rejections specifically.

### Pattern 4: Periodic Sweep vs Per-Connection Timer for Inactivity
**What:** Two options for inactivity timeout detection.
**Recommendation: Periodic sweep timer.** A single timer that iterates `peers_` every N seconds is simpler and cheaper than N per-connection timers. With max_peers=32, the sweep is trivial. Matches the existing pattern of periodic loops (expiry_scan, metrics, cursor_compaction).
**Sweep interval:** Check every 15-30 seconds. This is acceptable granularity -- a 120s timeout doesn't need millisecond precision.

### Anti-Patterns to Avoid
- **Ping/Pong sender for keepalive:** OUT OF SCOPE per requirements. Adds wire protocol messages and risks AEAD nonce desync.
- **Per-connection timers for inactivity:** Creates N timers for N peers. Unnecessary complexity.
- **Jitter that violates bounds:** Jitter should never make the delay < 1s or > 60s (the success criteria bounds).
- **Storing ACL rejection state in PeerInfo:** PeerInfo is per-connection, created fresh each connect. ACL rejection tracking must live in Server (persists across reconnect attempts) or as a separate map in PeerManager.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Random number generation | Custom RNG | `std::mt19937` + `std::uniform_int_distribution` | Already used in peer_manager.cpp for PEX shuffle |
| Timer management | Custom timer tracking | Timer-cancel pattern with `asio::steady_timer*` members | Established project pattern, Phase 42 consolidated |
| Config validation | Separate validation logic | Extend existing `validate_config()` accumulator | Phase 42 built the error accumulation pattern |

## Common Pitfalls

### Pitfall 1: The handshake_ok Bug (CRITICAL)
**What goes wrong:** ACL rejection in `on_peer_connected()` fires AFTER `on_ready` has already set `handshake_ok = true`. In `reconnect_loop()` line 254, `if (ok || handshake_ok)` sees handshake_ok=true, resets delay to 1s. This creates a tight 1-second retry loop against a peer that will always reject.
**Why it happens:** The on_ready callback fires in Connection::run() after handshake but before message_loop. Then PeerManager::on_peer_connected is called (via on_connected_), does the ACL check, and calls conn->close(). The connection returns from run() with ok=false BUT handshake_ok is already true.
**How to avoid:** The ACL rejection signal must prevent delay reset. Two approaches:
1. Add an `acl_rejected` flag alongside `handshake_ok` and check `if ((ok || handshake_ok) && !acl_rejected)` for delay reset.
2. Use the new ACL rejection callback to set per-address state in Server before the reconnect loop evaluates.
**Warning signs:** Peer logs show "reconnecting to X in 1s" repeating rapidly for an ACL-rejecting peer.

### Pitfall 2: Reconnect for Discovered Peers
**What goes wrong:** `connect_once()` currently has no reconnect path. When a discovered peer disconnects, it's gone forever until the next PEX round.
**Why it happens:** `connect_once` was designed as fire-and-forget for PEX-discovered peers. But CONN-01 says "all outbound peers."
**How to avoid:** Either convert `connect_once` to use a reconnect path, or have the reconnect logic live in PeerManager using a callback when a discovered peer disconnects. The simplest approach: make `connect_once` use `connect_to_peer` (which already has reconnect) instead of its current fire-and-forget lambda.

### Pitfall 3: Inactivity Timeout During Sync
**What goes wrong:** A large sync (transferring many blobs) may involve long gaps between PeerManager-visible messages because the actual data flows through Connection::message_loop -> on_message callback. If a sync takes >120s with no non-sync messages, the inactivity timer could kill a healthy connection.
**Why it happens:** Sync messages ARE application messages that flow through `on_peer_message()`. As long as `last_message_time` is updated in `on_peer_message()` (which handles ALL message types including sync), this is not a problem. Sync messages (ReconcileInit, ReconcileRanges, ReconcileItems, BlobRequest, BlobResponse, SyncRequest, SyncComplete) all pass through on_peer_message.
**How to avoid:** Update `last_message_time` at the TOP of `on_peer_message()`, before any type-specific dispatch. This catches all message types.
**Warning signs:** Connections dropping during active syncs.

### Pitfall 4: Jitter Overflow at Max Delay
**What goes wrong:** If delay_sec = 60 and jitter adds up to 30, effective delay = 90, violating the 60s max.
**How to avoid:** Cap the jittered delay: `int effective = std::min(delay_sec + jitter, max_delay)`. Or apply jitter as a fraction of delay rather than additive.

### Pitfall 5: SIGHUP Race with Reconnect
**What goes wrong:** SIGHUP resets ACL rejection counters while a reconnect_loop is sleeping in its 600s extended backoff. The timer continues sleeping even after SIGHUP.
**How to avoid:** Store the extended-backoff timer pointer (like the timer-cancel pattern) so SIGHUP can cancel it, forcing an immediate reconnect attempt with reset counters.

### Pitfall 6: Inactivity Timer on Fresh Connections
**What goes wrong:** A connection that just completed handshake but hasn't exchanged any application messages yet could be killed by the inactivity sweep if `last_message_time` isn't initialized.
**How to avoid:** Initialize `last_message_time` in `on_peer_connected()` (when the peer is added to peers_) to `steady_clock::now()`. This gives the peer a full timeout window to send its first message.

## Code Examples

### Adding Jitter to reconnect_loop (Server)
```cpp
// In Server class (server.h): add member
std::mt19937 rng_{std::random_device{}()};

// In reconnect_loop (server.cpp):
asio::awaitable<void> Server::reconnect_loop(const std::string& address) {
    int delay_sec = 1;
    constexpr int max_delay = 60;

    while (!draining_) {
        // Apply jitter: uniform random in [0, delay/2]
        int jitter = 0;
        if (delay_sec > 1) {
            std::uniform_int_distribution<int> dist(0, delay_sec / 2);
            jitter = dist(rng_);
        }
        int effective_delay = std::min(delay_sec + jitter, max_delay);
        spdlog::info("reconnecting to {} in {}s (base={}s)", address, effective_delay, delay_sec);
        // ... timer wait using effective_delay ...
    }
}
```

### ACL Rejection Tracking (Server)
```cpp
// In Server class (server.h):
struct ReconnectState {
    int delay_sec = 1;
    int acl_rejection_count = 0;
};
std::unordered_map<std::string, ReconnectState> reconnect_state_;
using AclRejectedCallback = std::function<void(const std::string& address)>;
AclRejectedCallback on_acl_rejected_;

// Called from PeerManager when ACL rejects:
void Server::on_acl_rejected(const std::string& address) {
    auto& state = reconnect_state_[address];
    ++state.acl_rejection_count;
    if (state.acl_rejection_count >= 3) {  // Threshold
        state.delay_sec = 600;  // Extended backoff
    }
}
```

### Inactivity Sweep Timer (PeerManager)
```cpp
// In PeerInfo (peer_manager.h):
uint64_t last_message_time = 0;  // steady_clock ms since epoch

// In on_peer_connected:
info.last_message_time = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

// In on_peer_message (at the TOP, before dispatch):
auto* peer = find_peer(conn);
if (peer) {
    peer->last_message_time = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Sweep loop:
asio::awaitable<void> PeerManager::inactivity_check_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        inactivity_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        inactivity_timer_ = nullptr;
        if (ec || stopping_) co_return;

        auto now_ms = /* steady_clock now in ms */;
        uint64_t timeout_ms = static_cast<uint64_t>(config_.inactivity_timeout_seconds) * 1000;
        std::vector<net::Connection::Ptr> to_close;
        for (const auto& peer : peers_) {
            if (peer.last_message_time > 0 &&
                (now_ms - peer.last_message_time) > timeout_ms) {
                to_close.push_back(peer.connection);
            }
        }
        for (auto& conn : to_close) {
            spdlog::warn("inactivity timeout: disconnecting {}", conn->remote_address());
            conn->close();
        }
    }
}
```

### Config Field Addition
```cpp
// In Config struct (config.h):
uint32_t inactivity_timeout_seconds = 120;  // 0 = disabled

// In load_config (config.cpp):
cfg.inactivity_timeout_seconds = j.value("inactivity_timeout_seconds", cfg.inactivity_timeout_seconds);
// Also add to known_keys set

// In validate_config (config.cpp):
if (cfg.inactivity_timeout_seconds != 0 && cfg.inactivity_timeout_seconds < 30) {
    errors.push_back("inactivity_timeout_seconds must be 0 (disabled) or >= 30 (got " +
                      std::to_string(cfg.inactivity_timeout_seconds) + ")");
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| No reconnect for discovered peers | `connect_once()` fire-and-forget | v1.0 | Discovered peers lost on disconnect |
| Exponential backoff, no jitter | Same | v1.0 | Thundering herd possible |
| No inactivity detection | None | Never existed | Dead peers consume connection slots |
| No ACL-aware backoff | Tight retry loop (bug) | v2.0 (ACL added) | CPU waste on ACL-rejecting peers |

## Open Questions

1. **ACL rejection threshold before extended backoff**
   - What we know: The CONTEXT.md says "after repeated rejections" and specifies 600s extended backoff
   - What's unclear: Exact count threshold
   - Recommendation: Use 3 consecutive rejections. Low enough to detect quickly, high enough to tolerate transient issues (e.g., SIGHUP in progress on remote peer).

2. **Inactivity timeout default value**
   - What we know: Must be configurable. Success criteria says "configurable inactivity timeout"
   - What's unclear: Best default
   - Recommendation: 120 seconds (2 minutes). Matches common keepalive intervals in TCP applications. Long enough that normal sync gaps don't trigger false positives. Short enough to detect actual dead peers reasonably quickly.

3. **Should connect_once peers get full reconnect or limited reconnect?**
   - What we know: CONN-01 says "all outbound peers" not "bootstrap only"
   - What's unclear: Whether discovered peers should reconnect indefinitely or have a retry limit
   - Recommendation: Full reconnect for discovered peers, same as bootstrap. If they're ACL-rejected, CONN-02 suppression handles it. If they're simply offline, normal backoff handles it. The peer persistence system already handles pruning after `MAX_PERSIST_FAILURES=3`.

4. **Inactivity sweep interval**
   - What we know: Needs to be periodic, not per-connection
   - What's unclear: Optimal interval
   - Recommendation: 30 seconds. Provides acceptable detection latency (peer dead for up to timeout+30s before disconnect) while keeping CPU overhead negligible.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (FetchContent) |
| Quick run command | `cd build && ctest --test-dir . -R "server\|peer_manager\|config" --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir . --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CONN-01 | Reconnect with jitter, discovered peer reconnect | unit | `cd build && ctest -R "test_server" --output-on-failure` | Exists (db/tests/net/test_server.cpp) -- needs new test cases |
| CONN-02 | ACL rejection suppression, extended backoff, SIGHUP reset | unit | `cd build && ctest -R "test_peer_manager\|test_server" --output-on-failure` | Exists -- needs new test cases |
| CONN-03 | Inactivity timeout detection and disconnect | unit | `cd build && ctest -R "test_peer_manager" --output-on-failure` | Exists -- needs new test cases |
| CONN-03 | Config field validation | unit | `cd build && ctest -R "test_config" --output-on-failure` | Exists (db/tests/config/test_config.cpp) -- needs new test cases |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . && ctest --test-dir . -R "server\|peer_manager\|config" --output-on-failure`
- **Per wave merge:** `cd build && cmake --build . && ctest --test-dir . --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. Tests for CONN-01/02/03 will be added alongside implementation as new TEST_CASE sections in existing test files:
- `db/tests/net/test_server.cpp` -- reconnect jitter, ACL suppression (Server layer)
- `db/tests/peer/test_peer_manager.cpp` -- inactivity timeout, ACL signal (PeerManager layer)
- `db/tests/config/test_config.cpp` -- inactivity_timeout_seconds validation

## Sources

### Primary (HIGH confidence)
- **Direct code inspection** of server.h/.cpp, connection.h/.cpp, peer_manager.h/.cpp, config.h/.cpp -- all patterns, bugs, and integration points verified by reading actual source
- **Project memory (MEMORY.md)** -- architecture decisions, timer-cancel pattern, AEAD nonce desync risk, receiver-side-only keepalive decision

### Secondary (MEDIUM confidence)
- **CONTEXT.md** -- user decisions, heuristics for ACL rejection detection, integration points mapped during discussion phase

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - zero new dependencies, all existing libraries
- Architecture: HIGH - all patterns verified by reading existing source code, integration points precisely identified
- Pitfalls: HIGH - handshake_ok bug confirmed by line-level code reading (server.cpp:185-197,247-258), inactivity-during-sync scenario analyzed against message flow

**Research date:** 2026-03-20
**Valid until:** 2026-04-20 (stable -- no external dependencies, all internal patterns)
