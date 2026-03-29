# Pitfalls Research

**Domain:** Python SDK client for chromatindb (PQ-crypto binary protocol interop with C++ server)
**Researched:** 2026-03-28
**Confidence:** HIGH (verified against C++ source code, protocol spec, and library documentation)

## Critical Pitfalls

### Pitfall 1: Mixed Endianness in Wire Protocol

**What goes wrong:**
The chromatindb wire protocol uses **two different endianness conventions** within the same connection. The SDK developer assumes one convention and gets silent data corruption or decryption failures.

- **Big-endian:** Frame length prefix (4 bytes), AEAD nonce counter (8 bytes in 12-byte nonce), all binary payload fields (seq_num, blob_count, timestamps in response payloads, Subscribe count, etc.)
- **Little-endian:** Auth payload pubkey_size (4 bytes), canonical signing input fields (ttl as LE uint32, timestamp as LE uint64), FlatBuffers internal encoding

A Python developer using `struct.pack('>I', length)` for frame headers will naturally reach for `'>I'` for all uint32 fields. But the auth payload's pubkey_size is `'<I'` (little-endian), and the signing input's ttl/timestamp are also little-endian. Getting this wrong means: auth messages the server cannot parse, signatures that never verify, or blob hashes that never match.

**Why it happens:**
FlatBuffers uses little-endian internally, and the signing input was designed to be endianness-explicit (LE for numeric fields). But the transport layer follows network byte order convention (BE). The auth payload was hand-encoded in the C++ codebase and inherited LE from the author's convention. There is no single "endianness rule" for the protocol -- you must know each field's encoding.

**How to avoid:**
Create a Python module with explicit encode/decode functions for every wire format, not generic helpers. Each function documents which endianness it uses:

```python
# Frame header: always big-endian
def encode_frame_length(length: int) -> bytes:
    return struct.pack('>I', length)

# Auth payload pubkey_size: always little-endian
def encode_auth_pubkey_size(size: int) -> bytes:
    return struct.pack('<I', size)

# Signing input: ttl LE, timestamp LE
def encode_signing_input_ttl(ttl: int) -> bytes:
    return struct.pack('<I', ttl)

def encode_signing_input_timestamp(ts: int) -> bytes:
    return struct.pack('<Q', ts)
```

Write a comprehensive cross-language test: generate a signing input in C++ (via the existing unit tests or a small harness), capture the raw bytes, then verify Python produces identical bytes for the same input.

**Warning signs:**
- AEAD decrypt fails on the very first encrypted message (auth exchange) -- likely nonce or key derivation endianness wrong
- Signatures verify locally in Python but the server rejects them -- likely signing input endianness wrong
- WriteAck arrives but blob_hash/seq_num parse to nonsense values -- response payload endianness wrong

**Phase to address:**
Phase 1 (Transport layer). Encode/decode utilities must be locked down with byte-level tests before building anything on top.

---

### Pitfall 2: AEAD Nonce Counter Desynchronization

**What goes wrong:**
The send and receive counters drift between the Python SDK and the relay/node. Every subsequent message fails AEAD decryption, and the connection is lost. The root cause is subtle: any message that is encrypted but not sent (or sent but not counted) breaks all subsequent communication.

The C++ server increments `send_counter_` in `send_encrypted()` and `recv_counter_` in `recv_encrypted()` -- once per call, unconditionally. The PQ handshake auth exchange uses counters 0 (both directions), then the message loop starts at counter 1. The lightweight handshake does NOT use the auth counter -- the message loop starts at counter 0.

**Why it happens:**
Multiple causes:
1. **Handshake path confusion:** PQ handshake consumes nonce 0 for auth in each direction. Lightweight handshake does NOT -- session starts at nonce 0. SDK must track which path was used and set initial counters accordingly.
2. **Error handling that skips counter increment:** If the SDK encrypts a message, increments the counter, but the TCP send fails and the SDK retries, the counter is now ahead. Or if it reads a frame, increments recv_counter, but the frame is malformed and gets discarded -- counter is still incremented.
3. **Python's GIL and async interaction:** If using asyncio and accidentally allowing two concurrent sends, the counter increment is not atomic -- two coroutines could grab the same counter value.

**How to avoid:**
- Track counters as a single atomic integer per direction, always incremented exactly once per encrypt/decrypt call, regardless of success/failure (matching the C++ behavior)
- After PQ handshake: initial send_counter = 1, recv_counter = 1 (auth consumed 0)
- After lightweight handshake: initial send_counter = 0, recv_counter = 0 (no auth exchange)
- Never retry encryption with a new nonce -- if send fails, close the connection
- Use a single-writer pattern for send operations (asyncio.Lock or queue)

**Warning signs:**
- First message after handshake works, second fails -- counter off-by-one from handshake path
- Intermittent AEAD failures under concurrent request load -- race condition on counter
- Connection works for exactly N messages then fails -- counter initialized wrong, coincidentally aligned for N frames

**Phase to address:**
Phase 1 (Transport layer). The AEAD framing layer must be tested against the running relay with a known-good message sequence before building higher layers.

---

### Pitfall 3: HKDF Key Derivation Parameter Mismatch (PQ vs Lightweight Paths)

**What goes wrong:**
The SDK derives different session keys than the relay, causing every encrypted message to fail. The HKDF parameters differ fundamentally between the PQ and lightweight handshake paths, and the PROTOCOL.md documentation has a known discrepancy with the actual C++ implementation.

**Actual C++ behavior (source of truth, verified in handshake.cpp):**

**PQ path:**
- HKDF IKM = ML-KEM shared secret (raw)
- HKDF salt = **empty** (not SHA3-256 of pubkeys as PROTOCOL.md states)
- HKDF info strings: `"chromatin-init-to-resp-v1"`, `"chromatin-resp-to-init-v1"`
- Session fingerprint = SHA3-256(shared_secret || initiator_signing_pk || responder_signing_pk)

**Lightweight path:**
- HKDF IKM = initiator_nonce || responder_nonce (64 bytes)
- HKDF salt = initiator_signing_pk || responder_signing_pk (5184 bytes, raw concatenation)
- HKDF info strings: same as PQ path
- Session fingerprint = SHA3-256(IKM || salt) = SHA3-256(nonces || pubkeys)

**Why it happens:**
The PROTOCOL.md says the PQ path uses `SHA3-256(initiator_signing_pubkey || responder_signing_pubkey)` as the HKDF salt. The actual code uses empty salt. An SDK developer reading only PROTOCOL.md will implement the wrong derivation. Additionally, the two paths have completely different IKM/salt structures -- the lightweight path packs pubkeys into the salt (5184 bytes), while the PQ path uses no salt at all.

**How to avoid:**
- Implement from the C++ source code (`db/net/handshake.cpp`), NOT from PROTOCOL.md alone
- Use pysodium's `crypto_kdf_hkdf_sha256_extract()` and `crypto_kdf_hkdf_sha256_expand()` to exactly match the libsodium calls
- For PQ path: pass empty salt (`b""`) to extract, shared_secret as IKM
- For lightweight path: pass concatenated pubkeys as salt, concatenated nonces as IKM
- Write a test that performs the HKDF derivation for known inputs and compares against C++ test vector output

**Warning signs:**
- Handshake auth message decryption fails (wrong key derived)
- Lightweight handshake fails but PQ works (or vice versa) -- path-specific parameter bug
- Keys appear correct length (32 bytes) but every AEAD operation fails -- subtle IKM/salt swap

**Phase to address:**
Phase 1 (Transport layer). Must be the very first thing validated -- the entire security of the session depends on correct key derivation. Capture test vectors from C++ unit tests.

---

### Pitfall 4: Canonical Signing Input Byte-for-Byte Mismatch

**What goes wrong:**
Python SDK produces blobs with valid-looking signatures that the C++ node always rejects. The canonical signing input `SHA3-256(namespace_id || data || ttl_le32 || timestamp_le64)` must produce the identical 32-byte digest on both sides. Any difference in concatenation order, endianness, or padding means the ML-DSA-87 signature computed by Python will never verify on the C++ side.

**Why it happens:**
The signing input is NOT the FlatBuffer encoding -- it is a custom canonical concatenation. The C++ code uses incremental SHA3-256 (liboqs `OQS_SHA3_sha3_256_inc_*`) to feed the fields directly into the sponge without intermediate allocation. Python must produce the exact same byte sequence before hashing.

Specific traps:
1. **ttl is uint32 little-endian (4 bytes):** Python's `struct.pack('<I', ttl)` -- not `'>I'`, not `'<Q'`
2. **timestamp is uint64 little-endian (8 bytes):** Python's `struct.pack('<Q', timestamp)` -- not seconds-as-float, not nanoseconds
3. **namespace_id is exactly 32 bytes:** Must be SHA3-256 of the signing public key, not the hex string
4. **data is raw bytes:** The application payload, not base64 or hex encoded
5. **Concatenation order matters:** namespace_id THEN data THEN ttl THEN timestamp, no separators

**How to avoid:**
Build and test the signing input before touching ML-DSA. Write a test that:
1. Uses known values: namespace_id (32 zero bytes), data (b"hello"), ttl (3600), timestamp (1000000)
2. Manually constructs: `b'\x00'*32 + b'hello' + struct.pack('<I', 3600) + struct.pack('<Q', 1000000)`
3. Hashes with `hashlib.sha3_256()`
4. Compares against the C++ `build_signing_input()` output for the same inputs

Cross-language test vectors are non-negotiable here. Generate them from the C++ test suite.

**Warning signs:**
- Server responds with no WriteAck and disconnects -- signature verification failed, ingest rejected
- Identical data produces different blob_hash values between Python and C++ -- the FlatBuffer encoding differs (separate issue) but this means dedup will not work

**Phase to address:**
Phase 2 (Blob operations). Signing input construction must be validated with cross-language test vectors before implementing any write operations.

---

### Pitfall 5: FlatBuffers Cross-Language Encoding Non-Determinism

**What goes wrong:**
Python's FlatBuffer encoder produces valid but differently-ordered bytes than C++, causing two problems: (a) blob_hash (SHA3-256 of the encoded FlatBuffer) differs between Python and C++ for the same logical blob, breaking dedup and content-addressing; (b) the server cannot find blobs written by the SDK because the hash used as the storage key differs.

**Why it happens:**
FlatBuffers does NOT guarantee deterministic encoding across different language implementations. The format allows flexibility in vtable layout, field ordering within the buffer, and string/vector placement. Even with `ForceDefaults(True)` set on both sides, the Python and C++ builders may produce different byte layouts for the same logical message.

The C++ code uses `builder.ForceDefaults(true)` and creates vectors/fields in a specific order (ns, pk, dt, sg, then CreateBlob with ttl/timestamp). The Python builder must use the exact same field creation order AND `builder.ForceDefaults(True)`.

But even with identical API call order, the Python FlatBuffers runtime may use different internal allocation strategies, producing valid but byte-different buffers.

**How to avoid:**
Two strategies (use both):

1. **For TransportMessage encoding:** Non-determinism is acceptable because the payload is opaque bytes and the message is not hashed. The server decodes the FlatBuffer, extracts type/payload/request_id, and discards the envelope.

2. **For Blob encoding (critical):** The blob_hash = SHA3-256(encoded_flatbuffer). If Python encodes differently, the hash differs. The solution is to either:
   - (a) Ensure the Python SDK uses the exact same field creation order and verifies byte-identical output against C++ test vectors, OR
   - (b) Have the server compute the blob_hash on its side (which it already does -- the node hashes the received FlatBuffer bytes, not a re-encoded version). So the SDK must store/reference the hash that the node returns in WriteAck, not a locally-computed hash.

   Option (b) is the safe path. The SDK should:
   - Send the blob, receive WriteAck with the server-computed blob_hash
   - Use that hash for all subsequent ReadRequest/ExistsRequest/DeleteRequest operations
   - Never assume locally-computed FlatBuffer hash matches the server's hash

**Warning signs:**
- WriteAck returns a blob_hash different from what Python computed locally
- ReadRequest with locally-computed hash returns "not found" but the blob exists
- ExistsRequest returns false for a blob the SDK just wrote

**Phase to address:**
Phase 2 (Blob operations). The blob encoding must be tested for hash consistency, and the SDK API should be designed to always use server-returned hashes.

---

### Pitfall 6: liboqs Version Mismatch Between Python and C++ Sides

**What goes wrong:**
The Python SDK uses a different version of liboqs than the C++ relay/node, causing incompatible key formats, signature sizes, or algorithm parameter changes. ML-DSA and ML-KEM went through NIST standardization with parameter changes between draft and final versions. A version mismatch means keys generated by one side cannot be used by the other.

**Why it happens:**
- liboqs-python is installed via pip and may pull a different liboqs version than the one built by the C++ project's CMake FetchContent
- NIST FIPS 204 (ML-DSA) and FIPS 203 (ML-KEM) finalization changed algorithm identifiers and potentially internal parameters between liboqs versions
- liboqs 0.12+ added context string support for ML-DSA; the C++ server does NOT use context strings (calls `OQS_SIG_sign` without context). If the Python side accidentally uses `sign_with_ctx_str`, signatures are incompatible.
- Algorithm names changed: "Dilithium5" became "ML-DSA-87", "Kyber1024" became "ML-KEM-1024" across liboqs versions

**How to avoid:**
- Pin the exact same liboqs version in both the Python SDK requirements and the C++ CMakeLists.txt
- Use the NIST final algorithm names: `"ML-DSA-87"` for signatures, `"ML-KEM-1024"` for KEM
- Explicitly use `Signature.sign()` (without context), NOT `sign_with_ctx_str()`
- Add a CI test that generates an ML-DSA-87 keypair in Python, signs a message, and verifies in C++ (and vice versa)
- Check `OQS_SIG_ml_dsa_87_length_signature` matches between both builds (should be 4627 bytes max)

**Warning signs:**
- `ImportError` or `ValueError` when instantiating `oqs.Signature("ML-DSA-87")` -- wrong algorithm name for the installed version
- Signature length differs from expected 4627 bytes
- Public key length differs from expected 2592 bytes
- KEM ciphertext length differs from expected 1568 bytes
- Handshake fails at auth verification despite correct key derivation

**Phase to address:**
Phase 0 (Project setup / dependency management). Version pinning must happen before any crypto code is written.

---

### Pitfall 7: TCP Stream Framing Incomplete Reads

**What goes wrong:**
The Python SDK reads partial frames from the TCP socket, attempts to decrypt incomplete ciphertext, and gets AEAD authentication failure. Or worse, it reads across a frame boundary, concatenating part of the next message into the current one.

**Why it happens:**
TCP is a stream protocol, not a message protocol. A single `socket.recv(4096)` call may return:
- Less than 4 bytes (partial length header)
- The header plus partial ciphertext
- Multiple complete frames concatenated
- Any split point within the data

Python's `socket.recv()` returns whatever is available, not a complete message. Developers used to HTTP libraries or higher-level protocols forget this.

**How to avoid:**
Implement a strict framing layer that:
1. Reads exactly 4 bytes for the length prefix (loop until complete)
2. Reads exactly `length` bytes of ciphertext (loop until complete)
3. Decrypts exactly that ciphertext
4. Returns the plaintext

```python
async def recv_frame(reader: asyncio.StreamReader) -> bytes:
    header = await reader.readexactly(4)  # raises IncompleteReadError on EOF
    length = struct.unpack('>I', header)[0]
    if length > MAX_FRAME_SIZE:
        raise ProtocolError(f"frame too large: {length}")
    ciphertext = await reader.readexactly(length)
    return ciphertext
```

Use `asyncio.StreamReader.readexactly()` -- it handles partial reads internally. Never use `read()` or `recv()` with a size hint.

**Warning signs:**
- Intermittent AEAD failures that correlate with message size or network latency
- Works on localhost but fails over real network (Nagle's algorithm splits packets differently)
- Works for small messages, fails for large blobs (more likely to be split across TCP segments)

**Phase to address:**
Phase 1 (Transport layer). The framing layer is the foundation -- must be rock-solid before anything else.

---

### Pitfall 8: PQ Handshake Message Sequence Confusion (Raw vs Encrypted)

**What goes wrong:**
The Python SDK encrypts a handshake message that should be sent raw, or sends raw a message that should be encrypted. The server cannot parse it and silently disconnects.

**Why it happens:**
The PQ handshake uses a mixed sequence:
1. **KemPubkey (raw):** Initiator sends unencrypted FlatBuffer `[4B len][TransportMessage]`
2. **KemCiphertext (raw):** Responder sends unencrypted FlatBuffer `[4B len][TransportMessage]`
3. **AuthSignature (encrypted):** Initiator sends `[4B len][AEAD ciphertext of TransportMessage]`
4. **AuthSignature (encrypted):** Responder sends `[4B len][AEAD ciphertext of TransportMessage]`

Messages 1-2 are raw TransportMessage FlatBuffers with length prefix. Messages 3-4 are AEAD-encrypted TransportMessage FlatBuffers with length prefix. The framing is identical (`[4B BE len][data]`) -- only the data content differs (plaintext FlatBuffer vs AEAD ciphertext).

Additionally, the KemPubkey payload contains BOTH the KEM public key AND the signing public key concatenated: `[kem_pk:1568][signing_pk:2592]` = 4160 bytes total. The KemCiphertext response also bundles: `[ciphertext:1568][signing_pk:2592]` = 4160 bytes. Missing the concatenated signing pubkey means the salt/fingerprint derivation is wrong.

**How to avoid:**
Implement the handshake as an explicit state machine:
```python
class HandshakeState(Enum):
    SEND_KEM_PUBKEY = 1      # send raw
    RECV_KEM_CIPHERTEXT = 2  # recv raw
    SEND_AUTH = 3             # send encrypted
    RECV_AUTH = 4             # recv encrypted
    COMPLETE = 5
```

Each state knows whether to use raw or encrypted I/O. Never allow calling the wrong I/O method for a given state.

**Warning signs:**
- Handshake hangs (server waiting for message, SDK sent wrong format)
- Server logs "AEAD decrypt failed" during step 3 -- step 3 was sent raw instead of encrypted
- Server logs "Invalid FlatBuffer" during step 1 -- step 1 was encrypted when it should be raw

**Phase to address:**
Phase 1 (Transport layer, handshake implementation). State machine must be explicit and tested step-by-step.

---

### Pitfall 9: PyNaCl vs pysodium vs cffi Bindings for HKDF-SHA256

**What goes wrong:**
The SDK developer uses PyNaCl (the popular high-level libsodium binding) only to discover it does not expose `crypto_kdf_hkdf_sha256_extract()` and `crypto_kdf_hkdf_sha256_expand()` at the Python level. PyNaCl wraps a curated subset of libsodium, and HKDF-SHA256 is not in that subset. The developer either uses a different KDF (wrong output) or implements HKDF manually (error-prone).

**Why it happens:**
PyNaCl focuses on high-level APIs (SecretBox, SealedBox, etc.) and does not expose all low-level libsodium functions. The HKDF functions were added to libsodium in v1.0.19 and are available in pysodium (which wraps all of libsodium via ctypes) but NOT in PyNaCl's high-level API.

**How to avoid:**
Use **pysodium** (not PyNaCl) for all libsodium operations:
- `pysodium.crypto_kdf_hkdf_sha256_extract(salt, ikm)` -- matches the C++ `crypto::KDF::extract()`
- `pysodium.crypto_kdf_hkdf_sha256_expand(prk, info, output_len)` -- matches `crypto::KDF::expand()`
- `pysodium.crypto_aead_chacha20poly1305_ietf_encrypt(msg, ad, nonce, key)` -- matches AEAD encrypt
- `pysodium.crypto_aead_chacha20poly1305_ietf_decrypt(ciphertext, ad, nonce, key)` -- matches AEAD decrypt

Alternatively, use Python's `cryptography` library which has HKDF-SHA256, but ensure you call it with the exact same extract-then-expand two-step process (not the one-shot `HKDF` which may compute differently).

**Warning signs:**
- `AttributeError: module 'nacl' has no attribute 'crypto_kdf_hkdf_sha256_extract'`
- Using `hmac` + `hashlib` for manual HKDF produces different PRK than libsodium (edge cases with empty salt handling)
- Mixed libraries (PyNaCl for AEAD, pysodium for HKDF) may link different libsodium versions

**Phase to address:**
Phase 0 (Dependency selection). Choose pysodium from the start and use it consistently.

---

### Pitfall 10: FlatBuffers Blob Field Ordering for Deterministic Encoding

**What goes wrong:**
The Python SDK creates FlatBuffer Blob fields in a different order than the C++ encoder, producing a valid but byte-different FlatBuffer. This means the blob_hash (SHA3-256 of the entire encoded FlatBuffer) computed by the SDK will not match the hash the server would compute if it re-encoded the same blob.

**Why it happens:**
The C++ `encode_blob()` creates vectors and the Blob table in this exact order:
1. `CreateVector(namespace_id)`
2. `CreateVector(pubkey)`
3. `CreateVector(data)`
4. `CreateVector(signature)`
5. `CreateBlob(builder, ns, pk, dt, ttl, timestamp, sg)`
6. `builder.Finish(fb_blob)`

FlatBuffers builds the buffer back-to-front. Vectors must be created before the table that references them. The order of vector creation affects their placement in the buffer, which affects the final byte layout.

If the Python SDK creates vectors in a different order (e.g., signature before data), the buffer will be valid FlatBuffers but produce a different SHA3-256 hash.

**How to avoid:**
- Mirror the exact C++ field creation order in Python
- Set `builder.ForceDefaults(True)` before creating any fields
- Write a cross-language test: encode the same Blob in both C++ and Python, compare the raw bytes
- If byte-identical encoding cannot be achieved, design the SDK to never compute blob_hash locally -- always use the server-returned hash from WriteAck

**Warning signs:**
- Locally computed blob_hash does not match WriteAck's blob_hash
- FlatBuffer Verifier passes but hashes differ

**Phase to address:**
Phase 2 (Blob operations). Cross-language encoding test is required before first write operation.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Use `cryptography` lib HKDF instead of pysodium | Fewer deps, familiar API | Potential extract/expand behavior differences with libsodium, separate code paths for AEAD vs KDF | Never -- use pysodium for both AEAD and HKDF |
| Skip FlatBuffer determinism testing | Ship faster | Silent hash mismatches, broken dedup, data loss via phantom "not found" | Never -- must validate before write operations |
| Hardcode message type enum values | Quick implementation | Breaks when server adds new types, no forward compatibility | MVP only -- replace with generated code from .fbs schema |
| Use synchronous socket I/O | Simpler code | Cannot handle pub/sub notifications while sending requests, blocks on large reads | Phase 1 prototype only -- must be async for pub/sub |
| Compute blob_hash locally | Avoid wait for WriteAck | Hash mismatch if FlatBuffer encoding differs | Never -- always use server-returned hash |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| liboqs-python | Using old algorithm names ("Dilithium5", "Kyber1024") | Use NIST final names: "ML-DSA-87", "ML-KEM-1024" |
| liboqs-python | Calling `sign_with_ctx_str()` (with context) | Use `sign()` (without context) -- server uses contextless signing |
| liboqs-python | Assuming `encap_secret()` returns `(shared_secret, ciphertext)` | Returns `(ciphertext, shared_secret)` -- tuple order matters |
| pysodium HKDF | Passing `None` for empty salt | Pass `b""` (empty bytes) -- pysodium may not handle None gracefully |
| pysodium AEAD | Omitting associated data parameter | Pass `b""` for AD -- the server uses empty AD, not None |
| pysodium AEAD | Using XChaCha20 variant | Must use IETF ChaCha20-Poly1305 (12-byte nonce), not XChaCha20 (24-byte nonce) |
| FlatBuffers Python | Forgetting `builder.ForceDefaults(True)` | Must be called after creating Builder, before adding any fields |
| FlatBuffers Python | Using `flatc --python` with wrong schema version | Generate Python bindings from the exact same .fbs files used by C++ build |
| hashlib SHA3 | Using SHA-256 instead of SHA3-256 | `hashlib.sha3_256()` not `hashlib.sha256()` -- they are different algorithms |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Synchronous crypto on main thread | UI/event loop freezes during ML-DSA sign (>10ms for 100 MiB blobs) | Run crypto in executor: `await loop.run_in_executor(None, sign_blob)` | Blobs > 1 MiB |
| Allocating new FlatBufferBuilder per message | GC pressure, memory fragmentation | Reuse builder with `builder.Reset()` between messages | >100 messages/sec |
| Re-reading full blob to compute hash | Double memory for large blobs | Hash the encoded FlatBuffer bytes in a streaming fashion, or use server-returned hash | Blobs > 10 MiB |
| Buffering entire response before processing | OOM on BatchReadResponse with large blobs | Process blob entries incrementally from the response payload | Batch responses with >10 MiB total |
| Creating new TCP connection per request | Connection overhead dominates (PQ handshake is ~10ms) | Connection pooling with persistent sessions | >10 requests/sec |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Storing ML-DSA-87 private key in plaintext file | Key theft = namespace impersonation | Use OS keyring or encrypted file with secure erase after loading |
| Logging raw key bytes or shared secrets | Key material in log files | Never log crypto material; log only key fingerprints (SHA3-256 of pubkey, truncated) |
| Not zeroing secret key memory after use | Key material remains in Python heap | Use `oqs.Signature` context manager (calls `__del__` which calls `OQS_SIG_free`); for pysodium keys, overwrite with zeros |
| Accepting any server public key without verification | MITM during handshake | SDK must pin or verify relay's signing public key against known-good value |
| Reusing AEAD nonce across connections | Nonce reuse breaks ChaCha20-Poly1305 security | Counters are per-connection, starting from 0/1 -- never share state between connections |
| Using system time for blob timestamps without NTP | Timestamp validation rejection (1h future / 30d past thresholds) | Document that system clock must be roughly correct; consider using server time from NodeInfoResponse |

## UX Pitfalls

| Pitfall | User Impact | Better Approach |
|---------|-------------|-----------------|
| Exposing raw byte arrays for namespace_id | Developer must manually SHA3-256 hash their pubkey | Compute namespace_id automatically from the identity's public key |
| Requiring manual nonce/counter management | Off-by-one errors crash the connection | Hide counters entirely inside the transport layer |
| Synchronous connect + handshake | Application hangs during startup if relay is slow | Async connect with configurable timeout and clear error messages |
| Exposing FlatBuffer internals in public API | Leaky abstraction, version coupling | Return Python dataclasses/NamedTuples from decode, accept them for encode |
| Silent failure on auth rejection | Developer does not know why connection dropped | Raise specific exceptions: `AuthenticationError`, `AccessDeniedError`, `PQHandshakeError` |
| No connection state visibility | Hard to debug "why is my SDK not working" | Provide `connection.state` property: CONNECTING, HANDSHAKE, AUTHENTICATED, CLOSED |

## "Looks Done But Isn't" Checklist

- [ ] **PQ Handshake:** Tested with real relay, not just unit tests -- verify both PQ and lightweight paths
- [ ] **AEAD framing:** Tested with messages larger than TCP MSS (~1460 bytes) -- ensures framing handles split reads
- [ ] **Signing input:** Cross-validated against C++ test vector with non-trivial data (large blob, non-zero TTL, realistic timestamp)
- [ ] **FlatBuffer encoding:** Blob hash compared between Python and C++ for identical input data
- [ ] **Pub/sub:** Notification handler works while request/response is in flight -- requires async architecture
- [ ] **request_id correlation:** Responses arrive out of order and are correctly matched -- requires pending-request map
- [ ] **Reconnection:** SDK handles relay restart gracefully -- re-handshake, re-subscribe, reset counters
- [ ] **Large blobs:** Successfully write and read a 100 MiB blob -- tests memory handling, frame size limits, timeout behavior
- [ ] **Error responses:** SDK handles StorageFull, QuotaExceeded, SyncRejected gracefully -- not just success paths
- [ ] **Connection cleanup:** No resource leaks on disconnect -- sockets closed, crypto state zeroed

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| AEAD nonce desync | LOW | Close connection, reconnect, re-handshake (counters reset to 0/1) |
| Wrong HKDF parameters | MEDIUM | Fix derivation code, re-test against test vectors, reconnect |
| FlatBuffer hash mismatch | MEDIUM | Switch to server-returned hashes only; if local hash needed, capture C++ test vectors |
| liboqs version mismatch | HIGH | Pin version, rebuild, re-test all crypto operations, may need to regenerate all keys |
| Signing input endianness wrong | MEDIUM | Fix struct.pack format strings, re-validate with test vectors, re-sign affected blobs |
| Mixed endianness in payload parsing | LOW | Fix specific pack/unpack calls, add unit tests per message type |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Mixed endianness | Phase 1 (Transport) | Byte-level tests for every encode/decode function, cross-language test vectors |
| AEAD nonce desync | Phase 1 (Transport) | Integration test: 100-message exchange with relay, verify no decrypt failures |
| HKDF parameter mismatch | Phase 1 (Transport) | Test vector comparison: known inputs -> known derived keys |
| Signing input mismatch | Phase 2 (Blob ops) | Cross-language test: same input -> same 32-byte digest |
| FlatBuffer non-determinism | Phase 2 (Blob ops) | Byte comparison of encoded blobs, or always use server hash |
| liboqs version mismatch | Phase 0 (Setup) | CI test: Python sign -> C++ verify roundtrip |
| TCP framing | Phase 1 (Transport) | Test with large messages (> MSS) over real TCP, not loopback only |
| Handshake message sequence | Phase 1 (Transport) | State machine test: step through each state, verify raw vs encrypted |
| PyNaCl vs pysodium | Phase 0 (Setup) | Import test for all required functions at project init |
| FlatBuffer field ordering | Phase 2 (Blob ops) | Cross-language blob encoding comparison |

## Sources

- C++ source: `db/net/handshake.cpp` -- actual HKDF derivation, session fingerprint computation, auth payload encoding (LE pubkey_size) [HIGH confidence]
- C++ source: `db/net/framing.cpp` -- nonce format (4 zero + 8-byte BE counter), frame format (4-byte BE length + ciphertext) [HIGH confidence]
- C++ source: `db/net/connection.cpp` -- send_encrypted/recv_encrypted counter behavior, handshake state machine, TrustedHello payload format [HIGH confidence]
- C++ source: `db/wire/codec.cpp` -- build_signing_input with LE ttl/timestamp, FlatBuffer encode order with ForceDefaults [HIGH confidence]
- C++ source: `db/net/protocol.cpp` -- TransportCodec FlatBuffer encode with ForceDefaults(true) [HIGH confidence]
- `db/PROTOCOL.md` -- wire format reference (NOTE: PQ handshake salt description differs from code) [HIGH confidence for transport/payload formats, MEDIUM for HKDF salt]
- [liboqs-python API](https://github.com/open-quantum-safe/liboqs-python) -- Signature.sign(), KeyEncapsulation.encap_secret() method signatures [HIGH confidence]
- [pysodium HKDF-SHA256 support](https://github.com/stef/pysodium) -- crypto_kdf_hkdf_sha256_extract/expand available since libsodium 1.0.19 [HIGH confidence]
- [FlatBuffers Python builder](https://github.com/google/flatbuffers/blob/master/python/flatbuffers/builder.py) -- ForceDefaults method [HIGH confidence]
- [FlatBuffers deterministic encoding discussion](https://groups.google.com/g/flatbuffers/c/v2RkM3KB1Qw) -- encoding NOT guaranteed deterministic across languages [MEDIUM confidence]
- [libsodium IETF ChaCha20-Poly1305 docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction) -- nonce format, key/nonce/tag sizes [HIGH confidence]
- [Python struct module](https://docs.python.org/3/library/struct.html) -- format strings for endianness-explicit packing [HIGH confidence]

---
*Pitfalls research for: Python SDK client for chromatindb PQ-crypto binary protocol*
*Researched: 2026-03-28*
