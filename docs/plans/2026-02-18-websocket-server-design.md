# WebSocket Server Design

> 2026-02-18

## Overview

The WebSocket server is the client-facing layer on top of the Kademlia engine.
Clients connect via WebSocket (JSON text frames) to store/fetch messages,
manage allowlists, send contact requests, and receive real-time push
notifications. The Kademlia layer handles all node-to-node replication,
responsibility assignment, and self-healing.

## Architecture

### Thread Model

```
Thread 1 (main) -- uWS event loop
  |-- Accepts WS connections on :4001
  |-- Handles all JSON commands
  |-- Sends push notifications (NEW_MESSAGE)
  |-- Timer: kademlia.tick() every 200ms
  |-- Signal handler: graceful shutdown

Thread 2 (background) -- TCP accept loop
  |-- Accepts TCP connections on :4000
  |-- Reads framed CHRM messages
  |-- Calls kademlia.handle_message()
  |-- On inbox STORE: on_store callback -> defer() -> push to Thread 1

Threads 3..N (worker pool) -- blocking operations
  |-- kademlia.store() calls (TCP sends to R nodes)
  |-- REDIRECT seq queries (TCP round-trips)
  |-- Results posted back via uWS::Loop::defer()
```

The uWS event loop is single-threaded and must never block. Any operation
involving TCP round-trips (STORE, SYNC_REQ) runs on the worker pool. The
worker posts results back to the uWS thread via `defer()`, which is
thread-safe.

### New Files

| File | Purpose |
|------|---------|
| `src/ws/ws_server.h` | WsServer class declaration |
| `src/ws/ws_server.cpp` | WsServer implementation |
| `src/ws/worker_pool.h` | Simple fixed-size thread pool with job queue |

### Modified Files

| File | Change |
|------|--------|
| `src/storage/storage.h` | Add `scan(table, prefix, callback)` method |
| `src/storage/storage.cpp` | Implement prefix scan using mdbx cursor |
| `src/kademlia/kademlia.h` | Add `set_on_store()` callback for push notifications |
| `src/kademlia/kademlia.cpp` | Call on_store callback after successful local store |
| `src/main.cpp` | Replace sleep loop with WsServer event loop |
| `src/CMakeLists.txt` | Add ws/ sources, link uWebSockets |

## Session Management

Each WebSocket connection has a Session:

```
struct Session {
    crypto::Hash fingerprint;
    std::vector<uint8_t> pubkey;
    bool authenticated = false;
    crypto::Hash challenge_nonce;
};
```

Sessions are stored in an `unordered_map<ws_t*, Session>` on the WsServer.
All session access happens on the uWS thread (no mutex needed). The uWS
`open` handler creates an empty session, `close` handler removes it.

## Authentication Flow

```
1. Client -> Node:   HELLO { fingerprint }

   Node checks is_responsible(SHA3-256("inbox:" || fingerprint)):
   - If NOT responsible: query R responsible nodes for their repl_log
     seq number for this inbox key (via worker pool). Respond with
     REDIRECT sorted by highest seq first. Close connection.
   - If responsible: generate 32-byte random nonce, store in session.

2. Node -> Client:   CHALLENGE { nonce }

3. Client -> Node:   AUTH { signature, pubkey }

   Node verifies:
   - SHA3-256(pubkey) == fingerprint (from HELLO)
   - ML-DSA-87 signature over the nonce is valid
   If valid: mark session authenticated.

4. Node -> Client:   OK { pending_messages }
```

### REDIRECT Response

When a node is not responsible for the client's inbox, it returns the
responsible nodes sorted by replication log sequence number (highest first).
The client connects to the most up-to-date node.

```json
{"type":"REDIRECT","id":1,"nodes":[
  {"address":"10.0.0.7","ws_port":4001,"seq":45},
  {"address":"10.0.0.9","ws_port":4001,"seq":45},
  {"address":"10.0.0.3","ws_port":4001,"seq":42}
]}
```

## Commands (MVP Scope)

All commands require an authenticated session. Every request includes an `id`
field echoed in the response.

### SEND

```json
{"type":"SEND","id":2,"to":"<recipient_fp>","blob":"<base64>"}
```

Processing (on worker thread):
1. Verify blob size <= 256 KiB
2. Generate msg_id (random 32 bytes), timestamp = server time
3. Build inbox message binary: `msg_id || sender_fp || timestamp || blob`
4. Compute `inbox_key = SHA3-256("inbox:" || recipient_fp)`
5. Call `kademlia.store(inbox_key, 0x02, message_binary)`
   - Responsible nodes check recipient's allowlist before accepting
   - Reject with STORE_ACK error if sender not on allowlist
6. On success (quorum met): defer SEND_ACK to uWS thread

```json
{"type":"SEND_ACK","id":2,"msg_id":"<hex>"}
```

On allowlist rejection: ERROR 403.

### FETCH

```json
{"type":"FETCH","id":7,"since":1708000000}
```

Processing (on uWS thread -- local storage only):
1. `storage.scan(TABLE_INBOXES, fingerprint_prefix)` using mdbx cursor
2. Composite key layout: `recipient_fp(32) || timestamp(8 BE) || msg_id(32)`
3. Skip entries where timestamp < `since` (if provided)
4. Collect and return messages

```json
{"type":"MESSAGES","id":7,"messages":[
  {"msg_id":"<hex>","from":"<fp>","blob":"<base64>","timestamp":1708000100},
  ...
]}
```

### ALLOW

```json
{"type":"ALLOW","id":4,"fingerprint":"<hex>","sequence":1,"signature":"<hex>"}
```

Processing (on worker thread):
1. Verify ML-DSA-87 signature (session's pubkey signs: `action(0x01) || allowed_fp || sequence`)
2. Verify sequence > currently stored sequence
3. Compute `allowlist_key = SHA3-256("allowlist:" || owner_fp)`
4. `kademlia.store(allowlist_key, 0x04, entry_binary)` -- replicates to R nodes
5. Respond OK

### REVOKE

Same as ALLOW but action = 0x00. Deletes the allowlist entry for the
specified fingerprint.

### CONTACT_REQUEST

```json
{"type":"CONTACT_REQUEST","id":6,"to":"<fp>","blob":"<base64>","pow_nonce":12345}
```

Processing (on worker thread):
1. Verify blob size <= 64 KiB
2. Verify PoW: `SHA3-256("request:" || sender_fp || recipient_fp || nonce)` has >= 16 leading zero bits
3. Build contact request binary: `sender_fp || pow_nonce || blob`
4. Compute `requests_key = SHA3-256("requests:" || recipient_fp)`
5. `kademlia.store(requests_key, 0x03, request_binary)`
6. Respond OK

No allowlist check -- PoW is the gatekeeper.

## Push Notifications

When a STORE arrives via Kademlia TCP (another node forwarded a message),
the WS layer pushes it to connected clients in real-time.

### Hookup

Kademlia gets a new callback:
```cpp
using StoreCallback = std::function<void(const crypto::Hash& key,
                                          uint8_t data_type,
                                          std::span<const uint8_t> value)>;
void set_on_store(StoreCallback cb);
```

Called after every successful local store in `handle_store()`.

### Flow

```
TCP thread: handle_store() stores inbox message
  -> on_store callback fires (TCP thread)
    -> uWS::Loop::defer([...] {
         WsServer checks: is recipient_fp in session map?
         If yes: send NEW_MESSAGE JSON on their WebSocket
       })
```

### Push Message (unsolicited, no id)

```json
{"type":"NEW_MESSAGE","msg_id":"<hex>","from":"<fp>","blob":"<base64>","timestamp":1708001000}
```

Contact requests also push:
```json
{"type":"CONTACT_REQUEST","from":"<fp>","blob":"<base64>"}
```

## Storage Changes

### New Method: scan()

```cpp
void scan(std::string_view table,
          std::span<const uint8_t> prefix,
          Callback cb) const;
```

Uses mdbx cursor: seek to first key >= prefix, iterate while key starts with
prefix, call `cb(key, value)` for each entry. Stop early if callback returns
false. This enables efficient inbox retrieval by fingerprint prefix.

### Allowlist Storage Layout

```
Table:  TABLE_ALLOWLISTS
Key:    allowlist_key(32) || allowed_fp(32)
Value:  allowlist entry binary (action, sequence, signature)
```

One entry per allowed contact. O(1) lookup for SEND validation:
`storage.get(TABLE_ALLOWLISTS, allowlist_key || sender_fp)`.

### Inbox Storage Layout

```
Table:  TABLE_INBOXES
Key:    recipient_fp(32) || timestamp(8 BE) || msg_id(32)
Value:  inbox message binary (msg_id, sender_fp, timestamp, blob)
```

Ordered by timestamp for FETCH. Prefix scan by recipient_fp.

## Worker Pool

Simple fixed-size thread pool:
- N threads (default: 4, configurable)
- `std::queue<std::function<void()>>` protected by mutex + condition variable
- `post(job)` enqueues work
- Workers pick jobs and execute
- Shutdown: poison pill pattern (post N empty jobs)

Used for: SEND, ALLOW, REVOKE, CONTACT_REQUEST, REDIRECT seq queries.

NOT used for: FETCH (local storage only, non-blocking), push notifications
(defer from TCP thread, non-blocking).

## Error Handling

All errors include the request `id`:

```json
{"type":"ERROR","id":1,"code":403,"reason":"not on allowlist"}
```

| Code | Meaning |
|------|---------|
| 400  | Malformed request (bad JSON, missing fields, unknown type) |
| 401  | Not authenticated (command before AUTH) |
| 403  | Forbidden (bad signature, not on allowlist) |
| 413  | Payload too large |
| 500  | Internal error |

### Responsibility Changes

On each incoming command, re-check `is_responsible()`. If no longer
responsible, respond with REDIRECT and close. Handles network topology
changes gracefully.

### Allowlist Race Condition

Server does best-effort allowlist enforcement. If a REVOKE is propagating
and a message sneaks through, the client filters it locally. The client
maintains its own authoritative allowlist and discards messages from revoked
contacts.

## main.cpp Integration

```cpp
// Current: while(running) { kademlia.tick(); sleep(200ms); }
// New:

WsServer ws(cfg, kademlia, storage, repl_log, keypair);
kademlia.set_on_store([&](auto& key, auto type, auto value) {
    ws.on_kademlia_store(key, type, value);  // defer() inside
});
ws.run();  // blocks on uWS event loop, tick() via timer

// After ws.run() returns (signal caught):
transport.stop();
recv_thread.join();
```

## Shutdown Sequence

1. SIGINT/SIGTERM caught
2. Close uWS listen socket (stop accepting new WS connections)
3. Close all active WS sessions
4. uWS event loop exits, `ws.run()` returns
5. Worker pool shutdown (drain queue, join threads)
6. `transport.stop()` -> TCP accept loop exits
7. Join TCP thread
8. Destructors clean up Storage, Kademlia, etc.

## Deferred to Post-MVP

- **Push notification gaps**: cross-node push when message stored on non-connected responsible node
- **Node failover**: automatic client reconnection when responsible node goes down
- **DELETE command**: client-driven message deletion (messages auto-expire after 7 days)
- **Rate limiting**: per-session command rate limiting (ERROR 429)
- **Connection pooling**: reuse TCP connections between nodes instead of connect-per-message
