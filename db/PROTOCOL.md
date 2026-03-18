# chromatindb Protocol Walkthrough

This document describes the wire protocol for connecting to and interacting with a chromatindb node. It is written for developers building a compatible client in any language. All values are described at the byte level, independent of any particular serialization library.

## Transport Layer

After the handshake completes, all communication uses AEAD-encrypted frames. Each frame has the following format:

```
[4 bytes: big-endian uint32 ciphertext_length]
[ciphertext_length bytes: AEAD ciphertext]
```

### AEAD Parameters

| Parameter | Value |
|-----------|-------|
| Algorithm | ChaCha20-Poly1305 (IETF, RFC 8439) |
| Key size | 32 bytes (derived from ML-KEM shared secret via HKDF) |
| Nonce size | 12 bytes |
| Nonce format | 4 zero bytes + 8-byte big-endian counter |
| Associated data | Empty (zero-length) |
| Tag size | 16 bytes (appended to ciphertext) |

Each direction (send and receive) maintains its own counter starting at 0. The counter increments by 1 after each frame. The maximum frame size is 110 MiB (115,343,360 bytes).

### Plaintext Format

The plaintext inside each AEAD frame is a FlatBuffers-encoded `TransportMessage`:

```
table TransportMessage {
    type: TransportMsgType;   // 1 byte enum
    payload: [ubyte];         // variable length, type-dependent
}
```

## Connection Lifecycle

### Step 1: TCP Connect

Connect to the node's `bind_address` (default port 4200) via TCP. No TLS -- the post-quantum handshake provides all transport security.

### Step 2: PQ Handshake

The handshake establishes session keys using ML-KEM-1024 for key exchange and ML-DSA-87 for mutual authentication. It consists of four messages: two raw (unencrypted) KEM messages, then two AEAD-encrypted authentication messages.

```
Initiator                              Responder
    |                                      |
    |--- [raw] KemPubkey ----------------->|  ML-KEM-1024 ephemeral public key (1568 bytes)
    |                                      |  Responder encapsulates: (ciphertext, shared_secret)
    |<-- [raw] KemCiphertext --------------|  ML-KEM-1024 ciphertext (1568 bytes)
    |                                      |  Initiator decapsulates: shared_secret
    |                                      |
    |   Both derive session keys via HKDF-SHA256:
    |     ikm    = shared_secret (ML-KEM output)
    |     salt   = SHA3-256(initiator_signing_pubkey || responder_signing_pubkey)
    |     info1  = "chromatin-init-to-resp-v1"  -->  initiator-to-responder key (32 bytes)
    |     info2  = "chromatin-resp-to-init-v1"  -->  responder-to-initiator key (32 bytes)
    |     info3  = "chromatin-session-fp-v1"    -->  session fingerprint (32 bytes)
    |                                      |
    |--- [encrypted] AuthSignature ------->|  ML-DSA-87 public key (2592 bytes)
    |                                      |  + ML-DSA-87 signature over session fingerprint
    |<-- [encrypted] AuthSignature --------|  ML-DSA-87 public key (2592 bytes)
    |                                      |  + ML-DSA-87 signature over session fingerprint
    |                                      |
    |   Session established.               |
```

**Message 1 -- KemPubkey (raw, unencrypted):** The initiator generates an ephemeral ML-KEM-1024 keypair and sends the 1568-byte public key as a `TransportMessage` with `type = KemPubkey (1)`. This message is NOT length-prefixed or encrypted -- it is sent as raw FlatBuffer bytes.

**Message 2 -- KemCiphertext (raw, unencrypted):** The responder uses the received public key to encapsulate a shared secret, producing a 1568-byte ciphertext. Sent as `TransportMessage` with `type = KemCiphertext (2)`, also raw.

**Key derivation:** Both sides now hold the same shared secret. They derive three values using HKDF-SHA256:
- **Initiator-to-responder key** (32 bytes) -- the initiator uses this as the send key; the responder uses it as the recv key
- **Responder-to-initiator key** (32 bytes) -- the reverse
- **Session fingerprint** (32 bytes) -- signed by both sides for mutual authentication

The HKDF salt is `SHA3-256(initiator_signing_pubkey || responder_signing_pubkey)`. At this point, the initiator does not yet know the responder's signing pubkey, so the responder's portion is initially unknown and filled in after the auth exchange.

**Message 3 -- AuthSignature (encrypted):** The initiator sends its ML-DSA-87 signing public key (2592 bytes) and a signature over the session fingerprint. This is the first AEAD-encrypted frame, using the initiator-to-responder key with nonce counter 0.

**Message 4 -- AuthSignature (encrypted):** The responder sends its ML-DSA-87 signing public key and signature. Encrypted with the responder-to-initiator key, nonce counter 0.

Both sides verify the peer's signature over the session fingerprint. After verification, AEAD nonce counters increment to 1 for both directions.

### Lightweight Handshake (Trusted Peers)

Connections from localhost (127.0.0.1, ::1) or addresses listed in `trusted_peers` use a simplified handshake that skips ML-KEM-1024 key exchange. This reduces connection latency for trusted LAN deployments.

```
Initiator                              Responder
    |                                      |
    |--- [raw] TrustedHello ------------->|  ML-DSA-87 pubkey (2592 bytes) + signature
    |                                      |  Responder checks trust list
    |<-- [raw] TrustedHello --------------|  ML-DSA-87 pubkey (2592 bytes) + signature
    |                                      |
    |   Both derive session keys via HKDF-SHA256:
    |     ikm    = SHA3-256(initiator_pubkey || responder_pubkey)
    |     info1  = "chromatin-init-to-resp-v1"  -->  initiator-to-responder key
    |     info2  = "chromatin-resp-to-init-v1"  -->  responder-to-initiator key
    |                                      |
    |   Session established (AEAD-encrypted from here).
```

If the responder does not recognize the initiator as trusted, it replies with `PQRequired (25)` instead of `TrustedHello`. The initiator then falls back to the full PQ handshake starting from KemPubkey.

### Step 3: Encrypted Session

All subsequent messages are AEAD-encrypted `TransportMessage` frames using the established session keys. Nonce counters continue incrementing from where the handshake left off.

If the node has `allowed_keys` configured, it checks the peer's signing public key namespace (`SHA3-256(peer_pubkey)`) against the access control list immediately after the handshake. Unauthorized peers are silently disconnected.

## Storing a Blob

### Blob Schema

The blob wire format is a FlatBuffers table with six fields:

```
table Blob {
    namespace_id: [ubyte];   // 32 bytes: SHA3-256(author's signing pubkey)
    pubkey: [ubyte];         // 2592 bytes: ML-DSA-87 signing public key
    data: [ubyte];           // variable length: application payload (max 100 MiB)
    ttl: uint32;             // seconds until expiry (protocol constant: 604800 = 7 days), 0 = permanent
    timestamp: uint64;       // author's Unix timestamp in seconds
    signature: [ubyte];      // up to 4627 bytes: ML-DSA-87 signature
}
```

### Canonical Signing Input

Blobs are signed over a canonical byte sequence, NOT over the raw FlatBuffer encoding. This makes signature verification independent of serialization format. The signing input is:

```
SHA3-256(namespace_id || data || ttl_le32 || timestamp_le64)
```

Where:
- `namespace_id` -- 32 bytes, the author's namespace
- `data` -- variable length, the blob's application payload
- `ttl_le32` -- 4 bytes, TTL in little-endian uint32
- `timestamp_le64` -- 8 bytes, timestamp in little-endian uint64

The concatenation is hashed with SHA3-256 to produce a 32-byte digest. This digest is then signed with the author's ML-DSA-87 private key.

### Sending a Data Message

To store a blob on a node:

1. Construct the canonical signing input as described above
2. Sign the SHA3-256 digest with the author's ML-DSA-87 private key
3. Build a FlatBuffers `Blob` with all six fields
4. Encode the `Blob` as FlatBuffer bytes
5. Wrap in a `TransportMessage` with `type = Data (8)` and the encoded blob as `payload`
6. Encrypt and send as an AEAD frame

The node validates the signature, checks for duplicates, verifies the namespace matches the public key, and stores the blob. If the node is at capacity, it sends a StorageFull message instead of accepting the blob.

## Retrieving Blobs (Sync Protocol)

Sync is a three-phase protocol that efficiently transfers blobs between two connected peers. Either side can initiate a sync round.

### Phase A: Namespace Exchange

```
Initiator                              Responder
    |--- SyncRequest (empty payload) ----->|
    |<-- SyncAccept (empty payload) -------|
    |--- NamespaceList ------------------->|
    |<-- NamespaceList --------------------|
```

The `NamespaceList` payload encodes all namespaces the node has blobs for, along with the latest sequence number per namespace:

```
Wire format: [count: 4 bytes BE uint32]
             [namespace_id: 32 bytes][latest_seq_num: 8 bytes BE uint64]
             [namespace_id: 32 bytes][latest_seq_num: 8 bytes BE uint64]
             ...
```

Each entry is 40 bytes (32-byte namespace ID + 8-byte sequence number). Both sides use the sequence numbers to determine which namespaces need syncing: if the peer has a higher sequence number for a namespace, that namespace has new blobs.

### Phase B: Hash Diff

For each namespace that needs syncing, both sides exchange their full list of blob hashes:

```
Wire format: [namespace_id: 32 bytes]
             [count: 4 bytes BE uint32]
             [hash: 32 bytes]
             [hash: 32 bytes]
             ...
```

Sent as a `TransportMessage` with `type = HashList (12)`. Each side computes the set difference: blob hashes the peer has that we do not. These are the blobs we need to request.

### Phase C: Blob Transfer

The requesting side sends `BlobRequest` messages, each containing up to 64 blob hashes to fetch:

```
BlobRequest wire format: [namespace_id: 32 bytes]
                         [count: 4 bytes BE uint32]
                         [hash: 32 bytes]
                         [hash: 32 bytes]
                         ...
```

The responder replies with `BlobTransfer` messages containing the requested blobs, one blob per transfer:

```
BlobTransfer wire format: [count: 4 bytes BE uint32]
                          [length: 4 bytes BE uint32][FlatBuffer-encoded Blob]
                          ...
```

Each blob is a FlatBuffers-encoded `Blob` (as described in the blob schema above). The receiving side validates each blob (signature, namespace, expiry) before storing it.

After all blob transfers complete, both sides exchange `SyncComplete` messages (empty payload, `type = SyncComplete (15)`).

Inline peer exchange (PEX) follows immediately after sync completes.

## Additional Interactions

### Blob Deletion

Namespace owners delete blobs by sending a **tombstone** -- a special blob whose data field contains a 4-byte magic prefix followed by the 32-byte hash of the target blob:

```
Tombstone data format: [0xDE 0xAD 0xBE 0xEF][target_blob_hash: 32 bytes]
                       (total: 36 bytes)
```

The tombstone is signed by the namespace owner and sent as a `TransportMessage` with `type = Delete (18)`. The payload is a FlatBuffers-encoded `Blob` where the `data` field contains the tombstone bytes. The `ttl` field is 0 (permanent).

The node responds with `DeleteAck (19)` (empty payload). Tombstones replicate via sync like regular blobs and permanently block future arrival of the deleted blob.

### Namespace Delegation

Namespace owners grant write access to other identities by creating a **delegation blob** -- a blob whose data field contains a 4-byte magic prefix followed by the delegate's ML-DSA-87 public key:

```
Delegation data format: [0xDE 0x1E 0x6A 0x7E][delegate_pubkey: 2592 bytes]
                        (total: 2596 bytes)
```

The delegation blob is signed by the namespace owner and sent as a regular `Data (8)` message. Once stored, the delegate can write blobs to the owner's namespace by signing with their own key. The node verifies that a valid delegation blob exists before accepting the delegate's writes.

Revocation is done by tombstoning the delegation blob.

### Pub/Sub Notifications

Peers subscribe to namespaces to receive real-time notifications when blobs are ingested or deleted.

**Subscribe** (`type = Subscribe (20)`): Payload contains a list of namespace IDs to subscribe to:

```
Subscribe/Unsubscribe wire format: [count: 2 bytes BE uint16]
                                   [namespace_id: 32 bytes]
                                   [namespace_id: 32 bytes]
                                   ...
```

**Unsubscribe** (`type = Unsubscribe (21)`): Same payload format. Removes the listed namespaces from the peer's subscription set.

**Notification** (`type = Notification (22)`): Sent by the node to subscribed peers when a blob is ingested or deleted. Fixed 77-byte payload:

```
Notification wire format: [namespace_id: 32 bytes]
                          [blob_hash: 32 bytes]
                          [seq_num: 8 bytes BE uint64]
                          [blob_size: 4 bytes BE uint32]
                          [is_tombstone: 1 byte (0 or 1)]
```

Subscriptions are connection-scoped and do not persist across reconnections.

### Peer Exchange (PEX)

PEX allows nodes to discover new peers without relying solely on bootstrap nodes. It runs inline after each sync round completes.

**PeerListRequest** (`type = PeerListRequest (16)`): Empty payload. Asks the peer for its known addresses.

**PeerListResponse** (`type = PeerListResponse (17)`): Contains a list of peer addresses:

```
PeerListResponse wire format: [count: 2 bytes BE uint16]
                              [addr_length: 2 bytes BE uint16][address: UTF-8 string]
                              [addr_length: 2 bytes BE uint16][address: UTF-8 string]
                              ...
```

Each address is a `host:port` string. Nodes share up to 8 addresses per response and connect to at most 3 newly discovered peers per PEX round.

### Storage Signaling

**StorageFull** (`type = StorageFull (23)`): Empty payload. Sent by a node when it has reached its configured `max_storage_bytes` limit and cannot accept more blobs. Peers receiving this message suppress sync pushes (blob transfers) to the full node until the next reconnection.

### Quota Signaling

**QuotaExceeded** (`type = QuotaExceeded (26)`): Empty payload. Sent by a node when a blob write would exceed the configured per-namespace byte or count quota. Unlike StorageFull (which signals global capacity), QuotaExceeded indicates that the specific namespace has reached its limit. Other namespaces may still accept writes.

## Message Type Reference

All 27 message types defined in the `TransportMsgType` enum:

| Value | Name | Description |
|-------|------|-------------|
| 0 | None | Reserved / unset |
| 1 | KemPubkey | ML-KEM-1024 ephemeral public key (handshake step 1) |
| 2 | KemCiphertext | ML-KEM-1024 ciphertext (handshake step 2) |
| 3 | AuthSignature | ML-DSA-87 public key + signature (handshake steps 3-4) |
| 4 | AuthPubkey | Reserved (authentication handled by AuthSignature) |
| 5 | Ping | Keepalive request (empty payload) |
| 6 | Pong | Keepalive response (empty payload) |
| 7 | Goodbye | Graceful disconnect (empty payload) |
| 8 | Data | Blob storage: FlatBuffer-encoded Blob payload |
| 9 | SyncRequest | Sync initiation (empty payload) |
| 10 | SyncAccept | Sync acceptance (empty payload) |
| 11 | NamespaceList | Sync Phase A: namespace IDs with sequence numbers |
| 12 | HashList | Sync Phase B: per-namespace blob hash list |
| 13 | BlobRequest | Sync Phase C: request blobs by hash (max 64 per message) |
| 14 | BlobTransfer | Sync Phase C: requested blob data |
| 15 | SyncComplete | Sync finished (empty payload) |
| 16 | PeerListRequest | PEX: request known peer addresses (empty payload) |
| 17 | PeerListResponse | PEX: list of known peer addresses |
| 18 | Delete | Blob deletion: FlatBuffer-encoded tombstone Blob |
| 19 | DeleteAck | Deletion acknowledgment (empty payload) |
| 20 | Subscribe | Pub/sub: subscribe to namespace notifications |
| 21 | Unsubscribe | Pub/sub: unsubscribe from namespace notifications |
| 22 | Notification | Pub/sub: blob ingested/deleted notification (77 bytes) |
| 23 | StorageFull | Capacity signaling: node at storage limit (empty payload) |
| 24 | TrustedHello | Lightweight handshake: trusted peer identity exchange (ML-DSA-87 pubkey + signature, no KEM) |
| 25 | PQRequired | Lightweight handshake rejection: responder requires full PQ handshake (empty payload) |
| 26 | QuotaExceeded | Quota signaling: namespace byte or count limit reached (empty payload) |
