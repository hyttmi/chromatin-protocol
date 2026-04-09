# Phase 102: Authentication & JSON Schema - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-09
**Phase:** 102-authentication-json-schema
**Areas discussed:** Auth protocol flow, JSON schema design, Message type filter, Auth-related config

---

## Auth Protocol Flow

| Option | Description | Selected |
|--------|-------------|----------|
| Relay-only challenge | Relay generates nonce, client signs, relay verifies locally. No node involvement. | + |
| Node-delegated auth | Relay forwards auth to node for verification. | |
| Mutual auth | Both sides prove identity. | |

**User's choice:** Relay-only challenge
**Notes:** Simple, no UDS dependency for auth.

| Option | Description | Selected |
|--------|-------------|----------|
| Immediately on WS upgrade | First message after upgrade is the challenge. 10s timer starts. | + |
| Client-initiated | Client sends hello first. | |
| During upgrade (custom header) | Embed in HTTP headers. | |

**User's choice:** Immediately on WS upgrade

| Option | Description | Selected |
|--------|-------------|----------|
| Drop and disconnect | Any non-auth message before auth -> Close(4001). | + |
| Queue then process | Buffer messages during auth. | |
| Ignore silently | Drop without disconnect. | |

**User's choice:** Drop and disconnect

| Option | Description | Selected |
|--------|-------------|----------|
| JSON error then close | Send auth_error with reason, then Close(4001). | + |
| Close frame reason only | Just close payload. | |
| Silent disconnect | Drop TCP. | |

**User's choice:** JSON error then close

| Option | Description | Selected |
|--------|-------------|----------|
| Store pubkey + namespace hash | Full pubkey (2592B) + SHA3-256 hash (32B). | + |
| Store pubkey hash only | Only 32-byte hash. | |
| Store nothing | Auth is gate-only. | |

**User's choice:** Store pubkey + namespace hash

| Option | Description | Selected |
|--------|-------------|----------|
| Optional ACL in config | allowed_client_keys, empty = open relay. SIGHUP-reloadable. | + |
| Always open | Any valid signer passes. | |
| Mandatory ACL | Must configure allowed keys. | |

**User's choice:** Optional ACL in config

| Option | Description | Selected |
|--------|-------------|----------|
| asio::post to ioc | Offload to existing thread pool. | + |
| Dedicated crypto pool | Separate thread pool for crypto. | |
| Inline on IO thread | Verify directly, blocks thread. | |

**User's choice:** asio::post to ioc

| Option | Description | Selected |
|--------|-------------|----------|
| Single 4001 for all auth failures | One close code, reason in JSON error message. | + |
| Granular codes per failure | 4001/4002/4003 for different failures. | |
| Use 1008 Policy Violation | Standard RFC 6455 code. | |

**User's choice:** Single 4001

| Option | Description | Selected |
|--------|-------------|----------|
| Text frame (JSON) | All auth messages as JSON text frames. | + |
| Binary frame | Raw binary challenge. | |

**User's choice:** Text frame (JSON)

| Option | Description | Selected |
|--------|-------------|----------|
| No relay signature | TLS proves server identity. | + |
| Relay signs challenge too | PQ mutual auth. | |

**User's choice:** No relay signature

| Option | Description | Selected |
|--------|-------------|----------|
| auth_ok with namespace | {"type":"auth_ok","namespace":"<hex>"} | + |
| Minimal auth_ok | Just {"type":"auth_ok"} | |
| Include relay + node info | Rich response with versions. | |

**User's choice:** auth_ok with namespace
**Notes:** User asked why client needs UDS connection for rich response. Clarified: relay needs UDS (Phase 103) for node info. Settled on namespace-only for Phase 102.

| Option | Description | Selected |
|--------|-------------|----------|
| Enum state machine | AWAITING_AUTH -> AUTHENTICATED. Explicit, extensible. | + |
| Boolean flag | Simple authenticated_ bool. | |
| Implicit (no state) | Auth as one-time coroutine gate. | |

**User's choice:** Enum state machine

| Option | Description | Selected |
|--------|-------------|----------|
| Replace idle with auth timer | 10s auth timer on upgrade. Cancel on auth. Post-auth keeps 30s/60s keepalive. | + |
| Separate auth + idle timers | Both running independently. | |
| Reuse idle timer with shorter value | One timer, change timeout. | |

**User's choice:** Replace idle with auth timer

| Option | Description | Selected |
|--------|-------------|----------|
| OpenSSL RAND_bytes | Already linking OpenSSL for TLS. CSPRNG. | + |
| liboqs random | OQS_randombytes(). Indirect. | |
| libsodium randombytes | Would need new dependency. | |

**User's choice:** OpenSSL RAND_bytes

| Option | Description | Selected |
|--------|-------------|----------|
| Connection cap + timeout is enough | 1024 max + 10s timeout. Per-IP is Phase 105. | + |
| Per-IP auth attempt tracking | Ban IPs after N failures. | |

**User's choice:** Connection cap + timeout is enough

| Option | Description | Selected |
|--------|-------------|----------|
| pubkey + signature as hex | Consistent with hex-for-keys convention. | + |
| pubkey hex + signature base64 | Mixed encoding. | |
| Both as base64 | Most compact but breaks convention. | |

**User's choice:** pubkey + signature as hex

| Option | Description | Selected |
|--------|-------------|----------|
| Ignore, already authenticated | Log debug, ignore. No disconnect. | + |
| Disconnect on re-auth | Protocol violation. | |
| Allow re-auth | Process new auth. | |

**User's choice:** Ignore, already authenticated

| Option | Description | Selected |
|--------|-------------|----------|
| Allow multiple sessions | Same pubkey, multiple concurrent sessions. Independent. | + |
| Evict previous session | New auth disconnects old. | |
| Reject duplicate identity | Block second auth from same pubkey. | |

**User's choice:** Allow multiple sessions

| Option | Description | Selected |
|--------|-------------|----------|
| Authenticator class in core/ | Separate class for challenge, verify, ACL. Testable. | + |
| Inline in WsSession | Auth logic in on_message(). | |
| Separate auth session wrapper | AuthSession wraps WsSession. | |

**User's choice:** Authenticator class in core/

| Option | Description | Selected |
|--------|-------------|----------|
| Raw 32-byte nonce | Client signs raw nonce bytes. Simple. | + |
| Nonce with domain prefix | "chromatindb-relay-auth-v1" || nonce. | |
| Nonce + relay pubkey | Bind auth to specific relay. | |

**User's choice:** Raw 32-byte nonce

| Option | Description | Selected |
|--------|-------------|----------|
| Transition state, then send auth_ok | Set AUTHENTICATED first, then send. | + |
| Send auth_ok, then transition | Send first, then set state. | |

**User's choice:** Transition state, then send auth_ok

---

## JSON Schema Design

| Option | Description | Selected |
|--------|-------------|----------|
| String type names | snake_case of enum name. Human-readable. | + |
| Integer type IDs | Match wire protocol directly. | |
| Both (type + type_id) | Redundant but accommodates both. | |

**User's choice:** String type names

| Option | Description | Selected |
|--------|-------------|----------|
| Hex for identity, base64 for data | Hex: namespace, hash, pubkey. Base64: blob data, signatures. | + |
| Everything hex | Simple but wasteful for large payloads. | |
| Everything base64 | Compact but opaque for identity fields. | |
| Hex threshold at 256 bytes | Size-based rule. | |

**User's choice:** Hex for identity, base64 for data

| Option | Description | Selected |
|--------|-------------|----------|
| Strings always | All uint64 as JSON strings. Consistent, no precision loss. | + |
| String only above 2^53 | Numbers below safe integer limit. | |
| Always numbers | Standard JSON numbers. | |

**User's choice:** Strings always

| Option | Description | Selected |
|--------|-------------|----------|
| C++ header with type registry | Schema IS the code. No separate spec file. | + |
| JSON Schema spec + code | Formal JSON Schema documents + code. | |
| Documentation only | Markdown doc. | |

**User's choice:** C++ header with type registry

| Option | Description | Selected |
|--------|-------------|----------|
| Client-chosen, relay-echoed | uint32 as JSON number. Same as binary protocol. | + |
| Client-chosen as string | Allow UUIDs. | |
| Relay-assigned | Relay generates IDs. | |

**User's choice:** Client-chosen, relay-echoed

| Option | Description | Selected |
|--------|-------------|----------|
| Uniform error envelope | {"type":"error","code":"...","message":"..."}. One shape. | + |
| Per-type error responses | Each request type has own error. | |
| HTTP-style status codes | Numeric status codes. | |

**User's choice:** Uniform error envelope

| Option | Description | Selected |
|--------|-------------|----------|
| Omit when default | Optional fields absent = default value. Minimal. | + |
| Always present, explicit defaults | Every field always present. | |
| Null for unset | Use JSON null. | |

**User's choice:** Omit when default

| Option | Description | Selected |
|--------|-------------|----------|
| Schema + type registry only | Define all 38 schemas, no translation. Phase 103 does translation. | + |
| Schema + full translation | Both schema and translation. | |
| Schema + 3-4 example translations | A few proof-of-concept translations. | |

**User's choice:** Schema + type registry only

| Option | Description | Selected |
|--------|-------------|----------|
| Code-only, PROTOCOL.md appendix later | Code is source of truth. Doc when relay ships. | + |
| relay/JSON_PROTOCOL.md alongside code | Separate Markdown doc. | |
| Code comments as documentation | Inline comments only. | |

**User's choice:** Code-only, PROTOCOL.md appendix later

| Option | Description | Selected |
|--------|-------------|----------|
| snake_case of enum name | ReadRequest -> "read_request". Predictable. | + |
| Lowercase joined | ReadRequest -> "readrequest". | |
| camelCase | ReadRequest -> "readRequest". | |

**User's choice:** snake_case of enum name

---

## Message Type Filter

| Option | Description | Selected |
|--------|-------------|----------|
| Allowlist of client types | Explicitly list 38 allowed types. New types blocked by default. | + |
| Blocklist of internal types | List blocked types. Not in list = passes. | |
| Direction-based filter | Classify by direction. | |

**User's choice:** Allowlist of client types

| Option | Description | Selected |
|--------|-------------|----------|
| At JSON parse time | Check type field against allowlist before further parsing. | + |
| After full parse | Parse entire message, then check type. | |
| At translation layer | Filter inside translator. | |

**User's choice:** At JSON parse time

| Option | Description | Selected |
|--------|-------------|----------|
| Error response, keep connection | Send error, don't disconnect. Client bug. | + |
| Error response + disconnect | Send error then close. | |
| Silent drop | Ignore entirely. | |

**User's choice:** Error response, keep connection

| Option | Description | Selected |
|--------|-------------|----------|
| Hardcoded constexpr | Static allowlist. Protocol-defined. | + |
| Config file | Operator-configurable allowed_types. | |

**User's choice:** Hardcoded constexpr

| Option | Description | Selected |
|--------|-------------|----------|
| Type check only in filter | Filter gates, translator validates. | + |
| Filter + basic structure validation | Check type AND required fields. | |
| Full validation in filter | Validate entire JSON structure. | |

**User's choice:** Type check only in filter

---

## Auth-Related Config

| Option | Description | Selected |
|--------|-------------|----------|
| Config field, SIGHUP-reloadable | max_connections in config, default 1024. | + |
| Config field, restart required | Only read at startup. | |
| Keep hardcoded 1024 | Don't make configurable. | |

**User's choice:** Config field, SIGHUP-reloadable

| Option | Description | Selected |
|--------|-------------|----------|
| Hardcoded constexpr | 10s, not configurable. | + |
| Config field | auth_timeout_seconds in config. | |

**User's choice:** Hardcoded constexpr

| Option | Description | Selected |
|--------|-------------|----------|
| Array of hex namespace hashes | 64-char hex strings. Empty = open. SIGHUP-reloadable. | + |
| Array of full hex pubkeys | 5184-char strings. Unwieldy. | |
| File-based ACL | Separate file with one hash per line. | |

**User's choice:** Array of hex namespace hashes

---

## Claude's Discretion

- Internal Authenticator class API design
- JSON schema constexpr data structure design
- Specific error code strings beyond mentioned ones
- Test file organization
- Whether type_registry and json_schema are one header or two

## Deferred Ideas

None -- discussion stayed within phase scope.
