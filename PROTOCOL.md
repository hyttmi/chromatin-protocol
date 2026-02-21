# Chromatin Protocol — Server Design

> Working document — v2 redesign. Server-only scope.
> Client protocol (message encryption, GEK, message format) deferred.

---

## 1. Overview

Chromatin is a decentralized, post-quantum-safe messaging node. It uses a
**unified Kademlia-routed, mdbx-backed architecture**: XOR distance determines
which nodes are responsible for storing which data (profiles, names, and
message inboxes), and all data is persisted in libmdbx with sequence-based
replication between responsible nodes.

There is no separate "relay layer" — every node stores and serves the data
it's responsible for. Clients connect via WebSocket to any responsible node
for their inbox.

This document covers the **server/node design only**. The server treats
messages as opaque encrypted blobs — it never sees plaintext.

**Design principles:**
- Zero classical cryptography — PQ only, everywhere
- Unified storage — Kademlia responsibility for ALL data (profiles, names, inboxes)
- Sequence-based replication — mdbx-backed, crash-recoverable
- 7-day TTL message expiry — automatic cleanup, multi-device friendly
- Recipient inbox model — one connection, all messages
- Allowlist-enforced delivery — only approved contacts can message you
- Censorship resistant — anyone can run a node, network is unstoppable
- Simple — small network (dozens to hundreds of nodes), not millions

---

## 2. Cryptographic Primitives (Server-Side)

| Purpose              | Algorithm              | Notes                                    |
|----------------------|------------------------|------------------------------------------|
| Node identity        | ML-DSA-87 (FIPS 204, Level 5) | Node signing, signature verification   |
| Hashing              | SHA3-256               | Node IDs, data keys, PoW verification    |
| TCP key exchange     | ML-KEM-1024 (FIPS 203) | Ephemeral per-connection key encapsulation |
| TCP encryption       | ChaCha20-Poly1305 (libsodium) | AEAD symmetric encryption for TCP transport |
| Node-to-node         | TCP (encrypted)        | Kademlia protocol + replication            |
| Client-to-node       | WebSocket              | Inbox delivery, auth, commands           |

The server verifies ML-DSA signatures on profiles, name records, and client
authentication. It does **not** encrypt/decrypt messages — that is the client's
responsibility. All TCP connections between nodes are encrypted using
ML-KEM-1024 key exchange and ChaCha20-Poly1305 AEAD (via libsodium).

---

## 3. Identity (Server Perspective)

The server stores and serves user profiles. It does not create them.

### 3.1 Profile Data

A profile is a signed, versioned document. The server stores and validates:

| Field            | Type                         | Notes                              |
|------------------|------------------------------|------------------------------------|
| fingerprint      | 32 bytes                     | SHA3-256(ml_dsa_pubkey)            |
| ml_dsa_pubkey    | bytes                        | Public signing key                 |
| ml_kem_pubkey    | bytes                        | Public encryption key              |
| bio              | string                       | Free text                          |
| avatar           | blob                         | Profile image                      |
| social_links     | list of {platform, handle}   | Social media addresses             |
| sequence         | uint64                       | Monotonically increasing version   |
| signature        | bytes                        | ML-DSA signature over all above    |

Note: no `relays` field. The network determines which nodes hold a user's
inbox via XOR distance — users don't choose their relay.

### 3.2 Storage Key

```
profile_key = SHA3-256("profile:" || fingerprint)
```

Stored on the R closest nodes to `profile_key`. Persistent (no expiry).

### 3.3 Server Validation Rules

On profile write:
1. `fingerprint == SHA3-256(ml_dsa_pubkey)` — fingerprint matches key
2. ML-DSA signature is valid over all fields
3. `sequence` is higher than currently stored version (replay prevention)

Only the key owner can update their profile.

---

## 4. Name Registration

Human-readable names mapped to fingerprints, stored on the network.

### 4.1 Name Record

```
{
    name:        string,       // 3-36 chars, [a-z0-9]
    fingerprint: 32 bytes,
    pow_nonce:   uint64,
    sequence:    uint64,
    signature:   bytes         // ML-DSA over all above
}
```

Storage key: `SHA3-256("name:" || name)`

### 4.2 Server Validation Rules

1. Verify PoW: `SHA3-256("chromatin:name:" || name || fingerprint || nonce)`
   has >= 28 leading zero bits
2. Verify ML-DSA signature
3. **First claim wins** — reject if name already registered to a different
   fingerprint
4. Owner can update (higher sequence + same fingerprint)

### 4.3 Properties

- Permanent — no expiry
- Free — PoW only (~1-2 minutes on modern CPU)
- Verification is instant (single hash check)

---

## 5. Unified Architecture

### 5.1 Single Responsibility Model

Every piece of data in the network — profiles, names, and message inboxes —
is assigned to nodes using the **same Kademlia XOR distance** mechanism and
replicated using the **same sequence-based mdbx replication**.

```
profile_key = SHA3-256("profile:" || fingerprint)     → R closest nodes store it
name_key    = SHA3-256("name:" || name)            → R closest nodes store it
inbox_key   = SHA3-256("inbox:" || fingerprint)    → R closest nodes store it
request_key = SHA3-256("requests:" || fingerprint) → R closest nodes store it
group_key   = SHA3-256("group:" || group_id)       → R closest nodes store it
```

No separate "DHT layer" vs "relay layer". One node, one storage engine, one
replication strategy.

### 5.2 Node Diagram

```
┌──────────────────────────────────────────────────┐
│                   CHROMATIN NODE                      │
│                                                   │
│  ┌──────────────────────────────────────────────┐ │
│  │           Kademlia Engine (TCP)               │ │
│  │                                               │ │
│  │  - Node discovery & gossip                    │ │
│  │  - XOR responsibility computation             │ │
│  │  - STORE / FIND_VALUE / replication           │ │
│  └──────────────────┬───────────────────────────┘ │
│                     │                             │
│  ┌──────────────────▼───────────────────────────┐ │
│  │           libmdbx (unified storage)           │ │
│  │                                               │ │
│  │  profiles/   — profiles I'm responsible   │ │
│  │  names/      — Name records I'm responsible   │ │
│  │  inboxes/    — Message inboxes I'm resp. for  │ │
│  │  requests/   — Contact request inboxes        │ │
│  │  allowlists/ — Allowlist per user fingerprint │ │
│  │  nodes/      — Full membership table          │ │
│  │  reputation/ — Trust scores per node          │ │
│  │  repl_log/   — Replication sequence log       │ │
│  └──────────────────▲───────────────────────────┘ │
│                     │                             │
│  ┌──────────────────┴───────────────────────────┐ │
│  │        WebSocket Server (uWebSockets)         │ │
│  │                                               │ │
│  │  - Client auth (ML-DSA challenge-response)    │ │
│  │  - Inbox delivery & real-time push            │ │
│  │  - SEND / ACK / ALLOW / REVOKE commands       │ │
│  │  - Contact request handling                   │ │
│  └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
        │ TCP                        │ WebSocket
        ▼                            ▼
  Other Nodes                   Clients
```

Single binary. One process, two listeners (TCP + WebSocket), one storage engine.
All TCP connections use ML-KEM-1024 + ChaCha20-Poly1305 encryption
(see Section 6.5).

---

## 6. Kademlia Engine

### 6.1 Simplified Kademlia

Chromatin targets dozens to hundreds of nodes. Every node maintains **full
membership** — no k-buckets, no iterative lookups. The routing table is
capped at **256 nodes** with LRU eviction (oldest `last_seen` evicted when full).

- 256-bit key space (SHA3-256 output)
- Node ID: `SHA3-256(node_ml_dsa_pubkey)`
- Distance: `XOR(id_a, id_b)` as unsigned 256-bit integer
- XOR distance determines **responsibility**, not routing
- All communication is **single-hop** (direct node-to-node TCP)
- TCP connections are pooled per destination (max 64, 60s idle timeout)

### 6.2 Responsibility

For a given key K, the R nodes with the smallest `XOR(node_id, K)` are
responsible for storing data associated with K.

- R = replication factor = `min(3, network_size)`
- Any node can compute responsibility (full membership knowledge)
- Applies uniformly to profiles, names, AND inboxes

### 6.3 Trust Boundary

Kademlia nodes enforce data integrity at the STORE level — not just the WS
server. This is critical because any node in the network can send STORE
messages directly via TCP:

- **Inbox messages**: Kademlia validates the recipient's allowlist before
  accepting. If the recipient has an allowlist and the sender is not on it,
  the STORE is rejected. No allowlist = open inbox (new user).
- **Contact requests**: Kademlia verifies the PoW (16 leading zero bits)
  and that the routing key matches the recipient_fp in the value.
- **Allowlist entries**: Kademlia verifies the ML-DSA-87 signature against
  the owner's public key (from TABLE_PROFILES). If the owner's profile is
  not stored yet, the entry is **rejected** — profile must propagate first.
- **Name records**: Same as allowlist — rejected if owner profile is absent.
- **SYNC_RESP validation**: All entries received via SYNC_RESP are validated
  using the same rules as direct STORE before being applied to storage.
- **Signature verification**: PING and FIND_NODE are accepted without
  signature verification (discovery). All other messages — including PONG,
  NODES, STORE, FIND_VALUE, SYNC_REQ, SYNC_RESP, STORE_ACK, SEQ_REQ,
  SEQ_RESP — MUST have valid ML-DSA-87 signatures. Messages from nodes
  whose public key is not yet known are rejected.
- **Pubkey propagation**: Public keys are learned via two mechanisms:
  (1) FIND_NODE includes the sender's pubkey in its payload, verified via
  `SHA3-256(pubkey) == sender_id`; (2) NODES responses include each node's
  pubkey, verified via `node_id == SHA3-256(pubkey)`.
- **Iterative discovery**: When a NODES response contains new nodes, the
  receiver sends FIND_NODE to each new node. This propagates pubkeys
  bidirectionally and is standard Kademlia iterative lookup behavior.
- **NODES node_id verification**: When processing NODES responses, the
  receiver verifies `node_id == SHA3-256(pubkey)` for each entry. Entries
  failing this check are silently dropped.

This dual-layer validation (Kademlia + WS server) ensures a single malicious
node cannot bypass protections by sending STORE messages directly.

### 6.4 Kademlia Messages (TCP)

| Message      | Purpose                                      |
|--------------|----------------------------------------------|
| PING         | Liveness check                               |
| PONG         | Liveness response                            |
| FIND_NODE    | Request K closest nodes (carries sender pubkey) |
| NODES        | Response with node list (IPv4 and IPv6)      |
| STORE        | Write a signed value to responsible node     |
| FIND_VALUE   | Request a value by key                       |
| VALUE        | Response with stored value                   |
| SYNC_REQ     | Request replication log entries after seq N   |
| SYNC_RESP    | Response with replication log entries         |
| STORE_ACK    | Acknowledgment of a STORE request            |
| SEQ_REQ      | Query replication log sequence for a key     |
| SEQ_RESP     | Replication log sequence response            |

All messages are ML-DSA signed by the sending node. PING and FIND_NODE are
accepted without signature verification (needed for initial discovery).
FIND_NODE carries the sender's pubkey so recipients can immediately verify
future signed messages. All other types — including PONG, NODES, and all
trust-sensitive messages — MUST have valid signatures. Messages from nodes
whose public key is not yet known are rejected (except PING and FIND_NODE).

### 6.5 TCP Transport Encryption

All TCP connections between nodes are encrypted using ML-KEM-1024 key
exchange and ChaCha20-Poly1305 AEAD (via libsodium). This protects
Kademlia messages from eavesdropping and tampering on the wire.

**Handshake:** A 3-message handshake (HELLO, ACCEPT, CONFIRM) establishes
an encrypted session:

1. **HELLO**: Initiator sends a `0xCE` probe byte, an ephemeral ML-KEM-1024
   public key, and a random nonce.
2. **ACCEPT**: Responder encapsulates a shared secret using the ephemeral key,
   signs the handshake transcript with ML-DSA-87, and returns a random nonce.
3. **CONFIRM**: Initiator signs the handshake transcript with ML-DSA-87,
   completing mutual authentication.

**Key derivation:** Two directional session keys are derived from the shared
secret using SHA3-256 with domain-separated prefixes:
- `i2r_key = SHA3-256("chromatin:tcp:i2r:" || ss || hello_random || accept_random)`
- `r2i_key = SHA3-256("chromatin:tcp:r2i:" || ss || hello_random || accept_random)`

All subsequent CHRM messages are encrypted with ChaCha20-Poly1305 using
per-direction nonce counters and length-header AAD.

**Encryption is mandatory.** The `0xCE` probe byte identifies encrypted
connections. Plaintext connections (starting with `0x43`) are rejected.
If the handshake fails, the connection is closed and the message is dropped.

**Security properties:**
- Mutual ML-DSA-87 authentication (both sides prove identity)
- Forward secrecy via ephemeral ML-KEM-1024 keys (per-connection)
- Post-quantum security (ML-KEM-1024 FIPS 203 + ML-DSA-87 FIPS 204)
- Replay protection via random nonces and per-message counters
- Direction separation prevents reflection attacks

See PROTOCOL-SPEC.md Section 2 for the complete wire format specification.

### 6.6 Self-Healing & Periodic Maintenance

Nodes run a periodic `tick()` (~200ms) that keeps the network self-healing:

**Re-bootstrap (refresh):**
- Every 30s (or 5s if routing table has < 3 nodes), the node sends FIND_NODE
  to its configured bootstrap peers AND all known nodes
- This ensures late-joining nodes are discovered, and nodes that started
  before bootstraps were ready will eventually connect
- Iterative: each round discovers new peers that were learned by existing ones

**Stale node detection:**
- Every 10s, nodes not seen for > 60s are PINGed
- PONG response updates `last_seen` in the routing table
- PING also updates `last_seen` for the sender (bidirectional liveness)

**Dead node eviction:**
- Every 10s, nodes not seen for > 120s are evicted from the routing table
- This prevents stale entries from accumulating

**TTL expiry:**
- Every 5 minutes, scans inbox and contact request tables
- Deletes inbox entries older than 7 days (MESSAGE_TTL)
- Deletes contact requests older than 7 days
- Removes inbox data from both TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS

**Pending store cleanup:**
- Pending STORE tracking entries (waiting for STORE_ACK) are cleaned up
  after 30 seconds if ACKs never arrive

**Responsibility transfer:**
- Every 60 seconds (when routing table size has changed), scans all data tables
  (profiles, names, contact requests, allowlists, AND inbox data)
- For each key, checks if there are new responsible nodes
- Pushes data via STORE to responsible nodes that don't have it yet

**Active sync:**
- Every 120 seconds, checks responsible keys against peer nodes
- Sends SEQ_REQ to peers, pulls missing entries via SYNC_REQ if behind
- Ensures eventual consistency when direct STORE delivery fails

**Replication log compaction:**
- Every 1 hour, compacts the replication log
- Retains the most recent 10,000 entries per key, deletes older ones
- Bounds storage growth while preserving enough sync history

**Constants:**

| Parameter              | Value  | Purpose                           |
|------------------------|--------|-----------------------------------|
| REFRESH_INTERVAL       | 30s    | Normal re-bootstrap interval      |
| REFRESH_INTERVAL_SPARSE| 5s     | When routing table has < 3 nodes  |
| PING_SWEEP_INTERVAL    | 10s    | How often to check for stale nodes|
| STALE_THRESHOLD        | 60s    | Ping nodes not seen for this long |
| EVICT_THRESHOLD        | 120s   | Remove nodes not seen this long   |
| TTL_SWEEP_INTERVAL     | 5m     | How often to check for expired data|
| MESSAGE_TTL            | 7 days | Inbox/contact request expiry      |
| PENDING_STORE_TIMEOUT  | 30s    | Timeout for STORE_ACK tracking    |
| TRANSFER_CHECK_INTERVAL| 60s    | Responsibility transfer check     |
| SYNC_INTERVAL          | 120s   | Active sync with responsible peers |
| COMPACT_INTERVAL       | 1h     | Replication log compaction        |
| COMPACT_KEEP_ENTRIES   | 10000  | Max entries per key after compact  |
| MAX_ROUTING_TABLE_SIZE | 256    | Maximum nodes in routing table    |
| CONN_MAX_IDLE          | 60s    | TCP connection pool idle timeout  |
| MAX_POOL_SIZE          | 64     | TCP connection pool max size      |

**Bootstrap node resilience:**
- If a bootstrap node goes down and comes back at the same address, existing
  nodes re-discover it automatically via periodic refresh
- If a bootstrap node is permanently replaced (new IP/identity), existing
  nodes in the network are unaffected — they maintain connections via the
  surviving bootstrap(s) and direct peer-to-peer discovery
- New nodes joining need at least one working bootstrap address in their config
- Future: DNS-based bootstrap (`bootstrap.cpunk.io` resolving to current IPs)
  would allow transparent bootstrap rotation without config changes

---

## 7. Sequence-Based Replication

### 7.1 Replication Log

Every data mutation on a node is recorded in a per-key **replication log**
stored in libmdbx. Each entry has a monotonically increasing sequence number.

```
Key: inbox:alice

  seq=1  ADD  { msg_id: "abc", blob: <encrypted>, ts: 1708000000 }
  seq=2  ADD  { msg_id: "def", blob: <encrypted>, ts: 1708000060 }
  seq=3  DEL  { msg_id: "abc" }    ← TTL expired or client DELETE
  seq=4  ADD  { msg_id: "ghi", blob: <encrypted>, ts: 1708000120 }
```

Operations: `ADD` (new message/profile/name), `DEL` (delete after ACK),
`UPD` (profile/name update with higher sequence).

### 7.2 Sync Protocol

Responsible nodes periodically sync with each other:

```
Node A (seq 5) ──SYNC_REQ { key, after_seq: 5 }──► Node B (seq 7)
Node A ◄──────SYNC_RESP { entries: [seq=6, seq=7] }─── Node B
Node A applies entries → now at seq 7
```

- Nodes pull from the peer with the **highest sequence** for a given key
- Lightweight: only transmits the delta (entries after N)
- Idempotent: replaying an entry that already exists is a no-op

### 7.3 Node Recovery

When a crashed node comes back, or a new node becomes responsible for a key
(due to network membership changes):

1. Node contacts the other R-1 responsible nodes for each key it owns
2. Requests their current sequence number for each key
3. Syncs from the node with the highest sequence
4. Once caught up, begins serving requests normally

### 7.4 Compaction

The replication log grows over time. To bound storage:

- Compaction runs every **1 hour** in the background
- For each key, retains the most recent **100 entries**, deletes older ones
- This is a simple, conservative policy that ensures sync catch-up works
  while bounding storage growth
- For inboxes: 7-day TTL on messages provides an additional natural bound
- For profiles/names: log is small (infrequent updates), compaction less critical

### 7.5 Write Flow

When a client stores data (e.g., sends a message to an inbox):

1. Client (or forwarding node) sends STORE to **all R responsible nodes**
2. Each node appends to its local replication log, increments seq
3. Write is confirmed when **W of R** nodes ACK (W = quorum, e.g. 2 of 3)
4. Background sync ensures the remaining nodes catch up

### 7.6 Message Expiry & Deletion

Messages expire automatically after 7 days (TTL). The node's periodic `tick()`
prunes expired messages from storage. This supports **multi-device**: all
devices can fetch messages independently within the 7-day window.

Clients may optionally send `DELETE` for messages they no longer need:

1. The node receiving the DELETE removes from TABLE_INBOX_INDEX and
   TABLE_MESSAGE_BLOBS locally
2. Writes a `DEL` entry to the replication log (seq increments)
3. Replicates the deletion via Kademlia STORE to other responsible nodes
4. Other responsible nodes sync the DEL entry via SYNC_REQ/SYNC_RESP
5. All R copies are deleted

Deletion is client-driven and optional. Each device tracks its own
`last_fetch_timestamp` locally for efficient incremental fetches.

---

## 8. WebSocket Server — Client Interface

### 8.1 Connection Model

Nodes support optional **TLS** (WSS) to protect connection metadata from network
observers. When `tls_cert_path` and `tls_key_path` are configured, the node
serves WebSocket over TLS on the same port. Clients SHOULD prefer `wss://`
connections.

A client connects to **any of the R nodes responsible for their inbox**.
**Multiple devices** can connect simultaneously with the same identity — push
notifications (NEW_MESSAGE, CONTACT_REQUEST) are delivered to all connected
devices for that fingerprint.

```
inbox_key = SHA3-256("inbox:" || fingerprint)
responsible_nodes = R closest nodes to inbox_key
client connects to any one of them via WebSocket
```

The client can discover responsible nodes by querying any node in the network
(since all nodes know full membership). If the node is not responsible, it
responds with **REDIRECT** — a list of responsible nodes sorted by replication
log sequence number (highest first). The client reconnects to the most
up-to-date node.

### 8.2 Client Authentication

```
1. Client → Node:   HELLO { fingerprint }

   Node checks is_responsible(SHA3-256("inbox:" || fingerprint)):
   - If NOT responsible: query R responsible nodes for their seq number,
     respond with REDIRECT sorted by highest seq first, close connection.
   - If responsible: generate 32-byte random nonce, continue.

2. Node   → Client: CHALLENGE { nonce }
3. Client → Node:   AUTH { ML-DSA-sign("chromatin-auth:" || nonce), pubkey }
4. Node   → Client: OK { pending_message_count }
```

Node verifies: `fingerprint == SHA3-256(pubkey)` and signature over
`"chromatin-auth:" || nonce` (47 bytes) is valid. The domain prefix prevents
cross-protocol signature replay.

### 8.3 WebSocket Messages

Control messages use JSON text frames. Large blob transfers (>64 KB) use binary
WebSocket frames with 1 MiB chunked transfer (see PROTOCOL-SPEC.md Section 5.7).

**Client → Node:**

| Message           | Purpose                                      |
|-------------------|----------------------------------------------|
| SEND              | Push encrypted blob to recipient's inbox     |
| LIST              | Retrieve message index (paginated, small blobs inlined) |
| GET               | Fetch a specific large blob by msg_id        |
| DELETE            | Optionally delete messages (client-driven, replicated) |
| ALLOW             | Add fingerprint to allowlist (client-signed)  |
| REVOKE            | Remove fingerprint from allowlist (client-signed)|
| CONTACT_REQUEST   | Send request to non-contact (PoW + timestamp) |
| RESOLVE_NAME      | Look up fingerprint by username              |
| GET_PROFILE       | Fetch a user's profile by fingerprint        |
| LIST_REQUESTS     | List pending contact requests                |
| SET_PROFILE       | Publish/update signed profile via Kademlia   |
| REGISTER_NAME     | Register a permanent name record via Kademlia |
| STATUS            | Health check (no auth required)              |

**Node → Client:**

| Message           | Purpose                                      |
|-------------------|----------------------------------------------|
| NEW_MESSAGE       | Incoming message (inline <=64KB, else metadata-only) |
| CONTACT_REQUEST   | Incoming contact request (PoW-verified)      |
| SEND_ACK          | Confirmation that message was stored         |
| SEND_READY        | Ready for chunked upload (large SEND)        |
| LIST_RESULT       | Message index response (paginated)           |
| GET_RESULT        | Blob response (inline or chunked)            |
| RESOLVE_NAME_RESULT | Name lookup response                       |
| PROFILE_RESULT    | Profile lookup response                      |
| LIST_REQUESTS_RESULT | Contact requests list response             |
| SET_PROFILE_ACK   | Profile stored confirmation                  |
| REGISTER_NAME_ACK | Name registered confirmation                 |
| STATUS_RESP       | Node health/status information               |
| REDIRECT          | List of responsible nodes (sorted by seq)    |
| ERROR             | Rejection with reason and error code         |

### 8.4 Message Send Flow

When Alice sends a message to Bob:

**Small message (<=64 KB):**
1. Alice sends `SEND { to: bob_fp, blob }` (inline base64) to her connected node
2. Alice's node computes `inbox_key = SHA3-256("inbox:" || bob_fp)`
3. Alice's node determines R responsible nodes for Bob's inbox
4. Alice's node stores in TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS locally
5. Alice's node forwards via TCP STORE to **all R responsible nodes**
6. Responsible nodes check Bob's allowlist — reject if Alice not allowed
7. If Bob is connected, push `NEW_MESSAGE` with blob inlined
8. Alice receives `SEND_ACK`

**Large message (>64 KB, up to 50 MiB):**
1. Alice sends `SEND { to: bob_fp, size }` (no blob, declares size)
2. Node responds `SEND_READY { request_id }` (or ERROR 413 if too large)
3. Alice uploads blob as binary 1 MiB chunks (UPLOAD_CHUNK frames)
4. On completion, node stores in both tables and replicates via TCP STORE
5. If Bob is connected, push `NEW_MESSAGE` with `blob: null` (metadata only)
6. Alice receives `SEND_ACK`
7. Bob fetches the blob later with `GET { msg_id }`

Incomplete chunked uploads are discarded after 30 seconds.

### 8.5 Allowlist

Each user has an allowlist stored on their responsible nodes.

- Managed via `ALLOW` / `REVOKE` commands (signed by the client)
- The client signs `"chromatin:allowlist:" || owner_fingerprint || action || allowed_fingerprint || sequence` with ML-DSA-87
- The node verifies the signature — only the inbox owner can modify their allowlist
- Kademlia STORE value includes `owner_fp` for signature verification at the DHT layer
- Stored in mdbx on all R responsible nodes (replicated like everything else)
- Allowlist key: `SHA3-256("allowlist:" || fingerprint)`
- Local mdbx key: `allowlist_key(32) || allowed_fp(32)` — O(1) lookup for SEND validation
- REVOKE is replicated as `Op::DEL` in the replication log (not `Op::ADD`)
- Replicated with the same seq-based mechanism
- Race condition: server does best-effort enforcement; client maintains authoritative
  allowlist locally and discards messages from revoked contacts

### 8.6 Rate Limiting

Per-connection token bucket rate limiting prevents abuse. Each connection
starts with 50 tokens and refills at 10 tokens/second. Commands consume
tokens at different rates (SEND: 2, CONTACT_REQUEST: 3, most others: 1).
Exceeding the rate limit returns error code 429.

---

## 9. Contact Requests

### 9.1 Request Inbox

Separate from the main inbox. Stored under:

```
request_key = SHA3-256("requests:" || fingerprint)
```

Same R-node responsibility, same replication. Accepts messages from unknown
senders with proof-of-work.

### 9.2 Flow

1. Alice looks up Bob's profile on network → gets Bob's fingerprint
2. Alice computes Bob's responsible nodes: R closest to
   `SHA3-256("requests:" || bob_fp)`
3. Alice computes PoW: find `nonce` such that
   `SHA3-256("chromatin:request:" || alice_fp || bob_fp || timestamp_BE || nonce_BE)` has M leading zero bits
4. Alice sends `CONTACT_REQUEST { to: bob_fp, blob, pow_nonce, timestamp }` to a
   responsible node. Timestamp must be within 1 hour of server time.
5. Node verifies PoW, stores in Bob's request inbox (replication log)
6. Bob receives `CONTACT_REQUEST` when connected
7. On accept: both sides send `ALLOW` to their responsible nodes

### 9.3 Request PoW

- Lighter than name registration (~seconds, not minutes)
- Prevents request spam
- Node verifies before storing

---

## 10. Group Messaging

### 10.1 Overview

Chromatin supports group messaging for up to 512 members per group. The design
follows a **fan-out model**: the sender encrypts the message once with a shared
Group Encryption Key (GEK), then sends a copy to each member's inbox
individually. The network treats group messages the same as regular inbox
messages for routing and storage purposes.

The server never sees plaintext — all group encryption and decryption is
performed client-side.

### 10.2 Group Encryption Key (GEK)

Each group has a symmetric AES-256-GCM key called the GEK. The group owner
generates the GEK and distributes it to each member by encrypting it with the
member's ML-KEM-1024 public key (from their profile).

- GEK is a 256-bit AES key used for AES-256-GCM encryption of message blobs
- Each member receives the GEK encrypted with their ML-KEM-1024 public key
- The encrypted GEK is stored in the GROUP_META record alongside the member list
- Members decrypt the GEK using their ML-KEM-1024 secret key

### 10.3 Fan-Out Model

When a group member sends a message:

1. Sender encrypts the message blob with the current GEK (AES-256-GCM)
2. Sender creates a GROUP_MESSAGE (data_type 0x05) for **each** group member
3. Each copy is routed to the recipient's inbox: `SHA3-256("inbox:" || recipient_fp)`
4. Responsible nodes store the message in TABLE_INBOX_INDEX + TABLE_MESSAGE_BLOBS
5. Connected recipients receive a NEW_MESSAGE push notification

This means group messages use the same storage, routing, and replication
infrastructure as 1:1 messages. No special server-side handling is needed
beyond recognizing the data type.

### 10.4 Group Metadata

Group metadata (data_type 0x06) is stored at routing key
`SHA3-256("group:" || group_id)` on the R closest nodes. It contains:

- Group identity (`group_id` = SHA3-256 of group creation record)
- Owner fingerprint (only the owner can modify the group)
- Version number (monotonically increasing)
- Member list with ML-KEM-1024 encrypted GEK per member
- ML-DSA-87 signature by the group owner

The group owner signs all metadata updates. Nodes verify the signature before
accepting a GROUP_META STORE. Version must be higher than the currently stored
version (replay prevention).

Local storage uses TABLE_GROUP_META with `group_id` as the key.

### 10.5 GEK Rotation

GEK rotation is required whenever a member is removed from the group:

1. Group owner generates a new GEK
2. Owner increments the GROUP_META version
3. Owner encrypts the new GEK with each remaining member's ML-KEM-1024 key
4. Owner publishes the updated GROUP_META record
5. Subsequent messages use the new `gek_version`

Removed members cannot decrypt messages sent after the rotation because they
do not have the new GEK. Messages sent before the rotation remain decryptable
by the removed member (no backward secrecy for past messages).

### 10.6 Constraints

- Maximum 512 members per group
- Group owner is the sole authority for membership changes
- GROUP_META is immutable per version — new version replaces old
- `group_id` is permanent (derived from creation record hash)

---

## 11. Node Identity & Lifecycle

### 11.1 Node Identity

Each node has:
- Its own ML-DSA-87 keypair (generated on first run)
- Node ID: `SHA3-256(node_ml_dsa_pubkey)`
- Publicly reachable address (IP:port for TCP, IP:port for WebSocket)
- All node-to-node messages are ML-DSA signed

### 11.2 Bootstrap

- 3 hardcoded bootstrap nodes: `0.bootstrap.cpunk.io`, `1.bootstrap.cpunk.io`,
  `2.bootstrap.cpunk.io`
- Bootstrap nodes are regular nodes with well-known DNS entries
- New node contacts any bootstrap → receives full membership list
- Bootstrap nodes have elevated role: can **slash bad nodes**

### 11.3 Node Joining

- **Open join** — any node can join by contacting a bootstrap
- No PoW or payment required to run a node
- New nodes start with low trust and earn reputation over time

### 11.4 Responsibility Transfer

When a new node joins (or an existing node leaves), the responsibility map
changes. Some keys may now have a new closest node.

1. New node computes which keys it is now responsible for
2. Contacts the other R-1 responsible nodes for each key
3. Syncs replication logs (SYNC_REQ / SYNC_RESP)
4. Begins serving those keys once caught up

Old responsible nodes that are no longer in the top R for a key can prune
that data after confirming the new responsible node is synced.

### 11.5 Trust & Reputation

- Nodes track reputation: uptime, responsiveness, correct behavior
- New nodes receive less responsibility until proven reliable
- Bootstrap nodes can **slash** (ban) malicious nodes:
  - Dropping messages, serving corrupt data, flooding
- Slash decisions propagate via gossip
- Slashed nodes are rejected by the network

---

## 12. Local Storage (libmdbx)

### 12.1 Database Layout

| Database         | Key Format                          | Value                     |
|------------------|-------------------------------------|---------------------------|
| profiles         | `SHA3-256("profile:" \|\| fp)`          | Signed profile document   |
| names            | `SHA3-256("name:" \|\| name)`       | Signed name record        |
| inbox_index      | `recipient_fp(32) \|\| msg_id(32)`  | sender_fp + timestamp + size (44 bytes) |
| message_blobs    | `msg_id(32)`                        | Encrypted blob (up to 50 MiB, 7-day TTL) |
| requests         | `recipient_fp(32) \|\| sender_fp(32)` | Contact request binary (composite key) |
| allowlists       | `SHA3-256("allowlist:" \|\| fp) \|\| allowed_fp(32)` | Allowlist entry (composite key, O(1) lookup) |
| repl_log         | `key \|\| seq_number`               | Replication log entries   |
| nodes            | `node_id`                           | Node info (addr, pubkey)  |
| group_meta       | `group_id(32)`                      | Group metadata binary (signed) |
| reputation       | `node_id`                           | Trust score + metrics     |

### 12.2 Replication Log Format

Each entry in `repl_log`:

```
{
    seq:       uint64,       // monotonically increasing per key
    op:        ADD | DEL | UPD,
    timestamp: uint64,       // milliseconds since Unix epoch
    data:      bytes         // the payload (blob, profile, etc.)
}
```

---

## 13. Tech Stack

| Component          | Library                        |
|--------------------|--------------------------------|
| Language           | C++                            |
| WebSocket          | uWebSockets                    |
| PQ crypto          | liboqs                         |
| Symmetric crypto   | libsodium (ChaCha20-Poly1305)  |
| Local storage      | libmdbx (C++ API)              |
| JSON               | jsoncpp                        |
| Logging            | spdlog                         |
| TCP networking     | POSIX sockets                  |

Minimal dependencies. Single binary deployment.
