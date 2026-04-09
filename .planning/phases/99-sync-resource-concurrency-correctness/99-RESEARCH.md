# Phase 99: Sync, Resource & Concurrency Correctness - Research

**Researched:** 2026-04-08
**Domain:** C++ coroutine correctness, MDBX transactional integrity, resource limit enforcement
**Confidence:** HIGH

## Summary

Phase 99 addresses eight concrete bugs/correctness gaps identified during the v2.2.0 audit. All issues have clear root causes, known fix locations, and deterministic verification strategies. This is a hardening phase with no new features and no protocol changes.

The bugs fall into three categories: (1) sync state leaks in `pending_fetches_` -- entries leak on rejected ingests and the hash-only key allows cross-namespace collision, (2) resource limit enforcement gaps -- subscription unbounded, bootstrap detection ignores port, capacity/quota checks race with concurrent ingests, and quota rebuild has iterator bug, and (3) coroutine counter safety -- verifying that all `NodeMetrics` increments happen on the io_context strand.

**Primary recommendation:** Fix each bug as a targeted, testable unit. Group by category (sync, resource, coro) for logical coherence. The TOCTOU fix (D-10/D-11) requires the most careful design since it changes the engine/storage contract. All other fixes are localized to single files.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** pending_fetches_ cleanup on rejected ingest: erase for ALL ingest outcomes in handle_blob_fetch_response, not just on `result.accepted && result.ack.has_value()`
- **D-02:** pending_fetches_ key: change from `array<uint8_t,32>` to `array<uint8_t,64>` (namespace||hash concatenation) with ArrayHash64 functor
- **D-03:** Verify clean_pending_fetches(conn) covers all map entries after key type change
- **D-04:** Verify collect_namespace_hashes snapshot consistency via MDBX read transaction scope
- **D-05:** Sequential sync phases (A/B/C) with MDBX MVCC handles concurrent writes
- **D-06:** Per-connection subscription limit in Subscribe handler (message_dispatcher.cpp)
- **D-07:** Default limit 256 per connection, configurable via `max_subscriptions_per_connection` (0 = unlimited)
- **D-08:** Error response uses existing protocol error mechanism (no new message type)
- **D-09:** Bootstrap peer detection: compare full endpoint (host + port), not just host
- **D-10:** TOCTOU on capacity: make check part of store transaction (atomic check-and-reserve inside store_blob)
- **D-11:** TOCTOU on quota: same pattern as D-10
- **D-12:** Quota rebuild iterator: fix erase-while-iterating with standard erase-returns-next pattern
- **D-13:** Verify NodeMetrics increments are all on io_context strand
- **D-14:** Fix strand confinement, NOT std::atomic (don't paper over design issue)
- **D-15:** Full TSAN run to verify no data races on NodeMetrics
- **D-16:** Each fix gets targeted unit test (fails without fix, passes with fix)
- **D-17:** Full TSAN run is acceptance gate for CORO-01

### Claude's Discretion
- Implementation order within each category
- Exact error message text for subscription limit rejection
- Whether to add debug logging for each fix (recommended: yes)

### Deferred Ideas (OUT OF SCOPE)
None
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| SYNC-01 | pending_fetches_ entries cleaned on rejected ingest (not just successful) | Line 194 in blob_push_manager.cpp only erases on `result.accepted && result.ack.has_value()` -- rejected and not-found paths leak entries. Fix: unconditional erase keyed by namespace+hash. |
| SYNC-02 | pending_fetches_ key includes namespace to prevent cross-namespace hash collision | Current key is `array<uint8_t,32>` (hash only) at blob_push_manager.h:67-68. Two blobs in different namespaces with same content hash collide. Fix: `array<uint8_t,64>` key. |
| SYNC-03 | Phase B reconciliation takes consistent hash snapshot | `get_hashes_by_namespace` uses MDBX `start_read()` (MVCC snapshot). Individual `get_blob` calls use separate read txns. Verified: this is safe -- missed additions caught next round, deleted blobs return nullopt (already handled). Recommend documenting this in a code comment. |
| RES-01 | Per-peer subscription count limit enforced at node level | Subscribe handler at message_dispatcher.cpp:222-233 has no limit check. Fix: add size check before insert, send error response on breach. |
| RES-02 | Bootstrap peer detection considers port, not just host | connection_manager.cpp:104 compares only host part. Fix: compare full `bp == info.address` string. |
| RES-03 | TOCTOU race on capacity and quota checks eliminated | Engine Step 0b (line 116-123) and Step 2a (line 207-226) run before store_blob's write transaction. Fix: pass limits to store_blob, check inside write txn. |
| RES-04 | Quota rebuild clear loop fixed (iterator invalidation) | storage.cpp:1373-1378 does to_next then erase -- erases wrong entry, skips every other. Fix: erase-returns-next pattern. |
| CORO-01 | Send/recv counters use proper strand confinement across co_await | All metrics increments verified: Data handler increments after `co_await asio::post(ioc_, ...)` (line 1169). Sync orchestrator runs entirely on ioc_. Connection manager increments are inline (non-async). All on io_context strand. TSAN run needed for definitive verification. |
</phase_requirements>

## Architecture Patterns

### Bug Fix Structure (per CONTEXT.md D-16)
Each bug fix follows a consistent pattern:
1. **Identify the exact bug location** (file:line)
2. **Write a test that reproduces the bug** (fails without fix)
3. **Apply the minimal fix**
4. **Verify the test passes**
5. **Add debug logging** (consistent with Phase 98 pattern)

### Relevant Project Structure
```
db/
  peer/
    blob_push_manager.h     # SYNC-01/SYNC-02: pending_fetches_ key type change
    blob_push_manager.cpp   # SYNC-01: cleanup on all ingest outcomes
    connection_manager.cpp  # RES-02: bootstrap detection
    message_dispatcher.cpp  # RES-01: subscription limit
    peer_types.h            # ArrayHash64 functor, NodeMetrics struct (CORO-01)
    metrics_collector.cpp   # CORO-01: metrics read side (all on ioc_)
    sync_orchestrator.cpp   # CORO-01: metrics write side (all on ioc_)
  engine/
    engine.cpp              # RES-03: capacity/quota TOCTOU removal
    engine.h                # RES-03: store_blob signature change
  storage/
    storage.cpp             # RES-03: atomic check-and-reserve, RES-04: quota rebuild fix
    storage.h               # RES-03: store_blob extended signature
  sync/
    sync_protocol.cpp       # SYNC-03: snapshot consistency verification
  tests/
    peer/test_peer_manager.cpp    # Existing integration tests
    engine/test_engine.cpp        # Existing quota tests
    storage/test_storage.cpp      # Existing storage tests
```

### Pattern 1: Composite Key with ArrayHash64
**What:** Extend the ArrayHash32 pattern in peer_types.h to a 64-byte key hash functor
**When to use:** When the pending_fetches_ map needs namespace+hash deduplication

```cpp
// Source: db/peer/peer_types.h (extending existing ArrayHash32 pattern)
struct ArrayHash64 {
    size_t operator()(const std::array<uint8_t, 64>& arr) const noexcept {
        uint64_t h;
        std::memcpy(&h, arr.data(), sizeof(h));
        return static_cast<size_t>(h);
    }
};
```

The first 8 bytes of a namespace||hash concatenation are from the namespace ID (SHA3-256 output), providing sufficient entropy for hash map distribution.

### Pattern 2: Atomic Check-and-Reserve in MDBX Write Transaction
**What:** Move capacity and quota checks into `store_blob`'s write transaction to eliminate TOCTOU
**When to use:** When a pre-check races with concurrent writes between co_await suspension points

The current flow is:
```
Engine::ingest()
  Step 0b: if (storage_.used_bytes() >= max)  -> REJECT  // READ txn
  ...co_await offload...                                   // <-- TOCTOU window
  Step 2a: if (quota.bytes + size > limit)     -> REJECT  // READ txn
  ...co_await offload...                                   // <-- TOCTOU window
  Step 4: storage_.store_blob(...)                         // WRITE txn
```

The fix adds optional limit parameters to `store_blob`:
```
Storage::store_blob(blob, hash, encoded, max_storage_bytes, quota_byte_limit, quota_count_limit)
  WRITE txn:
    dedup check
    used_bytes check (if max_storage_bytes > 0)
    quota check (if limits > 0)
    store + commit
```

Engine Steps 0b and 2a become early-exit optimizations (fast-reject on obvious over-limit) but are no longer the authoritative check. The write transaction is.

### Pattern 3: Erase-Returns-Next Iterator Pattern
**What:** Standard C++ pattern for erasing during iteration
**When to use:** When clearing entries from an MDBX cursor-based loop

```cpp
// WRONG (current code, storage.cpp:1373-1378):
auto result = cursor.to_first(false);
while (result.done) {
    auto next = cursor.to_next(false);  // moves cursor to next
    cursor.erase();                      // erases where cursor now points (WRONG!)
    result.done = next.done;
}

// CORRECT: erase current, then advance
auto result = cursor.to_first(false);
while (result.done) {
    cursor.erase();          // erase current position
    result = cursor.to_first(false);  // restart from first remaining
}
// OR (if MDBX erase preserves position):
auto result = cursor.to_first(false);
while (result.done) {
    cursor.erase();
    result = cursor.to_next(false);  // if erase returns next automatically
}
```

Note: The exact MDBX cursor behavior after erase needs verification. MDBX `cursor.erase()` invalidates the current position. The safest approach is to erase-then-restart-from-first, or use `txn.clear_map()` if available for a full clear.

### Anti-Patterns to Avoid
- **std::atomic for NodeMetrics:** D-14 explicitly forbids this. The design guarantees strand confinement; atomics would mask a real threading bug if one existed.
- **New message types for subscription limit:** D-08 says use existing protocol error mechanism. No new wire format.
- **Keeping capacity/quota checks only in engine:** After the fix, engine's checks become fast-reject hints. The authoritative check is in store_blob's write transaction.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| MDBX cursor clear | Manual iterate-and-erase | `txn.clear_map()` if available, or erase-from-first loop | Cursor invalidation after erase is subtle |
| Composite hash key | Custom hash combining | ArrayHash64 on first 8 bytes | Consistent with existing ArrayHash32 pattern |
| Thread-safe counters | std::atomic<uint64_t> | Strand confinement verification | Project design decision (D-14), atomics mask bugs |

## Common Pitfalls

### Pitfall 1: MDBX Cursor Invalidation After Erase
**What goes wrong:** Erasing an entry while iterating with a cursor invalidates the cursor position. The next `to_next()` call may skip entries or return unexpected results.
**Why it happens:** MDBX cursors point to B-tree nodes. Erasing rebalances the tree.
**How to avoid:** After erase, either restart from first or use the MDBX API that positions the cursor after erase. For full clear, prefer `txn.clear_map()`.
**Warning signs:** "Cleared X entries" logging shows fewer entries than expected.

### Pitfall 2: pending_fetches_ Key Change Ripple
**What goes wrong:** Changing the key type from `array<uint8_t,32>` to `array<uint8_t,64>` breaks all call sites that construct, insert, or lookup keys.
**Why it happens:** Three methods use pending_fetches_: `on_blob_notify` (insert), `handle_blob_fetch_response` (erase), and `clean_pending_fetches` (iterate+erase). The dedup check in `on_blob_notify` (line 128) also needs the composite key.
**How to avoid:** Create a helper to build the composite key from namespace+hash, use it consistently everywhere.
**Warning signs:** Compile errors will catch most issues. Test with two blobs in different namespaces sharing the same content hash.

### Pitfall 3: store_blob Signature Change Breaks Callers
**What goes wrong:** Adding capacity/quota parameters to `store_blob` requires updating all callers: engine.cpp (the main path) and potentially test code.
**Why it happens:** The overloaded `store_blob` has two signatures; adding params to the precomputed variant means engine.cpp must pass them.
**How to avoid:** Make the new parameters have defaults (0 = no check) so the no-arg `store_blob(blob)` convenience overload and existing test calls don't break.
**Warning signs:** Compilation failures in test_storage.cpp and test_engine.cpp.

### Pitfall 4: TOCTOU Fix Must Handle Duplicates Correctly
**What goes wrong:** If capacity/quota check inside store_blob's write txn rejects, but the blob is actually a duplicate (dedup check comes first), the rejection is wrong -- duplicates should succeed (they don't add storage).
**Why it happens:** The dedup check in store_blob returns early with Duplicate status before any capacity check. This ordering is already correct and must be preserved.
**How to avoid:** Verify that the dedup check (line 349-380) runs before the capacity/quota check inside the write transaction.
**Warning signs:** Duplicate blobs suddenly rejected as "storage full".

### Pitfall 5: Bootstrap Address Format Assumption
**What goes wrong:** The fix for RES-02 assumes `info.address` and `bootstrap_addresses_` entries have the same format (host:port). If formats differ (e.g., one has brackets for IPv6), the string comparison fails.
**Why it happens:** Bootstrap addresses come from config, remote addresses come from Asio.
**How to avoid:** Verify both sources use the same string format. Check existing test coverage for bootstrap detection. Consider normalizing if needed.
**Warning signs:** Bootstrap peers not detected despite correct host+port.

### Pitfall 6: Subscription Limit vs Batch Subscribe
**What goes wrong:** A single Subscribe message can contain multiple namespaces. The limit check must account for the total AFTER adding all namespaces in the batch, not just check against the current count.
**Why it happens:** The Subscribe handler loops through decoded namespaces and inserts each one.
**How to avoid:** Check `peer->subscribed_namespaces.size() + namespaces.size() > limit` before inserting any. Or check per-insert and reject the entire batch on first breach.
**Warning signs:** Clients can exceed the limit by sending a single Subscribe with many namespaces.

## Code Examples

### SYNC-01: Unconditional pending_fetches_ Cleanup
```cpp
// Source: db/peer/blob_push_manager.cpp handle_blob_fetch_response
// CURRENT (bug): only erases on accepted+ack
if (result.accepted && result.ack.has_value()) {
    pending_fetches_.erase(result.ack->blob_hash);
    ...
}

// FIX: erase for ALL outcomes using the original key (namespace||hash from blob)
// Build composite key from the blob we decoded (before engine result check)
auto pending_key = make_pending_key(blob.namespace_id, /* blob_hash from decode */);
pending_fetches_.erase(pending_key);  // Always clean up
if (result.accepted && result.ack.has_value()) {
    ...  // fan-out, metrics
}
```

Note: The blob hash for the pending key must be computed from the response blob data, since `result.ack->blob_hash` is only available on accepted results. Either use the blob content hash computed during decode, or the key used at insert time (which comes from the BlobNotify that triggered the fetch).

### SYNC-02: Composite Key Helper
```cpp
// Source: db/peer/blob_push_manager.h (new helper)
inline std::array<uint8_t, 64> make_pending_key(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash) {
    std::array<uint8_t, 64> key;
    std::memcpy(key.data(), namespace_id.data(), 32);
    std::memcpy(key.data() + 32, blob_hash.data(), 32);
    return key;
}
```

### RES-01: Subscription Limit Check
```cpp
// Source: db/peer/message_dispatcher.cpp Subscribe handler
if (type == wire::TransportMsgType_Subscribe) {
    auto* peer = find_peer(conn);
    if (peer) {
        auto namespaces = PeerManager::decode_namespace_list(payload);
        // D-06/D-07: Check subscription limit before adding
        if (max_subscriptions_ > 0 &&
            peer->subscribed_namespaces.size() + namespaces.size() > max_subscriptions_) {
            spdlog::warn("subscription limit exceeded for peer {} ({} + {} > {})",
                         peer_display_name(conn),
                         peer->subscribed_namespaces.size(),
                         namespaces.size(), max_subscriptions_);
            // D-08: Send error response using existing protocol error
            // (specific error TBD -- could reuse QuotaExceeded or a generic error byte)
            return;
        }
        for (const auto& ns : namespaces) {
            peer->subscribed_namespaces.insert(ns);
        }
    }
    return;
}
```

### RES-02: Full Endpoint Bootstrap Comparison
```cpp
// Source: db/peer/connection_manager.cpp on_peer_connected
// CURRENT (bug): compares host only
if (bp_host == ra_host) {

// FIX: compare full address string (host:port)
if (bp == info.address) {
```

### RES-04: Quota Rebuild Clear Fix
```cpp
// Source: db/storage/storage.cpp rebuild_quota_aggregates
// CURRENT (bug): to_next then erase skips entries
auto result = cursor.to_first(false);
while (result.done) {
    auto next = cursor.to_next(false);
    cursor.erase();  // erases NEXT, not current
    result.done = next.done;
}

// FIX Option A: erase current, cursor auto-advances or restart
auto result = cursor.to_first(false);
while (result.done) {
    cursor.erase();
    result = cursor.to_first(false);  // restart from first remaining
}

// FIX Option B: if MDBX clear_map() is available
txn.clear_map(impl_->quota_map);
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Hash-only pending_fetches key | Namespace+hash composite key | Phase 99 | Prevents cross-namespace collision |
| Pre-transaction capacity check | In-transaction check | Phase 99 | Eliminates TOCTOU race |
| Host-only bootstrap detection | Host+port comparison | Phase 99 | Multiple nodes per host work correctly |
| Unbounded subscriptions | Configurable per-connection limit | Phase 99 | Resource exhaustion prevention |

## Specific Implementation Notes

### SYNC-03 Snapshot Consistency (D-04/D-05)
After code analysis, `collect_namespace_hashes` is already safe:
1. `get_hashes_by_namespace` opens a read transaction (MVCC snapshot) at storage.cpp:626
2. Individual `get_blob` calls open separate read transactions
3. A concurrent write between these calls can:
   - Add a blob: not in our hash list, caught in next sync round (acceptable)
   - Delete a blob: `get_blob` returns nullopt, we skip it (handled at line 31)
4. The hash vector is returned by value -- it's a snapshot of the hash list at read time

**Recommendation:** Add a code comment in `collect_namespace_hashes` documenting this analysis. No code change needed. Optionally, wrap the entire function in a single read transaction for belt-and-suspenders consistency (get_hashes + all get_blob calls share one MVCC snapshot).

### CORO-01 Strand Confinement Verification
After auditing all `metrics_.*` increment sites:

| Location | Context | On ioc_ strand? |
|----------|---------|-----------------|
| message_dispatcher.cpp:125 | rate_limited (inline, pre-spawn) | YES |
| message_dispatcher.cpp:134,140,157,171 | sync_rejections (inline) | YES |
| message_dispatcher.cpp:1190,1192,1194,1202 | ingests/rejections/quota (after `co_await post(ioc_)`) | YES |
| sync_orchestrator.cpp:254,292,294,577,597,695,744,746,1024,1032 | cursor/sync metrics (coroutine on ioc_) | YES |
| blob_push_manager.cpp:208,211 | ingests (coroutine on ioc_, after `co_await post(ioc_)`) | YES |
| connection_manager.cpp:76,165,220 | peers connected/disconnected (inline callbacks) | YES |

**Conclusion:** All metrics increments are on the io_context strand. The code comment in peer_types.h:57 ("single io_context thread, no races") is accurate. TSAN run (D-15/D-17) will provide definitive verification.

### RES-03 TOCTOU Fix Design
The fix requires adding optional parameters to `store_blob`:

```cpp
// storage.h: extended store_blob signature
StoreResult store_blob(const wire::BlobData& blob,
                       const std::array<uint8_t, 32>& precomputed_hash,
                       std::span<const uint8_t> precomputed_encoded,
                       uint64_t max_storage_bytes = 0,
                       uint64_t quota_byte_limit = 0,
                       uint64_t quota_count_limit = 0);
```

Inside the write transaction, AFTER dedup check (which returns Duplicate early), BEFORE encryption:
1. If `max_storage_bytes > 0`: check `used_bytes() >= max_storage_bytes`
2. If `quota_byte_limit > 0 || quota_count_limit > 0`: read quota_map, check limits

Engine Steps 0b and 2a remain as fast-path optimizations (avoid expensive crypto for obviously-over-limit blobs) but are no longer authoritative.

New StoreResult::Status values needed: `CapacityExceeded` and `QuotaExceeded` (or reuse Error with a reason enum).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (latest via FetchContent) |
| Config file | db/CMakeLists.txt (lines 215-262) |
| Quick run command | `cd build && ctest -R "sync\|resource\|coro" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SYNC-01 | pending_fetches cleaned on rejected ingest | unit | `cd build && ctest -R "pending_fetches_cleanup_on_reject" --output-on-failure` | Wave 0 |
| SYNC-02 | composite namespace+hash key prevents collision | unit | `cd build && ctest -R "pending_fetches_composite_key" --output-on-failure` | Wave 0 |
| SYNC-03 | snapshot consistency verified | unit/code-review | Existing sync tests + comment verification | Existing (test_sync_protocol.cpp) |
| RES-01 | subscription limit rejection | unit | `cd build && ctest -R "subscription_limit" --output-on-failure` | Wave 0 |
| RES-02 | bootstrap host+port comparison | unit | `cd build && ctest -R "bootstrap_endpoint" --output-on-failure` | Wave 0 |
| RES-03 | atomic capacity/quota check in store_blob | unit | `cd build && ctest -R "store_blob_capacity\|store_blob_quota" --output-on-failure` | Wave 0 |
| RES-04 | quota rebuild clears all entries | unit | `cd build && ctest -R "quota_rebuild" --output-on-failure` | Wave 0 |
| CORO-01 | no data races on NodeMetrics | integration | `cd build-tsan && ctest --output-on-failure` (TSAN build) | Existing (all tests under TSAN) |

### Sampling Rate
- **Per task commit:** `cd build && ctest --output-on-failure` (full suite)
- **Per wave merge:** ASAN + UBSAN + TSAN builds
- **Phase gate:** All tests green under ASAN, UBSAN, and TSAN

### Wave 0 Gaps
- [ ] New test cases for SYNC-01, SYNC-02 in a test file (likely test_peer_manager.cpp or new test_blob_push_manager.cpp)
- [ ] New test cases for RES-01 (subscription limit)
- [ ] New test cases for RES-02 (bootstrap endpoint)
- [ ] New test cases for RES-03 (atomic capacity/quota in store_blob)
- [ ] New test cases for RES-04 (quota rebuild iterator)
- [ ] TSAN build verification for CORO-01

## Sources

### Primary (HIGH confidence)
- `db/peer/blob_push_manager.cpp` - verified pending_fetches_ leak at line 194 (only erases on accepted+ack)
- `db/peer/blob_push_manager.h:67-68` - verified hash-only key type
- `db/peer/connection_manager.cpp:97-109` - verified host-only bootstrap comparison
- `db/peer/message_dispatcher.cpp:222-233` - verified no subscription limit check
- `db/storage/storage.cpp:1366-1418` - verified quota rebuild iterator bug (to_next then erase)
- `db/engine/engine.cpp:114-226` - verified TOCTOU gap between capacity/quota checks and store_blob
- `db/storage/storage.cpp:339-457` - verified store_blob write transaction scope
- `db/sync/sync_protocol.cpp:21-40` - verified collect_namespace_hashes uses MDBX read txn
- `db/peer/peer_types.h:57-71` - verified NodeMetrics struct and comment

### Secondary (MEDIUM confidence)
- MDBX MVCC snapshot isolation - verified via `start_read()` call at storage.cpp:626, consistent with MDBX documentation
- Strand confinement of metrics increments - verified by auditing all 20+ increment sites across 5 source files

## Metadata

**Confidence breakdown:**
- Sync bugs (SYNC-01/02): HIGH - exact line numbers identified, bugs are obvious from code reading
- Sync snapshot (SYNC-03): HIGH - MDBX read transactions provide MVCC, verified in code
- Resource limits (RES-01/02/04): HIGH - straightforward bugs with clear fixes
- TOCTOU (RES-03): HIGH - race window identified between co_await points, fix pattern well understood
- Coroutine safety (CORO-01): HIGH - all increment sites audited, all on io_context strand; TSAN provides definitive verification

**Research date:** 2026-04-08
**Valid until:** 2026-05-08 (stable -- no external dependency changes)
