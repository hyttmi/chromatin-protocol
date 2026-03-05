# Phase 5: Peer System - Research

**Researched:** 2026-03-04
**Domain:** Peer discovery, hash-list diff sync, daemon integration (C++20/Asio)
**Confidence:** HIGH

## Summary

Phase 5 integrates all previous layers into a functioning peer-to-peer daemon. The existing codebase provides strong foundations: `Server` already handles bootstrap connections with reconnect, `Connection` provides PQ-encrypted message IO with callbacks, `BlobEngine` has the query primitives for sync (`get_blobs_since`, `list_namespaces`, `has_blob`), and `Storage` has `store_blob`/`has_blob` for dedup. The work divides naturally into three areas: (1) sync protocol messages and hash-list diff logic, (2) peer manager with connection limits and the sync timer, and (3) daemon CLI with `main()` integration.

The sync protocol follows a straightforward namespace-first approach: exchange namespace lists, then per-namespace exchange blob hash lists to identify missing blobs, then transfer the missing blobs. FlatBuffers schema extensions add new `TransportMsgType` values for sync messages. The daemon wires everything together with subcommand dispatch (`run`, `keygen`, `version`).

**Primary recommendation:** Build bottom-up: sync protocol messages first, then peer manager with sync orchestration, then daemon `main()` integration. Test sync logic with in-process peers sharing an `io_context`.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Subcommand structure: `chromatindb run` (daemon), `chromatindb keygen` (generate identity), `chromatindb version`
- Rich startup output: version, bind address, data dir, identity namespace hash, bootstrap peers listed, then connection/sync events as they happen
- `keygen` refuses if identity file already exists -- requires `--force` to overwrite
- Keypair stored inside data-dir: `{data-dir}/identity.key`
- Summary-per-sync-round at info level: "Synced with peer X: received N blobs, sent M blobs"
- Individual blob transfers logged at debug level only
- Both peer connect and disconnect events logged at info level
- Invalid blobs during sync logged at warn level
- Strike system: track validation failures per peer, disconnect after threshold
- Hard cap on max peer connections, configurable in JSON config (default 32)
- Reject inbound connections above cap, prioritize bootstrap peers
- Start daemon even if all bootstrap peers are unreachable -- accept inbound, retry bootstrap with exponential backoff
- Bootstrap-only discovery for v1 -- no peer list exchange
- Reconnect bootstrap peers only -- non-bootstrap peer disconnections are not retried
- Sync on connect (full sync after handshake) + periodic sync on configurable timer
- Either side can initiate a sync round
- Configurable sync interval: `sync_interval_seconds` in JSON config (default 60s)
- Per-namespace hash-list diff: exchange namespace list first, then per-namespace exchange blob hash lists

### Claude's Discretion
- Exact sync protocol message types and FlatBuffers schema additions
- Hash-list diff algorithm implementation details
- Strike system threshold and cooldown duration
- Max peers default value
- Sync batch sizes and flow control
- How to handle partial sync failures (namespace-level retry vs full restart)
- Expiry filtering implementation during sync (SYNC-03)

### Deferred Ideas (OUT OF SCOPE)
- Peer exchange (PEX/DISC-03)
- Resumable sync (SYNC-04)
- XXH3 fingerprints for sync negotiation (SYNC-05)
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| DISC-01 | Node connects to configured bootstrap nodes on startup | Server already does this via `connect_to_peer()` + `reconnect_loop()`. Need peer manager to enforce max_peers and track bootstrap vs non-bootstrap |
| DISC-02 | Node receives peer lists from bootstrap nodes and connects to discovered peers | For v1 bootstrap-only: simplified to "connects to configured peers only." No peer list exchange in v1 |
| SYNC-01 | Nodes exchange blob hash lists to identify missing blobs | New sync protocol: NamespaceList, HashList, BlobTransfer message types. Per-namespace hash comparison |
| SYNC-02 | Sync is bidirectional -- both nodes end up with union of data | Both sides run the same sync initiator logic. Either side can start a sync round |
| SYNC-03 | Sync skips expired blobs (don't replicate dead data) | Filter by checking blob TTL + timestamp against current time before including in hash lists and before accepting transferred blobs |
</phase_requirements>

## Standard Stack

### Core (already in project)
| Library | Version | Purpose | Status |
|---------|---------|---------|--------|
| Standalone Asio | 1.38.0 | Async networking, coroutines, timers | In use (Phase 4) |
| FlatBuffers | 25.2.10 | Wire format for sync messages | In use (Phase 1), schema extension needed |
| libmdbx | 0.13.11 | Blob storage | In use (Phase 2) |
| spdlog | 1.15.1 | Structured logging | In use (Phase 1) |
| nlohmann/json | 3.11.3 | Config parsing | In use (Phase 1) |
| liboqs | 0.15.0 | PQ crypto (ML-DSA-87, ML-KEM-1024) | In use (Phase 1) |
| libsodium | latest | AEAD (ChaCha20-Poly1305), HKDF | In use (Phase 1) |

### No New Dependencies
Phase 5 requires no new libraries. All sync, discovery, and daemon logic builds on existing infrastructure.

## Architecture Patterns

### Existing Project Structure
```
src/
├── config/     # JSON config + CLI parsing
├── crypto/     # RAII wrappers: hash, signing, kem, aead, kdf
├── engine/     # BlobEngine: ingest validation + queries
├── identity/   # NodeIdentity: keypair + namespace derivation
├── logging/    # spdlog initialization
├── net/        # Connection, Server, handshake, framing, protocol
├── storage/    # libmdbx Storage with blob/seq/expiry indexes
└── wire/       # FlatBuffers codec + BlobData struct
```

Phase 5 additions:
```
src/
├── sync/       # SyncProtocol class -- hash-list diff logic
├── peer/       # PeerManager -- connection tracking, limits, sync scheduling
└── main.cpp    # Daemon entry point with subcommand dispatch
```

### Pattern 1: Sync Protocol as Message Handler
**What:** A `SyncProtocol` class that operates on a single `Connection`, using `send_message`/`on_message` callbacks. It orchestrates the sync exchange: namespace list -> hash lists -> blob transfer.
**When to use:** When adding application-level protocol on top of the existing transport.
**How it fits:** Connection already has `on_message(callback)` and `send_message(type, payload)`. SyncProtocol registers as the handler and drives the state machine.

```cpp
class SyncProtocol {
public:
    SyncProtocol(Connection::Ptr conn, engine::BlobEngine& engine, storage::Storage& storage);

    // Initiate a sync round (caller side)
    asio::awaitable<SyncResult> initiate_sync();

    // Handle an incoming sync message (called from Connection's on_message)
    asio::awaitable<void> handle_message(wire::TransportMsgType type,
                                          std::vector<uint8_t> payload);
};
```

### Pattern 2: PeerManager Wraps Server
**What:** PeerManager owns Server and adds peer tracking, connection limits, sync scheduling, and the strike system. Server remains focused on TCP lifecycle.
**When to use:** To separate connection management from peer-level policies.

```cpp
class PeerManager {
public:
    PeerManager(const config::Config& config,
                identity::NodeIdentity& identity,
                engine::BlobEngine& engine,
                storage::Storage& storage,
                asio::io_context& ioc);

    void start();  // Starts Server + sync timer
    void stop();   // Stops Server + cancels timers
};
```

### Pattern 3: Coroutine-based Sync Timer
**What:** Use `asio::steady_timer` in a coroutine loop for periodic sync, matching the existing `reconnect_loop` pattern.

```cpp
asio::awaitable<void> sync_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(config_.sync_interval_seconds));
        co_await timer.async_wait(use_nothrow);
        if (stopping_) co_return;
        co_await sync_all_peers();
    }
}
```

### Anti-Patterns to Avoid
- **Thread-based sync:** Do NOT use std::thread for sync timers. Everything runs on the single-threaded `io_context` via coroutines (established project pattern).
- **Blocking the event loop:** Sync blob transfers must be async. Do NOT call blocking Storage operations from within coroutines without yielding. Storage operations are fast (libmdbx), but batch them reasonably.
- **Double-framing:** The existing transport already does length-prefix framing. Sync messages go through `send_message()` which handles encoding. Do NOT add additional framing on top.
- **Mixing raw and encrypted IO:** After handshake, ALL messages go through `send_encrypted`/`recv_encrypted` via `send_message`. Sync messages are no exception.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Message serialization | Custom binary parser for sync messages | FlatBuffers schema extensions | Deterministic encoding, schema evolution, already in project |
| Async timers | sleep loops or thread-based timers | `asio::steady_timer` in coroutine | Matches existing pattern, single-threaded model |
| Connection lifecycle | Custom TCP state machine | Extend existing `Connection` + `Server` | Already handles handshake, encrypt/decrypt, goodbye |
| Config new fields | Manual JSON parsing for new fields | Extend existing `Config` struct + `load_config` | Pattern established in Phase 1 |

## Common Pitfalls

### Pitfall 1: Sync deadlock with symmetric protocol
**What goes wrong:** Both sides initiate sync simultaneously, both waiting for the other's response.
**Why it happens:** "Either side can initiate" means both might start at the same time.
**How to avoid:** Use a simple tie-breaker: if a SyncRequest arrives while we're sending one, the peer with the lexicographically lower namespace_id wins (acts as initiator). The other side switches to responder mode. Alternatively, serialize sync rounds per-connection: one sync at a time, guarded by a flag.
**Warning signs:** Test with two nodes syncing on the same interval.

### Pitfall 2: Unbounded memory during hash list exchange
**What goes wrong:** A namespace with millions of blobs creates a hash list that doesn't fit in memory.
**Why it happens:** Sending all hashes at once.
**How to avoid:** Batch hash lists. Send hashes in pages (e.g., 1000 at a time), with a continuation flag. For v1 with reasonable data sizes, a single-batch approach is acceptable but include the max in the message format.
**Warning signs:** Growing memory usage during sync with large namespaces.

### Pitfall 3: Syncing expired blobs (SYNC-03 violation)
**What goes wrong:** Node sends blobs that have expired, peer stores them, they expire again on the peer -- wasted bandwidth.
**Why it happens:** Hash list includes all stored blobs without checking expiry.
**How to avoid:** When building hash lists for sync, check `timestamp + ttl > now` for non-permanent blobs. When receiving blobs during sync, verify expiry before ingesting (BlobEngine already validates signatures but doesn't check expiry -- add this check in sync path, NOT in BlobEngine since local writes shouldn't be expiry-checked).
**Warning signs:** Test that expired blobs don't appear in sync hash lists.

### Pitfall 4: Server connection tracking vs PeerManager duplication
**What goes wrong:** Server tracks connections in `connections_` vector, PeerManager also tracks peers -- state goes out of sync.
**Why it happens:** Two components managing the same underlying connections.
**How to avoid:** PeerManager wraps or replaces Server's connection tracking. Server should delegate lifecycle callbacks to PeerManager. Consider having PeerManager own the connection vector and pass callbacks to Server.
**Warning signs:** Connection count mismatches, leaked connections on disconnect.

### Pitfall 5: FlatBuffers schema backward compatibility
**What goes wrong:** Adding new enum values to TransportMsgType breaks existing encoded messages.
**Why it happens:** FlatBuffers uses the enum value as-is; adding new values between existing ones changes the mapping.
**How to avoid:** Append new TransportMsgType values at the end (after `Data = 8`). FlatBuffers enums are wire-stable when values are appended.
**Warning signs:** N/A (this is a one-time design decision).

### Pitfall 6: Coroutine lifetime with shared_ptr
**What goes wrong:** Connection destroyed while coroutine is suspended in sync.
**Why it happens:** Connection removed from connections_ while sync coroutine is `co_await`ing.
**How to avoid:** Capture `shared_from_this()` at the start of every coroutine that uses `this`. Connection already does this in `run()`. SyncProtocol should hold `Connection::Ptr`.
**Warning signs:** Use-after-free crashes in async sync operations.

## Code Examples

### Extending TransportMsgType FlatBuffers Enum
```fbs
enum TransportMsgType : byte {
    None = 0,
    KemPubkey = 1,
    KemCiphertext = 2,
    AuthSignature = 3,
    AuthPubkey = 4,
    Ping = 5,
    Pong = 6,
    Goodbye = 7,
    Data = 8,
    // Phase 5: Sync protocol
    SyncRequest = 9,
    SyncAccept = 10,
    NamespaceList = 11,
    HashList = 12,
    BlobRequest = 13,
    BlobData = 14,
    SyncComplete = 15
}
```

### Sync Protocol Flow
```
Initiator                    Responder
    |                             |
    |--- SyncRequest ----------->|
    |<-- SyncAccept -------------|
    |                             |
    |--- NamespaceList --------->|  (our namespaces + latest seq_nums)
    |<-- NamespaceList ----------|  (their namespaces + latest seq_nums)
    |                             |
    |  For each shared namespace:  |
    |--- HashList (ns, hashes) ->|  (hashes we have)
    |<-- HashList (ns, hashes) --|  (hashes they have)
    |                             |
    |  Diff: we need hashes they  |
    |  have but we don't          |
    |                             |
    |--- BlobRequest (hashes) -->|  (request blobs we're missing)
    |<-- BlobData (blobs) -------|  (receive blobs)
    |                             |
    |<-- BlobRequest (hashes) ---|  (they request blobs they're missing)
    |--- BlobData (blobs) ------>|  (send blobs)
    |                             |
    |--- SyncComplete ---------->|
    |<-- SyncComplete -----------|
```

### Extending Config for Phase 5
```cpp
struct Config {
    std::string bind_address = "0.0.0.0:4200";
    std::string storage_path = "./data/blobs";
    std::string data_dir = "./data";
    std::vector<std::string> bootstrap_peers;
    std::string log_level = "info";
    // Phase 5 additions:
    uint32_t max_peers = 32;
    uint32_t sync_interval_seconds = 60;
};
```

### Expiry Check for Sync (SYNC-03)
```cpp
bool is_blob_expired(const wire::BlobData& blob, uint64_t now) {
    if (blob.ttl == 0) return false;  // Permanent
    return (blob.timestamp + blob.ttl) <= now;
}
```

### Hash-List Diff
```cpp
// Given our hashes and their hashes, find what we're missing
std::vector<std::array<uint8_t, 32>> diff_hashes(
    const std::vector<std::array<uint8_t, 32>>& ours,
    const std::vector<std::array<uint8_t, 32>>& theirs) {
    // Use unordered_set for O(1) lookup
    std::unordered_set<std::string> our_set;
    for (const auto& h : ours) {
        our_set.insert(std::string(h.begin(), h.end()));
    }
    std::vector<std::array<uint8_t, 32>> missing;
    for (const auto& h : theirs) {
        if (our_set.find(std::string(h.begin(), h.end())) == our_set.end()) {
            missing.push_back(h);
        }
    }
    return missing;
}
```

### Subcommand CLI Dispatch
```cpp
int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Print usage
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "run") return cmd_run(argc, argv);
    if (cmd == "keygen") return cmd_keygen(argc, argv);
    if (cmd == "version") return cmd_version();
    // Unknown command
    return 1;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| DHT for peer discovery | Bootstrap-only | Project decision | Simpler, more reliable |
| Gossip protocol sync | Direct hash-list diff | Project decision | Predictable, no amplification |
| Thread-per-connection | Asio coroutines | Phase 4 | Single-threaded, no locking |

## Open Questions

1. **Hash list encoding in FlatBuffers**
   - What we know: Each hash is 32 bytes. A namespace with 1000 blobs = 32KB of hashes.
   - What's unclear: Whether to use a FlatBuffers vector of [ubyte] arrays or a single flat [ubyte] with implicit 32-byte chunks.
   - Recommendation: Use a flat `[ubyte]` vector with documented 32-byte alignment. Simpler wire format, no overhead per hash. Verify by checking length is divisible by 32.

2. **Concurrent sync sessions**
   - What we know: Each peer connection can independently sync.
   - What's unclear: Whether BlobEngine/Storage can handle concurrent reads from multiple sync sessions.
   - Recommendation: Storage is NOT thread-safe per its docs, but all coroutines run on the same `io_context` thread. Coroutine suspension points won't interleave Storage calls. This is safe as-is.

3. **Strike system thresholds**
   - What we know: Need to track invalid blobs per peer and disconnect after threshold.
   - What's unclear: Exact numbers.
   - Recommendation: 10 invalid blobs = disconnect, 5 minute cooldown before reconnect. These are reasonable defaults that can be tuned later.

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `src/net/server.h`, `src/net/connection.h`, `src/engine/engine.h`, `src/storage/storage.h` -- current interfaces
- FlatBuffers schema: `schemas/transport.fbs`, `schemas/blob.fbs` -- current wire format
- `CMakeLists.txt` -- dependency versions and build structure

### Secondary (HIGH confidence)
- Project CONTEXT.md -- locked design decisions for Phase 5
- Project REQUIREMENTS.md -- requirement definitions for DISC-01, DISC-02, SYNC-01, SYNC-02, SYNC-03
- Project STATE.md -- historical decisions and patterns from Phases 1-4

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, everything is already in the project
- Architecture: HIGH - sync protocol is well-understood, existing patterns provide clear extension points
- Pitfalls: HIGH - pitfalls derived from codebase analysis and real protocol design experience

**Research date:** 2026-03-04
**Valid until:** 2026-04-04 (30 days -- stable domain, no external API changes)
