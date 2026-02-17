# Helix Protocol — Server Design

> Working document — v2 redesign. Server-only scope.
> Client protocol (message encryption, GEK, message format) deferred.

---

## 1. Overview

Helix is a decentralized, post-quantum-safe messaging node. It uses a
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
- Delete-after-fetch — minimal storage footprint
- Recipient inbox model — one connection, all messages
- Allowlist-enforced delivery — only approved contacts can message you
- Censorship resistant — anyone can run a node, network is unstoppable
- Simple — small network (dozens to hundreds of nodes), not millions

---

## 2. Cryptographic Primitives (Server-Side)

| Purpose              | Algorithm              | Notes                                    |
|----------------------|------------------------|------------------------------------------|
| Node identity        | ML-DSA-1024 (Dilithium5) | Node signing, signature verification   |
| Hashing              | SHA3-256               | Node IDs, data keys, PoW verification    |
| Node-to-node         | UDP                    | Kademlia protocol + replication          |
| Client-to-node       | WebSocket              | Inbox delivery, auth, commands           |

The server verifies ML-DSA signatures on profiles, name records, and client
authentication. It does **not** encrypt/decrypt messages — that is the client's
responsibility.

---

## 3. Identity — DNA (Server Perspective)

The server stores and serves DNA profiles. It does not create them.

### 3.1 Profile Data

A DNA profile is a signed, versioned document. The server stores and validates:

| Field            | Type                         | Notes                              |
|------------------|------------------------------|------------------------------------|
| fingerprint      | 32 bytes                     | SHA3-256(ml_dsa_pubkey)            |
| ml_dsa_pubkey    | bytes                        | Public signing key                 |
| ml_kem_pubkey    | bytes                        | Public encryption key              |
| display_name     | string                       | Human-readable name                |
| bio              | string                       | Free text                          |
| avatar           | blob                         | Profile image                      |
| social_links     | list of {platform, handle}   | Social media addresses             |
| wallet_addresses | list of {chain, address}     | Crypto wallet addresses            |
| sequence         | uint64                       | Monotonically increasing version   |
| signature        | bytes                        | ML-DSA signature over all above    |

Note: no `relays` field. The network determines which nodes hold a user's
inbox via XOR distance — users don't choose their relay.

### 3.2 Storage Key

```
profile_key = SHA3-256("dna:" || fingerprint)
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
    name:        string,       // 3-36 chars, [a-z0-9._-]
    fingerprint: 32 bytes,
    pow_nonce:   uint64,
    sequence:    uint64,
    signature:   bytes         // ML-DSA over all above
}
```

Storage key: `SHA3-256("name:" || name)`

### 4.2 Server Validation Rules

1. Verify PoW: `SHA3-256("helix:name:" || name || fingerprint || nonce)`
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
profile_key = SHA3-256("dna:" || fingerprint)     → R closest nodes store it
name_key    = SHA3-256("name:" || name)            → R closest nodes store it
inbox_key   = SHA3-256("inbox:" || fingerprint)    → R closest nodes store it
request_key = SHA3-256("requests:" || fingerprint) → R closest nodes store it
```

No separate "DHT layer" vs "relay layer". One node, one storage engine, one
replication strategy.

### 5.2 Node Diagram

```
┌──────────────────────────────────────────────────┐
│                   HELIX NODE                      │
│                                                   │
│  ┌──────────────────────────────────────────────┐ │
│  │           Kademlia Engine (UDP)               │ │
│  │                                               │ │
│  │  - Node discovery & gossip                    │ │
│  │  - XOR responsibility computation             │ │
│  │  - STORE / FIND_VALUE / replication           │ │
│  └──────────────────┬───────────────────────────┘ │
│                     │                             │
│  ┌──────────────────▼───────────────────────────┐ │
│  │           libmdbx (unified storage)           │ │
│  │                                               │ │
│  │  profiles/   — DNA profiles I'm responsible   │ │
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
        │ UDP                        │ WebSocket
        ▼                            ▼
  Other Nodes                   Clients
```

Single binary. One process, two listeners (UDP + WebSocket), one storage engine.

---

## 6. Kademlia Engine

### 6.1 Simplified Kademlia

Helix targets dozens to hundreds of nodes. Every node maintains **full
membership** — no k-buckets, no iterative lookups.

- 256-bit key space (SHA3-256 output)
- Node ID: `SHA3-256(node_ml_dsa_pubkey)`
- Distance: `XOR(id_a, id_b)` as unsigned 256-bit integer
- XOR distance determines **responsibility**, not routing
- All communication is **single-hop** (direct node-to-node UDP)

### 6.2 Responsibility

For a given key K, the R nodes with the smallest `XOR(node_id, K)` are
responsible for storing data associated with K.

- R = replication factor = `min(3, network_size - 1)`
- Any node can compute responsibility (full membership knowledge)
- Applies uniformly to profiles, names, AND inboxes

### 6.3 Signed Values

All stored values are ML-DSA-1024 signed. Nodes verify signatures before
accepting writes. Only the key owner can update their data (higher sequence
+ valid signature). Same security as OpenDHT's `putSigned`.

### 6.4 Kademlia Messages (UDP)

| Message      | Purpose                                    |
|--------------|--------------------------------------------|
| PING         | Liveness check                             |
| PONG         | Liveness response                          |
| FIND_NODE    | Request membership list                    |
| NODES        | Response with node list                    |
| STORE        | Write a signed value to responsible node   |
| FIND_VALUE   | Request a value by key                     |
| VALUE        | Response with stored value                 |
| SYNC_REQ     | Request replication log entries after seq N |
| SYNC_RESP    | Response with replication log entries       |

All messages are ML-DSA signed by the sending node.

---

## 7. Sequence-Based Replication

### 7.1 Replication Log

Every data mutation on a node is recorded in a per-key **replication log**
stored in libmdbx. Each entry has a monotonically increasing sequence number.

```
Key: inbox:alice

  seq=1  ADD  { msg_id: "abc", blob: <encrypted>, ts: 1708000000 }
  seq=2  ADD  { msg_id: "def", blob: <encrypted>, ts: 1708000060 }
  seq=3  DEL  { msg_id: "abc" }    ← ACK received, message deleted
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

- Once **all R responsible nodes** have confirmed seq >= N, entries before N
  can be pruned from the log
- Compaction runs periodically in the background
- For inboxes: 7-day TTL on unacknowledged messages provides a natural bound
- For profiles/names: log is small (infrequent updates), compaction less critical

### 7.5 Write Flow

When a client stores data (e.g., sends a message to an inbox):

1. Client (or forwarding node) sends STORE to **all R responsible nodes**
2. Each node appends to its local replication log, increments seq
3. Write is confirmed when **W of R** nodes ACK (W = quorum, e.g. 2 of 3)
4. Background sync ensures the remaining nodes catch up

### 7.6 Delete-After-Fetch Replication

When a recipient ACKs a message:

1. The node receiving the ACK writes a `DEL` entry to the replication log
2. Seq increments
3. Other responsible nodes sync the DEL entry
4. All R copies are deleted

This ensures delete-after-fetch is consistent across all replicas.

---

## 8. WebSocket Server — Client Interface

### 8.1 Connection Model

A client connects to **any of the R nodes responsible for their inbox**:

```
inbox_key = SHA3-256("inbox:" || fingerprint)
responsible_nodes = R closest nodes to inbox_key
client connects to any one of them via WebSocket
```

The client can discover responsible nodes by querying any node in the network
(since all nodes know full membership).

### 8.2 Client Authentication

```
1. Client → Node:   HELLO { fingerprint }
2. Node   → Client: CHALLENGE { nonce }
3. Client → Node:   AUTH { ML-DSA-sign(nonce), pubkey }
4. Node   → Client: OK { pending_message_count }
```

Node verifies: `fingerprint == SHA3-256(pubkey)` and signature is valid.
Node also verifies it is responsible for this fingerprint's inbox.

### 8.3 WebSocket Messages

**Client → Node:**

| Message           | Purpose                                      |
|-------------------|----------------------------------------------|
| SEND              | Push encrypted blob to recipient's inbox     |
| ACK               | Confirm receipt, triggers delete on all R    |
| ALLOW             | Add fingerprint to allowlist                 |
| REVOKE            | Remove fingerprint from allowlist            |
| CONTACT_REQUEST   | Send request to non-contact (PoW required)   |
| FETCH_PENDING     | Pull queued messages                         |

**Node → Client:**

| Message           | Purpose                                      |
|-------------------|----------------------------------------------|
| NEW_MESSAGE       | Incoming encrypted blob from allowed contact |
| CONTACT_REQUEST   | Incoming contact request (PoW-verified)      |
| SEND_ACK          | Confirmation that message was stored         |
| ERROR             | Rejection with reason                        |

### 8.4 Message Send Flow

When Alice sends a message to Bob:

1. Alice sends `SEND { to: bob_fp, blob }` to her connected node
2. Alice's node computes `inbox_key = SHA3-256("inbox:" || bob_fp)`
3. Alice's node determines R responsible nodes for Bob's inbox
4. Alice's node forwards the blob to **all R responsible nodes** via UDP STORE
5. Responsible nodes check Bob's allowlist — reject if Alice not allowed
6. Responsible nodes append to Bob's inbox replication log
7. If Bob is connected to one of those nodes, push `NEW_MESSAGE` immediately
8. Alice receives `SEND_ACK`

### 8.5 Allowlist

Each user has an allowlist stored on their responsible nodes.

- Managed via `ALLOW` / `REVOKE` commands
- Stored in mdbx on all R responsible nodes (replicated like everything else)
- Allowlist key: `SHA3-256("allowlist:" || fingerprint)`
- Replicated with the same seq-based mechanism

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
   `SHA3-256("request:" || alice_fp || bob_fp || nonce)` has M leading zero bits
4. Alice sends `CONTACT_REQUEST { to: bob_fp, blob, pow_nonce }` to a
   responsible node
5. Node verifies PoW, stores in Bob's request inbox (replication log)
6. Bob receives `CONTACT_REQUEST` when connected
7. On accept: both sides send `ALLOW` to their responsible nodes

### 9.3 Request PoW

- Lighter than name registration (~seconds, not minutes)
- Prevents request spam
- Node verifies before storing

---

## 10. Node Identity & Lifecycle

### 10.1 Node Identity

Each node has:
- Its own ML-DSA-1024 keypair (generated on first run)
- Node ID: `SHA3-256(node_ml_dsa_pubkey)`
- Publicly reachable address (IP:port for UDP, IP:port for WebSocket)
- All node-to-node messages are ML-DSA signed

### 10.2 Bootstrap

- 3 hardcoded bootstrap nodes: `0.bootstrap.cpunk.io`, `1.bootstrap.cpunk.io`,
  `2.bootstrap.cpunk.io`
- Bootstrap nodes are regular nodes with well-known DNS entries
- New node contacts any bootstrap → receives full membership list
- Bootstrap nodes have elevated role: can **slash bad nodes**

### 10.3 Node Joining

- **Open join** — any node can join by contacting a bootstrap
- No PoW or payment required to run a node
- New nodes start with low trust and earn reputation over time

### 10.4 Responsibility Transfer

When a new node joins (or an existing node leaves), the responsibility map
changes. Some keys may now have a new closest node.

1. New node computes which keys it is now responsible for
2. Contacts the other R-1 responsible nodes for each key
3. Syncs replication logs (SYNC_REQ / SYNC_RESP)
4. Begins serving those keys once caught up

Old responsible nodes that are no longer in the top R for a key can prune
that data after confirming the new responsible node is synced.

### 10.5 Trust & Reputation

- Nodes track reputation: uptime, responsiveness, correct behavior
- New nodes receive less responsibility until proven reliable
- Bootstrap nodes can **slash** (ban) malicious nodes:
  - Dropping messages, serving corrupt data, flooding
- Slash decisions propagate via gossip
- Slashed nodes are rejected by the network

---

## 11. Local Storage (libmdbx)

### 11.1 Database Layout

| Database         | Key Format                          | Value                     |
|------------------|-------------------------------------|---------------------------|
| profiles         | `SHA3-256("dna:" \|\| fp)`          | Signed profile document   |
| names            | `SHA3-256("name:" \|\| name)`       | Signed name record        |
| inboxes          | `SHA3-256("inbox:" \|\| fp)`        | Pending encrypted blobs   |
| requests         | `SHA3-256("requests:" \|\| fp)`     | Pending contact requests  |
| allowlists       | `SHA3-256("allowlist:" \|\| fp)`    | Set of allowed fps        |
| repl_log         | `key \|\| seq_number`               | Replication log entries   |
| nodes            | `node_id`                           | Node info (addr, pubkey)  |
| reputation       | `node_id`                           | Trust score + metrics     |

### 11.2 Replication Log Format

Each entry in `repl_log`:

```
{
    seq:       uint64,       // monotonically increasing per key
    op:        ADD | DEL | UPD,
    timestamp: uint64,       // unix timestamp
    data:      bytes         // the payload (blob, profile, etc.)
}
```

---

## 12. Tech Stack

| Component          | Library                        |
|--------------------|--------------------------------|
| Language           | C++                            |
| WebSocket          | uWebSockets                    |
| PQ crypto          | liboqs                         |
| Local storage      | libmdbx (C++ API)              |
| JSON               | jsoncpp                        |
| Logging            | spdlog                         |
| UDP networking     | POSIX sockets                  |

No OpenSSL, no Boost, no OpenDHT. Single binary deployment.

---

## 13. Open Questions

1. **Gossip protocol:** SWIM or simpler for node membership and failure
   detection.

2. **Write quorum:** W = 2 of 3? Or W = 1 (fire-and-forget with background
   sync)?

3. **Max message size:** Limit for a single encrypted blob?

4. **Reputation specifics:** Metrics, thresholds, slashing criteria. How to
   prevent bootstraps from becoming censorship points?

5. **Sync frequency:** How often do responsible nodes sync replication logs?
   Event-driven (on write) vs periodic (every N seconds)?

6. **Responsibility transfer protocol:** Detailed handoff when nodes
   join/leave. How to avoid data loss during transitions?
