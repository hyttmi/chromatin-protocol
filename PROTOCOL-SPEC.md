# Chromatin Wire Protocol Specification

> Version 0x01 — 2026-02-20

This document defines the exact wire format and validation rules for the Chromatin
protocol. It is the implementor's reference — any conforming node MUST follow
this spec to interoperate with the Chromatin network.

For architectural overview and design rationale, see `PROTOCOL.md`.

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
| 0x02  | FIND_NODE  | Request           | Request K closest nodes              |
| 0x03  | NODES      | Response          | Node list response                  |
| 0x04  | STORE      | Request           | Store a signed value                |
| 0x05  | FIND_VALUE | Request           | Request a value by key              |
| 0x06  | VALUE      | Response          | Value response                      |
| 0x07  | SYNC_REQ   | Request           | Request replication log entries     |
| 0x08  | SYNC_RESP  | Response          | Replication log entries response    |
| 0x09  | STORE_ACK  | Response          | Acknowledgment of a STORE request   |
| 0x0A  | SEQ_REQ    | Request           | Query replication log sequence       |
| 0x0B  | SEQ_RESP   | Response          | Replication log sequence response    |

---

## 2. TCP Transport Encryption (ML-KEM-1024 + ChaCha20-Poly1305)

TCP node-to-node connections support optional encryption using a 3-message
handshake that provides mutual authentication and forward secrecy via
ephemeral ML-KEM-1024 key encapsulation.

### 2.1 Probe-Based Detection

The first byte of a new TCP connection determines whether encryption is
being negotiated:

| First Byte | Meaning                                    |
|------------|--------------------------------------------|
| `0xCE`     | Encrypted handshake (HELLO message follows)|
| `0x43`     | Plaintext CHRM message (`'C'` from magic)  |

This allows nodes to accept both encrypted and plaintext connections on the
same TCP port. Nodes that do not support encryption simply reject the `0xCE`
probe byte and close the connection. The initiator then falls back to
plaintext and caches the failure per destination to avoid repeated handshake
attempts.

### 2.2 Handshake Protocol

The handshake consists of three messages: HELLO, ACCEPT, and CONFIRM.

**HELLO** (Initiator -> Responder, ~1637 bytes):
```
[1 byte:  0xCE]                      // Encryption probe byte
[1 byte:  version]                   // Protocol version (0x01)
[1 byte:  cipher]                    // Cipher suite (0x01 = ChaCha20-Poly1305)
[32 bytes: initiator_node_id]        // SHA3-256(initiator ML-DSA-87 pubkey)
[2 bytes BE: kem_pk_len]             // ML-KEM-1024 public key length (1568)
[kem_pk_len bytes: ephemeral_kem_pubkey]  // Ephemeral ML-KEM-1024 public key
[32 bytes: hello_random]             // Cryptographic random nonce
```

**ACCEPT** (Responder -> Initiator, ~6265 bytes):
```
[1 byte:  version]                   // Protocol version (0x01)
[1 byte:  cipher]                    // Cipher suite (0x01 = ChaCha20-Poly1305)
[32 bytes: responder_node_id]        // SHA3-256(responder ML-DSA-87 pubkey)
[2 bytes BE: ct_len]                 // KEM ciphertext length (1568)
[ct_len bytes: kem_ciphertext]       // ML-KEM-1024 ciphertext (encapsulated shared secret)
[32 bytes: accept_random]            // Cryptographic random nonce
[2 bytes BE: sig_len]                // ML-DSA-87 signature length
[sig_len bytes: signature]           // ML-DSA-87 signature (~4627 bytes)
```

**CONFIRM** (Initiator -> Responder, ~4629 bytes):
```
[2 bytes BE: sig_len]                // ML-DSA-87 signature length
[sig_len bytes: signature]           // ML-DSA-87 signature (~4627 bytes)
```

### 2.3 Signature Coverage

Both ACCEPT and CONFIRM signatures provide mutual authentication and bind
the handshake to the specific participants and session.

**ACCEPT signature** (Responder signs):
```
signed_data = hello_random || initiator_node_id || responder_node_id || accept_random || kem_ciphertext
```

**CONFIRM signature** (Initiator signs):
```
signed_data = hello_random || accept_random || kem_ciphertext || responder_node_id
```

Both signatures include the random nonces and KEM ciphertext, preventing
replay attacks and binding the authentication to the key exchange.

### 2.4 Key Derivation

After the handshake, both sides derive directional session keys from the
shared secret (`ss`) produced by ML-KEM-1024 decapsulation:

```
i2r_key = SHA3-256("chromatin:tcp:i2r:" || ss || hello_random || accept_random)
r2i_key = SHA3-256("chromatin:tcp:r2i:" || ss || hello_random || accept_random)
```

- `i2r_key`: Used by the initiator to encrypt, responder to decrypt
- `r2i_key`: Used by the responder to encrypt, initiator to decrypt

Domain-separated prefixes (`"chromatin:tcp:i2r:"` and `"chromatin:tcp:r2i:"`)
ensure the two directional keys are cryptographically independent. Both
random nonces are included to bind the keys to this specific session.

### 2.5 Encrypted Frame Format

After handshake completion, all subsequent CHRM messages on the connection
are encrypted. Each encrypted frame has the following format:

```
[4 bytes BE: ciphertext_length]      // Length of the encrypted payload
[ciphertext_length bytes: ciphertext] // ChaCha20-Poly1305 encrypted CHRM message
```

**AEAD parameters:**
- **Key**: `i2r_key` or `r2i_key` (depending on direction)
- **Nonce**: 12 bytes — `[4 zero bytes][8 bytes BE: counter]`. The counter
  starts at 0 and increments by 1 for each message sent in that direction.
  Each direction maintains its own counter.
- **AAD** (Additional Authenticated Data): The 4-byte big-endian length
  header preceding the ciphertext. This binds the length to the ciphertext,
  preventing length-manipulation attacks.
- **Plaintext**: The complete CHRM message (including `CHRM` magic, header,
  payload, and signature as defined in Section 1).

### 2.6 Cipher Suites

| Value | Cipher                    | KEM            | Key Size |
|-------|---------------------------|----------------|----------|
| 0x01  | ChaCha20-Poly1305         | ML-KEM-1024    | 256-bit  |

Values 0x02-0xFF are reserved for future cipher suites. If a responder does
not support the requested cipher suite, it closes the connection. The
initiator SHOULD fall back to plaintext.

### 2.7 Backward Compatibility

Encryption is optional and backward compatible:

1. **Initiator probes**: Sends `0xCE` as the first byte of the connection.
2. **Responder without encryption**: Does not recognize `0xCE`, rejects the
   connection (closes socket or sends an error).
3. **Initiator fallback**: On handshake failure, the initiator falls back to
   plaintext (standard CHRM messages starting with `0x43`).
4. **Per-destination caching**: The initiator caches which destinations do
   not support encryption to avoid repeated handshake attempts. The cache
   is cleared periodically or when the destination's routing table entry
   is refreshed.

This ensures that nodes running older software without encryption support
can still participate in the network. As the network upgrades, more
connections will be encrypted without requiring coordinated deployment.

### 2.8 Security Properties

- **Mutual authentication**: Both sides prove their identity via ML-DSA-87
  signatures over the handshake transcript.
- **Forward secrecy**: Ephemeral ML-KEM-1024 keys are generated per
  connection. Compromise of long-term signing keys does not reveal past
  session keys.
- **Replay protection**: Random nonces (`hello_random`, `accept_random`)
  and per-message counters prevent replay attacks.
- **Direction separation**: Separate `i2r_key` and `r2i_key` prevent
  reflection attacks where a message sent in one direction is replayed
  in the other.
- **Post-quantum security**: ML-KEM-1024 (FIPS 203) and ML-DSA-87
  (FIPS 204) provide security against quantum adversaries.

---

## 3. TCP Payload Formats

### PING (0x00)

Empty payload (0 bytes).

### PONG (0x01)

Empty payload (0 bytes).

### FIND_NODE (0x02)

```
[2 bytes BE: pubkey_length]             // Sender's public key length (0 if omitted)
[pubkey_length bytes: sender_pubkey]    // Sender's ML-DSA-87 public key (optional)
```

Requests the K closest nodes to the sender's node ID from the recipient.
The sender SHOULD include its public key so the receiver can immediately
verify the sender's identity via `SHA3-256(pubkey) == sender_id`. This
enables the receiver to accept signed messages (STORE, FIND_VALUE, etc.)
from the sender without waiting for pubkey propagation via NODES responses.

If `pubkey_length` is 0, or the payload is empty, the receiver adds the
sender to its routing table without a public key (legacy behavior).

**Iterative discovery:** When a node receives NODES and discovers new peers,
it SHOULD send FIND_NODE to each newly discovered node. This implements
standard Kademlia iterative lookup and ensures bidirectional pubkey exchange.

### NODES (0x03)

Response with the K closest nodes to the requested target (K=20).

```
[2 bytes BE: node_count]
For each node:
  [32 bytes: node_id]
  [1 byte: address_family]        // 0x04 = IPv4, 0x06 = IPv6
  [4 or 16 bytes: address]        // 4 bytes for IPv4, 16 bytes for IPv6
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

Values 0x05-0xFF are reserved for future use (groups, channels, etc.).

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
  [8 bytes BE: timestamp]     // Milliseconds since Unix epoch
  [1 byte: data_type]         // 0x00=profile, 0x01=name, 0x02=inbox, 0x03=request, 0x04=allowlist
  [4 bytes BE: data_length]
  [data_length bytes: data]
```

### STORE_ACK (0x09)

Sent by a node back to the requesting node after successfully processing a STORE.

```
[32 bytes: key]
[1 byte: status]              // 0x00 = OK, 0x01 = rejected
```

### SEQ_REQ (0x0A)

Query a remote node for its replication log sequence number for a given key.
Used by REDIRECT to find the most up-to-date responsible node.

```
[32 bytes: key]
```

### SEQ_RESP (0x0B)

Response with the current replication log sequence number for the requested key.

```
[32 bytes: key]
[8 bytes BE: seq]
```

**SYNC_RESP validation:** Every ADD entry in a SYNC_RESP is validated
using the same rules as a direct STORE (see Section 6) before being
applied to local storage. This prevents malicious nodes from injecting
forged data via the sync protocol.

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

## 4. Data Formats

These define the structure of values carried inside STORE and VALUE payloads.

### Profile (data_type 0x00)

```
[32 bytes: fingerprint]                  // SHA3-256(ml_dsa_pubkey)
[2 bytes BE: pubkey_length]
[pubkey_length bytes: ml_dsa_pubkey]
[2 bytes BE: kem_pubkey_length]
[kem_pubkey_length bytes: ml_kem_pubkey]
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
[8 bytes BE: sequence]
[2 bytes BE: signature_length]
[signature_length bytes: ML-DSA-87 signature over all preceding fields]
```

Storage key: `SHA3-256("profile:" || fingerprint)`

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

Kademlia STORE value (includes recipient_fp for two-table routing):
```
[32 bytes: recipient_fingerprint]
[32 bytes: msg_id]
[32 bytes: sender_fingerprint]
[8 bytes BE: timestamp]              // Milliseconds since Unix epoch
[4 bytes BE: blob_length]
[blob_length bytes: encrypted blob]
```

Storage key (DHT routing): `SHA3-256("inbox:" || recipient_fingerprint)`

Receiving nodes parse `recipient_fp` from the value to build the two-table
storage model. Local mdbx storage uses two tables:

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
[32 bytes: recipient_fingerprint]
[32 bytes: sender_fingerprint]
[8 bytes BE: pow_nonce]
[8 bytes BE: timestamp]              // Milliseconds since Unix epoch (client-provided)
[4 bytes BE: blob_length]
[blob_length bytes: encrypted blob]
```

Minimum size: 84 bytes (32 + 32 + 8 + 8 + 4 + 0).

DHT routing key: `SHA3-256("requests:" || recipient_fingerprint)`

Local mdbx storage uses composite key for multi-sender support:
```
Key:   recipient_fp(32) || sender_fp(32)
Value: full contact request binary (recipient_fp || sender_fp || pow_nonce || timestamp || blob_len || blob)
```

PoW preimage: `"chromatin:request:" || sender_fingerprint || recipient_fingerprint || timestamp_BE`
Required: 16 leading zero bits in `SHA3-256(preimage || nonce_BE)`.

The timestamp is provided by the client and must be within 1 hour of the
server's current time (±3,600,000 ms). This prevents indefinite nonce reuse —
a valid PoW is only accepted during its validity window.

Including `recipient_fp` and `timestamp` in the binary allows Kademlia nodes
to verify PoW independently without access to the routing key derivation.

### Allowlist Entry (data_type 0x04)

Kademlia STORE value (includes owner_fp and allowed_fp for signature
verification and composite key construction):
```
[32 bytes: owner_fingerprint]
[32 bytes: allowed_fingerprint]
[1 byte: action]                         // 0x01 = allow, 0x00 = revoke
[8 bytes BE: sequence]
[SIGNATURE_SIZE bytes: ML-DSA-87 signature]
```

The signature covers a domain-separated message:
`"chromatin:allowlist:" || owner_fingerprint(32) || action(1) || allowed_fingerprint(32) || sequence(8 BE)`
signed by the owner's ML-DSA-87 key.

Storage key (DHT routing): `SHA3-256("allowlist:" || owner_fingerprint)`

Local mdbx storage uses composite key for O(1) lookup:
```
Key:   SHA3-256("allowlist:" || owner_fp)(32) || allowed_fp(32)
Value: owner_fp(32) || allowed_fp(32) || action(1) || sequence(8 BE) || signature
```

Receiving nodes verify the ML-DSA-87 signature against the owner's public key
(looked up from TABLE_PROFILES). If the owner has no stored profile yet, the
entry is **rejected** — the profile must propagate before allowlist entries.
This prevents forged entries during the bootstrap propagation window.

Receiving nodes parse `allowed_fp` from the Kademlia value to build the
composite storage key. REVOKE (action=0x00) deletes the entry rather than
storing it. To block a user, REVOKE them from the allowlist. There is no
separate blocklist.

---

## 5. WebSocket Client-to-Node Protocol

Clients connect to a responsible node via WebSocket. Control messages are JSON
text frames. Large blob transfers use binary WebSocket frames (see Section 5.7).
Every client request includes an `id` field for response correlation.

**TLS:** Nodes SHOULD enable TLS (WSS) to protect metadata (fingerprints,
allowlist operations, message counts) from network observers. When configured
with `tls_cert_path` and `tls_key_path`, the node listens with TLS on the same
`ws_port`. Clients SHOULD prefer `wss://` connections. Message content is
already encrypted with ML-KEM-1024, but TLS provides transport-level metadata
protection.

### 5.1 Authentication

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
3. ML-DSA-87 signature over `"chromatin-auth:" || nonce` (47 bytes) is valid.
   The domain prefix prevents cross-protocol signature replay attacks.

### 5.2 REDIRECT

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

### 5.3 Client Commands (after authentication)

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

// Step 2: Client sends binary UPLOAD_CHUNK frames (see Section 5.7)

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
{"type": "LIST", "id": 4, "limit": 50, "after": "<msg_id hex>"}
```

Optional fields:
- `limit` — Maximum number of messages to return (default 50, max 200)
- `after` — Cursor: start after this msg_id (for pagination). Omit for first page.

Response:
```json
{"type": "LIST_RESULT", "id": 4, "messages": [
  {"msg_id": "<hex>", "from": "<fingerprint hex>", "timestamp": 1708000100000, "size": 1200, "blob": "<base64>"},
  {"msg_id": "<hex>", "from": "<fingerprint hex>", "timestamp": 1708000200000, "size": 5242880, "blob": null}
], "has_more": true}
```

Messages with `size` <= 64 KB have their blob inlined (base64). Larger messages
have `blob: null` — the client must fetch them separately with GET.

`has_more` is `true` if there are more messages after the returned set.
Clients paginate by passing the last `msg_id` as the `after` cursor in the
next request. Clients track known `msg_id`s locally and skip GET for messages
they already have (client-side deduplication).

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
Then the server sends `chunks` binary DOWNLOAD_CHUNK frames (see Section 5.7).

**DELETE** — Optionally delete messages the client no longer needs:
```json
{"type": "DELETE", "id": 6, "msg_ids": ["<hex>", "<hex>"]}
```

Deletion is client-driven and optional. Messages expire automatically after
7 days via TTL. Multi-device users may choose not to delete until all devices
have fetched. Deletes from both TABLE_INBOX_INDEX and TABLE_MESSAGE_BLOBS.
Deletes are replicated via the replication log (`Op::DEL`) and synced to
other responsible nodes via SYNC_REQ/SYNC_RESP.
Response: `{"type": "OK", "id": 6}`

**ALLOW** — Add fingerprint to allowlist:
```json
{"type": "ALLOW", "id": 7, "fingerprint": "<hex>", "sequence": 1, "signature": "<hex>"}
```

The client signs `"chromatin:allowlist:" || owner_fingerprint || action(0x01) || allowed_fingerprint || sequence` with its
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
{"type": "CONTACT_REQUEST", "id": 9, "to": "<hex>", "blob": "<base64>", "pow_nonce": 12345, "timestamp": 1708000100000}
```
The `timestamp` is milliseconds since Unix epoch, provided by the client. The
node validates it is within 1 hour of server time and includes it in the PoW
preimage. Response: `{"type": "OK", "id": 9}`

**STATUS** — Health check (no authentication required):
```json
{"type": "STATUS", "id": 10}
```
Response:
```json
{
  "type": "STATUS_RESP", "id": 10,
  "node_id": "<hex>",
  "uptime_seconds": 12345,
  "connected_clients": 3,
  "authenticated_clients": 2,
  "routing_table_size": 15
}
```

**RESOLVE_NAME** — Look up a fingerprint by username:
```json
{"type": "RESOLVE_NAME", "id": 11, "name": "alice"}
```
Response:
```json
{"type": "RESOLVE_NAME_RESULT", "id": 11, "found": true, "fingerprint": "<hex>"}
```
If not found: `{"type": "RESOLVE_NAME_RESULT", "id": 11, "found": false}`

Only searches local storage (the node must be responsible for the name's
routing key to have the data).

**GET_PROFILE** — Fetch a user's profile by fingerprint:
```json
{"type": "GET_PROFILE", "id": 12, "fingerprint": "<hex>"}
```
Response:
```json
{
  "type": "PROFILE_RESULT", "id": 12, "found": true,
  "fingerprint": "<hex>", "pubkey": "<hex>", "kem_pubkey": "<hex>",
  "bio": "Hello world", "avatar": "<base64 or null>",
  "social_links": [{"platform": "x", "handle": "@alice"}],
  "sequence": 1
}
```
If not found: `{"type": "PROFILE_RESULT", "id": 12, "found": false}`

**LIST_REQUESTS** — List pending contact requests:
```json
{"type": "LIST_REQUESTS", "id": 13}
```
Response:
```json
{"type": "LIST_REQUESTS_RESULT", "id": 13, "requests": [
  {"from": "<fingerprint hex>", "timestamp": 1708000100000, "blob": "<base64>"}
]}
```

**SET_PROFILE** — Publish or update a signed profile:
```json
{"type": "SET_PROFILE", "id": 14, "profile": "<base64 of signed profile binary>"}
```
The client constructs and signs the profile binary (see Section 4). The node
validates the profile (fingerprint matches session, signature is valid) and
stores/replicates it via Kademlia.
Response: `{"type": "SET_PROFILE_ACK", "id": 14}`

**REGISTER_NAME** — Register a permanent name record:
```json
{"type": "REGISTER_NAME", "id": 15, "name_record": "<base64 of signed name record binary>"}
```
Client builds and signs the name record binary, node validates (PoW, signature,
name not already taken) and stores/replicates via Kademlia. Names are permanent
and cannot be updated — first claim wins.
Response: `{"type": "REGISTER_NAME_ACK", "id": 15}`

### 5.4 Server Push (unsolicited, no id)

Push notifications are delivered to **all connected devices** for a given
fingerprint. When multiple devices are authenticated with the same identity,
each receives the push notification independently.

**NEW_MESSAGE (small, <=64 KB)** — Incoming message inlined:
```json
{"type": "NEW_MESSAGE", "msg_id": "<hex>", "from": "<fingerprint hex>", "size": 1200, "blob": "<base64>", "timestamp": 1708000100000}
```

**NEW_MESSAGE (large, >64 KB)** — Metadata only, client fetches with GET:
```json
{"type": "NEW_MESSAGE", "msg_id": "<hex>", "from": "<fingerprint hex>", "size": 5242880, "blob": null, "timestamp": 1708000200000}
```

**CONTACT_REQUEST** — Incoming contact request (PoW already verified by node):
```json
{"type": "CONTACT_REQUEST", "from": "<hex>", "blob": "<base64>"}
```

### 5.5 Rate Limiting

Nodes enforce per-connection rate limiting using a token bucket algorithm.
Each connection starts with 50 tokens and refills at 10 tokens/second.
Different commands consume different token costs:

| Command          | Cost |
|------------------|------|
| SEND             | 2    |
| CONTACT_REQUEST  | 3    |
| SET_PROFILE / REGISTER_NAME | 2 |
| LIST / GET / DELETE / ALLOW / REVOKE | 1 |
| RESOLVE_NAME / GET_PROFILE / LIST_REQUESTS | 1 |
| HELLO / AUTH     | 1    |
| STATUS           | 0    |

When a client exceeds the rate limit, the node responds with error code 429.

### 5.5.1 Worker Pool Backpressure

The node's worker pool has a bounded queue (max 1024 pending jobs). When the
queue is full, new requests that require worker pool execution (HELLO redirect
checks, SEND, CONTACT_REQUEST, ALLOW, REVOKE) receive error code 503. This
prevents unbounded memory growth under high load.

### 5.6 Error Responses

```json
{"type": "ERROR", "id": 1, "code": 403, "reason": "signature verification failed"}
```

| Code | Meaning                                       |
|------|-----------------------------------------------|
| 400  | Malformed request                             |
| 401  | Not authenticated                             |
| 403  | Forbidden (not on allowlist, bad signature)   |
| 404  | Not found                                     |
| 408  | Upload timeout (incomplete chunked upload)    |
| 409  | Conflict (name already registered)            |
| 413  | Attachment too large (>50 MiB)                |
| 429  | Rate limited / upload already in progress     |
| 500  | Internal error                                |
| 503  | Service unavailable (worker pool at capacity) |

### 5.7 Binary WebSocket Frames — Chunked Transfer

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

**Server memory model:** Upload chunks are accumulated in memory. On
completion, the assembled blob is stored in TABLE_INBOX_INDEX (metadata)
and TABLE_MESSAGE_BLOBS (blob data), and replicated via Kademlia.

**Limits:** Maximum 1 concurrent chunked upload per connection. A second SEND
with `size` > 64 KB while an upload is in progress receives error 429.

---

## 6. Validation Rules

### TCP Message Validation

On every incoming TCP message, a conforming node MUST:

1. Verify magic == `CHRM`
2. Verify version == `0x01` (or a supported version)
3. Verify ML-DSA-87 signature over `magic || version || type || sender_port || sender_id || payload_length || payload`
4. Verify sender node ID exists in membership table (exception: PING and NODES from unknown nodes during bootstrap)
5. Verify message size <= implementation-defined limit (recommended: 50 MiB)

**Signature verification:** PING and FIND_NODE are accepted without signature
verification (needed for initial discovery by unknown nodes). All other
message types — including PONG, NODES, STORE, FIND_VALUE, SYNC_REQ,
SYNC_RESP, STORE_ACK, SEQ_REQ, SEQ_RESP — MUST have valid ML-DSA-87
signatures. Messages from nodes whose public key is not yet known are
rejected (except PING and FIND_NODE). Public keys are learned via two
mechanisms:

1. **FIND_NODE payload:** The sender includes its public key. The receiver
   verifies `SHA3-256(pubkey) == sender_id` and stores the pubkey.
2. **NODES responses:** Each node entry includes the node's ML-DSA-87 public
   key. The receiver verifies `node_id == SHA3-256(pubkey)` for each entry.

**FIND_NODE pubkey verification:** When a FIND_NODE message includes a
sender pubkey, the receiver MUST verify `SHA3-256(pubkey) == sender_id`
before storing it. Invalid pubkeys are silently ignored (the sender is
still added to the routing table without a pubkey).

**NODES response node_id verification:** When processing a NODES response,
the receiver MUST verify `node_id == SHA3-256(pubkey)` for each node entry.
Entries that fail this check are silently dropped. This prevents eclipse
attacks via forged node entries.

**Iterative discovery:** When a NODES response contains nodes not already
in the routing table, the receiver SHOULD send FIND_NODE to each new node.
This propagates the sender's pubkey to all discovered nodes, enabling
immediate signed message exchange (STORE, FIND_VALUE, etc.).

### Profile STORE Validation

1. `fingerprint == SHA3-256(ml_dsa_pubkey)` — reject if mismatch
2. ML-DSA-87 signature valid over all fields preceding the signature
3. `sequence` > currently stored sequence for this fingerprint — reject replays
4. Total profile size <= 1 MiB (including avatar)

### Name Registration STORE Validation

1. Name matches `^[a-z0-9]{3,36}$`
2. `SHA3-256("chromatin:name:" || name || fingerprint || nonce)` has >= 28 leading zero bits
3. ML-DSA-87 signature valid over all fields preceding the signature
4. Owner's profile MUST be stored locally — reject if absent (prevents forged entries during propagation window)
5. If name already registered to a different fingerprint — reject (first claim wins)
6. If same fingerprint — `sequence` must be higher than stored

### FIND_VALUE Scope

FIND_VALUE searches profiles and names only. Inbox messages, contact requests,
and allowlists use composite storage keys that cannot be derived from the DHT
routing key alone. These data types are accessed via scan() or replicated via
SYNC_REQ/SYNC_RESP instead.

### Inbox Message STORE Validation

1. Check recipient's allowlist:
   - If recipient has an allowlist AND sender is on it → allow
   - If recipient has an allowlist AND sender is NOT on it → reject
   - If recipient has NO allowlist entries → allow (open inbox, new user)
2. `blob_length` <= 50 MiB
3. `msg_id` is unique — reject duplicates (prevents replay attacks)

### Contact Request STORE Validation

1. Minimum size: 84 bytes
2. `timestamp` (from binary) must be within 1 hour of server time (±3,600,000 ms)
3. `SHA3-256("chromatin:request:" || sender_fp || recipient_fp || timestamp_BE || nonce_BE)` has >= 16 leading zero bits
4. `blob_length` <= 64 KiB

### Allowlist STORE Validation

1. `action` byte must be 0x00 (revoke) or 0x01 (allow)
2. ML-DSA-87 signature verified against owner's public key (from TABLE_PROFILES)
   - Signed data: `"chromatin:allowlist:" || owner_fingerprint(32) || action(1) || allowed_fingerprint(32) || sequence(8 BE)`
   - If owner's profile is not stored yet, the entry is **rejected** (profile must propagate first)
3. `sequence` > currently stored sequence (enforced by WS server)

### REVOKE Replication

When a REVOKE (action=0x00) is processed, it is recorded in the replication
log as `Op::DEL`. This ensures that when other responsible nodes sync via
SYNC_REQ/SYNC_RESP, the allowlist entry is deleted on those nodes as well.
ALLOW (action=0x01) is recorded as `Op::ADD`.

### Responsibility Check

A node MUST only accept STORE requests for keys it is responsible for. A node
is responsible for a key if it is one of the R closest nodes to that key by
XOR distance, where `R = min(3, network_size)`.

### Responsibility Transfer

When the routing table changes (nodes join or leave), a node MUST check if
data it holds is now closer to a newly discovered node. If so, the node
pushes that data via STORE to all current responsible nodes. This check runs
periodically (every 60 seconds) and only when the routing table size has
changed. Responsibility transfer covers all data types: profiles, names,
inbox messages (INDEX + BLOB tables), contact requests, and allowlist entries.

### Routing Table

The routing table holds up to **256 nodes**. When full and a new node is
discovered, the node with the oldest `last_seen` timestamp is evicted (LRU).
`closest_to()` queries use partial sort for efficiency — only the top K
results are sorted, not the entire table.

### TCP Connection Pooling

Nodes reuse TCP connections for node-to-node communication. After sending a
message, the socket is returned to a per-destination connection pool. Idle
connections are closed after 60 seconds. The pool is bounded at 64
connections. Dead connections are detected via `poll()` + `recv(MSG_PEEK)`
before reuse. Servers accept multiple messages per TCP connection.

### Replication Log Compaction

The replication log is compacted periodically (every 1 hour). For each key,
only the most recent 10,000 entries are retained. Older entries are deleted.
This bounds replication log storage growth while preserving enough history
for sync catch-up. The previous default of 100 was found to be dangerously
low — a busy inbox can exhaust it in minutes, causing data loss for slow
syncers.

### Active Sync

Nodes periodically sync with responsible peers (every 120 seconds by
default). For each key the node is responsible for, it checks if peers have
higher replication log sequences and pulls missing entries via SYNC_REQ.
This ensures eventual consistency even when direct STORE delivery fails.

### Data Ordering Requirement

Certain data types have ordering dependencies:

- **Profile must exist before allowlist entries** — Allowlist entries require
  the owner's public key (from their profile) for signature verification.
  Entries are rejected if the profile hasn't propagated yet.
- **Profile must exist before name records** — Same reasoning: name records
  reference a fingerprint whose public key must be verifiable.

### Secret Key Handling

Node secret keys MUST be zeroed from memory on destruction (e.g., using
`OQS_MEM_cleanse()` or equivalent). The node key file (`node.key`) MUST
be written with restrictive permissions (0600 — owner read/write only).

### TCP Transport

Node-to-node TCP uses `poll()` (not `select()`) to avoid the `FD_SETSIZE`
limit. The maximum number of concurrent TCP client connections is
configurable (default 256). Idle connections are tracked and evicted.
Connection pool keys use `[addr]:port` format for IPv6 compatibility.

### External Address

Nodes MUST configure `external_address` in settings.json with a routable
address when running behind NAT or binding to `0.0.0.0`. The default bind
address is not routable and will cause peer discovery failures.

### Cryptographic Random

All random values (challenge nonces, msg_ids) MUST be generated using a
cryptographic random source. Implementations using liboqs SHOULD use
`OQS_randombytes()` which is backed by the OS CSPRNG.

---

## 7. Protocol Constants

All values below are **configurable** via `settings.json`. The table shows
defaults. See `chromatin-node --generate-config` for a complete template.

| Constant                   | Value                                  |
|----------------------------|----------------------------------------|
| Magic                      | `CHRM` (0x43 0x48 0x52 0x4D)          |
| Protocol version           | `0x01`                                 |
| Hash algorithm             | SHA3-256 (32-byte output)              |
| Signing algorithm          | ML-DSA-87 (FIPS 204, Level 5)         |
| KEM algorithm              | ML-KEM-1024 (FIPS 203)                |
| Key space                  | 256 bits (SHA3-256 output)             |
| Timestamp format           | Milliseconds since Unix epoch (uint64) |
| Replication factor (R)     | `min(3, network_size)`                 |
| Name PoW difficulty        | 28 leading zero bits                   |
| Contact request PoW        | 16 leading zero bits                   |
| Contact request max drift  | 1 hour (3,600,000 ms)                  |
| Name regex                 | `^[a-z0-9]{3,36}$`                     |
| Max profile size           | 1 MiB                                  |
| Max message blob           | 50 MiB                                 |
| Inline threshold (WS)      | 64 KiB                                 |
| Chunk size (WS binary)     | 1 MiB (1,048,576 bytes)                |
| Chunked upload timeout     | 30 seconds                             |
| Max contact request blob   | 64 KiB                                 |
| Message TTL                | 7 days                                 |
| TTL sweep interval         | 5 minutes                              |
| Pending STORE timeout      | 30 seconds                             |
| Responsibility transfer    | 60 seconds (on routing table change)   |
| Active sync interval       | 120 seconds                            |
| Rate limit bucket size     | 50 tokens                              |
| Rate limit refill rate     | 10 tokens/second                       |
| Max TCP message            | 50 MiB (implementation limit)          |
| Routing table max nodes    | 256                                    |
| TCP connect timeout        | 5 seconds                              |
| TCP read timeout           | 5 seconds                              |
| TCP conn pool max idle     | 60 seconds                             |
| TCP conn pool max size     | 64 connections                         |
| Max TCP clients            | 256                                    |
| TCP encryption probe byte  | `0xCE`                                 |
| TCP encryption cipher 0x01 | ChaCha20-Poly1305 + ML-KEM-1024       |
| TCP i2r key prefix         | `"chromatin:tcp:i2r:"`                 |
| TCP r2i key prefix         | `"chromatin:tcp:r2i:"`                 |
| Worker pool threads        | 4                                      |
| Worker pool max queue      | 1024 jobs                              |
| Repl_log compact interval  | 1 hour                                 |
| Repl_log compact keep      | 10,000 entries per key                 |
| Auth nonce prefix          | `"chromatin-auth:"`                    |
| Allowlist signature prefix | `"chromatin:allowlist:"`               |
| Contact req PoW prefix     | `"chromatin:request:"`                 |
| Name PoW prefix            | `"chromatin:name:"`                    |
| Default TCP port           | 4000                                   |
| Default WebSocket port     | 4001                                   |
| MDBX max database size     | 1 GiB                                  |
| Bootstrap nodes            | `0.bootstrap.cpunk.io`                 |
|                            | `1.bootstrap.cpunk.io`                 |
|                            | `2.bootstrap.cpunk.io`                 |

---

## 8. Key Computation

All storage keys are derived using SHA3-256 with domain prefixes:

```
profile_key  = SHA3-256("profile:"       || fingerprint)
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

## 9. Extensibility

The protocol is designed for future extension:

- **Data types:** Values 0x05-0xFF in the STORE `data_type` field are reserved
  for future use (groups, channels, and any new data kind).
- **Profile fields:** New fields can be appended to the profile format in future
  protocol versions. The `sequence` + signature mechanism handles versioned
  updates.
- **Protocol version:** The version byte in the TCP header allows breaking
  changes. Nodes reject unknown versions, and implementations can support
  multiple versions simultaneously.
- **Message types:** Values 0x0C-0xFF are reserved for future TCP message types.
