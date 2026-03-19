# Phase 40: Sync Rate Limiting - Research

**Researched:** 2026-03-19
**Domain:** Rate limiting / abuse prevention for sync protocol messages (C++20, Asio coroutines)
**Confidence:** HIGH

## Summary

Phase 40 closes a concrete abuse vector: sync messages (SyncRequest, ReconcileInit/Ranges/Items, BlobRequest, BlobTransfer, SyncComplete) currently bypass the Phase 18 per-peer token bucket that meters Data/Delete messages. A malicious peer can initiate sync arbitrarily fast, consuming CPU (hash collection, reconciliation) and bandwidth without limit.

The implementation is well-scoped because all needed infrastructure exists. The token bucket (`try_consume_tokens`) in `peer_manager.cpp:39-56`, the PeerInfo struct with per-peer state, the SIGHUP reload path in `reload_config()`, the Config/load_config JSON parsing, and the FlatBuffers transport schema are all established patterns. The new work is: (1) a new SyncRejected wire message type, (2) sync cooldown tracking per peer, (3) extending the existing byte-rate bucket to cover sync messages, (4) concurrent session limiting (trivially extends the existing `syncing` bool), and (5) mid-sync byte budget checks at namespace boundaries.

**Primary recommendation:** Implement in two plans -- Plan 01 for wire protocol + config + PeerInfo changes, Plan 02 for enforcement logic + tests. All changes are in peer_manager.{h,cpp}, config.{h,cpp}, transport.fbs, and test_peer_manager.cpp.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Sync cooldown and concurrent session violations: reject and keep connection alive (NOT disconnect)
- New wire message type SyncRejected (type 30) with reason byte (cooldown, session limit, byte rate)
- Peer can still send Data/Delete after sync rejection -- only sync is refused
- Rejection is metering, NOT misbehavior -- does not feed into strike system
- Separate metrics counter for sync rejections (not reused from rate_limited)
- Sync message bytes count against the existing per-peer token bucket (shared with Data/Delete)
- Both initiator and responder account sync bytes against the bucket (symmetric enforcement)
- Check byte budget at namespace boundaries -- current namespace reconciliation completes fully, but no new namespace starts if budget exceeded
- When sync is cut short due to byte budget, send SyncComplete early (not SyncRejected) -- peer sees a shorter sync, remaining namespaces sync next round
- Responder mid-sync cutoff: stop responding silently, let initiator hit SYNC_TIMEOUT (30s). No mid-stream SyncRejected complexity.
- Sync cooldown: 30 seconds default (allows sync twice per sync interval)
- Max concurrent sync sessions: 1 (keep serial, matches existing syncing bool pattern)
- Sync rate limiting enabled by default (cooldown=30, max_sessions=1) -- this is closing an abuse vector, not an opt-in feature
- Cooldown=0 disables sync cooldown (operator override)
- All sync rate limit params SIGHUP-reloadable (follows Phase 18 pattern)
- Sync rate limits are node-local operational config, same as rate_limit_bytes_per_sec, max_peers, max_storage_bytes

### Claude's Discretion
- Byte-rate exceed during sync: disconnect (Phase 18 pattern) or reject-and-continue
- SyncRejected payload structure beyond reason byte
- Exact metrics counter naming and log verbosity
- PeerInfo fields for tracking cooldown timestamps and session count

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| RATE-01 | Sync initiation frequency limited per peer (configurable cooldown) | PeerInfo gets `last_sync_initiated` timestamp field; on_peer_message SyncRequest handler checks cooldown; SyncRejected(reason=cooldown) sent on violation; config adds `sync_cooldown_seconds` |
| RATE-02 | Sync messages included in per-peer byte-rate token bucket (extends existing rate limiter) | Existing `try_consume_tokens()` called for ALL message types (not just Data/Delete); sync byte accounting at namespace boundary for mid-sync cutoff; initiator checks budget before starting next namespace in run_sync_with_peer loop |
| RATE-03 | Concurrent sync sessions limited per peer (configurable max) | Config adds `max_sync_sessions` (default 1); existing `syncing` bool already enforces this -- extend with SyncRejected response instead of silent ignore; counter field not needed when max=1 (bool suffices) |
</phase_requirements>

## Standard Stack

### Core (no new dependencies)

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest | Coroutines, timers, io_context | Already in project, all async code uses this |
| FlatBuffers | 25.2.10 | Wire protocol schema (transport.fbs) | Already in project, all message types defined here |
| Catch2 | latest | Testing | Already in project, all tests use this |
| spdlog | latest | Logging | Already in project |
| nlohmann/json | latest | Config parsing | Already in project |

### No new libraries needed
This phase is purely extending existing infrastructure. No external dependencies.

## Architecture Patterns

### Recommended Change Structure

```
db/
  schemas/transport.fbs          # Add SyncRejected = 30
  wire/transport_generated.h     # Re-generate from schema (flatc)
  config/config.h                # Add sync_cooldown_seconds, max_sync_sessions
  config/config.cpp              # Parse new JSON fields, defaults
  peer/peer_manager.h            # PeerInfo: last_sync_initiated; NodeMetrics: sync_rejections
  peer/peer_manager.cpp          # Enforcement logic in on_peer_message + run_sync_with_peer + handle_sync_as_responder
  tests/peer/test_peer_manager.cpp  # New test cases
```

### Pattern 1: SyncRejected Wire Message

**What:** New TransportMsgType value (30) with a reason-byte payload.
**When to use:** When rejecting sync initiation due to cooldown, session limit, or byte rate.

```cpp
// transport.fbs: add after ReconcileItems = 29
SyncRejected = 30

// Payload format: [reason:1]
// reason values:
//   0x01 = cooldown (sync too frequent)
//   0x02 = session_limit (max concurrent sessions)
//   0x03 = byte_rate (byte budget exceeded pre-sync)
```

The payload is minimal -- just one byte. This is intentional: SyncRejected is a soft signal, not a protocol negotiation message. The peer just waits and tries again. Optional: include a 4-byte cooldown_remaining_seconds for the cooldown reason, but this is Claude's discretion.

### Pattern 2: Cooldown Check (Step 0 Pattern)

**What:** Check sync cooldown BEFORE spawning responder coroutine.
**Where:** `on_peer_message()` when `type == SyncRequest` (line 370-380 in peer_manager.cpp).

```cpp
// Current code (line 370-380):
if (type == wire::TransportMsgType_SyncRequest) {
    auto* peer = find_peer(conn);
    if (peer && !peer->syncing) {
        peer->sync_inbox.clear();
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await handle_sync_as_responder(conn);
        }, asio::detached);
    }
    return;
}

// New code -- check cooldown FIRST (Step 0), then session limit:
if (type == wire::TransportMsgType_SyncRequest) {
    auto* peer = find_peer(conn);
    if (!peer) return;

    // Step 0: Sync cooldown check (cheapest first)
    if (sync_cooldown_seconds_ > 0) {
        auto now_ms = steady_clock_ms_now();
        auto elapsed_ms = now_ms - peer->last_sync_initiated;
        if (elapsed_ms < static_cast<uint64_t>(sync_cooldown_seconds_) * 1000) {
            // Reject: cooldown not elapsed
            send_sync_rejected(conn, SyncRejectReason::Cooldown);
            ++metrics_.sync_rejections;
            return;
        }
    }

    // Step 1: Session limit check
    if (peer->syncing) {
        send_sync_rejected(conn, SyncRejectReason::SessionLimit);
        ++metrics_.sync_rejections;
        return;
    }

    // Update cooldown timestamp (AFTER checks pass)
    peer->last_sync_initiated = steady_clock_ms_now();
    peer->sync_inbox.clear();
    asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
        co_await handle_sync_as_responder(conn);
    }, asio::detached);
    return;
}
```

Critical: Update `last_sync_initiated` only AFTER checks pass. If updated before checking, a rejected sync would reset the cooldown.

### Pattern 3: Byte Budget Check at Namespace Boundary

**What:** Before starting reconciliation for the next namespace, check if peer has enough byte budget.
**Where:** The `for (const auto& ns : all_namespaces)` loop in `run_sync_with_peer()` (line 743) and the `while (true)` ReconcileInit loop in `handle_sync_as_responder()` (line 1124).

For the **initiator** (`run_sync_with_peer`):
```cpp
// Before each namespace iteration in the Phase B loop:
if (rate_limit_bytes_per_sec_ > 0) {
    // Peek at bucket without consuming -- if nearly empty, stop early
    // (actual consumption happens per-message in on_peer_message)
    if (peer->bucket_tokens == 0) {
        spdlog::info("sync with {}: byte budget exhausted, sending SyncComplete early",
                     conn->remote_address());
        break;  // Break out of namespace loop, fall through to SyncComplete
    }
}
```

For the **responder** (`handle_sync_as_responder`):
- The responder cannot proactively break -- it responds to initiator-driven ReconcileInit messages.
- Per CONTEXT.md decision: "Responder mid-sync cutoff: stop responding silently, let initiator hit SYNC_TIMEOUT (30s)."
- So the responder just stops sending responses. The initiator's `recv_sync_msg()` with 30s timeout will time out.
- This is the simplest approach and avoids mid-stream SyncRejected complexity.

### Pattern 4: Sync Byte Accounting in on_peer_message

**What:** Extend the existing rate limit check (currently lines 444-457) to cover ALL message types, not just Data/Delete.
**Current code:**
```cpp
// Line 444-457: Only Data/Delete are rate-checked
if ((type == wire::TransportMsgType_Data || type == wire::TransportMsgType_Delete) &&
    rate_limit_bytes_per_sec_ > 0) {
    auto* peer = find_peer(conn);
    if (peer && !try_consume_tokens(*peer, payload.size(),
                                     rate_limit_bytes_per_sec_, rate_limit_burst_)) {
        ++metrics_.rate_limited;
        spdlog::warn("rate limit exceeded...");
        asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
        return;
    }
}
```

**New approach:** Move rate check to top of `on_peer_message` before ANY message-type dispatch. All message types consume bucket tokens. The behavior on exceed depends on message type:
- For Data/Delete: disconnect (existing Phase 18 behavior, must NOT change)
- For sync messages during active sync: responder silently stops (per CONTEXT.md)
- For SyncRequest before sync starts: send SyncRejected with byte_rate reason

**Recommended discretion choice:** For byte-rate exceed during active sync, use disconnect (Phase 18 pattern). Reason: if a peer is exceeding the byte rate limit during sync, they are pushing more data than the bucket allows. Disconnecting is simpler and consistent. The "reject-and-continue" option adds complexity (tracking mid-sync byte violations) for minimal benefit -- the peer can reconnect and will get a fresh bucket.

### Pattern 5: SIGHUP Reload

**What:** Add sync rate limit params to the reload path.
**Where:** `reload_config()` in peer_manager.cpp (after line 1536).

```cpp
// After existing rate_limit reload (line 1528-1536):
sync_cooldown_seconds_ = new_cfg.sync_cooldown_seconds;
// max_sync_sessions_ not needed as member if always 1 -- but keep as member for future
if (sync_cooldown_seconds_ > 0) {
    spdlog::info("config reload: sync_cooldown={}s", sync_cooldown_seconds_);
} else {
    spdlog::info("config reload: sync_cooldown=disabled");
}
```

### Anti-Patterns to Avoid

- **Checking cooldown AFTER co_spawn:** The cooldown check MUST happen in `on_peer_message()` synchronously, before spawning the responder coroutine. If checked inside the coroutine, there is a race where multiple SyncRequests arrive before any coroutine runs.
- **Using wall clock for cooldown:** Use `steady_clock` (like the token bucket), not `std::time()`. Wall clock can jump (NTP, manual set).
- **Resetting cooldown on rejection:** Only update `last_sync_initiated` when the sync is ACCEPTED, not when rejected. Otherwise a rejected peer resets their own cooldown.
- **Consuming bytes for Ping/Pong in Connection:** Ping/Pong are handled inside `Connection::message_loop()` and never reach `on_peer_message`. They are NOT affected by this change. Only messages dispatched via `message_cb_` go through on_peer_message.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Token bucket | Custom bucket | Existing `try_consume_tokens()` | Already correct, handles overflow, tested |
| Config parsing | Manual JSON | Existing `load_config()` pattern | Just add `j.value("sync_cooldown_seconds", ...)` |
| SIGHUP reload | New signal handler | Existing `reload_config()` path | Add fields to existing function |
| Wire message | Raw byte encoding | FlatBuffers `transport.fbs` | Schema drives code generation |

## Common Pitfalls

### Pitfall 1: Byte Accounting Location
**What goes wrong:** Accounting sync bytes inside the sync coroutine instead of `on_peer_message`.
**Why it happens:** It seems natural to account bytes where they are processed.
**How to avoid:** Account in `on_peer_message` -- this is the single chokepoint where ALL messages pass through after `Connection::message_loop()` dispatches them. The sync coroutines receive messages via `route_sync_message` -> `sync_inbox` -> `recv_sync_msg`, which is AFTER `on_peer_message`. Accounting at the entry point ensures nothing bypasses it.
**Warning signs:** Sync messages appearing in wireshark but not counted in bucket.

### Pitfall 2: Rate Check Position in on_peer_message
**What goes wrong:** Placing rate check after sync message routing (lines 400-413) means sync messages are already queued before the check fires.
**Why it happens:** The current code structure checks Data/Delete rate at line 444, after sync routing at 370-413.
**How to avoid:** Move the byte-accounting check to the TOP of `on_peer_message`, before ANY message dispatch. The check should consume tokens from the bucket but NOT reject -- it just reduces the bucket. The actual rejection behavior depends on the message type (which is handled later). Alternatively, check for all types at the top, and for sync types, just consume tokens without disconnect action.
**Correct approach:** Consume tokens for ALL messages at top. If bucket empty AND message is Data/Delete: disconnect. If bucket empty AND message is SyncRequest: send SyncRejected. If bucket empty AND message is mid-sync (ReconcileInit etc.): route it anyway (responder will silently stop at namespace boundary per CONTEXT.md).

### Pitfall 3: FlatBuffers Enum Gap
**What goes wrong:** The TransportMsgType enum has a gap at value 12 (formerly HashList, now removed). Adding SyncRejected=30 is fine, but the EnumValuesTransportMsgType array and EnumNamesTransportMsgType array sizes must update correctly.
**Why it happens:** FlatBuffers generates arrays indexed by enum value, with empty strings for gaps.
**How to avoid:** Just add `SyncRejected = 30` to the schema and re-run flatc. The generated code handles gaps automatically. But DO check that the generated MAX value updates to `TransportMsgType_SyncRejected`.

### Pitfall 4: Initiator-Side SyncRejected Handling
**What goes wrong:** The initiator sends SyncRequest and waits for SyncAccept (line 648). If the responder sends SyncRejected instead, `recv_sync_msg` returns a message with type SyncRejected, not SyncAccept. The current code (line 649) just checks `accept_msg->type != TransportMsgType_SyncAccept` and logs "no SyncAccept received" + returns. This works but could be more informative.
**How to avoid:** After checking for SyncAccept, add an explicit check for SyncRejected:
```cpp
if (accept_msg->type == wire::TransportMsgType_SyncRejected) {
    spdlog::info("sync with {}: rejected (reason={})", ...);
    peer->syncing = false;
    co_return;
}
```
This gives operators better log visibility into why syncs are failing.

### Pitfall 5: Double-Counting Bytes
**What goes wrong:** Bytes are counted both in `on_peer_message` (when routing) and inside the sync coroutine.
**Why it happens:** Defensive over-accounting.
**How to avoid:** Account bytes ONCE in `on_peer_message`. The sync coroutine should only check the budget at namespace boundaries (peek at `bucket_tokens` without consuming). Never consume tokens inside the coroutine.

### Pitfall 6: Initiator Cooldown Confusion
**What goes wrong:** The cooldown is for INCOMING sync requests (responder side). The initiator side (`run_sync_with_peer`) is timer-driven (sync_interval_seconds). Don't add cooldown checks on the initiator side.
**Why it happens:** Conflating "how often we sync" with "how often a peer can ask us to sync".
**How to avoid:** Cooldown check is ONLY in `on_peer_message` for `SyncRequest` type. The initiator already has `sync_interval_seconds` as its natural rate limiter.

## Code Examples

### SyncRejected Helper Function
```cpp
// In anonymous namespace or as PeerManager private method:
enum class SyncRejectReason : uint8_t {
    Cooldown = 0x01,
    SessionLimit = 0x02,
    ByteRate = 0x03
};

void PeerManager::send_sync_rejected(net::Connection::Ptr conn, SyncRejectReason reason) {
    std::vector<uint8_t> payload = { static_cast<uint8_t>(reason) };
    asio::co_spawn(ioc_, [conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        co_await conn->send_message(wire::TransportMsgType_SyncRejected,
                                     std::span<const uint8_t>(payload));
    }, asio::detached);
}
```

### PeerInfo Extensions
```cpp
struct PeerInfo {
    // ... existing fields ...

    // Phase 40: Sync rate limiting
    uint64_t last_sync_initiated = 0;  // steady_clock ms since epoch (0 = never synced)
};
```

### Config Extensions
```cpp
struct Config {
    // ... existing fields ...

    uint32_t sync_cooldown_seconds = 30;   // Min time between incoming sync requests per peer (0 = disabled)
    uint32_t max_sync_sessions = 1;        // Max concurrent sync sessions per peer
};
```

### Config Loading Extension
```cpp
// In load_config():
cfg.sync_cooldown_seconds = j.value("sync_cooldown_seconds", cfg.sync_cooldown_seconds);
cfg.max_sync_sessions = j.value("max_sync_sessions", cfg.max_sync_sessions);
```

### PeerManager Member Extensions
```cpp
// In PeerManager private:
uint32_t sync_cooldown_seconds_ = 30;     // SIGHUP-reloadable
// max_sync_sessions_ = 1 is the default. If it stays 1, the existing
// syncing bool is sufficient. Keep as member for SIGHUP reload.
uint32_t max_sync_sessions_ = 1;

// Helper
void send_sync_rejected(net::Connection::Ptr conn, SyncRejectReason reason);
```

### Metrics Extension
```cpp
struct NodeMetrics {
    // ... existing fields ...
    uint64_t sync_rejections = 0;    // Sync rate limit rejections (cooldown + session limit)
};
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Sync messages bypass rate limiter | Sync messages share token bucket | Phase 40 (now) | Closes bandwidth abuse vector |
| Silent ignore on duplicate SyncRequest | SyncRejected message with reason byte | Phase 40 (now) | Peers get clear feedback |
| No sync initiation cooldown | 30s default cooldown per peer | Phase 40 (now) | Prevents CPU exhaustion via repeated sync |

## Open Questions

1. **SyncRejected payload -- include remaining cooldown?**
   - What we know: Reason byte is locked (CONTEXT.md). Beyond that is discretion.
   - What's unclear: Whether including uint32_be remaining_cooldown_seconds is worth the complexity.
   - Recommendation: Keep minimal (reason byte only). The peer's sync_interval_seconds is typically 60s, so it will just try again next round. Adding remaining_cooldown adds protocol complexity for zero practical benefit.

2. **Byte-rate exceed during active sync: disconnect or reject-and-continue?**
   - What we know: Claude's discretion per CONTEXT.md. Responder mid-sync cutoff is "stop responding silently" (locked).
   - Recommendation: **Disconnect** (Phase 18 pattern). Simpler, consistent, and a peer exceeding byte rate during sync is genuinely misbehaving. The "silent stop" decision is for the responder choosing to stop; a byte-rate violation is the peer sending too much.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (latest via FetchContent) |
| Config file | db/tests/ directory, CMakeLists.txt test targets |
| Quick run command | `cd build && ctest -R test_peer_manager --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| RATE-01 | Sync cooldown rejects too-frequent SyncRequest | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | Will be added in Wave 0 |
| RATE-01 | Cooldown=0 disables sync cooldown | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | Will be added in Wave 0 |
| RATE-02 | Sync messages consume token bucket bytes | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | Will be added in Wave 0 |
| RATE-02 | Mid-sync byte budget cutoff sends SyncComplete early | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | Will be added in Wave 0 |
| RATE-03 | Concurrent sync request rejected with SyncRejected | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | Will be added in Wave 0 |
| RATE-03 | Existing test "sync traffic not rate-limited" still passes | regression | `cd build && ctest -R test_peer_manager --output-on-failure` | Exists (line 1475) |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . && ctest -R test_peer_manager --output-on-failure`
- **Per wave merge:** `cd build && cmake --build . && ctest --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] New test cases in `db/tests/peer/test_peer_manager.cpp` -- covers RATE-01, RATE-02, RATE-03
- [ ] Existing tests still pass after rate check restructuring (regression)
- No new test files needed -- extend existing test file
- No new framework or fixtures needed -- reuse TempDir, make_signed_blob, etc.

## Sources

### Primary (HIGH confidence)
- Source code: `db/peer/peer_manager.h` (line 45-76: PeerInfo, NodeMetrics structs)
- Source code: `db/peer/peer_manager.cpp` (line 39-56: try_consume_tokens; line 366-457: on_peer_message; line 631-1031: run_sync_with_peer; line 1033-1412: handle_sync_as_responder; line 1491-1590: reload_config)
- Source code: `db/config/config.h` (line 14-38: Config struct)
- Source code: `db/config/config.cpp` (line 9-97: load_config)
- Source code: `db/schemas/transport.fbs` (line 1-48: wire protocol schema)
- Source code: `db/net/connection.h` (line 53-54: send_message signature)
- Source code: `db/tests/peer/test_peer_manager.cpp` (line 1475-1677: existing rate limit tests)
- CONTEXT.md: `.planning/phases/40-sync-rate-limiting/40-CONTEXT.md` (all locked decisions)

### Secondary (MEDIUM confidence)
- N/A -- all findings are from direct codebase inspection

### Tertiary (LOW confidence)
- N/A

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all existing infrastructure
- Architecture: HIGH - direct codebase inspection, clear patterns to follow
- Pitfalls: HIGH - identified from reading actual code paths and understanding coroutine dispatch

**Research date:** 2026-03-19
**Valid until:** No expiry (codebase-specific research, not library version dependent)
