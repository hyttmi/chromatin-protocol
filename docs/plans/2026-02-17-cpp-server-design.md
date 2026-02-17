# Helix C++ Server — Implementation Design

> Approved 2026-02-17. Layer-by-layer implementation of dna-helix server.

## Decisions

- **Language:** C++20
- **Build:** CMake + FetchContent
- **Deps:** liboqs, libmdbx, uWebSockets, jsoncpp, spdlog, GoogleTest
- **Scope:** Server binary only (helix-node). libdna (client library) is a separate project.
- **Approach:** Layer-by-layer, bottom-up, each layer independently testable

## Project Structure

```
dna-helix/
├── CMakeLists.txt
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── crypto/
│   │   ├── crypto.h
│   │   └── crypto.cpp
│   ├── storage/
│   │   ├── storage.h
│   │   └── storage.cpp
│   ├── kademlia/
│   │   ├── node_id.h / .cpp
│   │   ├── routing_table.h / .cpp
│   │   ├── kademlia.h / .cpp
│   │   └── udp_transport.h / .cpp
│   ├── replication/
│   │   ├── repl_log.h
│   │   └── repl_log.cpp
│   └── server/
│       ├── ws_server.h
│       └── ws_server.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── test_crypto.cpp
│   ├── test_storage.cpp
│   ├── test_node_id.cpp
│   ├── test_routing_table.cpp
│   ├── test_kademlia.cpp
│   └── test_repl_log.cpp
├── protocol.md
└── docs/
```

## Layer 1: Crypto

Thin wrappers around liboqs. No state, pure functions.

- `sha3_256(data)` — hash
- `sha3_256_prefixed(prefix, data)` — domain-separated hash ("dna:", "name:", "inbox:", etc.)
- `generate_keypair()` — ML-DSA-1024 keypair
- `sign(message, secret_key)` — ML-DSA sign
- `verify(message, signature, public_key)` — ML-DSA verify
- `verify_pow(preimage, nonce, required_zero_bits)` — PoW check

Server does NOT use ML-KEM (that's client-side encryption).

## Layer 2: Storage

libmdbx wrapper. One named DBI per table.

- Tables: `profiles`, `names`, `inboxes`, `requests`, `allowlists`, `repl_log`, `nodes`, `reputation`
- Operations: `put`, `get`, `del`, `foreach`
- Transactions handled internally per operation

## Layer 3: Kademlia Engine

Three components:

**NodeId** — 256-bit ID from SHA3-256(pubkey), XOR distance.

**RoutingTable** — Full membership table (not k-buckets, small network). Core operation: `closest_to(key, count)` returns R closest nodes.

**UdpTransport** — Send/recv serialized messages. Types: PING, PONG, FIND_NODE, NODES, STORE, FIND_VALUE, VALUE, SYNC_REQ, SYNC_RESP. All messages ML-DSA signed.

**Kademlia** — Ties it together. Bootstrap, message dispatch, store/find_value, responsibility computation. R = min(3, network_size - 1).

## Layer 4: Replication

Seq-based replication log on top of storage.

- Composite mdbx key: `key || seq_number`
- Operations: ADD, DEL, UPD
- `append(key, op, data)` — local mutation
- `entries_after(key, seq)` — for SYNC_REQ
- `apply(key, entries)` — from SYNC_RESP, idempotent
- `compact(key, before_seq)` — prune old entries

## Layer 5: WebSocket Server

Client-facing. uWebSockets event loop.

- 4-step auth: HELLO → CHALLENGE → AUTH → OK
- Commands: SEND, ACK, ALLOW, REVOKE, CONTACT_REQUEST, FETCH_PENDING
- Push: NEW_MESSAGE, CONTACT_REQUEST (to connected clients)
- SEND checks allowlist, forwards to R responsible nodes via UDP STORE
- ACK triggers DEL in replication log

## Layer 6: main.cpp

Wires everything. Single process, two threads (UDP recv + WS event loop).

1. Parse config
2. Generate or load node keypair
3. Init storage
4. Init routing table + UDP transport + Kademlia
5. Init replication log
6. Bootstrap (get membership)
7. Start UDP loop (background thread)
8. Start WebSocket server (main thread)
