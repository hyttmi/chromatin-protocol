# Phase 83: Bidirectional Keepalive - Research

**Researched:** 2026-04-04
**Domain:** Application-level TCP keepalive via Asio coroutines
**Confidence:** HIGH

## Summary

Phase 83 adds active keepalive to chromatindb: the node sends Ping to every connected TCP peer every 30 seconds and disconnects any peer that produces 60 seconds of silence (2 missed keepalive cycles). The existing infrastructure is nearly complete: `Connection::message_loop()` already handles Ping (replies with Pong) and Pong (currently a no-op comment "nothing to do for now"), `send_message()` uses the Phase 79 send queue for safe concurrent sends, and `PeerManager` already has an `inactivity_check_loop` that sweeps peers by `last_message_time` on PeerInfo.

The main gap is that Ping/Pong are handled inside `Connection::message_loop()` and never reach `PeerManager::on_peer_message()`, so the existing `PeerInfo::last_message_time` is NOT updated by Pong receipts. The CONTEXT.md decision D-03 places `last_recv_time_` on the Connection object, updated on every decoded message in `message_loop()`. This is the correct design: it captures ALL traffic including Ping/Pong, and PeerManager reads it during the keepalive sweep.

The implementation requires: (1) add `last_recv_time_` to Connection, updated on every decoded message in `message_loop()`, (2) add `keepalive_loop()` to PeerManager that sends Ping to all TCP peers every 30s and checks `last_recv_time_` for 60s silence, (3) wire up timer lifecycle (start/stop/cancel). The existing `inactivity_check_loop` becomes redundant once keepalive is active -- it should be replaced or unified.

**Primary recommendation:** Add `std::chrono::steady_clock::time_point last_recv_time_` to Connection (updated on every decoded message in message_loop), add `keepalive_loop()` coroutine to PeerManager using the established timer-cancel pattern, and replace the existing `inactivity_check_loop` with the new keepalive mechanism.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Single PeerManager coroutine. One timer fires every 30s, iterates all TCP peers, sends Ping to each via send_message(). Consistent with sync_timer_loop and expiry_scan_loop patterns. One timer for all peers.
- **D-02:** Any received message resets the silence timer. Update last-activity timestamp on every decoded message (Pong, Data, BlobNotify, sync, etc.). If the peer is sending any traffic, it's alive. Most forgiving approach.
- **D-03:** last_recv_time_ lives on the Connection object. Updated in message_loop on every decoded message. PeerManager reads it during the keepalive check. Connection owns the data since it owns the message_loop.
- **D-04:** Immediate TCP close via conn->close(). No Goodbye message (peer is unresponsive anyway). on_peer_disconnected handles cleanup (cursor grace period from Phase 82).

### Claude's Discretion
- Clock type for last_recv_time_ (steady_clock vs system_clock)
- Whether to skip Ping for peers currently in sync (probably not -- they're still alive)
- Keepalive interval configurability (hardcode 30s or add config field)
- Log level for keepalive disconnect (info vs warn)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CONN-01 | Node sends Ping to all TCP peers every 30 seconds (bidirectional keepalive) | keepalive_loop() coroutine iterates peers_, skips UDS, calls conn->send_message(Ping). Timer-cancel pattern identical to sync_timer_loop. |
| CONN-02 | Peer that doesn't respond within 2 missed keepalive cycles is disconnected | keepalive_loop() checks Connection::last_recv_time_; 60s silence threshold; conn->close() for disconnect (D-04). |
</phase_requirements>

## Architecture Patterns

### Recommended Changes

```
db/net/connection.h      -- Add last_recv_time_ member + public accessor
db/net/connection.cpp     -- Update last_recv_time_ on every decoded message in message_loop()
db/peer/peer_manager.h    -- Add keepalive_loop() declaration, keepalive_timer_ pointer
db/peer/peer_manager.cpp  -- Implement keepalive_loop(), wire into start/stop/cancel_all_timers
db/tests/peer/test_keepalive.cpp -- Unit tests for keepalive behavior (new file)
```

### Pattern 1: Timer-Cancel Coroutine (established pattern)

**What:** All PeerManager timer loops use the same structure: stack-local `asio::steady_timer`, pointer stored in member for external cancel, `while (!stopping_)` loop.

**When to use:** Every periodic task in PeerManager.

**Example (from existing sync_timer_loop):**
```cpp
asio::awaitable<void> PeerManager::keepalive_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        keepalive_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        keepalive_timer_ = nullptr;
        if (ec || stopping_) co_return;

        // Send Ping + check silence for each TCP peer
        // ...
    }
}
```

### Pattern 2: Peer Iteration with Connection Snapshot

**What:** Take a snapshot of Connection pointers before iterating peers_ to avoid iterator invalidation across co_await points. Used by `sync_all_peers()` and `on_blob_ingested()`.

**When to use:** Any coroutine that iterates peers_ and calls co_await (like send_message).

**Example (from sync_all_peers):**
```cpp
std::vector<net::Connection::Ptr> connections;
for (const auto& peer : peers_) {
    connections.push_back(peer.connection);
}
for (const auto& conn : connections) {
    // safe to co_await here -- peers_ may change but our copy is stable
}
```

### Pattern 3: UDS Skip for Peer-Internal Operations

**What:** UDS connections are clients, not peers. Keepalive is peer-to-peer only.

**When to use:** Any fan-out operation targeting TCP peers only.

**Example (from on_blob_ingested):**
```cpp
if (peer.connection->is_uds()) continue;  // UDS = client, not peer
```

### Anti-Patterns to Avoid

- **Iterating peers_ across co_await without snapshot:** peers_ is a deque; elements can be added/removed during co_await. Always snapshot Connection::Ptr before await-heavy loops.
- **Using system_clock for elapsed time:** Use steady_clock. system_clock can jump (NTP, user adjustment). steady_clock is monotonic.
- **Sending Ping from Connection's message_loop:** Ping must originate from PeerManager (one timer for all peers, D-01). Connection only handles received Ping/Pong.
- **Storing timestamp as uint64_t milliseconds:** The existing `last_message_time` uses this pattern on PeerInfo for historical reasons. For Connection, use `std::chrono::steady_clock::time_point` directly -- more type-safe, avoids manual epoch math, and aligns with D-03 placing it on Connection.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Monotonic time | Manual epoch calculations | `std::chrono::steady_clock::time_point` | Type-safe, monotonic, no manual conversion |
| Timer lifecycle | Custom timer management | Established timer-cancel pointer pattern | 8 existing coroutines use this pattern; consistency matters |
| Send serialization | Manual locking for Ping sends | `conn->send_message()` via send queue (Phase 79) | AEAD nonce ordering guaranteed by queue |

## Common Pitfalls

### Pitfall 1: Pong Never Reaches PeerInfo::last_message_time

**What goes wrong:** Ping/Pong are handled in `Connection::message_loop()` (line 762-770 of connection.cpp) and never call `message_cb_`. The existing `PeerInfo::last_message_time` in `on_peer_message()` is therefore NOT updated by Pong receipts. If keepalive relies on PeerInfo's timestamp, a peer that only responds with Pong (no other traffic) would be falsely detected as dead.

**Why it happens:** Connection handles Ping/Pong as transport-level messages before dispatching to PeerManager.

**How to avoid:** D-03 correctly places `last_recv_time_` on Connection, updated for every decoded message (including Ping, Pong, Goodbye, and all dispatched types). PeerManager reads `conn->last_recv_time()`.

**Warning signs:** Tests pass when peers exchange Data messages but fail for Ping-only traffic.

### Pitfall 2: Iterator Invalidation During Keepalive Loop

**What goes wrong:** `keepalive_loop()` iterates `peers_` and calls `co_await conn->send_message(Ping)` for each peer. Between awaits, a peer could connect or disconnect, modifying `peers_` deque and invalidating iterators.

**Why it happens:** Every co_await is a suspension point; other coroutines can run and modify shared state.

**How to avoid:** Snapshot `Connection::Ptr` vector before the loop (Pattern 2 above). After the send loop, re-find each peer via `find_peer(conn)` to read `last_recv_time_`.

**Warning signs:** Crash or undefined behavior when peers connect/disconnect during keepalive sweep.

### Pitfall 3: Race Between Keepalive Disconnect and Ongoing Sync

**What goes wrong:** Keepalive disconnects a peer that is in the middle of a slow sync (transferring many blobs). The sync coroutine sees a broken connection and aborts.

**Why it happens:** D-02 says "any received message resets the silence timer." During sync, the syncing peer is sending BlobResponse messages regularly, which will update `last_recv_time_`. So this is only a problem if the sync completely stalls (no messages for 60s).

**How to avoid:** No special handling needed. If a peer sends zero messages for 60 seconds during sync, the connection is genuinely dead and should be closed. The sync has a 120s per-blob timeout (`BLOB_TRANSFER_TIMEOUT`) which is already longer, but that's fine -- keepalive catches the case where the entire connection dies silently.

**Warning signs:** None expected -- this is a non-issue given D-02.

### Pitfall 4: Existing inactivity_check_loop Conflict

**What goes wrong:** The existing `inactivity_check_loop` (config: `inactivity_timeout_seconds = 120`) runs every 30s and disconnects peers idle for 120s based on `PeerInfo::last_message_time`. Once keepalive is active (Ping every 30s), this loop would never trigger because keepalive Pong responses update the timestamp. But the two mechanisms use different timestamps (`PeerInfo::last_message_time` vs `Connection::last_recv_time_`).

**Why it happens:** Historical: inactivity detection was added before active keepalive existed.

**How to avoid:** Replace or unify. The keepalive mechanism (60s timeout, active Ping probing) is strictly stronger than passive inactivity detection (120s timeout, no probing). Options: (a) remove `inactivity_check_loop` entirely and let keepalive handle it, (b) keep both but have keepalive also update `PeerInfo::last_message_time` so they stay in sync. Option (a) is cleaner -- keepalive subsumes inactivity detection.

**Warning signs:** Two different timeouts disconnecting peers at different times, confusing logs.

### Pitfall 5: Keepalive Ping on Freshly Connected Peer

**What goes wrong:** A peer connects and immediately receives a keepalive Ping before handshake completes. The Ping is sent as encrypted transport message, but the session isn't established yet.

**Why it happens:** `on_peer_connected` fires after handshake, so this is actually safe. The peer is in `peers_` only after authentication. However, `last_recv_time_` on Connection must be initialized to "now" during handshake completion, otherwise the first keepalive check (up to 30s later) sees a stale zero-initialized timestamp and might disconnect immediately.

**How to avoid:** Initialize `last_recv_time_` to `steady_clock::now()` when `authenticated_` becomes true (end of `do_handshake` success path), or when Connection is first created. Also initialize in `on_peer_connected` if checking from PeerInfo side.

**Warning signs:** Freshly connected peers immediately disconnected by keepalive.

## Discretion Recommendations

### Clock Type: Use `std::chrono::steady_clock`
**Recommendation:** `steady_clock` (not `system_clock`).
**Rationale:** Monotonic. No NTP jumps. Consistent with existing `start_time_` member in PeerManager and `bucket_last_refill` / `last_message_time` semantics elsewhere. All existing time tracking in PeerManager uses steady_clock milliseconds since epoch.
**Confidence:** HIGH.

### Skip Ping for Syncing Peers: No
**Recommendation:** Send Ping to all TCP peers including those currently syncing.
**Rationale:** The Ping is tiny (transport header only, no payload). Syncing peers are actively sending messages anyway, so the Pong is irrelevant for liveness -- but skipping them adds complexity (checking `syncing` flag) for zero benefit. Keep it simple.
**Confidence:** HIGH.

### Configurability: Hardcode 30s Interval, 60s Timeout
**Recommendation:** Use `static constexpr` values in PeerManager. No config field.
**Rationale:** Keepalive is a protocol-level behavior, not a user preference. The 30s/60s values are chosen to balance detection speed against bandwidth. Adding config fields adds complexity with no real user benefit. The existing `inactivity_timeout_seconds` config field can remain for backward compatibility but become effectively unused (or deprecated).
**Confidence:** HIGH.

### Log Level for Keepalive Disconnect: warn
**Recommendation:** `spdlog::warn` for keepalive disconnects.
**Rationale:** A peer going silent is abnormal (network failure, crash). It warrants attention but is not an error (it's expected in real deployments). Consistent with `inactivity_check_loop` which uses `spdlog::warn`. Include idle duration in the message for debugging.
**Confidence:** HIGH.

## Code Examples

### Connection: last_recv_time_ Member

```cpp
// In connection.h, private section:
std::chrono::steady_clock::time_point last_recv_time_;

// Public accessor:
std::chrono::steady_clock::time_point last_recv_time() const { return last_recv_time_; }
```

### Connection: Update in message_loop

```cpp
asio::awaitable<void> Connection::message_loop() {
    while (!closed_) {
        auto msg = co_await recv_encrypted();
        if (!msg) {
            if (!closed_) {
                spdlog::info("connection lost (read failed)");
            }
            break;
        }

        // Update last-recv time for keepalive (D-02, D-03: any decoded message resets silence)
        last_recv_time_ = std::chrono::steady_clock::now();

        auto decoded = TransportCodec::decode(*msg);
        if (!decoded) {
            spdlog::warn("received invalid transport message");
            continue;
        }
        // ... existing switch ...
    }
}
```

### Connection: Initialize last_recv_time_ on Authentication

```cpp
// At end of each successful do_handshake_* path, before co_return true:
last_recv_time_ = std::chrono::steady_clock::now();
```

Or simpler: initialize in the constructor/create methods:
```cpp
Connection::Connection(...)
    : socket_(std::move(socket)), ..., last_recv_time_(std::chrono::steady_clock::now()) {}
```

### PeerManager: keepalive_loop

```cpp
asio::awaitable<void> PeerManager::keepalive_loop() {
    static constexpr auto KEEPALIVE_INTERVAL = std::chrono::seconds(30);
    static constexpr auto KEEPALIVE_TIMEOUT  = std::chrono::seconds(60);

    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        keepalive_timer_ = &timer;
        timer.expires_after(KEEPALIVE_INTERVAL);
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        keepalive_timer_ = nullptr;
        if (ec || stopping_) co_return;

        auto now = std::chrono::steady_clock::now();

        // Snapshot connections (peers_ may change across co_await)
        std::vector<net::Connection::Ptr> tcp_peers;
        for (const auto& peer : peers_) {
            if (!peer.connection->is_uds()) {
                tcp_peers.push_back(peer.connection);
            }
        }

        // Phase 1: Check for dead peers (before sending new Pings)
        std::vector<net::Connection::Ptr> to_close;
        for (const auto& conn : tcp_peers) {
            auto silence = now - conn->last_recv_time();
            if (silence > KEEPALIVE_TIMEOUT) {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(silence).count();
                spdlog::warn("keepalive: disconnecting {} ({}s silent)",
                             conn->remote_address(), secs);
                to_close.push_back(conn);
            }
        }
        for (auto& conn : to_close) {
            conn->close();  // D-04: immediate close, no Goodbye
        }

        // Phase 2: Send Ping to remaining live TCP peers
        for (const auto& conn : tcp_peers) {
            if (std::find(to_close.begin(), to_close.end(), conn) != to_close.end()) {
                continue;  // Already closed
            }
            std::span<const uint8_t> empty{};
            co_await conn->send_message(wire::TransportMsgType_Ping, empty);
        }
    }
}
```

### PeerManager: Wiring in start/stop

```cpp
// In start(), after existing timer spawns:
asio::co_spawn(ioc_, keepalive_loop(), asio::detached);

// In cancel_all_timers():
if (keepalive_timer_) keepalive_timer_->cancel();
```

## Relationship to Existing Inactivity Detection

The existing `inactivity_check_loop` uses `PeerInfo::last_message_time` (updated in `on_peer_message`). The new keepalive uses `Connection::last_recv_time_` (updated in `message_loop`). Once keepalive is active:

1. Keepalive sends Ping every 30s, so peers always have traffic.
2. If peer responds with Pong, `last_recv_time_` updates (captured in message_loop).
3. If peer is truly dead, keepalive disconnects after 60s.
4. The old `inactivity_check_loop` (120s timeout) would never trigger because Pong resets `last_message_time` -- but wait, Pong does NOT update `PeerInfo::last_message_time` because Pong never reaches `on_peer_message`.

This means the two mechanisms would coexist awkwardly: keepalive (60s, using Connection timestamp) and inactivity (120s, using PeerInfo timestamp that misses Pong). The inactivity loop could still fire for peers that respond to Ping with Pong but send no other messages.

**Resolution:** Remove `inactivity_check_loop` and let keepalive handle all dead-peer detection. This is simpler and more correct. The `inactivity_timeout_seconds` config field can remain (with a deprecation comment) for config file backward compatibility, but the loop is no longer spawned.

Alternatively, if removing inactivity_check_loop feels too invasive for this phase: have `keepalive_loop` also update `PeerInfo::last_message_time` when it reads a fresh `last_recv_time_` from the Connection. But this is more complex than just removing the old loop.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (BUILD_TESTING section) |
| Quick run command | `cd build && ./db_tests "[keepalive]"` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CONN-01 | Node sends Ping to all TCP peers every 30s | unit | `./db_tests "[keepalive]" -c "sends Ping"` | No -- Wave 0 |
| CONN-02 | Peer silent for 60s is disconnected | unit | `./db_tests "[keepalive]" -c "disconnect"` | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ./db_tests "[keepalive]"`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before verify

### Wave 0 Gaps
- [ ] `db/tests/peer/test_keepalive.cpp` -- covers CONN-01, CONN-02
- [ ] Add to CMakeLists.txt test sources list

## Open Questions

1. **Should inactivity_check_loop be removed or kept alongside keepalive?**
   - What we know: Keepalive (60s active probing) is strictly stronger than passive inactivity (120s passive). They use different timestamps. Keeping both adds complexity without benefit.
   - What's unclear: Whether removing `inactivity_check_loop` needs deprecation of the `inactivity_timeout_seconds` config field.
   - Recommendation: Remove `inactivity_check_loop`, add deprecation comment to config field. Keepalive subsumes it. This can be a separate task within the plan if the planner wants to isolate it.

2. **Should `PeerInfo::last_message_time` still be updated in on_peer_message?**
   - What we know: It's used by PeerInfoRequest response (line 1570 for `connected_duration_ms`). It's also used by the existing inactivity loop.
   - What's unclear: If we remove inactivity_check_loop, is `last_message_time` still needed?
   - Recommendation: Keep updating `last_message_time` in `on_peer_message` for the PeerInfoRequest response. It serves a different purpose (reporting) than keepalive (liveness detection).

## Sources

### Primary (HIGH confidence)
- `db/net/connection.h` / `db/net/connection.cpp` -- Connection class, message_loop, send_message, Ping/Pong handling
- `db/peer/peer_manager.h` / `db/peer/peer_manager.cpp` -- PeerInfo, timer patterns, inactivity_check_loop, cancel_all_timers
- `db/tests/peer/test_event_expiry.cpp` -- PeerManager unit test pattern (most recent timer test)
- `db/tests/net/test_connection.cpp` -- Ping/Pong send queue test (line 582)
- `db/config/config.h` -- inactivity_timeout_seconds field

### Secondary (MEDIUM confidence)
- Asio steady_timer documentation -- timer-cancel pattern is well-established in this codebase

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, using existing Asio + Catch2
- Architecture: HIGH -- all patterns established in 8 prior timer coroutines
- Pitfalls: HIGH -- all identified from direct code reading of existing Ping/Pong handling and inactivity detection

**Research date:** 2026-04-04
**Valid until:** 2026-05-04 (stable codebase, no external dependency changes expected)
