# Architecture Research: Python SDK for chromatindb

**Domain:** Python SDK client library integrating with existing C++ relay/node system
**Researched:** 2026-03-29
**Confidence:** HIGH (based on direct source code analysis of relay, connection, handshake, framing, and protocol modules)

## System Overview

```
                     Python SDK (new)                           Existing C++ Infrastructure
                 ========================              ==========================================

  Application      sdk/python/
  Code             chromatindb/
                   +------------------+
                   |   client.py      |  <-- Public API: ChromatinClient
                   +--------+---------+
                            |
                   +--------v---------+
                   |   session.py     |  <-- Connection lifecycle, request pipelining
                   +--------+---------+
                            |
              +-------------+-------------+
              |                           |
     +--------v---------+     +-----------v----------+
     |   handshake.py   |     |   messages.py        |  <-- 38 message type encoders/decoders
     +--------+---------+     +-----------+----------+
              |                           |
     +--------v---------+     +-----------v----------+
     |   crypto.py      |     |   wire.py            |  <-- FlatBuffers TransportMessage codec
     +--------+---------+     +-----------+----------+
              |                           |
              +-------------+-------------+
                            |
                   +--------v---------+
                   |   transport.py   |  <-- TCP socket, length-prefixed framing, AEAD
                   +--------+---------+
                            |
                            | TCP
                            v
                   +------------------+
                   |   Relay (C++)    |  <-- PQ handshake responder, message filter, UDS forwarder
                   +--------+---------+
                            | UDS (TrustedHello)
                            v
                   +------------------+
                   |   Node (C++)     |  <-- Storage, sync, query engine
                   +------------------+
```

### Component Responsibilities

| Component | Responsibility | Integration Point |
|-----------|----------------|-------------------|
| `transport.py` | TCP socket, length-prefixed frame IO, AEAD encrypt/decrypt | Raw bytes over TCP to relay port |
| `crypto.py` | ML-KEM-1024, ML-DSA-87, SHA3-256, HKDF-SHA256, ChaCha20-Poly1305, nonce management | liboqs-python for PQ, PyNaCl for symmetric, hashlib for SHA3 |
| `handshake.py` | PQ handshake initiator state machine (KEM exchange + auth) | Sends/receives raw frames with relay's PQ responder |
| `wire.py` | FlatBuffers TransportMessage encode/decode | Must produce bytes identical to C++ TransportCodec |
| `messages.py` | Typed encoders/decoders for all 38 client message payloads | Binary wire formats matching PROTOCOL.md byte layouts |
| `session.py` | Connection lifecycle, request_id pipelining, response routing | Manages handshake -> message loop -> graceful close |
| `client.py` | Public API: connect, write, read, delete, list, query, subscribe | User-facing, hides protocol complexity |
| `identity.py` | ML-DSA-87 keypair management, load/save, namespace derivation | Key file format compatible with node's .key/.pub files |

## Recommended Project Structure

```
sdk/python/
+-- pyproject.toml            # Package metadata, dependencies, build config
+-- chromatindb/
|   +-- __init__.py           # Package root, re-exports ChromatinClient + Identity
|   +-- client.py             # Public API: ChromatinClient class
|   +-- session.py            # Connection + request pipelining + response dispatch
|   +-- handshake.py          # PQ handshake initiator (KemPubkey -> AuthSignature)
|   +-- transport.py          # TCP socket IO, length-prefixed frames, AEAD framing
|   +-- crypto.py             # All crypto primitives: KEM, signing, AEAD, KDF, hashing
|   +-- wire.py               # FlatBuffers TransportMessage codec
|   +-- messages.py           # Per-type payload encoders/decoders (38 types)
|   +-- identity.py           # ML-DSA-87 keypair: generate, load, save, namespace
|   +-- exceptions.py         # ChromatinError hierarchy
|   +-- constants.py          # Protocol constants (sizes, magic bytes, type enums)
|   +-- _generated/           # FlatBuffers generated Python code
|   |   +-- TransportMessage.py
|   |   +-- Blob.py
|   |   +-- TransportMsgType.py
|   +-- py.typed              # PEP 561 marker
+-- tests/
|   +-- test_crypto.py        # Unit tests: AEAD, KDF, nonce, signing
|   +-- test_handshake.py     # Handshake state machine tests with mock transport
|   +-- test_wire.py          # FlatBuffers encode/decode round-trip
|   +-- test_messages.py      # Payload encoder/decoder tests for all 38 types
|   +-- test_transport.py     # Frame IO tests
|   +-- test_session.py       # Connection lifecycle, pipelining
|   +-- test_client.py        # High-level API tests
|   +-- test_identity.py      # Key generation, load/save, namespace derivation
|   +-- test_interop.py       # Cross-language interop with running relay (Docker)
|   +-- conftest.py           # Shared fixtures
+-- examples/
|   +-- write_blob.py         # Minimal write example
|   +-- read_blob.py          # Read by namespace + hash
|   +-- subscribe.py          # Pub/sub notification listener
```

### Structure Rationale

- **Flat module layout:** 8 modules at package root. No nested sub-packages. The domain is narrow (one protocol, one connection type) and deep nesting adds import friction without benefit.
- **`_generated/`:** FlatBuffers codegen output isolated in a private sub-package. Generated files are committed (not generated at build time) because the `.fbs` schemas live in `db/schemas/` and the SDK user should not need `flatc` installed.
- **`tests/` at package level:** Follows pytest conventions. `test_interop.py` requires a running relay (Docker) and is skipped in normal test runs.
- **Separation of `crypto.py` from `handshake.py`:** Crypto module is pure primitives (encrypt, decrypt, sign, verify, hash, derive). Handshake module is protocol state machine logic that calls crypto. This keeps crypto testable with known test vectors.
- **Separation of `wire.py` from `messages.py`:** Wire handles FlatBuffers TransportMessage envelope (type + payload + request_id). Messages handles the inner payload bytes for each specific message type. Different layers, different concerns.

## Architectural Patterns

### Pattern 1: Counter-Based AEAD Nonce Management

**What:** Each direction (send/recv) has an independent uint64 counter starting at 0. Nonce = 4 zero bytes + 8-byte big-endian counter. Counter increments after every frame, including handshake auth messages.

**Why critical:** The relay's C++ connection uses `send_counter_` and `recv_counter_` (connection.h:158-159). The SDK is the initiator, so its send counter maps to the relay's recv counter and vice versa. A single missed or double-counted frame causes AEAD decryption failure and connection death.

**Implementation detail from source code:**

```python
def make_nonce(counter: int) -> bytes:
    """4 zero bytes + 8-byte big-endian counter = 12-byte AEAD nonce."""
    return b'\x00\x00\x00\x00' + counter.to_bytes(8, 'big')
```

**Counter lifecycle:**
1. Both send_counter and recv_counter start at 0
2. During PQ handshake, the initiator's first encrypted send (AuthSignature) uses send_counter=0
3. The initiator's first encrypted recv (peer's AuthSignature) uses recv_counter=0
4. After handshake completes, counters are at send=1, recv=1
5. Each subsequent send_message increments send_counter, each recv_message increments recv_counter
6. Counters NEVER reset, NEVER skip

**Trade-offs:** Simple and correct, but any frame loss or duplication is unrecoverable. The protocol has no retry/reorder mechanism -- connection must be restarted on any AEAD failure.

### Pattern 2: Length-Prefixed Frame IO (All Phases)

**What:** Every frame on the wire (including raw handshake messages) uses `[4-byte big-endian uint32 length][payload]`. After handshake, the payload is AEAD ciphertext. During handshake, the payload is raw FlatBuffer bytes.

**CRITICAL NOTE -- PROTOCOL.md discrepancy:** PROTOCOL.md states KemPubkey "is NOT length-prefixed." This is incorrect. The C++ `send_raw()` method (connection.cpp:107-123) always writes `[4-byte BE length header][data]`. The SDK MUST length-prefix all messages including handshake KEM exchange.

**Python implementation approach:**

```python
async def send_raw(self, data: bytes) -> None:
    """Send length-prefixed frame (used for both raw and encrypted)."""
    header = len(data).to_bytes(4, 'big')
    self._writer.write(header + data)
    await self._writer.drain()

async def recv_raw(self) -> bytes:
    """Receive length-prefixed frame."""
    header = await self._reader.readexactly(4)
    length = int.from_bytes(header, 'big')
    if length > MAX_FRAME_SIZE:
        raise ProtocolError(f"frame exceeds max size: {length}")
    return await self._reader.readexactly(length)
```

### Pattern 3: Request Pipelining with request_id Correlation

**What:** The client assigns a unique uint32 `request_id` to each request. The node echoes it in the response. Responses may arrive out of order because the node processes requests concurrently (thread pool offload for heavy ops).

**Implementation in session.py:**

```python
class Session:
    def __init__(self):
        self._next_request_id: int = 1
        self._pending: dict[int, asyncio.Future] = {}

    async def request(self, msg_type: int, payload: bytes) -> tuple[int, bytes]:
        """Send request and await correlated response."""
        rid = self._next_request_id
        self._next_request_id = (self._next_request_id + 1) & 0xFFFFFFFF
        future = asyncio.get_event_loop().create_future()
        self._pending[rid] = future
        await self._send_message(msg_type, payload, rid)
        return await future  # Returns (response_type, response_payload)
```

**Trade-offs:** Enables concurrent queries over a single connection. Requires a background task to dispatch received messages to pending futures by request_id. Server-initiated messages (Notification, type 21) always have request_id=0 and are dispatched to a notification callback, not the pending map.

### Pattern 4: FlatBuffers TransportMessage Envelope

**What:** All messages are wrapped in a FlatBuffers TransportMessage with three fields: `type` (byte enum), `payload` (byte vector), `request_id` (uint32). The C++ uses `ForceDefaults(true)` to ensure deterministic encoding.

**SDK approach:** Use generated Python FlatBuffers code from `flatc --python` against `db/schemas/transport.fbs`. The generated code produces `TransportMessage.py` with Builder and accessor classes.

**Encoding must match C++ exactly:**

```python
import flatbuffers
from chromatindb._generated import TransportMessage, TransportMsgType

def encode_transport(msg_type: int, payload: bytes, request_id: int = 0) -> bytes:
    builder = flatbuffers.Builder(len(payload) + 64)
    builder.ForceDefaults(True)  # CRITICAL: matches C++ ForceDefaults(true)
    payload_vec = builder.CreateByteVector(payload)
    TransportMessage.Start(builder)
    TransportMessage.AddType(builder, msg_type)
    TransportMessage.AddPayload(builder, payload_vec)
    TransportMessage.AddRequestId(builder, request_id)
    msg = TransportMessage.End(builder)
    builder.Finish(msg)
    return bytes(builder.Output())
```

**Note on `builder.Output()`:** FlatBuffers Python builder returns bytes in reverse order from C++. The `Finish()` call handles this, but `Output()` returns a bytearray that should be converted to bytes. The wire format produced is identical to C++ -- this has been verified in the FlatBuffers test suite.

## Data Flow

### PQ Handshake Flow (SDK as Initiator)

```
SDK (Python)                                Relay (C++)
    |                                           |
    |  1. Generate ephemeral ML-KEM-1024 keypair |
    |     using liboqs KeyEncapsulation           |
    |                                           |
    |-- [raw] KemPubkey ----------------------->|  TransportMessage(type=1)
    |   payload: [kem_pk:1568][signing_pk:2592] |  payload: [kem_pk:1568][signing_pk:2592]
    |   Framing: [4B length][flatbuffer bytes]  |
    |                                           |  Relay decodes, encapsulates -> shared_secret
    |<-- [raw] KemCiphertext -------------------|  TransportMessage(type=2)
    |   payload: [ciphertext:1568][signing_pk:2592]  payload: [ct:1568][relay_pk:2592]
    |                                           |
    |  2. Decapsulate -> shared_secret          |
    |  3. Derive session keys:                  |
    |     PRK = HKDF-Extract(salt=empty,        |  (Same derivation on relay side)
    |                        ikm=shared_secret) |
    |     send_key = HKDF-Expand(PRK,           |
    |       "chromatin-init-to-resp-v1", 32)    |
    |     recv_key = HKDF-Expand(PRK,           |
    |       "chromatin-resp-to-init-v1", 32)    |
    |     fingerprint = SHA3-256(shared_secret   |
    |       || init_signing_pk || resp_signing_pk)|
    |                                           |
    |-- [encrypted, nonce=0] AuthSignature ---->|  TransportMessage(type=3)
    |   payload: [pk_len:4 LE][signing_pk:2592] |  AEAD encrypt with send_key, nonce counter 0
    |            [ML-DSA-87 sig over fingerprint]|
    |                                           |
    |<-- [encrypted, nonce=0] AuthSignature ----|  TransportMessage(type=3)
    |   payload: [pk_len:4 LE][relay_pk:2592]   |  AEAD decrypt with recv_key, nonce counter 0
    |            [ML-DSA-87 sig over fingerprint]|
    |                                           |
    |  4. Verify relay's signature over         |
    |     session fingerprint                   |
    |                                           |
    |  Counters now: send=1, recv=1             |
    |  Session established.                     |
```

### CRITICAL: Key Derivation -- Code vs PROTOCOL.md Discrepancy

**PROTOCOL.md states:** salt = SHA3-256(initiator_signing_pubkey || responder_signing_pubkey)

**Actual C++ code (handshake.cpp:21-28):**
```cpp
std::span<const uint8_t> empty_salt{};
auto prk = crypto::KDF::extract(empty_salt, shared_secret);
```

**The code uses EMPTY salt, not SHA3-256 of pubkeys.** The SDK MUST follow the code (empty salt), not PROTOCOL.md. The session fingerprint computation also differs: the code computes `SHA3-256(shared_secret || init_pk || resp_pk)` directly, not via HKDF expand with a "session-fp" context string.

**Verified key derivation (from handshake.cpp):**
1. `PRK = HKDF-SHA256-Extract(salt=empty, ikm=shared_secret)` -- empty salt, shared_secret as IKM
2. `init_to_resp_key = HKDF-SHA256-Expand(PRK, "chromatin-init-to-resp-v1", 32)` -- SDK send_key
3. `resp_to_init_key = HKDF-SHA256-Expand(PRK, "chromatin-resp-to-init-v1", 32)` -- SDK recv_key
4. `fingerprint = SHA3-256(shared_secret || init_signing_pk || resp_signing_pk)` -- plain hash, NOT HKDF

### Auth Payload Wire Format

The AuthSignature payload (type 3) carries:

```
[pk_size: 4 bytes LITTLE-ENDIAN uint32]  <-- Note: LE, not BE
[signing_pubkey: pk_size bytes]           <-- 2592 bytes for ML-DSA-87
[signature: remaining bytes]              <-- ML-DSA-87 signature over fingerprint
```

This is explicitly little-endian for the size prefix (handshake.cpp:119-124), unlike most other protocol fields which use big-endian. The SDK must encode `pk_size` as LE here.

### Message Loop Flow (Post-Handshake)

```
SDK                                         Relay                    Node
 |                                           |                        |
 |-- [AEAD] TransportMessage(type, payload)->|                        |
 |   request_id = client-assigned            | is_client_allowed()?   |
 |                                           | YES: forward to node   |
 |                                           |-- [UDS AEAD] forward ->|
 |                                           |                        | process
 |                                           |<- [UDS AEAD] response -|
 |<- [AEAD] TransportMessage(resp, payload) -|                        |
 |   request_id echoed from request          |                        |
 |                                           |                        |
 |   Server-initiated (notifications):       |                        |
 |<- [AEAD] Notification(request_id=0) ------|<- Notification --------|
```

### Notification Dispatch Flow

```
Background recv loop:
    frame = await recv_encrypted()
    msg = decode_transport(frame)
    if msg.request_id != 0 and msg.request_id in pending:
        pending[msg.request_id].set_result((msg.type, msg.payload))
    elif msg.type == Notification:
        notification_callback(decode_notification(msg.payload))
    elif msg.type == Ping:
        await send_message(Pong, b'', request_id=0)
    elif msg.type == Goodbye:
        close()
    elif msg.type == StorageFull or msg.type == QuotaExceeded:
        # These may echo a request_id OR be spontaneous
        if msg.request_id in pending:
            pending[msg.request_id].set_exception(StorageError(...))
```

## Integration Points with Existing C++ Code

### Relay PQ Handshake Responder (relay_session.cpp + connection.cpp)

The relay accepts TCP clients and runs the PQ handshake as responder. Key integration details:

| Aspect | Relay Behavior | SDK Must Match |
|--------|---------------|----------------|
| First message | Expects KemPubkey (type 1) or TrustedHello (type 23) | SDK sends KemPubkey (always PQ, never trusted) |
| KemPubkey payload | `[kem_pk:1568][signing_pk:2592]` = 4160 bytes | Exact byte layout, sizes |
| KemCiphertext response | `[ciphertext:1568][signing_pk:2592]` = 4160 bytes | Parse at fixed offsets |
| Auth payload | `[pk_len:4 LE][pubkey][signature]` | LE uint32 for pk_len |
| AEAD | ChaCha20-Poly1305 IETF, empty AD, counter nonce | Identical AEAD params |
| FlatBuffers | `ForceDefaults(true)`, standard encoding | ForceDefaults(True) in Python builder |

**SDK always uses PQ handshake, never TrustedHello.** The SDK connects to the relay over TCP (not UDS). The relay is never a trusted peer for external clients. The TrustedHello path is relay-to-node only.

### Relay Message Filter (message_filter.cpp)

The relay uses a blocklist approach. These 21 types are blocked; everything else passes:

| Blocked Types | Reason |
|---------------|--------|
| None (0) | Invalid |
| KemPubkey (1), KemCiphertext (2), AuthSignature (3), AuthPubkey (4) | Handshake |
| TrustedHello (23), PQRequired (24) | Handshake |
| SyncRequest (9) through SyncComplete (14) | Peer sync |
| ReconcileInit (26), ReconcileRanges (27), ReconcileItems (28) | Peer sync |
| SyncRejected (29) | Peer sync |
| PeerListRequest (15), PeerListResponse (16) | PEX |
| StorageFull (22), QuotaExceeded (25) | Internal signals (blocked client->node, but node->client allowed) |

**If the SDK sends a blocked type, the relay disconnects immediately** (teardown "blocked message type"). The SDK must never send any blocked type.

### FlatBuffers Schema Compatibility

The SDK uses the same `.fbs` schemas as the C++ code:
- `db/schemas/transport.fbs` -- TransportMessage + TransportMsgType enum
- `db/schemas/blob.fbs` -- Blob table (for Data/Delete/Read payloads)

Generated Python code must come from the same schema version. Generate with:
```bash
flatc --python -o sdk/python/chromatindb/_generated/ db/schemas/transport.fbs db/schemas/blob.fbs
```

### Key File Format Compatibility

The node stores keys as raw binary files: `node.pub` (2592 bytes, ML-DSA-87 public key) and `node.key` (4896 bytes, ML-DSA-87 secret key). The SDK should use the same format for its identity files so keys can be inspected/compared across implementations.

## Crypto Library Mapping

| C++ Primitive | Python Library | Function |
|---------------|---------------|----------|
| `OQS_KEM("ML-KEM-1024")` | `liboqs.KeyEncapsulation("ML-KEM-1024")` | `.generate_keypair()`, `.encap_secret(pk)`, `.decap_secret(ct)` |
| `OQS_SIG("ML-DSA-87")` | `liboqs.Signature("ML-DSA-87")` | `.sign(msg)`, `.verify(msg, sig, pk)` |
| `crypto_aead_chacha20poly1305_ietf_encrypt` | `nacl.bindings.crypto_aead_chacha20poly1305_ietf_encrypt` | `(msg, aad, nonce, key)` |
| `crypto_aead_chacha20poly1305_ietf_decrypt` | `nacl.bindings.crypto_aead_chacha20poly1305_ietf_decrypt` | `(ct, aad, nonce, key)` |
| `crypto_kdf_hkdf_sha256_extract` + `_expand` | `cryptography.hazmat.primitives.kdf.hkdf` | `HKDF(algorithm=SHA256(), ...)` or separate Extract/Expand |
| `SHA3-256` | `hashlib.sha3_256` | stdlib, no external dep |

**HKDF strategy:** Use the `cryptography` library for HKDF-SHA256. It is widely available, stable, and implements RFC 5869 identically to libsodium. Use `HKDFExpand` for the expand step (we need separate extract/expand calls since the code does them independently). The `cryptography` library's HKDF with SHA-256 produces identical output to libsodium's `crypto_kdf_hkdf_sha256_extract/expand` -- both implement the same RFC.

**AEAD strategy:** Use PyNaCl's `nacl.bindings.crypto_aead_chacha20poly1305_ietf_encrypt/decrypt` because it is a direct binding to the same libsodium functions the C++ code uses. This eliminates any possibility of subtle behavioral differences in AAD handling or tag placement.

## Anti-Patterns

### Anti-Pattern 1: Sharing Send Counter Across Coroutines

**What people do:** Allow multiple concurrent coroutines to call `send_encrypted()` without serialization.
**Why it's wrong:** The AEAD nonce is derived from `send_counter_`. If two sends interleave, they may use the same nonce or use nonces out of order. On the relay side, `recv_counter_` increments linearly -- any mismatch means AEAD decrypt failure and connection death. This exact bug was the root cause of the PEX SIGSEGV in the C++ node (v1.0.0 Phase 52).
**Do this instead:** Serialize all sends through a single asyncio.Lock or channel. Only one frame in flight at a time on the send path.

### Anti-Pattern 2: Assuming Message Order Matches Request Order

**What people do:** Send request A then request B, assume response A arrives before response B.
**Why it's wrong:** The node dispatches requests concurrently. A ReadRequest for a large blob may complete after a NodeInfoRequest sent later. The `request_id` field exists precisely because ordering is not guaranteed.
**Do this instead:** Always match responses by `request_id`. Use a dict of pending futures keyed by `request_id`.

### Anti-Pattern 3: Using the `cryptography` Library for AEAD

**What people do:** Use `cryptography.hazmat.primitives.ciphers.aead.ChaCha20Poly1305` instead of PyNaCl's low-level binding.
**Why it's wrong:** The `cryptography` library's ChaCha20-Poly1305 has a different API surface for AAD handling. PyNaCl's `crypto_aead_chacha20poly1305_ietf_encrypt/decrypt` directly maps to libsodium's API with `aad=None` for empty AD -- matching the C++ `std::span<const uint8_t> empty_ad{}` exactly.
**Do this instead:** Use PyNaCl's `nacl.bindings.crypto_aead_chacha20poly1305_ietf_encrypt` for the AEAD layer. It is a direct binding to the same libsodium function the C++ code uses.

### Anti-Pattern 4: Generating FlatBuffers at Runtime

**What people do:** Try to build FlatBuffers manually with struct.pack instead of using the generated Python code.
**Why it's wrong:** FlatBuffers has a specific binary format (vtable, offsets, alignment) that is not trivially reproducible. Hand-crafted bytes will not verify with `VerifyTransportMessageBuffer` on the C++ side.
**Do this instead:** Use `flatc --python` to generate Python classes from `transport.fbs` and `blob.fbs`. Commit the generated code. Use the Builder API.

### Anti-Pattern 5: Forgetting ForceDefaults(True)

**What people do:** Create FlatBuffers without ForceDefaults, so default-valued fields (like `request_id = 0`) are omitted from the binary.
**Why it's wrong:** The C++ side always uses `ForceDefaults(true)`. While the FlatBuffers spec says default fields can be omitted, the C++ encoder explicitly includes them. If the Python encoder omits them, the binary output differs and the C++ verifier may behave differently.
**Do this instead:** Always set `builder.ForceDefaults(True)` before building TransportMessage.

## Build Order and Dependencies

### Phase 1: Foundation (no relay needed)

Build order driven by dependency graph:

1. **constants.py** -- Zero dependencies. Protocol constants, type enums, sizes.
2. **exceptions.py** -- Zero dependencies. Error hierarchy.
3. **crypto.py** -- Depends on liboqs-python, PyNaCl, cryptography, hashlib. Pure functions, testable with known vectors.
4. **wire.py** -- Depends on FlatBuffers generated code. TransportMessage encode/decode.
5. **identity.py** -- Depends on crypto.py. Keypair management.

All testable with unit tests, no network needed.

### Phase 2: Protocol Layer (mock transport)

6. **transport.py** -- Depends on crypto.py. TCP + framing. Testable with mock sockets.
7. **messages.py** -- Depends on constants.py. Payload encoders/decoders for all 38 types. Testable with round-trip encode/decode.
8. **handshake.py** -- Depends on crypto.py, wire.py, transport.py. Testable with mock transport (pre-recorded relay responses).

### Phase 3: Session and Client (needs relay)

9. **session.py** -- Depends on handshake.py, wire.py, transport.py, messages.py. Connection lifecycle + pipelining.
10. **client.py** -- Depends on session.py, identity.py, messages.py. Public API.

Integration testing requires a running relay + node (Docker).

## Scaling Considerations

| Concern | Single Connection | Multiple Connections |
|---------|------------------|---------------------|
| Throughput | ~33 blobs/sec for 1 MiB (relay benchmark baseline) | Connection pool with round-robin |
| Pipelining | 2^32 outstanding requests per connection | Unlikely to be bottleneck |
| Memory | One 110 MiB frame buffer worst case | Pool size * frame buffer |
| Latency | PQ handshake ~10-50ms (KEM + DSA ops) | Amortized by persistent connections |

### Scaling Priorities

1. **First bottleneck:** PQ handshake latency. ML-KEM-1024 encapsulation + ML-DSA-87 sign/verify takes ~10-50ms. Mitigate with persistent connections (reconnect on failure, do not reconnect per request).
2. **Second bottleneck:** Single-connection send serialization. AEAD nonce management requires serialized sends. For write-heavy workloads, a connection pool with multiple relay connections would help, but this is a future optimization -- single connection is sufficient for SDK v1.

## Sources

- [liboqs-python](https://github.com/open-quantum-safe/liboqs-python) -- Python bindings for ML-KEM-1024, ML-DSA-87
- [liboqs-python on PyPI](https://pypi.org/project/liboqs-python/) -- pip-installable package
- [PyNaCl](https://pynacl.readthedocs.io/) -- Python libsodium bindings (ChaCha20-Poly1305 IETF AEAD)
- [PyNaCl crypto_aead source](https://github.com/pyca/pynacl/blob/main/src/nacl/bindings/crypto_aead.py) -- Low-level AEAD API
- [cryptography library HKDF](https://cryptography.io/en/latest/hazmat/primitives/key-derivation-functions/) -- HKDF-SHA256 via hazmat primitives
- [FlatBuffers Python docs](https://flatbuffers.dev/languages/python/) -- Python runtime for FlatBuffers
- [flatbuffers on PyPI](https://pypi.org/project/flatbuffers/) -- pip-installable runtime
- [Python hashlib SHA3](https://docs.python.org/3/library/hashlib.html) -- Stdlib SHA3-256 support (since Python 3.6)
- [libsodium HKDF docs](https://doc.libsodium.org/key_derivation/hkdf) -- HKDF-SHA256 reference (added in libsodium 1.0.19)
- Direct source code analysis: `db/net/connection.cpp`, `db/net/handshake.cpp`, `db/net/framing.cpp`, `db/net/protocol.cpp`, `relay/core/relay_session.cpp`, `relay/core/message_filter.cpp`

---
*Architecture research for: Python SDK integrating with chromatindb relay*
*Researched: 2026-03-29*
