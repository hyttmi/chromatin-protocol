# Architecture Research: v2.0 Integration

**Domain:** Access control and larger blob support for existing chromatindb node
**Researched:** 2026-03-05
**Confidence:** HIGH (modifications to existing, well-understood codebase; no new external dependencies)

## Scope

This document analyzes how two v2.0 features integrate with the existing v1.0 architecture:

1. **Access control** (allowed_keys config, closed node model)
2. **Larger blob support** (bump from 15 MiB to ~100 MiB)

Plus the prerequisite **source restructure** (move to /db, namespace rename).

This is NOT a greenfield architecture. Every component already exists and works. The question is: where do new features attach, what existing code changes, and what is the correct build order.

## Current Architecture (v1.0 Baseline)

```
Inbound TCP ──► Server.accept_loop()
                    │
                    ▼
              Connection.run()
                    │
            ┌───────┴────────┐
            │  do_handshake() │   ML-KEM key exchange + ML-DSA mutual auth
            │  (4-step)       │   peer_pubkey_ set on success
            └───────┬────────┘
                    │ on_ready callback
                    ▼
            PeerManager.on_peer_connected()
                    │
                    ▼
              message_loop() ──► on_peer_message()
                                      │
                         ┌────────────┼────────────┐
                         ▼            ▼            ▼
                    SyncRequest    Data        PeerListRequest
                         │            │            │
                         ▼            ▼            ▼
                   SyncProtocol  BlobEngine   PEX exchange
                                  .ingest()
                                      │
                              ┌───────┴────────┐
                              │ 1. structural  │
                              │ 2. namespace   │
                              │ 3. signature   │
                              │ 4. storage     │
                              └────────────────┘
```

Key observation: **peer_pubkey_ is available immediately after handshake**, before `on_ready` fires and before `on_peer_connected` runs. This is the natural insertion point for access control.

## Feature 1: Access Control (Closed Node Model)

### Design Decision: Where Does the Pubkey Check Go?

Three options analyzed:

| Option | Location | When | Pros | Cons |
|--------|----------|------|------|------|
| A | Connection.run(), after handshake | Before on_ready fires | Earliest possible, no messages exchanged with unauthorized peers | Connection needs access to allowed_keys |
| B | PeerManager.on_peer_connected() | After on_ready, before message routing | Clean separation, PeerManager already has config | Brief window where unauthorized peer is "connected" |
| C | New AccessPolicy middleware | Between handshake and PeerManager | Extensible, testable in isolation | Over-engineered for a simple set lookup |

**Recommendation: Option A -- check in Connection.run(), right after do_handshake() succeeds.**

Rationale:
- The handshake already proves peer identity (mutual ML-DSA-87 auth). The peer_pubkey_ is set.
- Checking immediately means zero protocol messages are exchanged with unauthorized peers.
- The connection simply closes with a log message. No new wire protocol needed.
- The peer never reaches on_ready, never gets added to peers_, never triggers sync.
- No middleware needed. A simple callback or config reference is sufficient.

### Integration Design

```
Connection.run():
    hs_ok = co_await do_handshake();
    if (!hs_ok) { close; return false; }

    // NEW: Access control check
    if (access_policy_ && !access_policy_(peer_pubkey_)) {
        spdlog::warn("rejected peer: pubkey not in allowed_keys");
        close();
        if (close_cb_) close_cb_(self, false);
        co_return false;
    }

    if (ready_cb_) ready_cb_(self);
    co_await message_loop();
```

### New/Modified Components

| Component | Status | Changes |
|-----------|--------|---------|
| `config::Config` | MODIFY | Add `std::vector<std::string> allowed_keys` (hex-encoded pubkeys), add `bool closed_node = false` |
| `config::load_config` | MODIFY | Parse `allowed_keys` array and `closed_node` bool from JSON |
| `net::Connection` | MODIFY | Add `AccessPolicy` callback type and setter, check after handshake in `run()` |
| `net::Server` | MODIFY | Accept `AccessPolicy` from PeerManager, pass to each Connection on creation |
| `peer::PeerManager` | MODIFY | Build AccessPolicy lambda from config, pass to Server |
| `peer::AccessControl` | NEW (optional) | Encapsulates allowed_keys set, provides `is_allowed(pubkey) -> bool`. Could be a simple class or just inline in PeerManager. |

### Config Format

```json
{
  "bind_address": "0.0.0.0:4200",
  "data_dir": "./data",
  "closed_node": true,
  "allowed_keys": [
    "aabbccdd...hex-encoded-ml-dsa-87-pubkey...",
    "11223344...another-pubkey..."
  ],
  "bootstrap_peers": ["10.0.0.1:4200"]
}
```

### Access Control Semantics

| Mode | `closed_node` | `allowed_keys` | Behavior |
|------|---------------|----------------|----------|
| Open (v1.0 default) | `false` | empty | Accept all authenticated peers |
| Whitelist | `false` | non-empty | Accept listed + accept unlisted (warning log) |
| Closed | `true` | non-empty | ONLY accept listed pubkeys, reject all others |
| Invalid | `true` | empty | Startup error: closed_node requires allowed_keys |

This two-field design avoids a single "mode" enum and makes the config self-documenting. The `closed_node` flag is the hard gate; `allowed_keys` alone is advisory/logging.

**Simplification option:** If YAGNI applies, skip the "whitelist" mode entirely. Just have `allowed_keys`: if non-empty, enforce it (reject unlisted). If empty, accept all. One field, one behavior. The `closed_node` bool becomes unnecessary.

**Recommendation: Single field.** `allowed_keys` non-empty = closed node. Empty = open. No mode enum, no `closed_node` bool. If you later need an advisory mode, add it then.

### Data Flow: Access Check

```
TCP connect
    │
    ▼
Connection created (inbound or outbound)
    │
    ▼
do_handshake() ──► ML-KEM + ML-DSA mutual auth
    │                peer_pubkey_ now set
    ▼
access_policy_(peer_pubkey_)?
    │
    ├── YES ──► on_ready ──► PeerManager.on_peer_connected ──► normal flow
    │
    └── NO ──► log warning ──► close() ──► close_cb_ ──► removed from Server.connections_
```

### Impact on Existing Features

| Feature | Impact | Notes |
|---------|--------|-------|
| Bootstrap reconnect | None | Reconnect loop in Server continues. If bootstrap peer is not in allowed_keys, connections will keep failing (expected). Operator must ensure bootstrap peers are in allowed_keys. |
| PEX | Low | PEX discovers addresses, not pubkeys. A discovered peer connects, does handshake, gets rejected if not allowed. No protocol change needed. Closed nodes should probably not share peers via PEX (config-gated). |
| Sync | None | Sync only runs with connected peers. Rejected peers never connect. |
| Strike system | None | Strikes only apply to connected peers. |
| Peer persistence | Low | Persisted peers may include non-allowed peers. On reconnect, they get rejected at handshake. Harmless but wasteful. Consider not persisting peers that were rejected. |

### PEX in Closed Mode

In closed mode, PEX is likely undesirable -- you do not want your closed node advertising its peer list to outsiders (they are all authorized peers). Two options:

1. **Disable PEX entirely in closed mode.** Simplest. If all peers are in allowed_keys, you already know them.
2. **Allow PEX but only share/accept allowed peers.** More complex, marginal benefit.

**Recommendation: Disable PEX in closed mode.** In `PeerManager::start()`, skip `pex_timer_loop()` if allowed_keys is non-empty. Still respond to PeerListRequest from authorized peers (just return empty list or skip). This is a 2-line change.

## Feature 2: Larger Blob Support (~100 MiB)

### Layer-by-Layer Impact Analysis

#### Transport Layer: Framing

**Current state:**
- `MAX_FRAME_SIZE = 16 * 1024 * 1024` (16 MiB) in `framing.h`
- Frame header: 4-byte big-endian uint32_t length prefix
- Maximum representable: ~4 GiB (uint32_t max)
- Encrypted frames: plaintext -> AEAD encrypt -> [4B length][ciphertext + 16B tag]

**Change needed:**
- Bump `MAX_FRAME_SIZE` to `128 * 1024 * 1024` (128 MiB) or `108 * 1024 * 1024` (108 MiB)
- The 4-byte uint32_t header is sufficient (max ~4 GiB). No header format change needed.
- AEAD (ChaCha20-Poly1305) has no practical message size limit (2^38 bytes per message with a given nonce).

**Sizing rationale:**
- 100 MiB blob data + ML-DSA-87 pubkey (2,592 B) + signature (4,627 B) + FlatBuffer overhead (~100 B) = ~100 MiB blob on wire
- TransportMessage envelope adds FlatBuffer framing around the blob bytes = ~100 MiB payload
- AEAD tag adds 16 bytes
- Frame limit should be >= 100 MiB blob + protocol overhead. 128 MiB provides comfortable headroom.
- During sync, BlobTransfer messages can contain MULTIPLE blobs. A batch of large blobs could exceed 128 MiB. **Solution: send one blob per BlobTransfer message for large blobs, or cap batch encoding at MAX_FRAME_SIZE.** See sync protocol section below.

**Risk: Memory pressure.** A 100 MiB blob means:
1. Receive 100+ MiB into memory (recv_raw allocates `std::vector<uint8_t>(len)`)
2. AEAD decrypt produces another 100 MiB buffer
3. FlatBuffer decode may produce another copy
4. BlobEngine.ingest encodes to FlatBuffer (another copy)
5. Storage writes to libmdbx

Peak memory per connection per large blob: ~400-500 MiB. With 32 peers potentially syncing large blobs simultaneously, this is problematic.

**Mitigation strategy (for v2.0 scope):**
- Accept the memory cost for now. 100 MiB blobs are the exception, not the rule. Most blobs remain small.
- Do NOT implement streaming/chunking in v2.0. That is a major protocol change.
- Consider adding a `max_blob_size` config parameter so operators can tune it per-node.
- The existing `MAX_FRAME_SIZE` check in `recv_raw()` already protects against unbounded allocation.

#### Connection Layer

**Current state:**
- `recv_raw()` reads 4-byte header, validates against MAX_FRAME_SIZE, allocates vector, reads payload
- `send_encrypted()` encrypts plaintext in one shot, calls `send_raw()` which writes header + ciphertext

**Change needed:**
- Update `MAX_FRAME_SIZE` constant -- that is the only change. Both `recv_raw()` and `send_raw()` already handle variable-size payloads correctly.
- No streaming needed. `asio::async_read` with a buffer of the declared size works fine for 100 MiB.
- TCP handles fragmentation transparently. Asio's `async_read` reads until the buffer is full.

#### Wire Format: FlatBuffers

**Current state:**
- `Blob.data` field is `[ubyte]` (variable-length byte vector). No size limit in the schema.
- `TransportMessage.payload` is `[ubyte]`. No size limit.

**Change needed:**
- None in the schema itself. FlatBuffers handles large byte vectors fine (uses 32-bit offset internally, max ~2 GiB per buffer).
- `encode_blob()` and `decode_blob()` work with arbitrary sizes.

#### BlobEngine: Ingest Pipeline

**Current state:**
- 4-stage pipeline: structural -> namespace -> signature -> storage
- No size check anywhere in the pipeline

**Change needed:**
- Add a size limit check as **Step 0** (before structural checks, cheapest possible rejection):

```cpp
// Step 0: Size limit
constexpr uint32_t MAX_BLOB_DATA_SIZE = 100 * 1024 * 1024;  // 100 MiB
if (blob.data.size() > MAX_BLOB_DATA_SIZE) {
    return IngestResult::rejection(IngestError::too_large, "blob data exceeds max size");
}
```

- Add `IngestError::too_large` to the enum.
- Make this configurable via Config if desired, or keep as constexpr (consistent with TTL being constexpr).

**Recommendation: constexpr.** Consistent with the TTL decision. Protocol invariant, not user-configurable. If you allow different nodes to have different max blob sizes, sync breaks (node A stores a 100 MiB blob, syncs to node B which has a 50 MiB limit -- B rejects it).

#### Storage: libmdbx

**Current state:**
- `geometry.size_upper = 64 GiB` -- plenty of room
- Values stored as `mdbx::slice(encoded.data(), encoded.size())` -- no size limit enforced by the API
- libmdbx max value size: ~2 GiB (INT32_MAX). 100 MiB is well within limits.
- Default page size: 4 KiB. Large values span multiple overflow pages automatically.

**Change needed:**
- None. libmdbx handles 100 MiB values natively via overflow pages.
- Performance consideration: writing a 100 MiB value creates ~25,000 overflow pages in a single transaction. This is fine for occasional large blobs but would be slow if the majority of writes are 100 MiB.
- Read performance for large values is excellent (memory-mapped, sequential read).

**Potential issue: write transaction size.** The sync protocol ingests blobs one at a time (each `store_blob()` call opens its own write transaction). This is correct -- a 100 MiB write in its own transaction is fine. If we batched multiple large blobs in one transaction, we could hit issues with transaction size vs. free page list. Current design avoids this.

#### Sync Protocol

**Current state:**
- `encode_blob_transfer()`: batches ALL requested blobs into one payload
  - Wire format: `[count:u32BE][len1:u32BE][blob1_flatbuf]...[lenN:u32BE][blobN_flatbuf]`
- If peer requests 5 blobs of 100 MiB each, the transfer message is ~500 MiB
- This would exceed MAX_FRAME_SIZE even at 128 MiB

**Change needed -- this is the most significant v2.0 integration concern:**

Option A: **Cap batch size in BlobTransfer messages.** When encoding a BlobTransfer, stop adding blobs when cumulative size approaches MAX_FRAME_SIZE. Send multiple BlobTransfer messages for one BlobRequest. Receiver processes them incrementally.

Option B: **One BlobTransfer per BlobRequest.** Instead of batching, request one blob at a time for large blobs. Simpler but more round trips.

Option C: **Size-aware batching.** Small blobs batch as today. Large blobs (> some threshold like 10 MiB) get individual transfers.

**Recommendation: Option A (capped batching).** Minimal protocol change -- both sides already handle multiple BlobTransfer messages in the sync loop. The change is in `PeerManager`'s sync orchestration: instead of sending all requested hashes in one BlobRequest, split into groups that fit within MAX_FRAME_SIZE.

Concretely:
1. When responding to a BlobRequest, fetch blobs by hash and accumulate into a BlobTransfer message.
2. When the accumulated size exceeds a threshold (e.g., 80% of MAX_FRAME_SIZE = ~100 MiB), send the current batch and start a new one.
3. The requesting side already loops on `recv_sync_msg` -- it naturally handles multiple BlobTransfer messages.
4. The requesting side needs to track how many BlobTransfer messages to expect. **Solution: include a count or "more" flag in BlobTransfer, OR have the responder send a final empty BlobTransfer as sentinel, OR track by hash count.**

Simplest approach: The responder sends N BlobTransfer messages (one per hash group), then nothing. The requester knows how many hashes it requested and counts received blobs until all are accounted for or timeout. This works because the existing protocol already tracks `pending_responses` per BlobRequest.

**Wait -- re-reading the sync code more carefully:** The current protocol sends ONE BlobTransfer per BlobRequest. The `pending_responses` counter decrements per BlobTransfer received. If we split one BlobRequest's response into multiple BlobTransfer messages, `pending_responses` logic breaks.

**Better approach:** Split the BlobRequest itself. Instead of one BlobRequest with 50 hashes (potentially 50 x 100 MiB), send multiple smaller BlobRequests. Each gets one BlobTransfer response. The `pending_responses` counter increments per BlobRequest sent.

This requires changes to sync orchestration in PeerManager but NOT to the wire protocol or message types. The hash list already supports any number of hashes per BlobRequest message.

**Updated recommendation:**

In the BlobRequest encoding step (Phase C1 of sync):
1. Instead of `encode_hash_list(ns, ALL missing hashes)`, batch hashes into groups where estimated total blob size fits in MAX_FRAME_SIZE.
2. Without knowing blob sizes in advance, batch by count: e.g., max 10 hashes per BlobRequest, or estimate size from hash-list metadata.
3. Simplest: just limit to 1 hash per BlobRequest for "potentially large" blobs. Since we do not know sizes, apply a reasonable batch limit universally (e.g., 50 hashes per request, which at 15 MiB old limit = 750 MiB, but at 100 MiB new limit could be 5 GiB).

**Final recommendation: Add a MAX_HASHES_PER_REQUEST constant (e.g., 10). Split BlobRequest messages when the hash list exceeds this count. Each BlobRequest gets one BlobTransfer response. pending_responses counts correctly.** This is backward-compatible (the other side does not care how many BlobRequests you send) and bounds memory usage on the responder side.

### Configuration Changes

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `allowed_keys` | `string[]` | `[]` | Hex-encoded ML-DSA-87 pubkeys. Non-empty = closed mode. |
| `max_blob_size` | removed | N/A | Not configurable -- protocol invariant (constexpr 100 MiB) |

## Source Restructure

### Current Layout

```
src/
  crypto/      hash, signing, kem, aead, kdf
  wire/        codec, blob_generated.h, transport_generated.h
  config/      config
  logging/     logging
  identity/    identity
  storage/     storage
  engine/      engine
  net/         connection, server, framing, handshake, protocol
  sync/        sync_protocol
  peer/        peer_manager
  main.cpp
tests/
  crypto/      test_hash, test_signing, ...
  wire/        test_codec
  config/      test_config
  ...
schemas/
  blob.fbs, transport.fbs
CMakeLists.txt
```

### Target Layout (from milestone context: "move to /db")

```
db/
  src/
    crypto/      (unchanged)
    wire/        (unchanged)
    config/      (unchanged)
    logging/     (unchanged)
    identity/    (unchanged)
    storage/     (unchanged)
    engine/      (unchanged)
    net/         (unchanged)
    sync/        (unchanged)
    peer/        (unchanged)
    main.cpp
  tests/
    ...
  schemas/
    blob.fbs, transport.fbs
  CMakeLists.txt
```

### Namespace Rename: `chromatin::` to `chromatindb::`

**Scope of change:**
- Every `.h` and `.cpp` file uses `namespace chromatin::*`
- FlatBuffers schemas use `namespace chromatin.wire;`
- All includes use `"crypto/hash.h"` etc. (relative, no namespace prefix) -- unchanged
- Test files reference `chromatin::*` types

**Estimated file count:** All 53 source files + 18 test files + 2 schema files = ~73 files.

**Approach:** Mechanical find-and-replace:
1. `chromatin::` -> `chromatindb::` in all .h/.cpp files
2. `namespace chromatin` -> `namespace chromatindb` in all .h/.cpp files
3. `chromatin.wire` -> `chromatindb.wire` in .fbs files
4. Regenerate FlatBuffer headers
5. Rebuild and run tests

**Risk:** Zero functional risk. Pure rename. But it touches every file, so it must be the FIRST phase to avoid merge conflicts with feature work.

## Build Order

The three work streams have clear dependencies:

```
Phase 1: Source Restructure (move to /db + namespace rename)
    │      No functional changes. Mechanical. Must go first to avoid
    │      conflicts with subsequent feature work.
    │
    ▼
Phase 2: Access Control (allowed_keys, closed node)
    │      Modifies: Config, Connection, Server, PeerManager
    │      New: AccessPolicy callback pattern
    │      Independent of blob size changes.
    │
    ▼
Phase 3: Larger Blob Support
           Modifies: framing.h (MAX_FRAME_SIZE), engine.h (MAX_BLOB_DATA_SIZE, IngestError::too_large)
           Modifies: PeerManager sync (batched BlobRequests)
           Independent of access control changes.
```

Phases 2 and 3 are functionally independent and could theoretically be done in parallel, but serializing them avoids merge conflicts in shared files (PeerManager, Config). Phase 2 goes first because it is higher-value (the primary v2.0 goal) and lower-risk.

### Phase 1: Source Restructure

**Work items:**
1. Create `db/` directory, move `src/`, `tests/`, `schemas/`, `CMakeLists.txt`
2. Update CMakeLists.txt paths
3. Find-and-replace namespace `chromatin` -> `chromatindb`
4. Regenerate FlatBuffer headers
5. Build and run all tests
6. Update `src/main.cpp` version string

**Files touched:** All ~73+ files (mechanical)
**Risk:** Low (no logic changes)
**Estimated size:** Small (automated rename + path changes)

### Phase 2: Access Control

**Work items:**
1. Add `allowed_keys` field to `Config` struct, parse from JSON
2. Add `AccessPolicy` callback type to Connection (or simple `std::function<bool(span<const uint8_t>)>`)
3. Add access check in `Connection::run()` after handshake
4. Wire PeerManager to build AccessPolicy from config and pass through Server to connections
5. Disable PEX timer when allowed_keys is non-empty
6. Tests: config parsing, access rejection, closed-mode PEX behavior

**Files modified:**
- `config/config.h` + `.cpp` (add field, parse)
- `net/connection.h` + `.cpp` (add callback, add check in run())
- `net/server.h` + `.cpp` (propagate policy to connections)
- `peer/peer_manager.h` + `.cpp` (build policy, gate PEX)
- Tests: `test_config.cpp`, `test_connection.cpp`, new `test_access_control.cpp` or extend `test_peer_manager.cpp`

**Risk:** Low-medium. The handshake + access check path must be correct to not break open-mode operation. Good test coverage needed for both open and closed modes.

### Phase 3: Larger Blob Support

**Work items:**
1. Bump `MAX_FRAME_SIZE` to `128 * 1024 * 1024` in `framing.h`
2. Add `MAX_BLOB_DATA_SIZE = 100 * 1024 * 1024` constexpr and `IngestError::too_large` in engine
3. Add size check as Step 0 in `BlobEngine::ingest()`
4. Add `MAX_HASHES_PER_REQUEST` constant for sync batching
5. Modify sync BlobRequest encoding to split large hash lists
6. Tests: oversized blob rejection, large blob ingest, sync with large blobs, batched BlobRequest

**Files modified:**
- `net/framing.h` (bump constant)
- `engine/engine.h` + `.cpp` (add size check, new error)
- `peer/peer_manager.cpp` (split BlobRequest batching in sync Phase C)
- `sync/sync_protocol.h` (add MAX_HASHES_PER_REQUEST constant)
- Tests: `test_engine.cpp`, `test_framing.cpp`, `test_peer_manager.cpp`

**Risk:** Medium. The sync batching change is the most subtle -- must ensure pending_responses tracking remains correct when splitting BlobRequests. Integration testing with actual large blobs over real connections is important.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Middleware/Interceptor Chain
**What:** Building a generic middleware pipeline for access control.
**Why bad:** Over-engineered for a single check. Adds indirection and complexity.
**Instead:** A simple callback/lambda on Connection. One check, one place.

### Anti-Pattern 2: Streaming/Chunking Protocol for Large Blobs
**What:** Adding a new chunked transfer protocol for blobs > N MiB.
**Why bad:** Major protocol change. Both sides need to agree. Breaks simplicity of "blob is one message."
**Instead:** Raise the frame limit and batch BlobRequests more conservatively. The blobs are still "medium" (100 MiB, not 10 GiB). Memory is cheap. Streaming can wait for a future version if needed.

### Anti-Pattern 3: Making max_blob_size Configurable per Node
**What:** Letting each node set its own blob size limit.
**Why bad:** Sync breaks. Node A stores a 100 MiB blob, syncs to Node B which rejects it because its limit is 50 MiB. Data loss in the network.
**Instead:** Protocol invariant (constexpr), like TTL. All nodes agree on the same limit.

### Anti-Pattern 4: Access Control in PeerManager Instead of Connection
**What:** Checking allowed_keys in on_peer_connected instead of after handshake.
**Why bad:** The peer is briefly "connected" -- gets a PeerInfo entry, may trigger sync-on-connect before the check runs. Race condition window.
**Instead:** Check in Connection.run() before on_ready fires. Rejected peer never enters PeerManager state.

## Component Interaction Summary

### Access Control Data Flow

```
Config JSON ──► Config.allowed_keys (vector<string>)
                    │
                    ▼
PeerManager constructor ──► build AccessPolicy lambda
                                │ convert hex strings to binary pubkeys
                                │ store in unordered_set for O(1) lookup
                                ▼
Server.set_access_policy(policy)
    │
    │ accept_loop / connect_to_peer / reconnect_loop:
    │ conn->set_access_policy(access_policy_)
    │
    ▼
Connection.run()
    │ do_handshake() ──► peer_pubkey_ set
    │ access_policy_(peer_pubkey_) ──► bool
    │   YES ──► on_ready ──► normal flow
    │   NO  ──► close ──► gone
```

### Larger Blob Data Flow

```
Inbound blob (wire) ──► recv_encrypted() ──► TransportCodec::decode()
    │                       ▲
    │                   MAX_FRAME_SIZE = 128 MiB (was 16 MiB)
    │
    ▼
BlobEngine.ingest()
    │
    ├── Step 0: size check (NEW) ──► reject if data > 100 MiB
    ├── Step 1: structural
    ├── Step 2: namespace
    ├── Step 3: signature
    └── Step 4: storage ──► libmdbx (handles 100 MiB values natively)

Sync outbound: BlobRequest now batched
    │
    ├── collect missing hashes
    ├── split into groups of MAX_HASHES_PER_REQUEST
    ├── send multiple BlobRequest messages
    ├── pending_responses++ per BlobRequest sent
    └── receive BlobTransfer, pending_responses-- per BlobTransfer received
```

## Testing Strategy

### Access Control Tests

| Test | What It Verifies |
|------|-----------------|
| Config parses allowed_keys from JSON | Config parsing |
| Config with empty allowed_keys = open mode | Open mode default |
| Connection rejects unlisted pubkey when allowed_keys set | Core access control |
| Connection accepts listed pubkey when allowed_keys set | Positive case |
| Connection accepts all when allowed_keys empty | Open mode behavior |
| PEX disabled when allowed_keys non-empty | Closed mode PEX gating |
| Bootstrap reconnect to non-allowed peer fails gracefully | Error handling |

### Larger Blob Tests

| Test | What It Verifies |
|------|-----------------|
| BlobEngine rejects blob with data > MAX_BLOB_DATA_SIZE | Size limit enforcement |
| BlobEngine accepts blob at exactly MAX_BLOB_DATA_SIZE | Boundary condition |
| Storage round-trips a 100 MiB blob | libmdbx large value handling |
| Framing round-trips a 100 MiB encrypted frame | Transport layer |
| Sync batches BlobRequests when hash count > MAX_HASHES_PER_REQUEST | Sync batching |
| Sync correctly tracks pending_responses with multiple BlobRequests | Counter correctness |

## Sources

- [libmdbx GitHub - max value size ~2 GiB (INT32_MAX)](https://github.com/erthink/libmdbx)
- [libmdbx documentation - settings and limits](https://libmdbx.dqdkfa.ru/group__c__settings.html)
- Existing codebase analysis (all source files read directly)
- v1.0 retrospective (patterns established, lessons learned)
