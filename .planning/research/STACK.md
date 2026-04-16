# Stack Research: CLI Polish (v4.1.0)

**Domain:** CLI client enhancements -- contact groups, chunked large files, request pipelining, configurable node constants
**Researched:** 2026-04-15
**Confidence:** HIGH (no new dependencies, all patterns proven in existing codebase)

## Context

The CLI client (`cli/`) is a standalone C++20 binary connecting to a chromatindb node via UDS or TCP. It already has: SQLite contacts, envelope encryption (segmented AEAD), chunked transport framing (1 MiB sub-frames), synchronous blocking I/O over Asio, and `config.json` for host/port defaults.

This document covers stack additions/changes needed for four new features:
1. **Contact groups** -- SQLite schema extension
2. **Chunked large files** -- >500 MiB without full memory buffering
3. **Request pipelining** -- parallel downloads via concurrent I/O
4. **Configurable node constants** -- 10 hardcoded values move to config.json with SIGHUP reload

## Recommended Stack

### Core Technologies (No Changes)

The existing CLI stack requires ZERO new FetchContent or system dependencies.

| Technology | Version | Current Usage | New Usage |
|------------|---------|---------------|-----------|
| SQLite3 | 3.53.0 (system) | contacts table | +groups table, +group_members table |
| Standalone Asio | 1.38.0 | Synchronous blocking I/O | Async coroutine I/O for pipelining |
| libsodium | system | ChaCha20-Poly1305, HKDF | Incremental SHA3-256 for streaming hash |
| liboqs | 0.15.0 | ML-DSA-87, ML-KEM-1024, SHA3-256 | Same (signing chunks uses same path) |
| nlohmann/json | 3.11.3 | config.json parsing | +node constants in config.json |
| FlatBuffers | 25.2.10 | Wire format encode/decode | Same |
| spdlog | 1.15.1 | Logging | Same |

### No New Dependencies Required

Every feature can be implemented with the existing stack. This is the correct approach because:
- SQLite already handles contact data; groups are a schema addition, not a technology addition
- Asio already supports async/coroutine I/O; the CLI just needs to use it
- The envelope encryption already supports segmented AEAD (1 MiB segments); streaming just changes the I/O pattern
- nlohmann/json already parses config.json; adding fields is trivial

## Feature-Specific Stack Analysis

### 1. Contact Groups (SQLite Schema Extension)

**What's needed:** Two new tables in the existing `~/.chromatindb/contacts.db`.

**Schema design:**

```sql
CREATE TABLE IF NOT EXISTS groups (
    name TEXT PRIMARY KEY,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS group_members (
    group_name TEXT NOT NULL REFERENCES groups(name) ON DELETE CASCADE,
    contact_name TEXT NOT NULL REFERENCES contacts(name) ON DELETE CASCADE,
    PRIMARY KEY (group_name, contact_name)
);
```

**Why this design:**
- `ON DELETE CASCADE` on `group_members.group_name` ensures deleting a group removes all memberships
- `ON DELETE CASCADE` on `group_members.contact_name` ensures deleting a contact removes them from all groups
- SQLite3 3.53.0 fully supports foreign keys (available since 3.6.19). Must enable with `PRAGMA foreign_keys = ON` at connection open (SQLite disables FK enforcement by default per connection)
- Composite primary key `(group_name, contact_name)` prevents duplicate memberships
- No separate ID columns -- names are the natural keys, matching existing contacts table design

**Integration point:** `ContactDB` class gains group methods. The `--share @team` syntax resolves a group name to the list of member KEM pubkeys, which feeds into the existing `envelope::encrypt()` recipient list.

**SQLite API usage:** Same raw `sqlite3_*` C API already used in `contacts.cpp`. No ORM, no wrapper library. Keep it simple.

**Critical: PRAGMA foreign_keys = ON** must be added to `ContactDB::ContactDB()` constructor, before `init_schema()`. Without this, `ON DELETE CASCADE` silently does nothing.

**Confidence:** HIGH -- SQLite foreign keys are well-proven, schema is straightforward.

### 2. Chunked Large Files (>500 MiB)

**Problem:** Current flow reads entire file into memory (`read_file_bytes`), envelope-encrypts it (another copy), serializes to FlatBuffer (another copy). A 500 MiB file uses ~1.5 GB RAM.

**Solution: Client-side multi-blob chunking with manifest.** Split the file into independently-encrypted chunks, each stored as a separate blob. A manifest blob ties them together.

**Why NOT streaming single-blob encryption:**
- FlatBuffers requires the complete `data` field at serialization time. The node's `encode_blob()` needs the full payload to build the FlatBuffer
- The node's ingest pipeline computes `SHA3-256(namespace || data || ttl || timestamp)` over the COMPLETE blob data for signing. Streaming this would require the node to buffer the entire blob anyway
- The node enforces `MAX_BLOB_DATA_SIZE = 500 MiB` as a protocol invariant -- cannot send larger single blobs

**Chunk design:**

Each chunk is an independent envelope-encrypted blob with magic prefix `CPAR` (0x43504152) for identification:

```
Chunk blob data: [CPAR:4][chunk_index:4BE][total_chunks:4BE][manifest_hash:32][file_data_segment]
```

Manifest blob (also envelope-encrypted):

```
Manifest data: [CMAN:4][original_filename_len:2BE][original_filename][total_size:8BE][chunk_count:4BE][chunk_hash_0:32][chunk_hash_1:32]...
```

**Chunk size: 50 MiB.** Because:
- 50 MiB per chunk means a 2 GB file = 40 chunks = 40 blobs. Manageable
- Each chunk + envelope overhead + FlatBuffer fits well within `MAX_BLOB_DATA_SIZE` (500 MiB)
- Memory per chunk: ~150 MiB peak (read 50 MiB + encrypt ~50 MiB + FlatBuffer ~50 MiB). Acceptable
- Larger chunks (100 MiB) would reduce blob count but increase memory. 50 MiB is the sweet spot
- Smaller chunks (10 MiB) would create too many blobs for very large files (200 blobs for 2 GB)

**Streaming read pattern (no full-file buffering):**

```cpp
// Read file in 50 MiB chunks using ifstream
std::ifstream file(path, std::ios::binary);
std::vector<uint8_t> chunk_buf(CHUNK_SIZE);

while (file.read(reinterpret_cast<char*>(chunk_buf.data()), CHUNK_SIZE) || file.gcount() > 0) {
    size_t bytes_read = static_cast<size_t>(file.gcount());
    auto chunk_data = build_chunk_payload(chunk_index, total_chunks, manifest_hash_placeholder, chunk_buf, bytes_read);
    auto envelope = envelope::encrypt(chunk_data, recipients);
    // sign + serialize + send over connection
}
```

**Two-pass upload for manifest:**
1. First pass: upload all chunks, collect their blob hashes from WriteAck responses
2. Second pass: build manifest with chunk hashes, upload manifest blob
3. Return manifest hash as the file's "handle"

**Download:** Read manifest blob, extract chunk hashes, fetch chunks (pipelined -- see section 3), reassemble in order.

**SHA3-256 for canonical signing:** Each chunk is independently signed. The existing `build_signing_input()` computes `SHA3-256(namespace || data || ttl || timestamp)` per chunk. No streaming hash needed here because each chunk is small enough (50 MiB) to fit in memory.

**No changes to node code.** The node stores chunks as regular blobs. The manifest is a regular blob. All client-side logic.

**Confidence:** HIGH -- multi-blob chunking with manifest is a well-established pattern (similar to git packfiles, S3 multipart upload). No protocol changes required.

### 3. Request Pipelining (Parallel Downloads)

**Problem:** Current `get()` sends one ReadRequest, waits for ReadResponse, then sends the next. Sequential. Downloading 40 chunks of a large file takes 40 round trips.

**Solution: Send multiple ReadRequests using request_id correlation, receive responses out of order.**

**The node already supports this.** From PROTOCOL.md:
> "The node may process requests concurrently and responses may arrive in a different order than requests were sent. Clients must use request_id to correlate responses, not assume ordering."

**Implementation approach -- synchronous pipelining (no async rewrite needed):**

The current `Connection` class uses synchronous (blocking) Asio I/O. Full async/coroutine conversion is overkill for the CLI (it's a command-line tool, not a server). Instead, use a pipeline window:

```cpp
// Pipeline: send up to N requests before waiting for any response
static constexpr uint32_t PIPELINE_DEPTH = 8;

// Phase 1: Send up to PIPELINE_DEPTH requests
std::map<uint32_t, std::string> pending;  // request_id -> hash_hex
uint32_t rid = 1;
size_t send_idx = 0;

// Fill pipeline
while (send_idx < hash_hexes.size() && pending.size() < PIPELINE_DEPTH) {
    conn.send(MsgType::ReadRequest, payload, rid);
    pending[rid] = hash_hexes[send_idx];
    ++rid; ++send_idx;
}

// Phase 2: Receive responses and refill pipeline
while (!pending.empty()) {
    auto resp = conn.recv();
    auto it = pending.find(resp->request_id);
    // process response, remove from pending
    pending.erase(it);

    // Refill pipeline
    if (send_idx < hash_hexes.size()) {
        conn.send(MsgType::ReadRequest, payload, rid);
        pending[rid] = hash_hexes[send_idx];
        ++rid; ++send_idx;
    }
}
```

**Why synchronous pipelining, not async coroutines:**
- The CLI is a short-lived command-line tool. It connects, does work, disconnects
- Synchronous send/recv is simpler to reason about than `co_await` chains
- The bottleneck is network RTT, not CPU. Pipelining removes RTT serialization
- Pipeline depth of 8 means 8x throughput improvement for sequential downloads
- No need to change the `Connection` class from synchronous to async

**Why pipeline depth 8:**
- Too small (2-4): Still leaves network idle time between batches
- Too large (32+): Memory pressure from 32 concurrent large responses in flight
- 8 is standard for HTTP/2 default concurrent streams and a proven sweet spot
- For chunked file downloads (50 MiB chunks), 8 in-flight = 400 MiB buffered. Acceptable

**Request ID management:** The existing `Connection::send()` already accepts `request_id`. The existing `Connection::recv()` already returns `DecodedTransport` with `request_id`. No changes to the transport layer needed.

**Chunked transport interaction:** When a response uses chunked framing (blob >1 MiB), the receiver must consume the entire chunked sequence (header + data sub-frames + sentinel) before receiving the next response. The current `Connection::recv()` already handles this correctly -- it detects `0x01` header byte and reads all sub-frames. Pipelining works because the node serializes chunked responses per-request_id on the wire.

**Upload pipelining:** For chunked file uploads, send multiple chunks without waiting for each WriteAck. Same pipeline window pattern, but with `MsgType::Data` sends.

**Confidence:** HIGH -- the protocol explicitly supports request_id pipelining, the transport layer already handles it, and synchronous pipeline windows are a proven pattern.

### 4. Configurable Node Constants

**Problem:** 10 hardcoded `constexpr` values in `db/peer/` control sync/peer behavior (PEX interval, max peers per exchange, sync timeouts, keepalive intervals, etc.). Operators want to tune these without recompiling.

**Identified constants to make configurable:**

| Constant | Current Value | Location | Config Key |
|----------|--------------|----------|------------|
| `PEX_INTERVAL_SEC` | 300 | pex_manager.h | `pex_interval_seconds` |
| `MAX_PEERS_PER_EXCHANGE` | 8 | pex_manager.h | `max_peers_per_exchange` |
| `MAX_DISCOVERED_PER_ROUND` | 3 | pex_manager.h | `max_discovered_per_round` |
| `MAX_PERSISTED_PEERS` | 100 | pex_manager.h | `max_persisted_peers` |
| `MAX_PERSIST_FAILURES` | 3 | pex_manager.h | `max_persist_failures` |
| `KEEPALIVE_INTERVAL` | 30s | connection_manager.cpp | `keepalive_interval_seconds` |
| `KEEPALIVE_TIMEOUT` | 60s | connection_manager.cpp | `keepalive_timeout_seconds` |
| `MAX_HASHES_PER_REQUEST` | 64 | sync_orchestrator.h | `max_hashes_per_request` |
| `BLOB_TRANSFER_TIMEOUT` | 120s | sync_orchestrator.h | `blob_transfer_timeout_seconds` |
| `STRIKE_THRESHOLD` | 10 | connection_manager.h | `strike_threshold` |

**Stack impact: NONE.** The node already uses nlohmann/json for config parsing and has SIGHUP reload infrastructure. Adding 10 more fields to `Config` struct and `load_config()` is mechanical.

**Pattern (already established in config.h):**

```cpp
// In Config struct:
uint32_t pex_interval_seconds = 300;
uint32_t max_peers_per_exchange = 8;
// ... etc

// In load_config():
if (j.contains("pex_interval_seconds") && j["pex_interval_seconds"].is_number_unsigned())
    cfg.pex_interval_seconds = j["pex_interval_seconds"].get<uint32_t>();
```

**SIGHUP reload:** The existing reload infrastructure already covers `max_peers`, `allowed_client_keys`, `allowed_peer_keys`, `rate_limit_*`, `metrics_bind`. Adding these 10 values to the SIGHUP handler is straightforward. Each component (PexManager, ConnectionManager, SyncOrchestrator) needs an `update_config()` method that atomically swaps the tunable values.

**Validation:** Add bounds checks in `validate_config()`. Examples:
- `pex_interval_seconds`: min 10, max 3600 (prevent spamming or stalling)
- `keepalive_interval_seconds`: min 5, max 300
- `keepalive_timeout_seconds`: must be > `keepalive_interval_seconds`
- `strike_threshold`: min 1, max 1000
- `blob_transfer_timeout_seconds`: min 10, max 600

**Confidence:** HIGH -- extending existing config/reload pattern. No new libraries.

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| SQLite FK + cascade for groups | Separate groups.json file | SQLite already in use for contacts, FK cascade handles cleanup automatically, atomic transactions for group + member ops |
| 50 MiB client-side chunks + manifest | Streaming single-blob encryption | FlatBuffers requires complete data for serialization, node requires complete data for signing hash. Cannot stream a single blob |
| 50 MiB chunk size | 10 MiB chunks | Too many blobs for large files (200 for 2 GB). More blobs = more metadata overhead, more signing operations |
| 50 MiB chunk size | 100 MiB chunks | Higher peak memory per chunk (~300 MiB). 50 MiB keeps peak under 200 MiB |
| Synchronous pipeline window | Full async/coroutine rewrite | CLI is short-lived tool, async complexity not justified. Synchronous pipeline achieves same throughput gain |
| Pipeline depth 8 | Unbounded pipelining | Memory explosion with many large responses. 8 is proven sweet spot |
| nlohmann/json config (existing) | TOML config | Node already uses JSON config. Using same format for consistency. No new dependency |
| Config fields with SIGHUP | Environment variables | Inconsistent with existing node config pattern. Env vars cannot be reloaded without restart |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| mmap for file reading | Complicates error handling (SIGBUS on truncated files), no benefit for sequential reads | `std::ifstream` with fixed-size buffer reads |
| Separate database for groups | Unnecessary -- SQLite handles multiple tables in one file perfectly | Add tables to existing contacts.db |
| SQLite WAL mode | CLI is single-process, single-thread access. WAL adds complexity for concurrent access that does not exist | Default journal mode (sufficient) |
| Async Asio coroutines for CLI | CLI is a command-line tool, not a long-running server. Coroutine machinery adds complexity without benefit | Synchronous blocking I/O with pipeline window |
| Thread pool for parallel downloads | Threading adds synchronization complexity. Pipeline window on single connection achieves the same throughput | Single-connection synchronous pipeline |
| YAML/TOML for config | Different format from node's config.json. Would add a dependency | nlohmann/json (already used) |
| SQLite ORM (sqlite_orm, etc.) | Additional dependency for simple CRUD. Raw sqlite3 API is already proven in contacts.cpp | Raw sqlite3_* C API |
| Protocol changes for chunking | Node code is frozen. Chunking must be client-side only | CPAR/CMAN magic bytes in blob data |

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| SQLite3 3.53.0 | Foreign key constraints | FK support since 3.6.19 (2009). Must enable with `PRAGMA foreign_keys = ON` per connection |
| SQLite3 3.53.0 | `ON DELETE CASCADE` | Fully supported. Requires FK pragma enabled |
| Standalone Asio 1.38.0 | Synchronous pipeline pattern | `send()` + `recv()` blocking calls work fine for pipelining. No async needed |
| nlohmann/json 3.11.3 | Config extension | Adding fields to JSON parsing is trivial |
| liboqs 0.15.0 | Per-chunk signing | Each chunk is independently signed with ML-DSA-87. No change to signing API |
| libsodium (system) | Per-chunk envelope encryption | Each chunk is independently encrypted. Existing `envelope::encrypt()` works per chunk |

## Installation

```bash
# No new dependencies. Everything is already in cli/CMakeLists.txt.
# The only change is adding new .cpp files for group commands and chunked file logic.
```

## Integration Points

### Contact Groups -> Envelope Encryption
`--share @team` resolves group "team" -> member contacts -> KEM pubkeys -> `envelope::encrypt()` recipients.
No changes to envelope code. Groups are purely a ContactDB concern.

### Chunked Files -> Transport Layer
Each chunk goes through the existing `Connection::send(MsgType::Data, ...)` path.
If chunk envelope > 1 MiB, `Connection::send()` already uses chunked transport framing.
Double framing is fine: application-level chunking (50 MiB file chunks) is orthogonal to transport-level chunking (1 MiB wire frames).

### Pipelining -> Connection
`Connection` already supports `request_id` on send and returns it on recv.
Pipeline window is a higher-level pattern built ON TOP of the existing Connection API.
No changes to Connection needed.

### Node Config -> Existing SIGHUP Infrastructure
New config fields are added to the existing `Config` struct, parsed in `load_config()`, validated in `validate_config()`, and propagated via the existing SIGHUP reload path.

## Sources

- SQLite3 3.53.0 foreign key documentation: https://www.sqlite.org/foreignkeys.html -- verified FK pragma requirement (HIGH confidence)
- chromatindb PROTOCOL.md request_id section -- confirms out-of-order response support (HIGH confidence)
- Existing codebase: `cli/src/contacts.cpp` (SQLite patterns), `cli/src/connection.cpp` (transport), `cli/src/envelope.cpp` (segmented AEAD), `db/config/config.h` (config pattern) -- all verified via source inspection (HIGH confidence)

---
*Stack research for: CLI Polish (v4.1.0)*
*Researched: 2026-04-15*
