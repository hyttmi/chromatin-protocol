# Phase 71: Transport & PQ Handshake - Research

**Researched:** 2026-03-29
**Domain:** Async TCP transport, ML-KEM-1024 key exchange, ML-DSA-87 mutual auth, AEAD encrypted framing
**Confidence:** HIGH

## Summary

Phase 71 implements the SDK's transport layer: a PQ-authenticated, AEAD-encrypted TCP session between the Python SDK (initiator) and a live chromatindb relay (responder). The protocol is fully specified by the C++ implementation -- the SDK replicates the initiator side exactly, byte-for-byte.

The core building blocks (AEAD encrypt/decrypt, HKDF-SHA256, ML-DSA-87 sign/verify, FlatBuffers encode/decode) are already implemented and tested in Phase 70. Phase 71 composes these into: (1) TCP connection via asyncio streams, (2) PQ handshake state machine (KEM exchange + auth), (3) encrypted frame send/recv, (4) background reader coroutine for multiplexed dispatch, and (5) a `ChromatinClient` context manager for clean lifecycle management.

liboqs-python's `oqs.KeyEncapsulation("ML-KEM-1024")` provides the ephemeral KEM operations. Key sizes are confirmed: public key 1568 bytes, ciphertext 1568 bytes, shared secret 32 bytes -- matching C++ exactly. All crypto is zero-dependency beyond existing Phase 70 packages (liboqs-python, PyNaCl).

**Primary recommendation:** Implement as three internal modules (`_handshake.py`, `_framing.py`, `_transport.py`) plus the public `client.py`, building bottom-up with unit tests at each layer before integration testing against the KVM relay.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Context manager pattern -- `async with ChromatinClient.connect(host, port, identity) as conn:`. Handshake on enter, Goodbye + close on exit. No explicit connect/close alternative.
- **D-02:** Class name: `ChromatinClient`. Exported from `chromatindb` package top-level.
- **D-03:** High-level API only -- no exposed transport methods (send_frame, recv_frame). All transport internals are private.
- **D-04:** Phase 71 exposes only what works: connect, disconnect, ping/pong. No stubs for future data operations. Phase 72 adds read/write/etc.
- **D-05:** Pure asyncio streams -- `asyncio.open_connection()` for TCP. Zero networking dependencies.
- **D-06:** Phase 71 is async-only. Sync wrapper deferred to Phase 74 (packaging).
- **D-07:** Configurable handshake timeout (default 10s). Raises HandshakeError on timeout. No separate TCP connect timeout -- asyncio handles that.
- **D-08:** Background reader coroutine spawned after handshake. Continuously reads and decrypts frames, dispatches by request_id to pending futures, and routes notifications to a queue. Foundation for Phase 72/73 multiplexing.
- **D-09:** request_id counter auto-assigned by transport layer. Internal counter increments per request. Callers never see or manage request_id.
- **D-10:** On disconnection or frame corruption: reader cancels all pending request futures with ConnectionError, sets connection state to closed. Next user operation raises immediately.
- **D-11:** Integration tests run against live KVM swarm relay (192.168.1.200:4433). No Docker compose for SDK tests.
- **D-12:** Both unit tests and integration tests. Unit tests for frame encode/decode, nonce counter logic, AEAD encrypt/decrypt (no network). Integration tests for full handshake against KVM relay.
- **D-13:** Relay address from environment variables: `CHROMATINDB_RELAY_HOST` / `CHROMATINDB_RELAY_PORT` with defaults 192.168.1.200 / 4433. Integration tests skip if relay unreachable (pytest.mark.integration).

### Claude's Discretion
- Internal module organization (transport.py vs splitting into handshake.py + framing.py + client.py)
- Notification queue implementation details (asyncio.Queue vs callback registration)
- Ping/pong implementation details and keepalive behavior
- pytest fixture structure for integration tests (conftest.py, identity fixtures)
- Internal error handling granularity (which exceptions for which failure modes)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| XPORT-02 | SDK performs ML-KEM-1024 key exchange with relay (PQ handshake initiator) | KEM API verified: `oqs.KeyEncapsulation("ML-KEM-1024")` with `generate_keypair()`, `encap_secret()`, `decap_secret()`. Sizes confirmed: pk=1568, ct=1568, ss=32. Handshake state machine fully specified in `db/net/handshake.cpp` and `db/net/connection.cpp:408-496`. |
| XPORT-03 | SDK performs mutual ML-DSA-87 authentication during handshake | Auth exchange: sign session fingerprint with `Identity.sign()`, verify peer's signature with `Identity.verify()`. Auth payload format: `[4-byte LE pubkey_size][pubkey][signature]`. Session fingerprint = SHA3-256(shared_secret \|\| init_sig_pk \|\| resp_sig_pk). |
| XPORT-04 | SDK encrypts/decrypts all post-handshake frames with ChaCha20-Poly1305 AEAD | `crypto.aead_encrypt/aead_decrypt` from Phase 70. Wire frame: `[4-byte BE ciphertext_length][AEAD ciphertext]`. Empty associated data. Nonce = `[4 zero bytes][8-byte BE counter]`. |
| XPORT-05 | SDK maintains correct per-direction AEAD nonce counters | Two independent counters (send_counter, recv_counter), both start at 0. Counter 0 consumed by auth exchange during handshake. Post-handshake messages start at counter 1. |
| XPORT-07 | SDK supports connection lifecycle (connect, disconnect via Goodbye, context manager) | `async with ChromatinClient.connect(host, port, identity) as conn:` pattern. Goodbye = TransportMsgType 7, empty payload, encrypted frame. Context manager sends Goodbye on `__aexit__`. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| asyncio (stdlib) | Python 3.10+ | TCP streams, coroutines, task management | D-05: zero networking dependencies, `open_connection()` for TCP |
| liboqs-python | 0.14.1 (installed) | ML-KEM-1024 ephemeral key exchange | Already in deps from Phase 70, provides `oqs.KeyEncapsulation` |
| PyNaCl | 1.5.0 (installed) | ChaCha20-Poly1305 AEAD via libsodium | Already in deps from Phase 70, `aead_encrypt/aead_decrypt` |
| flatbuffers | 25.12.19 (installed) | TransportMessage wire encoding | Already in deps from Phase 70, `encode/decode_transport_message` |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| pytest | 9.0.2 (installed) | Test framework | All unit and integration tests |
| hashlib (stdlib) | Python 3.10+ | SHA3-256 for session fingerprint | Already used in Phase 70 crypto.py |
| struct (stdlib) | Python 3.10+ | Binary packing (BE/LE integers) | Frame headers, auth payload encoding |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| asyncio streams | aiohttp/trio | D-05 locks to pure asyncio, zero deps |
| Manual frame parsing | asyncio.Protocol | Streams simpler for length-prefixed framing |
| asyncio.Queue for notifications | Callbacks | Queue is simpler, backpressure-aware, natural async |

**Installation:** Already installed. No new dependencies required for Phase 71.

**Version verification:** Verified via pip in `.venv`:
- liboqs-python: 0.14.1 (system liboqs: 0.15.0, minor version mismatch warning is cosmetic)
- pynacl: 1.5.0
- flatbuffers: 25.12.19
- pytest: 9.0.2

## Architecture Patterns

### Recommended Module Structure
```
sdk/python/chromatindb/
  __init__.py          # Add ChromatinClient to exports
  client.py            # ChromatinClient: public API, context manager
  _handshake.py        # PQ handshake state machine (initiator only)
  _framing.py          # Encrypted frame read/write, nonce management
  _transport.py        # Background reader, request dispatch, connection state
  exceptions.py        # Add HandshakeError, ConnectionError subclasses
  crypto.py            # (existing) AEAD, SHA3-256, HKDF
  identity.py          # (existing) ML-DSA-87 Identity
  wire.py              # (existing) FlatBuffers encode/decode
  _hkdf.py             # (existing) Pure-Python HKDF-SHA256
```

### Pattern 1: PQ Handshake Initiator State Machine
**What:** Replicate the C++ `do_handshake_initiator_pq()` as an async function operating on asyncio StreamReader/StreamWriter.
**When to use:** Every SDK connection to a relay.
**Protocol sequence (4 steps):**

```
SDK (initiator)                          Relay (responder)
    |                                        |
    |-- [raw] KemPubkey(kem_pk||sig_pk) ---->|
    |                                        |
    |<--- [raw] KemCiphertext(ct||sig_pk) ---|
    |                                        |
    |   (both derive session keys via HKDF)  |
    |                                        |
    |-- [enc] AuthSignature(pk_size||pk||sig) ->|  (counter=0)
    |                                        |
    |<- [enc] AuthSignature(pk_size||pk||sig) --|  (counter=0)
    |                                        |
    |   (post-handshake: counters at 1)      |
```

**Critical implementation detail from C++ source (`connection.cpp:408-496`):**
The C++ Connection class does NOT use `HandshakeInitiator::create_auth_message()` or `verify_peer_auth()`. Instead, after taking session keys from the HandshakeInitiator, the Connection builds the auth payload inline and uses its own `send_encrypted()`/`recv_encrypted()` methods. This means:
- The Connection's `send_counter_` and `recv_counter_` (both initialized to 0) are used for the auth messages
- After handshake, the counters are at 1 for both directions

The Python SDK must replicate this behavior exactly: session keys derived by the handshake, but auth exchange encrypted using the transport layer's own counter-based framing.

### Pattern 2: Length-Prefixed Frame IO
**What:** All messages use `[4-byte BE length][payload]` framing. During KEM exchange, payload is plaintext. After key derivation, payload is AEAD ciphertext.
**Wire format:**

```python
# Raw send (KEM exchange phase):
#   [4-byte BE len(data)] [data]
async def send_raw(writer, data: bytes) -> None:
    writer.write(struct.pack(">I", len(data)))
    writer.write(data)
    await writer.drain()

# Raw recv:
async def recv_raw(reader) -> bytes:
    header = await reader.readexactly(4)
    length = struct.unpack(">I", header)[0]
    if length > MAX_FRAME_SIZE:
        raise ProtocolError("frame exceeds maximum size")
    return await reader.readexactly(length)

# Encrypted send (post-key-derivation):
#   encrypt plaintext -> ciphertext (includes AEAD tag)
#   send_raw(ciphertext)
async def send_encrypted(writer, plaintext, key, counter) -> int:
    nonce = make_nonce(counter)
    ciphertext = aead_encrypt(plaintext, b"", nonce, key)
    await send_raw(writer, ciphertext)
    return counter + 1

# Encrypted recv:
#   recv_raw() -> ciphertext
#   decrypt ciphertext -> plaintext
async def recv_encrypted(reader, key, counter) -> tuple[bytes, int]:
    ciphertext = await recv_raw(reader)
    nonce = make_nonce(counter)
    plaintext = aead_decrypt(ciphertext, b"", nonce, key)
    if plaintext is None:
        raise DecryptionError("AEAD decrypt failed")
    return plaintext, counter + 1
```

### Pattern 3: Nonce Construction
**What:** 12-byte AEAD nonce from counter.
**Format:** `[4 zero bytes][8-byte BE counter]`

```python
# Source: db/net/framing.cpp make_nonce()
def make_nonce(counter: int) -> bytes:
    return b"\x00\x00\x00\x00" + struct.pack(">Q", counter)
```

### Pattern 4: Background Reader with Request Dispatch
**What:** After handshake, a background asyncio.Task continuously reads and decrypts frames, routing responses by request_id.
**Architecture:**

```python
# Pending requests: {request_id: asyncio.Future}
# Notification queue: asyncio.Queue for unsolicited messages

async def _reader_loop(self):
    try:
        while not self._closed:
            plaintext = await self._recv_encrypted()
            msg_type, payload, request_id = decode_transport_message(plaintext)

            if msg_type == TransportMsgType.Ping:
                await self._send_ping_response()
            elif msg_type == TransportMsgType.Pong:
                pass  # heartbeat ack
            elif msg_type == TransportMsgType.Goodbye:
                self._closed = True
                break
            elif request_id in self._pending:
                self._pending.pop(request_id).set_result((msg_type, payload))
            else:
                await self._notifications.put((msg_type, payload, request_id))
    except Exception as e:
        self._close_with_error(e)
```

### Pattern 5: Context Manager Lifecycle
**What:** `ChromatinClient.connect()` returns an async context manager.

```python
class ChromatinClient:
    @classmethod
    async def connect(cls, host: str, port: int, identity: Identity,
                      *, timeout: float = 10.0) -> AsyncContextManager[ChromatinClient]:
        # Returns async context manager
        ...

    async def __aenter__(self) -> ChromatinClient:
        # TCP connect + handshake (with timeout)
        ...

    async def __aexit__(self, *exc_info) -> None:
        # Send Goodbye, cancel reader, close socket
        ...
```

### Anti-Patterns to Avoid
- **Using `write_frame()`/`read_frame()` from C++ framing.h directly:** The Connection class separates encryption from framing. The SDK must do the same: `send_raw` handles length-prefixed framing, `send_encrypted` handles AEAD on top of `send_raw`. Do NOT combine them into a single function that matches `write_frame()`.
- **Sharing nonce counters between handshake and transport:** The C++ Connection owns the counters and uses them for both auth messages and post-handshake traffic. The handshake state machine only handles KEM exchange. Counters live in the transport layer, not the handshake.
- **Blocking the event loop with crypto:** ML-KEM-1024 `generate_keypair()` and `encap_secret()` are fast (<1ms). ML-DSA-87 `sign()`/`verify()` are ~1ms. No need for `loop.run_in_executor()` at SDK scale (single connection). The C++ uses a thread pool because the node handles many concurrent connections.
- **Using `asyncio.StreamReader.read()` instead of `readexactly()`:** `read()` may return partial data. Always use `readexactly()` for length-prefixed framing.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| KEM key exchange | Custom Diffie-Hellman | `oqs.KeyEncapsulation("ML-KEM-1024")` | PQ-secure, matches C++ node exactly |
| AEAD encryption | Custom cipher | `crypto.aead_encrypt/aead_decrypt` (Phase 70) | Already tested byte-identical to C++ |
| HKDF key derivation | Custom KDF | `_hkdf.hkdf_extract/hkdf_expand` (Phase 70) | Already tested byte-identical to C++ |
| FlatBuffers encoding | Manual binary serialization | `wire.encode/decode_transport_message` (Phase 70) | Generated code matches C++ schema |
| ML-DSA-87 signing | Custom signature scheme | `identity.Identity.sign/verify` (Phase 70) | Already tested with C++ test vectors |

**Key insight:** Phase 71 is pure composition -- every cryptographic primitive is already implemented and tested in Phase 70. The new code is protocol orchestration (handshake sequencing, frame IO, connection lifecycle), not crypto.

## Common Pitfalls

### Pitfall 1: Auth Payload Endianness Mismatch
**What goes wrong:** Auth payload pubkey_size is little-endian (4 bytes LE), but frame length prefix is big-endian (4 bytes BE). Mixing them up causes handshake failure.
**Why it happens:** The protocol uses mixed endianness -- BE for framing/nonces, LE for auth payload fields (matching C++ implementation).
**How to avoid:** Use `struct.pack("<I", pubkey_size)` for auth payload, `struct.pack(">I", frame_length)` for frame headers, `struct.pack(">Q", counter)` for nonces.
**Warning signs:** Handshake succeeds locally (loopback) but fails against relay, or auth verification fails despite correct keys.

### Pitfall 2: Nonce Counter Off-by-One
**What goes wrong:** Post-handshake messages fail to decrypt because nonce counters are wrong.
**Why it happens:** Auth exchange consumes counter 0 for both send and recv directions. If post-handshake code starts at counter 0 instead of 1, every frame is encrypted/decrypted with the wrong nonce.
**How to avoid:** After handshake completes, assert `send_counter == 1` and `recv_counter == 1`. The counter increments happen during `send_encrypted`/`recv_encrypted` which are also used for the auth exchange.
**Warning signs:** First post-handshake message (e.g., Ping) causes AEAD decrypt failure.

### Pitfall 3: KEM Pubkey Message Payload Ordering
**What goes wrong:** SDK sends `[sig_pk][kem_pk]` instead of `[kem_pk][sig_pk]` and relay fails to parse.
**Why it happens:** The payload has no length prefixes -- it is just two concatenated fixed-size keys. Getting the order wrong silently produces garbage keys.
**How to avoid:** Strictly follow C++ `HandshakeInitiator::start()` -- payload is `[kem_pk:1568][sig_pk:2592]` (KEM public key first, then signing public key). Same order for KemCiphertext response: `[kem_ct:1568][sig_pk:2592]`.
**Warning signs:** KEM decapsulation produces wrong shared secret, auth verification fails.

### Pitfall 4: asyncio Reader Task Cleanup
**What goes wrong:** Background reader task leaks or raises `CancelledError` that propagates unexpectedly.
**Why it happens:** `asyncio.Task` must be explicitly cancelled and awaited on disconnect. If the context manager's `__aexit__` doesn't properly cancel+await the reader task, you get warnings about destroyed pending tasks.
**How to avoid:** In `__aexit__`: (1) send Goodbye, (2) cancel reader task, (3) `await` the cancelled task wrapped in `try/except asyncio.CancelledError`, (4) close writer.
**Warning signs:** "Task was destroyed but it is pending!" warnings, or unhandled exceptions in event loop.

### Pitfall 5: Partial Read on Connection Reset
**What goes wrong:** `asyncio.StreamReader.readexactly()` raises `asyncio.IncompleteReadError` when the connection drops mid-frame.
**Why it happens:** Relay closes connection, or network interruption during read.
**How to avoid:** Catch `asyncio.IncompleteReadError` and `ConnectionResetError` in the reader loop. Set connection state to closed and cancel all pending request futures with `ConnectionError`.
**Warning signs:** Unhandled exceptions in background task instead of clean disconnection.

### Pitfall 6: Handshake Timeout Not Covering Full Sequence
**What goes wrong:** Timeout only covers TCP connect but not the 4-step handshake exchange.
**Why it happens:** `asyncio.open_connection()` has its own timeout, but the handshake involves multiple round-trips that can each hang.
**How to avoid:** Wrap the entire handshake (all 4 steps) in `asyncio.wait_for(handshake_coro, timeout=timeout)`. D-07 specifies a single configurable timeout (default 10s).
**Warning signs:** SDK hangs indefinitely if relay accepts TCP but stalls during handshake.

### Pitfall 7: FlatBuffer Payload Byte Extraction Performance
**What goes wrong:** Using `bytes([msg.Payload(j) for j in range(length)])` for large payloads is O(n) with high constant factor.
**Why it happens:** Phase 70 chose this pattern to avoid numpy dependency, which is fine for small payloads (handshake messages are <8KB). But large data payloads in Phase 72 could be slow.
**How to avoid:** For Phase 71, handshake payloads are small enough that the per-element loop is fine. Note this for Phase 72 optimization if needed.
**Warning signs:** Not an issue for Phase 71 (handshake payloads are fixed ~4KB-8KB).

## Code Examples

Verified patterns from C++ source and existing Phase 70 SDK code:

### KEM Key Exchange (Initiator Side)
```python
# Source: db/net/handshake.cpp HandshakeInitiator::start() lines 160-172
# and connection.cpp do_handshake_initiator_pq() lines 408-431
import oqs
from chromatindb.wire import encode_transport_message, TransportMsgType

# Step 1: Generate ephemeral KEM keypair, send KemPubkey
kem = oqs.KeyEncapsulation("ML-KEM-1024")
kem_pk = kem.generate_keypair()  # returns 1568-byte public key
signing_pk = identity.public_key  # 2592-byte ML-DSA-87 public key

# Payload: [kem_pk:1568][signing_pk:2592] -- no length prefix
payload = bytes(kem_pk) + signing_pk
msg = encode_transport_message(TransportMsgType.KemPubkey, payload)
await send_raw(writer, msg)

# Step 2: Receive KemCiphertext, decapsulate
raw = await recv_raw(reader)
msg_type, resp_payload, _ = decode_transport_message(raw)
assert msg_type == TransportMsgType.KemCiphertext
assert len(resp_payload) == 1568 + 2592  # ct + sig_pk

kem_ct = resp_payload[:1568]
responder_sig_pk = resp_payload[1568:]

shared_secret = kem.decap_secret(kem_ct)  # 32-byte shared secret
```

### Session Key Derivation
```python
# Source: db/net/handshake.cpp derive_session_keys() lines 14-55
from chromatindb.crypto import sha3_256
from chromatindb._hkdf import hkdf_extract, hkdf_expand

# HKDF Extract with empty salt (per C++ implementation, NOT PROTOCOL.md)
prk = hkdf_extract(b"", shared_secret)

# Derive directional keys
init_to_resp = hkdf_expand(prk, b"chromatin-init-to-resp-v1", 32)
resp_to_init = hkdf_expand(prk, b"chromatin-resp-to-init-v1", 32)

# Session fingerprint: SHA3-256(shared_secret || initiator_sig_pk || responder_sig_pk)
fingerprint = sha3_256(shared_secret + identity.public_key + responder_sig_pk)

# Initiator: send_key = init_to_resp, recv_key = resp_to_init
send_key = init_to_resp
recv_key = resp_to_init
```

### Auth Payload Encoding
```python
# Source: db/net/handshake.cpp encode_auth_payload() lines 114-130
import struct

def encode_auth_payload(signing_pubkey: bytes, signature: bytes) -> bytes:
    pk_size = len(signing_pubkey)
    # 4-byte LITTLE-ENDIAN pubkey size (NOT big-endian!)
    header = struct.pack("<I", pk_size)
    return header + signing_pubkey + signature

def decode_auth_payload(data: bytes) -> tuple[bytes, bytes]:
    if len(data) < 4:
        raise ProtocolError("auth payload too short")
    pk_size = struct.unpack("<I", data[:4])[0]
    if len(data) < 4 + pk_size:
        raise ProtocolError("auth payload truncated")
    pubkey = data[4 : 4 + pk_size]
    signature = data[4 + pk_size :]
    return pubkey, signature
```

### Auth Exchange (Initiator)
```python
# Source: db/net/connection.cpp do_handshake_initiator_pq() lines 433-496
# Note: Connection builds auth inline, does NOT use HandshakeInitiator methods

# Step 3: Send our encrypted auth (uses send_counter=0)
signature = identity.sign(fingerprint)
auth_payload = encode_auth_payload(identity.public_key, signature)
auth_msg = encode_transport_message(TransportMsgType.AuthSignature, auth_payload)
send_counter = await send_encrypted(writer, auth_msg, send_key, send_counter)
# send_counter is now 1

# Step 4: Receive and verify peer's encrypted auth (uses recv_counter=0)
resp_auth_plaintext, recv_counter = await recv_encrypted(reader, recv_key, recv_counter)
# recv_counter is now 1
msg_type, resp_auth_payload, _ = decode_transport_message(resp_auth_plaintext)
assert msg_type == TransportMsgType.AuthSignature

resp_pk, resp_sig = decode_auth_payload(resp_auth_payload)
valid = Identity.verify(fingerprint, resp_sig, resp_pk)
if not valid:
    raise HandshakeError("peer auth signature invalid")
```

### Nonce Construction
```python
# Source: db/net/framing.cpp make_nonce() lines 8-17
import struct

def make_nonce(counter: int) -> bytes:
    """Build 12-byte AEAD nonce: [4 zero bytes][8-byte BE counter]."""
    return b"\x00\x00\x00\x00" + struct.pack(">Q", counter)
```

### Goodbye and Cleanup
```python
# Source: db/net/connection.cpp close_gracefully() lines 817-829
async def close_gracefully(writer, send_key, send_counter):
    goodbye_msg = encode_transport_message(TransportMsgType.Goodbye, b"")
    await send_encrypted(writer, goodbye_msg, send_key, send_counter)
    writer.close()
    await writer.wait_closed()
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `HandshakeInitiator.create_auth_message()` | Connection inline auth with own counters | v1.0 connection.cpp | SDK must use transport counters for auth, not separate handshake counters |
| HKDF salt = SHA3-256(pubkeys) per PROTOCOL.md | Empty HKDF salt per C++ source | Discovered Phase 70 | SDK uses empty salt; PROTOCOL.md fix deferred to Phase 74 |
| Uniform endianness | Mixed: BE framing, LE auth payload | C++ original design | Each field must use correct endianness per C++ source |

**Deprecated/outdated:**
- `HandshakeInitiator.create_auth_message()` and `verify_peer_auth()`: These exist in `handshake.cpp` but the Connection class does NOT use them for the PQ initiator path. The SDK should follow the Connection pattern (inline auth with transport counters).

## Open Questions

1. **Ping/Pong keepalive behavior**
   - What we know: C++ node replies to Ping with Pong (empty payload). No keepalive timer in Connection class itself.
   - What's unclear: Should SDK send periodic keepalive pings? At what interval?
   - Recommendation: Implement `ping()` as a user-callable method for Phase 71. No automatic keepalive timer -- keep it simple per YAGNI. Phase 72/73 can add keepalive if needed.

2. **Notification queue backpressure**
   - What we know: Background reader routes non-request messages to notification queue.
   - What's unclear: What happens if notifications pile up (e.g., many Subscribe notifications) and nobody reads the queue?
   - Recommendation: Use `asyncio.Queue(maxsize=1000)` as a reasonable default. If full, drop oldest or log warning. This is a Phase 73 concern (Pub/Sub) but the queue infrastructure is set up in Phase 71.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Python 3.10+ | SDK runtime | Yes | 3.14.3 | -- |
| liboqs-python | ML-KEM-1024 KEM | Yes | 0.14.1 | -- |
| PyNaCl | ChaCha20-Poly1305 AEAD | Yes | 1.5.0 | -- |
| flatbuffers | Wire format | Yes | 25.12.19 | -- |
| pytest | Testing | Yes | 9.0.2 | -- |
| KVM relay (192.168.1.200:4433) | Integration tests | No (currently offline) | -- | Tests skip with pytest.mark.integration |

**Missing dependencies with no fallback:**
- None

**Missing dependencies with fallback:**
- KVM relay currently offline (connect refused). Integration tests designed to skip gracefully when relay unreachable (D-13). Unit tests cover all handshake logic without network.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | pytest 9.0.2 |
| Config file | `sdk/python/pyproject.toml` [tool.pytest.ini_options] |
| Quick run command | `cd sdk/python && .venv/bin/python3 -m pytest tests/ -x -q` |
| Full suite command | `cd sdk/python && .venv/bin/python3 -m pytest tests/ -v` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| XPORT-02 | ML-KEM-1024 key exchange with relay | unit + integration | `pytest tests/test_handshake.py -x` | No -- Wave 0 |
| XPORT-03 | Mutual ML-DSA-87 auth during handshake | unit + integration | `pytest tests/test_handshake.py -x` | No -- Wave 0 |
| XPORT-04 | AEAD-encrypted post-handshake frames | unit | `pytest tests/test_framing.py -x` | No -- Wave 0 |
| XPORT-05 | Correct per-direction nonce counters | unit | `pytest tests/test_framing.py -x` | No -- Wave 0 |
| XPORT-07 | Connection lifecycle (context manager, Goodbye) | unit + integration | `pytest tests/test_client.py -x` | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** `cd sdk/python && .venv/bin/python3 -m pytest tests/ -x -q`
- **Per wave merge:** `cd sdk/python && .venv/bin/python3 -m pytest tests/ -v`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/test_framing.py` -- covers XPORT-04, XPORT-05 (nonce construction, frame encode/decode, encrypt/decrypt roundtrip)
- [ ] `tests/test_handshake.py` -- covers XPORT-02, XPORT-03 (KEM exchange, session key derivation, auth payload encode/decode, full handshake unit test with mock reader/writer)
- [ ] `tests/test_client.py` -- covers XPORT-07 (context manager, Goodbye, disconnection handling)
- [ ] `tests/test_integration.py` -- covers XPORT-02/03/04/05/07 end-to-end against live relay
- [ ] Update `tests/conftest.py` -- add Identity fixture, relay address fixture, integration marker
- [ ] Update `pyproject.toml` -- add `markers = ["integration: requires live relay"]` to pytest config

## Sources

### Primary (HIGH confidence)
- `db/net/handshake.h` + `db/net/handshake.cpp` -- complete handshake state machine, session key derivation, auth payload encoding
- `db/net/connection.h` + `db/net/connection.cpp` -- PQ initiator handshake flow (lines 408-496), encrypted frame IO, message loop, graceful close
- `db/net/framing.h` + `db/net/framing.cpp` -- nonce construction, frame format, MAX_FRAME_SIZE
- `db/crypto/kem.h` -- ML-KEM-1024 key/ciphertext sizes (PK=1568, SK=3168, CT=1568, SS=32)
- `sdk/python/chromatindb/crypto.py` -- verified AEAD encrypt/decrypt API
- `sdk/python/chromatindb/_hkdf.py` -- verified HKDF extract/expand with empty salt
- `sdk/python/chromatindb/identity.py` -- verified ML-DSA-87 sign/verify API
- `sdk/python/chromatindb/wire.py` -- verified FlatBuffers encode/decode
- Live verification: `oqs.KeyEncapsulation("ML-KEM-1024")` tested -- all sizes match C++

### Secondary (MEDIUM confidence)
- CONTEXT.md protocol quirks (mixed endianness, empty HKDF salt, nonce starts at 1) -- confirmed by reading C++ source directly

### Tertiary (LOW confidence)
- None -- all findings verified against C++ source code

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already installed and tested in Phase 70
- Architecture: HIGH -- protocol fully specified by C++ source, no ambiguity
- Pitfalls: HIGH -- all identified from direct C++ source reading, not speculation

**Research date:** 2026-03-29
**Valid until:** 2026-04-28 (stable -- protocol is frozen, only SDK implementation work)

---
*Phase: 71-transport-pq-handshake*
*Research completed: 2026-03-29*
