# v1.3.0: Protocol Concurrency & Query Foundation

**Date:** 2026-03-24
**Status:** Design approved
**Milestone:** v1.3.0
**Depends on:** v1.2.0 (Relay & Client Protocol) -- shipped 2026-03-23

## Goal

Clients can send concurrent requests with correlation IDs, check blob existence without data transfer, and discover node capabilities. This is the foundation milestone -- extended query types (time range, delegation, metadata, etc.) come in a subsequent milestone that builds on the concurrent dispatch model established here.

## Context

The current protocol (v1.2.0) is functional but minimal:

| Operation | Types | Pattern |
|-----------|-------|---------|
| Write | Data -> WriteAck | Request/response |
| Delete | Delete -> DeleteAck | Request/response |
| Read | ReadRequest -> ReadResponse | Request/response |
| List | ListRequest -> ListResponse | Request/response |
| Stats | StatsRequest -> StatsResponse | Request/response |
| Pub/Sub | Subscribe/Unsubscribe/Notification | Fire-and-forget + push |
| Lifecycle | Ping/Pong/Goodbye | Keepalive |

**Problems this milestone solves:**

1. **No concurrent requests.** The node processes messages sequentially per connection. Multiple in-flight requests queue behind each other. SDK clients cannot pipeline.
2. **No correlation.** Request/response pairs have no ID -- clients must serialize and wait for each response before sending the next request. Unusable for high-throughput workloads.
3. **No existence check.** Clients must issue a full ReadRequest (transferring blob data) just to check if a blob exists. Wasteful for sync-style "which of these do I have?" patterns.
4. **No capability discovery.** No way for a client or operator to ask the node what it supports, its version, storage state, or peer topology.

## Design

### 1. Transport Envelope: request_id

Add a `request_id` field to the FlatBuffers transport schema:

```fbs
table TransportMessage {
    type: TransportMsgType;
    payload: [ubyte];
    request_id: uint32;
}
```

**Semantics:**

- Set by the client on request messages. Echoed verbatim by the node on the corresponding response.
- The node never generates request_ids -- it only echoes.
- IDs are per-connection, not globally unique. The client owns the ID space and reuses values however it wants.
- Operations that are not request/response (sync, PEX, pub/sub, handshake, lifecycle) use `request_id = 0`.

**No backward compatibility constraints.** The node is not deployed anywhere. This is a clean breaking change to the transport envelope.

**Code changes:**

| File | Change |
|------|--------|
| `db/schemas/transport.fbs` | Add `request_id: uint32` field to `TransportMessage` |
| `db/net/protocol.h` | Add `uint32_t request_id = 0` to `DecodedMessage` |
| `db/net/protocol.cpp` | `TransportCodec::encode` takes `request_id` param, `TransportCodec::decode` extracts it |
| `db/net/connection.h` | `send_message` gains `uint32_t request_id` param. `MessageCallback` signature becomes `(Ptr, TransportMsgType, vector<uint8_t>, uint32_t)` |
| `db/net/connection.cpp` | Thread `request_id` through encode/decode/callback dispatch |
| All message handlers | Accept extra `request_id` param; request/response handlers echo it, others ignore it |
| `relay/core/relay_session.cpp` | Forward `request_id` in `handle_client_message` / `handle_node_message` |

### 2. Concurrent Request Dispatch

The message loop stays single-threaded for reading. Request/response handlers get dispatched concurrently via the existing thread pool + offload pattern.

**Three dispatch categories:**

| Category | Operations | Dispatch | Rationale |
|----------|-----------|----------|-----------|
| Inline | Ping, Pong, Goodbye, Subscribe, Unsubscribe, ExistsRequest, NodeInfoRequest | IO thread | Cheap: flag set, hash lookup, in-memory reads |
| Offloaded | ReadRequest, ListRequest, StatsRequest | `co_spawn` + offload to thread pool, `post(ioc_)` back before send | Touches mdbx storage, may block briefly |
| Write path | Data (ingest), Delete | Existing pattern | Already uses `offload()` for crypto |
| Peer ops | Sync*, Reconcile*, PEX | Unchanged | Connection-scoped coroutines, not request/response |

**Critical safety constraint:** `send_encrypted` uses `send_counter_++` for the AEAD nonce. This is not thread-safe. All `send_message` calls MUST execute on the IO thread.

**Pattern for offloaded handlers:**

```cpp
case ReadRequest:
    asio::co_spawn(ioc_, [this, conn, payload, request_id]() -> asio::awaitable<void> {
        // 1. Offload heavy work to thread pool
        auto response = co_await offload(pool_, [&]{ return handle_read(payload); });
        // 2. Back on IO thread -- safe to touch connection state
        co_await conn->send_message(ReadResponse, response, request_id);
    }, asio::detached);
    break;
```

This follows the proven pattern from `recv_sync_msg` (TSAN finding in v1.0.0): do expensive work on the pool, `co_await asio::post(ioc_)` to return to the IO thread, then touch connection state.

**Ordering:** If a client sends ReadRequest A then ReadRequest B, B might complete before A if A's storage read is slower. This is expected -- `request_id` lets the client match responses. The SDK handles ordering if needed.

### 3. ExistsRequest / ExistsResponse

Check blob existence without data transfer.

**Wire types:**

```fbs
// In TransportMsgType enum:
ExistsRequest = 38,
ExistsResponse = 39,
```

**Payload schemas:**

```fbs
table ExistsRequestPayload {
    namespace_id: [ubyte];   // 32 bytes
    blob_hash: [ubyte];      // 32 bytes
}

table ExistsResponsePayload {
    exists: bool;
    blob_hash: [ubyte];      // Echo back for client-side correlation
}
```

**Dispatch:** Inline. Single mdbx key-existence check -- microseconds.

**Implementation:**

| File | Change |
|------|--------|
| `db/schemas/transport.fbs` | Add enum values + payload tables |
| `db/storage/storage.h/cpp` | Add `has_blob(namespace_id, blob_hash) -> bool` -- key-existence check without reading value |
| `db/engine/engine.h/cpp` | Add `blob_exists(namespace_id, blob_hash) -> bool` -- thin wrapper |
| Message handler | Decode request, call `blob_exists`, encode response with `request_id` |
| `relay/core/message_filter.cpp` | Add `ExistsRequest`, `ExistsResponse` to `is_client_allowed()` |

### 4. NodeInfoRequest / NodeInfoResponse

Node self-description for operators and SDK capability detection.

**Wire types:**

```fbs
// In TransportMsgType enum:
NodeInfoRequest = 40,
NodeInfoResponse = 41,
```

**Payload schemas:**

```fbs
table NodeInfoRequestPayload {
    // Empty -- no parameters needed
}

table NodeInfoResponsePayload {
    version: string;              // e.g. "1.3.0"
    git_hash: string;             // e.g. "fd084c0"
    uptime_seconds: uint64;
    peer_count: uint32;
    namespace_count: uint32;
    total_blobs: uint64;
    storage_bytes_used: uint64;
    storage_bytes_max: uint64;    // 0 = unlimited
    supported_types: [ubyte];     // TransportMsgType values this node handles
}
```

**Key field: `supported_types`.** The SDK sends NodeInfoRequest on connect, gets back the list of message types the node understands. When future milestones add new query types, old nodes won't list them. No version parsing, no feature flags -- capability discovery via the protocol itself.

**Dispatch:** Inline. All data is in-memory: uptime is a clock diff, peer count from `Server`/`PeerManager`, storage stats from `Storage`, version from compiled-in constants (`CHROMATINDB_VERSION`, `CHROMATINDB_GIT_HASH`).

**Implementation:**

| File | Change |
|------|--------|
| `db/schemas/transport.fbs` | Add enum values + payload tables |
| Message handler | Assemble response from `Server` (peer count), `Storage` (namespace/blob/byte counts), compiled constants (version/git hash), startup timestamp (uptime) |
| `relay/core/message_filter.cpp` | Add `NodeInfoRequest`, `NodeInfoResponse` to `is_client_allowed()` |

### 5. Documentation

| File | Change |
|------|--------|
| `db/PROTOCOL.md` | Document request_id semantics, concurrent dispatch model, ExistsRequest/ExistsResponse, NodeInfoRequest/NodeInfoResponse |
| `README.md` | Update protocol section with new capabilities |

### 6. Relay Impact

The relay is a transparent forwarder. Changes are minimal:

1. **request_id forwarding:** `handle_client_message` and `handle_node_message` already receive decoded messages and call `send_message`. Add `request_id` to both paths.
2. **Message filter:** Add `ExistsRequest` (38), `ExistsResponse` (39), `NodeInfoRequest` (40), `NodeInfoResponse` (41) to `is_client_allowed()`.
3. **No concurrent dispatch in relay.** The relay doesn't process requests -- it forwards them. The node handles concurrency.

## Phasing

Likely 3-4 phases, executed in dependency order:

1. **Transport foundation** -- FlatBuffers schema change, codec, connection signature, relay forwarding. All existing tests updated for new signature.
2. **Concurrent dispatch** -- Offload pattern for Read/List/Stats handlers. Concurrent request tests.
3. **New types** -- ExistsRequest/ExistsResponse, NodeInfoRequest/NodeInfoResponse. Relay filter updates.
4. **Documentation & verification** -- PROTOCOL.md, README.md, integration tests.

## What This Does NOT Include

- Extended query types (TimeRange, DelegationList, Metadata, NamespaceList, PeerInfo, Health) -- subsequent milestone
- Connection pooling -- SDK concern
- Full worker pool model -- current offload pattern is sufficient; can evolve later
- Changes to sync, PEX, pub/sub, or write path semantics
- Thread pool sizing -- uses existing `asio::thread_pool`

## Type Budget

After this milestone: types 0-41 used, 214 slots remaining (byte enum, max 255).
