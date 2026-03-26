# Pitfalls Research: v1.4.0 Extended Query Suite

**Domain:** Adding batch queries, time-range queries, health endpoints, delegation/namespace enumeration, and metadata inspection to a coroutine-based C++20 storage node
**Researched:** 2026-03-26
**Confidence:** HIGH (derived from codebase analysis of 13 shipped milestones, proven patterns, and known failure modes)

## Critical Pitfalls

### Pitfall 1: Batch Response Frame Size Exceeding MAX_FRAME_SIZE

**What goes wrong:**
BatchRead returns multiple full blobs in a single response message. If the client requests N blobs and each is up to 100 MiB, the response payload can exceed MAX_FRAME_SIZE (110 MiB). The `send_encrypted` call succeeds at the sender but `recv_raw` at the receiver sees `len > MAX_FRAME_SIZE` and drops the frame as invalid, causing a silent response loss or connection teardown.

**Why it happens:**
Existing single-item queries (ReadResponse, ExistsResponse, StatsResponse) produce small, bounded responses. BatchRead is the first query type where the response size scales with request count AND blob sizes. Developers naturally aggregate results into a single response vector without checking total size against the protocol's frame limit.

**How to avoid:**
1. Cap `max_batch_size` at a low, safe value (e.g., 64 items for BatchExists, 16 for BatchRead).
2. For BatchRead, enforce a cumulative response size cap well below MAX_FRAME_SIZE (e.g., 100 MiB). Stop adding blobs to the response once the cap would be exceeded, and set a `partial` flag in the response.
3. Alternative design: return blob metadata (hash + size) for oversized items instead of inline data, letting the client fall back to individual ReadRequests for large blobs.
4. Validate: construct a worst-case response in tests (max items * max individual size) and assert it fits within MAX_FRAME_SIZE.

**Warning signs:**
- BatchRead tests only use small blobs (< 1 KiB) and never hit the frame limit.
- No test that sends a batch request resulting in > 100 MiB aggregate response.
- Response builder does not track cumulative size during iteration.

**Phase to address:**
BatchRead phase -- must be designed into the wire format from the start. The response format needs a `partial` indicator and the handler needs cumulative size tracking.

---

### Pitfall 2: AEAD Nonce Desync from Concurrent Batch/Health Responses

**What goes wrong:**
`send_counter_` in Connection is a non-atomic `uint64_t` incremented inside `send_encrypted()`. The existing dispatch model is safe because all `send_message()` calls happen on the IO thread (coroutine-IO handlers run on `ioc_`, and offload-transfer handlers do `co_await asio::post(ioc_)` before sending). But if a new handler (e.g., Health or BatchRead) is mistakenly dispatched without the IO-thread transfer pattern, or if a handler spawns a detached coroutine that sends a response while another handler's coroutine also sends on the same connection, the nonce counter races and the remote side fails to decrypt subsequent messages.

This is the exact same bug class as the PEX SIGSEGV (v1.0.0 Phase 50) where a detached SyncRejected coroutine raced with the sync initiator's writes.

**Why it happens:**
With 11 new message types being added, each requiring a co_spawn handler, it is easy to:
- Forget the `co_await asio::post(ioc_)` transfer after an offload.
- Accidentally create a handler that does thread pool work and then sends without transferring back.
- Copy-paste a Data/Delete handler pattern but omit the transfer step because the new handler "doesn't need offload" (but later adds one).

**How to avoid:**
1. Enforce the dispatch model classification established in Phase 62: every new handler must be categorized as inline, coroutine-IO, or coroutine-offload-transfer.
2. All 11 new handlers should be coroutine-IO (co_spawn on ioc_) since they only do synchronous storage reads. None should need thread pool offload (no crypto, no large-blob hashing).
3. If any handler later needs offload (e.g., BatchRead for large response encoding), add the `co_await asio::post(ioc_)` transfer before the `send_message` call.
4. Test under TSAN with concurrent request pipelining -- multiple in-flight requests on the same connection.

**Warning signs:**
- TSAN reports data race on `send_counter_`.
- Remote side logs AEAD decryption failures after a burst of pipelined requests.
- Tests only send one request at a time per connection (no pipelining).

**Phase to address:**
Every phase that adds a new handler. The dispatch classification comment block in `on_peer_message` must be updated with each new type.

---

### Pitfall 3: Time-Range Query Without Secondary Index -- Full Namespace Scan

**What goes wrong:**
TimeRange queries need blobs within a timestamp window `[start_us, end_us]` for a given namespace. Timestamps are stored INSIDE the encrypted blob payload (in the FlatBuffer-encoded BlobData). The current indexes are:
- `blobs_map`: `[namespace:32][hash:32]` -> encrypted blob (no timestamp in key)
- `seq_map`: `[namespace:32][seq_be:8]` -> `hash:32` (seq_num in key, not timestamp)
- `expiry_map`: `[expiry_ts_be:8][hash:32]` -> `namespace:32` (has a time component but it is expiry time = timestamp/1000000 + ttl, not blob timestamp)

Without a timestamp-indexed sub-database, TimeRange queries must: iterate the seq_map for the namespace, fetch each blob from blobs_map, decrypt it, decode the FlatBuffer, and check the timestamp field. For a namespace with 10,000 blobs, that is 10,000 decrypt + decode operations per query.

**Why it happens:**
Timestamps were designed as a signed-blob field (part of the canonical signing input), not as a query dimension. The storage layer was built for content-addressed retrieval and sequential polling (seq_num), not temporal queries.

**How to avoid:**
Two options, ranked by preference:

1. **Leverage seq_num ordering as a timestamp proxy (recommended).** Seq_nums are monotonically increasing per namespace and roughly correlate with insertion time. The TimeRange handler can:
   - Binary search the seq_map for the approximate start position using the existing `lower_bound` cursor pattern.
   - Iterate forward, fetching + decrypting blobs and checking timestamps.
   - Stop early when timestamps exceed the end of the window (relying on approximate monotonicity).
   - This avoids a new sub-database but requires decrypting blobs in the range. Cap iteration at a limit (e.g., 100 results) to bound work.

2. **Add a timestamp sub-database.** Key: `[namespace:32][timestamp_be:8]`, Value: `[hash:32]`. Populated on store_blob, cleaned on delete. Enables pure index-based range queries without decryption. But this adds a new sub-database (8th), increases write amplification, and requires migration for existing data.

Option 1 is correct for v1.4.0: YAGNI on the index, use limit + cursor pagination, accept that time-range is approximate-then-filter. If performance is later unacceptable, option 2 can be added.

**Warning signs:**
- TimeRange query latency > 100ms on namespaces with > 1000 blobs.
- CPU spikes during time-range queries from decryption overhead.
- Client sends open-ended time range (start=0) triggering full namespace scan.

**Phase to address:**
TimeRange phase. The decision (option 1 vs 2) must be made during plan design, not during implementation. The wire format should include a `limit` field and `has_more` flag regardless of which approach is chosen.

---

### Pitfall 4: Relay Message Filter Not Updated for New Types

**What goes wrong:**
The relay's `message_filter.cpp` has a `switch` with explicit `case` entries for each allowed client message type. Adding 11 new request/response pairs (22 new enum values in `transport.fbs`) requires updating THREE locations:
1. `transport.fbs` -- enum definition
2. `message_filter.cpp` -- `is_client_allowed()` switch
3. `NodeInfoResponse` supported_types array in `peer_manager.cpp`

If any of these are missed:
- Missing from transport.fbs: compilation error (caught early, good).
- Missing from message_filter.cpp: relay disconnects clients who send the new request type ("blocked message type" teardown). This looks like the relay is broken but the node works fine over UDS.
- Missing from supported_types: SDK capability detection fails for the new type.

**Why it happens:**
The relay is a separate binary (`relay/`) and separate mental context. When adding a handler to `peer_manager.cpp`, the developer is focused on the node code. The relay filter and NodeInfo supported_types are easy to forget because they are in different files and don't cause compilation errors.

**How to avoid:**
1. Make a checklist for every new message type pair: transport.fbs, message_filter.cpp, NodeInfoResponse supported_types, PROTOCOL.md wire format.
2. Write a test that exercises each new request/response through the relay path (client -> relay -> node -> relay -> client).
3. Consider: a static_assert or compile-time check that the supported_types array size matches the number of client-allowed types.

**Warning signs:**
- New query works over UDS but fails through relay.
- Relay logs show "blocked message type {N}" for the new type numbers.
- NodeInfoResponse supported_types count doesn't increase after adding new types.

**Phase to address:**
Every phase that adds new message types. This is a per-phase checklist item, not a single-phase concern. The relay filter update should be part of every plan that defines new TransportMsgType values.

---

### Pitfall 5: DelegationList Cursor Iteration During Concurrent Delegation Modification

**What goes wrong:**
DelegationList iterates the `delegation_map` sub-database with a cursor, filtering entries by namespace prefix. If a concurrent ingest (on the IO thread) creates or revokes a delegation blob while the DelegationList query is iterating:
- libmdbx read transactions provide MVCC snapshot isolation, so the cursor sees a consistent snapshot. This is SAFE.
- **BUT**: if the DelegationList handler uses a write transaction (accidentally) or if the handler is not wrapped in a read transaction (iterates individual `get()` calls instead), each call sees a different snapshot, which CAN produce inconsistent results (e.g., listing a delegation that was just revoked, or missing one that was just created).

**Why it happens:**
Existing iteration patterns (list_namespaces, get_blob_refs_since) correctly use `start_read()` + cursor. But DelegationList is the first query that iterates the delegation_map, which has a different key structure (`[namespace:32][delegate_pk_hash:32]`). A developer might implement it as a loop of `has_valid_delegation()` calls (each opening its own read txn) instead of a single cursor scan, breaking snapshot consistency.

**How to avoid:**
1. Implement DelegationList as a single Storage method that opens ONE read transaction, creates a cursor on delegation_map, seeks to the namespace prefix, and iterates entries with matching namespace prefix.
2. Do NOT implement it as N calls to `has_valid_delegation()` -- each opens a separate read txn.
3. Follow the exact pattern of `list_namespaces()`: `start_read()`, `open_cursor()`, prefix scan with `lower_bound()`.

**Warning signs:**
- DelegationList returns different results on consecutive calls without any writes (non-reproducible inconsistency from separate txns).
- The storage method for DelegationList calls other storage methods instead of using a cursor directly.

**Phase to address:**
DelegationList phase. The storage API design must be a single-txn cursor scan.

---

### Pitfall 6: Health Endpoint Blocking the IO Thread

**What goes wrong:**
Health/readiness checks need to be lightweight and fast. If the Health handler calls `storage_.used_bytes()` or `storage_.list_namespaces()` or `engine_.list_namespaces()`, these are synchronous libmdbx operations that open transactions. Under heavy load with long-running write transactions, MVCC readers can block on the B-tree lock momentarily. If health checks are frequent (e.g., Kubernetes liveness probe every 5s), this adds IO-thread contention.

Worse: if the health handler includes "can I write?" verification (e.g., opening a write txn to prove storage is writable), this blocks the IO thread until any in-flight write transaction completes. Since libmdbx serializes write transactions, a compaction in progress (which opens a long write via `env.copy()`) would make the health endpoint hang.

**Why it happens:**
Health endpoints in traditional HTTP servers are trivial (return 200 OK). In a single-IO-thread event loop with MVCC storage, "is the database healthy?" is not free. The temptation is to check storage health thoroughly, but this conflicts with the non-blocking IO model.

**How to avoid:**
1. Health/liveness should be a pure in-memory check: is the node running? Is the io_context not stopped? Return immediately. No storage access.
2. Readiness can check lightweight state: `peer_count() > 0`, `!stopping_`, maybe `storage_.used_bytes()` (O(1) mdbx env info, no transaction needed).
3. Do NOT open write transactions in health checks.
4. Do NOT call `list_namespaces()` or any cursor-based scan from health checks.
5. Classify Health as INLINE dispatch (like Subscribe/Unsubscribe) -- no co_spawn needed if no storage access.

**Warning signs:**
- Health check latency spikes during compaction.
- Health check opens a read or write transaction.
- Kubernetes kills the pod because the liveness probe timed out during a long write.

**Phase to address:**
Health/StorageStatus phase. Health must be designed as a zero-IO path. StorageStatus can use the O(1) `used_bytes()`/`used_data_bytes()` but should avoid cursor scans.

---

### Pitfall 7: NamespaceList Response Size Unbounded on Large Nodes

**What goes wrong:**
The existing `list_namespaces()` storage method returns ALL namespaces as a vector. For a node storing 10,000 namespaces, the NamespaceList response would be `4 + 10000 * 40 = ~400 KB` (count + namespace_id:32 + latest_seq:8 per entry). This is within MAX_FRAME_SIZE but:
1. The list_namespaces() cursor scan with seek-to-max per namespace is O(N * log(B)) where B is B-tree depth. For 10,000 namespaces, this could take 100+ ms.
2. The response is built entirely in memory before sending.
3. There is no pagination -- client gets everything or nothing.

For the 1000-namespace Docker stress test, this is fine. For a production node with 100,000+ namespaces, this becomes a DoS vector: any client can trigger an expensive full scan.

**Why it happens:**
The existing `list_namespaces()` was designed for internal use (integrity_scan, node info) where scanning all namespaces is acceptable. Exposing it as a client-facing query without pagination follows the "works in testing, fails in production" anti-pattern.

**How to avoid:**
1. Add `offset` and `limit` parameters to the NamespaceList wire format.
2. Implement a `list_namespaces_paginated(cursor_ns, limit)` storage method that uses `lower_bound(cursor_ns)` and returns up to `limit` entries plus a `has_more` flag.
3. Cap limit at a reasonable value (e.g., 100, matching ListResponse cap).
4. The response format should include the last namespace_id as a cursor for the next page.

**Warning signs:**
- NamespaceList tests only create 1-5 namespaces.
- No limit parameter in the request wire format.
- Handler calls `storage_.list_namespaces()` directly without pagination.

**Phase to address:**
NamespaceList phase. Pagination must be in the wire format from the start -- retrofitting pagination is a breaking protocol change.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Using seq_num as timestamp proxy for TimeRange | No new sub-database, no migration | Imprecise results for non-monotonic timestamps (delegated writes, sync-received blobs) | v1.4.0 MVP -- add timestamp index later if clients need precision |
| Returning all delegations without pagination | Simpler wire format | Unbounded response for namespaces with many delegates | Acceptable if namespace delegation count is naturally small (< 100) |
| Health as inline dispatch with no storage check | Fastest possible response | Health "OK" doesn't guarantee storage is accessible | Acceptable -- StorageStatus provides deeper checks, Health is liveness only |
| Hardcoded batch limits (e.g., 64 for BatchExists) | Simple, safe | Client can't request larger batches if network allows | Acceptable pre-SDK -- can be made configurable later |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| transport.fbs enum extension | Adding new types after MAX without updating MAX sentinel | Always add before the closing brace; regenerate `transport_generated.h` and verify `TransportMsgType_MAX` updates |
| Relay message filter | Adding request type but forgetting response type (or vice versa) | Always add both request AND response to `is_client_allowed()` -- responses flow node -> relay -> client |
| NodeInfoResponse supported_types | Hardcoded array in peer_manager.cpp becomes stale | Update the `supported[]` array AND `types_count` every time a new client-facing type is added |
| Storage method signatures | Using `std::span<const uint8_t>` (unsized) instead of `std::span<const uint8_t, 32>` for namespace/hash params | Use sized spans (`span<const uint8_t, 32>`) to get compile-time size checking -- matches existing API |
| libmdbx cursor lifecycle | Opening cursor on a map that doesn't exist (typo in map name) | Maps are created at startup in `open_env()` -- new sub-databases need to be added there |
| Binary wire format | Forgetting big-endian encoding for multi-byte integers | Use the existing `encode_be_u64()` / `encode_be_u32()` helpers; never use host-byte-order memcpy for wire integers |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| BatchRead fetching + decrypting N blobs sequentially | Response latency = N * decrypt_time (~0.5ms each) | Cap batch size at 16-32; consider parallel decrypt only if profiling shows bottleneck | > 64 blobs per batch |
| NamespaceList full scan with seek-per-namespace | Latency grows linearly with namespace count | Paginate with limit; cache namespace list if needed | > 1000 namespaces |
| TimeRange decrypting all blobs in seq range | CPU spike on large time windows | Enforce limit per request; stop iteration at limit | > 1000 blobs in time window |
| DelegationList scanning entire delegation_map | Slow if delegation_map has entries for many namespaces | Prefix scan with `lower_bound([ns][0x00...])` and stop at namespace boundary | > 10000 total delegations across all namespaces |
| StorageStatus calling `list_namespaces()` for tombstone count | Full namespace scan just to count tombstones | Use `txn.get_map_stat(tombstone_map).ms_entries` for O(1) tombstone count | Always -- there is no reason to scan |
| Multiple concurrent BatchRead requests from same client | Pipelined requests each decrypt N blobs, multiplying IO-thread work | Per-connection concurrency limit or queue depth limit | > 4 concurrent batch requests |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| BatchExists leaking tombstone existence | Client can probe whether a specific blob was deleted (information leak about deletion activity) | Return `exists=false` for tombstoned blobs (consistent with ExistsRequest: "tombstones return false") |
| NamespaceList exposing all namespace IDs to any client | In closed-node mode, enumerating all namespaces reveals what pubkeys have stored data | Consider: should NamespaceList be filtered by ACL? Or is this acceptable since the client already authenticated? Document the decision. |
| PeerInfo exposing peer IP addresses | Client can enumerate all connected peer IPs (network topology disclosure) | Only expose peer count, not addresses. Or expose only hashed identifiers. |
| Health endpoint as availability oracle | External attacker probes health to confirm node is alive before targeting | Health should require authentication (relay enforces this since all client messages require PQ handshake) |
| Unbounded batch requests as DoS amplification | Client sends BatchRead with 1000 hashes, node does 1000 storage lookups + decryptions | Enforce strict batch size limits in the handler (reject oversized requests with strike) |

## "Looks Done But Isn't" Checklist

- [ ] **BatchExists:** Does it handle duplicate hashes in the request? (client sends same hash twice -- response should not have duplicates or should handle gracefully)
- [ ] **BatchRead:** Does it respect MAX_FRAME_SIZE for the aggregate response? Build a test with N blobs whose total size approaches the limit.
- [ ] **TimeRange:** Does it handle the case where `start_timestamp > end_timestamp`? (reject or return empty)
- [ ] **TimeRange:** Does it handle microsecond vs second timestamp confusion? (blob.timestamp is seconds, but the field name is ambiguous -- verify units in wire format docs)
- [ ] **DelegationList:** Does it return delegation blob hashes (so client can fetch the full delegation) or just delegate pubkey hashes? The delegation_map value is `[delegation_blob_hash:32]` -- decide what the client needs.
- [ ] **NamespaceList:** Does it have pagination? Without it, works in tests but fails at scale.
- [ ] **NamespaceStats:** Does it handle non-existent namespaces? (return zeros, not error -- consistent with get_namespace_quota returning {0,0} for unknown)
- [ ] **Health:** Is it truly zero-IO? Check that no storage method is called.
- [ ] **StorageStatus:** Does it include tombstone_map stats? The map stat is O(1) but must be explicitly queried.
- [ ] **PeerInfo:** Does it avoid leaking peer IP addresses? Design the response format before implementation.
- [ ] **All new types:** Are all 22 new enum values (11 request + 11 response) added to message_filter.cpp?
- [ ] **All new types:** Are all new client-facing types added to NodeInfoResponse supported_types array?
- [ ] **All new types:** Does PROTOCOL.md document the byte-level wire format for each new message?

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Frame size exceeded in BatchRead | LOW | Add cumulative size check + partial flag; no wire format change needed if designed with partial from start |
| AEAD nonce desync | HIGH | Debug via TSAN; fix by ensuring all send_message calls are on IO thread; may need to audit all 11 new handlers |
| Missing relay filter entries | LOW | Add the missing case statements; write a relay integration test to prevent regression |
| Unbounded NamespaceList | MEDIUM | Requires wire format change to add pagination; breaking change for any deployed clients |
| Health blocking on storage | LOW | Remove storage calls from health handler; pure refactor |
| TimeRange without index | MEDIUM | If seq_num proxy is too imprecise, adding a timestamp sub-database requires storage migration |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Batch frame size overflow | BatchRead design phase | Test with aggregate response > 50 MiB; assert response fits in MAX_FRAME_SIZE |
| AEAD nonce desync | Every handler phase | Run pipelined request tests under TSAN; verify all send_message calls are on IO thread |
| Time-range scan cost | TimeRange phase | Benchmark TimeRange on 10,000-blob namespace; verify limit enforced |
| Relay filter missing types | Every phase adding types | Test each new type through relay path; relay disconnect = failure |
| DelegationList snapshot consistency | DelegationList phase | Implement as single-txn cursor scan; code review for multi-txn anti-pattern |
| Health blocking IO | Health phase | Verify zero storage calls in health handler; measure latency during compaction |
| NamespaceList unbounded | NamespaceList phase | Wire format includes limit + cursor; test with 1000+ namespaces |
| NodeInfoResponse stale types | Final docs/integration phase | Assert supported_types count matches relay filter allowed count |
| Timestamp unit confusion | TimeRange phase | Document units explicitly in PROTOCOL.md wire format; test with known timestamps |
| Batch duplicate handling | BatchExists/BatchRead phase | Test with duplicate hashes in request; verify no crash or duplicate responses |

## Sources

- Codebase analysis: `db/net/connection.cpp` line 155 (`send_counter_++` in `send_encrypted`)
- Codebase analysis: `db/peer/peer_manager.cpp` lines 523-538 (dispatch model comment block)
- Codebase analysis: `db/storage/storage.cpp` lines 696-783 (`list_namespaces` cursor pattern)
- Codebase analysis: `relay/core/message_filter.cpp` (explicit allow-list switch)
- Codebase analysis: `db/net/framing.h` (`MAX_FRAME_SIZE` = 110 MiB)
- Codebase analysis: `db/storage/storage.h` line 82 ("Thread safety: NOT thread-safe")
- Project memory: PEX SIGSEGV (v1.0.0) -- AEAD nonce desync from concurrent SyncRejected writes
- Project memory: IO-thread transfer pattern (v1.3.0 Phase 62) -- `co_await asio::post(ioc_)` before send_message
- Project memory: Coroutine params by value (v0.9.0) -- const ref captures dangle across suspension points
- Retrospective: v0.8.0 -- "Thread pool offload for stateless crypto is straightforward; the hard part is ensuring stateful AEAD is never accessed from workers"
- Retrospective: v1.3.0 -- dispatch model classification (inline, coroutine-IO, coroutine-offload-transfer)

---
*Pitfalls research for: chromatindb v1.4.0 Extended Query Suite*
*Researched: 2026-03-26*
