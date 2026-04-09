# Phase 102: Authentication & JSON Schema - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Clients authenticate via ML-DSA-87 challenge-response over WebSocket, the JSON message schema for all 38 relay-allowed types is designed and validated in code, and the message type allowlist filter is implemented. No JSON-to-FlatBuffers translation (Phase 103), no UDS connection (Phase 103), no subscription routing (Phase 104).

</domain>

<decisions>
## Implementation Decisions

### Auth Protocol Flow
- **D-01:** Relay-only challenge-response. Relay generates 32-byte random nonce (OpenSSL RAND_bytes), sends to client as JSON text frame `{"type":"challenge","nonce":"<64-char hex>"}`. Client signs the raw 32-byte nonce with ML-DSA-87, sends back `{"type":"challenge_response","pubkey":"<hex>","signature":"<hex>"}`. Relay verifies locally using liboqs. No node involvement in auth.
- **D-02:** Challenge sent immediately after WebSocket upgrade completes. 10s auth timer starts. Replaces the Phase 101 30s idle timeout during auth phase.
- **D-03:** Any non-auth message received before authentication completes -> send JSON error then Close(4001) + disconnect. Only `challenge_response` accepted in AWAITING_AUTH state.
- **D-04:** Auth failure sends `{"type":"auth_error","reason":"<code>"}` (e.g., "invalid_signature", "timeout", "unknown_key") then Close(4001). Single 4001 close code for all auth failures.
- **D-05:** All auth messages are JSON text WebSocket frames. Consistent with post-auth JSON protocol.
- **D-06:** After successful auth, WsSession stores client's full ML-DSA-87 pubkey (2592 bytes) and SHA3-256(pubkey) namespace hash (32 bytes). Needed for Phase 104 subscription routing and UDS request attribution.
- **D-07:** Optional `allowed_client_keys` in relay config. Array of 64-char hex namespace hashes. Empty/absent = open relay (any valid ML-DSA-87 signer). SIGHUP-reloadable. Mirrors node's ACL pattern.
- **D-08:** ML-DSA-87 signature verification offloaded via `asio::post(ioc)` to the existing hardware_concurrency() thread pool. Same pattern as node's verify_with_offload. No dedicated crypto pool.
- **D-09:** Connection cap (max_connections) + 10s auth timeout is sufficient abuse protection. No per-IP rate limiting (that's Phase 105 OPS-03 scope).
- **D-10:** `auth_ok` response: `{"type":"auth_ok","namespace":"<64-char hex>"}`. Includes client's namespace hash for confirmation. No node info until UDS available (Phase 103+).
- **D-11:** WsSession has explicit state enum: `AWAITING_AUTH -> AUTHENTICATED`. `on_message()` checks state before processing. Clean, extensible for future states.
- **D-12:** No relay signature during auth. TLS certificate proves server identity at transport layer. Relay's ML-DSA-87 identity is for UDS auth to node (Phase 103), not client-facing.
- **D-13:** Duplicate `challenge_response` after already authenticated -> log at debug, ignore silently. No disconnect.
- **D-14:** Multiple concurrent sessions from same client identity (same pubkey) allowed. Each session independent. No dedup, no cross-session coordination.
- **D-15:** Auth logic in new `core/authenticator.h` class. Handles challenge generation, signature verification, ACL check. WsSession calls it during AWAITING_AUTH state. Testable in isolation.
- **D-16:** State transition to AUTHENTICATED happens before sending auth_ok. If auth_ok send fails, session cleaned up normally.
- **D-17:** Client signs the raw 32-byte nonce. No domain prefix, no context binding. Nonce is random and unique per connection.

### JSON Schema Design
- **D-18:** Message types identified by string names in JSON: snake_case of the enum name. `ReadRequest` -> `"read_request"`, `BatchReadResponse` -> `"batch_read_response"`. Type registry maps strings to enum integers internally.
- **D-19:** Binary field encoding: hex for identity/hash fields (namespace 32B, hash 32B, pubkey 2592B), base64 for payload data (blob data up to 100 MiB, signatures 4627B). Consistent with architecture doc.
- **D-20:** All uint64 fields (seq_num, timestamp, byte counts) encoded as JSON strings. Always. No conditional number/string based on magnitude. Per PROT-03.
- **D-21:** Schema defined in C++ headers: `translate/type_registry.h` (string name <-> enum mapping), `translate/json_schema.h` (field names, encoding rules as constexpr data). The schema IS the code. No separate JSON Schema spec file.
- **D-22:** `request_id` is client-chosen uint32 as JSON number, echoed by relay/node in responses. Same semantics as binary protocol. Absent = fire-and-forget.
- **D-23:** Uniform error envelope for all relay-originated errors: `{"type":"error","request_id":N,"code":"<machine_code>","message":"<human_readable>"}`. One error shape for bad JSON, blocked type, unauthed, etc.
- **D-24:** Optional fields omitted from JSON when they have default/zero value. Relay fills defaults during translation. Keeps messages minimal.
- **D-25:** Phase 102 scope: schema definition + type registry + message filter. Actual JSON<->FlatBuffers translation is Phase 103 (PROT-01).
- **D-26:** Code-only schema documentation. PROTOCOL.md JSON appendix added when relay ships. No separate schema document.

### Message Type Filter
- **D-27:** Allowlist of 38 client-allowed type strings. Any type not in allowlist is rejected. New protocol types blocked by default until explicitly added. Safer than blocklist.
- **D-28:** Filter check at JSON parse time: extract "type" field, check allowlist before any further parsing or translation. Early rejection saves CPU. Also filters outbound (node responses with disallowed types never forwarded).
- **D-29:** Blocked type -> error response `{"type":"error","code":"blocked_type","message":"Message type 'X' is not allowed"}` with request_id if present. Keep connection open (client bug, not malicious).
- **D-30:** Allowlist is hardcoded constexpr in `core/message_filter.h`. Protocol-defined, not operator-configurable. Changes require relay rebuild.
- **D-31:** Filter only checks type string. Field-level structure validation happens in the translation layer (Phase 103).

### Auth-Related Config
- **D-32:** `max_connections` field in RelayConfig. Default 1024. SIGHUP-reloadable. Over-limit: log warning, no mass disconnect. Same pattern as node's max_peers.
- **D-33:** Auth timeout (10s) hardcoded as constexpr. Not configurable. Operators don't need to tune this.
- **D-34:** `allowed_client_keys` as JSON array of 64-char hex namespace hashes. Empty array or absent = open relay. SIGHUP-reloadable.

### Claude's Discretion
- Internal Authenticator class API design (method signatures, constructor parameters)
- JSON schema constexpr data structure design in translate/ headers
- Specific error code strings beyond the ones mentioned
- Test file organization within relay/tests/
- Whether type_registry and json_schema are one header or two

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Architecture
- `.planning/research/ARCHITECTURE.md` -- Component layout, auth flow diagram (lines 320-333), JSON schema design (lines 246-256), message type filter concept
- `.planning/research/STACK.md` -- Technology stack decisions

### Protocol
- `db/PROTOCOL.md` -- Wire format spec, all 62 message types (lines 741-810), relay-blocked types, supported_types concept (line 739), auth handshake patterns

### Existing Code (Phase 100-101 output)
- `relay/identity/relay_identity.h` -- ML-DSA-87 identity with sign() method, PUBLIC_KEY_SIZE/SIGNATURE_SIZE constants
- `relay/ws/ws_session.h` -- WsSession with on_message(), write_frame(), idle timer, state tracking location
- `relay/ws/ws_session.cpp` -- Session lifecycle implementation (add auth state machine here)
- `relay/ws/session_manager.h` -- SessionManager with add/remove/get/for_each
- `relay/ws/ws_acceptor.h` -- WsAcceptor with 1024 hardcoded limit (make configurable)
- `relay/core/session.h` -- Send queue with enqueue/drain, WriteCallback injection
- `relay/config/relay_config.h` -- RelayConfig struct (add max_connections, allowed_client_keys)
- `relay/config/relay_config.cpp` -- Config loader (extend for new fields)
- `relay/relay_main.cpp` -- Main with SIGHUP handler (extend for config reload)

### Node Patterns (reference only, not modified)
- `db/net/auth_helpers.h` -- Auth payload encode/decode pattern (LE pubkey_size). Reference for auth payload structure, NOT reused in relay (relay uses JSON, not binary auth payloads).
- `db/crypto/signing.h` -- ML-DSA-87 constants (PUBLIC_KEY_SIZE = 2592, SIGNATURE_SIZE = 4627). RelayIdentity already has its own copies.

### Requirements
- `.planning/REQUIREMENTS.md` -- AUTH-01, AUTH-02, AUTH-03, AUTH-04, PROT-02, PROT-03, PROT-05, SESS-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/identity/relay_identity.h` -- Has sign() but no verify(). Need standalone ML-DSA-87 verify function for auth (or add static verify method). liboqs OQS_SIG_verify() directly.
- `relay/ws/ws_session.h` -- on_message() currently receives opcode + data. Auth state machine hooks here. idle_timer_ can be repurposed as auth_timer_ during AWAITING_AUTH state.
- `relay/config/relay_config.h` -- RelayConfig struct ready to extend with max_connections and allowed_client_keys fields.
- `relay/ws/ws_acceptor.h` -- Hardcoded 1024 max connections. Phase 102 makes this `max_connections_` from config.
- `relay/relay_main.cpp` -- SIGHUP handler already reloads TLS. Extend for allowed_client_keys and max_connections reload.

### Established Patterns
- Send queue: core::Session enqueue() -> drain coroutine -> WsSession::write_frame(). Auth messages flow through same path.
- Config: nlohmann::json loader with validate_relay_config(). Add new field validation.
- Signal handling: SIGHUP member coroutine pattern (not lambda, per memory note on stack-use-after-return).
- Testing: Catch2 unit tests in relay/tests/. test_session.cpp, test_ws_frame.cpp, test_ws_handshake.cpp as patterns.

### Integration Points
- `relay/ws/ws_session.cpp` -- Add SessionState enum, auth state machine in on_message(), auth timer
- `relay/core/` -- New authenticator.h/cpp
- `relay/translate/` -- New type_registry.h, json_schema.h (empty dir with .gitkeep currently)
- `relay/core/` -- New message_filter.h
- `relay/config/relay_config.h` -- Add max_connections, allowed_client_keys
- `relay/ws/ws_acceptor.cpp` -- Replace hardcoded 1024 with config value

</code_context>

<specifics>
## Specific Ideas

- Auth flow mirrors the node's challenge-response conceptually but uses JSON over WebSocket instead of binary over TCP. No shared code with db/ auth.
- The 38-type allowlist is derived from PROTOCOL.md's supported_types list in NodeInfoResponse. Cross-reference to ensure completeness.
- Schema design before translation code -- Phase 102 defines the contract, Phase 103 implements the conversion. This ordering prevents translating without a clear spec.
- hex for pubkey in challenge_response is consistent but large (5184 chars for 2592 bytes). Accepted trade-off for consistency with "hex for identity fields" convention.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 102-authentication-json-schema*
*Context gathered: 2026-04-09*
