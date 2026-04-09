# Phase 102: Authentication & JSON Schema - Research

**Researched:** 2026-04-09
**Domain:** ML-DSA-87 challenge-response authentication, JSON message schema design, message type filtering
**Confidence:** HIGH

## Summary

Phase 102 adds three interrelated components to the relay: (1) ML-DSA-87 challenge-response authentication over WebSocket, (2) JSON schema definitions for all 38 client-allowed message types, and (3) a message type allowlist filter. All three are definition/schema/verification work -- no JSON-to-FlatBuffers translation (Phase 103) and no UDS connection (Phase 103).

The relay already has everything needed: liboqs for ML-DSA-87 verification, OpenSSL for RAND_bytes challenge generation, nlohmann/json for message parsing, and a WsSession with on_message() ready for auth state machine injection. The translate/ directory exists (empty with .gitkeep) ready for schema headers. No new dependencies required.

**Primary recommendation:** Build the Authenticator class first (standalone, testable), then integrate the auth state machine into WsSession, then define the JSON schema and type registry in translate/ headers, and finally add the message filter. Each component is independently testable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Relay-only challenge-response. 32-byte OpenSSL RAND_bytes nonce as hex, client signs raw 32 bytes with ML-DSA-87, relay verifies via liboqs.
- D-02: Challenge sent immediately after WS upgrade. 10s auth timer replaces Phase 101 30s idle timeout.
- D-03: Non-auth message before auth -> JSON error then Close(4001) + disconnect.
- D-04: Auth failure sends `{"type":"auth_error","reason":"<code>"}` then Close(4001).
- D-05: All auth messages are JSON text WebSocket frames.
- D-06: After auth, WsSession stores full ML-DSA-87 pubkey (2592 bytes) + SHA3-256(pubkey) namespace hash (32 bytes).
- D-07: Optional `allowed_client_keys` in config. Array of 64-char hex namespace hashes. Empty/absent = open relay. SIGHUP-reloadable.
- D-08: ML-DSA-87 verification offloaded via `asio::post(ioc)` to hardware_concurrency() thread pool.
- D-09: Connection cap + 10s timeout is sufficient abuse protection. No per-IP rate limiting (Phase 105).
- D-10: Auth OK response: `{"type":"auth_ok","namespace":"<64-char hex>"}`.
- D-11: WsSession state enum: AWAITING_AUTH -> AUTHENTICATED.
- D-12: No relay signature during auth. TLS proves server identity.
- D-13: Duplicate challenge_response after auth -> log debug, ignore.
- D-14: Multiple concurrent sessions from same pubkey allowed.
- D-15: Auth logic in `core/authenticator.h`. Testable in isolation.
- D-16: State transition to AUTHENTICATED before sending auth_ok.
- D-17: Client signs raw 32-byte nonce. No domain prefix.
- D-18: JSON type strings: snake_case of enum name (ReadRequest -> "read_request").
- D-19: Binary field encoding: hex for identity/hash (32-byte), base64 for payload data/signatures.
- D-20: All uint64 as JSON strings. Always. No conditional.
- D-21: Schema defined in C++ headers: `translate/type_registry.h`, `translate/json_schema.h`. Schema IS the code.
- D-22: request_id is uint32 as JSON number. Same semantics as binary protocol.
- D-23: Uniform error envelope: `{"type":"error","request_id":N,"code":"<code>","message":"<text>"}`.
- D-24: Optional fields omitted when default/zero value.
- D-25: Phase 102 scope: schema + type registry + message filter. No JSON<->FB translation (Phase 103).
- D-26: Code-only schema documentation. PROTOCOL.md appendix when relay ships.
- D-27: Allowlist of 38 client-allowed type strings. New types blocked by default.
- D-28: Filter at JSON parse time: extract "type", check allowlist before further parsing. Also filters outbound.
- D-29: Blocked type -> error response with request_id, keep connection open.
- D-30: Allowlist is hardcoded constexpr in `core/message_filter.h`. Not operator-configurable.
- D-31: Filter only checks type string. Field-level validation in Phase 103.
- D-32: `max_connections` in RelayConfig. Default 1024. SIGHUP-reloadable.
- D-33: Auth timeout 10s hardcoded as constexpr. Not configurable.
- D-34: `allowed_client_keys` as JSON array of 64-char hex namespace hashes.

### Claude's Discretion
- Internal Authenticator class API design (method signatures, constructor parameters)
- JSON schema constexpr data structure design in translate/ headers
- Specific error code strings beyond those mentioned
- Test file organization within relay/tests/
- Whether type_registry and json_schema are one header or two

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| AUTH-01 | Relay sends random 32-byte challenge on WebSocket connect | OpenSSL RAND_bytes available (relay links OpenSSL::Crypto). Authenticator class generates challenge, WsSession sends as JSON text frame. |
| AUTH-02 | Client signs challenge with ML-DSA-87, relay verifies | liboqs OQS_SIG_verify() available. RelayIdentity already uses liboqs. Standalone verify function in Authenticator. OQS_SHA3_sha3_256 for namespace derivation. |
| AUTH-03 | Auth timeout (10s) disconnects unresponsive clients | WsSession idle_timer_ already exists (30s). Replace with 10s auth_timer_ in AWAITING_AUTH state. Cancel on successful auth. |
| AUTH-04 | Auth verification offloaded to thread pool (non-blocking) | asio::post(ioc) offloads to existing hardware_concurrency() thread pool. Same pattern as node's verify_with_offload but simpler (no dedicated pool). |
| PROT-02 | Binary fields as hex (hashes/namespaces) or base64 (data/signatures) | Schema headers define encoding rules per field. hex for 32-byte fields, base64 for variable-length binary. |
| PROT-03 | uint64 fields as JSON strings | Schema headers mark uint64 fields (seq_num, timestamp, byte counts) as string-encoded. |
| PROT-05 | Message type filtering (allowlist for 38 client-allowed types) | Confirmed 38 types from node's supported_types array: {5,6,7,8,17,18,19,20,21,30-58}. Hardcoded constexpr allowlist. |
| SESS-03 | Configurable max concurrent WebSocket connections | Add max_connections to RelayConfig (default 1024). Replace WsAcceptor hardcoded MAX_CONNECTIONS. SIGHUP-reloadable. |
</phase_requirements>

## Standard Stack

### Core (All Already Available)

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs | 0.15.0 | ML-DSA-87 signature verification (OQS_SIG_verify) + SHA3-256 (OQS_SHA3_sha3_256) | Already linked by relay. Provides both crypto primitives needed for auth. |
| OpenSSL | 3.6.2 (system) | RAND_bytes for challenge nonce generation | Already linked by relay for TLS. CSPRNG available. |
| nlohmann/json | 3.11.3 | JSON parsing for auth messages + schema validation | Already fetched by relay CMake. |
| Standalone Asio | 1.38.0 | async IO, thread pool offload via asio::post(ioc) | Already linked. Thread pool exists (hardware_concurrency() threads in main). |
| Catch2 | 3.7.1 | Unit tests for authenticator, schema, filter | Already fetched by relay test CMake. |
| spdlog | 1.15.1 | Structured logging for auth events | Already linked. |

### No New Dependencies

Phase 102 requires ZERO new dependencies. All crypto, JSON, and async primitives are already available in the relay's dependency set.

## Architecture Patterns

### Recommended File Layout

```
relay/
  core/
    authenticator.h          # NEW: Challenge generation, signature verification, ACL check
    authenticator.cpp         # NEW: liboqs verify, OpenSSL RAND_bytes, SHA3-256
    message_filter.h          # NEW: Constexpr 38-type allowlist, check functions
  translate/
    type_registry.h           # NEW: String <-> enum mapping for 38 types
    json_schema.h             # NEW: Field names, encoding rules, type metadata
  config/
    relay_config.h            # MODIFY: Add max_connections, allowed_client_keys
    relay_config.cpp          # MODIFY: Parse new fields, validate hex hashes
  ws/
    ws_session.h              # MODIFY: Add SessionState enum, client identity storage, auth timer
    ws_session.cpp            # MODIFY: Auth state machine in on_message(), auth timer management
    ws_acceptor.h             # MODIFY: Replace hardcoded 1024 with config value
    ws_acceptor.cpp           # MODIFY: Read max_connections from config
  relay_main.cpp              # MODIFY: Extend SIGHUP to reload allowed_client_keys, max_connections
  tests/
    test_authenticator.cpp    # NEW: Auth challenge/verify/ACL tests
    test_message_filter.cpp   # NEW: Allowlist/blocklist tests
    test_type_registry.cpp    # NEW: String<->enum mapping tests
    test_json_schema.cpp      # NEW: Encoding rule tests (optional, may fold into registry)
```

### Pattern 1: Authenticator as Standalone Class

**What:** Authenticator owns challenge generation, signature verification, ACL checking, and namespace derivation. WsSession delegates to it during AWAITING_AUTH state.

**Why:** Testable without WebSocket. Constructor injection of ACL list enables easy mocking.

**Design:**

```cpp
// core/authenticator.h
namespace chromatindb::relay::core {

struct AuthResult {
    bool success;
    std::string error_code;   // "invalid_signature", "unknown_key", etc.
    std::array<uint8_t, 32> namespace_hash{};  // Valid only if success
    std::vector<uint8_t> public_key;           // Valid only if success (2592 bytes)
};

class Authenticator {
public:
    /// Construct with optional ACL. Empty set = open relay.
    explicit Authenticator(
        std::unordered_set<std::array<uint8_t, 32>, /* hash */> allowed_keys = {});

    /// Generate a 32-byte random challenge via OpenSSL RAND_bytes.
    std::array<uint8_t, 32> generate_challenge();

    /// Verify client's challenge_response. Blocking -- call from thread pool.
    /// @param challenge The 32-byte nonce sent to client.
    /// @param pubkey Client's ML-DSA-87 public key (2592 bytes).
    /// @param signature Client's signature over the challenge (4627 bytes).
    AuthResult verify(
        std::span<const uint8_t, 32> challenge,
        std::span<const uint8_t> pubkey,
        std::span<const uint8_t> signature);

    /// Reload allowed keys (SIGHUP). Thread-safe.
    void reload_allowed_keys(
        std::unordered_set<std::array<uint8_t, 32>, /* hash */> new_keys);

private:
    mutable std::mutex acl_mutex_;
    std::unordered_set<std::array<uint8_t, 32>, /* hash */> allowed_keys_;
};

} // namespace chromatindb::relay::core
```

**Verification steps (in verify()):**
1. Check pubkey size == 2592 bytes, signature size == 4627 bytes (cheapest first -- Step 0 pattern)
2. `OQS_SIG_verify(sig, challenge.data(), 32, signature.data(), sig_len, pubkey.data())` -- the expensive op
3. Compute `namespace_hash = OQS_SHA3_sha3_256(pubkey)`
4. If ACL non-empty: check namespace_hash in allowed set
5. Return AuthResult with appropriate error_code on failure

### Pattern 2: WsSession Auth State Machine

**What:** SessionState enum gates message processing. on_message() checks state before acting.

**Design:**

```cpp
// In ws_session.h
enum class SessionState : uint8_t {
    AWAITING_AUTH,
    AUTHENTICATED
};

// New members in WsSession:
SessionState state_ = SessionState::AWAITING_AUTH;
std::array<uint8_t, 32> challenge_{};          // Nonce sent to client
std::vector<uint8_t> client_pubkey_;           // Stored after auth (2592 bytes)
std::array<uint8_t, 32> client_namespace_{};   // SHA3-256(pubkey)
asio::steady_timer auth_timer_;                // 10s auth timeout (replaces idle_timer_ during auth)
```

**on_message() state machine:**

```
AWAITING_AUTH:
  1. Parse JSON from text frame data
  2. If type != "challenge_response" -> send error, Close(4001)
  3. Decode hex pubkey and signature from JSON
  4. co_await asio::post(ioc) to offload to thread pool
  5. Call authenticator_.verify(challenge_, pubkey, signature)
  6. If failed -> send auth_error, Close(4001)
  7. Store client_pubkey_, client_namespace_
  8. Set state_ = AUTHENTICATED
  9. Cancel auth_timer_
  10. Send auth_ok response

AUTHENTICATED:
  1. Parse JSON "type" field
  2. Check message_filter allowlist
  3. If blocked -> send error, keep connection
  4. (Phase 102: log and drop. Phase 103 adds translation + UDS forwarding)
```

**Auth timer lifecycle:**
- start(): spawn auth_timer_ (10s). If fires -> Close(4001, "auth timeout").
- on_message() in AWAITING_AUTH: cancel auth_timer_ after successful verify.
- The Phase 101 idle_timer_ (30s) is repurposed: active only in AUTHENTICATED state.

### Pattern 3: Type Registry with Bidirectional Mapping

**What:** constexpr data mapping between JSON type strings and TransportMsgType enum values.

**Design:**

```cpp
// translate/type_registry.h
namespace chromatindb::relay::translate {

struct TypeEntry {
    std::string_view json_name;    // "read_request"
    int8_t wire_type;              // 31 (TransportMsgType_ReadRequest)
};

// Full registry of 38 client-allowed types.
// Sorted by json_name for binary search.
inline constexpr TypeEntry TYPE_REGISTRY[] = {
    {"batch_exists_request",  49},
    {"batch_exists_response", 50},
    {"batch_read_request",    53},
    {"batch_read_response",   54},
    {"data",                   8},
    {"delete",                17},
    {"delete_ack",            18},
    {"delegation_list_request",  51},
    {"delegation_list_response", 52},
    {"exists_request",        37},
    {"exists_response",       38},
    {"goodbye",                7},
    {"list_request",          33},
    {"list_response",         34},
    {"metadata_request",      47},
    {"metadata_response",     48},
    {"namespace_list_request",  41},
    {"namespace_list_response", 42},
    {"namespace_stats_request",  45},
    {"namespace_stats_response", 46},
    {"node_info_request",     39},
    {"node_info_response",    40},
    {"notification",          21},
    {"peer_info_request",     55},
    {"peer_info_response",    56},
    {"ping",                   5},
    {"pong",                   6},
    {"quota_exceeded",        25},
    {"read_request",          31},
    {"read_response",         32},
    {"stats_request",         35},
    {"stats_response",        36},
    {"storage_full",          22},
    {"storage_status_request",  43},
    {"storage_status_response", 44},
    {"subscribe",             19},
    {"time_range_request",    57},
    {"time_range_response",   58},
    {"unsubscribe",           20},
    {"write_ack",             30},
};
// Count: 40 entries = 38 client types + ping + pong
// (Wait: ping/pong are WebSocket-level, not JSON-level.
//  But node supported_types includes them. Need to decide.)

std::optional<int8_t> type_from_string(std::string_view name);
std::optional<std::string_view> type_to_string(int8_t wire_type);

} // namespace chromatindb::relay::translate
```

### Pattern 4: JSON Schema as Constexpr Metadata

**What:** Each type's field names, encoding rules, and expected types are defined as constexpr data. Phase 103 uses this metadata to drive translation.

**Design:**

```cpp
// translate/json_schema.h
namespace chromatindb::relay::translate {

enum class FieldEncoding : uint8_t {
    HEX,           // 32-byte hash/namespace -> 64-char hex string
    HEX_PUBKEY,    // 2592-byte pubkey -> 5184-char hex string
    BASE64,        // Variable-length binary (data, signatures) -> base64
    UINT64_STRING, // uint64 -> JSON string (prevents JS truncation)
    UINT32_NUMBER, // uint32 -> JSON number (fits in JS Number)
    UINT16_NUMBER, // uint16 -> JSON number
    UINT8_NUMBER,  // uint8 -> JSON number
    BOOL,          // 0x00/0x01 -> JSON boolean
    UTF8_STRING,   // Length-prefixed string -> JSON string
};

struct FieldSpec {
    std::string_view name;     // JSON field name
    FieldEncoding encoding;
    size_t wire_offset;        // Offset in binary payload
    size_t wire_size;          // Size in binary payload (0 = variable)
};

struct MessageSchema {
    std::string_view type_name;           // "read_request"
    int8_t wire_type;                     // 31
    bool has_flatbuffer_payload;          // true for Data/Delete/ReadResponse/BatchReadResponse
    std::span<const FieldSpec> fields;    // For non-FB payloads
};

// Full schema array -- populated in json_schema.cpp or as constexpr.
// Phase 103 uses this to drive translation.
const MessageSchema* schema_for_type(int8_t wire_type);
const MessageSchema* schema_for_name(std::string_view name);

} // namespace chromatindb::relay::translate
```

### Pattern 5: Message Filter as Constexpr Allowlist

**What:** A compile-time set of allowed type strings. Single lookup function.

```cpp
// core/message_filter.h
namespace chromatindb::relay::core {

/// Check if a JSON message type string is allowed through the relay.
/// Returns true for all 38 client-allowed types.
bool is_type_allowed(std::string_view type_name);

/// Check if a wire type integer is allowed (for outbound filtering).
bool is_wire_type_allowed(int8_t wire_type);

} // namespace chromatindb::relay::core
```

**Implementation:** std::array<std::string_view, 38> sorted, binary search, or unordered_set at startup. For 38 entries, linear scan is also perfectly fast.

### Anti-Patterns to Avoid

- **Coupling auth to WsSession internals:** The Authenticator must be testable without a WebSocket connection. Pass challenge/pubkey/signature as bytes, return AuthResult.
- **Blocking IO thread during verify:** ML-DSA-87 verify is CPU-intensive (~ms). MUST use asio::post(ioc) to offload. Never call OQS_SIG_verify inline in on_message().
- **Runtime-configurable allowlist:** The 38-type allowlist is protocol-defined. Making it configurable invites operator errors that break the protocol. Hardcoded constexpr.
- **Separate JSON schema document:** The schema IS the code. A separate spec file would drift. Schema headers are the source of truth.
- **Using libsodium for random:** Relay does not link libsodium. Use OpenSSL RAND_bytes (already available) or OQS_randombytes (which wraps system random).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Random nonce generation | Custom PRNG | OpenSSL RAND_bytes() | Cryptographically secure, already linked |
| ML-DSA-87 verify | Custom PQ crypto | OQS_SIG_verify() via liboqs | Proven, project-wide standard |
| SHA3-256 hashing | Custom hash | OQS_SHA3_sha3_256() | liboqs provides, relay already uses it in RelayIdentity |
| JSON parsing | Custom parser | nlohmann::json | Already a dependency, handles all edge cases |
| Hex encode/decode | Custom hex utils | Local to_hex/from_hex functions (relay already has to_hex in main) | Small utility, but don't duplicate -- extract to shared header |
| Thread pool offload | Custom thread pool | asio::post(ioc) | ioc runs on hardware_concurrency() threads already |

**Key insight:** Every crypto primitive needed for auth already exists in the relay's dependency set. The only new code is the orchestration (Authenticator class) and the schema data structures.

## The 38 Client-Allowed Types

Confirmed from the node's `message_dispatcher.cpp` NodeInfoResponse handler (line 534-541):

```cpp
static constexpr uint8_t supported[] = {
    5, 6, 7, 8,                      // Ping, Pong, Goodbye, Data
    17, 18, 19, 20, 21,              // Delete, DeleteAck, Subscribe, Unsubscribe, Notification
    30, 31, 32, 33, 34, 35, 36,      // WriteAck, Read{Req,Resp}, List{Req,Resp}, Stats{Req,Resp}
    37, 38, 39, 40,                  // Exists{Req,Resp}, NodeInfo{Req,Resp}
    41, 42, 43, 44, 45, 46,          // NsList{Req,Resp}, StorageStatus{Req,Resp}, NsStats{Req,Resp}
    47, 48, 49, 50, 51, 52,          // Metadata{Req,Resp}, BatchExists{Req,Resp}, DelegationList{Req,Resp}
    53, 54, 55, 56, 57, 58           // BatchRead{Req,Resp}, PeerInfo{Req,Resp}, TimeRange{Req,Resp}
};
```

**Blocked types (24 total):** 0 (None), 1-4 (KEM/Auth handshake), 9-14 (Sync protocol), 15-16 (PEX), 22-24 (StorageFull, TrustedHello, PQRequired), 25 (QuotaExceeded), 26-29 (Reconcile/SyncRejected), 59-62 (BlobNotify/Fetch, SyncNamespaceAnnounce).

**Wait -- the supported_types array includes types 22 (StorageFull) and 25 (QuotaExceeded) by omission check:**

Let me recount. The array has: 5,6,7,8 (4), 17,18,19,20,21 (5), 30-36 (7), 37-40 (4), 41-46 (6), 47-52 (6), 53-58 (6). Total = 4+5+7+4+6+6+6 = 38. StorageFull (22), PQRequired (24), QuotaExceeded (25), and SyncRejected (29) are NOT in the array. They are node-to-peer signals that don't apply to relay clients.

**Mapping to JSON type strings (D-18: snake_case of enum name):**

| Wire Type | Enum Name | JSON String |
|-----------|-----------|-------------|
| 5 | Ping | "ping" |
| 6 | Pong | "pong" |
| 7 | Goodbye | "goodbye" |
| 8 | Data | "data" |
| 17 | Delete | "delete" |
| 18 | DeleteAck | "delete_ack" |
| 19 | Subscribe | "subscribe" |
| 20 | Unsubscribe | "unsubscribe" |
| 21 | Notification | "notification" |
| 30 | WriteAck | "write_ack" |
| 31 | ReadRequest | "read_request" |
| 32 | ReadResponse | "read_response" |
| 33 | ListRequest | "list_request" |
| 34 | ListResponse | "list_response" |
| 35 | StatsRequest | "stats_request" |
| 36 | StatsResponse | "stats_response" |
| 37 | ExistsRequest | "exists_request" |
| 38 | ExistsResponse | "exists_response" |
| 39 | NodeInfoRequest | "node_info_request" |
| 40 | NodeInfoResponse | "node_info_response" |
| 41 | NamespaceListRequest | "namespace_list_request" |
| 42 | NamespaceListResponse | "namespace_list_response" |
| 43 | StorageStatusRequest | "storage_status_request" |
| 44 | StorageStatusResponse | "storage_status_response" |
| 45 | NamespaceStatsRequest | "namespace_stats_request" |
| 46 | NamespaceStatsResponse | "namespace_stats_response" |
| 47 | MetadataRequest | "metadata_request" |
| 48 | MetadataResponse | "metadata_response" |
| 49 | BatchExistsRequest | "batch_exists_request" |
| 50 | BatchExistsResponse | "batch_exists_response" |
| 51 | DelegationListRequest | "delegation_list_request" |
| 52 | DelegationListResponse | "delegation_list_response" |
| 53 | BatchReadRequest | "batch_read_request" |
| 54 | BatchReadResponse | "batch_read_response" |
| 55 | PeerInfoRequest | "peer_info_request" |
| 56 | PeerInfoResponse | "peer_info_response" |
| 57 | TimeRangeRequest | "time_range_request" |
| 58 | TimeRangeResponse | "time_range_response" |

**Note on Ping/Pong/Goodbye:** These are in the node's supported_types but their semantics differ at the relay boundary. WebSocket already has native ping/pong (opcodes 0x9/0xA). The relay handles WS-level ping/pong directly. JSON-level "ping"/"pong"/"goodbye" would be forwarded to the node via UDS (Phase 103). The type registry should include them; the message filter allows them.

## JSON Schema Design: Per-Type Field Specifications

### Auth-Only Messages (Relay-Originated, Not In Type Registry)

These never reach the node -- they are relay <-> client only:

```json
// Relay -> Client (immediately after WS upgrade)
{"type": "challenge", "nonce": "<64-char hex>"}

// Client -> Relay
{"type": "challenge_response", "pubkey": "<5184-char hex>", "signature": "<hex>"}

// Relay -> Client (success)
{"type": "auth_ok", "namespace": "<64-char hex>"}

// Relay -> Client (failure)
{"type": "auth_error", "reason": "invalid_signature|timeout|unknown_key|..."}

// Relay -> Client (general error)
{"type": "error", "request_id": 42, "code": "blocked_type", "message": "human readable"}
```

### Field Encoding Rules Summary

| Field Type | Encoding | Examples |
|------------|----------|----------|
| namespace_id (32 bytes) | hex string (64 chars) | `"a1b2c3...64chars"` |
| blob_hash (32 bytes) | hex string (64 chars) | `"d4e5f6...64chars"` |
| pubkey (2592 bytes) | hex string (5184 chars) | `"abcd...5184chars"` |
| signature (4627 bytes) | base64 string | `"SGVsbG8..."` |
| blob data (up to 100 MiB) | base64 string | `"AQIDBA..."` |
| seq_num (uint64) | string | `"12345678901234"` |
| timestamp (uint64) | string | `"1712678400"` |
| byte counts (uint64) | string | `"1048576"` |
| count (uint32) | number | `42` |
| ttl (uint32) | number | `86400` |
| limit (uint32) | number | `100` |
| boolean flags (uint8 0/1) | boolean | `true` / `false` |
| status codes (uint8) | number | `0` or `1` |
| version strings | string | `"3.0.0"` |
| request_id (uint32) | number | `42` |

### Example JSON Messages (Key Types)

**ReadRequest (client -> node):**
```json
{"type": "read_request", "request_id": 1, "namespace": "a1b2...64hex", "hash": "d4e5...64hex"}
```

**ReadResponse (node -> client, found):**
```json
{"type": "read_response", "request_id": 1, "found": true, "blob": {"namespace": "a1b2...", "pubkey": "...", "data": "<base64>", "ttl": 86400, "timestamp": "1712678400", "signature": "<base64>"}}
```

**ReadResponse (node -> client, not found):**
```json
{"type": "read_response", "request_id": 1, "found": false}
```

**Data (client -> node, blob write):**
```json
{"type": "data", "request_id": 2, "namespace": "a1b2...", "pubkey": "...", "data": "<base64>", "ttl": 86400, "timestamp": "1712678400", "signature": "<base64>"}
```

**WriteAck (node -> client):**
```json
{"type": "write_ack", "request_id": 2, "hash": "d4e5...64hex", "seq_num": "42", "status": 0}
```

**Subscribe (client -> node):**
```json
{"type": "subscribe", "request_id": 3, "namespaces": ["a1b2...64hex", "c3d4...64hex"]}
```

**Notification (node -> client):**
```json
{"type": "notification", "namespace": "a1b2...64hex", "hash": "d4e5...64hex", "seq_num": "42", "size": 1024, "is_tombstone": false}
```

**NodeInfoResponse (node -> client):**
```json
{"type": "node_info_response", "request_id": 5, "version": "3.0.0", "git_hash": "abc123", "uptime": "3600", "peer_count": 5, "namespace_count": 12, "total_blobs": "1000000", "storage_used": "1073741824", "storage_max": "10737418240", "supported_types": ["data", "read_request", "..."]}
```

## Common Pitfalls

### Pitfall 1: Blocking IO Thread with OQS_SIG_verify

**What goes wrong:** ML-DSA-87 verification is CPU-bound (~1-5ms). Calling it inline in on_message() blocks the io_context thread, stalling all other sessions on that thread.
**Why it happens:** on_message() is called from the read_loop coroutine on the io_context.
**How to avoid:** Use `co_await asio::post(ioc, asio::use_awaitable)` to bounce to the thread pool before calling verify(). Return to the io_context after.
**Warning signs:** High latency on other sessions during auth bursts.

### Pitfall 2: Auth Timer Race with Close

**What goes wrong:** Auth timer fires AFTER session is already closing (e.g., client disconnected). Double-close or use-after-free.
**Why it happens:** Timer callback captures shared_ptr but session may have entered closing_ state.
**How to avoid:** Check `closing_` in timer callback. Cancel timer on any close path. Use the same shared_ptr capture pattern as Phase 101 idle_timer_.
**Warning signs:** Spurious "auth timeout" logs for already-closed sessions.

### Pitfall 3: Hex Decode Validation on Pubkey

**What goes wrong:** Client sends malformed hex in pubkey field (odd length, invalid chars). from_hex returns garbage or throws.
**Why it happens:** Relay trusts client input without validation.
**How to avoid:** Validate hex string length (must be exactly 5184 chars for pubkey, 64 chars for namespace) and character set before decoding. Return auth_error with descriptive reason on failure.
**Warning signs:** Crashes or assertion failures in hex decode.

### Pitfall 4: Forgetting to Cancel Idle Timer During Auth

**What goes wrong:** Phase 101's idle_timer_ (30s) fires during auth flow, disconnecting the client before the 10s auth timeout.
**Why it happens:** WsSession::start() spawns idle_timer_ (30s). Auth flow uses its own 10s timer but doesn't cancel the idle timer.
**How to avoid:** In the auth phase, the 10s auth_timer_ replaces the idle_timer_. Either (a) don't start idle_timer_ until AUTHENTICATED, or (b) cancel idle_timer_ immediately and use auth_timer_ instead. Option (a) is cleaner.
**Warning signs:** Auth succeeds at 5s but session disconnects at 30s.

### Pitfall 5: SIGHUP Reload Race with Auth Check

**What goes wrong:** SIGHUP reloads allowed_client_keys while verify() is checking ACL. Partially-updated set.
**Why it happens:** Multiple threads access allowed_keys_ set.
**How to avoid:** Mutex-protect allowed_keys_ (small set, lock is brief). Or use atomic swap of shared_ptr to the set. Mutex is simpler and matches node's ACL pattern.
**Warning signs:** Intermittent auth failures after SIGHUP.

### Pitfall 6: String Naming Convention Mismatch

**What goes wrong:** JSON type string "ReadRequest" instead of "read_request". Client and relay disagree on naming.
**Why it happens:** D-18 says snake_case but developer forgets for compound names.
**How to avoid:** Type registry is the single source of truth. All string literals in one constexpr array. Tests verify bidirectional roundtrip for all 38 types.
**Warning signs:** "blocked_type" errors for valid message types.

## Code Examples

### ML-DSA-87 Verification (liboqs)

```cpp
// Source: relay/identity/relay_identity.cpp pattern + liboqs API
#include <oqs/oqs.h>
#include <oqs/sha3.h>

bool verify_signature(std::span<const uint8_t> message,
                      std::span<const uint8_t> signature,
                      std::span<const uint8_t> pubkey) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) return false;

    OQS_STATUS rc = OQS_SIG_verify(sig,
        message.data(), message.size(),
        signature.data(), signature.size(),
        pubkey.data());

    OQS_SIG_free(sig);
    return rc == OQS_SUCCESS;
}

std::array<uint8_t, 32> compute_namespace(std::span<const uint8_t> pubkey) {
    std::array<uint8_t, 32> hash{};
    OQS_SHA3_sha3_256(hash.data(), pubkey.data(), pubkey.size());
    return hash;
}
```

### OpenSSL RAND_bytes for Challenge Generation

```cpp
// Source: OpenSSL API
#include <openssl/rand.h>

std::array<uint8_t, 32> generate_challenge() {
    std::array<uint8_t, 32> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return nonce;
}
```

### Thread Pool Offload Pattern (Asio)

```cpp
// Source: Asio documentation + node's verify_with_offload pattern
// In WsSession::on_message() AWAITING_AUTH handler:

// Parse JSON, extract pubkey_bytes and sig_bytes...

// Offload verification to thread pool.
// asio::post(ioc) dispatches to any thread in the pool.
co_await asio::post(ioc_, asio::use_awaitable);

// Now running on a thread pool thread.
auto result = authenticator_.verify(challenge_, pubkey_bytes, sig_bytes);

// Return to io_context executor for session state mutation.
co_await asio::post(executor_, asio::use_awaitable);

if (!result.success) {
    // Send auth_error and close...
}
// Store identity, transition state...
```

### Hex Encode/Decode Utility

```cpp
// relay_main.cpp already has to_hex(). For Phase 102, extract to a shared header
// or keep inline in authenticator. from_hex is the new need:

std::optional<std::vector<uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned int byte;
        auto [ptr, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, byte, 16);
        if (ec != std::errc{} || ptr != hex.data() + i + 2) return std::nullopt;
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Old relay: PQ handshake (KEM+auth) over TCP | Relay v2: JSON challenge-response over WSS | v3.0.0 (Phase 102) | TLS handles transport security; auth is just identity proof |
| Old relay: blocklist (21 blocked types) | Relay v2: allowlist (38 allowed types) | v3.0.0 (Phase 102) | Safer default: new types blocked until explicitly added |
| Old relay: binary auth payloads | Relay v2: JSON auth messages | v3.0.0 (Phase 102) | Consistent with post-auth JSON protocol |

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | relay/tests/CMakeLists.txt |
| Quick run command | `cd build && ctest -R relay --output-on-failure` |
| Full suite command | `cd build && ctest -R relay --output-on-failure` |

### Phase Requirements -> Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| AUTH-01 | generate_challenge returns 32 random bytes | unit | `ctest -R test_authenticator` | Wave 0 |
| AUTH-02 | verify() accepts valid signature, rejects invalid | unit | `ctest -R test_authenticator` | Wave 0 |
| AUTH-02 | ACL check accepts allowed key, rejects unknown | unit | `ctest -R test_authenticator` | Wave 0 |
| AUTH-03 | 10s timer fires -> Close(4001) | unit | `ctest -R test_authenticator` (timer mock) | Wave 0 |
| AUTH-04 | verify runs on thread pool (not blocking IO) | integration | Manual inspection / TSAN | Manual-only |
| PROT-02 | Schema marks hash fields as hex, data as base64 | unit | `ctest -R test_type_registry` | Wave 0 |
| PROT-03 | Schema marks uint64 fields as string-encoded | unit | `ctest -R test_type_registry` | Wave 0 |
| PROT-05 | Filter allows 38 types, blocks everything else | unit | `ctest -R test_message_filter` | Wave 0 |
| SESS-03 | max_connections from config replaces hardcoded 1024 | unit | `ctest -R test_relay_config` | Extend existing |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R relay --output-on-failure`
- **Per wave merge:** Full relay test suite
- **Phase gate:** Full suite green before verify-work

### Wave 0 Gaps
- [ ] `relay/tests/test_authenticator.cpp` -- covers AUTH-01, AUTH-02, AUTH-03
- [ ] `relay/tests/test_message_filter.cpp` -- covers PROT-05
- [ ] `relay/tests/test_type_registry.cpp` -- covers PROT-02, PROT-03, type string mapping
- [ ] Extend `relay/tests/test_relay_config.cpp` -- covers SESS-03 (max_connections, allowed_client_keys)

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| OpenSSL | RAND_bytes, TLS | Yes | 3.6.2 | -- |
| liboqs | OQS_SIG_verify, SHA3-256 | Yes | 0.15.0 (FetchContent) | -- |
| CMake | Build system | Yes | 4.3.1 | -- |
| g++ | C++20 compilation | Yes | 15.2.1 | -- |
| Catch2 | Unit tests | Yes | 3.7.1 (FetchContent) | -- |

**Missing dependencies:** None. All required tools and libraries are available.

## Open Questions

1. **Ping/Pong/Goodbye at JSON level vs WebSocket level**
   - What we know: Node's supported_types includes Ping(5), Pong(6), Goodbye(7). WebSocket already has native ping/pong.
   - What's unclear: Should JSON-level "ping"/"pong" be forwarded to node via UDS, or should WebSocket-level ping/pong be sufficient? Goodbye (7) is clearly node-level (graceful disconnect from node).
   - Recommendation: Include all three in type registry and allowlist. Phase 103 decides if WS ping/pong replaces node-level ping/pong or if both coexist. For Phase 102, just register them.

2. **Hex utility location**
   - What we know: relay_main.cpp has inline to_hex(). Node has db/util/hex.h. Relay currently doesn't link db/ code.
   - What's unclear: Should relay have its own hex.h or eventually link chromatindb_lib for shared utilities?
   - Recommendation: Create relay-local `relay/util/hex.h` with to_hex() and from_hex(). Keep relay independent of db/. Small code, not worth the coupling.

## Sources

### Primary (HIGH confidence)
- `relay/identity/relay_identity.cpp` -- liboqs OQS_SIG API usage pattern, OQS_SHA3_sha3_256
- `relay/ws/ws_session.h/.cpp` -- Current session lifecycle, idle timer, on_message stub
- `relay/ws/ws_acceptor.h/.cpp` -- Current MAX_CONNECTIONS hardcoded, TLS context management
- `relay/config/relay_config.h/.cpp` -- Config struct, JSON loader, validation
- `relay/core/session.h` -- Send queue pattern, WriteCallback injection
- `db/peer/message_dispatcher.cpp` (lines 534-541) -- Authoritative 38-type supported_types array
- `db/PROTOCOL.md` -- Complete wire format specs for all 62 message types
- `db/wire/transport_generated.h` -- TransportMsgType enum (0-62)
- `db/crypto/verify_helpers.h` -- verify_with_offload pattern (thread pool reference)

### Secondary (MEDIUM confidence)
- OpenSSL RAND_bytes API documentation -- CSPRNG for challenge generation
- liboqs API -- OQS_SIG_verify signature, OQS_SIG_new/free lifecycle

### Tertiary (LOW confidence)
- None -- all findings based on direct source analysis

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all dependencies already present and verified in codebase
- Architecture: HIGH -- patterns derived from existing Phase 100-101 code and CONTEXT.md decisions
- Pitfalls: HIGH -- identified from direct analysis of existing code paths and race conditions
- JSON schema: HIGH -- field specs derived from PROTOCOL.md wire format documentation

**Research date:** 2026-04-09
**Valid until:** 2026-05-09 (stable -- no external dependency changes expected)
