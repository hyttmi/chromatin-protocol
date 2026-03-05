# Feature Landscape

**Domain:** Closed node access control + larger blob support for decentralized PQ-secure database node
**Researched:** 2026-03-05
**Milestone:** v2.0

## Table Stakes

Features that are baseline expectations for a closed/private node model and larger blob support. Missing any of these makes the feature set feel broken or half-implemented.

| Feature | Why Expected | Complexity | Dependencies on Existing | Notes |
|---------|--------------|------------|--------------------------|-------|
| **Allowed-keys config** | Core of the closed node model. Without a pubkey whitelist there is no access control. Standard pattern in Nostr relays (strfry, nostr-rs-relay) and private p2p nodes (Quorum, Tendermint). | LOW | `config::Config` struct, `config::load_config()`, `config::parse_args()` | Add `std::vector<std::string> allowed_keys` to Config, parse from JSON config file. Hex-encoded pubkey strings (ML-DSA-87 pubkeys are 2592 bytes = 5184 hex chars). |
| **Connection-level auth gating** | After handshake completes, `peer_pubkey()` is available. Check it against the allowed-keys list. Reject unauthorized peers immediately -- do not let them send blobs or query data. This is how every private p2p network works (Quorum, Tendermint, Bitcoin permissioned forks). | LOW | `Connection::run()` -> `do_handshake()` -> `ready_cb_`, `Server::set_accept_filter()`, `PeerManager::on_peer_connected()` | The handshake already reveals the peer's ML-DSA-87 pubkey. After auth succeeds, check against allowed_keys. If not allowed, send Goodbye and close. No protocol changes needed. |
| **Open mode preservation** | If allowed_keys is empty/absent, node operates exactly as v1.0 (permissionless). Users who don't want access control should not be forced into it. | LOW | `config::Config` | Empty allowed_keys = open mode. Only enforce when list is non-empty. Zero behavior change for existing deployments. |
| **Blob size limit enforcement** | Currently NO blob size check exists in the engine ingest path. The only limit is `MAX_FRAME_SIZE` (16 MiB) in the framing layer. For larger blobs (50-100 MiB), an explicit, configurable limit must exist in the engine, checked before expensive signature verification. | LOW | `engine::BlobEngine::ingest()`, `config::Config` | Add `max_blob_size` to Config (default 100 MiB). Check `blob.data.size()` as first step in ingest, before pubkey size check. Add `IngestError::too_large`. |
| **Frame size increase** | `MAX_FRAME_SIZE` is currently 16 MiB. A 100 MiB blob with overhead (2592B pubkey + 4627B sig + 32B namespace + 12B ttl/timestamp) produces a FlatBuffer ~100 MiB. After AEAD encryption + FlatBuffer transport wrapping, the frame will exceed 16 MiB. Must increase to at least 128 MiB. | LOW | `net::MAX_FRAME_SIZE` in `framing.h`, `Connection::recv_raw()` | Single constexpr change. But must be coordinated with memory allocation strategy. |
| **Blob transfer batching for sync** | `encode_blob_transfer()` currently packs ALL requested blobs into a single message. With 100 MiB blobs, a batch of even 2 blobs would be 200 MiB in a single allocation. Must send blobs one-at-a-time during sync for larger sizes. | MED | `sync::SyncProtocol::encode_blob_transfer()`, `PeerManager::run_sync_with_peer()`, `PeerManager::handle_sync_as_responder()` | Change sync protocol to send one BlobTransfer message per blob instead of batching all into one message. The receiver already has the hash list so it knows how many to expect. |

## Differentiators

Features that add real value beyond the minimum. Not expected but make the system notably better.

| Feature | Value Proposition | Complexity | Dependencies on Existing | Notes |
|---------|-------------------|------------|--------------------------|-------|
| **SIGHUP config reload for allowed_keys** | Hot-reload the pubkey whitelist without restarting the daemon. Standard Unix daemon convention. Allows adding/removing authorized peers without downtime. Nostr relays (strfry) support plugin reloading; production daemons (nginx, HAProxy, sshd) all support SIGHUP reload. | MED | `config::load_config()`, `PeerManager`, Asio signal handling | Register SIGHUP handler via `asio::signal_set`. On signal, re-read config file, update allowed_keys atomically. Optionally disconnect peers whose keys were removed. The `Server` already has `asio::signal_set signals_` for SIGINT/SIGTERM -- extend it. |
| **Disconnect revoked peers** | When allowed_keys changes via SIGHUP reload, actively disconnect peers whose pubkeys were removed from the list. Without this, revoked peers remain connected until they disconnect naturally. | MED | `PeerManager::peers_` (the connected peer list), SIGHUP reload | After reloading config, iterate `peers_`, compare each `connection->peer_pubkey()` against new allowed_keys. Send Goodbye to revoked peers. |
| **Per-peer write restriction** | In closed mode, only peers in allowed_keys can write blobs. But a variant: allow some peers to read-only (sync/query) while others can read+write. Useful for relay nodes that should mirror data but not inject new blobs. | MED | `engine::BlobEngine::ingest()`, allowed_keys config | Extend allowed_keys config to specify permissions per key: `{"key": "...", "write": true}` vs `{"key": "...", "write": false}`. Engine checks before ingest. Adds config complexity. |
| **Configurable max blob size via config** | Make `max_blob_size` a runtime config parameter rather than a compile-time constant. Different deployments have different needs (a relay-backing node might want 100 MiB, a lightweight mirror might want 10 MiB). | LOW | `config::Config`, `engine::BlobEngine` | Already planned in table stakes, but making it truly configurable (not just a new constexpr) is the differentiator. |
| **Memory-aware blob reception** | For 100 MiB blobs, `recv_raw()` allocates `std::vector<uint8_t>(len)` where `len` could be 100+ MiB. On a memory-constrained node, this could cause allocation failure or OOM. Check available memory or use a capped allocation before reading the frame body. | MED | `Connection::recv_raw()` | Check declared frame length against `max_blob_size + overhead` before allocating the receive buffer. Reject with close if too large. This prevents a malicious peer from forcing a 4 GiB allocation (the frame length is uint32_t, max ~4 GiB). Currently only `MAX_FRAME_SIZE` caps this. |

## Anti-Features

Features to explicitly NOT build for v2.0. Either YAGNI, wrong layer, or add too much complexity for the value.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| **Namespace-level ACLs** | The node already has namespace ownership via SHA3-256(pubkey). Adding a separate ACL per namespace is relay-layer logic. The database is intentionally dumb -- it stores blobs and verifies ownership. Namespace-level access decisions belong in Layer 2 (Relay). | Connection-level allowed_keys covers the actual use case: controlling WHO can talk to this node. |
| **Streaming/chunked blob transfer** | Chunking a blob across multiple frames adds enormous protocol complexity (reassembly, partial failure, out-of-order chunks, resume). For 50-100 MiB files, single-frame transfer with adequate frame size works fine. Streaming makes sense at 1+ GiB; that is not this milestone. libmdbx supports values up to 1 GiB via overflow pages. | Increase `MAX_FRAME_SIZE` to 128 MiB. Send one blob per sync transfer message. If future needs exceed this, chunking is a separate milestone. |
| **inotify-based config watching** | inotify fires on every file save, catching half-written config files. The standard Unix approach is explicit SIGHUP. Every production daemon uses SIGHUP, not inotify. | SIGHUP signal handler to reload config on demand. |
| **Certificate-based auth / TLS** | Adding TLS or X.509 certificates would duplicate the existing PQ-encrypted transport. The handshake already does KEM key exchange + ML-DSA-87 mutual auth. Adding TLS means OpenSSL (explicitly out of scope) and redundant crypto layers. | The existing handshake IS the auth layer. Use `peer_pubkey()` from the handshake result. |
| **Rate limiting per blob size** | Adds complexity for minimal gain in a closed node model. If you control who connects (allowed_keys), you trust those peers not to spam. Rate limiting is a hardening feature for open nodes -- not relevant for closed mode. | For v2.0, access control IS the rate limiting. If needed later, rate limiting is a separate hardening milestone. |
| **Encrypted-at-rest blobs** | The data field is opaque to the node. If the relay/app layer wants encrypted payloads, it encrypts before writing to the node. The node does not need to know. Adding encryption-at-rest adds key management complexity to the database layer. | Relay encrypts data before writing blobs. Node stores them as-is. |
| **Blob compression** | Compression before storage saves disk but adds CPU overhead on every ingest/retrieve. For a signed blob store, the data is opaque -- the node does not know if it is already compressed. Compressing encrypted data (from relay layer) yields zero savings. | Leave compression to the application layer. The node stores raw blobs. |
| **Dynamic peer permissions (CRDT-based ACL)** | p2panda and similar projects use CRDTs for collaborative ACL management. This is massively complex and only makes sense for fully decentralized group management. chromatindb's closed node model is operator-controlled -- the node operator decides who connects, period. | Static config file with SIGHUP reload. The operator manages the list. |

## Feature Dependencies

```
Allowed-keys config
    -> Connection-level auth gating (needs the list to check against)
    -> Open mode preservation (empty list = open)
    -> SIGHUP config reload (needs the config to reload)
        -> Disconnect revoked peers (needs SIGHUP + peer tracking)

Frame size increase
    -> Blob size limit enforcement (must be checked before frame allocation)
    -> Blob transfer batching (one-per-message instead of batch)

Config max_blob_size
    -> Blob size limit enforcement (engine uses the config value)
    -> Memory-aware blob reception (connection uses the config value)
```

## MVP Recommendation

**Priority 1 -- Access Control (core deliverable):**
1. Allowed-keys config -- add to Config struct, parse from JSON
2. Connection-level auth gating -- check peer_pubkey() after handshake, reject unauthorized
3. Open mode preservation -- empty list = no enforcement

**Priority 2 -- Larger Blob Support (core deliverable):**
4. Blob size limit enforcement -- add max_blob_size to Config, check in engine
5. Frame size increase -- bump MAX_FRAME_SIZE to 128 MiB
6. Blob transfer batching -- one blob per sync transfer message

**Priority 3 -- Polish (differentiators worth building):**
7. SIGHUP config reload -- hot-reload allowed_keys without restart
8. Disconnect revoked peers -- clean up connections after key revocation
9. Memory-aware blob reception -- validate frame length before allocating

**Defer:**
- Per-peer write restriction: adds config schema complexity, YAGNI for initial closed node model. Can be added later if relay nodes need read-only access.
- Everything in anti-features: wrong layer, wrong time, or wrong approach.

## Implementation Notes

### Access Control Integration Points

The existing code already has the perfect hook for connection-level auth gating. In `connection.cpp`, after `do_handshake()` succeeds and `peer_pubkey_` is populated, the `ready_cb_` fires. In `PeerManager::on_peer_connected()`, this is where the allowed_keys check belongs:

```
Connection::run()
  -> do_handshake() succeeds, peer_pubkey_ is set
  -> ready_cb_(self) fires
  -> PeerManager::on_peer_connected(conn)
     -> NEW: if allowed_keys non-empty && conn->peer_pubkey() not in allowed_keys
        -> conn->close_gracefully() (sends Goodbye)
        -> return (don't add to peers_)
```

No protocol changes. No new wire messages. No changes to the handshake. The ML-DSA-87 pubkey from the handshake IS the identity check.

### Larger Blob Size Considerations

**Current frame path:** Blob -> FlatBuffer encode -> TransportMessage wrap -> AEAD encrypt -> length-prefix frame

A 100 MiB blob produces approximately:
- BlobData.data: 100 MiB
- FlatBuffer overhead: ~7.3 KiB (pubkey 2592B + sig 4627B + namespace 32B + ttl 4B + timestamp 8B + FlatBuffer framing)
- TransportMessage wrap: minimal (type byte + payload vector)
- AEAD tag: 16 bytes
- Frame header: 4 bytes

Total frame size: ~100.007 MiB. Well within a 128 MiB MAX_FRAME_SIZE.

**libmdbx:** Supports values up to 1 GiB. Values larger than ~2 KiB use overflow pages (contiguous page sequences). A 100 MiB blob will occupy ~25,000 overflow pages (at 4 KiB page size). This is within libmdbx's design parameters but increases write amplification. The mmap geometry is already set to 64 GB upper bound.

**Memory pressure:** A single 100 MiB blob ingest path touches:
1. `recv_raw()`: allocates vector<uint8_t>(~100 MiB) for ciphertext
2. AEAD decrypt: allocates another ~100 MiB for plaintext
3. TransportCodec::decode: parses in-place (FlatBuffers zero-copy)
4. BlobData construction: copies data field (~100 MiB)
5. Signature verification: reads from BlobData
6. Storage: serializes to FlatBuffer again (~100 MiB) for storage

Peak memory for one blob ingest: ~400 MiB (4 copies in flight). This is acceptable for a server daemon but worth documenting. The key mitigation is: only one blob is in-flight per connection at a time (sequential protocol), and the sync protocol sends one blob per transfer message.

### Config Schema for Allowed Keys

```json
{
    "allowed_keys": [
        "abcdef1234...",
        "567890abcd..."
    ],
    "max_blob_size_mib": 100
}
```

Hex-encoded ML-DSA-87 public keys (5184 hex characters each). The config parser should validate key format (correct length, valid hex) at load time and reject the config if any key is malformed.

Alternative considered and rejected: using SHA3-256(pubkey) as the namespace ID instead of the raw pubkey. The pubkey is what comes out of the handshake, so comparing against the raw pubkey avoids an extra hash operation per connection. Also, the operator who adds keys to the config will get them from the identity file, which stores the raw pubkey.

## Complexity Assessment

| Feature | Lines of Code (est.) | Risk | Touches |
|---------|---------------------|------|---------|
| Allowed-keys config | ~30 | LOW | config.h, config.cpp |
| Connection-level auth gating | ~20 | LOW | peer_manager.cpp |
| Open mode preservation | ~5 | LOW | peer_manager.cpp (conditional) |
| Blob size limit enforcement | ~25 | LOW | engine.h, engine.cpp, config.h |
| Frame size increase | ~5 | LOW | framing.h |
| Blob transfer batching | ~60 | MED | sync_protocol.cpp, peer_manager.cpp |
| SIGHUP config reload | ~40 | MED | server.cpp or main.cpp, peer_manager.cpp |
| Disconnect revoked peers | ~25 | MED | peer_manager.cpp |
| Memory-aware reception | ~15 | LOW | connection.cpp |
| **Total (all features)** | **~225** | | |

This is a small, focused milestone. The codebase hooks are already in place. The main risk is in blob transfer batching, which changes the sync protocol flow.

## Sources

- [strfry Nostr relay](https://github.com/hoytech/strfry) -- write policy plugin architecture, pubkey whitelist patterns
- [strfry plugins documentation](https://github.com/hoytech/strfry/blob/master/docs/plugins.md) -- event-level vs connection-level policy
- [Nostr RS Relay (Start9)](https://docs.start9.com/0.3.5.x/service-guides/nostr/nostr-rs-relay.html) -- whitelist config requiring at least one pubkey
- [Quorum p2p auth fix](https://github.com/ConsenSys/quorum/pull/897/files) -- disconnect unauthorized peers after handshake
- [p2panda access control](https://p2panda.org/2025/07/28/access-control.html) -- CRDT-based ACL design (too complex for our use case)
- [libmdbx README](https://github.com/erthink/libmdbx/blob/master/README.md) -- overflow pages, max value 1 GiB, performance characteristics
- [SIGHUP convention](https://blog.devtrovert.com/p/sighup-signal-for-configuration-reloads) -- standard Unix daemon reload pattern
- [Azure Blob Storage performance](https://learn.microsoft.com/en-us/azure/storage/blobs/storage-performance-checklist) -- chunked transfer best practices
- Existing codebase: connection.cpp (handshake exposes peer_pubkey_), framing.h (MAX_FRAME_SIZE), engine.cpp (no blob size check), sync_protocol.cpp (batch encoding)
