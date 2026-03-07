# Phase 10: Access Control - Research

**Researched:** 2026-03-06
**Domain:** Connection-level access control, Unix signal handling, runtime config reload
**Confidence:** HIGH

## Summary

Phase 10 adds an allowed_keys whitelist to chromatindb so node operators can restrict which pubkeys connect. The implementation touches four areas: (1) config parsing for the `allowed_keys` JSON array, (2) ACL gating in PeerManager after handshake, (3) PEX disable in closed mode, and (4) SIGHUP-based hot-reload with immediate revocation.

The codebase is well-structured for this. The `on_ready` callback in Connection fires after handshake but before message loop -- the exact right place for ACL checks. The Server already has `asio::signal_set signals_` for SIGINT/SIGTERM, so SIGHUP can be added alongside. PeerManager runs on a single io_context thread, so the SIGHUP handler must post to io_context (async-signal-safe pattern via Asio's signal_set, which already handles this correctly).

**Primary recommendation:** Introduce an `AccessControl` class that owns the allowed_keys set and provides `is_allowed(namespace_hash)` and `is_closed_mode()` queries. PeerManager holds an instance and checks it in `on_peer_connected`. SIGHUP reloads by re-parsing config and swapping the set atomically (single-threaded, so just replace).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- `allowed_keys` is a JSON array of hex strings -- SHA3-256 namespace hashes (64 hex chars each)
- No labels, no objects -- just raw hex strings
- Strict validation at startup: malformed keys (wrong length, non-hex) cause the daemon to refuse to start with a clear error
- The node's own namespace is implicitly allowed even if not listed -- prevents accidental self-lockout
- Silent TCP close after handshake -- no protocol-level denial message, no information leak
- No rate limiting -- the PQ handshake cost (ML-KEM + ML-DSA) is already a natural rate limiter
- ACL gates both directions: inbound connections AND outbound connections (including bootstrap peers) are checked against allowed_keys
- On SIGHUP key revocation: immediate disconnect, even mid-sync. Revoked means revoked
- Rejected connections logged at **warn** level with namespace hash + IP address
- Startup logs mode clearly: e.g. `access control: closed mode (3 allowed keys)` or `access control: open mode`
- SIGHUP reload logs a diff summary: keys added, keys removed, peers disconnected
- SIGHUP reloads **only** `allowed_keys` from the config file -- not bind_address, max_peers, or other fields
- Config file path remembered from startup (--config flag or default)
- If config file is missing or has invalid JSON during reload: log error, keep current allowed_keys unchanged (fail-safe)
- Same strict validation on reload as startup: if any key is malformed, reject the entire reload and keep current config

### Claude's Discretion
- SIGHUP signal handler implementation details (async-signal-safe patterns)
- Exact gating point in Connection lifecycle (likely on_ready callback)
- Internal data structure for allowed_keys lookup (set vs unordered_set)
- PEX disable mechanism in closed mode (skip timer loop vs no-op handlers)

### Deferred Ideas (OUT OF SCOPE)
- None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| ACL-01 | Node operator can specify `allowed_keys` in config JSON to restrict which pubkeys can connect (non-empty = closed mode, empty = open mode) | Config struct extension + `allowed_keys` parsing with hex validation; AccessControl class for mode queries |
| ACL-02 | Unauthorized peers are rejected at connection level after handshake, before entering PeerManager state | ACL check in `on_peer_connected` callback, which fires from `on_ready` (after handshake, before message loop); silent close via `conn->close()` |
| ACL-03 | PEX is disabled when node is in closed mode (non-empty `allowed_keys`) | Guard `pex_timer_loop` and `handle_pex_as_responder` with `is_closed_mode()` check; also skip PEX in inline post-sync exchange |
| ACL-04 | Node operator can hot-reload `allowed_keys` via SIGHUP without restarting the daemon | `asio::signal_set` for SIGHUP in Server, posts reload to io_context; re-parses config file, validates, swaps allowed_keys set |
| ACL-05 | Peers whose pubkey is revoked from `allowed_keys` are disconnected on config reload | After allowed_keys swap, iterate peers_ and close any whose namespace hash is no longer in set |
</phase_requirements>

## Standard Stack

### Core (already in project)
| Library | Version | Purpose | Relevance |
|---------|---------|---------|-----------|
| Standalone Asio | 1.38.0 | async IO, signal_set, coroutines | signal_set handles SIGHUP async-safely; already used for SIGINT/SIGTERM |
| nlohmann/json | 3.11.3 | Config parsing | Already parses config; extend for `allowed_keys` array |
| spdlog | 1.15.1 | Structured logging | Logging ACL decisions at warn/info levels |
| Catch2 | 3.7.1 | Testing | Unit tests for ACL logic, config parsing, integration |

### No New Dependencies
This phase requires zero new libraries. Everything is achievable with existing stack.

## Architecture Patterns

### Pattern 1: AccessControl Class
**What:** A dedicated class that owns the allowed_keys set and answers access queries.
**Why:** Separates ACL logic from PeerManager, making it testable in isolation. PeerManager already has too many responsibilities.
**Structure:**
```cpp
// db/acl/access_control.h
namespace chromatindb::acl {

class AccessControl {
public:
    using NamespaceHash = std::array<uint8_t, 32>;

    /// Construct from config (validates keys, may throw)
    explicit AccessControl(const std::vector<std::string>& hex_keys,
                           std::span<const uint8_t, 32> own_namespace);

    /// Check if a namespace hash is allowed
    bool is_allowed(std::span<const uint8_t, 32> namespace_hash) const;

    /// Whether access control is active (non-empty allowed_keys)
    bool is_closed_mode() const;

    /// Number of allowed keys (excluding implicit self)
    size_t allowed_count() const;

    /// Reload from new key list. Returns {added, removed} counts for logging.
    struct ReloadResult { size_t added; size_t removed; };
    ReloadResult reload(const std::vector<std::string>& hex_keys);

private:
    std::set<NamespaceHash> allowed_keys_;
    NamespaceHash own_namespace_;
};

} // namespace chromatindb::acl
```

### Pattern 2: Namespace Hash Derivation for ACL Check
**What:** Peer presents signing pubkey during handshake. ACL checks the SHA3-256 hash of that pubkey (the namespace hash).
**How:** `identity::NodeIdentity::namespace_id()` already does `SHA3-256(signing_pubkey)`. Same hash function applied to `conn->peer_pubkey()`.
```cpp
// In ACL check:
auto peer_ns = crypto::sha3_256(conn->peer_pubkey());
if (!acl.is_allowed(peer_ns)) {
    conn->close();
    return;
}
```

### Pattern 3: SIGHUP via Asio signal_set
**What:** Asio's `signal_set` is the correct way to handle Unix signals in an Asio program. It defers signal delivery to the io_context event loop, making it safe to call any code (not just async-signal-safe functions).
**Existing pattern:** Server already uses `signals_(ioc, SIGINT, SIGTERM)` and `signals_.async_wait(...)`. SIGHUP can be added to the same signal_set, or a separate one for cleaner separation.
**Important:** `signal_set::add(SIGHUP)` can be called after construction. Alternatively, construct a new signal_set for SIGHUP. Using a separate signal_set is cleaner since SIGHUP has different semantics than SIGINT/SIGTERM.
```cpp
// In Server or PeerManager:
asio::signal_set sighup_signal_(ioc, SIGHUP);
sighup_signal_.async_wait([this](asio::error_code ec, int) {
    if (ec) return;
    reload_config();
    // Re-arm for next SIGHUP
    setup_sighup_handler();
});
```

### Pattern 4: Config Path Retention
**What:** Store the config file path from startup so SIGHUP can re-read it.
**Where:** Either in Config struct as a new field, or passed to PeerManager separately.
**Recommendation:** Add `std::filesystem::path config_path` to Config struct. Set during `parse_args()` when `--config` is provided.

### Pattern 5: ACL Gating Point
**What:** The ACL check must happen after handshake (we need peer_pubkey) but before the peer enters PeerManager state.
**Where:** `on_peer_connected()` is called from the `on_ready` callback, which fires after `do_handshake()` succeeds but before `message_loop()` starts. This is the correct gating point.
**For outbound connections:** Same `on_peer_connected()` fires for both inbound and outbound. The ACL check applies uniformly.

### Anti-Patterns to Avoid
- **Checking ACL in accept_filter:** The accept_filter fires BEFORE handshake. At that point we don't know the peer's pubkey. ACL must be post-handshake.
- **Using std::atomic for allowed_keys:** Unnecessary. PeerManager runs on a single io_context thread. SIGHUP handler posts to io_context, so reload runs on the same thread. No data races.
- **Modifying Connection class:** ACL is a policy decision that belongs in PeerManager, not Connection. Connection should remain a pure transport abstraction.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Hex string validation | Custom regex/parser | Simple loop checking length==64 and chars in [0-9a-fA-F] | Simple enough, but must handle both cases |
| Signal handling | raw signal(), sigaction() | asio::signal_set | Already async-safe, integrates with event loop |
| Thread-safe config swap | Mutex, atomic shared_ptr | Single-threaded swap (post to io_context) | PeerManager is explicitly single-threaded |

## Common Pitfalls

### Pitfall 1: Forgetting to Re-arm SIGHUP Handler
**What goes wrong:** Asio signal_set delivers once, then stops listening. If you don't call async_wait again after handling, subsequent SIGHUPs are ignored.
**How to avoid:** Always re-arm in the handler callback before any early returns.

### Pitfall 2: ACL Check After Peer Added to peers_ Deque
**What goes wrong:** If you add the peer to `peers_` first and then check ACL, a disconnect during the check leaves stale entries.
**How to avoid:** Check ACL FIRST in `on_peer_connected`. Only add to `peers_` if allowed. For rejected peers, close immediately and return.

### Pitfall 3: Bootstrap Peer Reconnect Loop with ACL
**What goes wrong:** If a bootstrap peer's key is not in allowed_keys, the Server's reconnect_loop will keep reconnecting, handshaking, and getting rejected forever.
**How to avoid:** The ACL rejection happens in `on_peer_connected` (PeerManager layer). The Server doesn't know about ACL. When the connection closes immediately after handshake, Server's reconnect_loop will re-trigger. This is acceptable -- the reconnect backoff (exponential up to 60s) limits the cost. Alternatively, consider whether bootstrap peers should be implicitly allowed. Per CONTEXT.md, they are NOT -- ACL gates all directions.

### Pitfall 4: Mid-Sync Disconnect Cleanup
**What goes wrong:** When revoking a key via SIGHUP, the peer might be mid-sync with `syncing = true`. Closing the connection without proper cleanup could leave sync state inconsistent.
**How to avoid:** Use `conn->close()` (not `close_gracefully()` -- no goodbye to unauthorized peers). The `on_peer_disconnected` callback handles removal from `peers_`. The sync coroutine will detect the closed connection on the next recv/send and exit.

### Pitfall 5: PEX Inline After Sync in Closed Mode
**What goes wrong:** The inline PEX exchange at the end of `run_sync_with_peer` and `handle_sync_as_responder` runs even in closed mode, leaking peer addresses.
**How to avoid:** Guard both inline PEX blocks with `!acl.is_closed_mode()`. Also guard the standalone PEX timer loop and responder.

### Pitfall 6: Own Namespace Not in allowed_keys
**What goes wrong:** Operator forgets to include own key in allowed_keys. The node would reject connections to itself (which doesn't happen in practice, but the implicit allow prevents confusion in any edge case).
**How to avoid:** Per CONTEXT.md, own namespace is implicitly allowed. AccessControl constructor takes `own_namespace` and always includes it.

## Code Examples

### Hex String Validation
```cpp
bool is_valid_hex_key(const std::string& key) {
    if (key.size() != 64) return false;
    for (char c : key) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

std::array<uint8_t, 32> hex_to_bytes(const std::string& hex) {
    std::array<uint8_t, 32> result{};
    for (size_t i = 0; i < 32; ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        result[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }
    return result;
}
```

### ACL Check in on_peer_connected
```cpp
void PeerManager::on_peer_connected(net::Connection::Ptr conn) {
    // Derive peer's namespace hash from their signing pubkey
    auto peer_ns = crypto::sha3_256(conn->peer_pubkey());

    // ACL check (before adding to peers_)
    if (!acl_.is_allowed(peer_ns)) {
        spdlog::warn("access denied: namespace={} ip={}",
                      to_hex(peer_ns), conn->remote_address());
        conn->close();  // Silent close, no goodbye
        return;
    }

    // ... existing on_peer_connected logic ...
}
```

### SIGHUP Reload
```cpp
void PeerManager::setup_sighup_handler() {
    sighup_signal_.async_wait([this](asio::error_code ec, int) {
        if (ec) return;
        handle_sighup();
        setup_sighup_handler();  // Re-arm
    });
}

void PeerManager::handle_sighup() {
    spdlog::info("SIGHUP received, reloading config...");

    // Re-read config file (path stored from startup)
    try {
        auto new_cfg = config::load_config(config_path_);
        auto result = acl_.reload(new_cfg.allowed_keys);

        spdlog::info("config reload: +{} keys, -{} keys",
                      result.added, result.removed);

        // Disconnect revoked peers
        if (result.removed > 0) {
            disconnect_unauthorized_peers();
        }
    } catch (const std::exception& e) {
        spdlog::error("config reload failed: {} (keeping current config)", e.what());
    }
}

void PeerManager::disconnect_unauthorized_peers() {
    size_t disconnected = 0;
    auto snapshot = peers_;  // Copy -- disconnection modifies peers_
    for (const auto& peer : snapshot) {
        auto peer_ns = crypto::sha3_256(peer.connection->peer_pubkey());
        if (!acl_.is_allowed(peer_ns)) {
            spdlog::warn("revoking peer: namespace={} ip={}",
                          to_hex(peer_ns), peer.address);
            peer.connection->close();
            disconnected++;
        }
    }
    spdlog::info("config reload: disconnected {} peers", disconnected);
}
```

## Open Questions

1. **Config path default behavior**
   - What we know: `parse_args` supports `--config <path>`. If not provided, no config file is loaded.
   - What's unclear: If no `--config` is provided, SIGHUP has nothing to reload from. Should we log a warning?
   - Recommendation: If no config path was provided at startup, SIGHUP logs "no config file to reload" and does nothing. Document this behavior.

2. **Outbound ACL and reconnect loops**
   - What we know: ACL gates both directions. Bootstrap peers will keep reconnecting via Server::reconnect_loop.
   - What's unclear: Is repeated handshake + immediate close for disallowed bootstrap peers acceptable?
   - Recommendation: Yes, acceptable. The exponential backoff limits cost. An operator who puts a peer in bootstrap_peers but not allowed_keys has a configuration error, and the warn-level log makes it visible.

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/net/server.cpp` -- signal_set pattern for SIGINT/SIGTERM
- Codebase analysis: `db/net/connection.cpp` -- `run()` flow: handshake -> on_ready -> message_loop
- Codebase analysis: `db/peer/peer_manager.cpp` -- `on_peer_connected`, PEX timer/responder patterns
- Codebase analysis: `db/config/config.cpp` -- JSON parsing with nlohmann/json
- Codebase analysis: `db/identity/identity.h` -- `namespace_id()` = SHA3-256(pubkey)

### Secondary (MEDIUM confidence)
- Asio documentation: `signal_set` delivers signals via io_context, async-signal-safe by design

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new deps, all existing
- Architecture: HIGH - patterns follow existing codebase conventions exactly
- Pitfalls: HIGH - derived from direct codebase analysis

**Research date:** 2026-03-06
**Valid until:** 2026-04-06 (stable domain, codebase-specific)
