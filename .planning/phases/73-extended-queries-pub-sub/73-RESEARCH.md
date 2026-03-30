# Phase 73: Extended Queries & Pub/Sub - Research

**Researched:** 2026-03-30
**Domain:** Python SDK binary protocol encode/decode, async pub/sub, typed result dataclasses
**Confidence:** HIGH

## Summary

Phase 73 extends the Python SDK from 6 client methods (Phase 72) to 16+ methods covering all 38 client-facing message types. The work divides into three categories: (1) ten query/introspection methods with new codec encode/decode functions and typed result dataclasses, (2) pub/sub lifecycle (subscribe, unsubscribe, notification consumer), and (3) extending `__init__.py` exports.

All wire formats are binary, big-endian (consistent with Phase 72 patterns). The existing `_codec.py` + `types.py` + `client.py` architecture scales directly -- each new query follows the same pattern: encode payload via struct.pack, send via `_request_with_timeout()`, decode response into a frozen dataclass. Pub/sub is architecturally different -- it uses the transport's existing `_notifications` queue (request_id=0 routing) with a new async iterator consumer.

**Primary recommendation:** Implement in three waves: (1) codec + types for all 10 query types, (2) client methods for all 10 queries, (3) pub/sub lifecycle. Keep all codec functions in the existing `_codec.py` (it grows from ~300 to ~800 lines, still manageable for a single-domain module).

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Async iterator pattern -- `async for notification in conn.notifications():`. Lazy, backpressure-friendly, Pythonic. Consumes from existing transport `_notifications` queue.
- **D-02:** Single merged stream for all subscriptions. One `conn.notifications()` yields notifications across all subscribed namespaces. Each notification carries its namespace.
- **D-03:** Notification dataclass: `Notification(namespace: bytes, blob_hash: bytes, seq_num: int)`. Minimal -- user calls `read_blob()` if they want the data.
- **D-04:** `subscribe()` awaits server confirmation before returning. Round-trip ensures subscription is active. **CAVEAT: See Open Questions #1 -- C++ node does not send a response for Subscribe messages.**
- **D-05:** Client tracks active subscriptions in a set. `conn.subscriptions` property returns frozenset. Prevents duplicate subscribes, enables clean unsubscribe-all on disconnect.
- **D-06:** Auto-cleanup on disconnect -- client sends Unsubscribe for all tracked namespaces during graceful shutdown (Goodbye sequence).
- **D-07:** `list[bytes]` input for batch_exists and batch_read. Consistent with bytes-only convention (Phase 72 D-05).
- **D-08:** `batch_exists()` returns `dict[bytes, bool]` -- maps each hash to existence status. O(1) lookup by hash.
- **D-09:** `batch_read()` returns `BatchReadResult(blobs: dict[bytes, ReadResult | None], truncated: bool)`. Honest about partial results via truncation flag.
- **D-10:** No client-side batch size limits. Server enforces its own limits; truncation flag handles overflow. YAGNI.
- **D-11:** All result types in single `types.py` module. Currently 5 types (Phase 72), will grow to ~18. Still manageable, one import location.
- **D-12:** Python-native types for introspection results -- `str` for version/address, `list[str]` for supported types, `int` for counts. SDK is the translation layer.
- **D-13:** All new result types exported from `chromatindb` top-level `__init__.py` in `__all__`. Consistent with Phase 72 pattern.

### Claude's Discretion
- Codec function organization for 10+ new encode/decode functions (extend _codec.py or split)
- Exact field names/types for each introspection result type (derived from C++ wire format)
- Integration test organization for 10+ new query types
- Whether metadata query returns full ReadResult or a lighter MetadataResult type
- TimeRangeResult structure (reuse ListPage pattern or new type)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| QUERY-01 | SDK queries blob metadata without payload (MetadataRequest/MetadataResponse) | Wire format: [ns:32][hash:32] request, [found:1][hash:32][ts:8BE][ttl:4BE][size:8BE][seq:8BE][pk_len:2BE][pk:N] response |
| QUERY-02 | SDK batch-checks blob existence (BatchExistsRequest/BatchExistsResponse) | Wire format: [ns:32][count:4BE][hash:32*N] request, [exists:1*N] response |
| QUERY-03 | SDK batch-reads multiple blobs (BatchReadRequest/BatchReadResponse) | Wire format: [ns:32][cap:4BE][count:4BE][hash:32*N] request, [trunc:1][count:4BE][entries...] response |
| QUERY-04 | SDK queries blobs by time range (TimeRangeRequest/TimeRangeResponse) | Wire format: [ns:32][start:8BE][end:8BE][limit:4BE] request, [trunc:1][count:4BE][entries...48*N] response |
| QUERY-05 | SDK lists namespaces (NamespaceListRequest/NamespaceListResponse) | Wire format: [after_ns:32][limit:4BE] request, [count:4BE][has_more:1][entries...40*N] response |
| QUERY-06 | SDK queries per-namespace stats (NamespaceStatsRequest/NamespaceStatsResponse) | Wire format: [ns:32] request, 41-byte response with found flag + 5 uint64 fields |
| QUERY-07 | SDK queries storage status (StorageStatusRequest/StorageStatusResponse) | Wire format: empty request, 44-byte fixed response |
| QUERY-08 | SDK queries node info and capabilities (NodeInfoRequest/NodeInfoResponse) | Wire format: empty request, variable-length response with length-prefixed strings |
| QUERY-09 | SDK queries peer info (PeerInfoRequest/PeerInfoResponse) | Wire format: empty request, trust-gated response (8-byte summary or full detail) |
| QUERY-10 | SDK lists delegations (DelegationListRequest/DelegationListResponse) | Wire format: [ns:32] request, [count:4BE][entries...64*N] response |
| PUBSUB-01 | SDK subscribes to namespace notifications (Subscribe) | Wire format: [count:2BE][ns:32*N], no server response (fire-and-forget) |
| PUBSUB-02 | SDK unsubscribes from namespace notifications (Unsubscribe) | Wire format: [count:2BE][ns:32*N], no server response (fire-and-forget) |
| PUBSUB-03 | SDK receives and dispatches notification callbacks (Notification) | Wire format: 77-byte payload [ns:32][hash:32][seq:8BE][size:4BE][tombstone:1] |

</phase_requirements>

## Standard Stack

### Core (already installed)
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| struct (stdlib) | n/a | Big-endian binary encode/decode | All wire payloads use struct.pack/unpack |
| asyncio (stdlib) | n/a | Async iterator for notifications, Queue | Foundation for pub/sub consumer |
| dataclasses (stdlib) | n/a | Frozen result types | Existing pattern from Phase 72 |
| flatbuffers | ~=25.12 | TransportMessage envelope (FlatBuffer) | Existing, envelope only |

### Supporting (already installed)
No new dependencies required. Phase 73 is pure Python binary protocol work using stdlib `struct`.

## Architecture Patterns

### Recommended Module Changes
```
sdk/python/chromatindb/
  _codec.py        # EXTEND: +10 encode functions, +10 decode functions
  types.py         # EXTEND: +13 new frozen dataclasses
  client.py        # EXTEND: +10 query methods, +subscribe/unsubscribe/notifications
  _transport.py    # MINOR: add send_message() for fire-and-forget (pub/sub)
  __init__.py      # EXTEND: export all new types
sdk/python/tests/
  test_codec.py    # EXTEND: test all new encode/decode
  test_types.py    # EXTEND: test all new types
  test_client_ops.py  # EXTEND: test all new client methods (mock transport)
  test_integration.py # EXTEND: integration tests for queries + pub/sub
```

### Pattern 1: Query Method (established Phase 72 pattern)
**What:** Each query follows: validate args, encode payload, send_request, check response type, decode, return typed result.
**When to use:** All 10 QUERY-* requirements.
**Example:**
```python
# Source: sdk/python/chromatindb/client.py (existing pattern)
async def metadata(
    self, namespace: bytes, blob_hash: bytes
) -> MetadataResult | None:
    payload = encode_metadata_request(namespace, blob_hash)
    resp_type, resp_payload = await self._request_with_timeout(
        TransportMsgType.MetadataRequest, payload
    )
    if resp_type != TransportMsgType.MetadataResponse:
        raise ProtocolError(
            f"expected MetadataResponse (48), got type {resp_type}"
        )
    return decode_metadata_response(resp_payload)
```

### Pattern 2: Fire-and-Forget Send (new for pub/sub)
**What:** Subscribe/Unsubscribe are sent without expecting a response from the server.
**When to use:** PUBSUB-01, PUBSUB-02.
**Example:**
```python
# New: send without send_request (no response expected)
async def subscribe(self, namespace: bytes) -> None:
    if len(namespace) != 32:
        raise ValueError(f"namespace must be 32 bytes, got {len(namespace)}")
    payload = encode_subscribe([namespace])
    await self._transport.send_message(
        TransportMsgType.Subscribe, payload
    )
    self._subscriptions.add(namespace)
```

### Pattern 3: Async Iterator for Notifications (D-01)
**What:** `notifications()` yields `Notification` objects from the transport queue.
**When to use:** PUBSUB-03.
**Example:**
```python
async def notifications(self):
    """Yield notifications as an async iterator (D-01, D-02)."""
    while not self._transport.closed:
        try:
            msg_type, payload, _ = await asyncio.wait_for(
                self._transport.notifications.get(),
                timeout=1.0,
            )
        except asyncio.TimeoutError:
            continue
        if msg_type == TransportMsgType.Notification:
            yield decode_notification(payload)
```

### Anti-Patterns to Avoid
- **Using send_request for Subscribe/Unsubscribe:** The C++ node does NOT send responses for these message types. Using `send_request()` would cause the future to hang until timeout. Must use fire-and-forget send instead.
- **Blocking the notification iterator:** The async iterator must not block indefinitely on queue.get() -- use a timeout loop so the iterator exits cleanly when the transport closes.
- **Mixing Pong and Notification dispatch:** Both have request_id=0. The transport reader already special-cases Pong (line 79-83 in _transport.py). Notifications reach the `_notifications` queue via the else branch (line 91-98). This routing is correct and must not be changed.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Binary encode/decode | Custom bit manipulation | `struct.pack(">Q", val)` / `struct.unpack` | Consistent with existing codec, handles endianness |
| Async iteration protocol | Custom `__aiter__`/`__anext__` | `async def notifications()` generator function | Simpler, async generator with `yield` is sufficient |
| Subscription tracking | Complex state machine | `set[bytes]` + frozenset property | D-05 is deliberately simple |

## Common Pitfalls

### Pitfall 1: Subscribe Timeout
**What goes wrong:** Using `send_request()` for Subscribe causes a hang/timeout because the C++ node processes Subscribe inline without sending a response.
**Why it happens:** D-04 says "awaits server confirmation" but the C++ protocol doesn't define a SubscribeAck message type.
**How to avoid:** Use a fire-and-forget send via the transport's send_encrypted directly (needs a new `send_message()` method on Transport that doesn't create a pending future). The "confirmation" is that the send completed without error.
**Warning signs:** Subscribe calls timing out during integration tests.

### Pitfall 2: Notification Payload Size Mismatch
**What goes wrong:** Notification payload is exactly 77 bytes. If the decode function doesn't validate length, partial or corrupt notifications cause silent errors.
**Why it happens:** Notifications are server-pushed (request_id=0), not request-response correlated.
**How to avoid:** Strict length check: `if len(payload) != 77: raise ProtocolError(...)`.
**Warning signs:** Garbage data in Notification fields during integration tests.

### Pitfall 3: BatchRead FlatBuffer-within-Binary
**What goes wrong:** BatchReadResponse entries with status=0x01 contain a FlatBuffer-encoded blob (from `wire::encode_blob()`), not raw data. The size field is the FlatBuffer size, not the raw data size.
**Why it happens:** The C++ handler calls `wire::encode_blob(*blob)` which produces a full FlatBuffer Blob -- the same format as ReadResponse's found case.
**How to avoid:** Decode each found entry's data as FlatBuffer Blob (reuse existing `decode_read_response` logic from _codec.py, specifically the Blob FlatBuffer decode).
**Warning signs:** Garbled data when reading batch results.

### Pitfall 4: MetadataResponse Variable Length
**What goes wrong:** MetadataResponse has a variable-length pubkey at the end (ML-DSA-87 = 2592 bytes). Hardcoding fixed size fails.
**Why it happens:** Response format includes `pubkey_len:2BE` followed by `pubkey:N` bytes.
**How to avoid:** Parse `pubkey_len` at offset 61, then read exactly `pubkey_len` bytes.
**Warning signs:** ProtocolError on metadata queries.

### Pitfall 5: NamespaceListRequest Cursor
**What goes wrong:** The cursor for NamespaceListRequest is a 32-byte namespace_id (not an integer like ListRequest). Passing zeros means "start from beginning".
**Why it happens:** Different pagination model -- namespaces are sorted by their 32-byte ID, so the cursor is the last namespace returned.
**How to avoid:** Cursor is `bytes` (32 zero bytes for first page, last namespace_id for subsequent pages).
**Warning signs:** Pagination loops that never terminate or skip entries.

### Pitfall 6: PeerInfoResponse Trust-Gated Format
**What goes wrong:** PeerInfoResponse has two completely different formats depending on trust level. SDK connects via relay, which the node may or may not treat as trusted.
**Why it happens:** Relay connections are TCP from the relay IP, not UDS. Whether the relay IP is in the trusted list depends on node configuration.
**How to avoid:** Detect response format by length: 8 bytes = untrusted summary, >8 bytes = trusted full detail. Decode accordingly. Return a PeerInfo result that indicates which variant was received.
**Warning signs:** Decode errors on PeerInfoResponse payload.

### Pitfall 7: Notification D-03 vs Wire Format
**What goes wrong:** D-03 defines `Notification(namespace: bytes, blob_hash: bytes, seq_num: int)` but the wire format is 77 bytes including `blob_size: uint32` and `is_tombstone: bool` (offset 72-76). D-03 omits these two fields.
**Why it happens:** D-03 was defined as "minimal" during context discussion, but the wire format carries more data.
**How to avoid:** Include `blob_size` and `is_tombstone` in the Notification dataclass. The wire data is there -- dropping it wastes useful information. Recommend expanding D-03 to include these fields.
**Warning signs:** Users asking why they can't tell if a notification is for a tombstone.

## Wire Format Reference (Authoritative)

All formats verified from `db/peer/peer_manager.cpp`. All multi-byte integers are big-endian unless noted.

### MetadataRequest (47) / MetadataResponse (48)
**Request:** `[namespace:32][blob_hash:32]` = 64 bytes
**Response (not found):** `[0x00]` = 1 byte
**Response (found):** `[0x01][hash:32][timestamp:8BE][ttl:4BE][data_size:8BE][seq_num:8BE][pubkey_len:2BE][pubkey:N]` = 63 + N bytes
- Source: peer_manager.cpp:1134-1210

### BatchExistsRequest (49) / BatchExistsResponse (50)
**Request:** `[namespace:32][count:4BE][blob_hash:32 * count]` = 36 + 32*count bytes
- Server limit: count in [1, 1024]
**Response:** `[exists:1 * count]` = count bytes (0x00=not found, 0x01=found)
- Source: peer_manager.cpp:1214-1254

### BatchReadRequest (53) / BatchReadResponse (54)
**Request:** `[namespace:32][cap_bytes:4BE][count:4BE][blob_hash:32 * count]` = 40 + 32*count bytes
- Server limits: count in [1, 256], cap_bytes clamped to [1, 4MiB]
- cap_bytes=0 defaults to 4MiB
**Response:** `[truncated:1][result_count:4BE][entries...]`
- Per-entry found: `[0x01][hash:32][size:8BE][FlatBuffer_blob:size bytes]`
- Per-entry not found: `[0x00][hash:32]`
- FlatBuffer blob is the same encoding as ReadResponse found payload (without the 0x01 prefix)
- Source: peer_manager.cpp:1297-1416

### TimeRangeRequest (57) / TimeRangeResponse (58)
**Request:** `[namespace:32][start_ts:8BE][end_ts:8BE][limit:4BE]` = 52 bytes
- Server limits: start <= end (else strike), limit clamped to [1, 100]
**Response:** `[truncated:1][count:4BE][entries...]`
- Per-entry: `[blob_hash:32][seq_num:8BE][timestamp:8BE]` = 48 bytes
- Source: peer_manager.cpp:1504-1602

### NamespaceListRequest (41) / NamespaceListResponse (42)
**Request:** `[after_namespace:32][limit:4BE]` = 36 bytes
- after_namespace = 32 zero bytes for first page
- limit clamped to [1, 1000], default 100
**Response:** `[count:4BE][has_more:1][entries...]`
- Per-entry: `[namespace_id:32][blob_count:8BE]` = 40 bytes
- Source: peer_manager.cpp:953-1025

### NamespaceStatsRequest (45) / NamespaceStatsResponse (46)
**Request:** `[namespace:32]` = 32 bytes
**Response:** `[found:1][blob_count:8BE][total_bytes:8BE][delegation_count:8BE][quota_bytes_limit:8BE][quota_count_limit:8BE]` = 41 bytes fixed
- If not found, bytes 1-40 are all zeros
- Source: peer_manager.cpp:1074-1131

### StorageStatusRequest (43) / StorageStatusResponse (44)
**Request:** empty payload (0 bytes)
**Response:** `[used_data:8BE][max_storage:8BE][tombstone_count:8BE][namespace_count:4BE][total_blobs:8BE][mmap_bytes:8BE]` = 44 bytes fixed
- Source: peer_manager.cpp:1028-1071

### NodeInfoRequest (39) / NodeInfoResponse (40)
**Request:** empty payload (0 bytes)
**Response:** variable length:
```
[version_len:1][version:N]
[git_hash_len:1][git_hash:N]
[uptime_seconds:8BE]
[peer_count:4BE]
[namespace_count:4BE]
[total_blobs:8BE]
[storage_used:8BE]
[storage_max:8BE]
[types_count:1][supported_types:types_count bytes]
```
- Source: peer_manager.cpp:865-950

### PeerInfoRequest (55) / PeerInfoResponse (56)
**Request:** empty payload (0 bytes)
**Response (untrusted):** `[peer_count:4BE][bootstrap_count:4BE]` = 8 bytes
**Response (trusted):** `[peer_count:4BE][bootstrap_count:4BE][entries...]`
- Per-entry: `[addr_len:2BE][addr:N][is_bootstrap:1][syncing:1][peer_is_full:1][connected_duration_ms:8BE]`
- SDK connects via relay -- trust level depends on node config. Must handle both formats.
- Source: peer_manager.cpp:1419-1501

### DelegationListRequest (51) / DelegationListResponse (52)
**Request:** `[namespace:32]` = 32 bytes
**Response:** `[count:4BE][entries...]`
- Per-entry: `[delegate_pk_hash:32][delegation_blob_hash:32]` = 64 bytes
- Source: peer_manager.cpp:1257-1294

### Subscribe (19) / Unsubscribe (20) -- Fire-and-forget
**Payload:** `[count:2BE][namespace:32 * count]`
- **No server response.** Node processes inline and returns. No ack, no confirmation message.
- Source: peer_manager.cpp:622-648, 2925-2951

### Notification (21) -- Server-pushed
**Payload:** 77 bytes fixed:
```
[namespace:32][blob_hash:32][seq_num:8BE][blob_size:4BE][is_tombstone:1]
```
- Sent with request_id=0 (already routed to Transport._notifications queue)
- Source: peer_manager.cpp:2954-2975

## Code Examples

### Codec: encode_metadata_request
```python
# Follows existing encode_read_request pattern exactly
def encode_metadata_request(namespace: bytes, blob_hash: bytes) -> bytes:
    if len(namespace) != 32:
        raise ValueError(f"namespace must be 32 bytes, got {len(namespace)}")
    if len(blob_hash) != 32:
        raise ValueError(f"blob_hash must be 32 bytes, got {len(blob_hash)}")
    return namespace + blob_hash
```

### Codec: decode_metadata_response
```python
def decode_metadata_response(payload: bytes) -> MetadataResult | None:
    if len(payload) < 1:
        raise ProtocolError("empty MetadataResponse")
    if payload[0] == 0x00:
        return None
    if payload[0] != 0x01:
        raise ProtocolError(f"unexpected MetadataResponse flag: {payload[0]:#x}")
    if len(payload) < 63:
        raise ProtocolError(f"MetadataResponse too short: {len(payload)} bytes")
    blob_hash = payload[1:33]
    timestamp = struct.unpack(">Q", payload[33:41])[0]
    ttl = struct.unpack(">I", payload[41:45])[0]
    data_size = struct.unpack(">Q", payload[45:53])[0]
    seq_num = struct.unpack(">Q", payload[53:61])[0]
    pubkey_len = struct.unpack(">H", payload[61:63])[0]
    if len(payload) != 63 + pubkey_len:
        raise ProtocolError(f"MetadataResponse size mismatch")
    pubkey = payload[63:63 + pubkey_len]
    return MetadataResult(
        blob_hash=blob_hash, timestamp=timestamp, ttl=ttl,
        data_size=data_size, seq_num=seq_num, pubkey=pubkey,
    )
```

### Codec: encode_batch_exists_request
```python
def encode_batch_exists_request(namespace: bytes, hashes: list[bytes]) -> bytes:
    if len(namespace) != 32:
        raise ValueError(f"namespace must be 32 bytes, got {len(namespace)}")
    for h in hashes:
        if len(h) != 32:
            raise ValueError(f"each hash must be 32 bytes, got {len(h)}")
    count = len(hashes)
    return namespace + struct.pack(">I", count) + b"".join(hashes)
```

### Codec: decode_notification
```python
def decode_notification(payload: bytes) -> Notification:
    if len(payload) != 77:
        raise ProtocolError(f"Notification must be 77 bytes, got {len(payload)}")
    namespace = payload[0:32]
    blob_hash = payload[32:64]
    seq_num = struct.unpack(">Q", payload[64:72])[0]
    blob_size = struct.unpack(">I", payload[72:76])[0]
    is_tombstone = payload[76] == 1
    return Notification(
        namespace=namespace, blob_hash=blob_hash,
        seq_num=seq_num, blob_size=blob_size, is_tombstone=is_tombstone,
    )
```

### Transport: send_message (new, for fire-and-forget)
```python
async def send_message(self, msg_type: int, payload: bytes) -> None:
    """Send a message without expecting a response (fire-and-forget)."""
    if self._closed:
        raise ChromatinConnectionError("connection is closed")
    msg = encode_transport_message(msg_type, payload)
    async with self._send_lock:
        self._send_counter = await send_encrypted(
            self._writer, msg, self._send_key, self._send_counter,
        )
```

### Client: notifications async generator (D-01)
```python
async def notifications(self):
    """Async iterator yielding Notification objects (D-01, D-02).

    Usage:
        async for notif in conn.notifications():
            print(f"New blob in {notif.namespace.hex()}: {notif.blob_hash.hex()}")
    """
    while not self._transport.closed:
        try:
            msg_type, payload, _ = await asyncio.wait_for(
                self._transport.notifications.get(),
                timeout=1.0,
            )
        except asyncio.TimeoutError:
            continue
        if msg_type == TransportMsgType.Notification:
            yield decode_notification(payload)
```

## New Dataclasses (types.py)

All frozen, following existing Phase 72 pattern:

```python
@dataclass(frozen=True)
class MetadataResult:
    blob_hash: bytes      # 32-byte hash
    timestamp: int        # seconds
    ttl: int              # seconds
    data_size: int        # raw data size in bytes
    seq_num: int          # sequence number
    pubkey: bytes         # ML-DSA-87 public key (2592 bytes)

@dataclass(frozen=True)
class BatchReadResult:
    blobs: dict[bytes, ReadResult | None]  # hash -> result or None
    truncated: bool

@dataclass(frozen=True)
class TimeRangeEntry:
    blob_hash: bytes      # 32-byte hash
    seq_num: int
    timestamp: int        # seconds

@dataclass(frozen=True)
class TimeRangeResult:
    entries: list[TimeRangeEntry]
    truncated: bool

@dataclass(frozen=True)
class NamespaceEntry:
    namespace_id: bytes   # 32-byte namespace
    blob_count: int

@dataclass(frozen=True)
class NamespaceListResult:
    namespaces: list[NamespaceEntry]
    cursor: bytes | None  # 32-byte cursor for next page, None if no more

@dataclass(frozen=True)
class NamespaceStats:
    found: bool
    blob_count: int
    total_bytes: int
    delegation_count: int
    quota_bytes_limit: int
    quota_count_limit: int

@dataclass(frozen=True)
class StorageStatus:
    used_data_bytes: int
    max_storage_bytes: int    # 0 = unlimited
    tombstone_count: int
    namespace_count: int
    total_blobs: int
    mmap_bytes: int

@dataclass(frozen=True)
class NodeInfo:
    version: str
    git_hash: str
    uptime_seconds: int
    peer_count: int
    namespace_count: int
    total_blobs: int
    storage_used_bytes: int
    storage_max_bytes: int    # 0 = unlimited
    supported_types: list[int]

@dataclass(frozen=True)
class PeerDetail:
    address: str
    is_bootstrap: bool
    syncing: bool
    peer_is_full: bool
    connected_duration_ms: int

@dataclass(frozen=True)
class PeerInfo:
    peer_count: int
    bootstrap_count: int
    peers: list[PeerDetail]   # empty list if untrusted response

@dataclass(frozen=True)
class DelegationEntry:
    delegate_pk_hash: bytes   # 32-byte hash
    delegation_blob_hash: bytes  # 32-byte hash

@dataclass(frozen=True)
class DelegationList:
    entries: list[DelegationEntry]

@dataclass(frozen=True)
class Notification:
    namespace: bytes      # 32-byte namespace
    blob_hash: bytes      # 32-byte hash
    seq_num: int
    blob_size: int        # uint32 data size
    is_tombstone: bool    # True if notification is for a tombstone
```

**Design decisions:**
- **MetadataResult vs ReadResult:** MetadataResult is a separate type (not ReadResult) because it has `data_size` and `pubkey` instead of `data` and `signature`. Different shape, different type.
- **TimeRangeResult:** New type (not reusing ListPage) because entries include timestamps and the result has a truncation flag instead of cursor.
- **Notification expanded beyond D-03:** Includes `blob_size` and `is_tombstone` from wire format. D-03 said "minimal" but dropping available wire data is wasteful. This is a Claude's Discretion area.
- **PeerInfo handles both trust variants:** `peers` list is empty for untrusted responses, populated for trusted. Single type covers both.

## Open Questions

1. **Subscribe "confirmation" (D-04 conflict)**
   - What we know: D-04 says "subscribe() awaits server confirmation before returning." The C++ node processes Subscribe inline (peer_manager.cpp:622-633) and does NOT send any response message. There is no SubscribeAck message type in the protocol.
   - What's unclear: Whether D-04 intended "awaits TCP send completion" or "awaits a round-trip response."
   - Recommendation: Implement subscribe as fire-and-forget send. The "confirmation" is that `send_encrypted` completes without error (meaning the message was sent to the relay). Document this in the method docstring. This is the only viable approach given the protocol.

2. **Notification D-03 expansion**
   - What we know: D-03 defines `Notification(namespace, blob_hash, seq_num)`. The wire format carries two additional fields: `blob_size` (uint32) and `is_tombstone` (bool).
   - What's unclear: Whether the user explicitly wanted the minimal 3-field version or the discussion just didn't cover the full wire format.
   - Recommendation: Include all 5 fields from the wire format. Dropping available data violates YAGNI in reverse -- users will ask for these fields and we'd have to add them later. The dataclass is still simple.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest + pytest-asyncio |
| Config file | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && python -m pytest tests/ -x --ignore=tests/test_integration.py -q` |
| Full suite command | `cd sdk/python && python -m pytest tests/ -v` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| QUERY-01 | Metadata query encode/decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k metadata -x` | extend existing |
| QUERY-02 | BatchExists encode/decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k batch_exists -x` | extend existing |
| QUERY-03 | BatchRead encode/decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k batch_read -x` | extend existing |
| QUERY-04 | TimeRange encode/decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k time_range -x` | extend existing |
| QUERY-05 | NamespaceList encode/decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k namespace_list -x` | extend existing |
| QUERY-06 | NamespaceStats encode/decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k namespace_stats -x` | extend existing |
| QUERY-07 | StorageStatus decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k storage_status -x` | extend existing |
| QUERY-08 | NodeInfo decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k node_info -x` | extend existing |
| QUERY-09 | PeerInfo decode (both variants) + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k peer_info -x` | extend existing |
| QUERY-10 | DelegationList decode + client method | unit | `cd sdk/python && python -m pytest tests/test_codec.py -k delegation -x` | extend existing |
| PUBSUB-01 | Subscribe encode + client method + subscription tracking | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -k subscribe -x` | extend existing |
| PUBSUB-02 | Unsubscribe encode + client method + subscription removal | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -k unsubscribe -x` | extend existing |
| PUBSUB-03 | Notification decode + async iterator consumer | unit | `cd sdk/python && python -m pytest tests/test_client_ops.py -k notification -x` | extend existing |

### Sampling Rate
- **Per task commit:** `cd sdk/python && python -m pytest tests/ -x --ignore=tests/test_integration.py -q`
- **Per wave merge:** `cd sdk/python && python -m pytest tests/ -v`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
None -- existing test infrastructure covers all phase requirements. Test files exist and just need extension.

## Sources

### Primary (HIGH confidence)
- `db/peer/peer_manager.cpp` lines 622-648, 865-1602, 2900-2975 -- Authoritative wire format for all 13 message type pairs
- `sdk/python/chromatindb/client.py` -- Existing client pattern (Phase 72)
- `sdk/python/chromatindb/_codec.py` -- Existing codec pattern (Phase 72)
- `sdk/python/chromatindb/_transport.py` -- Transport notification queue and request dispatch
- `sdk/python/chromatindb/types.py` -- Existing frozen dataclass pattern
- `sdk/python/chromatindb/wire.py` -- TransportMsgType enum with all 58 constants

### Secondary (MEDIUM confidence)
- Phase 73 CONTEXT.md decisions D-01 through D-13

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all stdlib
- Architecture: HIGH - extends established Phase 72 patterns verbatim
- Wire formats: HIGH - read directly from C++ source code (authoritative)
- Pitfalls: HIGH - verified against C++ handlers, found Subscribe no-response issue
- Pub/sub: MEDIUM - D-04 conflicts with protocol reality, recommendation given

**Research date:** 2026-03-30
**Valid until:** 2026-04-30 (stable -- wire formats are locked)
