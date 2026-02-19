# Chromatin Wire Protocol Specification

> Version 0x01 — 2026-02-17

This document defines the exact wire format and validation rules for the Chromatin
protocol. It is the implementor's reference — any conforming node MUST follow
this spec to interoperate with the Chromatin network.

For architectural overview and design rationale, see `protocol.md`.

---

## 1. TCP Node-to-Node Wire Format

All node-to-node communication uses TCP with the following message structure.
Messages are self-framing: the receiver reads header fields sequentially, then
uses the payload and signature length fields to read the variable-length portions.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      'C'      |      'H'      |      'R'      |      'M'      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Version    |     Type      |      Sender Port (2 bytes)    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                     Sender Node ID (32 bytes)                 |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload Length (4 bytes BE)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Payload (variable)                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Signature Length (2 bytes BE)               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                ML-DSA-87 Signature (variable, ~4627 bytes)    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Header Fields

| Field          | Size    | Description                                         |
|----------------|---------|-----------------------------------------------------|
| Magic          | 4 bytes | `CHRM` (0x43 0x48 0x52 0x4D). Always present.       |
| Version        | 1 byte  | Protocol version. Currently `0x01`.                 |
| Type           | 1 byte  | Message type (see table below).                     |
| Sender Port    | 2 bytes | Big-endian. Sender's TCP listening port.              |
| Sender Node ID | 32 bytes| SHA3-256 of sender's ML-DSA-87 public key.          |
| Payload Length  | 4 bytes | Big-endian. Length of the Payload field in bytes.    |
| Payload        | variable| Type-specific binary payload.                       |
| Signature Length| 2 bytes | Big-endian. Length of the Signature field in bytes.  |
| Signature      | variable| ML-DSA-87 signature.                                |

### Signature Coverage

The signature is computed over all bytes preceding the Signature Length field:

```
signed_data = magic || version || type || sender_port || sender_id || payload_length || payload
```

### Byte Order

All multi-byte integers in the protocol are **big-endian** (network byte order).

### Maximum Message Size

No hard protocol limit (TCP stream). Implementations SHOULD enforce a
configurable per-message limit (default: 50 MiB) to prevent memory exhaustion.
Future versions may introduce dedicated storage nodes for large files.

### Message Types

| Value | Type       | Direction         | Description                         |
|-------|------------|-------------------|-------------------------------------|
| 0x00  | PING       | Request           | Liveness check                      |
| 0x01  | PONG       | Response          | Liveness response                   |
| 0x02  | FIND_NODE  | Request           | Request full membership list        |
| 0x03  | NODES      | Response          | Node list response                  |
| 0x04  | STORE      | Request           | Store a signed value                |
| 0x05  | FIND_VALUE | Request           | Request a value by key              |
| 0x06  | VALUE      | Response          | Value response                      |
| 0x07  | SYNC_REQ   | Request           | Request replication log entries     |
| 0x08  | SYNC_RESP  | Response          | Replication log entries response    |
| 0x09  | STORE_ACK  | Response          | Acknowledgment of a STORE request   |

---

## 2. TCP Payload Formats

### PING (0x00)

Empty payload (0 bytes).

### PONG (0x01)

Empty payload (0 bytes).

### FIND_NODE (0x02)

Empty payload (0 bytes). Requests the full membership list from the recipient.

### NODES (0x03)

Response with the full node list.

```
[2 bytes BE: node_count]
For each node:
  [32 bytes: node_id]
  [4 bytes: IPv4 address]
  [2 bytes BE: tcp_port]
  [2 bytes BE: ws_port]
  [2 bytes BE: pubkey_length]
  [pubkey_length bytes: ML-DSA-87 public key]
```

### STORE (0x04)

Store a value on a responsible node.

```
[32 bytes: key]
[1 byte: data_type]
[4 bytes BE: value_length]
[value_length bytes: value]
```

Data types:

| Value | Type            |
|-------|-----------------|
| 0x00  | Profile         |
| 0x01  | Name record     |
| 0x02  | Inbox message   |
| 0x03  | Contact request |
| 0x04  | Allowlist entry |

Values 0x05-0xFF are reserved for future use (wallets, groups, channels, etc.).

### FIND_VALUE (0x05)

```
[32 bytes: key]
```

### VALUE (0x06)

```
[32 bytes: key]
[1 byte: found]              // 0x01 = found, 0x00 = not found
[4 bytes BE: value_length]   // 0 if not found
[value_length bytes: value]
```

### SYNC_REQ (0x07)

```
[32 bytes: key]
[8 bytes BE: after_seq]
```

### SYNC_RESP (0x08)

```
[32 bytes: key]
[2 bytes BE: entry_count]
For each entry:
  [8 bytes BE: seq]
  [1 byte: op]                // 0x00 = ADD, 0x01 = DEL, 0x02 = UPD
  [8 bytes BE: timestamp]     // Unix timestamp
  [4 bytes BE: data_length]
  [data_length bytes: data]
```

### STORE_ACK (0x09)

Sent by a node back to the requesting node after successfully processing a STORE.

```
[32 bytes: key]
[1 byte: status]              // 0x00 = OK, 0x01 = rejected
```

**Replication model (two-tier ACK):**

Client-facing operations (WebSocket SEND) are acknowledged immediately after
the receiving node stores the data locally. Replication to peer nodes is
asynchronous. STORE_ACK is a node-to-node signal — the originating node tracks
how many peers have confirmed storage. This information is internal bookkeeping
for replication health; the client is never blocked waiting for it.

Write quorum `W = min(2, R)` where `R = min(3, network_size)`. A store is
considered "durably replicated" when `W` nodes (including the originator) have
confirmed. If a peer does not ACK within a reasonable window, the originating
node relies on SYNC_REQ/SYNC_RESP to reconcile later.

---

## 3. Data Formats

These define the structure of values carried inside STORE and VALUE payloads.

### Profile (data_type 0x00)

```
[32 bytes: fingerprint]                  // SHA3-256(ml_dsa_pubkey)
[2 bytes BE: pubkey_length]
[pubkey_length bytes: ml_dsa_pubkey]
[2 bytes BE: kem_pubkey_length]
[kem_pubkey_length bytes: ml_kem_pubkey]
[1 byte: display_name_length]
[display_name_length bytes: display_name (UTF-8)]
[2 bytes BE: bio_length]
[bio_length bytes: bio (UTF-8)]
[4 bytes BE: avatar_length]              // 0 if no avatar
[avatar_length bytes: avatar]
[1 byte: social_links_count]
  For each:
    [1 byte: platform_length]
    [platform_length bytes: platform (UTF-8)]
    [1 byte: handle_length]
    [handle_length bytes: handle (UTF-8)]
[1 byte: wallet_count]
  For each:
    [1 byte: chain_length]
    [chain_length bytes: chain (UTF-8)]
    [1 byte: address_length]
    [address_length bytes: address (UTF-8)]
[8 bytes BE: sequence]
[2 bytes BE: signature_length]
[signature_length bytes: ML-DSA-87 signature over all preceding fields]
```

Storage key: `SHA3-256("dna:" || fingerprint)`

### Name Record (data_type 0x01)

```
[1 byte: name_length]
[name_length bytes: name (ASCII, lowercase)]
[32 bytes: fingerprint]
[8 bytes BE: pow_nonce]
[8 bytes BE: sequence]
[2 bytes BE: signature_length]
[signature_length bytes: ML-DSA-87 signature over all preceding fields]
```

Storage key: `SHA3-256("name:" || name)`

### Inbox Message (data_type 0x02)

```
[32 bytes: msg_id]
[32 bytes: sender_fingerprint]
[8 bytes BE: timestamp]
[4 bytes BE: blob_length]
[blob_length bytes: encrypted blob]
```

Storage key (DHT routing): `SHA3-256("inbox:" || recipient_fingerprint)`

Local mdbx storage uses two tables:

**TABLE_INBOX_INDEX** — Lightweight metadata for LIST responses:
```
Key:   recipient_fp(32) || msg_id(32)
Value: sender_fp(32) || timestamp(8 BE) || size(4 BE)
```

**TABLE_MESSAGE_BLOBS** — Raw encrypted blob data:
```
Key:   msg_id(32)
Value: encrypted blob (up to 50 MiB)
```

Prefix scan on TABLE_INBOX_INDEX by `recipient_fp` returns all message metadata.
DELETE removes entries from both tables.

### Contact Request (data_type 0x03)

```
[32 bytes: sender_fingerprint]
[8 bytes BE: pow_nonce]
[4 bytes BE: blob_length]
[blob_length bytes: encrypted blob]
```

Storage key: `SHA3-256("requests:" || recipient_fingerprint)`

### Allowlist Entry (data_type 0x04)

```
[1 byte: action]                         // 0x01 = allow, 0x00 = revoke
[32 bytes: allowed_fingerprint]
[8 bytes BE: sequence]
[2 bytes BE: signature_length]
[signature_length bytes: ML-DSA-87 signature over all preceding fields]
```

Storage key: `SHA3-256("allowlist:" || owner_fingerprint)`

To block a user, REVOKE them from the allowlist. There is no separate blocklist.

---

## 4. WebSocket Client-to-Node Protocol

Clients connect to a responsible node via WebSocket. Control messages are JSON
text frames. Large blob transfers use binary WebSocket frames (see Section 4.6).
Every client request includes an `id` field for response correlation.

### 4.1 Authentication

```json
// 1. Client -> Node
{"type": "HELLO", "id": 1, "fingerprint": "<64 hex chars>"}

// 2. Node -> Client
{"type": "CHALLENGE", "id": 1, "nonce": "<64 hex chars>"}

// 3. Client -> Node
{"type": "AUTH", "id": 1, "signature": "<hex>", "pubkey": "<hex>"}

// 4. Node -> Client
{"type": "OK", "id": 1, "pending_messages": 3}
```

The node verifies:
1. This node is responsible for the fingerprint's inbox — if NOT responsible,
   respond with REDIRECT (see below) and close the connection
2. `fingerprint == SHA3-256(pubkey)`
3. ML-DSA-87 signature over the nonce is valid

### 4.2 REDIRECT

If the node receiving HELLO is not responsible for the client's inbox, it
queries the R responsible nodes for their replication log sequence number and
returns them sorted by highest seq first. The client reconnects to the most
up-to-date node.

```json
{"type": "REDIRECT", "id": 1, "nodes": [
  {"address": "10.0.0.7", "ws_port": 4001, "seq": 45},
  {"address": "10.0.0.9", "ws_port": 4001, "seq": 45},
  {"address": "10.0.0.3", "ws_port": 4001, "seq": 42}
]}
```

The connection is closed after sending REDIRECT.

### 4.3 Client Commands (after authentication)

**SEND (small, <=64 KB)** — Push encrypted blob inline to recipient's inbox:
```json
{"type": "SEND", "id": 2, "to": "<fingerprint hex>", "blob": "<base64>"}
```
Response:
```json
{"type": "SEND_ACK", "id": 2, "msg_id": "<hex>"}
```

**SEND (large, >64 KB up to 50 MiB)** — Two-step: announce then chunked upload:
```json
// Step 1: Announce
{"type": "SEND", "id": 3, "to": "<fingerprint hex>", "size": 5242880}

// Response: ready for chunks
{"type": "SEND_READY", "id": 3, "request_id": 42}

// Step 2: Client sends binary UPLOAD_CHUNK frames (see Section 4.6)

// Step 3: After all chunks received
{"type": "SEND_ACK", "id": 3, "msg_id": "<hex>"}
```

If `size` > 50 MiB:
```json
{"type": "ERROR", "id": 3, "code": 413, "reason": "attachment too large"}
```

Incomplete chunked uploads are discarded after 30 seconds.

**LIST** — Retrieve message index with small blobs inlined:
```json
{"type": "LIST", "id": 4}
```
Response:
```json
{"type": "LIST_RESULT", "id": 4, "messages": [
  {"msg_id": "<hex>", "from": "<fingerprint hex>", "timestamp": 1708000100, "size": 1200, "blob": "<base64>"},
  {"msg_id": "<hex>", "from": "<fingerprint hex>", "timestamp": 1708000200, "size": 5242880, "blob": null}
]}
```

Messages with `size` <= 64 KB have their blob inlined (base64). Larger messages
have `blob: null` — the client must fetch them separately with GET.

LIST always returns all messages within the 7-day TTL window. Clients track
known `msg_id`s locally and skip GET for messages they already have (client-side
deduplication).

**GET** — Fetch a specific message blob by msg_id:
```json
{"type": "GET", "id": 5, "msg_id": "<hex>"}
```

Small blob response (<=64 KB):
```json
{"type": "GET_RESULT", "id": 5, "msg_id": "<hex>", "blob": "<base64>"}
```

Large blob response (>64 KB): JSON header followed by binary chunks:
```json
{"type": "GET_RESULT", "id": 5, "msg_id": "<hex>", "size": 5242880, "chunks": 5}
```
Then the server sends `chunks` binary DOWNLOAD_CHUNK frames (see Section 4.6).

**DELETE** — Optionally delete messages the client no longer needs:
```json
{"type": "DELETE", "id": 6, "msg_ids": ["<hex>", "<hex>"]}
```

Deletion is client-driven and optional. Messages expire automatically after
7 days via TTL. Multi-device users may choose not to delete until all devices
have fetched. Deletes from both TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS.

**ALLOW** — Add fingerprint to allowlist:
```json
{"type": "ALLOW", "id": 7, "fingerprint": "<hex>", "sequence": 1, "signature": "<hex>"}
```

The client signs `action(0x01) || allowed_fingerprint || sequence` with its
ML-DSA-87 key. The node verifies the signature and that sequence > currently
stored sequence. Response: `{"type": "OK", "id": 7}`

**REVOKE** — Remove fingerprint from allowlist:
```json
{"type": "REVOKE", "id": 8, "fingerprint": "<hex>", "sequence": 2, "signature": "<hex>"}
```

Same as ALLOW but with `action(0x00)`. Deletes the allowlist entry for the
specified fingerprint. Response: `{"type": "OK", "id": 8}`

**CONTACT_REQUEST** — Send contact request with PoW:
```json
{"type": "CONTACT_REQUEST", "id": 9, "to": "<hex>", "blob": "<base64>", "pow_nonce": 12345}
```

### 4.4 Server Push (unsolicited, no id)

**NEW_MESSAGE (small, <=64 KB)** — Incoming message inlined:
```json
{"type": "NEW_MESSAGE", "msg_id": "<hex>", "from": "<fingerprint hex>", "size": 1200, "blob": "<base64>", "timestamp": 1708000100}
```

**NEW_MESSAGE (large, >64 KB)** — Metadata only, client fetches with GET:
```json
{"type": "NEW_MESSAGE", "msg_id": "<hex>", "from": "<fingerprint hex>", "size": 5242880, "blob": null, "timestamp": 1708000200}
```

**CONTACT_REQUEST** — Incoming contact request (PoW already verified by node):
```json
{"type": "CONTACT_REQUEST", "from": "<hex>", "blob": "<base64>"}
```

### 4.5 Error Responses

```json
{"type": "ERROR", "id": 1, "code": 403, "reason": "signature verification failed"}
```

| Code | Meaning                                       |
|------|-----------------------------------------------|
| 400  | Malformed request                             |
| 401  | Not authenticated                             |
| 403  | Forbidden (not on allowlist, bad signature)   |
| 404  | Not found                                     |
| 409  | Conflict (name already registered)            |
| 413  | Attachment too large (>50 MiB)                |
| 429  | Rate limited / upload already in progress     |
| 500  | Internal error                                |

### 4.6 Binary WebSocket Frames — Chunked Transfer

Large blob transfers (>64 KB) use binary WebSocket frames with fixed 1 MiB
(1,048,576 byte) chunks. The last chunk may be smaller.

**Frame format:**
```
[1 byte: frame_type]
[4 bytes BE: request_id]
[2 bytes BE: chunk_index]    // 0-based
[payload: up to 1 MiB]
```

7 bytes overhead per chunk.

**Frame types:**

| Value | Type           | Direction        | Description                   |
|-------|----------------|------------------|-------------------------------|
| 0x01  | UPLOAD_CHUNK   | Client -> Node   | Chunk of a SEND blob          |
| 0x02  | DOWNLOAD_CHUNK | Node -> Client   | Chunk of a GET response       |

Number of chunks = `ceil(blob_size / 1_048_576)`.

**Server memory model:** Upload chunks are written directly to a temp file
(max 1 MiB memory per active upload). On completion, the file is moved to
TABLE_MESSAGE_BLOBS. Downloads read 1 MiB at a time from storage.

**Limits:** Maximum 1 concurrent chunked upload per connection. A second SEND
with `size` > 64 KB while an upload is in progress receives error 429.

---

## 5. Validation Rules

### TCP Message Validation

On every incoming TCP message, a conforming node MUST:

1. Verify magic == `CHRM`
2. Verify version == `0x01` (or a supported version)
3. Verify ML-DSA-87 signature over `magic || version || type || sender_port || sender_id || payload_length || payload`
4. Verify sender node ID exists in membership table (exception: FIND_NODE from unknown nodes during bootstrap)
5. Verify message size <= implementation-defined limit (recommended: 50 MiB)

### Profile STORE Validation

1. `fingerprint == SHA3-256(ml_dsa_pubkey)` — reject if mismatch
2. ML-DSA-87 signature valid over all fields preceding the signature
3. `sequence` > currently stored sequence for this fingerprint — reject replays
4. Total profile size <= 1 MiB (including avatar)

### Name Registration STORE Validation

1. Name matches `^[a-z0-9]{3,36}$`
2. `SHA3-256("chromatin:name:" || name || fingerprint || nonce)` has >= 28 leading zero bits
3. ML-DSA-87 signature valid over all fields preceding the signature
4. If name already registered to a different fingerprint — reject (first claim wins)
5. If same fingerprint — `sequence` must be higher than stored

### Inbox Message STORE Validation

1. Recipient's allowlist contains `sender_fingerprint` — reject if not allowed
2. `blob_length` <= 50 MiB
3. `msg_id` is unique — reject duplicates

### Contact Request STORE Validation

1. `SHA3-256("request:" || sender_fp || recipient_fp || nonce)` has >= 16 leading zero bits
2. `blob_length` <= 64 KiB

### Allowlist STORE Validation

1. ML-DSA-87 signature valid — only the inbox owner can modify their allowlist
2. `sequence` > currently stored sequence

### Responsibility Check

A node MUST only accept STORE requests for keys it is responsible for. A node
is responsible for a key if it is one of the R closest nodes to that key by
XOR distance, where `R = min(3, network_size)`.

---

## 6. Protocol Constants

| Constant                   | Value                                  |
|----------------------------|----------------------------------------|
| Magic                      | `CHRM` (0x43 0x48 0x52 0x4D)          |
| Protocol version           | `0x01`                                 |
| Hash algorithm             | SHA3-256 (32-byte output)              |
| Signing algorithm          | ML-DSA-87 (FIPS 204, Level 5)         |
| KEM algorithm              | ML-KEM-1024 (FIPS 203, client-side)   |
| Key space                  | 256 bits (SHA3-256 output)             |
| Replication factor (R)     | `min(3, network_size)`                 |
| Name PoW difficulty        | 28 leading zero bits                   |
| Contact request PoW        | 16 leading zero bits                   |
| Name regex                 | `^[a-z0-9]{3,36}$`                 |
| Max profile size           | 1 MiB                                  |
| Max message blob           | 50 MiB                                 |
| Inline threshold (WS)      | 64 KiB                                 |
| Chunk size (WS binary)     | 1 MiB (1,048,576 bytes)                |
| Chunked upload timeout     | 30 seconds                             |
| Max contact request blob   | 64 KiB                                 |
| Message TTL                | 7 days (604800 seconds)                |
| Max TCP message            | 50 MiB (implementation limit)          |
| Default TCP port           | 4000                                   |
| Default WebSocket port     | 4001                                   |
| Bootstrap nodes            | `0.bootstrap.cpunk.io`                 |
|                            | `1.bootstrap.cpunk.io`                 |
|                            | `2.bootstrap.cpunk.io`                 |

---

## 7. Key Computation

All storage keys are derived using SHA3-256 with domain prefixes:

```
profile_key  = SHA3-256("dna:"       || fingerprint)
name_key     = SHA3-256("name:"      || name)
inbox_key    = SHA3-256("inbox:"     || fingerprint)
request_key  = SHA3-256("requests:"  || fingerprint)
allow_key    = SHA3-256("allowlist:" || fingerprint)
```

Node ID:
```
node_id = SHA3-256(node_ml_dsa_87_public_key)
```

XOR distance:
```
distance(a, b) = a XOR b    (as 256-bit unsigned integer)
```

Responsibility: The R nodes with the smallest `distance(node_id, key)` are
responsible for storing data associated with that key.

---

## 8. Extensibility

The protocol is designed for future extension:

- **Data types:** Values 0x05-0xFF in the STORE `data_type` field are reserved
  for future use (wallets, groups, channels, and any new data kind).
- **Profile fields:** New fields can be appended to the profile format in future
  protocol versions. The `sequence` + signature mechanism handles versioned
  updates.
- **Protocol version:** The version byte in the TCP header allows breaking
  changes. Nodes reject unknown versions, and implementations can support
  multiple versions simultaneously.
- **Message types:** Values 0x0A-0xFF are reserved for future TCP message types.
