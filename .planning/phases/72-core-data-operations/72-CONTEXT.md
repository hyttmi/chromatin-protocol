# Phase 72: Core Data Operations - Context

**Gathered:** 2026-03-29
**Status:** Ready for planning

<domain>
## Phase Boundary

SDK performs complete blob lifecycle (write, read, delete, list, exists) against a live relay. Adds public methods to ChromatinClient that build on the Phase 71 transport layer (send_request/response correlation). Includes payload encoding/decoding for 5 request-response pairs, typed result dataclasses, and integration tests against the KVM swarm. No extended queries, no pub/sub, no batch operations -- those are Phase 73.

</domain>

<decisions>
## Implementation Decisions

### Write API
- **D-01:** Full convenience -- user provides `(data, ttl)`. SDK auto-generates timestamp via `int(time.time())`, builds canonical signing input, signs with identity, encodes blob as FlatBuffer, sends as Data message, parses WriteAck.
- **D-02:** TTL is required (no default). Prevents accidental permanent blobs or surprise expiry. `write_blob(data=b"...", ttl=3600)`.
- **D-03:** Namespace is implicit from connected identity. SDK derives `sha3_256(identity.public_key)` -- user never passes namespace to write. Matches security model (can only write to own namespace).

### Read/List/Exists API
- **D-04:** Namespace is always explicit for read/list/exists. User passes namespace bytes for every call. Honest -- you can read any namespace on the node, not just your own.
- **D-05:** Bytes only for namespace and hash arguments. No hex string convenience. Consistent with crypto layer (`identity.namespace` returns bytes).
- **D-06:** Cursor-based pagination for list_blobs. Returns `ListPage` with blobs + cursor. Pass cursor to next call via `after=` parameter. Maps to C++ seq_num-based pagination.

### Return types
- **D-07:** Typed frozen dataclasses for all results: `WriteResult`, `ReadResult`, `DeleteResult`, `BlobRef`, `ListPage`. Located in new `chromatindb.types` module. Named fields, IDE autocomplete, self-documenting.
- **D-08:** `WriteResult(blob_hash: bytes, seq_num: int, duplicate: bool)`
- **D-09:** `ReadResult(data: bytes, ttl: int, timestamp: int, signature: bytes)` -- full blob metadata
- **D-10:** `BlobRef(blob_hash: bytes, seq_num: int)` -- used in ListPage entries
- **D-11:** `ListPage(blobs: list[BlobRef], cursor: int | None)` -- cursor is None when no more pages

### Delete semantics
- **D-12:** Opaque tombstone -- user calls `delete_blob(blob_hash)`. SDK internally builds tombstone data (4-byte magic `0xDEADBEEF` + 32-byte target hash), signs it, sends as Delete message (type 17), parses DeleteAck (type 18). User never sees tombstone construction.
- **D-13:** Returns `DeleteResult(tombstone_hash: bytes, seq_num: int, duplicate: bool)`.

### Error handling
- **D-14:** `read_blob` returns `None` for not-found (normal case, not exceptional). `exists` returns `bool`. Python idiom -- missing data is not an error.
- **D-15:** Single `ProtocolError` for actual failures (write rejected, unexpected response type, connection closed mid-request). No granular subtypes for v1 -- node doesn't send rich error codes.
- **D-16:** Per-request timeout (configurable on ChromatinClient, default 10s). Raises `ConnectionError` on timeout. Prevents indefinite hangs from dropped requests.

### Claude's Discretion
- Internal payload encoding/decoding helper organization (single `_codec.py` or inline in client methods)
- Blob FlatBuffer encoding for Data/Delete messages (reuse existing `wire.py` patterns)
- Test fixture structure for integration tests
- Whether `DeleteResult` duplicates `WriteResult` fields or uses its own type
- `ListPage` exact field naming

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Wire format protocol (binary payload encoding for each message type)
- `db/peer/peer_manager.cpp` -- Authoritative encoder for all response payloads:
  - WriteAck (line ~1627): `[blob_hash:32][seq_num_be:8][status:1]` (41 bytes, status 0=stored, 1=duplicate)
  - DeleteAck (line ~666): Same format as WriteAck
  - ReadRequest (line ~717): `[namespace:32][blob_hash:32]` (64 bytes)
  - ReadResponse (line ~730): `[found:1][encoded_blob...]` or `[not_found:0x00]` (1 byte)
  - ListRequest (line ~750): `[namespace:32][since_seq_be:8][limit_be:4]` (44 bytes)
  - ListResponse (line ~776): `[count_be:4][ [hash:32][seq_be:8] * count ][has_more:1]`
  - ExistsRequest (line ~836): `[namespace:32][blob_hash:32]` (64 bytes)
  - ExistsResponse (line ~850): `[exists:1][blob_hash:32]` (33 bytes)

### Blob FlatBuffer encoding (Data/Delete message payloads)
- `db/wire/codec.h` -- BlobData struct (namespace_id, pubkey, data, ttl, timestamp, signature), encode_blob/decode_blob, TOMBSTONE_MAGIC
- `db/wire/codec.cpp` -- FlatBuffer encode/decode with ForceDefaults(true)
- `db/schemas/blob.fbs` -- Blob table schema (namespace_id, pubkey, data, ttl, timestamp, signature)

### Canonical signing input (SDK already implements, used for write and delete)
- `db/wire/codec.cpp` -- build_signing_input(): SHA3-256(namespace || data || ttl_le32 || timestamp_le64)

### Existing SDK modules (Phase 70/71, already implemented)
- `sdk/python/chromatindb/client.py` -- ChromatinClient with connect/ping/goodbye (add write/read/delete/list/exists here)
- `sdk/python/chromatindb/_transport.py` -- Transport.send_request(msg_type, payload) -> (msg_type, payload)
- `sdk/python/chromatindb/crypto.py` -- build_signing_input(), sha3_256(), aead_encrypt/decrypt
- `sdk/python/chromatindb/identity.py` -- Identity.sign(), identity.namespace, identity.public_key
- `sdk/python/chromatindb/wire.py` -- TransportMsgType enum, encode_transport_message, decode_transport_message
- `sdk/python/chromatindb/exceptions.py` -- ProtocolError, ConnectionError already defined
- `sdk/python/chromatindb/generated/transport_generated.py` -- TransportMsgType constants (Data=8, Delete=17, DeleteAck=18, WriteAck=30, ReadRequest=31, ReadResponse=32, ListRequest=33, ListResponse=34, ExistsRequest=37, ExistsResponse=38)

### Known protocol details
- Data message payload = FlatBuffer-encoded Blob (via encode_blob)
- Delete message payload = FlatBuffer-encoded Blob where data = tombstone (4-byte magic + 32-byte target hash)
- Tombstone TTL is always 0 (permanent)
- blob_hash = SHA3-256 of the full FlatBuffer-encoded blob bytes (not the signing input)
- FlatBuffers not deterministic cross-language -- use server-returned blob_hash from WriteAck, not locally computed hash
- All multi-byte integers in wire payloads are big-endian (except signing input ttl/timestamp which are little-endian)
- List limit capped at 100 by node; has_more flag indicates more pages

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Transport.send_request(msg_type, payload)` -- request-response correlation ready, auto-assigns request_id
- `crypto.build_signing_input(namespace, data, ttl, timestamp)` -- canonical signing input, byte-identical to C++
- `identity.sign(message)` -- ML-DSA-87 signing
- `identity.namespace` -- 32-byte namespace derived from pubkey
- `identity.public_key` -- raw public key bytes
- `wire.encode_transport_message()` / `decode_transport_message()` -- FlatBuffer envelope
- `wire.TransportMsgType` -- all message type constants
- FlatBuffer Blob generated code in `chromatindb.generated` -- for encoding Data/Delete blob payloads

### Established Patterns
- Phase 71: `Transport.send_request()` sends encoded message, returns `(msg_type, payload)` tuple
- Phase 71: `send_lock` serializes outgoing frames (thread-safe sends)
- Phase 70: All crypto ops produce byte-identical output to C++
- Phase 70: `wire.py` encode uses `builder.ForceDefaults(true)` equivalent -- matching C++ deterministic encoding

### Integration Points
- ChromatinClient gains new public methods (write_blob, read_blob, delete_blob, list_blobs, exists)
- New `chromatindb.types` module for result dataclasses
- Integration tests target KVM relay at 192.168.1.200:4201 (env vars: CHROMATINDB_RELAY_HOST / CHROMATINDB_RELAY_PORT)

</code_context>

<specifics>
## Specific Ideas

- blob_hash from WriteAck is authoritative -- SDK must NOT compute its own hash and compare (FlatBuffers cross-language non-determinism)
- Tombstone magic is `0xDEADBEEF` (4 bytes) followed by 32-byte target blob hash
- Node returns WriteAck for both Data and Delete (status 0=stored, 1=duplicate); DeleteAck has identical format
- ReadResponse encodes full blob as FlatBuffer when found -- SDK must decode blob fields from it
- ListResponse seq_num in BlobRef entries serves as the pagination cursor for the next request

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 72-core-data-operations*
*Context gathered: 2026-03-29*
