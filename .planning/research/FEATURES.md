# Feature Research: Python SDK for chromatindb

**Domain:** Python client SDK for binary-protocol database with PQ cryptography
**Researched:** 2026-03-28
**Confidence:** HIGH

## Feature Landscape

### Table Stakes (Users Expect These)

Features any Python database/network client must have. Missing these means the SDK feels broken, not just incomplete.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Context manager connection lifecycle | Every Python DB client (redis-py, psycopg, boto3) supports `with client:` or `async with client:` for deterministic resource cleanup. Developers expect it. | LOW | `__enter__`/`__exit__` for sync, `__aenter__`/`__aexit__` for async. Handles TCP socket + AEAD state teardown (Goodbye message before close). |
| Async-first API with `async`/`await` | The protocol uses persistent TCP connections with multiplexed request_id pipelining. This is inherently async. Python developers building on top of chromatindb will use asyncio (FastAPI, etc.). | MEDIUM | Use `asyncio.open_connection()` for TCP. All send/recv operations are coroutines. The AEAD framing (length-prefixed encrypted frames) maps naturally to asyncio stream reads. |
| Typed exception hierarchy | redis-py has `ConnectionError`, `TimeoutError`, `ResponseError`. boto3 has `ClientError`, `BotoCoreError`. Developers catch specific exceptions for retry logic. | LOW | Base `ChromatinError`. Children: `ConnectionError` (TCP/handshake failures), `AuthenticationError` (PQ handshake signature verification failed, ACL rejection), `ProtocolError` (unexpected message type, malformed response), `StorageFullError`, `QuotaExceededError`, `TimeoutError`, `NotFoundError` (for read/exists returning absent). |
| Write blobs with automatic signing | The whole point of the SDK. Client creates a blob with data + TTL, SDK handles SHA3-256 canonical hash, ML-DSA-87 signing, FlatBuffers encoding, and sends Data(8). Returns WriteAck with hash + seq_num + status. | HIGH | Requires: liboqs-python for ML-DSA-87 signing, hashlib.sha3_256 for canonical hash, FlatBuffers Python runtime for Blob encoding. The signing input is `SHA3-256(namespace_id || data || ttl_le32 || timestamp_le64)` -- SDK must construct this exactly. |
| Read blobs by namespace + hash | `client.read(namespace, blob_hash)` sends ReadRequest(31), receives ReadResponse(32). Returns decoded blob or None. | MEDIUM | Decode FlatBuffer Blob from response payload. Return a typed `Blob` dataclass with namespace_id, data, ttl, timestamp, pubkey, signature fields. |
| Delete blobs with tombstone signing | `client.delete(namespace, target_hash)` constructs tombstone data `[0xDE 0xAD 0xBE 0xEF][target_hash:32]`, signs it, sends Delete(17). Returns DeleteAck confirmation. | MEDIUM | Reuses the write path but with tombstone magic prefix and TTL=0. Only namespace owners can delete. |
| List blobs with cursor pagination | `client.list(namespace, since_seq=0, limit=100)` sends ListRequest(33). Returns entries + has_more flag. Must support full iteration via `async for entry in client.list_all(namespace):`. | MEDIUM | The protocol already has cursor-based pagination (since_seq + has_more). SDK wraps this in an async iterator that auto-paginates. This is the pattern Google, Azure, and AWS SDKs all use for paginated results. |
| Request-response correlation via request_id | The node echoes request_id and may respond out-of-order. SDK must assign unique request_ids, match responses to pending requests, and support concurrent in-flight requests. | HIGH | Internal dispatch table mapping request_id -> asyncio.Future. Background reader task receives frames, decrypts, extracts request_id, resolves the matching Future. This is the core multiplexing engine. |
| Connection to relay with PQ handshake | The SDK connects to the relay (port 4201), performs the ML-KEM-1024 key exchange + ML-DSA-87 mutual authentication, derives AEAD session keys, then sends/receives encrypted frames. | HIGH | Four-message handshake: KemPubkey -> KemCiphertext -> AuthSignature -> AuthSignature. Key derivation via HKDF-SHA256. This is the single most complex feature -- it touches liboqs-python (ML-KEM-1024 encapsulate, ML-DSA-87 sign/verify), hashlib (SHA3-256 for salt), cryptography lib (HKDF-SHA256), and AEAD encryption setup. |
| AEAD encrypted transport | All post-handshake frames use ChaCha20-Poly1305 with counter-based nonces. SDK must maintain send/recv nonce counters, encrypt outgoing frames, decrypt incoming frames, and handle the length-prefix framing. | HIGH | Nonce = 4 zero bytes + 8-byte big-endian counter. Counters start at 1 (0 used for auth). pysodium or PyNaCl low-level bindings for `crypto_aead_chacha20poly1305_ietf_encrypt/decrypt`. Must handle MAX_FRAME_SIZE (110 MiB). |
| Identity management (keypair load/generate) | Developers need to create or load ML-DSA-87 signing keypairs. The keypair determines the namespace (SHA3-256 of pubkey). | MEDIUM | `Identity.generate()` creates new ML-DSA-87 keypair. `Identity.from_file(path)` loads existing keys (matching relay's SSH-style .key/.pub sibling pattern). `identity.namespace` property returns SHA3-256(pubkey). `identity.public_key`, `identity.secret_key` accessors. |
| Keepalive (Ping/Pong) | The node has inactivity detection (default 120s). If the SDK doesn't send messages, the connection gets dropped. | LOW | Background task sends Ping(5) every N seconds when idle. Responds to incoming Ping with Pong(6). No request_id needed (keepalive is fire-and-forget). |
| Timeout on all operations | Every request must have a configurable timeout. Hanging forever on a read is unacceptable. | LOW | Per-operation `timeout` kwarg, default from client config. Uses `asyncio.wait_for()`. On timeout, remove the pending Future from the dispatch table and raise `TimeoutError`. |
| Graceful disconnect | Send Goodbye(7) before closing the socket. Clean up AEAD state and pending requests (cancel all Futures with `ConnectionError`). | LOW | Called from `__aexit__` / `close()`. Cancel background reader task, drain pending requests, send Goodbye, close socket. |

### Differentiators (Competitive Advantage)

Features that elevate the SDK beyond a bare protocol wrapper. Not required for launch but create significant developer value.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Pub/sub with async iterator | `async for notification in client.subscribe([ns1, ns2]):` yields Notification objects. Natural Python pattern (like NATS-py, aioredis). Real-time blob notifications without polling. | MEDIUM | Subscribe(19) registers namespaces. Background reader routes Notification(21) messages to an `asyncio.Queue`. The async iterator yields from the queue. Unsubscribe(20) on iterator exit or explicit call. Connection-scoped (server forgets subscriptions on disconnect). |
| Sync (blocking) wrapper API | Not all Python code is async. Scripts, notebooks, CLI tools want `client.read(ns, hash)` without `await`. Azure SDK pattern: separate sync client class that wraps async internally. | MEDIUM | `ChromatinClient` (sync) wraps `AsyncChromatinClient` using `asyncio.run()` or a dedicated event loop thread. Same method names, same return types. Import from `chromatindb` (sync) or `chromatindb.aio` (async). Higher priority than most differentiators because many users will start with sync code. |
| Batch operations | `client.batch_exists(namespace, [hash1, hash2, ...])` and `client.batch_read(namespace, [hash1, hash2, ...])` map to BatchExistsRequest(49) and BatchReadRequest(53). Single round-trip for multiple blobs. | LOW | Already supported by the protocol. SDK just needs to encode the request, decode the response array. BatchRead has a `cap_bytes` parameter and `truncated` flag that the SDK should expose. |
| Node introspection | `client.node_info()` returns version, uptime, peer count, storage stats, supported message types. `client.storage_status()` returns disk usage. Used for monitoring and capability detection. | LOW | NodeInfoRequest(39), StorageStatusRequest(43). Simple request-response, no state. SDK can use `supported_types` from NodeInfoResponse to feature-detect what the connected node supports. |
| Namespace operations | `client.list_namespaces()` with cursor pagination. `client.namespace_stats(ns)` for per-namespace counters. Useful for admin dashboards, data exploration. | LOW | NamespaceListRequest(41) with auto-pagination async iterator. NamespaceStatsRequest(45) returns blob count, bytes, delegation count, quotas. |
| Blob metadata without data transfer | `client.metadata(namespace, blob_hash)` returns timestamp, TTL, size, seq_num, signer pubkey -- without downloading the blob data. | LOW | MetadataRequest(47). Useful for building indexes, checking freshness, auditing signers. Much cheaper than full read for large blobs. |
| Delegation management | `client.delegate(delegate_pubkey)` creates a delegation blob (magic prefix + pubkey). `client.revoke_delegation(delegate_pubkey)` tombstones it. `client.list_delegations(namespace)` queries existing delegations. | MEDIUM | Delegation blob format: `[0xDE 0x1E 0x6A 0x7E][delegate_pubkey:2592]`. Signed by namespace owner. Revocation = tombstone the delegation blob hash. DelegationListRequest(51) for listing. |
| Time range queries | `client.time_range(namespace, start_time, end_time, limit=100)` returns blob references within a time window. Useful for "get everything from the last hour." | LOW | TimeRangeRequest(57). Returns hash + seq + timestamp tuples. Client uses these with read/batch_read to fetch actual data. |
| Peer info query | `client.peer_info()` returns connected peer information. Trust-gated (full detail only for trusted connections). | LOW | PeerInfoRequest(55). Admin/monitoring use case. |
| Request pipelining | Send multiple requests without waiting for responses. Responses matched by request_id. Improves throughput for batch-style workloads. | MEDIUM | Already required by the dispatch table design (table stakes). The differentiator is exposing it ergonomically: `async with client.pipeline() as pipe: pipe.read(ns, h1); pipe.read(ns, h2); results = await pipe.execute()`. Optional -- individual await per call also pipelines naturally since the background reader processes responses as they arrive. |
| Auto-reconnect with backoff | Connection drops in production. SDK should reconnect automatically with jittered exponential backoff, re-establish PQ handshake, and re-subscribe to pub/sub namespaces. | HIGH | Follows nats-py pattern: configurable `allow_reconnect`, `max_reconnect_attempts`, `reconnect_wait`. Callbacks for `disconnected_cb`, `reconnected_cb`, `error_cb`. Re-subscribes automatically. Pending requests get `ConnectionError`. Complex because it requires re-handshake and re-subscription state tracking. |

### Anti-Features (Commonly Requested, Often Problematic)

Features that seem good but create real problems. Explicitly avoid these.

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| Connection pooling | "I need multiple concurrent requests" | chromatindb supports request_id multiplexing -- a single connection handles unlimited concurrent requests. Multiple connections add AEAD state overhead, handshake latency, and connection limits at the node. Connection pools solve HTTP/1.1 head-of-line blocking, which does not exist here. | Single persistent connection with request_id multiplexing. One connection, many concurrent requests. Matches the protocol design. |
| ORM / schema layer on top of blob storage | "I want to store Python objects, not bytes" | chromatindb is a blob store. Adding serialization opinions (pickle, msgpack, JSON) couples the SDK to application-layer concerns. Every ORM for blob stores ends up being a leaky abstraction that users fight against. | Provide raw bytes in, raw bytes out. Users choose their own serialization. Document examples with msgpack/JSON in the tutorial. |
| Automatic key rotation | "Rotate my signing keys periodically" | Key rotation changes the namespace (SHA3-256 of pubkey). All existing data is in the old namespace. The protocol has no concept of key migration. Attempting to automate this creates data loss or orphaned namespaces. | Document that namespaces are permanently bound to keypairs. If users need key rotation, they create a new identity and use delegation for transition period. |
| Transparent encryption of blob data | "Encrypt my data before storing" | The transport is already PQ-encrypted (AEAD). Data-at-rest encryption is handled by the node. Adding client-side encryption means the SDK manages more keys, and blobs become opaque to any other client that doesn't have the key. This is an application-layer concern. | Users who want E2E encryption do it in their application layer before passing bytes to the SDK. Document this pattern in the tutorial. |
| Sync-only API (no async) | "I don't want to deal with asyncio" | The protocol is fundamentally async (persistent connection, multiplexed responses, server-pushed notifications). A sync-only API either blocks the event loop or hides an internal thread, creating debugging nightmares. | Async-first with a sync wrapper class (see Differentiators). The sync wrapper runs a dedicated asyncio event loop in a background thread. Same API surface, same behavior, no async keywords needed by the caller. |
| Automatic retry on all errors | "Retry everything automatically" | StorageFull, QuotaExceeded, and AuthenticationError are not transient -- retrying them wastes resources and delays error reporting. Only connection-level failures and timeouts benefit from retry. | Retry only at connection level (reconnect). Individual operation failures surface immediately. Users implement their own retry logic with `tenacity` or similar for the specific errors they consider retransient. |
| Thread-safe client | "Share one client across threads" | asyncio is single-threaded by design. Making the client thread-safe requires locks around the AEAD nonce counters, the dispatch table, and the socket. This adds overhead and complexity for a pattern that's fundamentally wrong (share the event loop, not the client). | One client per event loop (async) or one client per thread (sync wrapper creates its own loop). Document this clearly. |
| Streaming large blob uploads | "Upload in chunks for 100 MiB blobs" | The protocol sends a complete FlatBuffer-encoded Blob in a single Data(8) message. There is no chunked upload protocol. The signing input requires the full data to compute SHA3-256. Streaming would require a protocol change. | Accept bytes or file path, load into memory, sign, send. For 100 MiB blobs, this means ~100 MiB in memory. Document the memory requirement. For very large data, users chunk at the application layer (multiple blobs). |
| Caching layer | "Cache frequently read blobs locally" | Cache invalidation is hard. Pub/sub notifications could invalidate, but tombstones, TTL expiry, and delegation changes create complex invalidation logic. A cache that serves stale data is worse than no cache. | Users who need caching implement it in their application with their own invalidation strategy, possibly driven by pub/sub notifications. |
| Multiple simultaneous node connections | "Connect to several nodes for redundancy" | chromatindb nodes replicate data via peer sync. Connecting to multiple nodes means duplicate writes (each write replicates to all peers), duplicate reads (any node has the data), and complex consistency decisions. The relay is the single entry point by design. | Connect to one relay. The relay forwards to the node, which replicates to peers. If the relay is down, reconnect with backoff. For HA, run multiple relays behind a load balancer (future concern, not SDK concern). |

## Feature Dependencies

```
[Identity Management]
    |
    +--requires--> [PQ Handshake (ML-KEM-1024 + ML-DSA-87)]
    |                  |
    |                  +--requires--> [AEAD Encrypted Transport]
    |                                     |
    |                                     +--requires--> [FlatBuffers Encoding/Decoding]
    |                                     |                  |
    |                                     |                  +--enables--> [Write Blobs]
    |                                     |                  +--enables--> [Read Blobs]
    |                                     |                  +--enables--> [All Query Operations]
    |                                     |
    |                                     +--requires--> [Request-Response Dispatch (request_id)]
    |                                                        |
    |                                                        +--enables--> [Request Pipelining]
    |                                                        +--enables--> [Concurrent Operations]
    |
    +--enables--> [Blob Signing (Write/Delete)]

[Keepalive] --requires--> [AEAD Encrypted Transport]

[Pub/Sub Notifications] --requires--> [AEAD Encrypted Transport]
                         --requires--> [Request-Response Dispatch] (for subscribe/unsubscribe)
                         --independent of--> [Request-Response Dispatch] (notifications are server-pushed, request_id=0)

[Sync Wrapper] --requires--> [Async Client] (wraps the complete async API)

[Auto-Reconnect] --requires--> [Async Client]
                  --enhances--> [Pub/Sub] (re-subscribes after reconnect)

[Context Manager] --requires--> [Graceful Disconnect]

[Batch Operations] --requires--> [AEAD Encrypted Transport]
                    --requires--> [Request-Response Dispatch]

[Delegation Management] --requires--> [Blob Signing]
                        --requires--> [Identity Management]

[All Query Ops (metadata, namespace list, stats, etc.)] --requires--> [AEAD Encrypted Transport]
                                                                      [Request-Response Dispatch]
```

### Dependency Notes

- **PQ Handshake requires Identity**: The handshake signs the session fingerprint with the client's ML-DSA-87 key. No identity = no connection.
- **AEAD Transport requires Handshake**: Session keys are derived from the handshake. The transport layer cannot function without completed key exchange.
- **All operations require AEAD Transport**: Every message after the handshake is encrypted. There is no unencrypted message path for clients.
- **Write/Delete require Identity for signing**: The blob signing input includes the namespace (derived from identity pubkey). The signature uses the identity's secret key.
- **Pub/Sub is partially independent of request_id dispatch**: Subscribe/Unsubscribe are normal request-response. But Notification messages arrive asynchronously with request_id=0 and must be routed to the subscription queue, not the request dispatch table.
- **Sync Wrapper requires complete Async Client**: The wrapper delegates every method to the async implementation. It cannot be built incrementally -- it needs the async API to be stable first.

## MVP Definition

### Launch With (v1)

Minimum viable SDK -- enough for a developer to connect, store blobs, read them back, and iterate results.

- [ ] **Identity management** -- generate/load ML-DSA-87 keypairs, derive namespace
- [ ] **PQ handshake** -- ML-KEM-1024 key exchange + ML-DSA-87 mutual authentication with relay
- [ ] **AEAD encrypted transport** -- ChaCha20-Poly1305 framing with counter nonces
- [ ] **FlatBuffers encoding/decoding** -- TransportMessage and Blob encode/decode
- [ ] **Request-response dispatch** -- request_id assignment, response matching, concurrent support
- [ ] **Write blobs** -- canonical signing input, ML-DSA-87 sign, Data(8) + WriteAck(30)
- [ ] **Read blobs** -- ReadRequest(31) + ReadResponse(32), return typed Blob or None
- [ ] **Delete blobs** -- tombstone construction, Delete(17) + DeleteAck(18)
- [ ] **List blobs with pagination** -- ListRequest(33) + ListResponse(34), auto-paginating async iterator
- [ ] **Exists check** -- ExistsRequest(37) + ExistsResponse(38)
- [ ] **Typed exceptions** -- ChromatinError hierarchy covering all error conditions
- [ ] **Context manager** -- `async with` for connection lifecycle
- [ ] **Keepalive** -- background Ping/Pong to prevent inactivity disconnect
- [ ] **Timeout support** -- per-operation and default timeouts
- [ ] **Graceful disconnect** -- Goodbye message, cleanup

### Add After Validation (v1.x)

Features to add once the core write/read/list loop is proven.

- [ ] **Pub/sub notifications** -- async iterator for real-time blob change events
- [ ] **Sync (blocking) wrapper** -- separate `ChromatinClient` class for non-async code
- [ ] **Batch operations** -- batch_exists, batch_read for multi-blob efficiency
- [ ] **Node introspection** -- node_info, storage_status, peer_info queries
- [ ] **Namespace operations** -- list_namespaces, namespace_stats with pagination
- [ ] **Blob metadata** -- metadata query without data transfer
- [ ] **Delegation management** -- delegate, revoke, list_delegations
- [ ] **Time range queries** -- time_range for temporal blob discovery
- [ ] **Stats query** -- per-namespace stats (count, bytes, quota)

### Future Consideration (v2+)

Features to defer until the SDK has real users providing feedback.

- [ ] **Auto-reconnect with backoff** -- defer because it requires re-subscription state tracking, is complex, and most initial users will be in controlled environments
- [ ] **Request pipelining context** -- explicit `pipeline()` context manager for batch-submit patterns; individual `await` already pipelines naturally
- [ ] **CLI tool** -- admin operations via command line; separate package, not part of SDK
- [ ] **Capability negotiation** -- use NodeInfoResponse.supported_types to warn/error when calling unsupported operations

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| PQ handshake + AEAD transport | HIGH | HIGH | P1 |
| Identity management | HIGH | MEDIUM | P1 |
| Write blobs (with signing) | HIGH | HIGH | P1 |
| Read blobs | HIGH | MEDIUM | P1 |
| Delete blobs | HIGH | MEDIUM | P1 |
| List blobs (paginated) | HIGH | MEDIUM | P1 |
| Request-response dispatch | HIGH | HIGH | P1 |
| Typed exceptions | HIGH | LOW | P1 |
| Context manager + graceful disconnect | HIGH | LOW | P1 |
| Keepalive | MEDIUM | LOW | P1 |
| Timeout support | HIGH | LOW | P1 |
| Exists check | MEDIUM | LOW | P1 |
| Pub/sub notifications | HIGH | MEDIUM | P2 |
| Sync wrapper | HIGH | MEDIUM | P2 |
| Batch exists/read | MEDIUM | LOW | P2 |
| Node info + storage status | MEDIUM | LOW | P2 |
| Namespace list + stats | MEDIUM | LOW | P2 |
| Blob metadata | MEDIUM | LOW | P2 |
| Delegation management | MEDIUM | MEDIUM | P2 |
| Time range queries | LOW | LOW | P2 |
| Stats query | LOW | LOW | P2 |
| Peer info | LOW | LOW | P2 |
| Auto-reconnect | MEDIUM | HIGH | P3 |
| Pipeline context | LOW | MEDIUM | P3 |

**Priority key:**
- P1: Must have for launch -- core connection, crypto, read/write loop
- P2: Should have, add when core is stable -- remaining 38 message types, sync wrapper, pub/sub
- P3: Nice to have, future consideration -- complex infrastructure features

## Competitor Feature Analysis

| Feature | redis-py | boto3 (S3) | nats-py | cassandra-driver | Our Approach |
|---------|----------|------------|---------|------------------|--------------|
| Sync + Async | Separate `Redis` / `redis.asyncio.Redis` classes | aioboto3 wraps boto3 | Async-only | Sync with async futures | Async-first (`chromatindb.aio`), sync wrapper (`chromatindb`) |
| Connection | `Redis(host, port)` context manager | `boto3.client('s3')` | `await nats.connect(url)` | `Cluster([ips]).connect()` | `await ChromatinClient.connect(host, port, identity)` |
| Auth | Password/TLS | AWS credentials (IAM) | Token/NKey/JWT | Username/password | ML-DSA-87 keypair (Identity object) |
| Write | `set(key, value)` | `put_object(Body=data)` | `publish(subject, data)` | `execute(INSERT ...)` | `write(data, ttl=0)` auto-signs with identity |
| Read | `get(key)` returns bytes | `get_object()` returns stream | `subscribe(subject)` | `execute(SELECT ...)` | `read(namespace, hash)` returns Blob |
| Pagination | SCAN cursor | `list_objects_v2(ContinuationToken)` | N/A | Auto-paging ResultSet | Auto-paging async iterator (`list_all()`) |
| Pub/Sub | `pubsub.subscribe()`, `listen()` | N/A (use SNS) | `subscribe(subject, cb)` | N/A | `subscribe([namespaces])` async iterator |
| Batch ops | Pipeline | `delete_objects()` (limited batch) | N/A | `execute_concurrent()` | `batch_exists()`, `batch_read()` protocol-native |
| Error types | `ConnectionError`, `ResponseError` | `ClientError`, `BotoCoreError` | `NatsError` hierarchy | `DriverException` hierarchy | `ChromatinError` hierarchy |
| Multiplexing | Pipeline (explicit batch) | Per-request connection | Single connection, subjects | Connection per host | request_id multiplexing (automatic) |
| Reconnect | Built-in retry | Not applicable (HTTP) | `allow_reconnect=True` | Built-in reconnection | Deferred to P3 |

### Key Insight from Competitor Analysis

The closest analogues are **redis-py** (persistent TCP connection, binary protocol, pub/sub) and **nats-py** (async-first, persistent connection, reconnection, pub/sub). boto3 and cassandra-driver have less relevant patterns because they use HTTP or their own multiplexing layers.

chromatindb's request_id multiplexing is architecturally identical to HTTP/2 stream IDs or NATS request-reply -- a single connection handles all concurrency. This means **no connection pool needed**, which simplifies the SDK significantly compared to redis-py (which needs pools for sync pub/sub + commands on same connection).

### Key SDK API Shape (Recommended)

Based on competitor analysis, the recommended API surface:

```python
# Async API (primary)
from chromatindb.aio import ChromatinClient
from chromatindb import Identity

identity = Identity.generate()
# or: identity = Identity.from_file("~/.chromatindb/identity")

async with ChromatinClient.connect("relay.example.com", 4201, identity) as client:
    # Write
    ack = await client.write(b"hello world", ttl=3600)
    # ack.hash, ack.seq_num, ack.status

    # Read
    blob = await client.read(identity.namespace, ack.hash)
    # blob.data, blob.timestamp, blob.ttl

    # Delete
    await client.delete(identity.namespace, ack.hash)

    # List with auto-pagination
    async for entry in client.list_all(identity.namespace):
        print(entry.hash, entry.seq_num)

    # Pub/sub
    async for notification in client.subscribe([identity.namespace]):
        print(notification.hash, notification.is_tombstone)

# Sync API (wrapper)
from chromatindb import ChromatinClient, Identity

identity = Identity.from_file("~/.chromatindb/identity")
with ChromatinClient.connect("relay.example.com", 4201, identity) as client:
    ack = client.write(b"hello world", ttl=3600)
    blob = client.read(identity.namespace, ack.hash)
```

## Sources

- [Azure SDK Python Design Guidelines](https://azure.github.io/azure-sdk/python_design.html) -- sync/async dual client pattern, naming conventions, pagination, exception hierarchy (HIGH confidence)
- [redis-py asyncio documentation](https://redis.readthedocs.io/en/stable/examples/asyncio_examples.html) -- context manager pattern, connection pool ownership, pub/sub iteration (HIGH confidence)
- [nats-py client](https://github.com/nats-io/nats.py) -- async-first design, reconnection with backoff, callback events (HIGH confidence)
- [liboqs-python](https://github.com/open-quantum-safe/liboqs-python) -- ML-DSA-87 Signature class, ML-KEM-1024 KeyEncapsulation class API (HIGH confidence)
- [FlatBuffers Python runtime](https://flatbuffers.dev/languages/python/) -- Builder class, generated accessors, numpy optimization (HIGH confidence)
- [Python hashlib SHA3-256](https://docs.python.org/3/library/hashlib.html) -- stdlib, no external dependency (HIGH confidence)
- [cryptography library HKDF](https://cryptography.io/en/latest/hazmat/primitives/key-derivation-functions/) -- HKDF-SHA256 for session key derivation (HIGH confidence)
- [libsodium IETF ChaCha20-Poly1305](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction) -- AEAD primitives for transport encryption (HIGH confidence)
- [Google Page Iterators](https://googleapis.dev/python/google-api-core/latest/page_iterator.html) -- auto-pagination wrapper pattern (HIGH confidence)
- [Cassandra Python Driver ResultSet](https://docs.datastax.com/en/developer/python-driver/3.18/api/cassandra/cluster/) -- transparent pagination in iterators (MEDIUM confidence)
- [HTTPX Exception Hierarchy](https://www.python-httpx.org/exceptions/) -- ProtocolError, LocalProtocolError, RemoteProtocolError pattern (MEDIUM confidence)
- [chromatindb PROTOCOL.md](../../../db/PROTOCOL.md) -- authoritative wire format for all 58 message types (HIGH confidence, primary source)

---
*Feature research for: Python SDK for chromatindb*
*Researched: 2026-03-28*
