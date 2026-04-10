# Phase 103: UDS Multiplexer & Protocol Translation - Research

**Researched:** 2026-04-10
**Domain:** UDS transport, FlatBuffers codec, request multiplexing, JSON<->binary translation
**Confidence:** HIGH

## Summary

Phase 103 connects the relay to the node via a single multiplexed UDS connection and implements bidirectional JSON<->binary protocol translation. The relay must implement the TrustedHello handshake (including HKDF key derivation and AEAD encryption), a TransportCodec for FlatBuffers transport envelopes, a request_id-based multiplexer for routing responses to the correct WebSocket client, and a table-driven translator that converts JSON messages to binary payloads (and vice versa) using the FieldSpec metadata from Phase 102.

**Critical finding: CONTEXT.md D-03 is wrong.** It states "After TrustedHello, the UDS connection is unencrypted (AEAD nonce counter never starts)." The actual node behavior (verified in `db/net/connection.cpp` and `db/PROTOCOL.md`) is that UDS connections use the SAME AEAD-encrypted frame format as TCP after TrustedHello. Session keys are derived via HKDF-SHA256 from exchanged nonces and signing pubkeys. All post-handshake messages are encrypted with ChaCha20-Poly1305. The relay MUST implement HKDF + AEAD for the UDS connection to work.

**Additional finding: Several FieldSpec schemas from Phase 102 are incomplete.** ListRequest is missing `since_seq` (u64BE), NamespaceListRequest is missing `after_namespace` (32-byte cursor) and `limit` (u32BE), MetadataResponse has a different structure than described, and several response types (BatchExistsResponse, DelegationListResponse, PeerInfoResponse, ListResponse) have compound/variable-length binary formats that the flat FieldSpec model cannot fully describe. Phase 103 must fix these schema gaps during translator implementation.

**Primary recommendation:** Split into two plans: (1) UDS connection manager with TrustedHello handshake + AEAD + TransportCodec + RequestRouter, (2) Table-driven translator with schema corrections + FlatBuffer special cases + WsSession integration.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Relay connects via `asio::local::stream_protocol::socket` directly -- NOT via db/net/Connection
- D-02: Relay implements TrustedHello exchange itself (simplified version per D-02 description, but ACTUAL protocol requires full handshake -- see Critical Finding above)
- D-03: UDS connection described as "unencrypted" -- INCORRECT per actual node behavior (see Summary)
- D-04: Relay starts WS acceptor immediately; UDS connect async with jittered backoff (1s base, 30s cap); clients get node_unavailable error before UDS ready
- D-05: Relay uses its ML-DSA-87 identity for TrustedHello handshake
- D-06: UDS read loop decodes TransportMessage FlatBuffers, dispatches to RequestRouter
- D-07: RequestRouter counter starts at 1 (0 reserved), wraps at UINT32_MAX
- D-08: Pending request map: unordered_map<uint32_t, PendingRequest> with client_session_id, client_request_id, created_timestamp
- D-09: Fire-and-forget messages (Ping, Goodbye) bypass RequestRouter; Subscribe/Unsubscribe forwarded directly (Phase 104 adds aggregation)
- D-10: Client disconnect purges all pending entries
- D-11: 60s stale entry cleanup sweep
- D-12: Requests without client request_id still get relay_rid assigned
- D-13: Table-driven generic encoder/decoder using FieldSpec metadata -- no per-type handler functions
- D-14: Non-FlatBuffer types (35 of 40) use flat concatenation of fields in FieldSpec order
- D-15: FlatBuffer types (Data=8, ReadResponse=32, BatchReadResponse=54) need special-case handling
- D-16: .fbs schemas copied into relay/wire/, compiled locally; relay does NOT link against db/wire/
- D-17: Data message JSON format with hex/base64 encoding conventions
- D-18: ReadResponse translation: [status_byte][flatbuffer_blob_data] -> JSON with base64 data
- D-19: BatchReadResponse translation with JSON array of blobs
- D-20: ReadResponse and BatchReadResponse always sent as binary WS frames (opcode 0x2)
- D-21: Binary frame payload IS still JSON with base64 data fields
- D-22: WsSession on_message AUTHENTICATED path is the entry point
- D-23: UDS receive path: decode -> resolve -> translate -> send
- D-24: Translation functions in relay/translate/translator.h/cpp (stateless, pure)
- D-25: UDS multiplexer in relay/core/uds_multiplexer.h/cpp, request router in relay/core/request_router.h/cpp

### Claude's Discretion
- Internal API design for UdsMultiplexer, RequestRouter, and translator functions
- TrustedHello handshake implementation details (byte-level encoding)
- Exact error messages for edge cases
- Test organization within relay/tests/
- Whether to add send_binary() to WsSession or reuse write_frame with opcode parameter

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| MUX-01 | Single multiplexed UDS connection from relay to node | UDS lifecycle pattern, TrustedHello handshake protocol fully documented, AEAD encryption requirement discovered |
| MUX-02 | Relay-scoped request_id allocation with client-to-relay mapping for response routing | RequestRouter design pattern, pending map, stale cleanup, fire-and-forget routing rules |
| PROT-01 | Table-driven JSON to FlatBuffers translation for all 38 relay-allowed message types | FieldSpec schema gaps identified, compound binary formats documented, FlatBuffer special cases enumerated |
| PROT-04 | Binary WebSocket frames for large payloads (ReadResponse, BatchReadResponse) | WsSession write_frame currently hardcodes OPCODE_TEXT, needs opcode parameter or send_binary method |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | UDS socket, async I/O, coroutines | Already in relay; provides local::stream_protocol::socket |
| FlatBuffers | 25.2.10 | Transport envelope codec, blob encode/decode | Already in db/ build; relay copies .fbs + _generated.h |
| OpenSSL | 3.3+ (system) | HKDF-SHA256, ChaCha20-Poly1305 AEAD, EVP API | Already a relay dependency; provides all crypto primitives needed for UDS handshake |
| liboqs | 0.15.0 | SHA3-256 (OQS_SHA3_sha3_256), ML-DSA-87 signing | Already in relay for auth; SHA3-256 needed for session fingerprint |
| nlohmann/json | 3.11.3 | JSON parse/serialize for translator | Already in relay |
| Catch2 | 3.7.1 | Unit test framework | Already in relay tests |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| spdlog | 1.15.1 | Structured logging for UDS lifecycle events | Already in relay; log connect/disconnect/handshake/errors |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| OpenSSL HKDF/AEAD | libsodium | Would add new dependency; OpenSSL already linked, has same primitives |
| Copied _generated.h | flatc at build time | Would need CMake custom_command + flatc binary; pre-generated headers are simpler and match db/ pattern |

**Installation:**
No new dependencies needed. All libraries already in relay CMakeLists.txt or can use existing FetchContent targets.

**FlatBuffers addition to relay CMake:**
```cmake
# Reuse FlatBuffers from root build, or fetch independently
if(NOT TARGET flatbuffers)
  FetchContent_Declare(flatbuffers_relay
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.2.10
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(flatbuffers_relay)
endif()
```

## Architecture Patterns

### Recommended Project Structure (Phase 103 additions)
```
relay/
  core/
    uds_multiplexer.h/cpp    # Single UDS connection to node
    request_router.h/cpp     # request_id mapping + demux
  translate/
    translator.h/cpp         # Table-driven JSON <-> binary codec
  util/
    hex.h                    # Shared to_hex/from_hex (extracted from ws_session.cpp + relay_main.cpp)
    base64.h/cpp             # Base64 encode/decode using OpenSSL EVP
    endian.h                 # BE read/write helpers (relay's own, not db/util/endian.h)
  wire/
    transport_generated.h    # Copied from db/wire/ (TransportMessage)
    blob_generated.h         # Copied from db/wire/ (Blob)
    transport_codec.h/cpp    # Relay's own TransportCodec (encode/decode envelope)
    blob_codec.h/cpp         # Relay's own encode_blob/decode_blob
  tests/
    test_translator.cpp      # Table-driven translation unit tests
    test_request_router.cpp  # RequestRouter unit tests
    test_uds_handshake.cpp   # TrustedHello + AEAD handshake tests
    test_transport_codec.cpp # TransportCodec encode/decode tests
```

### Pattern 1: UDS Connection with Full TrustedHello Handshake

**What:** The relay opens a UDS socket and performs the full TrustedHello handshake: nonce exchange, HKDF key derivation, auth signature exchange over the encrypted channel. All subsequent messages are AEAD-encrypted.

**When to use:** Always -- this is how the node's UDS acceptor works. There is no unencrypted UDS path.

**Handshake protocol (from db/net/connection.cpp):**
1. Relay sends TrustedHello: `TransportCodec::encode(TrustedHello, [nonce:32][signing_pubkey:2592])`
   - Frame: `[4B BE length][FlatBuffer TransportMessage]` (raw, unencrypted)
2. Node responds TrustedHello: `TransportCodec::encode(TrustedHello, [nonce:32][signing_pubkey:2592])`
3. Both sides derive session keys via HKDF-SHA256:
   - IKM = initiator_nonce || responder_nonce (64 bytes)
   - Salt = initiator_signing_pk || responder_signing_pk (5184 bytes)
   - PRK = HKDF-Extract(salt, IKM)
   - init_to_resp_key = HKDF-Expand(PRK, "chromatin-init-to-resp-v1", 32)
   - resp_to_init_key = HKDF-Expand(PRK, "chromatin-resp-to-init-v1", 32)
   - session_fingerprint = SHA3-256(IKM || Salt)
4. Auth exchange (encrypted from here):
   - Relay sends encrypted auth: `[4B BE len][AEAD(TransportCodec::encode(AuthPubkey, auth_payload))]`
   - Auth payload: `[pubkey_size:2 LE][pubkey:2592][signature:4627]` (signature signs session_fingerprint)
   - Node sends encrypted auth response (same format)
5. Post-handshake: all messages are `[4B BE len][AEAD(TransportCodec::encode(type, payload, request_id))]`

**AEAD details:**
- Algorithm: ChaCha20-Poly1305
- Nonce: `[4 zero bytes][8B BE counter]` starting at counter=1 (counter=0 consumed by auth exchange)
- Each direction has independent send/recv counters
- Empty associated data

**Implementation with OpenSSL (relay has no libsodium):**
```cpp
// HKDF-SHA256 via OpenSSL EVP
EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
EVP_PKEY_derive_init(pctx);
EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256());
EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, salt_len);
EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, ikm_len);
// For extract: EVP_PKEY_HKDF_MODE_EXTRACT_ONLY
// For expand: EVP_PKEY_HKDF_MODE_EXPAND_ONLY with info string

// ChaCha20-Poly1305 AEAD via OpenSSL EVP
EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, key, nonce);
// No AAD (empty associated data)
EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len);
EVP_EncryptFinal_ex(ctx, ciphertext + len, &final_len);
EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag);
```

### Pattern 2: TransportCodec (Relay's Own Copy)

**What:** The relay implements its own TransportCodec for encoding/decoding FlatBuffer TransportMessage envelopes. This is a straightforward reimplementation of `db/net/protocol.h/cpp` using the copied `transport_generated.h`.

**Reference implementation (db/net/protocol.cpp):**
```cpp
// Encode: FlatBufferBuilder -> CreateTransportMessage(type, payload_vec, request_id) -> Finish
// Decode: Verifier -> GetTransportMessage -> extract type, payload, request_id
```

The relay's copy lives in `relay/wire/transport_codec.h/cpp` and is byte-compatible with the node's codec.

### Pattern 3: Table-Driven JSON <-> Binary Translation

**What:** A single pair of functions (`json_to_binary` / `binary_to_json`) that iterate the FieldSpec array from Phase 102's MessageSchema and encode/decode each field according to its FieldEncoding type. No per-type handler functions.

**How it works for non-FlatBuffer types (35 of 40):**
- JSON -> binary: iterate FieldSpec array, extract JSON value by json_name, encode per FieldEncoding (HEX_32 -> decode hex to 32 bytes, UINT64_STRING -> parse string to uint64 -> store BE, etc.), concatenate in order
- Binary -> JSON: iterate FieldSpec array, read bytes from payload at accumulated offset, decode per FieldEncoding, set JSON value

**How it works for FlatBuffer types (3 of 40):**
- Data (type 8): JSON -> FlatBuffer BlobData (use relay's own encode_blob)
- ReadResponse (type 32): `[status:1][FlatBuffer BlobData]` -> JSON (decode_blob + status)
- BatchReadResponse (type 54): `[truncated:1][count:u32BE][status:1][hash:32][size:u64BE][fb_blob]...` -> JSON array

### Pattern 4: Request-ID Multiplexing

**What:** RequestRouter assigns relay-scoped request_ids, maintains a pending map, and routes responses to the correct client.

**Key detail (from CONTEXT.md D-09):** Fire-and-forget messages bypass the router:
- Ping (5), Pong (6), Goodbye (7): forwarded directly, no request_id rewriting
- Subscribe (19), Unsubscribe (20): forwarded directly in Phase 103 (Phase 104 adds aggregation)
- Notification (21): server-initiated (request_id=0), routed via namespace matching (Phase 104)

### Anti-Patterns to Avoid
- **Linking against db/net/ or db/wire/**: The relay MUST NOT depend on db/ code. Copy generated headers, reimplement codec. Phase 100 D-01 established this boundary.
- **Unencrypted UDS assumption**: D-03 is wrong. The node always uses AEAD after TrustedHello. Implementing a raw/unencrypted UDS client will fail silently (garbled data).
- **Monolithic FieldSpec for compound types**: Several binary formats (ListResponse, BatchExistsResponse, PeerInfoResponse) have variable-length compound entries that a flat FieldSpec array cannot describe. These need either extended schema metadata or explicit decode helpers.
- **Blocking JSON parse for large messages**: Blob data can be 100 MiB. Base64 encode/decode of large payloads dominates CPU. Consider async offload for Data messages.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Base64 encode/decode | Custom base64 codec | OpenSSL EVP_EncodeBlock/EVP_DecodeBlock | Correct, fast, handles padding. OpenSSL already linked. |
| HKDF-SHA256 | Manual HMAC-based KDF | OpenSSL EVP_PKEY_derive with HKDF mode | RFC 5869 compliant, handles extract/expand modes |
| ChaCha20-Poly1305 | Custom AEAD | OpenSSL EVP_chacha20_poly1305 | Constant-time, hardware-optimized on supported CPUs |
| FlatBuffer transport envelope | Custom binary envelope | FlatBuffers library (already fetched) | Matches node's TransportMessage exactly |
| Hex encode/decode | Per-file local functions | Shared relay/util/hex.h | Currently duplicated in ws_session.cpp and relay_main.cpp |

**Key insight:** The relay already has OpenSSL linked for TLS. Use its crypto primitives for the UDS handshake instead of adding libsodium as a new dependency.

## Common Pitfalls

### Pitfall 1: AEAD Counter Synchronization
**What goes wrong:** Send/recv counters get out of sync, causing AEAD decrypt failures.
**Why it happens:** The auth exchange consumes the first nonce (counter=0). If the relay starts its post-handshake counters at 0 instead of 1, every subsequent message fails.
**How to avoid:** After auth exchange, both send_counter_ and recv_counter_ must be 1 (auth used counter 0). Follow the node's Connection::message_loop pattern exactly.
**Warning signs:** "AEAD decrypt failed" immediately after successful handshake.

### Pitfall 2: Auth Payload Endianness
**What goes wrong:** Auth payload pubkey_size uses LITTLE-endian (not big-endian like everything else).
**Why it happens:** This is a protocol-defined exception documented in project memory: "Auth payload uses LITTLE-endian for pubkey_size."
**How to avoid:** Use LE encoding for the 2-byte pubkey_size in auth payload. The decode function in `db/net/auth_helpers.h` is the reference.
**Warning signs:** Node rejects auth with "malformed auth payload" after TrustedHello.

### Pitfall 3: FieldSpec Schema Gaps
**What goes wrong:** The generic translator produces incorrect binary payloads because the FieldSpec definitions from Phase 102 don't match the actual wire format.
**Why it happens:** Phase 102 designed schemas before fully verifying against the node's MessageDispatcher. Several schemas are incomplete or incorrect.
**How to avoid:** Fix schemas during Phase 103 implementation. See "Schema Gaps" section below.
**Warning signs:** Node sends strike/disconnect for "too short" payloads.

### Pitfall 4: ListResponse Compound Format
**What goes wrong:** ListResponse decoded incorrectly because FieldSpec says `HEX_32_ARRAY` for hashes, but actual format is `[count:u32BE][ [hash:32][seq_num:u64BE] ]... [truncated:u8]`.
**Why it happens:** The binary format interleaves hashes with seq_nums. A flat HEX_32_ARRAY encoding loses the seq_nums.
**How to avoid:** ListResponse needs a custom decode that produces `[{hash: "hex", seq_num: "uint64_string"}, ...]` JSON entries, not a flat array of hex strings.
**Warning signs:** Client receives incomplete ListResponse data (missing seq_nums).

### Pitfall 5: Binary WebSocket Frame Confusion
**What goes wrong:** Client parser breaks because it doesn't expect JSON in a binary frame.
**Why it happens:** Per D-20/D-21, binary frames contain JSON (not raw FlatBuffers). The binary opcode is a signal that the payload may be large, not that it's a different format.
**How to avoid:** Document clearly for clients. Server-side: the send_binary path must still encode the response as JSON.
**Warning signs:** Clients that switch parsers based on opcode fail.

### Pitfall 6: Missing UDS AEAD (D-03 Bug)
**What goes wrong:** Relay sends raw unencrypted TransportMessage after TrustedHello. Node receives garbled data and disconnects.
**Why it happens:** Following CONTEXT.md D-03 literally ("unencrypted"). The actual protocol uses AEAD.
**How to avoid:** Implement full HKDF key derivation + AEAD encryption after TrustedHello. Reference: `db/net/handshake.cpp::derive_lightweight_session_keys()` and `db/net/connection.cpp::send_encrypted()/recv_encrypted()`.
**Warning signs:** Node immediately disconnects after TrustedHello with no error message (garbled first message).

## Schema Gaps (Phase 102 FieldSpec Corrections Needed)

These FieldSpec definitions from Phase 102 do not match the actual node wire format. Phase 103 MUST fix them.

### ListRequest (type 33) -- Missing since_seq
**Current schema:** `{request_id, namespace, limit}`
**Actual wire format:** `[namespace:32][since_seq:u64BE][limit:u32BE]` (44 bytes)
**Fix:** Add `{"since_seq", FieldEncoding::UINT64_STRING}` between namespace and limit

### ListResponse (type 34) -- Compound entries
**Current schema:** `{request_id, hashes: HEX_32_ARRAY, truncated: BOOL}`
**Actual wire format:** `[count:u32BE][ [hash:32][seq_num:u64BE] * count ][truncated:u8]`
**Fix:** Cannot represent with flat FieldSpec. Need custom decode that produces `[{hash, seq_num}, ...]`

### NamespaceListRequest (type 41) -- Missing cursor + limit
**Current schema:** `{request_id}` (empty payload implied)
**Actual wire format:** `[after_namespace:32][limit:u32BE]` (36 bytes)
**Fix:** Add `after_namespace` (HEX_32) and `limit` (UINT32_NUMBER) fields

### NamespaceListResponse (type 42) -- Compound entries
**Current schema:** `{request_id, namespaces: HEX_32_ARRAY}`
**Actual wire format:** `[count:u32BE][ [namespace:32][blob_count:u64BE] * count ][has_more:u8]` (variable)
**Fix:** Need custom decode for entries, add has_more flag

### MetadataResponse (type 48) -- Different structure
**Current schema:** `{request_id, found, namespace, hash, size, ttl, timestamp, seq_num, is_tombstone}`
**Actual wire format:** `[status:1][hash:32][timestamp:u64BE][ttl:u32BE][data_size:u64BE][seq_num:u64BE][pubkey_len:u16BE][pubkey:N]`
**Fix:** Complete restructure: remove namespace/is_tombstone (not in wire), add data_size, pubkey. Status byte (0=found, 1=not found) determines if remaining fields are present.

### BatchExistsRequest (type 49) -- Missing count prefix
**Current schema:** `{request_id, namespace, hashes: HEX_32_ARRAY}`
**Actual wire format:** `[namespace:32][count:u32BE][hash:32 * count]` (36 + 32*N bytes)
**Fix:** The HEX_32_ARRAY encoding must encode count prefix before hashes

### BatchExistsResponse (type 50) -- Bool array, not single BOOL
**Current schema:** `{request_id, results: BOOL}` (single BOOL)
**Actual wire format:** `[result:u8 * count]` (one byte per input hash, 0x00/0x01)
**Fix:** Need BOOL_ARRAY encoding type, or custom decode

### DelegationListResponse (type 52) -- Array of pubkeys with count
**Current schema:** `{request_id, delegations: HEX_PUBKEY}` (single pubkey)
**Actual wire format:** `[count:u32BE][ [namespace:32][pubkey_hash:32] * count ]`
**Fix:** Need array encoding, and the format is namespace+hash pairs, not raw pubkeys

### PeerInfoResponse (type 56) -- Two distinct formats
**Current schema:** `{request_id, peer_count: UINT32_NUMBER}`
**Actual wire format (untrusted):** `[peer_count:u32BE][bootstrap_count:u32BE]` (8 bytes)
**Actual wire format (trusted/UDS):** `[peer_count:u32BE][bootstrap_count:u32BE][ [addr_len:u16BE][addr:N][is_bootstrap:u8][syncing:u8][peer_is_full:u8][duration_ms:u64BE] * peer_count ]`
**Fix:** UDS connections always get trusted format. Need custom decode for variable-length peer entries.

### BatchReadRequest (type 53) -- Missing count prefix
**Current schema:** `{request_id, namespace, hashes: HEX_32_ARRAY, max_bytes: UINT32_NUMBER}`
**Actual wire format:** `[namespace:32][count:u32BE][max_bytes:u32BE][hash:32 * count]`
**Fix:** Count prefix needed before hashes, max_bytes position is between count and hashes

### Approach for Schema Gaps
The generic table-driven translator works for the ~25 simple message types with flat binary formats. For the ~12 types with compound/variable formats, use a hybrid approach:
1. Keep FieldSpec for documentation and field-name mapping
2. Add a `custom_encode`/`custom_decode` function pointer to MessageSchema (or a tag like `is_compound`)
3. Write explicit encode/decode functions for compound types (not per-type "handlers" -- they're codec helpers for the generic translator)

This preserves the spirit of D-13 (no per-type handler functions for request processing) while acknowledging that some binary formats are structurally complex.

## Code Examples

### UDS Multiplexer Core API
```cpp
// relay/core/uds_multiplexer.h
namespace chromatindb::relay::core {

class UdsMultiplexer {
public:
    UdsMultiplexer(asio::io_context& ioc,
                   const std::string& uds_path,
                   const identity::RelayIdentity& identity);

    /// Start async connect + handshake. Non-blocking.
    void start();

    /// Send a message to the node. Returns false if not connected.
    asio::awaitable<bool> send_message(uint8_t type,
                                        std::span<const uint8_t> payload,
                                        uint32_t request_id);

    /// Whether UDS is connected and authenticated.
    bool is_connected() const;

    /// Set callback for received messages (dispatches to RequestRouter).
    using MessageCallback = std::function<void(uint8_t type,
                                                std::vector<uint8_t> payload,
                                                uint32_t request_id)>;
    void set_on_message(MessageCallback cb);

private:
    asio::awaitable<void> connect_loop();  // Retry with jittered backoff
    asio::awaitable<bool> do_handshake();  // TrustedHello + HKDF + auth
    asio::awaitable<void> read_loop();     // Recv + decrypt + decode + dispatch
    asio::awaitable<void> drain_send_queue(); // Serialized sends

    // AEAD state
    std::vector<uint8_t> send_key_;   // 32 bytes
    std::vector<uint8_t> recv_key_;   // 32 bytes
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;
};
}
```

### RequestRouter Core API
```cpp
// relay/core/request_router.h
namespace chromatindb::relay::core {

struct PendingRequest {
    uint64_t client_session_id;
    uint32_t client_request_id;
    std::chrono::steady_clock::time_point created;
};

class RequestRouter {
public:
    /// Register a client request. Returns relay-scoped request_id.
    uint32_t register_request(uint64_t client_id, uint32_t client_rid);

    /// Resolve a node response by relay_rid. Removes entry.
    std::optional<PendingRequest> resolve_response(uint32_t relay_rid);

    /// Remove all pending entries for a disconnected client.
    void remove_client(uint64_t client_id);

    /// Purge entries older than 60 seconds.
    void purge_stale();

private:
    uint32_t next_relay_rid_ = 1;
    std::unordered_map<uint32_t, PendingRequest> pending_;
};
}
```

### Translator Core API
```cpp
// relay/translate/translator.h
namespace chromatindb::relay::translate {

struct TranslateResult {
    uint8_t wire_type;
    std::vector<uint8_t> payload;
};

/// JSON -> binary payload for sending to node.
/// Uses MessageSchema::fields for table-driven encoding.
/// Special-cases FlatBuffer types (Data, ReadResponse, BatchReadResponse).
std::optional<TranslateResult> json_to_binary(const nlohmann::json& msg);

/// Binary payload -> JSON for sending to client.
/// type: wire type from TransportMessage.
/// payload: raw payload bytes (after transport envelope decode).
std::optional<nlohmann::json> binary_to_json(uint8_t type,
                                              std::span<const uint8_t> payload);

/// Whether a response type should be sent as binary WS frame.
bool is_binary_response(uint8_t type);
}
```

### AEAD Encryption via OpenSSL
```cpp
// relay/wire/aead.h -- Relay's own AEAD using OpenSSL
namespace chromatindb::relay::wire {

constexpr size_t AEAD_KEY_SIZE = 32;
constexpr size_t AEAD_NONCE_SIZE = 12;
constexpr size_t AEAD_TAG_SIZE = 16;

/// Encrypt plaintext with ChaCha20-Poly1305.
/// Returns ciphertext || tag (plaintext.size() + 16 bytes).
std::vector<uint8_t> aead_encrypt(std::span<const uint8_t> plaintext,
                                   std::span<const uint8_t, AEAD_KEY_SIZE> key,
                                   uint64_t counter);

/// Decrypt ciphertext || tag with ChaCha20-Poly1305.
/// Returns plaintext on success, nullopt on auth failure.
std::optional<std::vector<uint8_t>> aead_decrypt(
    std::span<const uint8_t> ciphertext_and_tag,
    std::span<const uint8_t, AEAD_KEY_SIZE> key,
    uint64_t counter);

/// Build 12-byte nonce: [4 zero bytes][8B BE counter]
std::array<uint8_t, AEAD_NONCE_SIZE> make_nonce(uint64_t counter);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Per-client UDS (old relay) | Single multiplexed UDS | v3.0.0 Phase 103 | Eliminates O(N) PeerInfo pollution in node |
| Raw FlatBuffers over WS | JSON with hex/base64 encoding | v3.0.0 Phase 102 | Clients need only WebSocket + JSON, no FlatBuffers library |
| Python SDK required | Any WebSocket client | v3.0.0 | Eliminates SDK dependency entirely |

## Open Questions

1. **AEAD vs unencrypted UDS**
   - What we know: Node's actual behavior uses AEAD encryption on UDS after TrustedHello. CONTEXT.md D-03 says unencrypted.
   - What's unclear: Was D-03 an intentional simplification that would require node-side changes, or a misunderstanding?
   - Recommendation: Implement AEAD. The node's behavior is fixed (db/ is frozen this milestone). Trying to use unencrypted UDS will fail. D-03 is simply incorrect.

2. **Schema gap resolution strategy**
   - What we know: ~12 message types have binary formats that don't fit the flat FieldSpec model
   - What's unclear: Whether to fix FieldSpec definitions (extend the model) or add custom codec helpers
   - Recommendation: Hybrid approach. Fix simple gaps (add missing fields). For compound types, add custom encode/decode helpers called from within the generic translator. This is NOT per-type handler functions (D-13) -- it's codec-level format adaptation.

3. **Base64 performance for large blobs**
   - What we know: Blob data can be up to 100 MiB. Base64 encoding adds ~33% overhead.
   - What's unclear: Whether base64 encode/decode of a 100 MiB blob will block the event loop unacceptably.
   - Recommendation: Start with synchronous base64 in the translator. Profile under load. If needed, offload large Data messages to thread pool (like auth verify does with asio::post).

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| OpenSSL | AEAD + HKDF | Yes | 3.6.2 | -- |
| flatc | .fbs compilation (if needed) | Yes | built from FetchContent | Use pre-generated headers |
| CMake | Build system | Yes | 4.3.1 | -- |
| Catch2 | Unit tests | Yes | 3.7.1 | -- |

**Missing dependencies with no fallback:** None.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt |
| Quick run command | `cd build && ctest -R relay -j4 --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| MUX-01 | Single UDS connect + TrustedHello + AEAD handshake | unit | `cd build && ctest -R test_uds -j1 --output-on-failure` | Wave 0 |
| MUX-02 | RequestRouter: register, resolve, remove_client, purge_stale | unit | `cd build && ctest -R test_request_router -j1 --output-on-failure` | Wave 0 |
| PROT-01 | Table-driven JSON<->binary for all 38 types | unit | `cd build && ctest -R test_translator -j1 --output-on-failure` | Wave 0 |
| PROT-04 | Binary WS frame for ReadResponse/BatchReadResponse | unit | `cd build && ctest -R test_translator -j1 --output-on-failure` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R relay -j4 --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `relay/tests/test_translator.cpp` -- covers PROT-01, PROT-04 (JSON<->binary roundtrip for all 38 types + binary frame identification)
- [ ] `relay/tests/test_request_router.cpp` -- covers MUX-02 (register, resolve, remove_client, purge_stale, counter wrap)
- [ ] `relay/tests/test_transport_codec.cpp` -- covers relay's own TransportCodec (encode/decode roundtrip)
- [ ] `relay/tests/test_aead.cpp` -- covers AEAD encrypt/decrypt, HKDF key derivation, nonce construction
- [ ] FlatBuffers dependency in relay/tests/CMakeLists.txt

## Sources

### Primary (HIGH confidence)
- `db/net/connection.cpp` -- TrustedHello handshake implementation with AEAD (lines 261-598)
- `db/net/handshake.cpp` -- derive_lightweight_session_keys HKDF implementation (lines 62-109)
- `db/net/protocol.cpp` -- TransportCodec encode/decode reference (44 lines)
- `db/net/framing.h` -- Frame format: 4-byte BE length prefix, MAX_FRAME_SIZE
- `db/peer/message_dispatcher.cpp` -- All 20+ message handler binary formats (1200+ lines)
- `db/wire/codec.h` -- BlobData struct, encode_blob/decode_blob API
- `db/schemas/transport.fbs` -- TransportMessage FlatBuffer schema
- `db/schemas/blob.fbs` -- Blob FlatBuffer schema
- `db/PROTOCOL.md` -- Wire format documentation, UDS section confirms AEAD encryption
- `relay/translate/json_schema.h` -- Phase 102 FieldSpec definitions (304 lines)
- `relay/translate/json_schema.cpp` -- Schema lookup table (122 lines)
- `relay/translate/type_registry.h` -- 40-entry type registry
- `relay/ws/ws_session.h/cpp` -- Current WsSession with AUTHENTICATED stub
- `relay/core/session.h` -- Send queue with enqueue/drain
- `relay/relay_main.cpp` -- Main entry point, current startup sequence
- `relay/CMakeLists.txt` -- Current build dependencies
- `.planning/research/ARCHITECTURE.md` -- v3.0.0 architecture design

### Secondary (MEDIUM confidence)
- `.planning/phases/103-uds-multiplexer-protocol-translation/103-CONTEXT.md` -- User decisions (D-03 INCORRECT re: unencrypted UDS)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already in project, no new dependencies
- Architecture: HIGH -- patterns verified against actual node code, handshake protocol fully traced
- Pitfalls: HIGH -- AEAD requirement discovered by tracing actual code paths, schema gaps enumerated by comparing FieldSpec to MessageDispatcher handlers
- Schema gaps: HIGH -- each gap verified by reading actual handler code in message_dispatcher.cpp

**Research date:** 2026-04-10
**Valid until:** 2026-05-10 (stable -- node code is frozen for v3.0.0)
