# Domain Pitfalls: v2.0 Closed Node Model + Larger Blobs

**Domain:** Adding access control and larger blob support to an existing PQ-encrypted blob store (chromatindb v2.0)
**Researched:** 2026-03-05
**Confidence:** HIGH (based on direct codebase analysis + verified external sources)

---

## Critical Pitfalls

Mistakes that cause security vulnerabilities, data corruption, or require rewrites.

### Pitfall 1: Access Control Enforced at the Wrong Layer

**What goes wrong:** Access control is implemented in BlobEngine::ingest() or PeerManager::on_peer_message() instead of at the transport/handshake layer. An unauthorized peer connects, completes the full ML-KEM-1024 handshake (1568-byte pubkey exchange + encapsulation), exchanges ML-DSA-87 auth signatures (4627-byte signatures), and only gets rejected on first write. They can freely read (get_blob, list_namespaces have no access checks).

**Why it happens:** BlobEngine::ingest() is the natural "validation" point in the current architecture -- it already does structural checks, namespace ownership, and signature verification. Developers add access control there because it's where other validation lives. But access control is a transport concern (who can connect), not an application concern (is this blob valid).

**Consequences:** (1) DoS vector: each unauthorized connection forces ~10ms of ML-KEM encapsulation + ML-DSA verification CPU. An attacker sending connection requests can peg the CPU with crypto operations that will all be rejected. (2) Read access is unguarded: a closed node that only checks writes still allows unauthorized reads of all data. (3) Unnecessary bandwidth: the handshake exchanges ~8 KiB of PQ keys before rejection.

**Prevention:**
1. Enforce at the earliest possible point in the handshake. In the current code, the initiator's signing pubkey is embedded in the KemPubkey message payload (bytes 1568..4160 of the decoded payload in `HandshakeResponder::receive_kem_pubkey()`). The responder can check this pubkey against allowed_keys BEFORE calling `kem.encaps()` -- saving the entire encapsulation cost.
2. For the responder side (inbound connections): check the initiator's signing pubkey against allowed_keys after decoding the KemPubkey message but before encapsulating. If not allowed, close the connection immediately.
3. For the initiator side (outbound connections): the responder's signing pubkey is in the KemCiphertext message (bytes 1568..4160 of the decoded payload in `HandshakeInitiator::receive_kem_ciphertext()`). Check after decapsulation (KEM cost is unavoidable when initiating, but you can skip the expensive ML-DSA auth exchange).
4. Do NOT add access checks to BlobEngine or Storage. The correct layering: transport gates who connects, engine validates blob correctness.

**Detection:** Connect to a closed node with an unauthorized key and time the rejection. If it takes >1ms (KEM operation completed), the check is too late.

**Phase to address:** First phase of access control implementation. This is an architectural decision that must be correct before code is written.

---

### Pitfall 2: Memory Exhaustion from Large Blobs in Sync Protocol

**What goes wrong:** Three compounding memory issues when blob size increases from ~15 MiB to 100 MiB:

**Issue A -- Hash collection loads all blob data:** `SyncProtocol::collect_namespace_hashes()` (sync_protocol.cpp:26-44) calls `engine_.get_blobs_since(namespace_id, 0)`, which calls `storage_.get_blobs_by_seq()`, which loads EVERY blob's full data (including the `std::vector<uint8_t> data` field) into memory. For a namespace with 20 x 100 MiB blobs, this allocates ~2 GiB just to compute 32-byte hashes. The blob data is loaded, the blob is re-encoded via `wire::encode_blob(blob)`, hashed, then discarded.

**Issue B -- Batch blob transfer:** `SyncProtocol::encode_blob_transfer()` (sync_protocol.cpp:247-260) concatenates ALL requested blobs into a single `std::vector<uint8_t>`. Ten missing 100 MiB blobs = 1 GiB single allocation.

**Issue C -- Receive buffer in coroutine frame:** `Connection::recv_raw()` (connection.cpp:66-89) allocates `std::vector<uint8_t> data(len)` which, for a 1 GiB batch transfer, allocates 1 GiB in the coroutine frame. With 32 peers syncing simultaneously, this compounds.

**Consequences:** OOM kills, node crashes during sync. Even without OOM, RSS spikes cause swap thrashing on systems without swap disabled.

**Prevention:**
1. **Fix Issue A immediately:** The sequence index (`seq_map`) already stores `[namespace:32][seq:8] -> hash:32`. Rewrite `collect_namespace_hashes()` to iterate the seq_map cursor directly and collect hash values, without loading any blob data. This is O(count * 32 bytes) instead of O(total_blob_data). The code for cursor iteration already exists in `Storage::get_blobs_by_seq()` -- adapt it to read only the value (hash) without fetching from blobs_map.
2. **Fix Issue B:** Transfer blobs one at a time. Replace the batch pattern `[count][len1][blob1]...[lenN][blobN]` with individual BlobTransfer messages, one per blob. This bounds peak memory to one blob at a time.
3. **Fix Issue C:** For a single 100 MiB blob, a 100 MiB receive buffer is acceptable. But add a per-connection memory budget: if total outstanding receive buffer memory across all connections exceeds a threshold (e.g., 512 MiB), back-pressure by pausing reads on low-priority connections.

**Detection:** Run sync between two nodes where one has a namespace with ten 50 MiB blobs. Monitor RSS with `ps -o rss`. If RSS exceeds 2x the total blob data size, Issue A is active.

**Phase to address:** Must be fixed BEFORE bumping the blob size limit. This is a prerequisite, not a "nice to have."

---

### Pitfall 3: libmdbx Overflow Pages and Write Amplification for Large Values

**What goes wrong:** libmdbx stores values in B-tree pages (default 4 KiB). Values larger than approximately 1/4 page size (~1 KiB with default 4 KiB pages) go into "overflow pages" -- contiguous runs of pages allocated just for that value. A 100 MiB blob consumes ~25,600 contiguous overflow pages. The copy-on-write MVCC model means ANY write transaction touching a large blob must allocate a fresh 25,600-page run, even for a simple re-ingest (dedup check requires reading the blob, and the write transaction locks the pages).

**Why it happens:** libmdbx/LMDB is designed as a key-value store for structured data, not a blob store. Large values are technically supported (up to ~2 GiB per the `mdbx.h` definition: `MDBX_val` with `size_t` length) but with significant overhead. The B-tree page model optimizes for values that fit in a page.

**Consequences:**
- Database file bloat: fragmentation from freed overflow page runs leaves gaps that may not be reusable for different-sized blobs.
- Slow writes: allocating 25,600 contiguous pages in a fragmented database requires searching the freelist.
- Slow expiry: `run_expiry_scan()` deleting a 100 MiB blob frees 25,600 pages in one transaction. With many expired large blobs, the freelist management itself becomes expensive.
- The mmap region must be large enough to hold all active data. With 64 GiB upper geometry (current setting), you can store ~640 x 100 MiB blobs before hitting the limit.

**Prevention:**
1. **Store blob data externally on the filesystem.** Use the content hash as the filename: `data_dir/blobs/<hex_sha3_256>`. Store only metadata in libmdbx: `[namespace:hash] -> {pubkey, ttl, timestamp, signature, data_size, file_path}`. This is the standard pattern for blob stores (Git, IPFS, container registries all do this).
2. External storage eliminates overflow page fragmentation, mmap pressure, and write amplification. Deletion is just `unlink()` -- no transaction overhead.
3. If keeping blobs in libmdbx (simpler code, ACID guarantees): increase page size to 65536 bytes via `geometry.pagesize`. This reduces overflow page count by 16x (25,600 pages -> 1,600 pages for 100 MiB). BUT: this is a database-wide setting -- all small metadata values also use 64 KiB pages, wasting space. And it's a migration: existing databases cannot change page size without dump/reload.
4. If using external storage: ensure atomic write (write to temp file, fsync, rename) to prevent partial blobs on crash. Content-addressing makes this safe -- if the file is corrupt, re-derive the hash to detect it.

**Detection:** After storing ten 100 MiB blobs, check `ls -la data_dir/blobs.mdbx`. If the file is >1.5x the total blob data, overflow fragmentation is the cause.

**Phase to address:** Architectural decision needed in the first phase of larger blob support. This affects storage, sync, and the wire format (blob transfer may send file contents directly instead of FlatBuffer-encoded values).

---

### Pitfall 4: Sync Protocol Timeout with Large Blob Transfers

**What goes wrong:** The sync protocol uses a 30-second timeout (`SYNC_TIMEOUT` in peer_manager.cpp:299,453). With 100 MiB blobs, transferring even a few blobs takes much longer. At 10 MB/s (typical VPS upload): one 100 MiB blob takes 10 seconds, three take 30 seconds -- hitting the timeout exactly. The peer considers the sync failed and disconnects.

**Why it happens:** The sync timeout was designed for small blobs. The timeout applies to waiting for the next message (`recv_sync_msg` calls), and during large blob transfer, the sender is busy writing data and cannot send the next sync message until the transfer completes. The sequential protocol design means the entire connection is blocked during sync.

**Consequences:** Sync consistently fails for large blobs. The node retries on the next sync interval, fails again, and large blobs never sync. The 60-second sync interval (config_.sync_interval_seconds) means the node tries and fails every minute.

**Prevention:**
1. Make sync timeout adaptive based on expected transfer size. After computing the diff, estimate total bytes to transfer and set timeout = max(30s, estimated_bytes / MIN_EXPECTED_BANDWIDTH).
2. Better: use per-blob transfer with acknowledgments. After sending each blob, wait for an ack before sending the next. This creates a natural heartbeat and allows the receiver to signal progress.
3. The remaining BlobRequest loop (peer_manager.cpp:409-421, 534-546) already has a short 2-second timeout for "any remaining" messages. This is fine for small blobs but must be increased or made adaptive for large ones.

**Detection:** Test sync between two nodes with 100 MiB blobs over a bandwidth-limited connection (e.g., `tc qdisc` to limit to 10 MB/s). If sync times out, this is the cause.

**Phase to address:** Must be addressed in the sync protocol changes for larger blob support.

---

## Moderate Pitfalls

### Pitfall 5: Frame Size Limit vs Blob Size Limit Inconsistency

**What goes wrong:** The current `MAX_FRAME_SIZE` is 16 MiB (framing.h:13). If the blob limit is bumped to 100 MiB but MAX_FRAME_SIZE isn't updated, blobs larger than ~16 MiB minus AEAD tag minus framing overhead cannot be transmitted. The `recv_raw()` function rejects frames exceeding MAX_FRAME_SIZE (connection.cpp:78-80). The `read_frame()` function throws on frames exceeding MAX_FRAME_SIZE (framing.cpp:56-57).

The subtler issue: a 100 MiB blob, after FlatBuffer encoding (adds pubkey, signature, namespace, ttl, timestamp overhead -- ~7 KiB for ML-DSA-87), is ~100.007 MiB. After AEAD encryption (adds 16-byte Poly1305 tag), it's ~100.007 MiB + 16 bytes. The length prefix is 4 bytes. So MAX_FRAME_SIZE must be at least 100.007 MiB + some margin.

**Why it happens:** MAX_FRAME_SIZE was set as a reasonable limit for v1.0's use case. It's a `constexpr` in a header file with no relationship to MAX_BLOB_SIZE (which doesn't exist as a constant yet).

**Prevention:**
1. Define both constants in a shared location: `MAX_BLOB_DATA_SIZE` (the user-facing limit on blob.data.size()) and `MAX_FRAME_SIZE` (which must be >= MAX_BLOB_DATA_SIZE + max_overhead). Calculate MAX_FRAME_SIZE from MAX_BLOB_DATA_SIZE with explicit overhead accounting.
2. Better: implement chunked blob transfer so MAX_FRAME_SIZE doesn't need to equal MAX_BLOB_SIZE. Keep MAX_FRAME_SIZE at 16 MiB, chunk larger blobs across multiple frames. This also solves the memory pressure issue.
3. If increasing MAX_FRAME_SIZE: use it as a hard limit, not a suggestion. The receive side must reject frames exceeding the limit BEFORE allocating the buffer (which it already does in `recv_raw` -- good).

**Detection:** Attempt to ingest and sync a blob larger than MAX_FRAME_SIZE. If the sync fails with "frame exceeds maximum size," this is the cause.

**Phase to address:** Early in the larger blob support phase. Must be resolved before any large blob testing.

---

### Pitfall 6: Config Reload Without Coroutine Safety

**What goes wrong:** Adding `allowed_keys` to the config and supporting runtime reload (SIGHUP) introduces mutable state that coroutines read. The current Config is `const config::Config&` everywhere (PeerManager, Server, SyncProtocol). Making it mutable or adding a reload mechanism risks data races.

However, the codebase runs on a single `io_context` thread. Asio signal handling via `asio::signal_set` (already used in server.h:77) posts the handler to the io_context, so it runs on the same thread as coroutines. This means there is NO data race IF the reload handler is an Asio-dispatched callback, not a raw signal handler.

**The real danger:** A developer implements SIGHUP by calling `signal(SIGHUP, handler)` with a raw C signal handler that modifies the config directly. This runs on the signal delivery thread (any thread), not the io_context thread, creating a data race with any coroutine reading config values.

**Prevention:**
1. Use `asio::signal_set` (already in the codebase) to register SIGHUP. The handler runs on the io_context thread -- no race.
2. On reload: parse the new config file into a fresh Config struct, then swap the reference. Since all code runs on one thread, a synchronous swap is safe.
3. For `allowed_keys`: cache the set at connection acceptance time. Don't re-read it during the message loop.
4. For existing connections after key revocation: iterate `peers_` (which is safe -- single thread) and close connections whose pubkey is no longer in allowed_keys. This uses the existing `close_gracefully()` coroutine pattern.

**Detection:** After implementing config reload, add a log line in the reload handler that prints `std::this_thread::get_id()` and compare with the id printed at io_context startup. If they differ, you have a thread safety issue.

**Phase to address:** When implementing the config-based access control.

---

### Pitfall 7: Source Restructure Breaking FlatBuffers Generated Headers

**What goes wrong:** The CMakeLists.txt has hardcoded paths for FlatBuffers schema compilation (lines 91-117):
```cmake
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/schemas)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/wire)
```
If the source tree is restructured (e.g., `src/wire/` moves to `src/protocol/wire/`), the generated headers output to the wrong location. The `#include "wire/blob_generated.h"` in other files fails.

Additionally, every source file is explicitly listed in `add_library(chromatindb_lib ...)` (lines 122-141) and `add_executable(chromatindb_tests ...)` (lines 166-185). Moving any file requires updating both lists.

**Why it happens:** CMake with explicit source lists is correct practice (avoids glob() pitfalls) but fragile under restructuring. FetchContent cached builds in `build/_deps/` add complexity -- stale object files from pre-restructure builds cause confusing linker errors.

**Prevention:**
1. If restructuring: do it in a single atomic commit. Move files AND update ALL of: CMakeLists.txt source lists, FlatBuffers output paths, include paths in `target_include_directories`, and all `#include` directives.
2. After restructuring, ALWAYS do a clean build: `rm -rf build && cmake -B build && cmake --build build`. Do not trust incremental builds.
3. Verify test discovery still works: `cmake --build build --target test` should find all tests.
4. Check FlatBuffers schema namespace: if the `.fbs` file has `namespace chromatin.wire;`, this generates C++ code in `namespace chromatin::wire`. If the C++ namespace is renamed, update the `.fbs` file too and regenerate.
5. Strongly recommend: restructure FIRST, in isolation, before adding any new features. Verify all 155 tests pass. Then start feature work on the clean structure.

**Detection:** Build failure after restructure. Linker errors (undefined reference) = missing source in CMakeLists.txt. Compile errors (file not found) = missing include path update. Compile errors (namespace not found) = FlatBuffers schema namespace mismatch.

**Phase to address:** First phase of the milestone, before any feature work.

---

### Pitfall 8: TOCTOU Between Handshake Auth Check and Connection Lifetime

**What goes wrong:** In a closed node, the access check happens at handshake time: peer's signing pubkey is compared against `allowed_keys`. The handshake succeeds, the connection enters `message_loop()`, and the peer has full access for the lifetime of the connection. If the operator removes the peer's key from `allowed_keys` and reloads config, the peer retains access until disconnect.

**Why it happens:** This is inherent to connection-based protocols. The authentication happens once (at connect), not on every message. It's the same model as SSH, TLS client auth, and WireGuard -- auth at handshake, access for session lifetime.

**Consequences:** Key revocation has delayed effect. The severity depends on the deployment: for a private node with 2-3 known peers and infrequent key changes, this is acceptable. For a node with many peers and frequent access changes, it's a gap.

**Prevention:**
1. Accept the per-connection model. Auth at handshake, access for session lifetime. This is the standard approach and the simplest to implement correctly.
2. On config reload: iterate connected peers, disconnect any whose pubkey is no longer in `allowed_keys`. This gives "eventual" revocation (within seconds of reload, not instant mid-message).
3. Do NOT add per-message access checks. This adds O(n) lookup on every message on the hot path, complicates the coroutine flow, and the single-threaded model means the disconnect-on-reload approach is effectively instant anyway.
4. Log when a connected peer's key is revoked but they remain connected, so the operator knows revocation is pending disconnect.

**Detection:** Remove a connected peer's key from config, reload, and verify the peer gets disconnected within seconds (not at next sync interval or next restart).

**Phase to address:** Access control phase, specifically in the config reload handler.

---

### Pitfall 9: Blob Size Validation Missing from Ingest Pipeline

**What goes wrong:** The current `BlobEngine::ingest()` validates pubkey size, signature presence, namespace ownership, and signature correctness (engine.cpp:39-73). It does NOT validate `blob.data.size()`. If MAX_BLOB_DATA_SIZE is defined but not checked in the ingest path, a peer can send an oversized blob that passes all crypto validation and gets stored.

The frame size check in `recv_raw()` (connection.cpp:78) provides a coarse guard, but MAX_FRAME_SIZE may be larger than MAX_BLOB_DATA_SIZE (it includes protocol overhead). And during sync, `SyncProtocol::ingest_blobs()` calls `engine_.ingest()` -- the blob was already received and parsed from a valid frame.

**Consequences:** An oversized blob is stored, consuming excessive storage. It then replicates to other nodes via sync, amplifying the damage.

**Prevention:**
1. Add `blob.data.size() > MAX_BLOB_DATA_SIZE` as the FIRST check in `BlobEngine::ingest()`, before the pubkey size check. This is the cheapest possible rejection (size_t comparison).
2. Also add the check in `SyncProtocol::ingest_blobs()` before calling `engine_.ingest()`, as a defense-in-depth measure.
3. Define `MAX_BLOB_DATA_SIZE` as a constexpr alongside `BLOB_TTL_SECONDS` in the protocol constants.

**Detection:** Attempt to ingest a blob with data larger than the limit. If it succeeds, the check is missing.

**Phase to address:** When defining the new blob size limit. Trivial to implement but easy to forget.

---

## Minor Pitfalls

### Pitfall 10: Hardcoded Protocol Constants in Multiple Files

**What goes wrong:** Protocol constants are scattered: `MAX_FRAME_SIZE` in framing.h, `BLOB_TTL_SECONDS` in config.h, `STRIKE_THRESHOLD` and `PEX_INTERVAL_SEC` in peer_manager.h. There is no `MAX_BLOB_DATA_SIZE`. When bumping limits for v2.0, each constant must be found and updated independently. Missing one creates silent inconsistencies.

**Prevention:** Consolidate all protocol constants into a single header (e.g., `src/protocol_constants.h` or `src/config/protocol.h`). Include: MAX_BLOB_DATA_SIZE, MAX_FRAME_SIZE (derived from MAX_BLOB_DATA_SIZE), BLOB_TTL_SECONDS, SYNC_TIMEOUT_BASE, etc. Individual modules can still define module-specific constants locally, but protocol-wide constants belong in one place.

**Phase to address:** Early in the milestone, before adding new constants.

---

### Pitfall 11: Namespace Rename Cascading Through 40+ Files

**What goes wrong:** If the C++ namespace `chromatin::` is renamed, every `namespace chromatin {` block, every `chromatin::` qualified name, and the FlatBuffers schema `namespace` declarations must be updated. The codebase has nested namespaces across 19 source files and 21 header files (40+ total). The FlatBuffers generated headers (`blob_generated.h`, `transport_generated.h`) use the namespace from the `.fbs` schema -- updating C++ files without the schema leaves the generated code in the old namespace.

**Prevention:**
1. Question whether the rename is necessary. The existing `chromatin::` namespace works. YAGNI.
2. If renaming: use `grep -rn "chromatin::" src/ tests/` and `grep -rn "namespace chromatin" src/ tests/` to find all occurrences. Update `.fbs` schema files. Regenerate FlatBuffers headers. Clean build.
3. Do not attempt a partial rename or gradual migration. Do it all at once.

**Phase to address:** Only if explicitly planned. Recommend against unless there's a strong reason.

---

### Pitfall 12: Expiry Scan Transaction Size with Large Blobs

**What goes wrong:** `run_expiry_scan()` (storage.cpp:444-507) deletes all expired blobs in a SINGLE write transaction. If blobs are stored in libmdbx (not externally), deleting a 100 MiB blob frees ~25,600 overflow pages. Deleting 10 expired 100 MiB blobs frees ~256,000 pages in one transaction. The freelist management for this many pages can cause the transaction to run slowly or even exceed internal limits.

**Prevention:**
1. If using external blob storage (recommended, see Pitfall 3): expiry just deletes index entries + unlinks files. Transaction size is tiny. This pitfall becomes a non-issue.
2. If keeping blobs in libmdbx: batch the expiry scan. Delete N blobs per transaction (e.g., 5 large blobs), commit, then start a new transaction. This bounds the freelist work per transaction.

**Phase to address:** Relevant only if large blobs are stored in libmdbx. If external storage is used, this is moot.

---

### Pitfall 13: AEAD Nonce Counter Not a Concern (But Looks Like One)

**What goes wrong:** Nothing, actually. This is a "looks dangerous but isn't" entry. The codebase uses IETF ChaCha20-Poly1305 with a 64-bit counter in the nonce (4 zero bytes + 8-byte big-endian counter). The IETF variant supports messages up to 64*(2^32)-64 bytes (~256 GiB) per encryption call. A 100 MiB blob is 0.04% of this limit. The 64-bit counter allows 2^64 frames per direction per connection, which at 1 million frames/second would take 584,542 years to exhaust.

**Why this matters:** When reviewing the code for large blob support, someone will look at the nonce construction in `make_nonce()` (framing.cpp:8-17) and wonder if larger messages cause problems. They don't. The per-message size limit is ~256 GiB. The counter space is effectively infinite.

**Prevention:** No code changes needed. Document this in a comment near `make_nonce()` so future reviewers don't waste time investigating.

**Phase to address:** No action needed. Include in code review notes.

---

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Severity | Mitigation |
|-------------|---------------|----------|------------|
| Source restructure | Build breakage from stale objects (Pitfall 7) | Moderate | Single atomic commit, clean build, verify all 155 tests |
| Source restructure | Namespace rename cascading (Pitfall 11) | Moderate | Avoid rename unless necessary. If needed, update .fbs schemas too |
| Access control | Auth at wrong layer (Pitfall 1) | **Critical** | Enforce in handshake before KEM encapsulation, not in engine |
| Access control | TOCTOU on key revocation (Pitfall 8) | Moderate | Disconnect revoked peers on config reload |
| Access control | Config reload thread safety (Pitfall 6) | Moderate | Use asio::signal_set, not raw signal handler |
| Larger blob support | Memory exhaustion in sync (Pitfall 2) | **Critical** | Fix hash collection to use index, transfer blobs individually |
| Larger blob support | libmdbx overflow pages (Pitfall 3) | **Critical** | External blob storage or larger page size |
| Larger blob support | Sync timeout too short (Pitfall 4) | **Critical** | Adaptive timeout or per-blob transfer with acks |
| Larger blob support | Frame/blob size mismatch (Pitfall 5) | Moderate | Define constants together, derive MAX_FRAME from MAX_BLOB |
| Larger blob support | Missing blob size validation (Pitfall 9) | Moderate | Add size check as first ingest validation |
| Larger blob support | Expiry scan transaction size (Pitfall 12) | Minor | Batch deletions or use external storage |
| Protocol constants | Scattered constants (Pitfall 10) | Minor | Consolidate to single header early |

## Key Insight: Ordering Matters

The critical pitfalls have a dependency chain:

1. **Source restructure** (Pitfall 7) should happen FIRST because it's mechanical, testable, and gives a clean foundation. Do not restructure while simultaneously adding features.
2. **Storage architecture decision** (Pitfall 3: external vs libmdbx) must be decided BEFORE larger blob implementation because it affects sync protocol, ingest pipeline, and expiry. This is the hardest decision in the milestone.
3. **Access control** (Pitfall 1) can be implemented independently of blob size changes. It modifies the handshake, not the storage or sync layers. Can be done in parallel with or before larger blob work.
4. **Sync protocol changes** (Pitfalls 2, 4, 5) depend on the storage decision. If blobs are external, sync transfers file contents. If blobs are in libmdbx, sync transfers FlatBuffer-encoded values.

## Sources

- [libmdbx README - overflow pages, value size limits](https://github.com/erthink/libmdbx/blob/master/README.md) - HIGH confidence
- [IETF ChaCha20-Poly1305 construction - libsodium docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction) - HIGH confidence (official docs, confirms ~256 GiB per-message limit)
- [RFC 7539 - ChaCha20 and Poly1305 for IETF Protocols](https://www.rfc-editor.org/rfc/rfc7539) - HIGH confidence (IETF standard)
- [C++20 coroutine memory allocation - Raymond Chen](https://devblogs.microsoft.com/oldnewthing/20231115-00/?p=109020) - HIGH confidence (Microsoft developer blog)
- [C++20 coroutine downsides](https://reductor.dev/cpp/2023/08/10/the-downsides-of-coroutines.html) - MEDIUM confidence (independent analysis)
- [TOCTOU race conditions - CWE-367](https://cwe.mitre.org/data/definitions/367.html) - HIGH confidence (MITRE standard)
- [CMake FetchContent pitfalls](https://runebook.dev/en/docs/cmake/module/fetchcontent) - MEDIUM confidence
- chromatindb v1.0 source code (direct analysis of all 19 source files + 21 headers) - HIGH confidence
- chromatindb v1.0 RETROSPECTIVE.md - HIGH confidence (verified project history)

---
*Pitfalls research for: chromatindb v2.0 -- closed node model + larger blob support*
*Researched: 2026-03-05*
