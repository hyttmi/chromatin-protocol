# Phase 82: Reconcile-on-Connect & Safety Net - Research

**Researched:** 2026-04-04
**Domain:** Peer sync lifecycle, cursor management, timer-cancel coroutines (C++20 / Asio)
**Confidence:** HIGH

## Summary

Phase 82 implements three related behaviors: (1) full XOR-fingerprint reconciliation runs automatically when a peer connects, (2) a safety-net periodic reconciliation replaces the old sync timer at a 600s default interval, and (3) disconnected peer cursors are preserved with a 5-minute grace period for reconnect reuse. All three touch `PeerManager` and its timer/coroutine infrastructure.

The existing codebase provides nearly all the building blocks. `run_sync_with_peer()` already performs full reconciliation. `sync_timer_loop()` already runs periodic syncs. `on_peer_connected()` already spawns `run_sync_with_peer()` for initiator connections. The cursor compaction loop (`cursor_compaction_loop()`) runs every 6 hours. The work is mostly restructuring existing code: (a) rename/replace config field, (b) add a disconnect-time cursor preservation map, (c) modify the reconnect path to check preserved cursors, (d) change the cursor compaction loop to also clean stale preserved cursors.

**Primary recommendation:** Adapt existing patterns (timer-cancel coroutine, PeerInfo syncing flag, storage cursor API) rather than introducing new abstractions. The total new code should be under 200 lines.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Preserve SyncProtocol cursor state per namespace when a peer disconnects. Store in a map keyed by peer pubkey hash with disconnect timestamp. On reconnect within 5 minutes, restore cursors so sync resumes from last position instead of full reconciliation.
- **D-02:** Lazy cleanup on reconnect check. Store disconnect timestamp with cursor state. On reconnect, check if within 5 min -- reuse or discard. Periodically scan (e.g. during safety-net cycle) to free stale entries. No per-peer timer.
- **D-03:** Sync all peers each cycle. Every 600s, run sync_all_peers() as currently implemented. Simple, complete, long enough that cost is negligible.
- **D-04:** Bypass cooldown for safety net. Safety-net sync always runs regardless of cooldown. Cooldown exists to prevent rapid-fire from aggressive peers, not from the node's own safety net.
- **D-05:** Use existing syncing flag to gate BlobNotify. Phase 79's on_blob_notify handler already suppresses BlobNotify to peers with syncing=true (storm suppression). Sync-on-connect naturally gates BlobNotify -- no new mechanism needed.
- **D-06:** Keep initiator-only sync-on-connect. Current pattern: initiator sends SyncRequest, responder handles it. No change to the existing pattern.
- **D-07:** Rename in-place, no backward compat. Replace `sync_interval_seconds` with `safety_net_interval_seconds` in config.h. Default 600s. Old configs with the old field name are silently ignored. Pre-MVP: no backward compat needed.

### Claude's Discretion
- Data structure for storing disconnected peer cursor state (map type, what exactly the cursor contains)
- How cursor state is extracted from SyncProtocol on disconnect (snapshot method or direct copy)
- How cursor state is restored on reconnect (SyncProtocol setter or constructor parameter)
- Whether the safety-net loop is the same coroutine as the old sync_timer_loop or a new one
- How stale cursor entries are cleaned up during the periodic scan
- Validation rules for safety_net_interval_seconds (minimum, maximum)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| MAINT-04 | Peer cursors are compacted immediately when a peer disconnects (with 5-minute grace period for transient disconnects) | Cursor preservation map in PeerManager with disconnect timestamp; lazy cleanup in safety-net cycle; existing `cleanup_stale_cursors()` API in Storage |
| MAINT-05 | Full reconciliation runs on peer connect/reconnect (catch-up path) | `on_peer_connected()` already spawns `run_sync_with_peer()` for initiator connections; add grace-period cursor check before sync dispatch |
| MAINT-06 | Safety-net reconciliation runs at a long interval (default 600s) as a monitoring signal | Adapt `sync_timer_loop()` to use `safety_net_interval_seconds`; bypass cooldown per D-04 |
| MAINT-07 | sync_interval_seconds config field repurposed to safety_net_interval_seconds with 600s default | Rename in config.h/config.cpp, update known_keys set, update validate_config, update all references |
</phase_requirements>

## Architecture Patterns

### Current Code Structure (relevant to this phase)

```
db/
├── config/
│   ├── config.h          # Config struct -- sync_interval_seconds (to rename)
│   └── config.cpp        # JSON parsing, known_keys set, validate_config()
├── peer/
│   ├── peer_manager.h    # PeerInfo, PeerManager class, timer pointers
│   └── peer_manager.cpp  # on_peer_connected, on_peer_disconnected, sync_timer_loop,
│                         # sync_all_peers, run_sync_with_peer, cursor_compaction_loop
├── storage/
│   ├── storage.h         # SyncCursor struct, get/set/delete_sync_cursor, cleanup_stale_cursors
│   └── storage.cpp       # MDBX cursor persistence implementation
└── tests/
    ├── peer/test_peer_manager.cpp  # 4260 lines, comprehensive peer manager tests
    ├── peer/test_event_expiry.cpp  # Event-driven expiry tests (Phase 81 template)
    └── config/test_config.cpp      # Config loading/validation tests
```

### Pattern 1: Timer-Cancel Coroutine (established)

Every periodic task in PeerManager follows this exact pattern. The safety-net loop and cursor cleanup should follow the same pattern.

```cpp
// Source: db/peer/peer_manager.cpp line 2633-2645
asio::awaitable<void> PeerManager::sync_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        sync_timer_ = &timer;   // Expose for cancel_all_timers()
        timer.expires_after(std::chrono::seconds(config_.sync_interval_seconds));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        sync_timer_ = nullptr;
        if (ec || stopping_) co_return;

        co_await sync_all_peers();
    }
}
```

### Pattern 2: Initiator-Only Sync on Connect (established)

```cpp
// Source: db/peer/peer_manager.cpp line 461-465
// Only the initiator (outbound) side triggers sync on connect.
if (conn->is_initiator()) {
    asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
        co_await run_sync_with_peer(conn);
    }, asio::detached);
}
```

### Pattern 3: Cursor Compaction (existing, to be adapted)

```cpp
// Source: db/peer/peer_manager.cpp line 3642-3665
// Current: runs every 6 hours, calls storage_.cleanup_stale_cursors()
// Phase 82: adapt for 5-minute grace period on disconnect, plus periodic stale cleanup
```

### Recommended Design: Disconnected Cursor State Map

Per D-01/D-02, store disconnected cursor state in PeerManager, NOT in Storage. The cursors in Storage (MDBX) are per-peer per-namespace and persist across restarts. The grace-period map is transient (in-memory only) and tracks whether a recently-disconnected peer's cursors should be reused or discarded.

```cpp
// New member in PeerManager (peer_manager.h)
struct DisconnectedPeerState {
    uint64_t disconnect_time;  // steady_clock ms since epoch
    // No cursor data needed -- cursors already persist in Storage (MDBX)
    // We just need to know WHEN the peer disconnected to decide:
    // - within 5 min: skip full reconciliation (cursors still valid in Storage)
    // - after 5 min: discard cursors, run full reconciliation
};

// Map: peer pubkey hash -> disconnect timestamp
std::unordered_map<std::array<uint8_t, 32>, DisconnectedPeerState, ArrayHash32>
    disconnected_peers_;

static constexpr uint64_t CURSOR_GRACE_PERIOD_MS = 5 * 60 * 1000;  // 5 minutes
```

**Key insight:** The actual SyncCursor data already lives in MDBX (`storage_.get_sync_cursor()`). We do NOT need to copy cursor data out of storage on disconnect. We only need to track WHEN a peer disconnected, so on reconnect we can decide: "are the stored cursors still fresh enough to reuse?"

### Pattern 4: Connect-Time Cursor Reuse Logic

On `on_peer_connected()`, before spawning `run_sync_with_peer()`:

```cpp
// Check if this peer recently disconnected (within grace period)
auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
auto it = disconnected_peers_.find(peer_hash);
if (it != disconnected_peers_.end()) {
    auto now_ms = /* steady_clock ms */;
    if (now_ms - it->second.disconnect_time <= CURSOR_GRACE_PERIOD_MS) {
        // Cursors in Storage are still valid -- full reconciliation
        // will naturally use them (cursor_skip_namespaces logic in run_sync_with_peer)
        spdlog::info("peer {} reconnected within grace period, cursors preserved",
                     ns_hex);
    } else {
        // Grace period expired -- delete cursors from Storage
        storage_.cleanup_peer_cursors(peer_hash);  // or iterate and delete
        spdlog::info("peer {} reconnected after grace period, cursors discarded",
                     ns_hex);
    }
    disconnected_peers_.erase(it);
}
```

**Important subtlety:** `run_sync_with_peer()` already checks cursors via `storage_.get_sync_cursor()` and decides whether to skip namespaces. If cursors exist and seq_num matches, the namespace is skipped. So cursor reuse is automatic -- we just need to NOT delete cursors within the grace period. The existing `cursor_compaction_loop()` runs every 6 hours and only removes cursors for peers not in the connected set. We need to also preserve cursors for recently-disconnected peers.

### Pattern 5: Safety-Net Cooldown Bypass (D-04)

Currently, `sync_all_peers()` iterates peers and calls `run_sync_with_peer()` for each. The cooldown check is in `handle_sync_as_responder()` (responder side), not in the initiator's `run_sync_with_peer()`. Looking at the responder code:

```cpp
// Source: db/peer/peer_manager.cpp line ~587-621
// Step 1: Cooldown check
auto now_ms = /* steady_clock ms */;
uint64_t elapsed_ms = now_ms - peer->last_sync_initiated;
if (sync_cooldown_seconds_ > 0 && elapsed_ms < (sync_cooldown_seconds_ * 1000ULL)) {
    send_sync_rejected(conn, SYNC_REJECT_COOLDOWN);
    ++metrics_.sync_rejections;
    peer->syncing = false;
    co_return;
}
```

The cooldown is per-peer on the responder side. When the safety-net initiates sync (via `sync_all_peers()` -> `run_sync_with_peer()`), the initiator sends SyncRequest. The REMOTE peer's responder may reject due to cooldown. With a 600s safety-net interval and 30s cooldown, this is never triggered. D-04 says "Safety-net sync always runs regardless of cooldown" -- this is already naturally satisfied because 600s >> 30s cooldown. No code change needed for D-04 unless the safety-net interval is configured below the cooldown (which validation should prevent or warn about).

### Anti-Patterns to Avoid

- **Per-peer timer for grace period:** D-02 explicitly forbids this. Use lazy cleanup instead.
- **Copying cursor data to memory on disconnect:** Cursors already persist in MDBX. Only track disconnect timestamp.
- **New coroutine for safety-net:** Adapt the existing `sync_timer_loop()` instead (D-03 says "run sync_all_peers() as currently implemented").
- **Modifying run_sync_with_peer for grace period:** The cursor logic already works -- just control when cursors get deleted.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Cursor persistence | Custom file storage | `storage_.get/set/delete_sync_cursor()` | Already MDBX-backed, transactional |
| Peer hash computation | Manual hashing | `crypto::sha3_256(conn->peer_pubkey())` | Consistent with existing code |
| Timer management | Raw timer handling | Timer-cancel pattern (sync_timer_ pointer + cancel_all_timers) | Established project pattern, clean shutdown |
| Cursor cleanup for disconnected peers | Manual MDBX iteration | `storage_.cleanup_stale_cursors(known_hashes)` | Already implemented, takes known-good peer list |

## Common Pitfalls

### Pitfall 1: Deque invalidation during coroutine co_await
**What goes wrong:** `peers_` is a deque. Any insert/erase during a co_await invalidates iterators and pointers to PeerInfo.
**Why it happens:** `run_sync_with_peer()` contains many co_await points. During any of them, `on_peer_connected`/`on_peer_disconnected` can modify `peers_`.
**How to avoid:** Always re-lookup peer via `find_peer(conn)` after any co_await. The existing code already does this.
**Warning signs:** Using `PeerInfo*` across a co_await boundary.

### Pitfall 2: Race between disconnect and grace-period cursor deletion
**What goes wrong:** If `cursor_compaction_loop()` runs and doesn't know about the grace-period map, it deletes cursors for a recently-disconnected peer that might reconnect.
**Why it happens:** The existing compaction loop only checks the currently-connected set.
**How to avoid:** When building the "known peer hashes" set for `cleanup_stale_cursors()`, include both connected peers AND recently-disconnected peers from `disconnected_peers_` map.
**Warning signs:** Cursor compaction logging "removed N entries" for a peer that reconnects seconds later.

### Pitfall 3: Config rename breaks existing tests
**What goes wrong:** Tests that reference `sync_interval_seconds` (config validation, reload, E2E) will fail after the rename.
**Why it happens:** The field name change touches config.h, config.cpp, peer_manager.cpp, test_config.cpp, and test_peer_manager.cpp.
**How to avoid:** Search comprehensively for all occurrences. Update config validation to use the new name. Update the known_keys set.
**Warning signs:** Failing config validation tests, unknown-key warnings in logs.

### Pitfall 4: Safety-net timer reads stale config value
**What goes wrong:** If `sync_timer_loop()` reads `config_.sync_interval_seconds` (renamed to `safety_net_interval_seconds`), SIGHUP reload won't update it because Config is const-ref.
**Why it happens:** PeerManager stores `const config::Config& config_` which is immutable. Mutable copies are made for reloadable fields (e.g., `sync_cooldown_seconds_`).
**How to avoid:** Add a mutable `safety_net_interval_seconds_` member to PeerManager (like `sync_cooldown_seconds_`). Initialize from config in constructor. Update in `reload_config()`.
**Warning signs:** Safety-net interval doesn't change after SIGHUP.

### Pitfall 5: on_blob_ingested sends BlobNotify during sync-on-connect
**What goes wrong:** While a newly-connected peer is still reconciling, the local node receives a blob from another peer and sends BlobNotify to the syncing peer. The BlobNotify arrives mid-sync and could interfere.
**Why it happens:** `on_blob_ingested()` does NOT check `peer->syncing` before sending BlobNotify to TCP peers.
**How to avoid:** The RECEIVING peer's `on_blob_notify()` handler (line 3058) checks `peer->syncing` and suppresses the notification. This is correct -- the sender sends, the receiver discards. No change needed, but document this asymmetry.
**Warning signs:** None -- current design handles this correctly. Just be aware of the asymmetry.

### Pitfall 6: steady_clock vs system_clock for grace period
**What goes wrong:** Mixing clock types causes incorrect grace-period calculations.
**Why it happens:** PeerInfo uses steady_clock for `bucket_last_refill` and `last_message_time`, but SyncCursor uses system_clock for `last_sync_timestamp`.
**How to avoid:** Use steady_clock for the grace-period disconnect timestamp (it's monotonic, not affected by NTP jumps). This is consistent with existing PeerInfo timing fields.
**Warning signs:** Grace period timing is affected by system clock adjustments.

## Code Examples

### Example 1: Modified on_peer_disconnected (cursor preservation)

```cpp
// Add to on_peer_disconnected, BEFORE removing from peers_:
void PeerManager::on_peer_disconnected(net::Connection::Ptr conn) {
    auto ns_hex = to_hex(conn->peer_pubkey(), 8);
    bool graceful = conn->received_goodbye();
    spdlog::info("Peer {} disconnected ({})", ns_hex,
                 graceful ? "graceful" : "timeout");

    // Phase 82: Preserve cursor state for grace period (MAINT-04)
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    disconnected_peers_[peer_hash] = DisconnectedPeerState{now_ms};

    // Phase 80: Clean pending BlobFetch entries...
    // ... existing cleanup code ...

    // Remove from peers
    peers_.erase(/* ... */);
    ++metrics_.peers_disconnected_total;
}
```

### Example 2: Modified on_peer_connected (grace period check)

```cpp
// In on_peer_connected, after dedup check and before spawning sync:
auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
bool cursor_reuse = false;
{
    auto it = disconnected_peers_.find(peer_hash);
    if (it != disconnected_peers_.end()) {
        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now_ms - it->second.disconnect_time <= CURSOR_GRACE_PERIOD_MS) {
            cursor_reuse = true;
            spdlog::info("peer {} reconnected within grace period, cursors preserved", ns_hex);
        } else {
            // Grace period expired -- cursors will be naturally handled by
            // check_full_resync() time gap detection, or stale cursor cleanup
            spdlog::info("peer {} reconnected after grace period", ns_hex);
        }
        disconnected_peers_.erase(it);
    }
}

// Initiator-only sync-on-connect (existing pattern -- always runs full reconciliation)
// When cursor_reuse is true, run_sync_with_peer's cursor_skip_namespaces logic
// will naturally skip namespaces where seq_num matches (no code change in sync logic)
if (conn->is_initiator()) {
    asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
        co_await run_sync_with_peer(conn);
    }, asio::detached);
}
```

### Example 3: Config field rename

```cpp
// config.h: Replace sync_interval_seconds with safety_net_interval_seconds
uint32_t safety_net_interval_seconds = 600;  // Default 600s (was sync_interval_seconds = 60)

// config.cpp: Update parsing
cfg.safety_net_interval_seconds = j.value("safety_net_interval_seconds",
                                           cfg.safety_net_interval_seconds);

// config.cpp: Update known_keys set
// Replace "sync_interval_seconds" with "safety_net_interval_seconds"

// config.cpp: Update validate_config
if (cfg.safety_net_interval_seconds < 60) {
    errors.push_back("safety_net_interval_seconds must be >= 60 (got " +
                      std::to_string(cfg.safety_net_interval_seconds) + ")");
}
```

### Example 4: Safety-net stale cursor cleanup

```cpp
// In sync_timer_loop (now safety-net loop), after sync_all_peers():
// D-02: Periodic scan to free stale disconnected peer entries
{
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    for (auto it = disconnected_peers_.begin(); it != disconnected_peers_.end(); ) {
        if (now_ms - it->second.disconnect_time > CURSOR_GRACE_PERIOD_MS) {
            it = disconnected_peers_.erase(it);
        } else {
            ++it;
        }
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| 60s periodic sync | Event-driven push + 600s safety net | v2.0.0 Phase 79-82 | 10x reduction in sync traffic; push-based propagation |
| 6h cursor compaction | Grace-period aware compaction | v2.0.0 Phase 82 | Preserves cursors for transient disconnects |
| sync_interval_seconds=60 | safety_net_interval_seconds=600 | v2.0.0 Phase 82 | Reflects new sync model: push is primary, periodic is backstop |

## Open Questions

1. **Should cursor_compaction_loop (6h) be removed or kept alongside grace-period cleanup?**
   - What we know: The 6h compaction loop removes cursors for peers no longer in the connected set. The grace-period cleanup handles the 5-minute window. The safety-net cycle also does stale entry cleanup per D-02.
   - What's unclear: Whether the 6h loop is still needed after adding the safety-net cleanup.
   - Recommendation: Keep the 6h loop. It handles a different concern: removing cursors for peers that have permanently disappeared from the connected and persisted peer sets. The grace-period cleanup only handles the `disconnected_peers_` in-memory map. The 6h loop handles the MDBX cursor database. They are complementary. However, the 6h loop must be updated to exclude peers in `disconnected_peers_` from deletion.

2. **Should safety_net_interval_seconds be SIGHUP-reloadable?**
   - What we know: Other interval fields (sync_cooldown_seconds, rate limits) are SIGHUP-reloadable. The sync_timer_loop reads `config_.sync_interval_seconds` directly (not a mutable copy).
   - What's unclear: Whether changing the interval at runtime is useful enough to warrant the extra code.
   - Recommendation: Yes, make it SIGHUP-reloadable. Add a mutable `safety_net_interval_seconds_` member. The timer re-reads it each iteration anyway (timer-cancel pattern creates a new timer each loop iteration).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (BUILD_TESTING section) |
| Quick run command | `cd build && ./db/chromatindb_tests "[safety-net]" --order rand` |
| Full suite command | `cd build && ./db/chromatindb_tests --order rand` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| MAINT-04 | Cursor preserved on disconnect, freed after grace period | unit | `./db/chromatindb_tests "cursor grace period expires and cursors freed"` | Wave 0 |
| MAINT-04 | Grace period cleanup during safety-net cycle | unit | `./db/chromatindb_tests "safety-net cycle cleans stale disconnected entries"` | Wave 0 |
| MAINT-05 | Full reconciliation on connect | unit (E2E two-node) | `./db/chromatindb_tests "sync-on-connect runs full reconciliation"` | Wave 0 |
| MAINT-05 | Cursor reuse on reconnect within grace period | unit (E2E two-node) | `./db/chromatindb_tests "reconnect within grace period reuses cursors"` | Wave 0 |
| MAINT-06 | Safety-net timer fires at 600s interval | unit | `./db/chromatindb_tests "safety-net timer interval"` | Wave 0 |
| MAINT-06 | Safety-net bypasses cooldown | unit | `./db/chromatindb_tests "safety-net bypasses cooldown"` | Wave 0 |
| MAINT-07 | Config field renamed, default 600s | unit | `./db/chromatindb_tests "Default config has safety_net_interval_seconds 600"` | Wave 0 |
| MAINT-07 | Config validation rejects < 60 | unit | `./db/chromatindb_tests "validate_config: safety_net_interval_seconds"` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ./db/chromatindb_tests "[safety-net]" --order rand`
- **Per wave merge:** `cd build && ./db/chromatindb_tests --order rand`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/peer/test_peer_manager.cpp` -- add test cases for MAINT-04 (grace period), MAINT-05 (sync-on-connect reconciliation, cursor reuse), MAINT-06 (safety-net timer, cooldown bypass)
- [ ] `db/tests/config/test_config.cpp` -- add test cases for MAINT-07 (renamed field, default value, validation)
- [ ] Update existing tests that reference `sync_interval_seconds` to use `safety_net_interval_seconds`

## Sources

### Primary (HIGH confidence)
- `db/peer/peer_manager.h` -- PeerInfo struct, timer pointers, ArrayHash32 functor
- `db/peer/peer_manager.cpp` -- on_peer_connected (line 314), on_peer_disconnected (line 468), sync_timer_loop (line 2633), sync_all_peers (line 2618), run_sync_with_peer (line 1797), cursor_compaction_loop (line 3642), on_blob_ingested (line 2986), on_blob_notify (line 3046)
- `db/config/config.h` -- Config struct with sync_interval_seconds field
- `db/config/config.cpp` -- JSON parsing, known_keys set, validate_config()
- `db/storage/storage.h` -- SyncCursor struct, get/set/delete_sync_cursor, cleanup_stale_cursors
- `db/sync/sync_protocol.h` -- SyncProtocol class (no cursor state to extract -- cursors are in Storage)
- `db/tests/peer/test_peer_manager.cpp` -- existing test patterns (4260 lines)
- `db/tests/peer/test_event_expiry.cpp` -- recent Phase 81 test template
- `db/tests/config/test_config.cpp` -- config validation test patterns

### Secondary (MEDIUM confidence)
- `.planning/phases/82-reconcile-on-connect-safety-net/82-CONTEXT.md` -- user decisions D-01 through D-07

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing code paths
- Architecture: HIGH -- verified by reading all relevant source files line-by-line
- Pitfalls: HIGH -- identified from real code patterns (deque invalidation, clock types, config const-ref)

**Research date:** 2026-04-04
**Valid until:** 2026-05-04 (stable internal codebase, no external dependency changes)
