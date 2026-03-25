# Phase 62: Concurrent Dispatch - Research

**Researched:** 2026-03-25
**Domain:** Asio C++20 coroutine executor affinity, AEAD nonce safety, thread pool offload patterns
**Confidence:** HIGH

## Summary

Phase 62 transforms the request handler dispatch model so that heavy operations (Data, Delete, ReadRequest, ListRequest, StatsRequest) run their work concurrently on the thread pool while cheap operations (Subscribe, Unsubscribe, StorageFull, QuotaExceeded) execute inline on the IO thread. The critical invariant is that all `send_message` calls must happen on the IO thread because `send_encrypted()` increments `send_counter_` (the AEAD nonce) without synchronization.

The codebase already has a latent thread safety issue: after `co_await engine_.ingest()` (which internally uses `co_await crypto::offload(pool_, ...)`), the calling coroutine resumes on the thread pool thread -- not the IO thread. The coroutine then calls `conn->send_message()` from the pool thread, racing on `send_counter_`. This works today only because concurrent client requests (Read/List/Stats pipelining) are untested and uncommon. Phase 62 must fix this existing issue while enabling true concurrent dispatch.

The solution uses the established project pattern: `co_await asio::post(ioc_, asio::use_awaitable)` to transfer the coroutine back to the IO thread before calling `send_message`. This pattern is already used in `recv_sync_msg` (peer_manager.cpp line 909) for exactly this purpose.

**Primary recommendation:** For each handler coroutine that calls engine/storage methods (which internally offload to the pool), add `co_await asio::post(ioc_, asio::use_awaitable)` before every `send_message` call to guarantee IO thread affinity. Do NOT move storage/engine calls to explicit `offload()` -- they already offload internally where needed. The ReadRequest/ListRequest/StatsRequest handlers call synchronous storage methods (get_blob, get_blob_refs_since, get_namespace_quota) that are fast (MDBX memory-mapped reads) and NOT thread-safe, so they must also run on the IO thread.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CONC-03 | Request/response handlers for Read, List, and Stats are dispatched to the thread pool via the existing offload pattern, with responses sent back on the IO thread (AEAD nonce safety) | Analysis of current handler architecture shows: Data/Delete already offload crypto via `engine_.ingest()`/`engine_.delete_blob()` (which call `crypto::offload` internally). Read/List/Stats call synchronous storage methods that are fast but NOT thread-safe -- they must stay on IO thread. The core change is adding IO-thread transfer before `send_message` in Data/Delete handlers, and keeping Read/List/Stats on the IO thread (they are already fast). See Architecture Patterns section. |
| CONC-04 | Cheap operations (Ping, Pong, Goodbye, Subscribe, Unsubscribe, ExistsRequest, NodeInfoRequest) execute inline on the IO thread without offload overhead | Ping/Pong/Goodbye already handled inline in `Connection::message_loop()` (lines 760-783). Subscribe/Unsubscribe already execute inline in `on_peer_message` (no co_spawn). ExistsRequest and NodeInfoRequest are Phase 63 message types -- not yet implemented but CONC-04 establishes the inline pattern they will follow. No changes needed for existing cheap operations. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest via FetchContent | Coroutine executor, `asio::post`, `asio::use_awaitable` | Already in use; `co_await asio::post(ioc_, asio::use_awaitable)` is the executor transfer primitive |
| Catch2 | 3.7.1 | Unit/integration tests | Already in use; 500+ tests |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| spdlog | latest via FetchContent | Logging dispatch decisions | Already in use; log when offloaded vs inline |

No new libraries needed. All changes use existing dependencies.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `co_await asio::post(ioc_, use_awaitable)` | `asio::dispatch(ioc_, use_awaitable)` | `dispatch` runs inline if already on the right executor (optimization), but `post` always queues -- safer and more explicit. Use `post` for clarity since correctness is the priority. |
| Per-handler offload to pool | Keep current implicit offload via engine | Engine already offloads crypto internally. Adding explicit offload in handlers would double-dispatch and break Storage thread safety. Keep current architecture. |
| Mutex on send_counter_ | IO-thread-only send_message | Mutex would serialize all sends, defeating concurrency. IO-thread transfer is the correct pattern -- already established in codebase. |

## Architecture Patterns

### Current State: Handler Dispatch Model

```
on_peer_message()            [IO thread, synchronous callback]
  |
  +-- Subscribe/Unsubscribe  [IO thread, inline, no co_spawn]
  +-- StorageFull/QuotaExceeded [IO thread, inline]
  +-- SyncRequest             [IO thread, co_spawn -> sync coroutine]
  +-- Data/Delete             [IO thread, co_spawn -> engine.ingest() -> offload(pool) -> POOL THREAD -> send_message ON POOL THREAD (BUG)]
  +-- ReadRequest             [IO thread, co_spawn -> engine.get_blob() -> send_message ON IO THREAD (OK, no offload)]
  +-- ListRequest             [IO thread, co_spawn -> storage.get_blob_refs_since() -> send_message ON IO THREAD (OK)]
  +-- StatsRequest            [IO thread, co_spawn -> storage.get_namespace_quota() -> send_message ON IO THREAD (OK)]
```

### Target State: Safe Concurrent Dispatch

```
on_peer_message()            [IO thread, synchronous callback]
  |
  +-- Subscribe/Unsubscribe  [IO thread, inline, no co_spawn]  (CONC-04, unchanged)
  +-- StorageFull/QuotaExceeded [IO thread, inline]             (CONC-04, unchanged)
  +-- SyncRequest             [IO thread, co_spawn -> sync]     (unchanged, peer-only)
  +-- Data/Delete             [IO thread, co_spawn -> engine -> POOL THREAD -> post(ioc_) -> IO THREAD -> send_message]  (CONC-03, FIXED)
  +-- ReadRequest             [IO thread, co_spawn -> engine.get_blob() -> send_message]  (CONC-03, already correct)
  +-- ListRequest             [IO thread, co_spawn -> storage -> send_message]            (CONC-03, already correct)
  +-- StatsRequest            [IO thread, co_spawn -> storage/engine -> send_message]     (CONC-03, already correct)
```

### Pattern 1: IO Thread Transfer Before send_message
**What:** After any code path that may have run `co_await crypto::offload(pool_, ...)`, transfer back to the IO thread before calling `send_message`.
**When to use:** Data and Delete handler coroutines -- they call `engine_.ingest()` / `engine_.delete_blob()` which internally offload to the thread pool.
**Example:**
```cpp
// Source: existing pattern in recv_sync_msg (peer_manager.cpp:909)
// Data handler -- engine.ingest() offloads crypto to pool, resumes on pool thread
asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
    try {
        auto blob = wire::decode_blob(payload);
        // ... namespace filter ...
        auto result = co_await engine_.ingest(blob);
        // CRITICAL: After engine_.ingest(), we may be on the pool thread.
        // Transfer back to IO thread before send_message (AEAD nonce safety).
        co_await asio::post(ioc_, asio::use_awaitable);

        if (result.accepted && result.ack.has_value()) {
            // ... build ack_payload ...
            co_await conn->send_message(wire::TransportMsgType_WriteAck,
                                         std::span<const uint8_t>(ack_payload), request_id);
        }
        // ... rest of handler (notify_subscribers, metrics, strikes) ...
        // notify_subscribers accesses peers_ (IO-only) -- safe because we transferred back
    } catch (...) { ... }
}, asio::detached);
```

### Pattern 2: Read/List/Stats Stay on IO Thread (No Change Needed)
**What:** ReadRequest, ListRequest, and StatsRequest handlers call synchronous storage methods that are NOT thread-safe and fast (MDBX memory-mapped lookups). They do not call `crypto::offload`. They already run entirely on the IO thread.
**When to use:** Confirming correctness -- no code changes needed for these handlers.
**Why:** `engine_.get_blob()` is synchronous (no co_await, no offload). `storage_.get_blob_refs_since()` is synchronous. `storage_.get_namespace_quota()` is synchronous. The coroutine starts on the IO thread (via `co_spawn(ioc_, ...)`), never leaves it, and calls `send_message` on the IO thread. These are already correct.

### Pattern 3: Cheap Operations Stay Inline (CONC-04)
**What:** Subscribe, Unsubscribe, StorageFull (received), and QuotaExceeded (received) execute directly in `on_peer_message` without `co_spawn` -- they are fast in-memory operations (set insert/erase, flag update).
**When to use:** Message types that require no I/O, no crypto, and no response sending.
**Why:** These complete in microseconds. Thread pool dispatch overhead (context switch + queue) would dominate.

### Pattern 4: notify_subscribers Accesses peers_ (IO-Thread Only)
**What:** `notify_subscribers()` iterates `peers_` (a deque of PeerInfo) and calls `co_spawn(ioc_, ...)` for each subscriber. Since `peers_` is IO-thread-only state, `notify_subscribers` must be called from the IO thread.
**When to use:** After Data/Delete handlers transfer back to IO thread.
**Why:** Without the IO-thread transfer, `notify_subscribers` would iterate `peers_` from the pool thread -- a data race since `on_peer_connected`/`on_peer_disconnected` modify `peers_` on the IO thread.

### Anti-Patterns to Avoid
- **Wrapping ReadRequest/ListRequest/StatsRequest in offload():** Storage and Engine are NOT thread-safe. `engine_.get_blob()`, `storage_.get_blob_refs_since()`, `storage_.get_namespace_quota()`, `engine_.effective_quota()` must all run on the IO thread. Do NOT offload them to the pool.
- **Adding a mutex to send_counter_:** Serializes all sends per connection. The correct approach is IO-thread-only access. A mutex would also create a priority inversion risk where pool threads hold the lock while the IO thread waits.
- **Calling send_message from the pool thread "just this once":** Every call site must be on the IO thread. One exception creates a race that TSAN may not catch in every test run but will corrupt AEAD nonces in production.
- **Using dispatch() instead of post() for IO-thread transfer:** `asio::dispatch` runs inline if already on the target executor. This is an optimization that obscures the intent. Use `post()` to make the transfer explicit and always-queuing. The overhead is negligible (one ioc_ poll cycle).
- **Removing co_spawn from Data/Delete handlers:** The co_spawn is necessary because `engine_.ingest()` and `engine_.delete_blob()` are coroutines (they contain co_await internally). The handler must be a coroutine to use co_await.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| IO-thread transfer | Custom strand or mutex wrapper | `co_await asio::post(ioc_, asio::use_awaitable)` | Established Asio primitive, already used in codebase (recv_sync_msg), zero allocation overhead |
| Concurrent send serialization | Per-connection write queue | IO-thread-only send_message (single producer) | IO thread is already single-threaded -- no queue needed if all sends happen there |
| Request-response correlation | Custom tracking map | `request_id` field (Phase 61) | Already plumbed end-to-end; responses automatically correlate via echoed request_id |

**Key insight:** The concurrency model is NOT "dispatch work to pool, then respond" -- it is "coroutines run on IO thread, offload expensive computation to pool, transfer back to IO thread for state access and I/O." The IO thread remains the serialization point for all shared state (peers_, metrics_, storage_, engine_) and all AEAD nonce operations.

## Common Pitfalls

### Pitfall 1: send_message Called from Pool Thread After engine_.ingest()
**What goes wrong:** `engine_.ingest()` internally calls `co_await crypto::offload(pool_, ...)` which resumes the coroutine on a pool thread. The subsequent `conn->send_message()` call increments `send_counter_` without synchronization, causing AEAD nonce reuse or desync if another coroutine sends concurrently.
**Why it happens:** The `crypto::offload` documentation explicitly states "The coroutine resumes on the thread_pool thread after co_await." This is easy to forget because `co_spawn(ioc_, ...)` creates the illusion that the coroutine always runs on the IO thread.
**How to avoid:** Add `co_await asio::post(ioc_, asio::use_awaitable)` after `co_await engine_.ingest()` and `co_await engine_.delete_blob()`, before any `send_message` call.
**Warning signs:** TSAN data race on `send_counter_`. Intermittent AEAD decrypt failures on the remote side (nonce mismatch). Messages arriving with corrupt payloads.

### Pitfall 2: notify_subscribers Called from Pool Thread
**What goes wrong:** `notify_subscribers()` iterates `peers_` deque. If called from the pool thread (after engine offload), this races with `on_peer_connected`/`on_peer_disconnected` which modify `peers_` on the IO thread.
**Why it happens:** Data and Delete handlers call `notify_subscribers` after `engine_.ingest()` / `engine_.delete_blob()`. Without IO-thread transfer, these calls happen on the pool thread.
**How to avoid:** Same fix as Pitfall 1 -- transfer to IO thread before `send_message` and `notify_subscribers`. A single `co_await asio::post(ioc_, asio::use_awaitable)` covers both.
**Warning signs:** TSAN data race on `peers_`. Crash in `notify_subscribers` due to iterator invalidation. Missing notifications.

### Pitfall 3: Metrics Access from Pool Thread
**What goes wrong:** `++metrics_.ingests`, `++metrics_.rejections`, `record_strike()` etc. in Data/Delete handlers access IO-thread-only state from the pool thread.
**Why it happens:** These accesses are after `engine_.ingest()` returns, which may be on the pool thread.
**How to avoid:** Transfer to IO thread before ALL post-engine code, not just before `send_message`.
**Warning signs:** TSAN data race on `NodeMetrics` fields. Incorrect metric counts.

### Pitfall 4: Offloading Non-Thread-Safe Storage Methods
**What goes wrong:** Moving `storage_.get_blob_refs_since()` or `engine_.get_blob()` to the thread pool causes data races because Storage and BlobEngine are NOT thread-safe.
**Why it happens:** Developer assumes "offload = faster" without checking thread safety documentation.
**How to avoid:** Storage and Engine classes are documented as "NOT thread-safe. Caller must synchronize access." Do not call them from the pool. They are fast (memory-mapped reads) and don't need offloading.
**Warning signs:** TSAN reports on MDBX cursors. Database corruption. Intermittent crashes.

### Pitfall 5: Forgetting IO-Thread Transfer on Error Paths
**What goes wrong:** The early-return paths in Data/Delete handlers (namespace filter, error result, catch block) call `record_strike()` or access `metrics_` without transferring to the IO thread.
**Why it happens:** Only the "success" path gets the IO-thread transfer, but error paths also run after engine offload.
**How to avoid:** Place the `co_await asio::post(ioc_, asio::use_awaitable)` immediately after the engine call returns, BEFORE the result-checking if/else chain. This ensures ALL subsequent code (success and error) runs on the IO thread.
**Warning signs:** TSAN on `record_strike` or `metrics_` in error-path-only test scenarios.

### Pitfall 6: Breaking Sync Protocol Nonce Safety
**What goes wrong:** The sync protocol already has careful nonce management -- `peer->syncing` flag prevents concurrent sync coroutines, and the message_loop callback drops SyncRequest when syncing. Changing the dispatch model for sync messages could break this.
**Why it happens:** Over-zealous refactoring that moves sync message handling into the new dispatch model.
**How to avoid:** Do NOT change sync message dispatch. SyncRequest, SyncAccept, NamespaceList, BlobRequest, BlobTransfer, SyncComplete, PeerListRequest, PeerListResponse are peer-only protocol messages with their own concurrency model (sync_inbox queue + timer-cancel pattern). They are out of scope for CONC-03/CONC-04.
**Warning signs:** AEAD nonce desync during sync. Sync failures. "concurrent write" log messages.

## Code Examples

### Example 1: Data Handler with IO-Thread Transfer (Fixed)
```cpp
// Source: existing peer_manager.cpp Data handler (line 816-886), modified
if (type == wire::TransportMsgType_Data) {
    asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            auto blob = wire::decode_blob(payload);
            // Namespace filter: still on IO thread here (co_spawn(ioc_) just started)
            if (!sync_namespaces_.empty() &&
                sync_namespaces_.find(blob.namespace_id) == sync_namespaces_.end()) {
                spdlog::debug("dropping data for filtered namespace from {}",
                              conn->remote_address());
                co_return;
            }
            // engine_.ingest() offloads crypto to pool -- coroutine may resume on pool thread
            auto result = co_await engine_.ingest(blob);

            // CRITICAL: Transfer back to IO thread for all subsequent state access
            co_await asio::post(ioc_, asio::use_awaitable);

            // Everything below is now guaranteed to be on the IO thread:
            // - send_message (AEAD nonce safety)
            // - notify_subscribers (peers_ access)
            // - metrics_ updates
            // - record_strike()
            if (result.accepted && result.ack.has_value()) {
                auto ack = result.ack.value();
                std::vector<uint8_t> ack_payload(41);
                std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                for (int i = 7; i >= 0; --i) {
                    ack_payload[32 + (7 - i)] = static_cast<uint8_t>(
                        ack.seq_num >> (i * 8));
                }
                ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                co_await conn->send_message(wire::TransportMsgType_WriteAck,
                                             std::span<const uint8_t>(ack_payload), request_id);
            }
            // ... rest unchanged (notify_subscribers, metrics, error handling) ...
        } catch (const std::exception& e) {
            // catch block also on IO thread (exception thrown after post or before offload)
            spdlog::warn("malformed data message from peer {}: {}",
                         conn->remote_address(), e.what());
            record_strike(conn, e.what());
        }
    }, asio::detached);
    return;
}
```

### Example 2: Delete Handler with IO-Thread Transfer (Fixed)
```cpp
// Source: existing peer_manager.cpp Delete handler (line 632-678), modified
if (type == wire::TransportMsgType_Delete) {
    asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            auto blob = wire::decode_blob(payload);
            if (!sync_namespaces_.empty() &&
                sync_namespaces_.find(blob.namespace_id) == sync_namespaces_.end()) {
                co_return;
            }
            // engine_.delete_blob() offloads crypto to pool
            auto result = co_await engine_.delete_blob(blob);

            // Transfer back to IO thread
            co_await asio::post(ioc_, asio::use_awaitable);

            if (result.accepted && result.ack.has_value()) {
                // ... build ack_payload ...
                co_await conn->send_message(wire::TransportMsgType_DeleteAck,
                                             std::span<const uint8_t>(ack_payload), request_id);
                if (ack.status == engine::IngestStatus::stored) {
                    notify_subscribers(/* ... */);
                }
            } else if (result.error.has_value()) {
                record_strike(conn, result.error_detail);
            }
        } catch (const std::exception& e) {
            record_strike(conn, e.what());
        }
    }, asio::detached);
    return;
}
```

### Example 3: ReadRequest Handler (No Change Needed)
```cpp
// Source: existing peer_manager.cpp ReadRequest handler (line 697-728)
// This handler calls engine_.get_blob() which is synchronous (no offload).
// The coroutine starts on IO thread and never leaves. Already correct.
if (type == wire::TransportMsgType_ReadRequest) {
    asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            // ... parse namespace + hash ...
            auto blob = engine_.get_blob(ns, hash);  // Synchronous, IO thread
            // ... build response ...
            co_await conn->send_message(/* ... */);   // IO thread, safe
        } catch (...) { ... }
    }, asio::detached);
    return;
}
```

### Example 4: Inline Cheap Operations (No co_spawn)
```cpp
// Source: existing peer_manager.cpp Subscribe handler (line 604-616)
// No co_spawn, no coroutine, pure IO-thread inline execution.
// This is the CONC-04 pattern.
if (type == wire::TransportMsgType_Subscribe) {
    auto* peer = find_peer(conn);
    if (peer) {
        auto namespaces = decode_namespace_list(payload);
        for (const auto& ns : namespaces) {
            peer->subscribed_namespaces.insert(ns);
        }
    }
    return;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| All handlers co_spawn on ioc_, no explicit thread transfer | Data/Delete handlers add IO-thread transfer after engine calls | Phase 62 (this phase) | Fixes latent nonce race, enables safe concurrent pipelining |
| Sequential request processing (one at a time per connection) | Concurrent: message_loop reads next message while handler coroutines run in parallel | Phase 62 (this phase) | Clients can pipeline ReadRequest/ListRequest/StatsRequest |

**Key context:** The message_loop in Connection already supports concurrent dispatch -- it reads the next message immediately after dispatching via the `message_cb_` callback, which calls `on_peer_message` which co_spawns handler coroutines. The concurrent execution is already happening; Phase 62 makes it SAFE by ensuring all IO-thread-only state is accessed from the IO thread.

## Open Questions

1. **Should ReadRequest/ListRequest/StatsRequest also be offloaded?**
   - What we know: `engine_.get_blob()`, `storage_.get_blob_refs_since()`, `storage_.get_namespace_quota()` are synchronous MDBX reads. MDBX uses memory-mapped I/O, so these are typically microseconds for hot data. Storage is NOT thread-safe.
   - What's unclear: Whether large storage (millions of blobs) could make these slow enough to block the IO thread noticeably.
   - Recommendation: Keep on IO thread. Storage is not thread-safe, so offloading would require adding synchronization to Storage. This is out of scope (REQUIREMENTS.md: "Full worker pool model: Current offload pattern sufficient; can evolve later"). If blocking becomes a problem, it is a v1.4.0+ concern.

2. **Connection lifetime during concurrent handlers**
   - What we know: `conn` is `shared_ptr<Connection>`, captured by value in lambdas. The Connection is kept alive as long as any handler holds a reference.
   - What's unclear: What happens if the connection closes (peer disconnect) while a handler coroutine is mid-flight (between engine call and send_message)? `send_message` will fail (socket closed), which is fine -- the return value is checked (or ignored for co_spawn/detached).
   - Recommendation: No action needed. The existing `closed_` flag in Connection causes `send_encrypted` to fail gracefully. The handler coroutine will see send fail and exit.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | `db/CMakeLists.txt` (lines 197-245) |
| Quick run command | `cd build && ctest -R "test_peer_manager\|test_connection" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CONC-03 | Data handler send_message runs on IO thread after engine offload | TSAN build | `cd build && cmake --build . && ctest --output-on-failure` (with TSAN=ON) | Existing test infrastructure -- TSAN build catches thread violations |
| CONC-03 | ReadRequest/ListRequest/StatsRequest respond correctly to concurrent pipelined requests | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | db/tests/peer/test_peer_manager.cpp (exists, may need new test for pipelined requests) |
| CONC-04 | Subscribe/Unsubscribe execute inline without thread pool dispatch | unit | Code review + existing tests | Verified by absence of co_spawn in handler -- no separate test needed |
| CONC-03 | Responses arrive with correct request_id regardless of completion order | integration | `cd build && ctest -R test_peer_manager --output-on-failure` | Would need new test sending multiple concurrent requests with different request_ids |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . && ctest -R "test_peer_manager\|test_connection\|test_engine" --output-on-failure`
- **Per wave merge:** `cd build && cmake --build . && ctest --output-on-failure`
- **Phase gate:** Full suite green, plus TSAN build clean

### Wave 0 Gaps
- No new test files needed. The IO-thread transfer is a single-line fix per handler. TSAN catches violations automatically.
- Consider a concurrent pipelining test in test_peer_manager.cpp that sends multiple ReadRequests with different request_ids and verifies all responses arrive with correct request_ids. This would require a loopback test fixture.

## Sources

### Primary (HIGH confidence)
- Codebase direct inspection: `db/crypto/thread_pool.h` (lines 27-31: NOTE about pool thread resumption), `db/peer/peer_manager.cpp` (lines 906-909: existing `co_await asio::post(ioc_, use_awaitable)` pattern), `db/net/connection.cpp` (line 155: `send_counter_++` in `send_encrypted`), `db/engine/engine.cpp` (lines 159, 215, 232, 341, 350: all offload calls)
- Codebase thread safety docs: `db/storage/storage.h` (line 82: "NOT thread-safe"), `db/engine/engine.h` (line 65: "NOT thread-safe"), `db/peer/peer_manager.h` (line 90: "NOT thread-safe. Runs on single io_context thread.")
- Asio C++20 coroutine documentation: https://think-async.com/Asio/asio-1.20.0/doc/asio/overview/core/cpp20_coroutines.html -- executor binding for co_spawn

### Secondary (MEDIUM confidence)
- Asio GitHub issue #1112: https://github.com/chriskohlhoff/asio/issues/1112 -- confirms that mixing executors in coroutines causes memory safety violations

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new libraries, all changes use existing Asio primitives already in use in the codebase
- Architecture: HIGH - the IO-thread transfer pattern is already established in `recv_sync_msg`; the latent bug analysis is based on explicit developer comments in `thread_pool.h` and verified by the compensating `post()` call in sync code
- Pitfalls: HIGH - derived from direct code analysis of thread ownership at each point in the handler coroutine lifecycle

**Research date:** 2026-03-25
**Valid until:** 2026-04-25 (stable -- no external dependencies changing, all patterns are internal)
