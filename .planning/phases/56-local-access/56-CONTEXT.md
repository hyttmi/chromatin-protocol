# Phase 56: Local Access - Context

**Gathered:** 2026-03-22
**Status:** Ready for planning

<domain>
## Phase Boundary

Local processes can read and write blobs via Unix Domain Socket without TCP overhead or PQ crypto handshake. UDS is just an alternative transport — same wire protocol, same enforcement (ACL, rate limiting, quotas), full message type support. Single requirement: UDS-01.

</domain>

<decisions>
## Implementation Decisions

### UDS socket lifecycle
- Unlink stale socket on startup: if uds_path exists and bind fails, unlink and retry. Standard practice (systemd, nginx).
- Unlink socket on clean shutdown: clean up after ourselves.
- Socket permissions: 0660 (owner + group read/write). Allows group-based access control.

### Connection identity and enforcement
- Full enforcement: UDS connections get the SAME ACL, rate limiting, and quota enforcement as TCP connections. No exceptions.
- Identity via TrustedHello path: UDS clients send their pubkey using the existing TrustedHello handshake (no PQ crypto, just identity exchange). Reuses existing code. Client must have a keypair.
- All message types supported: write, read, subscribe, delete — UDS is a transport, not a restricted interface.

### Config and reload
- Config field: `uds_path` (string, empty by default = UDS disabled)
- Empty string = disabled: zero behavior change for existing configs. Operator explicitly enables by setting a path.
- Absolute path required: validate_config rejects relative paths (prevents ambiguity about socket location).
- NOT SIGHUP-reloadable: changing uds_path requires restart. Consistent with bind_address not being reloadable. Starting/stopping listeners at runtime is unnecessary complexity.

### Claude's Discretion
- Whether to create a separate UdsServer class or extend Server with UDS support
- How Connection class handles UDS socket vs TCP socket (template, variant, or separate class)
- Test strategy for UDS (unit tests with local socket pairs, integration tests)
- Whether to add UDS listener info to SIGUSR1 metrics dump
- PROTOCOL.md updates for UDS transport section

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Server` class in `db/net/server.h` — TCP acceptor + accept_loop pattern. UDS listener needs similar structure with `asio::local::stream_protocol`.
- `Connection` class in `db/net/connection.h` — wraps TCP socket. UDS needs same framing/messaging but different underlying socket type.
- TrustedHello handshake path — already handles identity exchange without PQ crypto. UDS connections use this directly.
- `is_trusted_address()` in PeerManager — trust check lambda chain. UDS connections are always trusted (skip PQ).
- `accept_filter_` in Server — connection limit filter. Applies to UDS connections too.
- Config validation pattern: add to Config struct, parse from_json, validate_config, known_keys, log at startup.

### Established Patterns
- Standalone Asio with C++20 coroutines for all async I/O
- Timer-cancel pattern for async loops
- PeerManager orchestrates Server + connections + all peer lifecycle
- ACL gating at on_peer_connected after handshake
- Config fields follow: struct member, JSON parsing, validation, startup log

### Integration Points
- `db/config/config.h` — add `uds_path` field (string, empty default)
- `db/config/config.cpp` — parse, validate (absolute path if non-empty), known_keys
- `db/net/server.h` / `db/net/server.cpp` — UDS listener (new class or extended Server)
- `db/net/connection.h` — UDS connection support (same wire protocol, different socket)
- `db/peer/peer_manager.cpp` — start UDS listener if uds_path is set, wire trust check to always return true for UDS
- `db/main.cpp` — startup log for UDS path

</code_context>

<specifics>
## Specific Ideas

- Asio's `local::stream_protocol` provides Unix domain socket support. `local::stream_protocol::acceptor` and `local::stream_protocol::socket` are the UDS equivalents of TCP.
- The existing Connection class may need to become templated on socket type, or a separate UdsConnection class could wrap a UDS socket with the same framing interface.
- UDS connections should mark themselves as trusted so the TrustedHello path is used automatically.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 56-local-access*
*Context gathered: 2026-03-22*
