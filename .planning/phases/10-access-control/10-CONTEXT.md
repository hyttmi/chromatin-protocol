# Phase 10: Access Control - Context

**Gathered:** 2026-03-06
**Status:** Ready for planning

<domain>
## Phase Boundary

Node operators can restrict which pubkeys connect by specifying `allowed_keys` in the config JSON. Non-empty list = closed mode (rejects unauthorized peers), empty list = open mode (backward compatible with v1.0). SIGHUP reloads allowed_keys without restart. Revoked peers are immediately disconnected.

</domain>

<decisions>
## Implementation Decisions

### Key format & identification
- `allowed_keys` is a JSON array of hex strings — SHA3-256 namespace hashes (64 hex chars each)
- No labels, no objects — just raw hex strings
- Strict validation at startup: malformed keys (wrong length, non-hex) cause the daemon to refuse to start with a clear error
- The node's own namespace is implicitly allowed even if not listed — prevents accidental self-lockout

### Rejection behavior
- Silent TCP close after handshake — no protocol-level denial message, no information leak
- No rate limiting — the PQ handshake cost (ML-KEM + ML-DSA) is already a natural rate limiter
- ACL gates both directions: inbound connections AND outbound connections (including bootstrap peers) are checked against allowed_keys
- On SIGHUP key revocation: immediate disconnect, even mid-sync. Revoked means revoked.

### Logging & observability
- Rejected connections logged at **warn** level with namespace hash + IP address
- Startup logs mode clearly: e.g. `access control: closed mode (3 allowed keys)` or `access control: open mode`
- SIGHUP reload logs a diff summary: keys added, keys removed, peers disconnected (e.g. `config reload: +2 keys, -1 key, disconnected 1 peer`)

### Config reload scope
- SIGHUP reloads **only** `allowed_keys` from the config file — not bind_address, max_peers, or other fields
- Config file path remembered from startup (--config flag or default)
- If config file is missing or has invalid JSON during reload: log error, keep current allowed_keys unchanged (fail-safe)
- Same strict validation on reload as startup: if any key is malformed, reject the entire reload and keep current config

### Claude's Discretion
- SIGHUP signal handler implementation details (async-signal-safe patterns)
- Exact gating point in Connection lifecycle (likely on_ready callback)
- Internal data structure for allowed_keys lookup (set vs unordered_set)
- PEX disable mechanism in closed mode (skip timer loop vs no-op handlers)

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Connection::peer_pubkey()` — returns peer's signing pubkey after handshake, can derive namespace hash from this
- `Connection::on_ready()` callback — fires after handshake, before message loop; natural ACL gating point
- `identity::NodeIdentity::namespace_id()` — already computes SHA3-256(signing_pubkey), same hash format for allowed_keys
- `config::load_config()` — JSON parsing with nlohmann/json, can be extended for allowed_keys field

### Established Patterns
- Config struct loaded once at startup via `load_config()` + `parse_args()` — needs extension for runtime reload
- Server uses `AcceptFilter` callback (pre-handshake) — ACL needs post-handshake gating (different point)
- PeerManager runs on single io_context thread (not thread-safe) — SIGHUP handler must post to io_context
- PEX has dedicated timer loop (`pex_timer_loop`) and responder (`handle_pex_as_responder`) — both need closed-mode check
- Server already has `asio::signal_set signals_` — SIGHUP handler can be added alongside existing signal handling

### Integration Points
- `Config` struct needs `allowed_keys` field (vector<string> or set)
- `PeerManager::on_peer_connected()` — where ACL check would run after handshake
- `PeerManager` needs access to a reloadable ACL list (not just const Config&)
- `main.cpp` — config path needs to be stored for SIGHUP reload
- PEX timer loop and responder need closed-mode awareness

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 10-access-control*
*Context gathered: 2026-03-06*
