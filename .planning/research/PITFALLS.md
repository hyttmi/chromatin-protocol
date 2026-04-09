# Pitfalls Research

**Domain:** WebSocket/JSON/TLS gateway relay for a FlatBuffers/AEAD binary protocol node (chromatindb Relay v2)
**Researched:** 2026-04-09
**Confidence:** HIGH (Asio Beast behavior verified via official docs + GitHub issues; binary-JSON translation pitfalls verified against existing wire protocol in PROTOCOL.md; backpressure/multiplexing pitfalls verified from existing codebase patterns and distributed systems literature)

## Critical Pitfalls

### Pitfall 1: Beast WebSocket one-async-write-at-a-time rule

**What goes wrong:**
Beast's `websocket::stream` asserts (then crashes) if a second `async_write` is initiated while one is already pending, even when both calls are on the same strand. This is not about thread safety -- strands serialize execution but do not prevent a coroutine from calling `async_write` while a previous `async_write` from a different coroutine is still in flight. In the relay, the node-to-client forwarding path and a concurrent Ping/challenge-response could both try to write to the same WebSocket stream simultaneously.

**Why it happens:**
Beast's WebSocket layer maintains internal framing state (opcode, fragmentation progress, mask state) that is not re-entrant. The library enforces "at most one active async read and one active async write" as a contract, not a suggestion. Violating it triggers `BOOST_ASSERT` in debug builds and undefined behavior in release builds. Developers familiar with raw TCP sockets (where queuing writes is the OS's job) or with the existing Connection class (which already solved this) miss that Beast requires the same discipline at the WebSocket layer.

**How to avoid:**
Implement a per-client send queue with a single drain coroutine, identical to the existing `Connection::drain_send_queue()` pattern. Every outbound WebSocket message (forwarded node response, Pong, challenge, close frame) goes through the queue. The drain coroutine pops one message at a time and calls `ws.async_write()`. This is a proven pattern in the codebase -- reuse it directly.

```
// Pseudocode: single drain coroutine per WebSocket session
while (!closed) {
    while (!queue.empty()) {
        auto msg = queue.pop_front();
        co_await ws.async_write(asio::buffer(msg));
    }
    co_await signal.async_wait();  // wake on enqueue
}
```

**Warning signs:**
- Any code path that calls `ws.async_write()` outside the drain coroutine
- Crashes in `pausation.hpp` or assertion failures in debug builds
- Intermittent TSAN reports on WebSocket stream internal state

**Phase to address:**
Phase 1 (core WebSocket session scaffolding). The send queue must exist before any message forwarding is wired up.

---

### Pitfall 2: Binary blob data in JSON -- base64 blowup and encoding bugs

**What goes wrong:**
The relay must translate binary FlatBuffer payloads to JSON for WebSocket clients. Every message payload contains raw byte arrays (32-byte namespace IDs, 32-byte hashes, variable-length blob data up to 100 MiB, 2592-byte ML-DSA-87 public keys, 4627-byte signatures). Naively base64-encoding a 100 MiB blob produces 133 MiB of JSON text. Hex-encoding produces 200 MiB. Both create massive memory allocations, slow serialization, and exceed practical WebSocket message limits.

Separately, encoding bugs are likely at field boundaries: the node uses big-endian integers for `seq_num` (uint64), `ttl` (uint32), `timestamp` (uint64), `limit` (uint32), and `count` (uint32). If the relay reads these as native-endian or parses JSON numbers with floating-point truncation (JavaScript clients cannot represent uint64 faithfully as `Number`), data silently corrupts. A uint64 `seq_num` of `2^53 + 1` round-trips through JavaScript `Number` as `2^53`, causing missed blobs in pagination.

**Why it happens:**
JSON has no binary type. Base64 is the standard compromise but adds 33% overhead and two encode/decode passes. For small payloads (hashes, keys) this is fine. For 100 MiB blob data, it is catastrophic. Additionally, nlohmann/json has documented slow performance parsing large base64 strings (200ms+ for ~1 MB base64 in issue #3202).

The uint64 truncation issue is intrinsic to JSON's number type (IEEE 754 double, 53-bit mantissa). Most JSON libraries silently truncate. The node's PROTOCOL.md uses uint64 for seq_num, timestamp, and storage_used -- all of which can exceed 2^53.

**How to avoid:**
1. **Use WebSocket binary frames for blob data.** Beast supports `ws.binary(true)` to set the opcode. Send ReadResponse, BatchReadResponse, and Data (type 8) payloads as binary WebSocket frames containing the raw FlatBuffer-encoded blob, not JSON. The JSON envelope carries metadata (type, request_id, status), and a separate binary frame carries the blob payload. This eliminates base64 overhead entirely for the hot path.

2. **Use hex encoding for fixed-size crypto fields** (namespace_id, blob_hash, pubkey). These are always 32 or 2592 bytes. Hex is human-readable for debugging, and the 2x overhead on 32 bytes is 64 bytes -- negligible. Base64 is acceptable too, but pick one and document it.

3. **Encode uint64 as JSON strings.** `"seq_num": "18446744073709551615"` instead of `"seq_num": 18446744073709551615`. This prevents JavaScript/JSON floating-point truncation. nlohmann/json supports this natively via `json::string_t`.

4. **Define the JSON schema once, up front.** Document every field name, type (hex string, decimal string, base64 string, number), and size constraint. Test round-trip fidelity for all 38 relay-allowed message types.

**Warning signs:**
- Any `json["field"] = uint64_value` without string conversion for values that can exceed 2^53
- Base64 encoding of blob data payloads (should be binary frames)
- Tests that only use small payloads and miss the 100 MiB path
- Missing schema documentation for any of the 38 message types

**Phase to address:**
Phase 2 (JSON schema definition + translation layer). Must be designed before any handlers are written. Binary frame decision for blob data must be made at schema design time, not retrofitted.

---

### Pitfall 3: Multiplexed UDS -- single point of failure for all clients

**What goes wrong:**
Relay v2 uses a single multiplexed UDS connection to the node, with `request_id` for demuxing responses. If the node restarts, crashes, or the UDS socket disconnects, every active client session loses its upstream. The relay must handle this without corrupting per-client state. Specific failure modes:

1. **Orphaned pending requests:** Client sends ReadRequest (request_id=42), relay forwards to node, node disconnects before responding. Client hangs forever waiting for request_id=42 response.
2. **Subscription loss:** All client subscriptions are registered on the node's UDS connection. Node restart wipes all subscription state. Clients stop receiving notifications with no error.
3. **Request ID collision after reconnect:** The relay reconnects to the node (new UDS connection, new AEAD nonce state). If the relay reuses request_id values that were in-flight on the old connection, responses from the new connection could be misrouted.
4. **Reconnect storm:** If 1000 clients are connected and the UDS drops, all 1000 simultaneously need subscription replay. Batching or throttling is needed to avoid overwhelming the node.

**Why it happens:**
The old relay used per-client UDS connections -- each client session had its own independent UDS pipe to the node. Relay v2 changes to a single multiplexed connection for efficiency. This introduces a shared fate dependency: node connection health directly determines all client sessions' health.

**How to avoid:**
1. **Pending request timeout + error response.** Every forwarded request gets a timer (e.g., 30 seconds). If no response arrives before timeout (either normal timeout or UDS disconnect), send an error response to the client: `{"type": "error", "request_id": 42, "reason": "upstream_timeout"}`. Never let a client hang.

2. **Subscription replay on reconnect.** Track all client subscriptions in-relay (already done in v1 relay). On UDS reconnect, batch-replay all active subscriptions in a single Subscribe message. Gate forwarding until replay completes (the `replay_pending_` pattern from the existing relay).

3. **Monotonic request_id with generation counter.** The relay assigns its own internal request_id values for the UDS connection, mapping them to per-client (client_id, client_request_id) tuples. Use a monotonically increasing counter that never wraps during a UDS session. On reconnect, the counter continues from where it left off -- no collision possible.

4. **UDS reconnect is relay-internal.** Clients do not see UDS reconnects. The relay buffers or errors pending requests, replays subscriptions, and resumes. Only if UDS reconnection fails after max retries does the relay start disconnecting clients.

**Warning signs:**
- Any code path where the relay forwards a request to node without setting a response timeout
- Missing subscription replay logic on UDS reconnect
- Request ID reuse or collision in the multiplexer mapping table
- UDS disconnect causing immediate client disconnects instead of triggering reconnect

**Phase to address:**
Phase 2-3 (multiplexer design + reconnect handling). The request_id mapping and timeout system must be designed alongside the translation layer. Reconnect logic builds on top.

---

### Pitfall 4: Backpressure inversion -- slow WebSocket client stalls all UDS reads

**What goes wrong:**
The relay reads messages from the UDS connection (node responses, notifications) and routes them to specific WebSocket clients. If one client's WebSocket is slow (congested network, slow consumer), its per-client send queue fills up. Without proper backpressure, two bad outcomes:

1. **Memory exhaustion:** The relay keeps reading from UDS and buffering messages for the slow client. With 100 MiB blobs and 1024-message queue, one slow client can consume 100 GiB of relay memory.
2. **UDS read stall (head-of-line blocking):** If the relay stops reading from UDS to apply backpressure, ALL clients stall because the UDS connection is shared. Node-side send queue fills up, potentially causing the node to disconnect the relay.

This is the fundamental tension of a multiplexed gateway: you cannot apply per-client backpressure on a shared upstream connection without affecting other clients.

**Why it happens:**
WebSocket has no built-in flow control (unlike TCP's window). The relay's UDS reader runs as a single coroutine that distributes messages. If it blocks on one client's full queue, the entire read loop stops. If it does not block, messages accumulate unbounded.

**How to avoid:**
1. **Bounded per-client send queue with disconnect-on-overflow.** This is the existing node pattern (MAX_SEND_QUEUE = 1024, disconnect on overflow). Apply it to WebSocket sessions. A slow client that falls behind gets disconnected. This is the correct trade-off for a relay -- one slow client must not degrade service for others.

2. **UDS reader never blocks on client send.** The UDS read coroutine enqueues messages to per-client queues using try-enqueue semantics. If a client queue is full, the message for that client is dropped (for notifications) or the client is disconnected (for request responses, since dropping those corrupts client state). The UDS reader proceeds immediately to the next message.

3. **Separate notification handling from request-response.** Notifications (type 21) are fire-and-forget -- dropping one is acceptable (client can poll to catch up). Request responses (ReadResponse, ListResponse, etc.) are client-initiated and must be delivered or errored. If a client's queue is full when a request response arrives, disconnect that client -- they are fatally behind.

4. **Message size limits.** Reject or refuse to forward blob data exceeding a relay-configurable maximum (e.g., 10 MiB default). The node supports 100 MiB blobs, but the relay does not need to. Relays serving web clients should have lower limits.

**Warning signs:**
- UDS read coroutine that `co_await`s on any per-client operation
- Missing `MAX_SEND_QUEUE` on WebSocket session send queues
- No distinction between droppable (notification) and non-droppable (response) messages
- Memory usage growing under load without client disconnections

**Phase to address:**
Phase 1 (session architecture). The per-client queue with overflow disconnect must be part of the initial session design. Retrofitting backpressure onto a working system is a rewrite.

---

### Pitfall 5: TLS certificate lifecycle -- reload without restart, but SSL_CTX is not atomic

**What goes wrong:**
The relay terminates TLS for WebSocket clients. Certificates expire and need rotation. The relay must reload certificates on SIGHUP without dropping existing connections. The naive approach -- calling `SSL_CTX_use_certificate_chain_file()` on the active `ssl::context` -- is undefined behavior (OpenSSL maintainers explicitly warn against modifying an active SSL_CTX). Existing SSL objects may read partially-updated state.

Additionally, if the relay creates a new `ssl::context` on SIGHUP, the TCP acceptor must be updated to use the new context for new connections. But the acceptor and its async_accept callback capture the old context by reference. This requires careful lifetime management.

**Why it happens:**
SSL_CTX is reference-counted but not thread-safe for mutation. OpenSSL's internal state (certificate chain, private key, session cache) is read by SSL objects during handshake without synchronization. The relay's existing SIGHUP infrastructure (from the node) expects `reload_config()` to atomically swap configuration, but SSL_CTX does not support atomic swap.

**How to avoid:**
1. **Atomic `shared_ptr<ssl::context>` swap.** Store the SSL context as `std::shared_ptr<asio::ssl::context>`. On SIGHUP, create an entirely new `ssl::context`, load the new cert/key into it, and atomically swap the shared_ptr (`std::atomic<std::shared_ptr>` or mutex-protected swap). Existing connections hold their own `shared_ptr` to the old context -- it stays alive until they disconnect. New connections get the new context.

2. **Validate cert/key before swap.** Load into a temporary `ssl::context` and call `SSL_CTX_check_private_key()` before publishing. Log an error and keep the old context if validation fails. Never leave the relay without a valid TLS context.

3. **Test with short-lived certs.** Use 1-minute validity certs in integration tests. SIGHUP after 30 seconds. Verify new connections use the new cert while existing connections continue on the old one.

**Warning signs:**
- `ssl::context` stored as a plain reference or raw pointer passed to the acceptor
- Any `SSL_CTX_use_*` call on a context that has active SSL objects
- SIGHUP handler that blocks the event loop while loading cert files
- Missing cert/key validation before swap

**Phase to address:**
Phase 3 (TLS termination). Must be designed when TLS support is added. Cannot be deferred to "later" because the acceptor's relationship to the SSL context is architectural.

---

### Pitfall 6: JSON-to-FlatBuffers translation -- silent field truncation and missing validation

**What goes wrong:**
The relay translates JSON messages from clients into binary payloads for the node's UDS connection. Each of the 38 relay-allowed message types has a specific binary layout documented in PROTOCOL.md (e.g., ReadRequest is exactly 64 bytes: 32-byte namespace + 32-byte hash; ListRequest is exactly 44 bytes: 32-byte namespace + 8-byte BE seq + 4-byte BE limit). Translation bugs include:

1. **Hex decode produces wrong length.** Client sends `"namespace": "abcd"` (2 bytes decoded) instead of a 64-character hex string (32 bytes decoded). The relay constructs a 2-byte namespace field. The node sees a malformed payload and strikes the relay's UDS connection.
2. **Integer encoding errors.** Client sends `"ttl": 3600` as a JSON number. Relay writes it as a 4-byte native-endian integer instead of big-endian. The node interprets it as a completely different value on little-endian machines (which is every x86 machine).
3. **Missing fields silently zero-filled.** Client omits `"limit"` from a ListRequest. Relay writes 0 for the limit field. The node treats limit=0 as "use default 100", which might be correct -- or might mask a client bug.
4. **Overflow on large numbers.** Client sends `"ttl": 4294967296` (exceeds uint32). Relay truncates to uint32 without error. Client expects permanent storage but gets TTL=0 (which IS permanent, by coincidence) or gets garbage.

**Why it happens:**
The node's binary protocol has strict byte-level layout requirements. JSON is flexible and forgiving. The translation layer must bridge this gap with strict validation. Every field in every message type needs: type checking, range validation, size validation, endianness conversion, and hex decoding. Missing any of these for any field in any of the 38 message types is a bug. With 38 types averaging 4-5 fields each, that is ~170 validation points.

**How to avoid:**
1. **Table-driven translation.** Define each message type's JSON schema as a compile-time table: field name, JSON type, binary offset, binary size, encoding (hex/decimal/base64), range (min/max), required/optional. Write a single generic translator that processes the table. This prevents per-type copy-paste bugs.

2. **Strict validation with explicit errors.** Every JSON-to-binary translation returns either a valid binary payload or a structured error. The error includes which field failed and why. Return this to the client as a JSON error response. Never forward a malformed payload to the node.

3. **Round-trip tests for every message type.** For each of the 38 types: construct a JSON message, translate to binary, translate back to JSON, assert field equality. Add edge cases: maximum values, minimum values, empty optional fields, hex strings of wrong length, integer overflow.

4. **Use the existing PROTOCOL.md as the source of truth.** It documents byte-level layouts for all message types. The translation layer is a mechanical transcription of that document. Any divergence is a bug.

**Warning signs:**
- Per-type handler functions with copy-pasted encode/decode logic
- Missing validation for any field in any message type
- Tests that only cover 5-10 message types and miss the others
- Hex string handling that does not validate length
- Native-endian integer writes instead of explicit `store_u32_be` / `store_u64_be`

**Phase to address:**
Phase 2 (JSON schema + translation layer). The table-driven approach and round-trip tests must be part of the translation phase. This is the highest risk for subtle bugs.

---

### Pitfall 7: WebSocket close handshake vs. TCP close -- dangling async operations

**What goes wrong:**
Beast's WebSocket close requires a proper close handshake: send close frame, wait for peer's close frame, then shut down the TCP connection. If the relay just closes the TCP socket (like the node does for misbehaving peers), Beast's internal state may not be cleaned up, and pending async operations may complete with unexpected error codes. Specifically:

1. `async_read` pending when `async_close` is initiated returns `operation_aborted` (not `websocket::error::closed`). Code that checks for `closed` misses the `aborted` case and may try to use the stream after it is destroyed.
2. If the SSL shutdown is not performed before TCP close, some TLS implementations send RST instead of FIN, causing the client to see a connection error instead of a clean close.
3. Destruction of the `websocket::stream` while an async operation is pending is undefined behavior.

**Why it happens:**
The existing node connection uses raw TCP with AEAD framing. Close is simple: close the socket, drain the send queue, fire the close callback. WebSocket adds protocol-level close negotiation. SSL adds another layer (SSL_shutdown). The relay must perform a 3-layer shutdown sequence: WebSocket close frame -> SSL shutdown -> TCP close. Getting the ordering wrong or skipping a layer causes dangling operations.

**How to avoid:**
1. **Always initiate close via `async_close()` with a close reason code.** Never directly close the TCP socket for a WebSocket connection. The drain coroutine should detect the close state and call `async_close()` as its final action.

2. **Handle both `operation_aborted` and `websocket::error::closed` in read loop.** Both indicate the connection is ending. Treat them the same: stop reading, let the close handshake complete, then clean up.

3. **Timeout the close handshake.** If the client does not respond with its close frame within 5 seconds, force-close the TCP socket. A misbehaving client should not hold relay resources indefinitely.

4. **Ensure `shared_from_this()` prevents premature destruction.** The WebSocket session must be kept alive (via shared_ptr captured in async operation completions) until all pending operations have completed. This is the same pattern as the existing Connection class.

**Warning signs:**
- Direct `socket.close()` or `lowest_layer().close()` calls on WebSocket connections
- Missing `operation_aborted` handling in the read loop
- Close handshake without a timeout timer
- Session destructor that does not assert no pending operations

**Phase to address:**
Phase 1 (session lifecycle). Close handling must be correct from the beginning. Retrofitting proper close semantics onto a working session is error-prone.

---

### Pitfall 8: Challenge-response auth over WebSocket -- timing and state machine complexity

**What goes wrong:**
Relay v2 uses ML-DSA-87 challenge-response for client authentication over WebSocket (replacing the PQ KEM handshake). The auth flow is: (1) client connects WebSocket, (2) relay sends challenge (random nonce), (3) client signs nonce with ML-DSA-87, (4) relay verifies signature and extracts client pubkey. This happens over an already-TLS-encrypted WebSocket. Pitfalls:

1. **No auth timeout.** Client connects, relay sends challenge, client never responds. The WebSocket session sits authenticated=false forever, consuming relay resources. Without a timeout, this is a trivial DoS vector.
2. **Message routing before auth complete.** If the relay's message handler does not check authentication state, a client could send a Data message before completing the challenge-response. The relay would forward it to the node as if from the relay's own identity.
3. **Challenge replay.** If the relay uses a predictable or reused challenge nonce, a client could replay a previously captured signature. The nonce must be cryptographically random and single-use.
4. **ML-DSA-87 verification is expensive (~1ms).** During auth, the relay must verify the signature. If done on the event loop, it blocks all other clients. Must be offloaded to a thread pool.

**Why it happens:**
The existing relay uses the node's PQ handshake (KEM + AEAD) which has built-in timeout and authentication gating via the Connection class. Relay v2 builds its own auth on top of WebSocket, losing the Connection class's built-in protections. The auth state machine must be reimplemented carefully.

**How to avoid:**
1. **Auth timeout: 10 seconds from WebSocket connect to auth complete.** Start a timer on connection accept. If auth is not complete when the timer fires, close the WebSocket.
2. **Auth gate on message handler.** The first thing the message handler checks is `authenticated_`. If false and the message is not an auth response, close the connection.
3. **Cryptographic random nonce, 32 bytes, from `randombytes_buf()`.** Store per-session, discard after verification. No challenge reuse.
4. **Offload ML-DSA-87 verify to `asio::thread_pool`.** Use the same `co_await asio::post(pool)` pattern the node uses for crypto offload. The event loop must not block on signature verification.

**Warning signs:**
- Missing auth timeout timer
- Message handler that does not check `authenticated_`
- Challenge nonce generated with `std::mt19937` instead of `randombytes_buf()`
- ML-DSA-87 verify called inline without thread pool offload

**Phase to address:**
Phase 2 (authentication). The auth timeout and gate must be in the first iteration of the auth flow. Crypto offload should be wired up at the same time.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Per-type handler functions instead of table-driven translation | Faster to write first 5 types | Copy-paste bugs across 38 types, schema changes require 38 edits | Never -- table-driven from day one |
| Base64 for all binary fields including blob data | Uniform JSON format | 33% bandwidth overhead, 200ms+ parse time for large blobs, memory pressure | Only for fields <1 KB (hashes, keys). Never for blob data. |
| Single `ssl::context` mutated on SIGHUP | Simpler code | Undefined behavior from OpenSSL, potential crashes under load | Never |
| Shared mutable state between UDS reader and WebSocket writers | Avoids per-client queues | Data races, TSAN findings, intermittent crashes | Never -- per-client queues from day one |
| Skipping WebSocket close handshake | Faster disconnect | RST instead of FIN, client-side errors, leaked async operations | Only on relay shutdown (force-close all) |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Beast WebSocket + Asio SSL | Forgetting to disable `tcp_stream` timeouts after connection | Call `get_lowest_layer(ws).expires_never()` after upgrade |
| Beast WebSocket binary mode | Setting `ws.binary(true)` globally instead of per-message | Set binary mode before each write based on message type |
| nlohmann/json + uint64 | `json j = uint64_value` -- loses precision above 2^53 | `json j = std::to_string(uint64_value)` for fields >32 bits |
| OpenSSL + SIGHUP | Modifying active SSL_CTX | Create new SSL_CTX, atomic swap shared_ptr, old stays alive via refcount |
| UDS Connection + AEAD | Assuming UDS is plaintext -- it uses TrustedHello then AEAD | UDS messages are encrypted; reuse existing Connection class as-is |
| FlatBuffers + JSON | Assuming FlatBuffer encoding is the JSON payload | FlatBuffer is the transport encoding; JSON fields map to the semantic data inside the FlatBuffer |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Base64 encoding 100 MiB blobs | 133 MiB JSON strings, OOM, 200ms+ parse | Binary WebSocket frames for blob data | Any blob >1 MiB |
| ML-DSA-87 verify on event loop | All clients stall during auth | Thread pool offload | >10 concurrent auth attempts |
| Per-notification JSON serialization | CPU bound on high fan-out | Pre-serialize notification JSON, send to all subscribers | >100 subscribers to same namespace |
| Unbounded UDS read buffering | Memory grows linearly with slow client count | Per-client queue with disconnect-on-overflow | >50 clients with 1+ slow |
| nlohmann/json parse for every inbound message | CPU overhead per message | Parse once, validate schema, extract fields | >1000 msg/sec |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Forwarding messages before auth complete | Unauthenticated writes to node, impersonation | Auth gate as first check in message handler |
| Predictable challenge nonce | Auth replay attacks | `randombytes_buf()`, 32 bytes, single-use |
| No auth timeout | DoS via connection exhaustion | 10-second timer from accept to auth complete |
| Passing client-supplied request_id directly to node | Request ID collision between clients on multiplexed UDS | Relay assigns its own UDS request_ids, maintains mapping |
| TLS config without `SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1` | Downgrade attacks | Require TLS 1.2+ minimum, prefer 1.3 |
| No rate limiting on WebSocket message rate | DoS via message flood | Per-client message rate limit, disconnect on exceed |

## "Looks Done But Isn't" Checklist

- [ ] **JSON schema:** All 38 relay-allowed message types have documented JSON schemas with field names, types, and validation rules
- [ ] **Round-trip tests:** Every message type has JSON->binary->JSON round-trip test with edge cases (max values, empty optionals, wrong-length hex)
- [ ] **Binary frames:** ReadResponse, BatchReadResponse, Data payloads use binary WebSocket frames (not base64 in JSON)
- [ ] **uint64 as strings:** seq_num, timestamp, storage_used, total_blobs encoded as JSON strings, not numbers
- [ ] **UDS reconnect:** Subscription replay, pending request timeout, request_id generation counter all tested
- [ ] **Backpressure:** Per-client send queue overflow triggers disconnect, UDS reader never blocks on client writes
- [ ] **TLS reload:** SIGHUP creates new ssl::context, old connections unaffected, cert validation before swap
- [ ] **Close handshake:** WebSocket async_close with timeout, SSL shutdown, both operation_aborted and error::closed handled
- [ ] **Auth timeout:** 10-second timer from accept, auth gate on all message handlers
- [ ] **Crypto offload:** ML-DSA-87 verify uses thread pool, event loop never blocks on signature verification

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Concurrent async_write crash | LOW | Add send queue, fix in one session class |
| Base64 blob encoding (already shipped) | MEDIUM | Add binary frame support, deprecate base64 blob responses, update all clients |
| Multiplexer request_id collision | HIGH | Redesign mapping table, audit all forwarding paths |
| Missing auth gate (exploited) | HIGH | Emergency patch, audit logs for unauthorized writes, rotate relay identity |
| TLS cert mutation crash | MEDIUM | Switch to atomic shared_ptr swap, restart relay |
| Translation bugs in shipped types | MEDIUM per type | Fix translation, add round-trip test, re-verify against PROTOCOL.md |
| Backpressure missing (OOM in prod) | HIGH | Add per-client queues + overflow disconnect, requires session redesign |
| Close handshake leak | LOW | Add async_close + timeout, fix read loop error handling |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Beast async_write serialization | Phase 1: Session scaffolding | TSAN clean under concurrent sends; no assertions in debug build |
| Binary data in JSON | Phase 2: JSON schema design | Round-trip tests for all 38 types; binary frames for blob data verified |
| Multiplexed UDS failure | Phase 2-3: Multiplexer + reconnect | UDS disconnect test with 10+ pending requests; subscription replay verified |
| Backpressure inversion | Phase 1: Session architecture | Slow-client test: one client paused, others unaffected; OOM test with 100 slow clients |
| TLS cert reload | Phase 3: TLS termination | SIGHUP test with short-lived certs; existing connections survive reload |
| JSON-FlatBuffers translation | Phase 2: Translation layer | 38 round-trip tests + edge cases; PROTOCOL.md fidelity check |
| WebSocket close handshake | Phase 1: Session lifecycle | Graceful close test; abrupt client disconnect test; close timeout test |
| Auth state machine | Phase 2: Authentication | Auth timeout test; pre-auth message rejection test; challenge uniqueness test |

## Sources

- [Beast WebSocket async_write assertion failure (GitHub #1080)](https://github.com/boostorg/beast/issues/1080) -- confirmed one-write-at-a-time rule
- [Beast WebSocket concurrent writes (GitHub #1092)](https://github.com/boostorg/beast/issues/1092) -- strand does not serialize async_write
- [Beast WebSocket write queue pattern (GitHub #2153)](https://github.com/boostorg/beast/issues/2153) -- external queue with drain recommended
- [Beast WebSocket usage docs](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_websocket.html) -- stream not thread-safe, no non-blocking mode
- [Beast async_close behavior (GitHub #1071)](https://github.com/boostorg/beast/issues/1071) -- operation_aborted vs error::closed
- [OpenSSL SSL_CTX hot reload (GitHub discussion #23185)](https://github.com/openssl/openssl/discussions/23185) -- refcounted contexts, never mutate active CTX
- [nlohmann/json base64 performance (GitHub #3202)](https://github.com/nlohmann/json/discussions/3202) -- 200ms+ for ~1MB base64
- [nlohmann/json large binary data (GitHub #3132)](https://github.com/nlohmann/json/discussions/3132) -- slow serialization for big arrays
- [WebSocket backpressure analysis](https://dev.to/safal_bhandari/understanding-backpressure-in-web-socket-471m) -- hidden buffer accumulation
- [Base64 vs Hex encoding overhead](https://securebin.ai/blog/base64-vs-hex-encoding/) -- 33% vs 100% size overhead
- chromatindb codebase: `db/net/connection.cpp` drain_send_queue pattern (lines 854-898) -- proven send serialization
- chromatindb codebase: `relay/core/relay_session.cpp` reconnect_loop (lines 207-297) -- existing UDS reconnect pattern
- chromatindb codebase: `db/PROTOCOL.md` (lines 618-740) -- byte-level wire format for all client message types

---
*Pitfalls research for: WebSocket/JSON/TLS gateway relay (chromatindb Relay v2)*
*Researched: 2026-04-09*
