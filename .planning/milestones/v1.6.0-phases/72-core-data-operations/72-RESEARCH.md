# Phase 72: Core Data Operations - Research

**Researched:** 2026-03-29
**Domain:** Python SDK blob lifecycle (write, read, delete, list, exists) over PQ-encrypted relay
**Confidence:** HIGH

## Summary

Phase 72 adds five public methods to `ChromatinClient` (write_blob, read_blob, delete_blob, list_blobs, exists) plus a new `chromatindb.types` module for typed result dataclasses. The transport layer (Phase 71) already provides `Transport.send_request(msg_type, payload) -> (msg_type, payload)` with automatic request_id correlation and AEAD encryption. Each operation follows the same pattern: encode a binary payload, call send_request with the correct message type, parse the binary response payload, return a typed result.

The wire formats are all fixed-layout binary (not FlatBuffers) except for Data/Delete messages which use FlatBuffer-encoded Blob payloads. All multi-byte integers in wire payloads are big-endian, except the canonical signing input where ttl is LE uint32 and timestamp is LE uint64. The blob hash is computed server-side as SHA3-256 of the full FlatBuffer-encoded blob bytes; the SDK must use the server-returned hash from WriteAck, not compute its own (FlatBuffers encoding is not deterministic cross-language).

**Primary recommendation:** Build a single `_codec.py` module with encode/decode functions for each message type's binary payload, keeping `client.py` focused on high-level API. Use `struct.pack`/`struct.unpack` for all binary encoding/decoding. Test with unit tests mocking Transport.send_request, plus integration tests against the KVM relay.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Full convenience -- user provides `(data, ttl)`. SDK auto-generates timestamp via `int(time.time())`, builds canonical signing input, signs with identity, encodes blob as FlatBuffer, sends as Data message, parses WriteAck.
- **D-02:** TTL is required (no default). Prevents accidental permanent blobs or surprise expiry. `write_blob(data=b"...", ttl=3600)`.
- **D-03:** Namespace is implicit from connected identity. SDK derives `sha3_256(identity.public_key)` -- user never passes namespace to write. Matches security model (can only write to own namespace).
- **D-04:** Namespace is always explicit for read/list/exists. User passes namespace bytes for every call. Honest -- you can read any namespace on the node, not just your own.
- **D-05:** Bytes only for namespace and hash arguments. No hex string convenience. Consistent with crypto layer (`identity.namespace` returns bytes).
- **D-06:** Cursor-based pagination for list_blobs. Returns `ListPage` with blobs + cursor. Pass cursor to next call via `after=` parameter. Maps to C++ seq_num-based pagination.
- **D-07:** Typed frozen dataclasses for all results: `WriteResult`, `ReadResult`, `DeleteResult`, `BlobRef`, `ListPage`. Located in new `chromatindb.types` module. Named fields, IDE autocomplete, self-documenting.
- **D-08:** `WriteResult(blob_hash: bytes, seq_num: int, duplicate: bool)`
- **D-09:** `ReadResult(data: bytes, ttl: int, timestamp: int, signature: bytes)` -- full blob metadata
- **D-10:** `BlobRef(blob_hash: bytes, seq_num: int)` -- used in ListPage entries
- **D-11:** `ListPage(blobs: list[BlobRef], cursor: int | None)` -- cursor is None when no more pages
- **D-12:** Opaque tombstone -- user calls `delete_blob(blob_hash)`. SDK internally builds tombstone data (4-byte magic `0xDEADBEEF` + 32-byte target hash), signs it, sends as Delete message (type 17), parses DeleteAck (type 18). User never sees tombstone construction.
- **D-13:** Returns `DeleteResult(tombstone_hash: bytes, seq_num: int, duplicate: bool)`.
- **D-14:** `read_blob` returns `None` for not-found (normal case, not exceptional). `exists` returns `bool`. Python idiom -- missing data is not an error.
- **D-15:** Single `ProtocolError` for actual failures (write rejected, unexpected response type, connection closed mid-request). No granular subtypes for v1 -- node doesn't send rich error codes.
- **D-16:** Per-request timeout (configurable on ChromatinClient, default 10s). Raises `ConnectionError` on timeout. Prevents indefinite hangs from dropped requests.

### Claude's Discretion
- Internal payload encoding/decoding helper organization (single `_codec.py` or inline in client methods)
- Blob FlatBuffer encoding for Data/Delete messages (reuse existing `wire.py` patterns)
- Test fixture structure for integration tests
- Whether `DeleteResult` duplicates `WriteResult` fields or uses its own type
- `ListPage` exact field naming

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| DATA-01 | SDK writes signed blobs (build canonical signing input, ML-DSA-87 sign, send Data message) | Blob FlatBuffer encoding via blob_generated.py, signing via crypto.build_signing_input + identity.sign, Data message type=8, WriteAck response parsing (41-byte binary) |
| DATA-02 | SDK reads blobs by namespace + hash (ReadRequest/ReadResponse) | ReadRequest payload: [ns:32][hash:32] (64 bytes), ReadResponse: [found:1][FlatBuffer blob...] or [0x00] (1 byte) |
| DATA-03 | SDK deletes blobs by owner via tombstone (Delete/DeleteAck) | Tombstone data: 0xDEADBEEF + target_hash (36 bytes), Delete message type=17, DeleteAck type=18 (41-byte binary, same format as WriteAck) |
| DATA-04 | SDK lists blobs in a namespace with pagination (ListRequest/ListResponse) | ListRequest: [ns:32][since_seq_be:8][limit_be:4] (44 bytes), ListResponse: [count_be:4][[hash:32][seq_be:8]*count][has_more:1] |
| DATA-05 | SDK checks blob existence without data transfer (ExistsRequest/ExistsResponse) | ExistsRequest: [ns:32][hash:32] (64 bytes), ExistsResponse: [exists:1][hash:32] (33 bytes) |
| DATA-06 | SDK sends keepalive (Ping/Pong) | Already implemented in Phase 71 via Transport.send_ping() -- just needs marking as complete |
</phase_requirements>

## Standard Stack

### Core (already installed)
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| flatbuffers | ~=25.12 | Blob FlatBuffer encoding (Data/Delete payloads) | Already in pyproject.toml, used by wire.py |
| liboqs-python | ~=0.14.0 | ML-DSA-87 signing | Already in pyproject.toml, used by identity.py |
| pynacl | ~=1.5.0 | ChaCha20-Poly1305 AEAD | Already in pyproject.toml, used by crypto.py |

### Supporting (already installed)
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| pytest | (dev dep) | Unit + integration tests | All test files |
| pytest-asyncio | (dev dep) | async test support (asyncio_mode=auto) | All async test functions |

### No New Dependencies
This phase uses only stdlib (`struct`, `time`, `dataclasses`) plus already-installed packages. No new pip installs needed.

## Architecture Patterns

### Recommended Project Structure
```
sdk/python/chromatindb/
    __init__.py         # Add types module exports
    client.py           # Add write_blob, read_blob, delete_blob, list_blobs, exists
    types.py            # NEW: WriteResult, ReadResult, DeleteResult, BlobRef, ListPage
    _codec.py           # NEW: encode/decode helpers for each message payload
    _transport.py       # Unchanged (send_request already works)
    crypto.py           # Unchanged (build_signing_input already works)
    identity.py         # Unchanged (sign already works)
    wire.py             # Unchanged (TransportMsgType, encode/decode_transport_message)
    generated/
        blob_generated.py       # Blob FlatBuffer (encode Data/Delete payloads)
        transport_generated.py  # TransportMsgType constants
```

### Pattern 1: Codec Module for Binary Payloads
**What:** Single `_codec.py` module with paired `encode_*` / `decode_*` functions for each message type's binary payload. Uses `struct.pack`/`struct.unpack` for fixed-layout binary fields.
**When to use:** Every request-response pair needs payload encoding and response decoding.
**Example:**
```python
import struct
from chromatindb.exceptions import ProtocolError

def encode_read_request(namespace: bytes, blob_hash: bytes) -> bytes:
    """Encode ReadRequest payload: [namespace:32][blob_hash:32]."""
    if len(namespace) != 32:
        raise ValueError(f"namespace must be 32 bytes, got {len(namespace)}")
    if len(blob_hash) != 32:
        raise ValueError(f"blob_hash must be 32 bytes, got {len(blob_hash)}")
    return namespace + blob_hash

def decode_write_ack(payload: bytes) -> tuple[bytes, int, bool]:
    """Decode WriteAck payload: [blob_hash:32][seq_num_be:8][status:1].
    Returns (blob_hash, seq_num, duplicate).
    """
    if len(payload) != 41:
        raise ProtocolError(f"WriteAck must be 41 bytes, got {len(payload)}")
    blob_hash = payload[:32]
    seq_num = struct.unpack(">Q", payload[32:40])[0]
    duplicate = payload[40] == 1
    return blob_hash, seq_num, duplicate
```

### Pattern 2: Blob FlatBuffer Encoding for Data/Delete
**What:** Encode blob payloads as FlatBuffer using `blob_generated.py` builder functions, matching C++ `encode_blob()` with `ForceDefaults`. The blob contains namespace_id, pubkey, data, ttl, timestamp, and signature.
**When to use:** Data (type 8) and Delete (type 17) messages.
**Example:**
```python
import flatbuffers
from chromatindb.generated.blob_generated import (
    BlobStart, BlobEnd,
    BlobAddNamespaceId, BlobAddPubkey, BlobAddData,
    BlobAddTtl, BlobAddTimestamp, BlobAddSignature,
)

def encode_blob(
    namespace_id: bytes, pubkey: bytes, data: bytes,
    ttl: int, timestamp: int, signature: bytes,
) -> bytes:
    """Encode a Blob FlatBuffer matching C++ encode_blob() with ForceDefaults."""
    estimated_size = len(data) + 8192
    builder = flatbuffers.Builder(estimated_size)
    builder.ForceDefaults(True)  # Match C++ deterministic encoding

    # CreateByteVector for each vector field (must be created before StartObject)
    ns_vec = builder.CreateByteVector(namespace_id)
    pk_vec = builder.CreateByteVector(pubkey)
    dt_vec = builder.CreateByteVector(data)
    sg_vec = builder.CreateByteVector(signature)

    BlobStart(builder)
    BlobAddNamespaceId(builder, ns_vec)
    BlobAddPubkey(builder, pk_vec)
    BlobAddData(builder, dt_vec)
    BlobAddTtl(builder, ttl)
    BlobAddTimestamp(builder, timestamp)
    BlobAddSignature(builder, sg_vec)
    blob = BlobEnd(builder)
    builder.Finish(blob)
    return bytes(builder.Output())
```

### Pattern 3: Blob FlatBuffer Decoding for ReadResponse
**What:** Decode FlatBuffer blob from ReadResponse payload (byte 0 is found flag, rest is FlatBuffer).
**When to use:** ReadResponse parsing.
**Example:**
```python
from chromatindb.generated.blob_generated import Blob

def decode_blob_from_response(payload: bytes) -> tuple[bytes, int, int, bytes] | None:
    """Decode ReadResponse. Returns (data, ttl, timestamp, signature) or None."""
    if len(payload) < 1:
        raise ProtocolError("empty ReadResponse")
    if payload[0] == 0x00:
        return None  # not found
    if payload[0] != 0x01:
        raise ProtocolError(f"unexpected ReadResponse flag: {payload[0]:#x}")

    fb_bytes = payload[1:]
    blob = Blob.GetRootAs(fb_bytes, 0)

    data_len = blob.DataLength()
    data = bytes(blob.Data(j) for j in range(data_len))

    ttl = blob.Ttl()
    timestamp = blob.Timestamp()

    sig_len = blob.SignatureLength()
    signature = bytes(blob.Signature(j) for j in range(sig_len))

    return data, ttl, timestamp, signature
```

### Pattern 4: Client Methods Delegate to Codec + Transport
**What:** Each public method in ChromatinClient: validates args, encodes payload via _codec, calls send_request, checks response type, decodes via _codec, wraps in typed result.
**When to use:** All five new public methods.
**Example:**
```python
async def read_blob(self, namespace: bytes, blob_hash: bytes) -> ReadResult | None:
    """Read a blob by namespace + hash. Returns None if not found."""
    payload = encode_read_request(namespace, blob_hash)
    resp_type, resp_payload = await asyncio.wait_for(
        self._transport.send_request(TransportMsgType.ReadRequest, payload),
        timeout=self._timeout,
    )
    if resp_type != TransportMsgType.ReadResponse:
        raise ProtocolError(f"expected ReadResponse, got {resp_type}")
    result = decode_read_response(resp_payload)
    if result is None:
        return None
    data, ttl, timestamp, signature = result
    return ReadResult(data=data, ttl=ttl, timestamp=timestamp, signature=signature)
```

### Anti-Patterns to Avoid
- **Computing blob_hash locally:** FlatBuffers encoding is NOT deterministic cross-language. The C++ node computes SHA3-256(encoded_flatbuffer) -- Python flatbuffers may produce a different byte layout for the same logical content. Always use the server-returned blob_hash from WriteAck.
- **Forgetting ForceDefaults(True):** Without ForceDefaults, FlatBuffers omits zero-value fields (e.g., ttl=0 for tombstones). The C++ node uses ForceDefaults(true), so the Python encoder must too.
- **Wrong endianness:** Wire payload integers are big-endian (`>` in struct format). Signing input ttl/timestamp are little-endian (`<` in struct format). Mixing these up produces wrong hashes or corrupted values.
- **Passing namespace to write:** The security model requires namespace = SHA3-256(pubkey). The SDK derives this automatically from the identity. User-supplied namespaces for write would be a security hole.
- **Using Data message type for delete:** Delete uses type 17, not type 8. Both carry FlatBuffer-encoded Blob payloads, but the node routes them differently. Delete response is DeleteAck (type 18), not WriteAck (type 30).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| FlatBuffer blob encoding | Custom byte packing for blob fields | blob_generated.py builder + ForceDefaults | Schema evolution, field ordering, alignment handled by FlatBuffers |
| Canonical signing input | Manual hash construction | crypto.build_signing_input() | Already validated against C++ test vectors, byte-identical |
| ML-DSA-87 signing | Custom signing code | identity.sign() | Already wraps liboqs-python correctly |
| Request-response correlation | Manual request_id tracking | Transport.send_request() | Handles request_id assignment, future management, error propagation |
| AEAD encrypted framing | Manual encrypt/send/recv/decrypt | Transport layer (Phase 71) | Full AEAD frame IO with nonce management already working |

**Key insight:** Phase 72 is primarily a payload encoding/decoding layer on top of the already-working Phase 71 transport. The complex crypto and network code is done.

## Common Pitfalls

### Pitfall 1: WriteAck vs DeleteAck Message Types
**What goes wrong:** Using WriteAck (type 30) when expecting DeleteAck (type 18), or vice versa.
**Why it happens:** Both WriteAck and DeleteAck have identical 41-byte payloads `[hash:32][seq_be:8][status:1]`. The only difference is the message type.
**How to avoid:** Each client method must check `resp_type` matches the expected response type. write_blob expects WriteAck (30), delete_blob expects DeleteAck (18).
**Warning signs:** Tests pass with wrong type check because payload parsing works either way.

### Pitfall 2: Tombstone TTL Must Be 0
**What goes wrong:** Setting a non-zero TTL on tombstone blobs.
**Why it happens:** Regular blobs require non-zero TTL (per D-02), so it's natural to pass the same TTL to delete. But tombstones are permanent.
**How to avoid:** Hard-code ttl=0 in delete_blob's internal tombstone construction. The user only provides blob_hash, not TTL.
**Warning signs:** Node may reject tombstones with non-zero TTL, or tombstones may expire.

### Pitfall 3: List Pagination Cursor Off-By-One
**What goes wrong:** Using the wrong seq_num as the cursor for the next page, causing duplicated or skipped entries.
**Why it happens:** C++ uses `get_blob_refs_since(ns, since_seq, limit+1)` -- "since" is exclusive (returns refs with seq > since_seq). The cursor must be the last seq_num in the current page.
**How to avoid:** Set cursor = last BlobRef's seq_num in the current page. Pass it as `since_seq` in the next ListRequest.
**Warning signs:** First page works but subsequent pages miss the first item or include the last item of the previous page.

### Pitfall 4: ReadResponse FlatBuffer Offset
**What goes wrong:** Parsing the FlatBuffer blob starting at byte 0 of the response instead of byte 1.
**Why it happens:** ReadResponse payload is `[found:1][flatbuffer_blob...]`. The first byte is the found flag. The FlatBuffer starts at offset 1.
**How to avoid:** Slice `payload[1:]` before passing to `Blob.GetRootAs()`.
**Warning signs:** FlatBuffer decode fails with "invalid buffer" error.

### Pitfall 5: Per-Request Timeout Must Wrap send_request
**What goes wrong:** Requests hang indefinitely if the node never responds (dropped by relay, node crash, etc.).
**Why it happens:** Transport.send_request awaits a Future that the reader loop resolves. If no response arrives, the Future never resolves.
**How to avoid:** Wrap each send_request call with `asyncio.wait_for(send_request(...), timeout=self._timeout)`. Catch `asyncio.TimeoutError` and raise `ConnectionError` per D-16.
**Warning signs:** Tests pass but production hangs on dropped requests.

### Pitfall 6: Namespace Argument Validation
**What goes wrong:** Passing a non-32-byte namespace to read/list/exists, producing a malformed request that the node rejects (or worse, silent strike and disconnect).
**Why it happens:** Namespace is `bytes`, no type system enforcement on length.
**How to avoid:** Validate `len(namespace) == 32` at the start of every method that accepts namespace. Validate `len(blob_hash) == 32` similarly. Raise ValueError immediately.
**Warning signs:** Node records a strike and silently closes connection after several bad requests.

## Code Examples

Verified patterns from existing codebase:

### Encoding a Blob FlatBuffer (from C++ codec.cpp, Python equivalent)
```python
# Source: db/wire/codec.cpp encode_blob() + db/schemas/blob.fbs
# Python equivalent using blob_generated.py
import flatbuffers
from chromatindb.generated.blob_generated import (
    BlobStart, BlobEnd,
    BlobAddNamespaceId, BlobAddPubkey, BlobAddData,
    BlobAddTtl, BlobAddTimestamp, BlobAddSignature,
)

def encode_blob(
    namespace_id: bytes, pubkey: bytes, data: bytes,
    ttl: int, timestamp: int, signature: bytes,
) -> bytes:
    estimated_size = len(data) + 8192
    builder = flatbuffers.Builder(estimated_size)
    builder.ForceDefaults(True)

    ns_vec = builder.CreateByteVector(namespace_id)
    pk_vec = builder.CreateByteVector(pubkey)
    dt_vec = builder.CreateByteVector(data)
    sg_vec = builder.CreateByteVector(signature)

    BlobStart(builder)
    BlobAddNamespaceId(builder, ns_vec)
    BlobAddPubkey(builder, pk_vec)
    BlobAddData(builder, dt_vec)
    BlobAddTtl(builder, ttl)
    BlobAddTimestamp(builder, timestamp)
    BlobAddSignature(builder, sg_vec)
    blob = BlobEnd(builder)
    builder.Finish(blob)
    return bytes(builder.Output())
```

### Decoding a Blob FlatBuffer (from C++ codec.cpp)
```python
# Source: db/wire/codec.cpp decode_blob()
from chromatindb.generated.blob_generated import Blob

def decode_blob(fb_bytes: bytes) -> dict:
    blob = Blob.GetRootAs(fb_bytes, 0)
    return {
        "data": bytes(blob.Data(j) for j in range(blob.DataLength())),
        "ttl": blob.Ttl(),
        "timestamp": blob.Timestamp(),
        "signature": bytes(blob.Signature(j) for j in range(blob.SignatureLength())),
    }
```

### Tombstone Data Construction (from C++ codec.cpp)
```python
# Source: db/wire/codec.cpp make_tombstone_data()
TOMBSTONE_MAGIC = b"\xDE\xAD\xBE\xEF"

def make_tombstone_data(target_hash: bytes) -> bytes:
    assert len(target_hash) == 32
    return TOMBSTONE_MAGIC + target_hash
```

### WriteAck Parsing (from C++ peer_manager.cpp line ~1627)
```python
# Source: db/peer/peer_manager.cpp Data handler WriteAck encoding
import struct

def decode_write_ack(payload: bytes) -> tuple[bytes, int, bool]:
    """Returns (blob_hash, seq_num, duplicate)."""
    if len(payload) != 41:
        raise ProtocolError(f"WriteAck payload must be 41 bytes, got {len(payload)}")
    blob_hash = payload[:32]
    seq_num = struct.unpack(">Q", payload[32:40])[0]
    duplicate = payload[40] == 1
    return blob_hash, seq_num, duplicate
```

### ListRequest Encoding (from C++ peer_manager.cpp line ~750)
```python
# Source: db/peer/peer_manager.cpp ListRequest handler (decoding side shows format)
import struct

def encode_list_request(namespace: bytes, since_seq: int, limit: int) -> bytes:
    """Encode: [namespace:32][since_seq_be:8][limit_be:4] = 44 bytes."""
    return namespace + struct.pack(">Q", since_seq) + struct.pack(">I", limit)
```

### ListResponse Parsing (from C++ peer_manager.cpp line ~776)
```python
# Source: db/peer/peer_manager.cpp ListResponse encoding
import struct

def decode_list_response(payload: bytes) -> tuple[list[tuple[bytes, int]], bool]:
    """Returns (list of (blob_hash, seq_num), has_more)."""
    if len(payload) < 5:
        raise ProtocolError(f"ListResponse too short: {len(payload)} bytes")
    count = struct.unpack(">I", payload[:4])[0]
    expected_len = 4 + count * 40 + 1
    if len(payload) != expected_len:
        raise ProtocolError(
            f"ListResponse size mismatch: expected {expected_len}, got {len(payload)}"
        )
    entries = []
    for i in range(count):
        off = 4 + i * 40
        blob_hash = payload[off:off + 32]
        seq_num = struct.unpack(">Q", payload[off + 32:off + 40])[0]
        entries.append((blob_hash, seq_num))
    has_more = payload[-1] == 1
    return entries, has_more
```

## Wire Format Reference

Authoritative binary formats verified against C++ source (db/peer/peer_manager.cpp):

### Data Message (type 8) -- Write
- **Payload:** FlatBuffer-encoded Blob (namespace_id, pubkey, data, ttl, timestamp, signature)
- **Response:** WriteAck (type 30): `[blob_hash:32][seq_num_be:8][status:1]` (41 bytes)
  - status: 0 = stored (new), 1 = duplicate
- **Error responses:** StorageFull (type 22, empty payload), QuotaExceeded (type 25, empty payload)

### Delete Message (type 17) -- Delete
- **Payload:** FlatBuffer-encoded Blob where data = tombstone (0xDEADBEEF + target_hash, 36 bytes), ttl = 0
- **Response:** DeleteAck (type 18): `[blob_hash:32][seq_num_be:8][status:1]` (41 bytes)
  - Same format as WriteAck

### ReadRequest (type 31)
- **Payload:** `[namespace:32][blob_hash:32]` (64 bytes)
- **Response:** ReadResponse (type 32): `[found:1][FlatBuffer Blob...]` or `[not_found:0x00]` (1 byte)

### ListRequest (type 33)
- **Payload:** `[namespace:32][since_seq_be:8][limit_be:4]` (44 bytes)
- **Response:** ListResponse (type 34): `[count_be:4][[hash:32][seq_be:8]*count][has_more:1]`
  - Node caps limit at 100, fetches limit+1 to detect has_more

### ExistsRequest (type 37)
- **Payload:** `[namespace:32][blob_hash:32]` (64 bytes)
- **Response:** ExistsResponse (type 38): `[exists:1][blob_hash:32]` (33 bytes)

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| SDK not available (C++ node only) | Python SDK with PQ crypto | v1.6.0 (in progress) | External clients can write/read/delete/list/check blobs |
| Direct TCP to node | Relay-mediated connections | v1.2.0 | Clients connect through relay, relay blocks peer-internal message types |

**Already stable/locked:**
- Wire formats are frozen (v1.3.0+)
- FlatBuffer schema is frozen
- Signing input format is frozen (hash-then-sign)
- Transport.send_request API is stable (Phase 71)

## Open Questions

1. **StorageFull / QuotaExceeded error handling for write_blob**
   - What we know: Node can respond to Data with StorageFull (type 22) or QuotaExceeded (type 25) instead of WriteAck. Both have empty payloads.
   - What's unclear: Whether relay forwards these error types to the client (relay uses blocklist, so they should pass through).
   - Recommendation: Handle them in write_blob by raising ProtocolError with descriptive message. Integration test will confirm relay forwarding.

2. **DATA-06 (Ping/Pong) is already implemented**
   - What we know: Transport.send_ping() was implemented in Phase 71 and tested in integration tests (test_ping_pong).
   - What's unclear: Nothing -- it works.
   - Recommendation: Mark DATA-06 as complete. No additional work needed beyond verifying the existing implementation is exposed via ChromatinClient.ping().

3. **Identity access in client methods**
   - What we know: ChromatinClient stores `self._identity` (set in connect()), which provides namespace, public_key, and sign().
   - What's unclear: Nothing technically, but write_blob and delete_blob need access to the identity for namespace derivation and signing.
   - Recommendation: Verify `self._identity` is accessible in client methods (it's set in connect() before __aenter__).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest + pytest-asyncio |
| Config file | sdk/python/pyproject.toml [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && python3 -m pytest tests/test_types.py tests/test_codec.py -x` |
| Full suite command | `cd sdk/python && python3 -m pytest tests/ -x` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| DATA-01 | write_blob signs, sends Data, parses WriteAck | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_blob tests/test_codec.py::test_decode_write_ack tests/test_client_ops.py::test_write_blob -x` | No -- Wave 0 |
| DATA-02 | read_blob sends ReadRequest, parses ReadResponse | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_read_request tests/test_codec.py::test_decode_read_response tests/test_client_ops.py::test_read_blob -x` | No -- Wave 0 |
| DATA-03 | delete_blob builds tombstone, sends Delete, parses DeleteAck | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_tombstone tests/test_codec.py::test_decode_delete_ack tests/test_client_ops.py::test_delete_blob -x` | No -- Wave 0 |
| DATA-04 | list_blobs sends ListRequest with pagination, parses ListResponse | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_list_request tests/test_codec.py::test_decode_list_response tests/test_client_ops.py::test_list_blobs -x` | No -- Wave 0 |
| DATA-05 | exists sends ExistsRequest, parses ExistsResponse bool | unit + integration | `python3 -m pytest tests/test_codec.py::test_encode_exists_request tests/test_codec.py::test_decode_exists_response tests/test_client_ops.py::test_exists -x` | No -- Wave 0 |
| DATA-06 | Ping/Pong keepalive | integration | `python3 -m pytest tests/test_integration.py::test_ping_pong -x` | Yes -- already passing |

### Sampling Rate
- **Per task commit:** `cd sdk/python && python3 -m pytest tests/test_types.py tests/test_codec.py tests/test_client_ops.py -x`
- **Per wave merge:** `cd sdk/python && python3 -m pytest tests/ -x`
- **Phase gate:** Full suite green + integration tests against KVM relay

### Wave 0 Gaps
- [ ] `tests/test_types.py` -- covers WriteResult, ReadResult, DeleteResult, BlobRef, ListPage dataclasses
- [ ] `tests/test_codec.py` -- covers encode/decode for all 5 message payload pairs + blob FlatBuffer
- [ ] `tests/test_client_ops.py` -- covers ChromatinClient.write_blob/read_blob/delete_blob/list_blobs/exists (mock transport)
- [ ] `tests/test_integration.py` additions -- covers DATA-01 through DATA-05 against live KVM relay

## Sources

### Primary (HIGH confidence)
- `db/peer/peer_manager.cpp` -- Authoritative wire format for all 5 request-response pairs (verified line-by-line)
- `db/wire/codec.cpp` + `db/wire/codec.h` -- FlatBuffer blob encode/decode and tombstone construction
- `db/schemas/blob.fbs` -- FlatBuffer Blob schema (6 fields)
- `sdk/python/chromatindb/_transport.py` -- Transport.send_request() API
- `sdk/python/chromatindb/crypto.py` -- build_signing_input() verified against C++ test vectors
- `sdk/python/chromatindb/identity.py` -- Identity.sign(), .namespace, .public_key
- `sdk/python/chromatindb/generated/blob_generated.py` -- Blob FlatBuffer builder functions
- `sdk/python/chromatindb/generated/transport_generated.py` -- TransportMsgType enum values

### Secondary (MEDIUM confidence)
- `.planning/phases/72-core-data-operations/72-CONTEXT.md` -- User decisions (all verified against C++ source)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, everything already installed and validated in Phase 70/71
- Architecture: HIGH -- all wire formats verified against authoritative C++ source, all building blocks exist
- Pitfalls: HIGH -- identified from actual C++ implementation details (endianness, tombstone TTL, FlatBuffer non-determinism, response type checking)

**Research date:** 2026-03-29
**Valid until:** Indefinite (wire formats frozen, SDK architecture stable)
