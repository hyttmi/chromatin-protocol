# Pitfalls Research

**Domain:** CLI polish features for chromatindb -- contact groups, chunked large files (>500 MiB), request pipelining, configurable node constants
**Researched:** 2026-04-15
**Confidence:** HIGH (verified against existing codebase envelope.cpp, connection.cpp, contacts.cpp, framing.h, config.h, peer_manager.h; streaming AEAD pitfalls verified via ImperialViolet, Google Tink docs, libsodium docs; AEAD nonce desync verified via hpke-js CVE GHSA-73g8-5h73-26h4 and existing node PEX SIGSEGV post-mortem; SQLite migration pitfalls verified via SQLite forum and ALTER TABLE docs)

## Critical Pitfalls

### Pitfall 1: Streaming envelope encryption -- truncation attack from missing last-segment marker

**What goes wrong:**
The current envelope format (CENV v1) has no explicit last-segment marker. Segment count is implicit from `ciphertext_region = envelope_size - 20 - (N * 1648)`. For files stored as a single blob on the node, this is fine -- the node stores the complete envelope, and the envelope size is known at read time. But chunked large files (>500 MiB) will be split into multiple blobs, each containing a sub-range of the file. If an attacker can delete the final chunk blob (via tombstone), the recipient reassembles all-but-last chunks and gets a truncated file that decrypts successfully. Each segment authenticates independently with its nonce-derived index, so missing the final segment produces valid plaintext for segments 0..K-1 with no authentication error.

This is the classic "segment truncation attack" documented by Adam Langley (ImperialViolet 2014) and mitigated in every serious streaming encryption library (Tink, age, libsodium secretstream). The current single-blob envelope sidesteps it because the blob size is tamper-evident. Multi-blob chunking breaks that assumption.

**Why it happens:**
The original envelope was designed for single blobs where the node enforces data integrity. Extending to multi-blob chunked files changes the trust model: the envelope must now be self-authenticating against truncation even when the transport cannot guarantee all chunks arrive.

**How to avoid:**
Mark the final segment. Two options:
1. **Sentinel in nonce:** Set a bit in the final segment's nonce (e.g., high bit of the XOR'd segment index). This is how age does it. Requires envelope format version bump to v2.
2. **Segment count in header:** Add a 4-byte segment count to the CENV header. Decryptor knows expected count upfront and rejects if fewer arrive. Simpler, no nonce space reduction.

Option 2 is better for this codebase because: (a) the nonce space is already tight (12 bytes with 4 bytes for segment index leaves 8 bytes of randomness, already below the 96-bit ideal), (b) adding a header field is a clean protocol change, and (c) it lets the decryptor pre-validate expected chunk count before any crypto.

Regardless of approach, the multi-blob chunk manifest (CPAR magic from BACKLOG.md) must include the total segment count so the CLI can reject incomplete reassemblies before attempting decryption.

**Warning signs:**
- Decryption of partial chunk sets succeeds without error
- No explicit validation of "did I get all chunks" before returning plaintext
- Tests that only verify with all chunks present, never test missing-last-chunk

**Phase to address:**
Chunked large file phase. Must be designed before implementing multi-blob chunking. The envelope format decision (v2 header with segment count) gates the implementation.

---

### Pitfall 2: Request pipelining -- AEAD nonce desync on the CLI side

**What goes wrong:**
The CLI uses synchronous blocking I/O with a single `send_counter_` / `recv_counter_` pair (connection.h:74-75). Currently, each command does one send, one recv, sequentially. Request pipelining means sending N ReadRequests before receiving any responses. This itself is safe for the send side (sends are still sequential). But the recv side is where it breaks: if the node responds out-of-order (it dispatches requests concurrently on its thread pool), and the CLI processes responses in the wrong order, the AEAD counter desynchronizes.

Critically: the node's server-side connection uses `request_id` for correlation but the AEAD layer is strictly sequential -- each encrypted frame increments the counter regardless of which request it responds to. The responses arrive in counter order on the wire even if the node processed them out-of-order internally, because the node's drain_send_queue coroutine serializes all outbound writes. So the nonce ordering is guaranteed by the node's architecture.

The real danger is on the CLI side: if the pipelining implementation uses multiple threads to read responses (for parallel downloads to disk), two threads could call `recv_encrypted()` concurrently, each reading partial frame data and corrupting the recv_counter_ state. The existing single-threaded blocking CLI does not have this problem, but pipelining tempts developers into multithreading.

**Why it happens:**
The existing codebase had this exact bug before: the v1.0.0 PEX SIGSEGV was caused by AEAD nonce desync from concurrent SyncRejected writes on the node side. The fix was the drain_send_queue coroutine (serializing all writes). The CLI does not have this pattern because it was single-threaded. Pipelining introduces the same class of bug.

**How to avoid:**
Keep the CLI single-threaded for network I/O. Pipeline by:
1. Send all N ReadRequests sequentially (incrementing send_counter_ normally)
2. Receive all N responses sequentially in a single reader thread (incrementing recv_counter_ normally)
3. Dispatch decryption and disk I/O to worker threads after the response bytes are received and decrypted

The parallelism is in the post-receive processing (envelope decrypt, file write), not in the network I/O. This matches the node's architecture: single reader coroutine, dispatches to thread pool for heavy work.

Do NOT attempt concurrent recv() calls. Do NOT use multiple connections to the same node for pipelining -- each connection has separate AEAD state, but the node's max_clients limit and the handshake overhead make this wasteful.

**Warning signs:**
- `recv_encrypted()` called from multiple threads
- `std::thread` or `std::async` wrapping `recv()` calls
- `recv_counter_` accessed without synchronization
- Tests that pass with 1 pipelined request but fail intermittently with 10+

**Phase to address:**
Request pipelining phase. The single-reader-thread architecture must be established before any parallel download logic is added. Must be documented as an invariant.

---

### Pitfall 3: Chunked transport framing ambiguity -- 0x01 flag byte collision with FlatBuffer data

**What goes wrong:**
The chunked transport protocol uses a first-byte sentinel: if `frame[0] == 0x01`, the frame is interpreted as a chunked header (`[0x01][type:1][request_id:4BE][total_size:8BE]`). But FlatBuffer-encoded TransportMessages can also start with 0x01 in their first byte (it is a valid FlatBuffer root table offset). Currently, the disambiguation works because a regular TransportMessage decoded via FlatBuffer never has a 14-byte frame that starts with 0x01 followed by a valid type byte at offset 1. But this is fragile -- it depends on FlatBuffer's internal encoding details.

When extending chunked framing for larger files (>500 MiB), the total_size field (8 bytes at offset 6) will hold values exceeding the current MAX_BLOB_DATA_SIZE of 500 MiB. If the receiver validates `total_size <= MAX_BLOB_DATA_SIZE` before assembling, legitimate large-file transfers will be rejected. If it does not validate, a malicious sender could claim a multi-terabyte total_size and exhaust memory during reassembly.

**Why it happens:**
The 0x01 sentinel was designed when chunked framing was only for payloads between 1 MiB and 500 MiB. Extending to >500 MiB requires updating the node's MAX_BLOB_DATA_SIZE or separating the "chunked transport limit" from the "blob storage limit." These are currently the same constant (500 MiB), but for chunked files they must diverge: the node stores individual chunk blobs within MAX_BLOB_DATA_SIZE, while the transport carries chunk manifests that reference multiple blobs.

**How to avoid:**
1. **Do not change the chunked transport framing for this milestone.** Each chunk blob is an independent envelope-encrypted blob under 500 MiB. The chunked transport framing carries individual chunk blobs, not the entire file. The total_size in the chunked header refers to a single chunk blob's transport size, not the file size.
2. **Add explicit size validation in the chunked reassembler.** `total_size` must be checked against a sane maximum (e.g., MAX_BLOB_DATA_SIZE + envelope overhead) before allocating the reassembly buffer. The existing `recv()` in connection.cpp:626 does `payload.reserve(static_cast<size_t>(total_size))` with NO validation -- fix this.
3. **Document the framing invariant:** chunked transport carries individual messages (one blob per chunked sequence), never an entire multi-blob file.

**Warning signs:**
- `payload.reserve(total_size)` without bounds checking -- current code has this bug
- Attempting to send an entire multi-gigabyte file as a single chunked transport sequence
- Tests that use small payloads and never exercise the reassembly buffer with adversarial total_size

**Phase to address:**
Chunked large file phase. The size validation fix should be addressed immediately as a pre-existing bug. The architectural decision (chunk blobs are independent, not one giant stream) must be established before implementation.

---

### Pitfall 4: SQLite schema migration -- adding groups table without versioning corrupts existing contacts

**What goes wrong:**
The current `contacts.cpp` calls `init_schema()` with a bare `CREATE TABLE IF NOT EXISTS contacts (...)`. Adding a `groups` table and a `group_members` junction table requires schema migration. The pitfalls:

1. **No schema version tracking.** If a future change needs to ALTER the contacts table (e.g., adding a `created_at` column), there is no way to know which version of the schema the database has. `CREATE TABLE IF NOT EXISTS` silently succeeds even if the table has different columns than expected.
2. **ALTER TABLE limitations in SQLite.** SQLite cannot drop columns (before 3.35.0), cannot change column types, and cannot add NOT NULL columns without defaults. If the initial groups schema is wrong, fixing it later requires the 12-step table recreation process (CREATE new, copy data, DROP old, RENAME new).
3. **Missing foreign key enforcement.** SQLite has foreign keys disabled by default. `PRAGMA foreign_keys = ON` must be called per-connection. Without it, deleting a contact leaves orphaned group_members rows, and deleting a group leaves orphaned membership entries.
4. **Concurrent CLI instances.** Two CLI processes could open the same contacts.db simultaneously. SQLite handles this via file locking, but the current code does not set a busy timeout. The second process gets SQLITE_BUSY and fails with a cryptic error.

**Why it happens:**
The original contacts implementation was deliberately minimal (single table, no versioning) because it only needed contacts. Adding groups turns it into a relational schema with foreign keys, junction tables, and potential for future evolution. The migration infrastructure must be added now, before the schema has more than one table.

**How to avoid:**
1. **Add a `schema_version` table immediately.** `CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)`. Check version on open: if 0 or missing, run v1 migration (current contacts table). If 1, run v2 migration (add groups + group_members). Each migration is idempotent and guarded by version check.
2. **Enable foreign keys.** Call `PRAGMA foreign_keys = ON` after every `sqlite3_open()`. Add `ON DELETE CASCADE` to group_members.contact_name referencing contacts.name.
3. **Set busy timeout.** Call `sqlite3_busy_timeout(db_, 5000)` (5 seconds) after open. This handles concurrent CLI instances gracefully.
4. **Use transactions for migrations.** Wrap each migration step in `BEGIN EXCLUSIVE; ... COMMIT;`. If any step fails, the entire migration rolls back.
5. **Get the groups schema right the first time.** Design the groups and group_members tables with all columns needed for v4.1.0. Do not plan to ALTER later.

**Warning signs:**
- No `schema_version` table in the database
- `PRAGMA foreign_keys` not called after open
- Migration code that does not check current version before altering
- Tests that always start with a fresh database and never test upgrade from v1

**Phase to address:**
Contact groups phase. Schema versioning must be the first thing implemented, before adding any new tables. The groups table schema must be designed and reviewed before coding.

---

### Pitfall 5: Configurable node constants -- SIGHUP reload of sync parameters during active sync sessions

**What goes wrong:**
Ten hardcoded constants are moving to config.json (from peer_manager.h, sync_orchestrator.h, pex_manager.h, connection_manager.h):

| Constant | Current Value | Risk on Hot Reload |
|----------|--------------|-------------------|
| SYNC_TIMEOUT | 30s | Active sync hangs if shortened mid-session |
| BLOB_TRANSFER_TIMEOUT | 120s | In-flight blob transfer killed if shortened |
| KEEPALIVE_INTERVAL | 30s | Timer restart races with keepalive coroutine |
| KEEPALIVE_TIMEOUT | 60s | Mid-evaluation timeout check uses stale/new value inconsistently |
| STRIKE_THRESHOLD | 10 | Lowering mid-session disconnects peers who were previously acceptable |
| PEX_INTERVAL_SEC | 300 | Timer restart races with PEX coroutine |
| MAX_PEERS_PER_EXCHANGE | 8 | Safe to change live |
| MAX_DISCOVERED_PER_ROUND | 3 | Safe to change live |
| MAX_PERSISTED_PEERS | 100 | Safe to change live (caps next persist write) |
| MAX_HASHES_PER_REQUEST | 64 | Mid-sync change causes protocol mismatch with peer |

The dangerous ones are timeouts and thresholds read inside active coroutines. If a sync coroutine reads SYNC_TIMEOUT at the start of a sync session, and SIGHUP changes it mid-session, the coroutine may use a mix of old and new values depending on when each read occurs. This is not a data race (single-threaded event loop), but it is a logic inconsistency.

MAX_HASHES_PER_REQUEST is the most dangerous: if Node A sends 64 hashes per request and Node B has been reconfigured to expect 32, the mismatch does not cause a protocol error (the receiver accepts any count <= 64), but changing it to >64 would exceed the current protocol's implicit contract.

**Why it happens:**
The node's existing SIGHUP handler already reloads several values (max_peers, rate limits, ACL, quotas) because those were designed for reload from the start. The sync/peer constants were not -- they are `static constexpr` in header files, read by coroutines that assume they never change. Moving them to runtime configuration requires analyzing every read site.

**How to avoid:**
1. **Snapshot constants at session start.** Each sync session, PEX round, or keepalive cycle captures the current config values into local variables at the start. The session uses only those locals, never re-reads the config mid-session. This is the pattern the node already uses for quotas (effective_quota() is called at ingest time, not cached).

2. **Validate ranges at config load time.** Each constant needs min/max validation:
   - SYNC_TIMEOUT: min 5s, max 300s
   - BLOB_TRANSFER_TIMEOUT: min 30s, max 600s
   - KEEPALIVE_INTERVAL: min 10s, max 300s (must be < KEEPALIVE_TIMEOUT)
   - KEEPALIVE_TIMEOUT: min 20s, max 600s (must be > KEEPALIVE_INTERVAL)
   - STRIKE_THRESHOLD: min 3, max 100
   - PEX_INTERVAL_SEC: min 60, max 3600
   - MAX_HASHES_PER_REQUEST: min 1, max 64 (protocol hard limit)

3. **Never exceed protocol invariants.** MAX_HASHES_PER_REQUEST must have a hard cap of 64 that config cannot override. This is a protocol-level constant, not a tuning parameter. Make the config validation reject values >64.

4. **Test SIGHUP during active sync.** Integration test: start a long sync, SIGHUP mid-sync, verify sync completes with the values captured at session start and subsequent sessions use new values.

**Warning signs:**
- Constants read directly from config inside coroutine loops (not captured at session start)
- Missing validation ranges for any configurable constant
- MAX_HASHES_PER_REQUEST configurable above 64
- KEEPALIVE_INTERVAL >= KEEPALIVE_TIMEOUT after reload (deadlock: keepalive always fires after timeout)
- Tests that only verify SIGHUP with idle node, never during active sync

**Phase to address:**
Configurable constants phase. The validation ranges and snapshot-at-session-start pattern must be established before any constant is made configurable. The config validation function (validate_config() in config.h) must be extended first.

---

### Pitfall 6: Chunked large files -- memory amplification from simultaneous chunk envelope encryption

**What goes wrong:**
The current `put` command reads the entire file into memory, builds the put payload, envelope-encrypts the whole thing, then sends. For a 500 MiB file, peak memory is roughly 3x file size (raw + payload + envelope). For chunked files >500 MiB, if each chunk is envelope-encrypted independently (as BACKLOG.md describes), and the CLI tries to encrypt multiple chunks in parallel for throughput, memory usage becomes `num_parallel_chunks * chunk_size * 3`. With 50 MiB chunks and 4 parallel encryptions, that is 600 MiB of peak memory just for the encryption buffers -- on top of whatever else the CLI is doing.

Even without parallelism, if the CLI reads a 2 GiB file fully into memory and then chunks it, the 2 GiB is held in memory for the entire duration of the upload.

**Why it happens:**
The current `read_file_bytes()` function reads the entire file into a `std::vector<uint8_t>`. This was fine for files up to 500 MiB. For multi-gigabyte files, the CLI must stream: read a chunk, encrypt it, send it, release the memory, then read the next chunk.

**How to avoid:**
1. **Stream from disk.** Read one chunk at a time using `std::ifstream` with a fixed-size read buffer. Never hold more than one (or a small number of) chunk(s) in memory simultaneously.
2. **Sequential chunk processing for MVP.** Encrypt and upload one chunk at a time. This limits peak memory to `chunk_size * 3` regardless of file size. Parallel chunk upload can be added later as an optimization, but the sequential path must work first and must be the default.
3. **Chunk size selection matters.** Too small (1 MiB) means too many blobs on the node and too many round trips. Too large (200 MiB) means high memory usage. The BACKLOG.md suggests 10-50 MiB. 50 MiB is a good default: 150 MiB peak memory, ~40 chunks for a 2 GiB file, manageable for the node's storage.
4. **Do not buffer the entire manifest until all chunks are uploaded.** Build the chunk manifest incrementally as each chunk is acknowledged. If the upload fails partway, the manifest is incomplete and the CLI reports partial upload (chunks already uploaded can be garbage-collected by TTL or tombstoned).

**Warning signs:**
- `read_file_bytes()` called for files >500 MiB
- `std::vector<uint8_t>` holding the entire file contents
- Chunk encryption parallelism without memory budgeting
- Peak memory tests that only use small files

**Phase to address:**
Chunked large file phase. The streaming read pattern must replace read_file_bytes() for the chunked path. The single-chunk-at-a-time architecture must be the starting point.

---

### Pitfall 7: Contact groups -- case sensitivity and name collision with contacts

**What goes wrong:**
The BACKLOG.md says `--share @team` resolves a group name. But contact names are also strings. If a user has a contact named "team" and a group named "team", `--share team` is ambiguous. If the `@` prefix is the only disambiguation, then `--share @alice` could be confused with a group named "alice" if the user forgets the `@`, or `--share alice` could fail to share with the contact alice if there is a group alice.

Additionally, contact names are case-sensitive in the current SQLite schema (`name TEXT PRIMARY KEY` -- SQLite TEXT comparison is case-sensitive by default). A user who adds "Alice" and then does `--share alice` gets "contact not found." Group names will have the same issue.

**Why it happens:**
The current CLI has no namespace collision between contacts and groups because groups do not exist yet. Adding groups introduces a second namespace. The `@` prefix convention works if enforced consistently, but users will forget it.

**How to avoid:**
1. **Enforce `@` prefix for groups at all parse points.** `--share @team` is always a group. `--share alice` is always a contact. No ambiguity. Store group names in the database without the `@` prefix.
2. **Case-insensitive lookup for both contacts and groups.** Use `COLLATE NOCASE` on the name columns: `name TEXT PRIMARY KEY COLLATE NOCASE`. This makes "Alice", "alice", and "ALICE" all refer to the same contact. Apply to both contacts and groups tables.
3. **Reject names starting with `@`.** Contact names must not start with `@` (reserved for group syntax). Validate on `contact add`. Group names must not start with `@` either (the `@` is CLI syntax, not part of the name).
4. **Unique constraint across namespaces is NOT needed.** A contact "alice" and a group "alice" can coexist because the `@` prefix disambiguates at the CLI level. But document this behavior explicitly.

**Warning signs:**
- Group lookup that does not check the `@` prefix
- Contact names stored with `@` prefix in the database
- Case-sensitive lookups in either contacts or groups
- Tests that always use lowercase names

**Phase to address:**
Contact groups phase. The naming convention and case-insensitive collation must be decided before implementing the groups table. Retrofit COLLATE NOCASE onto the existing contacts table as part of the schema migration.

---

### Pitfall 8: Request pipelining -- node responds with ErrorResponse (type 63) for some requests, breaking response parsing

**What goes wrong:**
When pipelining N ReadRequests, the CLI expects N ReadResponses. But the node can return ErrorResponse (type 63) for any individual request (e.g., blob not found, namespace not found, payload malformed). The ErrorResponse has a different payload format than ReadResponse. If the pipelining code assumes all responses are ReadResponse and tries to parse ErrorResponse as a ReadResponse, it gets garbage or a crash.

Additionally, the node sends ErrorResponse with the original request_id, so correlation is preserved. But if the CLI's pipelining code does not check the response type before parsing, it will fail.

This is compounded by the SyncNamespaceAnnounce (type 62) that the node sends immediately after handshake. If the CLI does not fully drain this message before starting pipelined requests, the first "response" it reads will be the announce, not a ReadResponse. The existing `drain_announce()` handles this for single requests, but it only drains one message. If the node sends additional unsolicited messages (e.g., BlobNotify for subscriptions), those would also appear in the response stream.

**Why it happens:**
The current CLI uses a strict request-response pattern: send one request, receive one response, check its type. Pipelining requires receiving multiple responses and matching them by request_id, handling mixed types in the response stream. This is fundamentally different from the sequential model.

**How to avoid:**
1. **Match responses by request_id, not by position.** Assign unique request_ids (1, 2, 3...) to each pipelined request. When receiving responses, match by request_id and handle each response type independently.
2. **Handle ErrorResponse in every response handler.** Every code path that reads a response must check for type 63 first. Extract the error code and message, report it for that specific request, and continue processing remaining responses.
3. **Drain unsolicited messages.** After handshake, drain SyncNamespaceAnnounce. During pipelining, if an unsolicited message type (not matching any pending request_id) is received, log it and continue reading. Do not count it as a response to a pipelined request.
4. **Track pending request_ids.** Maintain a set of outstanding request_ids. Each received response removes its request_id from the set. When the set is empty, all pipelined responses have been received. If a timeout expires with request_ids still pending, report which requests timed out.

**Warning signs:**
- Response parsing that assumes a fixed type without checking
- Missing ErrorResponse handling in the pipelining code
- Tests that only pipeline requests for blobs that exist, never for missing blobs
- No request_id tracking -- relying on response order instead of correlation

**Phase to address:**
Request pipelining phase. The request_id-based correlation and mixed-type response handling must be designed before implementing parallel downloads.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| No schema versioning in SQLite | Simpler initial code | Cannot safely ALTER tables later, stuck with initial schema | Never -- add schema_version table now |
| Read entire file into memory for chunked upload | Reuses existing read_file_bytes() | OOM for multi-gigabyte files, 3x memory amplification | Never for the chunked path |
| Case-sensitive contact/group names | No code change needed | User confusion, "not found" errors for capitalization differences | Never -- use COLLATE NOCASE from the start |
| Parallel recv() calls for pipelining | Simpler threading model | AEAD nonce desync, data corruption, intermittent crashes | Never -- single reader thread only |
| MAX_HASHES_PER_REQUEST configurable above 64 | More flexibility | Protocol violation, peers reject oversized requests | Never -- hard cap at 64 |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| SQLite foreign keys | Assuming they work by default | `PRAGMA foreign_keys = ON` after every sqlite3_open() |
| SQLite busy handling | No busy timeout -- SQLITE_BUSY crashes the CLI | `sqlite3_busy_timeout(db_, 5000)` after open |
| Envelope segment nonce | Reusing segment nonce across chunk boundaries in multi-blob scheme | Each chunk blob gets its own random base_nonce, segment indices restart at 0 per chunk |
| AEAD counter + pipelining | Multiple threads calling recv_encrypted() | Single reader thread, dispatch work after decryption |
| Node's SyncNamespaceAnnounce | Not draining it before pipelined reads | drain_announce() before any pipelined request sequence |
| Configurable KEEPALIVE_INTERVAL/TIMEOUT | Setting interval >= timeout via config | Validate KEEPALIVE_INTERVAL < KEEPALIVE_TIMEOUT at config load time |
| Chunked recv total_size | No bounds check -- `payload.reserve(total_size)` directly | Validate total_size <= MAX_BLOB_DATA_SIZE + overhead before reserve (pre-existing bug in connection.cpp:626) |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Full file buffering for multi-GiB uploads | OOM on machines with <8 GiB RAM | Stream from disk, one chunk at a time | Files >1 GiB |
| Parallel chunk encryption without memory budget | Memory spikes to N * chunk_size * 3 | Sequential encrypt-send-release cycle for MVP | 4+ parallel chunks of 50 MiB |
| Group membership resolution with N contacts | N individual SQLite queries per group | Single JOIN query: SELECT ... FROM group_members JOIN contacts | Groups with >20 members |
| Pipelining without response timeout | CLI hangs forever if node drops one response | Per-request timeout timer, report timed-out request_ids | Any network instability |
| SIGHUP reload parsing large config | JSON parse blocks event loop | Use the existing async SIGHUP pattern (coroutine member function) | Config file >1 MB (unlikely but possible with many namespace_quotas) |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| No last-segment marker in multi-blob envelopes | Silent truncation: attacker deletes final chunk, recipient gets truncated file that decrypts "successfully" | Segment count in CENV v2 header or final-segment sentinel in nonce |
| Chunk manifest not signed or integrity-protected | Attacker creates fake manifest pointing to wrong chunks, recipient reassembles corrupted file | Manifest blob is signed by the file owner like any other blob; verify namespace ownership on reassembly |
| recv total_size unchecked in chunked reassembly | Memory exhaustion attack: malicious sender claims multi-TiB total_size | Validate total_size <= MAX_BLOB_DATA_SIZE + envelope overhead before allocating |
| Group names as SQL injection vector | SQLite injection via crafted group name | Already mitigated by prepared statements (existing contacts code uses sqlite3_bind_text). Maintain this pattern. |
| Config reload lowers STRIKE_THRESHOLD below current strike count | Instant mass disconnection of previously-acceptable peers | Snapshot threshold at connection start, or only apply new threshold to new connections |

## UX Pitfalls

| Pitfall | User Impact | Better Approach |
|---------|-------------|-----------------|
| Case-sensitive contact lookup | "contact not found" for capitalization typo | COLLATE NOCASE on name columns |
| No progress indicator for multi-GiB uploads | User thinks CLI is frozen during long upload | Print chunk progress: "Uploading chunk 5/40 (250 MiB / 2.0 GiB)" |
| Ambiguous --share syntax | User confuses contact name and group name | Enforce @prefix for groups, clear error on ambiguity |
| Silent partial upload on network failure | User thinks upload completed | Report "uploaded 12/40 chunks -- partial upload, run cleanup or retry" |
| SIGHUP reload success/failure invisible | Operator sends SIGHUP, does not know if it worked | Log reloaded values at INFO level (existing pattern, extend to new constants) |

## "Looks Done But Isn't" Checklist

- [ ] **Schema versioning:** contacts.db has a schema_version table and migration from v1 (contacts only) to v2 (contacts + groups + group_members) is tested
- [ ] **Foreign keys enabled:** `PRAGMA foreign_keys = ON` called after every sqlite3_open(); ON DELETE CASCADE on group_members; test that deleting a contact removes memberships
- [ ] **Last-segment marker:** Multi-blob envelope format authenticates completeness; test with missing-last-chunk produces decryption error, not truncated file
- [ ] **Chunked recv bounds check:** connection.cpp validates total_size before `reserve()`; test with adversarial total_size (e.g., UINT64_MAX)
- [ ] **Pipelining response correlation:** Responses matched by request_id; ErrorResponse handled; test with mix of found/not-found blobs in single pipeline
- [ ] **Config validation ranges:** All 10 configurable constants have min/max validation; test that invalid values are rejected at load time
- [ ] **SIGHUP snapshot:** Active sync sessions use values captured at session start; test SIGHUP during active sync does not change behavior mid-session
- [ ] **Streaming disk read:** Chunked upload reads one chunk at a time; peak memory is bounded; test with file larger than available RAM (or mock)
- [ ] **Case-insensitive names:** Both contacts and groups use COLLATE NOCASE; test mixed-case lookup succeeds
- [ ] **Progress reporting:** Multi-chunk uploads print progress per chunk; multi-blob downloads print progress per blob

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Missing last-segment marker (shipped) | HIGH | Requires envelope format version bump, all existing multi-blob envelopes must be re-encrypted or accepted as legacy v1 |
| AEAD nonce desync from parallel recv | HIGH | Connection is corrupted, must disconnect and reconnect; all in-flight data lost |
| SQLite schema without versioning (already in use) | MEDIUM | Add schema_version table, detect v1 by table inspection, run migration |
| Unchecked total_size in chunked recv | LOW | Add bounds check, existing data unaffected (bug only exploitable by malicious sender) |
| Case-sensitive names (already have mixed-case data) | MEDIUM | ALTER to COLLATE NOCASE, deduplicate any conflicts (unlikely but must handle) |
| Config reload mid-sync (values inconsistent) | LOW | Fix by adding snapshot-at-session-start; existing sessions may have had inconsistent behavior but would self-correct on next sync |
| Memory exhaustion from unbounded chunk encryption | LOW | Switch to streaming read pattern; no persistent data corruption |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Truncation attack (no last-segment marker) | Chunked large files | Test: delete final chunk blob, verify decryption fails; test: full reassembly succeeds |
| AEAD nonce desync from parallel recv | Request pipelining | TSAN test with pipelined requests; verify recv_counter_ accessed from single thread only |
| Chunked transport total_size unchecked | Chunked large files (or immediate pre-fix) | Test: send chunked header with total_size = UINT64_MAX, verify rejection |
| SQLite schema migration | Contact groups | Test: open v1 database, verify migration to v2; test: open v2 database, verify no re-migration |
| SIGHUP mid-sync inconsistency | Configurable constants | Integration test: SIGHUP during active sync, verify session uses original values |
| Memory amplification from chunk buffering | Chunked large files | Memory usage test: upload 2 GiB file, verify peak memory < 300 MiB |
| Group/contact name ambiguity | Contact groups | Test: contact "team" and group "team" coexist; --share @team gets group; --share team gets contact |
| Pipelining mixed response types | Request pipelining | Test: pipeline 10 requests where 3 return ErrorResponse; verify all 10 handled correctly |

## Sources

- [ImperialViolet: Encrypting Streams (Adam Langley, 2014)](https://www.imperialviolet.org/2014/06/27/streamingencryption.html) -- segment truncation attack, last-chunk marker requirement
- [Google Tink: Streaming AEAD](https://developers.google.com/tink/streaming-aead) -- segments bound to location, cannot be reordered
- [Google Tink: AES-GCM-HKDF Streaming](https://developers.google.com/tink/streaming-aead/aes_gcm_hkdf_streaming) -- segment nonce derivation, final segment authentication
- [hpke-js AEAD nonce reuse CVE (GHSA-73g8-5h73-26h4)](https://github.com/dajiaji/hpke-js/security/advisories/GHSA-73g8-5h73-26h4) -- concurrent Seal() calls reuse nonce, total confidentiality loss
- [CWE-364: Signal Handler Race Condition](https://cwe.mitre.org/data/definitions/364.html) -- async-signal-safe operations only in handlers
- [HashiCorp Vault SIGHUP race (GitHub #27100)](https://github.com/hashicorp/vault/issues/27100) -- SIGHUP during startup race condition
- [SQLite Forum: Schema Versioning and Migration](https://www.sqliteforum.com/p/sqlite-versioning-and-migration-strategies) -- ALTER TABLE limitations, version tracking
- [David Rothlis: Declarative SQLite Schema Migration](https://david.rothlis.net/declarative-schema-migration-for-sqlite/) -- migration patterns for SQLite
- chromatindb codebase: `cli/src/envelope.cpp` -- current CENV v1 segmented encryption (no last-segment marker)
- chromatindb codebase: `cli/src/connection.cpp:626` -- chunked recv with unchecked total_size (pre-existing bug)
- chromatindb codebase: `db/peer/peer_manager.h:76-88` -- hardcoded sync/peer constants to be made configurable
- chromatindb codebase: v1.0.0 PEX SIGSEGV post-mortem -- AEAD nonce desync from concurrent writes (same class of bug)
- chromatindb PROTOCOL.md: CENV envelope format (lines 1153-1283) -- segment nonce derivation, no last-segment marker

---
*Pitfalls research for: CLI polish features (groups, chunked files, pipelining, configurable constants)*
*Researched: 2026-04-15*
