# Stack Research: v2.0.0 Event-Driven Architecture

**Project:** chromatindb -- push-based sync, event-driven expiry, SDK auto-reconnect, bidirectional keepalive
**Researched:** 2026-04-02
**Confidence:** HIGH

## Scope

This research covers ONLY what the v2.0.0 milestone adds or changes in the existing stack. The validated stack (Standalone Asio 1.38.0, libmdbx, FlatBuffers, Python SDK with asyncio/PyNaCl/liboqs-python) is shipped and NOT re-researched. Focus: Asio patterns for push notifications and event-driven timers, libmdbx ordered key lookups for next-expiry, asyncio reconnect patterns for the SDK, and bidirectional keepalive on both C++ and Python sides.

## Verdict: Zero New Dependencies

**No new C++ or Python packages are needed.** Every capability required for v2.0.0 is already available in the existing stack.

| Capability | Provider | Already Available? | Notes |
|------------|----------|--------------------|-------|
| Push notification to peers after ingest | `asio::io_context` single-thread dispatch | YES | `notify_subscribers()` already fans out; extend to peer-level notifications |
| New wire message types (BlobNotify, BlobFetch) | FlatBuffers `transport.fbs` enum extension | YES | Add 2-4 new enum values to `TransportMsgType` |
| Event-driven expiry timer (next-deadline) | `asio::steady_timer` with `expires_after()` reschedule | YES | Replace fixed-interval scan loop with dynamic deadline |
| Next-expiry-time query from storage | libmdbx `cursor.to_first()` on `expiry_map` | YES | Keys are `[expiry_ts_be:8][hash:32]` -- first key IS the earliest expiry |
| Disconnect-triggered cursor cleanup | `on_peer_disconnected()` callback | YES | Already exists; replace 6h timer with immediate action |
| SDK auto-reconnect with backoff | Python `asyncio.sleep()` + exponential backoff | YES | Stdlib only; no new pip deps |
| SDK transparent request retry | Python `asyncio` exception handling + re-dispatch | YES | Wrap `_request_with_timeout` with retry logic |
| Bidirectional keepalive (C++ node) | `asio::steady_timer` periodic Ping sender | YES | Timer-cancel pattern already used for 8+ coroutines |
| Bidirectional keepalive (Python SDK) | `asyncio.Task` periodic Ping sender | YES | Transport already has `send_ping()` and `_send_pong()` |
| Safety-net reconciliation (background) | `asio::steady_timer` with long interval | YES | Same pattern as existing `sync_timer_loop()` |

## C++ Node: Asio Patterns for New Features

### 1. Push-Based Peer Sync Notifications

**Pattern:** Extend existing `notify_subscribers()` fan-out to all connected peers (not just pub/sub subscribers).

The current architecture already has the exact mechanism needed:
1. Blob ingested via `engine_.ingest()` -> WriteAck sent
2. `notify_subscribers()` already fires for pub/sub subscribers after ingest
3. For push sync: add a parallel `notify_peers()` call that sends a new `BlobNotify` message to ALL connected peers (not just subscribers)

**Why this works with existing Asio patterns:**
- `PeerManager` runs on single `io_context` thread -- no locking needed
- `peers_` deque already iterated in `notify_subscribers()`
- `co_spawn(ioc_, ...)` with `asio::detached` for async send per peer (proven pattern in `notify_subscribers`)
- Notification is fire-and-forget -- no response expected, no request_id

**New wire types needed:**
```
BlobNotify = 59,     // Peer -> Peer: "I have this blob"
BlobFetchRequest = 60,   // Peer -> Peer: "Send me this blob"
BlobFetchResponse = 61   // Peer -> Peer: "Here is the blob"
```

`BlobNotify` payload: `[namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]`
-- identical to existing `Notification` payload format (77 bytes). Reuse `encode_notification()`.

`BlobFetchRequest` payload: `[namespace_id:32][blob_hash:32]` (64 bytes)

`BlobFetchResponse` payload: FlatBuffer-encoded blob (same as `BlobTransfer` in sync). Or a 1-byte "not found" status.

**Why NOT use `asio::experimental::channel`:**
Although Asio 1.38.0 ships `experimental::channel<>` and `experimental::concurrent_channel<>`, they are unnecessary here because:
1. The codebase runs on a single `io_context` thread -- no producer/consumer coordination needed
2. The existing `co_spawn` + detached fire-and-forget pattern is proven in 78 phases
3. Channels add complexity (buffer sizing, backpressure semantics) for zero benefit
4. The `experimental::` namespace signals instability -- the API may change

**Use `asio::experimental::channel` only if** a future milestone introduces multi-io_context architecture (currently: YAGNI).

### 2. Event-Driven Expiry Timer

**Pattern:** Replace fixed-interval `expiry_scan_loop()` with a dynamic deadline timer that fires at exactly the next expiry time.

**Current implementation (to be replaced):**
```cpp
// Polls every expiry_scan_interval_seconds_ (default 60s)
asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        timer.expires_after(std::chrono::seconds(expiry_scan_interval_seconds_));
        co_await timer.async_wait(...);
        storage_.run_expiry_scan();
    }
}
```

**New implementation pattern:**
```cpp
asio::awaitable<void> PeerManager::expiry_timer_loop() {
    while (!stopping_) {
        auto next = storage_.get_next_expiry_time();  // NEW Storage API
        if (!next.has_value()) {
            // No blobs with TTL -- sleep for a long interval, reschedule on ingest
            timer.expires_after(std::chrono::hours(1));
        } else {
            auto now = storage::system_clock_seconds();
            auto delay = (*next > now) ? (*next - now) : 0;
            timer.expires_after(std::chrono::seconds(delay));
        }
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        storage_.run_expiry_scan();  // Purge all expired blobs
        // Loop re-reads next expiry time
    }
}
```

**Reschedule trigger:** After every `store_blob()` with TTL > 0, check if the new blob's expiry is earlier than the current timer deadline. If yes, cancel the timer to force re-evaluation:
```cpp
if (expiry_timer_ && new_expiry < current_expiry_deadline_) {
    expiry_timer_->cancel();  // Wakes coroutine, re-reads next expiry
}
```

**New Storage API needed:**
```cpp
/// Return the earliest expiry timestamp, or nullopt if no blobs have TTL.
/// O(1): reads first key from expiry_map B-tree cursor.
std::optional<uint64_t> get_next_expiry_time();
```

This is trivially implemented using `cursor.to_first()` on `expiry_map` -- the first key's first 8 bytes (big-endian) are the earliest expiry timestamp. The expiry_map is already keyed as `[expiry_ts_be:8][hash:32]`, so B-tree ordering gives us the earliest expiry for free. This is a read-only operation in a read transaction -- zero overhead.

**Why `steady_timer` and not `system_timer`:**
The existing codebase exclusively uses `asio::steady_timer` (monotonic clock). Expiry times are stored as wall-clock seconds, but the timer only needs to know "how many seconds until expiry" (relative delay), not an absolute deadline. `expires_after(seconds(delay))` works correctly with `steady_timer`.

### 3. Disconnect-Triggered Cursor Cleanup

**Pattern:** Move cursor compaction from 6-hour timer to immediate action in `on_peer_disconnected()`.

**Current implementation:**
```cpp
asio::awaitable<void> PeerManager::cursor_compaction_loop() {
    while (!stopping_) {
        timer.expires_after(std::chrono::hours(6));
        co_await timer.async_wait(...);
        // Prune cursors for disconnected peers
    }
}
```

**New implementation:** Delete `cursor_compaction_loop()` entirely. In `on_peer_disconnected()`:
```cpp
void PeerManager::on_peer_disconnected(net::Connection::Ptr conn) {
    // ... existing cleanup ...
    // Compact cursors for this peer immediately
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    storage_.delete_peer_cursors(peer_hash);
}
```

`delete_peer_cursors()` already exists in `Storage` and is O(n) in number of namespaces for that peer -- typically small. No Asio pattern change needed; just call it synchronously from the existing callback.

### 4. Bidirectional Keepalive (C++ Node Side)

**Pattern:** Add a periodic Ping sender coroutine alongside the existing receiver-side inactivity timeout.

**Current state:** Node has receiver-side inactivity detection only (disconnect if no message received for N seconds). There is NO active Ping sending from the node.

**New addition:**
```cpp
asio::awaitable<void> PeerManager::keepalive_loop() {
    while (!stopping_) {
        timer.expires_after(std::chrono::seconds(keepalive_interval_seconds_));
        co_await timer.async_wait(...);
        // Send Ping to all connected peers
        for (auto& peer : peers_) {
            co_spawn(ioc_, send_keepalive_ping(peer.connection), asio::detached);
        }
    }
}
```

**Keepalive interval:** Should be less than `inactivity_timeout_seconds / 2` to ensure the remote side sees traffic before its timeout fires. With default `inactivity_timeout_seconds = 120`, a 30-second keepalive interval is appropriate. This matches the existing 30-second inactivity check sweep interval.

**Wire protocol:** Uses existing `Ping`/`Pong` messages (types 5/6). No new message types needed.

**Interaction with existing inactivity check:**
- `inactivity_check_loop()` remains as-is -- it detects dead peers that don't respond to Pings
- `keepalive_loop()` ensures both sides see traffic, enabling faster dead-connection detection
- `last_message_time` is already updated at top of `on_peer_message()` for ALL message types including Pong

### 5. Reconcile-On-Connect

**Pattern:** Trigger full reconciliation when a peer connects, instead of waiting for the next sync timer tick.

In `on_peer_connected()`, after ACL check:
```cpp
asio::co_spawn(ioc_, run_sync_with_peer(conn), asio::detached);
```

This already happens implicitly in the current architecture (sync timer fires periodically), but making it explicit on connect reduces initial sync delay from 0-60s to immediate.

### 6. Safety-Net Reconciliation

**Pattern:** Keep a low-frequency background reconciliation timer (10-15 minutes) as a monitoring safety net.

```cpp
// Replace current sync_timer_loop() 60s interval with 600-900s
timer.expires_after(std::chrono::seconds(safety_net_reconciliation_seconds_));
```

This is the existing `sync_timer_loop()` with a longer interval. The `sync_interval_seconds` config field already supports this -- just change the default from 60 to 600.

## Python SDK: Auto-Reconnect and Keepalive

### 7. SDK Auto-Reconnect with Transparent Retry

**Pattern:** Wrap `ChromatinClient` connection lifecycle with automatic reconnection on connection loss. No new pip dependencies.

**Architecture decision: Reconnect inside `ChromatinClient`, NOT in `Transport`.**

Reconnection requires re-establishing the TCP connection and re-performing the PQ handshake (new AEAD keys, new nonce counters). This means creating a new `Transport` instance entirely -- the old one is dead. Therefore, reconnect logic belongs in `ChromatinClient` which owns the connection parameters.

**Implementation pattern:**
```python
class ChromatinClient:
    def __init__(self, transport: Transport) -> None:
        self._transport = transport
        self._subscriptions: set[bytes] = set()
        # Reconnect state
        self._reconnect_enabled = False
        self._max_reconnect_attempts = 0  # 0 = unlimited
        self._reconnect_delay = 1.0  # Starting backoff (seconds)
        self._max_reconnect_delay = 60.0  # Cap
        self._reconnecting = False

    async def _reconnect(self) -> None:
        """Re-establish connection with exponential backoff + jitter."""
        delay = self._reconnect_delay
        attempts = 0
        while True:
            attempts += 1
            if self._max_reconnect_attempts > 0 and attempts > self._max_reconnect_attempts:
                raise ChromatinConnectionError("max reconnect attempts exceeded")
            try:
                await self._connect()  # TCP + handshake
                await self._restore_subscriptions()  # Re-subscribe
                return
            except Exception:
                jitter = random.uniform(0, delay * 0.25)
                await asyncio.sleep(delay + jitter)
                delay = min(delay * 2, self._max_reconnect_delay)
```

**Transparent retry for requests:**
```python
async def _request_with_retry(self, msg_type: int, payload: bytes) -> tuple[int, bytes]:
    """Send request, reconnect on failure, retry once."""
    try:
        return await self._request_with_timeout(msg_type, payload)
    except ChromatinConnectionError:
        if not self._reconnect_enabled:
            raise
        await self._reconnect()
        return await self._request_with_timeout(msg_type, payload)
```

**Key decisions:**
- **One retry after reconnect, not infinite retries.** If the retry also fails, raise to caller. Prevents silent infinite loops.
- **Re-subscribe after reconnect.** `self._subscriptions` tracks active subscriptions; replay them on the new connection. This is why subscriptions are connection-scoped.
- **Jitter: 0-25% of delay.** Matches the AWS Full Jitter algorithm. Prevents thundering herd if multiple SDK clients reconnect simultaneously.
- **No new pip deps.** `random` and `asyncio.sleep` are stdlib.

**`connect()` factory change:**
```python
@classmethod
def connect(
    cls,
    host: str,
    port: int,
    identity: Identity,
    *,
    timeout: float = 10.0,
    auto_reconnect: bool = False,
    max_reconnect_attempts: int = 0,
    reconnect_delay: float = 1.0,
    max_reconnect_delay: float = 60.0,
) -> ChromatinClient:
```

### 8. SDK Bidirectional Keepalive

**Pattern:** Background `asyncio.Task` that sends periodic Ping to keep the connection alive and detect dead servers.

```python
async def _keepalive_loop(self) -> None:
    """Periodic Ping sender for dead connection detection."""
    while not self._transport.closed:
        await asyncio.sleep(self._keepalive_interval)
        try:
            await asyncio.wait_for(self._transport.send_ping(), timeout=10.0)
        except (ChromatinConnectionError, asyncio.TimeoutError):
            if self._reconnect_enabled:
                await self._reconnect()
            else:
                break
```

**Transport already handles Pong response** -- the `_reader_loop` resolves the pending ping future when Pong arrives. No changes to `Transport` needed.

**Default interval:** 30 seconds, matching the C++ node's keepalive/inactivity check interval.

## Storage Layer: New API for Event-Driven Expiry

### `get_next_expiry_time()` Implementation

```cpp
std::optional<uint64_t> Storage::get_next_expiry_time() {
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->expiry_map);
        auto first = cursor.to_first(false);
        if (!first.done) return std::nullopt;  // No entries
        auto key = cursor.current(false).key;
        if (key.length() < 8) return std::nullopt;
        return decode_be_u64(static_cast<const uint8_t*>(key.data()));
    } catch (const std::exception& e) {
        spdlog::error("get_next_expiry_time error: {}", e.what());
        return std::nullopt;
    }
}
```

**Why this is O(1):** libmdbx stores keys in sorted B-tree order. `cursor.to_first()` navigates to the leftmost leaf -- a single tree traversal, O(log n) in B-tree depth but effectively O(1) for practical database sizes (libmdbx B-tree depth is typically 2-4 levels for millions of entries). The expiry_map key prefix is `[expiry_ts_be:8]` in big-endian, so the smallest timestamp is always the first key.

**No index changes needed.** The existing `expiry_map` with `[expiry_ts_be:8][hash:32]` key format already provides sorted-by-timestamp ordering. This is the same B-tree the existing `run_expiry_scan()` iterates -- we are just reading the first entry without scanning.

## Wire Protocol Changes

### New Message Types

| Type | Value | Direction | Purpose |
|------|-------|-----------|---------|
| `BlobNotify` | 59 | Peer -> Peer | Push notification: "I have blob X" |
| `BlobFetchRequest` | 60 | Peer -> Peer | "Send me blob X" |
| `BlobFetchResponse` | 61 | Peer -> Peer | Blob data or not-found |

**Total after v2.0.0:** 61 message types (up from 58 in v1.5.0).

**BlobNotify payload format:** Identical to existing `Notification` (type 21) payload:
`[namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]` (77 bytes)

Re-using the same format means `encode_notification()` and `decode_notification()` work for both pub/sub (client) notifications and peer sync notifications with zero code duplication.

**BlobFetchRequest payload format:**
`[namespace_id:32][blob_hash:32]` (64 bytes)

Same format as `ReadRequest` payload but routed peer-to-peer instead of client-to-node.

**BlobFetchResponse payload format:**
- Success: `[status:1=0x00][flatbuffer_encoded_blob:N]`
- Not found: `[status:1=0x01]`

The status byte distinguishes "here is the blob" from "I don't have it" without needing separate message types.

### Relay Filter Update

The relay message filter (blocklist) must be updated:
- `BlobNotify` (59): BLOCK -- peer-internal, not client-facing
- `BlobFetchRequest` (60): BLOCK -- peer-internal
- `BlobFetchResponse` (61): BLOCK -- peer-internal

These are peer-to-peer sync primitives and must not pass through the relay to clients. The relay's blocklist approach (block known peer types, allow everything else) means these MUST be explicitly added to the block list.

## Configuration Changes

### New Config Fields

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `keepalive_interval_seconds` | `uint32_t` | 30 | Ping interval for bidirectional keepalive |
| `safety_net_reconciliation_seconds` | `uint32_t` | 600 | Background full-reconciliation interval (replaces sync_interval_seconds for pull-based sync) |

### Modified Config Fields

| Field | Old Default | New Default | Reason |
|-------|-------------|-------------|--------|
| `sync_interval_seconds` | 60 | Repurposed or removed | Replaced by push notifications + safety-net timer |
| `expiry_scan_interval_seconds` | 60 | Removed | Replaced by event-driven next-expiry timer |

### Removed Timer Loops

| Timer | Current | Replacement |
|-------|---------|-------------|
| `cursor_compaction_loop()` | 6-hour periodic | Immediate on `on_peer_disconnected()` |
| `expiry_scan_loop()` | 60s periodic scan | `expiry_timer_loop()` with dynamic deadline |
| `sync_timer_loop()` | 60s periodic sync | Safety-net at 600s; primary sync is push-based |

### New Timer Loops

| Timer | Interval | Purpose |
|-------|----------|---------|
| `keepalive_loop()` | 30s | Send Ping to all peers |
| `expiry_timer_loop()` | Dynamic (next expiry time) | Fire at exact expiry deadline |
| `safety_net_reconciliation_loop()` | 600s | Full reconciliation as monitoring signal |

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| Fire-and-forget `co_spawn` for peer notifications | `asio::experimental::channel<>` | Single io_context thread -- no producer/consumer needed; channel is experimental and adds complexity for zero benefit |
| `steady_timer` with dynamic `expires_after()` for expiry | Dedicated expiry watcher thread | Single-threaded Asio model is proven; adding threads would require mutex on storage access |
| Reconnect in `ChromatinClient` (owns connection params) | Reconnect in `Transport` (lower level) | Transport is stateless after handshake; reconnection requires new TCP + new PQ handshake = new Transport |
| Reuse `Notification` payload format for `BlobNotify` | New dedicated format | Identical fields; zero code duplication in encoder/decoder |
| Immediate cursor cleanup on disconnect | Keep 6-hour timer with shorter interval | Timers waste CPU when the exact event (disconnect) is already observable |
| 30s keepalive with 120s inactivity timeout | Configurable keepalive with auto-derived timeout | KISS -- 30s/120s is well-proven in production systems (SSH default is 15s/45s) |
| `BlobFetchResponse` with status byte | Separate `BlobFetchNotFound` message type | One message type with status byte is simpler than two types; matches `WriteAck` pattern |
| stdlib `random` for jitter | `secrets` module | Jitter is not a security-critical random value; `random` is faster and clearer |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| `asio::experimental::channel<>` | Still experimental in Asio 1.38.0; API may change; unnecessary for single-threaded dispatch | Direct `co_spawn` per-peer fan-out |
| `asio::system_timer` / `asio::deadline_timer` | Wall-clock timers affected by NTP/manual adjustments; codebase exclusively uses `steady_timer` | `asio::steady_timer` with `expires_after()` relative delay |
| Python `tenacity` or `backoff` libraries | New pip dependency for something achievable in ~20 lines of stdlib | Stdlib `asyncio.sleep` + `random.uniform` |
| Dedicated watcher thread for expiry | Breaks single-threaded invariant; requires mutex on storage | `steady_timer` reschedule in io_context thread |
| `asyncio.Queue` for reconnect signaling | Over-engineering; reconnect is triggered by exception in request path | Direct `try/except` in `_request_with_retry` |
| WebSocket keepalive frames | Not applicable -- custom binary protocol over TCP | Existing `Ping`/`Pong` message types (5/6) |

## Version Compatibility

| Component | Current Version | Required Changes | Compatibility Notes |
|-----------|-----------------|------------------|---------------------|
| Standalone Asio | 1.38.0 | None | All features used (steady_timer, co_spawn, use_awaitable) are stable, non-experimental |
| libmdbx | latest via FetchContent | None | cursor.to_first() on sorted map is core API since inception |
| FlatBuffers | latest via FetchContent | Add 3 enum values to transport.fbs | Backward-incompatible wire change; acceptable per project constraints |
| liboqs-python | ~=0.14.0 | None | Not involved in v2.0.0 changes |
| PyNaCl | ~=1.5.0 | None | Not involved in v2.0.0 changes |
| flatbuffers (Python) | ~=25.12 | Regenerate transport_generated.py | Auto-generated from transport.fbs |
| Python | >=3.10 | None | asyncio, random, secrets all in stdlib |

## Sources

- Standalone Asio [1.38.0 documentation](https://think-async.com/Asio/asio-1.38.0/doc/) -- steady_timer, co_spawn, use_awaitable (verified in project's FetchContent build)
- Asio experimental::channel [1.30.2 reference](https://think-async.com/Asio/asio-1.30.2/doc/asio/reference/experimental__concurrent_channel.html) -- evaluated and rejected for this use case
- Asio [steady_timer reference](https://think-async.com/Asio/asio-1.36.0/doc/asio/reference/steady_timer.html) -- expires_after, cancel, expires_at behavior verified
- libmdbx [GitHub repository](https://github.com/erthink/libmdbx) -- cursor.to_first() B-tree traversal confirmed
- AWS [Exponential Backoff And Jitter](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/) -- Full Jitter algorithm referenced for SDK reconnect
- Existing codebase: `db/peer/peer_manager.h` (timer-cancel pattern, notify_subscribers), `db/storage/storage.cpp` (expiry_map key format), `sdk/python/chromatindb/_transport.py` (Ping/Pong handling), `sdk/python/chromatindb/client.py` (subscription tracking, connect pattern)

---
*Stack research for: v2.0.0 Event-Driven Architecture*
*Researched: 2026-04-02*
