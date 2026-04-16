# Architecture Patterns

**Domain:** CLI Polish features for chromatindb (contact groups, chunked large files, request pipelining, configurable node constants)
**Researched:** 2026-04-15

## Current Architecture Summary

Two independent binaries sharing no code at link time:

```
CLI (cli/)                          Node (db/)
  main.cpp                            main.cpp
  commands.cpp  -- put/get/ls/...     config/ -- JSON config + SIGHUP reload
  connection.cpp -- sync blocking I/O net/    -- async Asio, AEAD, chunked framing
  contacts.cpp  -- SQLite contact DB  peer/   -- PeerManager facade (6 components)
  envelope.cpp  -- CENV encryption    engine/ -- BlobEngine ingest/query
  identity.cpp  -- ML-DSA/ML-KEM keys storage/-- libmdbx ACID store
  wire.cpp      -- FlatBuffer codec   sync/   -- reconciliation protocol
```

CLI is synchronous, blocking, single-threaded. It opens one `Connection` per command invocation, sends request(s), receives response(s), exits. The Connection class handles both UDS (TrustedHello) and TCP (full PQ handshake) with AEAD-encrypted framing.

Node is async (Asio coroutines, single io_context thread + thread_pool for crypto offload). It is frozen MVP -- no code changes to db/ for this milestone except the configurable constants feature.

## Feature 1: Contact Groups

### What Changes

**Scope: CLI only. Zero node or protocol changes.**

Contact groups are a purely client-side convenience layer on top of the existing SQLite `contacts` table. When a user types `--share @team`, the CLI expands `@team` to all member KEM pubkeys and passes them as envelope recipients -- exactly what `--share alice --share bob` does today.

### Components Modified

| Component | Change | Scope |
|-----------|--------|-------|
| `contacts.h/cpp` (ContactDB) | Add `contact_groups` table + CRUD methods | New SQLite table, new methods |
| `commands.h/cpp` | Add `group create/list/add-member/rm-member/rm` subcommands | New command handlers |
| `commands.cpp::load_recipient_kem_pubkeys` | Recognize `@group_name` prefix, expand to member KEM pubkeys | Modify existing helper |
| `main.cpp` | Parse `group` subcommand + wire to command handlers | Extend arg parsing |

### New Components

None. All group logic lives in the existing `ContactDB` class and `commands.cpp`.

### Schema Design

```sql
CREATE TABLE IF NOT EXISTS contact_groups (
  group_name TEXT NOT NULL,
  contact_name TEXT NOT NULL REFERENCES contacts(name),
  PRIMARY KEY (group_name, contact_name)
);
```

A group is just a named set of existing contacts. No new crypto, no new storage, no new dependencies.

### Data Flow

```
put --share @team file.txt
  |
  main.cpp: parse --share @team
  |
  load_recipient_kem_pubkeys():
    detect '@' prefix -> ContactDB::get_group_members("team")
    for each member name -> ContactDB::get(name) -> kem_pk
    return vector<kem_pk>
  |
  envelope::encrypt(plaintext, all_kem_pks)  // unchanged
  |
  Connection::send(Data, flatbuf)            // unchanged
```

### Integration Points

- `load_recipient_kem_pubkeys()` in `commands.cpp` is the single expansion point. Today it handles file paths and contact name lookups. Add `@` prefix detection for groups.
- Groups reference contacts by name. If a contact is removed, its group memberships become dangling. Either: (a) CASCADE DELETE in SQLite, or (b) skip missing members at expand time with a warning. Recommendation: CASCADE DELETE -- clean, no silent surprises.

### Dependencies

None. Can be built first or in parallel with anything else. No impact on other features.

---

## Feature 2: Chunked Large File Support (>500 MiB)

### What Changes

**Scope: CLI envelope encryption + new CLI-level chunking protocol. Node changes: none. MAX_BLOB_DATA_SIZE stays at 500 MiB.**

The current `put` command reads the entire file into memory, encrypts the entire payload as one envelope, serializes it as one FlatBuffer blob, and sends it over the wire. This works up to ~500 MiB but fails for larger files because:
1. `read_file_bytes()` loads the entire file into a `std::vector<uint8_t>`
2. `envelope::encrypt()` allocates plaintext + all segment ciphertexts + stanza data
3. `encode_blob()` copies all data into a FlatBuffer
4. Peak memory is approximately 3x file size

### Architecture Decision: Application-Layer Chunking

The node's MAX_BLOB_DATA_SIZE (500 MiB) is a protocol invariant. Rather than changing the node's blob size limit (which would cascade into sync, replication, storage, and every peer in the network), large files are split into multiple blobs at the application layer.

This is the approach described in BACKLOG.md: "Split large files into chunks, each independently envelope-encrypted."

### New Component: ChunkedFile

A manifest blob + N chunk blobs, all stored as normal blobs in the user's namespace.

```
Manifest blob (CPAR magic):
  [0x43,0x50,0x41,0x52]  -- "CPAR" magic (4 bytes)
  [version:1]              -- 0x01
  [chunk_count:4BE]        -- number of chunks
  [original_size:8BE]      -- original file size
  [chunk_size:4BE]         -- size of each chunk (last may be smaller)
  [metadata_len:4BE]       -- length of JSON metadata
  [metadata_json]          -- {"name": "file.ext", "size": N}
  For each chunk:
    [chunk_hash:32]        -- SHA3-256 blob hash of chunk blob (from WriteAck)

Each chunk blob (CDAT magic):
  [0x43,0x44,0x41,0x54]  -- "CDAT" magic (4 bytes)
  [sequence:4BE]           -- chunk index (0-based)
  [data]                   -- raw chunk data
```

Both manifest and chunk blobs are envelope-encrypted with the same recipient list. Each chunk is independently encrypted and independently stored as a normal blob.

### Components Modified

| Component | Change | Scope |
|-----------|--------|-------|
| `commands.cpp::put` | Detect file size > threshold, split into chunks, upload chunks first, then manifest | Major restructure of put() |
| `commands.cpp::get` | Detect CPAR magic in decrypted data, fetch all chunk blobs, reassemble | Major restructure of get() |
| `commands.cpp::rm` | Detect CPAR magic, tombstone manifest AND all chunk blobs | Moderate extension |
| `envelope.h/cpp` | No change -- each chunk is encrypted independently | Unchanged |
| `connection.h/cpp` | No change -- each chunk is sent as a normal blob via existing chunked transport | Unchanged |
| `wire.h` | Add CPAR_MAGIC/CDAT_MAGIC constants, is_chunked_manifest() helper | Small additions |

### New Components

| Component | Purpose |
|-----------|---------|
| `chunked_file.h/cpp` | Split file into chunks, build manifest, reassemble from manifest + chunks. Pure logic, no I/O. |

### Data Flow: Upload

```
put large_file.bin --share @team
  |
  Detect: file_size > CHUNK_THRESHOLD (e.g., 50 MiB per chunk)
  |
  Open file, read chunk 0 (50 MiB)
  |
  build CDAT payload: [magic][seq:0][chunk_data]
  envelope::encrypt(cdat_payload, recipients) -> chunk_envelope_0
  Build blob, sign, send Data, recv WriteAck -> chunk_hash_0
  |
  Repeat for chunks 1..N-1  (sequential, one at a time)
  |
  Build CPAR manifest with all chunk_hashes
  envelope::encrypt(manifest, recipients) -> manifest_envelope
  Build blob, sign, send Data, recv WriteAck -> manifest_hash
  |
  Print manifest_hash (this is what the user shares)
```

### Data Flow: Download

```
get <manifest_hash>
  |
  ReadRequest(manifest_hash) -> ReadResponse -> decrypt -> detect CPAR magic
  |
  Parse manifest: chunk_count, chunk_hashes[], metadata
  |
  For each chunk_hash (sequential or pipelined -- see Feature 3):
    ReadRequest(chunk_hash) -> ReadResponse -> decrypt -> strip CDAT header -> raw data
  |
  Reassemble chunks in order -> write to output file
```

### Memory Efficiency

With application-layer chunking, peak memory per chunk is ~3x chunk_size. With 50 MiB chunks, peak memory is ~150 MiB regardless of file size. The existing 1 MiB segment encryption in `envelope.cpp` handles each 50 MiB chunk incrementally.

For truly streaming encryption (never buffering more than 1 MiB), the envelope format would need restructuring -- but that is a much larger change. The BACKLOG.md notes this is "blocked by FlatBuffer requiring full blob data for serialization." The chunk-based approach sidesteps this: each chunk is small enough to buffer.

### Chunk Size Recommendation

**50 MiB per chunk.** Rationale:
- Small enough that 3x buffering = ~150 MiB peak memory per chunk
- Large enough that a 5 GB file = 100 chunks (manageable manifest, not too many blobs)
- Well under the 500 MiB node limit with room for envelope overhead (~1648 bytes per recipient stanza + AEAD tags)
- The BACKLOG.md suggests "10-50 MiB" -- use the upper end to minimize chunk count

### Integration Points

- `put` and `get` in `commands.cpp` are the primary integration points. The chunking logic wraps around the existing single-blob upload/download path.
- Chunk blobs are normal blobs in the user's namespace. They replicate, expire, and get deleted like any other blob. TTL of chunks must match the manifest's TTL.
- Deleting a chunked file means tombstoning the manifest AND all chunks. The `rm` command needs to understand CPAR manifests.

### Dependencies

- Best if contact groups are done first (so `--share @team` works with chunked uploads)
- Pipelining (Feature 3) would significantly speed up chunked downloads but is not required -- sequential download works

---

## Feature 3: Request Pipelining (Parallel Downloads)

### What Changes

**Scope: CLI only. Zero node changes.**

The node already supports concurrent request dispatch (v1.3.0 Phase 62). Multiple requests with different `request_id` values can be in flight simultaneously. The CLI currently sends one request, waits for one response, repeats. Pipelining sends multiple ReadRequests before reading any responses.

### Architecture Decision: Send-Then-Receive Pipeline

The simplest effective approach: send all ReadRequests first (each with a unique `request_id`), then receive all responses. The node processes them concurrently via its thread pool and sends responses as they complete.

This is NOT full async multiplexing (which would require an event loop in the CLI). It is batch pipelining: fire N requests, then collect N responses. The responses may arrive out of order (matched by `request_id`).

### Components Modified

| Component | Change | Scope |
|-----------|--------|-------|
| `connection.h/cpp` | No structural change needed -- existing send/recv work for pipelining | Unchanged |
| `commands.cpp::get` | Restructure from sequential send-recv to pipelined send-all-then-recv-all | Moderate restructure |
| `commands.cpp::get` (chunked) | After parsing CPAR manifest, pipeline all chunk ReadRequests | Integration with Feature 2 |

### New Components

None needed. The existing `Connection::send()` and `Connection::recv()` work fine -- just call send() N times, then recv() N times.

### Data Flow: Pipelined Get

```
get hash1 hash2 hash3 hash4
  |
  Connection::send(ReadRequest, [ns|hash1], rid=1)
  Connection::send(ReadRequest, [ns|hash2], rid=2)
  Connection::send(ReadRequest, [ns|hash3], rid=3)
  Connection::send(ReadRequest, [ns|hash4], rid=4)
  |
  // Node processes concurrently, sends responses as they complete
  |
  resp_a = Connection::recv()  // might be rid=3
  resp_b = Connection::recv()  // might be rid=1
  resp_c = Connection::recv()  // might be rid=4
  resp_d = Connection::recv()  // might be rid=2
  |
  Match responses to original requests by request_id
  Process/write each file
```

### Critical Constraint: AEAD Nonce Ordering

The AEAD transport uses a monotonic counter-based nonce. Send and receive counters are independent. Sending multiple requests increments `send_counter_` for each. Receiving multiple responses increments `recv_counter_` for each. This is fine -- the counters are per-direction and the CLI controls the send order. The node's per-connection send queue with drain coroutine (documented in Key Decisions) ensures responses are sent in a deterministic order per connection.

**Important:** The node's send queue serializes outbound messages. Responses are sent in the order they are enqueued (which is the order they complete processing). The CLI must recv() them in the same order they arrive on the wire -- it cannot skip ahead. This means: if the CLI sends requests 1,2,3 and the node enqueues responses in order 2,1,3 (because request 2 finished first), the CLI must recv() in order 2,1,3 and match by request_id.

The existing `Connection::recv()` already reads the next message off the wire in order. No change needed to recv(). The caller just needs a map from request_id to the original request context.

### Pipeline Depth

Cap at a reasonable limit (e.g., 16 concurrent requests) to avoid overwhelming the node's send queue or socket buffer. For chunked file downloads with 100 chunks, pipeline in batches of 16.

### Integration Points

- `get` command is the primary integration point
- Chunked file download (Feature 2) is the biggest beneficiary -- fetching 100 chunks sequentially is slow, pipelined is fast
- The `--all` flag already fetches multiple blobs -- it would benefit immediately

### Dependencies

- Can be built standalone but maximum value comes from combining with chunked files (Feature 2)
- Contact groups (Feature 1) are independent

---

## Feature 4: Configurable Node Constants

### What Changes

**Scope: Node (db/) only. CLI unaffected.**

10 hardcoded `static constexpr` values in the peer subsystem need to move to `Config` and become SIGHUP-reloadable.

### Constants to Extract

| Constant | Current Location | Current Value | Config Key |
|----------|-----------------|---------------|------------|
| `PEX_INTERVAL_SEC` | `pex_manager.h:23`, `peer_manager.h:84` | 300 | `pex_interval_seconds` |
| `MAX_PEERS_PER_EXCHANGE` | `pex_manager.h:24`, `peer_manager.h:85` | 8 | `max_peers_per_exchange` |
| `MAX_DISCOVERED_PER_ROUND` | `pex_manager.h:25`, `peer_manager.h:86` | 3 | `max_discovered_per_round` |
| `MAX_PERSISTED_PEERS` | `pex_manager.h:26`, `peer_manager.h:87` | 100 | `max_persisted_peers` |
| `MAX_PERSIST_FAILURES` | `pex_manager.h:27`, `peer_manager.h:88` | 3 | `max_persist_failures` |
| `STRIKE_THRESHOLD` | `connection_manager.h:80`, `peer_manager.h:80` | 10 | `strike_threshold` |
| `STRIKE_COOLDOWN_SEC` | `peer_manager.h:81` | 300 | `strike_cooldown_seconds` |
| `KEEPALIVE_INTERVAL` | `connection_manager.cpp:393` | 30s | `keepalive_interval_seconds` |
| `KEEPALIVE_TIMEOUT` | `connection_manager.cpp:394` | 60s | `keepalive_timeout_seconds` |
| `CURSOR_GRACE_PERIOD_MS` | `connection_manager.h:71` | 300000 | `cursor_grace_period_seconds` |

### Components Modified

| Component | Change | Scope |
|-----------|--------|-------|
| `config/config.h` | Add 10 new fields with current defaults | Struct fields |
| `config/config.cpp` | Parse 10 new JSON keys, validate ranges | JSON parsing + validation |
| `peer/pex_manager.h/cpp` | Replace 5 `static constexpr` with config refs | Read from config |
| `peer/connection_manager.h/cpp` | Replace 3 `static constexpr` with config refs | Read from config |
| `peer/peer_manager.h/cpp` | Remove duplicate constexpr, delegate to sub-components | Cleanup |
| `peer/peer_manager.cpp::reload_config()` | Propagate new values to sub-components on SIGHUP | Extend existing reload |

### Architecture Pattern: Config Reference Injection

The sub-components (`PexManager`, `ConnectionManager`, `SyncOrchestrator`) already receive a `const Config&` at construction. The pattern is:
1. Add fields to `Config` struct with current values as defaults
2. Replace `static constexpr` with reads from `config_.field_name`
3. In `reload_config()`, re-read the config file (existing mechanism) and the new values take effect

For timer-based values (PEX_INTERVAL, KEEPALIVE_INTERVAL), the new value takes effect at the next timer expiry -- no need to cancel/restart timers on SIGHUP.

### Validation Rules

| Field | Min | Max | Notes |
|-------|-----|-----|-------|
| `pex_interval_seconds` | 10 | 3600 | Too low = spam, too high = slow discovery |
| `max_peers_per_exchange` | 1 | 32 | Bounded by wire format |
| `max_discovered_per_round` | 1 | 16 | Bounds connection storms |
| `max_persisted_peers` | 10 | 10000 | Bounds peers.json size |
| `max_persist_failures` | 1 | 100 | Prune threshold |
| `strike_threshold` | 3 | 100 | Too low = aggressive disconnects |
| `strike_cooldown_seconds` | 60 | 3600 | Recovery window |
| `keepalive_interval_seconds` | 10 | 600 | Must be < keepalive_timeout |
| `keepalive_timeout_seconds` | 30 | 1200 | Must be > keepalive_interval |
| `cursor_grace_period_seconds` | 60 | 3600 | Cursor compaction grace |

### Integration Points

- Existing `validate_config()` in `config.cpp` already accumulates errors and throws -- add new validations there
- Existing SIGHUP handler in `PeerManager::reload_config()` already re-reads config.json and applies changes -- extend it
- Tests that reference the constexpr values need updating to read from config or use known defaults

### Dependencies

Completely independent of CLI features. Can be built in parallel with everything else.

---

## Suggested Build Order

```
Phase 1: Contact Groups (CLI-only, pure SQLite, zero risk)
  |       + Configurable Node Constants (node-only, parallel track)
  |
Phase 2: Chunked Large Files (CLI, depends on groups for --share @team convenience)
  |
Phase 3: Request Pipelining (CLI, highest value when combined with chunked downloads)
```

### Rationale

1. **Contact Groups first** because it is the simplest feature, unblocks `--share @team` for all subsequent testing, and has zero risk to existing functionality. Building it first means all later features can use groups in their test workflows.

2. **Configurable Node Constants in parallel with groups** because it is node-only, has no dependency on CLI features, and is a straightforward extraction of existing constants into config. It is also the only feature that touches the frozen node codebase, so doing it early and in isolation reduces risk.

3. **Chunked Large Files second** because it is the most complex feature and benefits from having groups available for testing multi-recipient chunked uploads. It introduces new CLI components (`chunked_file.h/cpp`) and restructures the `put`/`get` commands significantly.

4. **Request Pipelining last** because its primary value is accelerating chunked file downloads. Building it after chunked files means it can be tested against real multi-chunk downloads. It also has the most subtle correctness constraint (AEAD nonce ordering, request_id matching) and benefits from a stable foundation.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Modifying the Node's MAX_BLOB_DATA_SIZE for Large Files
**What:** Increasing MAX_BLOB_DATA_SIZE to allow single blobs > 500 MiB.
**Why bad:** Cascades into sync protocol (one-blob-at-a-time memory bound), replication (all peers must handle the new size), storage (libmdbx page allocation), and every existing deployment. Breaks protocol compatibility.
**Instead:** Application-layer chunking. The node stays at 500 MiB, the CLI splits large files into multiple blobs.

### Anti-Pattern 2: Full Async I/O in CLI for Pipelining
**What:** Converting the CLI from synchronous blocking I/O to an async event loop with io_context::run().
**Why bad:** Massive complexity increase for a command-line tool. The CLI runs one command and exits. An event loop is overkill.
**Instead:** Batch pipelining -- send N requests synchronously, receive N responses synchronously. The socket buffer handles the overlap.

### Anti-Pattern 3: Streaming Envelope Encryption
**What:** Restructuring envelope.cpp to encrypt data in a streaming fashion without buffering the full chunk.
**Why bad:** The FlatBuffer blob serialization requires the full data field. The envelope format has stanza AD that covers the entire ciphertext region. Streaming would require a new envelope format version and FlatBuffer schema change.
**Instead:** Use chunk sizes small enough (50 MiB) that 3x buffering is acceptable (~150 MiB peak). True streaming is a future optimization if needed.

### Anti-Pattern 4: Separate Group Database File
**What:** Creating a new SQLite database file for groups.
**Why bad:** Adds a second file to manage, backup, and migrate. Groups reference contacts by name -- they belong in the same database.
**Instead:** Add a `contact_groups` table to the existing `contacts.db`.

## Scalability Considerations

| Concern | 10 files | 1000 files | 10 GB file |
|---------|----------|------------|------------|
| Contact groups | Instant lookup | Instant lookup | N/A |
| Chunked upload | Single blob, no chunking | Single blob each | 200 chunks at 50 MiB, ~150 MiB peak RAM |
| Pipelined download | 1 request (no pipeline) | 63 batches of 16 | 200 chunks in 13 batches of 16 |
| Node constants | No impact | No impact | No impact |

## Sources

- Codebase analysis: `cli/src/` (all source files), `db/config/config.h`, `db/peer/peer_manager.h`, `db/peer/pex_manager.h`, `db/peer/connection_manager.h`, `db/net/framing.h`
- `cli/BACKLOG.md` -- feature descriptions and known constraints
- `.planning/PROJECT.md` -- milestone context and requirements
- MEMORY.md -- architecture decisions, key lessons, locked decisions
