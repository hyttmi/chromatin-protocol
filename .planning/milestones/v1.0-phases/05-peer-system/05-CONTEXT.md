# Phase 5: Peer System - Context

**Gathered:** 2026-03-04
**Status:** Ready for planning

<domain>
## Phase Boundary

Nodes discover each other via configured bootstrap peers, synchronize their blob stores bidirectionally via per-namespace hash-list diff, and operate as a running daemon with subcommand CLI. Peer exchange (PEX/DISC-03) and advanced sync features (resumable sync, XXH3 fingerprints) are deferred to v2.

</domain>

<decisions>
## Implementation Decisions

### Daemon invocation
- Subcommand structure: `chromatindb run` (daemon), `chromatindb keygen` (generate identity), `chromatindb version`
- Rich startup output: version, bind address, data dir, identity namespace hash, bootstrap peers listed, then connection/sync events as they happen
- `keygen` refuses if identity file already exists — requires `--force` to overwrite
- Keypair stored inside data-dir: `{data-dir}/identity.key`

### Sync observability
- Summary-per-sync-round at info level: "Synced with peer X: received N blobs, sent M blobs"
- Individual blob transfers logged at debug level only
- Both peer connect and disconnect events logged at info level: "Connected to peer abc123@1.2.3.4:4200" / "Peer abc123 disconnected (graceful/timeout)"
- Invalid blobs during sync logged at warn level
- Strike system: track validation failures per peer, disconnect after threshold (e.g., 10 invalid blobs), cooldown before reconnect

### Peer limits & behavior
- Hard cap on max peer connections, configurable in JSON config (e.g., default 32)
- Reject inbound connections above cap, prioritize bootstrap peers
- Start daemon even if all bootstrap peers are unreachable — accept inbound connections, retry bootstrap in background with exponential backoff
- Bootstrap-only discovery for v1 — nodes only connect to configured bootstrap peers, no peer list exchange
- Reconnect bootstrap peers only — non-bootstrap peer disconnections are not retried

### Sync triggering
- Sync on connect (full sync immediately after handshake) + periodic sync on a configurable timer
- Either side can initiate a sync round — each peer manages its own sync timer independently
- Configurable sync interval: `sync_interval_seconds` in JSON config (default 60s)
- Per-namespace hash-list diff: exchange namespace list first, then per shared namespace exchange blob hash lists

### Claude's Discretion
- Exact sync protocol message types and FlatBuffers schema additions
- Hash-list diff algorithm implementation details
- Strike system threshold and cooldown duration
- Max peers default value
- Sync batch sizes and flow control
- How to handle partial sync failures (namespace-level retry vs full restart)
- Expiry filtering implementation during sync (SYNC-03)

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Server` (src/net/server.h): Already has `connect_to_peer()`, `reconnect_loop()` with exponential backoff, signal handling, connection lifecycle management
- `Connection` (src/net/connection.h): Has `on_message` callback, `send_message`, `peer_pubkey()`, Ping/Pong/Goodbye already implemented
- `BlobEngine` (src/engine/engine.h): `get_blobs_since()`, `list_namespaces()`, `get_blob()` — sync query primitives exist
- `Storage` (src/storage/storage.h): `has_blob()` for dedup checks during sync, `store_blob()` for ingest
- `TransportCodec` (src/net/protocol.h): FlatBuffers transport message encode/decode
- `NodeIdentity` (src/identity/identity.h): Auto-keygen with keypair persistence
- `Config` (src/config/config.h): Already has `bootstrap_peers`, `bind_address`, `data_dir` — needs `max_peers` and `sync_interval_seconds`

### Established Patterns
- Asio coroutines (C++20 `co_await`) for all async networking
- `use_nothrow` token for non-throwing async ops
- FlatBuffers for all wire format messages (transport_generated.h)
- RAII wrappers for all crypto resources
- Catch2 for testing with structured test cases

### Integration Points
- `main()` needs to create: Config, Storage, BlobEngine, NodeIdentity, Server, io_context — wire them together
- Server needs BlobEngine reference to handle incoming sync data
- New TransportMsgType values needed for sync protocol messages
- Config needs new fields: `max_peers`, `sync_interval_seconds`

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 05-peer-system*
*Context gathered: 2026-03-04*
