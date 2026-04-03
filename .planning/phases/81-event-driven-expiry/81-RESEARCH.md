# Phase 81: Event-Driven Expiry - Research

**Researched:** 2026-04-03
**Domain:** Asio timer management, MDBX cursor queries, event-driven coroutine scheduling
**Confidence:** HIGH

## Summary

This phase replaces the periodic 60-second expiry scan timer with a precise event-driven timer that fires at exactly the earliest blob's expiry time. The implementation touches three well-understood areas of the codebase: (1) adding a Storage method to query the earliest expiry time via MDBX cursor, (2) rewriting the `expiry_scan_loop()` coroutine to use absolute-time-based rearming instead of fixed intervals, and (3) hooking into `on_blob_ingested()` to rearm the timer when a shorter-TTL blob arrives.

The existing codebase provides strong foundations. The `expiry_map` sub-database is already sorted by `[expiry_ts_be:8][hash:32]` -- big-endian encoding means the first cursor position IS the earliest expiry, giving O(1) queries. The timer-cancel pattern is used across 8+ coroutine loops in PeerManager. The `on_blob_ingested()` callback is the natural hook point for ingest-triggered rearming.

**Primary recommendation:** Add `Storage::get_earliest_expiry()` using read-only cursor, rewrite `expiry_scan_loop()` to compute duration from wall-clock expiry minus current time, and add expiry check in `on_blob_ingested()` by extending its signature with `uint64_t expiry_time` (0 = no expiry / TTL=0).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Use MDBX cursor query to determine the next earliest expiry time. O(1) with a sorted secondary index on expiry time. No in-memory state to maintain -- storage is the source of truth.
- **D-02:** If a blob is deleted/purged before its expiry timer fires, the timer fires anyway, finds nothing expired, and rearms to the next earliest. Simple, correct, tiny wasted wakeup. No need to cancel/rearm on delete.
- **D-03:** Check in on_blob_ingested (Phase 79 unified callback). After on_blob_ingested fires, compute the new blob's expiry and compare to the current timer target. If earlier, cancel and rearm. Reuses existing hook, no new callback needed.
- **D-04:** (Claude's Discretion) How to get the expiry time -- extend on_blob_ingested signature, query storage, or pass through IngestResult. Claude picks the least invasive approach.
- **D-05:** Process all expired blobs in one scan. When the timer fires, `run_expiry_scan()` purges ALL expired blobs (existing behavior). After the scan, query for next earliest expiry and rearm. Existing batch behavior with precise timing.
- **D-06:** Fully replace the periodic scan. Remove the periodic timer (`expiry_scan_loop()`) entirely. On startup, query storage for earliest expiry and set the initial timer. The `expiry_scan_interval_seconds` config option becomes unused.

### Claude's Discretion
- Storage API for querying earliest expiry time (new method on Storage or cursor-based)
- Whether `expiry_scan_interval_seconds` config is removed or deprecated (kept in struct but ignored)
- Timer state tracking (simple `uint64_t next_expiry_target_` vs more complex)
- Edge case: no blobs in storage (no timer armed, arm on first ingest)
- Edge case: all blobs have TTL=0 (never expire) -- timer stays disarmed

### Deferred Ideas (OUT OF SCOPE)
None.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| MAINT-01 | Expiry uses a next-expiry timer that fires at exactly the earliest blob's expiry time (O(1) via MDBX cursor) | `expiry_map` is sorted by `[expiry_ts_be:8][hash:32]` -- `cursor.to_first()` returns earliest expiry in O(1). New `Storage::get_earliest_expiry()` method using read-only transaction. |
| MAINT-02 | After processing an expired blob, timer rearms to the next earliest expiry (chain processing) | After `run_expiry_scan()` completes, call `get_earliest_expiry()` and rearm timer. If no more expiring blobs, timer stays disarmed. |
| MAINT-03 | Expiry timer rearms when a blob with an earlier expiry is ingested | Hook in `on_blob_ingested()`: compute expiry from ttl+timestamp, compare to `next_expiry_target_`, cancel and rearm timer if earlier. |
</phase_requirements>

## Architecture Patterns

### Current Expiry Flow (to be replaced)
```
PeerManager::start()
  -> co_spawn(expiry_scan_loop())
       -> loop:
            steady_timer.expires_after(60s)
            co_await timer.async_wait()
            storage_.run_expiry_scan()
```

### New Event-Driven Expiry Flow
```
PeerManager::start()
  -> query storage_.get_earliest_expiry()
  -> if found: co_spawn(expiry_scan_loop()) with initial target
  -> if empty: no timer armed (will arm on first ingest)

on_blob_ingested(... expiry_time ...)
  -> if expiry_time > 0 && (no timer OR expiry_time < next_expiry_target_):
       cancel current timer, set next_expiry_target_ = expiry_time

expiry_scan_loop():
  -> loop:
       compute duration = next_expiry_target_ - now()
       steady_timer.expires_after(duration)
       co_await timer.async_wait()
       if cancelled: check next_expiry_target_ (may have been rearmed)
       storage_.run_expiry_scan()
       query storage_.get_earliest_expiry()
       if found: set next_expiry_target_, continue loop
       if empty: co_return (will restart from on_blob_ingested)
```

### Pattern: Timer Rearm via Cancel
The codebase uses a consistent pattern for cancellable timers:
```cpp
// In coroutine loop:
asio::steady_timer timer(ioc_);
expiry_timer_ = &timer;            // Expose pointer for external cancel
timer.expires_after(duration);
auto [ec] = co_await timer.async_wait(
    asio::as_tuple(asio::use_awaitable));
expiry_timer_ = nullptr;
if (ec || stopping_) co_return;    // Cancelled or shutting down
```
When another path needs to rearm: `if (expiry_timer_) expiry_timer_->cancel();`
The coroutine resumes with `ec = operation_aborted`, loops back, recomputes target.

### Pattern: Wall Clock to Steady Timer Duration
Expiry timestamps are wall-clock seconds since epoch. Asio's `steady_timer` uses `steady_clock` durations. Conversion:
```cpp
uint64_t now = storage::system_clock_seconds();  // or storage_.clock()
if (target_expiry > now) {
    auto duration = std::chrono::seconds(target_expiry - now);
    timer.expires_after(duration);
} else {
    timer.expires_after(std::chrono::seconds(0));  // Already expired, fire immediately
}
```

### Anti-Patterns to Avoid
- **Using `system_timer` or `system_clock::time_point`:** Asio's steady_timer is the correct choice. System clock can jump (NTP). Steady clock is monotonic. The small drift between wall-clock expiry and steady-clock duration is acceptable (sub-second).
- **Storing a `steady_clock::time_point` as the target:** The timer target is a wall-clock timestamp (from MDBX). Store it as `uint64_t` (seconds since epoch) and recompute the duration each time the timer is armed.
- **Spawning a new coroutine on each ingest:** The timer cancel pattern handles rearming within the existing coroutine. No new coroutine needed.
- **In-memory priority queue of expiry times:** D-01 locks this -- storage is source of truth. MDBX cursor is O(1) for first entry. No in-memory state.

## Detailed Design Decisions (Claude's Discretion Areas)

### D-04 Resolution: Extend on_blob_ingested Signature

Three options were identified:

| Option | Approach | Invasiveness | Overhead |
|--------|----------|-------------|----------|
| A | Add `uint64_t expiry_time` parameter to `on_blob_ingested()` | Moderate (6 call sites + callback types) | Zero runtime overhead |
| B | Query storage inside `on_blob_ingested()` | Minimal (no signature change) | One MDBX read txn per ingest |
| C | Pass through IngestResult | Moderate (change IngestResult struct) | Zero runtime overhead |

**Recommendation: Option A -- extend signature.** The expiry time is trivially computed at every call site as `blob.ttl > 0 ? blob.timestamp + blob.ttl : 0`. All call sites already have the `blob` variable in scope. The function runs on the IO thread, so no threading concerns. Six call sites is manageable.

Call sites that need updating:
1. `peer_manager.cpp` line ~719 (Delete handler)
2. `peer_manager.cpp` line ~1680 (Data/client write handler)
3. `peer_manager.cpp` line ~3086 (BlobFetchResponse handler)
4. `sync_protocol.cpp` line ~88 (sync ingest callback)
5. `sync_protocol.h` line ~63 (OnBlobIngested typedef)
6. `peer_manager.cpp` line ~158 (lambda wiring in constructor)

### Config: Deprecate, Don't Remove

Keep `expiry_scan_interval_seconds` in `Config` struct but ignore it in PeerManager. Log a deprecation warning on startup if it's set to a non-default value. This avoids breaking existing config files.

### Timer State Tracking

Use a single `uint64_t next_expiry_target_` member initialized to 0 (meaning "no timer armed"). On the IO thread, this is race-free (single-threaded io_context).

```cpp
uint64_t next_expiry_target_ = 0;  // 0 = no timer, wall-clock seconds otherwise
```

### Edge Case: No Blobs / All TTL=0

When `get_earliest_expiry()` returns `std::nullopt`:
- On startup: don't spawn the coroutine at all. Set `next_expiry_target_ = 0`.
- After scan: the coroutine returns (co_return). `next_expiry_target_ = 0`.
- On ingest of a TTL>0 blob: `next_expiry_target_` is 0, so any expiry is "earlier". Spawn the coroutine fresh via `co_spawn`.

This requires a way to re-spawn the coroutine from `on_blob_ingested()`. Since `on_blob_ingested()` is not a coroutine (it's a regular void function), use `asio::co_spawn(ioc_, expiry_scan_loop(), asio::detached)` when transitioning from disarmed to armed.

### Edge Case: Timer Fires, Nothing Expired

Per D-02: `run_expiry_scan()` returns 0, then `get_earliest_expiry()` finds the actual next entry. Timer rearms. This handles deleted-before-expiry blobs with zero complexity.

## New Storage API

### `Storage::get_earliest_expiry()`

```cpp
/// Query the earliest expiry timestamp in the expiry_map.
/// O(1) via MDBX cursor seek to first key.
/// @return The earliest expiry timestamp (wall-clock seconds), or nullopt if no expiring blobs.
std::optional<uint64_t> get_earliest_expiry() const;
```

Implementation pattern:
```cpp
std::optional<uint64_t> Storage::get_earliest_expiry() const {
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->expiry_map);
        auto first = cursor.to_first(false);
        if (!first.done) return std::nullopt;

        auto key_data = cursor.current(false).key;
        if (key_data.length() < 8) return std::nullopt;

        uint64_t expiry_ts = decode_be_u64(
            static_cast<const uint8_t*>(key_data.data()));
        return expiry_ts;
    } catch (const std::exception& e) {
        spdlog::error("get_earliest_expiry: {}", e.what());
        return std::nullopt;
    }
}
```

This is a read-only transaction -- no write lock contention. The `const` qualifier is appropriate because `start_read()` doesn't modify the environment.

## Common Pitfalls

### Pitfall 1: Coroutine Restart Race
**What goes wrong:** `on_blob_ingested()` calls `co_spawn(expiry_scan_loop())` to restart the coroutine, but the previous coroutine hasn't fully exited yet. Two concurrent expiry loops run simultaneously.
**Why it happens:** `co_return` in a detached coroutine doesn't synchronize with the spawner.
**How to avoid:** Use a boolean `expiry_loop_running_` flag. Set to true before co_spawn, set to false before co_return. In `on_blob_ingested()`, only co_spawn if `!expiry_loop_running_`. Since everything runs on the single IO thread, no atomics needed.
**Warning signs:** Double purge log messages, unexpected timer cancels.

### Pitfall 2: Wall Clock Jump
**What goes wrong:** System clock jumps forward (NTP correction). Timer was armed for "expiry in 3600s" but now "now" is 3500s later. The steady_timer still fires after the original 3600s of real time, not at the wall-clock expiry.
**Why it happens:** `steady_timer` measures monotonic elapsed time, not wall-clock time.
**How to avoid:** This is actually the CORRECT behavior -- the timer fires approximately when the blob expires. The `run_expiry_scan()` always checks `expiry_ts > now` using the wall clock, so even if the timer fires slightly early or late, the scan is always correct. Accept sub-second inaccuracy.
**Warning signs:** None -- this is a non-issue by design.

### Pitfall 3: Underflow in Duration Calculation
**What goes wrong:** `target_expiry - now()` underflows when the blob is already expired (target < now).
**Why it happens:** Unsigned subtraction wraps around to a huge value, timer sleeps for years.
**How to avoid:** Always check `target_expiry > now` before subtraction. If already expired, use `seconds(0)` to fire immediately.
**Warning signs:** Timer never fires, expired blobs accumulate.

### Pitfall 4: Forgetting to Update next_expiry_target_ on Cancel
**What goes wrong:** `on_blob_ingested()` cancels the timer but doesn't update `next_expiry_target_`. The coroutine resumes, reads the old `next_expiry_target_`, and re-arms to the wrong time.
**How to avoid:** Always update `next_expiry_target_` BEFORE cancelling the timer. The coroutine, upon resume from cancel, should re-read `next_expiry_target_` to get the new value.
**Warning signs:** Timer fires at the old time instead of the earlier one.

### Pitfall 5: Expiry Timer Not Armed After Startup With Existing Data
**What goes wrong:** Node starts with blobs in storage that have expiry times, but the expiry loop is never started because the initial query fails or is skipped.
**Why it happens:** Startup code path doesn't call `get_earliest_expiry()`.
**How to avoid:** In `start()`, always query earliest expiry. If found, spawn the coroutine and set `next_expiry_target_`. If not found, leave timer disarmed.
**Warning signs:** Blobs that should expire on a restarted node never get purged.

### Pitfall 6: Coroutine Uses expiry_scan_interval_seconds_ After Removal
**What goes wrong:** After removing the periodic timer, some code path still references `expiry_scan_interval_seconds_` causing the timer to fall back to periodic behavior.
**How to avoid:** Search for all references to `expiry_scan_interval_seconds_` and ensure they're either removed or gated behind the deprecation path. Grep audit in verification.
**Warning signs:** Periodic scan behavior persists in logs.

## Code Examples

### Rewritten expiry_scan_loop()
```cpp
// Source: Derived from existing pattern at peer_manager.cpp:2873
asio::awaitable<void> PeerManager::expiry_scan_loop() {
    expiry_loop_running_ = true;
    while (!stopping_) {
        uint64_t target = next_expiry_target_;
        if (target == 0) break;  // No expiry target -- exit loop

        uint64_t now = storage::system_clock_seconds();
        auto duration = (target > now)
            ? std::chrono::seconds(target - now)
            : std::chrono::seconds(0);

        asio::steady_timer timer(ioc_);
        expiry_timer_ = &timer;
        timer.expires_after(duration);
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        expiry_timer_ = nullptr;
        if (stopping_) break;
        if (ec) continue;  // Cancelled (rearmed) -- loop back to read new target

        auto purged = storage_.run_expiry_scan();
        if (purged > 0) {
            spdlog::info("expiry scan: purged {} blobs", purged);
        }

        // Rearm to next earliest expiry
        auto next = storage_.get_earliest_expiry();
        if (next.has_value()) {
            next_expiry_target_ = *next;
        } else {
            next_expiry_target_ = 0;
            break;  // No more expiring blobs
        }
    }
    expiry_loop_running_ = false;
}
```

### Expiry Check in on_blob_ingested()
```cpp
// Source: Derived from existing on_blob_ingested at peer_manager.cpp:2951
void PeerManager::on_blob_ingested(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    uint64_t expiry_time,          // NEW: 0 = no expiry (TTL=0)
    net::Connection::Ptr source) {

    // ... existing notification fan-out code ...

    // Event-driven expiry: check if new blob expires sooner than current target
    if (expiry_time > 0) {
        if (next_expiry_target_ == 0 || expiry_time < next_expiry_target_) {
            next_expiry_target_ = expiry_time;
            if (expiry_loop_running_) {
                // Rearm existing timer
                if (expiry_timer_) expiry_timer_->cancel();
            } else {
                // Spawn new loop (was idle -- no expiring blobs existed)
                asio::co_spawn(ioc_, expiry_scan_loop(), asio::detached);
            }
        }
    }
}
```

### Call Site Update Pattern
```cpp
// At every call site (6 total), compute expiry_time from blob:
uint64_t expiry_time = (blob.ttl > 0)
    ? static_cast<uint64_t>(blob.timestamp) + static_cast<uint64_t>(blob.ttl)
    : 0;
on_blob_ingested(
    blob.namespace_id,
    result.ack->blob_hash,
    result.ack->seq_num,
    static_cast<uint32_t>(blob.data.size()),
    wire::is_tombstone(blob.data),
    expiry_time,
    source);
```

### Storage Method
```cpp
// Source: Pattern from run_expiry_scan() at storage.cpp:1024
std::optional<uint64_t> Storage::get_earliest_expiry() const {
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->expiry_map);
        auto first = cursor.to_first(false);
        if (!first.done) return std::nullopt;

        auto key_data = cursor.current(false).key;
        if (key_data.length() < 8) return std::nullopt;

        return decode_be_u64(
            static_cast<const uint8_t*>(key_data.data()));
    } catch (const std::exception& e) {
        spdlog::error("get_earliest_expiry: {}", e.what());
        return std::nullopt;
    }
}
```

## Integration Points Summary

### Files Modified

| File | Change | Scope |
|------|--------|-------|
| `db/storage/storage.h` | Add `get_earliest_expiry() const` declaration | 1 method |
| `db/storage/storage.cpp` | Implement `get_earliest_expiry()` | ~15 lines |
| `db/peer/peer_manager.h` | Add `next_expiry_target_`, `expiry_loop_running_` members; update `on_blob_ingested` signature | 3 lines + signature |
| `db/peer/peer_manager.cpp` | Rewrite `expiry_scan_loop()`, update `on_blob_ingested()`, update `start()`, update 4 call sites | ~60 lines changed |
| `db/sync/sync_protocol.h` | Update `OnBlobIngested` typedef to include `uint64_t expiry_time` | 1 line |
| `db/sync/sync_protocol.cpp` | Update callback invocation to pass expiry_time | 1 line |
| `db/config/config.h` | Add deprecation comment to `expiry_scan_interval_seconds` | Comment only |

### Files Created

| File | Purpose |
|------|---------|
| `db/tests/peer/test_event_expiry.cpp` | Unit tests for event-driven expiry behavior |

### Existing Tests Unaffected

The existing storage expiry tests in `test_storage.cpp` test `run_expiry_scan()` directly with injectable clocks. They remain valid and unmodified -- the storage layer behavior doesn't change. The new tests cover the PeerManager timer orchestration layer.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3 |
| Config file | `CMakeLists.txt` (FetchContent) |
| Quick run command | `cmake --build build && ./build/db/tests/chromatindb_tests "[event-expiry]"` |
| Full suite command | `cmake --build build && ./build/db/tests/chromatindb_tests` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| MAINT-01 | Timer fires at earliest expiry time | unit | `./build/db/tests/chromatindb_tests "[event-expiry]" -c "timer fires at exact expiry"` | Wave 0 |
| MAINT-01 | get_earliest_expiry returns correct value | unit | `./build/db/tests/chromatindb_tests "[storage][earliest-expiry]"` | Wave 0 |
| MAINT-02 | After scan, timer rearms to next earliest | unit | `./build/db/tests/chromatindb_tests "[event-expiry]" -c "chain rearm"` | Wave 0 |
| MAINT-03 | Ingest with shorter TTL rearms timer | unit | `./build/db/tests/chromatindb_tests "[event-expiry]" -c "ingest rearm"` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cmake --build build && ./build/db/tests/chromatindb_tests "[event-expiry]" "[storage][earliest-expiry]"`
- **Per wave merge:** `cmake --build build && ./build/db/tests/chromatindb_tests`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/storage/test_storage.cpp` -- add `[storage][earliest-expiry]` test section for `get_earliest_expiry()`
- [ ] `db/tests/peer/test_event_expiry.cpp` -- new file for PeerManager event-driven expiry tests (MAINT-01, MAINT-02, MAINT-03)

## Testing Strategy

### Storage Layer Tests (in test_storage.cpp)

Test `get_earliest_expiry()` using the existing injectable clock pattern:

```cpp
TEST_CASE("get_earliest_expiry returns nullopt when empty", "[storage][earliest-expiry]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    REQUIRE_FALSE(store.get_earliest_expiry().has_value());
}

TEST_CASE("get_earliest_expiry returns earliest time", "[storage][earliest-expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    // Blob expiring at 1100 (ts=1000, ttl=100)
    auto blob1 = make_test_blob(0x01, "early", 100, 1000);
    store.store_blob(blob1);

    // Blob expiring at 2000 (ts=1000, ttl=1000)
    auto blob2 = make_test_blob(0x01, "late", 1000, 1000);
    store.store_blob(blob2);

    auto earliest = store.get_earliest_expiry();
    REQUIRE(earliest.has_value());
    REQUIRE(*earliest == 1100);
}

TEST_CASE("get_earliest_expiry skips TTL=0 blobs", "[storage][earliest-expiry]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto permanent = make_test_blob(0x01, "permanent", 0, 1000);
    store.store_blob(permanent);

    REQUIRE_FALSE(store.get_earliest_expiry().has_value());
}

TEST_CASE("get_earliest_expiry updates after scan", "[storage][earliest-expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto blob1 = make_test_blob(0x01, "first", 100, 1000);   // expires 1100
    auto blob2 = make_test_blob(0x01, "second", 200, 1000);  // expires 1200
    store.store_blob(blob1);
    store.store_blob(blob2);

    REQUIRE(*store.get_earliest_expiry() == 1100);

    fake_time = 1101;
    store.run_expiry_scan();  // Purges blob1

    REQUIRE(*store.get_earliest_expiry() == 1200);
}
```

### PeerManager Integration Tests

Testing the full timer flow requires an `io_context` with controlled time advancement. The simplest approach uses real timers with very short durations (1-2 seconds) and injectable storage clocks:

1. **Timer fires at exact expiry (MAINT-01):** Store blob with known expiry, verify `run_expiry_scan()` is called at the right time.
2. **Chain rearm (MAINT-02):** Store two blobs with different expiry times, verify second scan fires after first.
3. **Ingest rearm (MAINT-03):** Arm timer for far-future expiry, ingest blob with near-future expiry, verify timer fires at near-future time.
4. **Empty storage (edge case):** Start with no blobs, verify no timer. Ingest a blob, verify timer arms.
5. **All TTL=0 (edge case):** Ingest only permanent blobs, verify no timer arms.

## Open Questions

1. **`get_earliest_expiry()` after `run_expiry_scan()`**
   - What we know: `run_expiry_scan()` uses a write transaction. `get_earliest_expiry()` uses a read transaction. They're never concurrent (single IO thread).
   - What's unclear: Whether the read transaction in `get_earliest_expiry()` always sees the writes from the just-committed `run_expiry_scan()` write transaction.
   - Recommendation: MDBX guarantees read-after-write visibility for subsequent transactions on the same thread. This is a non-issue. HIGH confidence.

## Sources

### Primary (HIGH confidence)
- `db/storage/storage.cpp` lines 61-67: `make_expiry_key()` confirms `[expiry_ts_be:8][hash:32]` key format
- `db/storage/storage.cpp` lines 1024-1133: `run_expiry_scan()` implementation with cursor.to_first() pattern
- `db/storage/storage.cpp` lines 427-436: Expiry entry creation (TTL>0 only, expiry = timestamp + ttl)
- `db/peer/peer_manager.cpp` lines 2873-2888: Current `expiry_scan_loop()` coroutine
- `db/peer/peer_manager.cpp` lines 2951-2990: `on_blob_ingested()` implementation
- `db/peer/peer_manager.h` line 325: `expiry_timer_` pointer member
- `db/peer/peer_manager.h` line 340: `expiry_scan_interval_seconds_` member
- `db/wire/codec.h` lines 11-18: `BlobData` struct with ttl and timestamp fields

### Secondary (MEDIUM confidence)
- Asio `steady_timer` behavior: cancel causes `operation_aborted` error code on waiting coroutine

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all existing libraries
- Architecture: HIGH - pattern directly extends existing timer-cancel coroutine pattern used 8+ times
- Pitfalls: HIGH - all based on concrete code analysis, not theoretical

**Research date:** 2026-04-03
**Valid until:** 2026-05-03 (stable domain, no external dependencies)
