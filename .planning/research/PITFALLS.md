# Domain Pitfalls

**Domain:** Sync resumption, namespace quotas, crypto performance optimization, test relocation, and cleanup for chromatindb (C++20 distributed blob store)
**Researched:** 2026-03-16
**Stack context:** libmdbx (MMAP/ACID), liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256), libsodium (ChaCha20-Poly1305), Standalone Asio (C++20 coroutines, single io_context thread), 100 MiB blob support, DARE encryption, one-blob-at-a-time sequential sync

## Critical Pitfalls

Mistakes that cause data loss, sync divergence, or rewrites. These directly threaten correctness.

### Pitfall 1: Sync cursor staleness after deletions and expiry

**What goes wrong:** Per-peer sync cursors track "last synced seq_num" per namespace to avoid re-syncing already-transferred blobs. But the current storage layer creates seq_map gaps: `delete_blob_data()` removes seq_map entries, and `run_expiry_scan()` removes blobs from blobs_map without touching seq_map (leaving orphan seq entries pointing to deleted blobs). A cursor pointing to seq_num=50 means "I have everything up to 50" -- but if blobs at seq 30-40 were deleted via tombstones and the peer never received the tombstones, the cursor would skip them. The peer never learns about the deletions.

**Why it happens:** Cursors are a natural optimization for "resume where I left off," but the underlying data is mutable. Deletions, tombstones, and expiry create a non-monotonic view: data that existed at cursor-time may be gone, and new tombstones may have appeared that weren't synced.

**Consequences:** Permanent sync divergence between peers. Node A deletes a blob, stores tombstone. Node B's cursor is past the tombstone's seq_num. Node B never requests the tombstone, so Node B keeps the deleted blob forever. This is a silent correctness failure -- the network appears healthy but data is inconsistent.

**Prevention:**
- Cursors should track the hash-level diff, not just seq_num. The current hash-list diff approach (collect all hashes, compute set difference) is correct because it catches tombstones as "hashes peer has that we don't have." Sync resumption must preserve this property.
- Use seq_num cursor as an optimization hint only: "start scanning hashes from this seq_num" rather than "skip everything below this seq_num."
- Always include tombstone hashes in sync regardless of cursor position. Tombstones are small (36 bytes) and correctness-critical.
- After cursor-based sync, fall back to full hash-list diff periodically (e.g., every Nth sync round) to catch any drift.

**Detection:** Log hash counts per namespace after sync. If two peers with cursors show "0 missing" but blob counts differ, the cursor is masking divergence.

### Pitfall 2: Namespace quota check-then-act race in single-threaded context

**What goes wrong:** Namespace quota enforcement requires checking current namespace usage (size or count) before accepting a write. In chromatindb's single io_context thread model, this seems safe -- but it is not, because the check and the store happen across a co_await boundary. The flow is: (1) check quota, (2) verify signature (ML-DSA-87, which is synchronous but slow), (3) store blob. Between step 1 and step 3, the io_context can schedule other coroutines. If two ingest coroutines for the same namespace are in flight (e.g., direct write + sync ingest), both check quota at step 1 and both pass, then both store at step 3, exceeding the quota.

**Why it happens:** The current code already has this pattern for global storage limits (`used_bytes() >= max_storage_bytes_` in engine.cpp:53), but global limits are approximate by design (libmdbx `mi_geo.current` is coarse). Namespace quotas are expected to be precise, making this race visible and consequential.

**Consequences:** Namespace quota silently exceeded by the number of concurrent in-flight writes (typically 2: one sync, one direct write). For a small quota (e.g., 10 blobs), this is a 20% overrun. For a tight size quota (e.g., 10 MiB), two concurrent 5 MiB blobs could push to 20 MiB.

**Prevention:**
- Accept that namespace quotas are approximate, like the existing global limit. Document "quota may be exceeded by up to N blobs during concurrent writes" where N = max concurrent ingests per namespace.
- Alternatively: check quota inside the libmdbx write transaction (after `start_write()`), where the check-and-store is atomic. This is the correct approach because libmdbx write transactions are serialized -- only one write txn exists at a time. Move the quota check from `engine.cpp` into `storage.cpp::store_blob()` where it can be inside the write txn.
- Do NOT add mutexes. chromatindb is single-threaded by design. The serialization point is the libmdbx write transaction, not a C++ mutex.

**Detection:** Test with concurrent ingests to the same namespace at quota boundary. If quota is 5 and you send 6 concurrently, check whether 5 or 6 are stored.

### Pitfall 3: Crypto offloading to thread pool breaks AEAD nonce ordering

**What goes wrong:** The large blob crypto bottleneck (96% CPU at 1 MiB blobs, 15.3 blobs/sec) tempts offloading ML-DSA-87 verification + SHA3-256 hashing to a thread pool. But the current architecture relies on single-threaded execution for correctness: AEAD send/recv counters (Connection::send_counter_, recv_counter_) are plain uint64_t with no synchronization. If two verification results arrive from a thread pool and both try to send responses, the AEAD nonce ordering breaks and the encrypted channel corrupts.

**Why it happens:** The bottleneck is real and dramatic (15.3 vs 50.2 blobs/sec). The natural instinct is "use more cores." But the entire protocol stack from Connection through PeerManager assumes single-threaded execution on one io_context.

**Consequences:** AEAD nonce desync corrupts the encrypted channel. Peer sees "AEAD decrypt failed" and disconnects. Every sync with large blobs fails. In testing this looks like "intermittent connection drops" that are impossible to reproduce deterministically because they depend on thread scheduling.

**Prevention:**
- Offload ONLY the pure computation (ML-DSA-87 verify + SHA3-256 hash) to a thread pool. The result (bool verified + hash bytes) must be posted back to the io_context via `asio::post(ioc_, ...)` before any protocol action (send, store, etc.).
- Use the pattern: `co_await asio::post(thread_pool, use_awaitable)` to switch contexts, run CPU work, then `co_await asio::post(ioc_, use_awaitable)` to return to the io_context before touching any connection state.
- Verify that no Connection, PeerInfo, or Storage member is accessed from the thread pool. All of these are single-thread-only.
- The SHA3-256 hash in `wire::blob_hash()` is called inside `storage_.store_blob()` which is inside a write transaction -- this must stay on the io_context thread. Only the verification in `engine_.ingest()` can be offloaded.
- Profile first: the bottleneck may be `SHA3-256(1MiB)` + `ML-DSA-87 verify(1MiB signing input)`. If SHA3-256 dominates, offloading just the hash may be sufficient.

**Detection:** Run large-blob sync with ASAN + TSAN enabled. TSAN will catch any data race from thread pool access to single-threaded state. Without TSAN, the symptom is "AEAD decrypt failed" log messages during sync.

## Moderate Pitfalls

Mistakes that cause incorrect behavior, wasted effort, or performance issues, but are recoverable.

### Pitfall 4: CMake catch_discover_tests breaks when tests move to db/

**What goes wrong:** Moving test source files from `tests/` to `db/tests/` requires updating the CMakeLists.txt test target, but `catch_discover_tests()` has a subtle dependency on the test executable's working directory and link targets. The current setup has `chromatindb_tests` in the root CMakeLists.txt linking against `chromatindb_lib`. If tests move into `db/CMakeLists.txt`, the Catch2 target (`Catch2::Catch2WithMain`) must be available in that scope, `catch_discover_tests()` must be called in the same directory as `add_executable()`, and the CTest configuration must propagate correctly.

**Why it happens:** CMake's `catch_discover_tests()` works by running the test binary with `--list-tests` at CTest time. The binary path is resolved relative to the CMake binary directory where `add_executable()` was called. Moving the test target to a subdirectory changes this path.

**Consequences:** `ctest` reports "No tests found" after the relocation. The build succeeds but `ctest --test-dir build` shows 0 tests. CI passes because it only checks the build step, not the test step. Tests silently stop running.

**Prevention:**
- Keep `include(CTest)` in the root CMakeLists.txt (or wherever `enable_testing()` is called).
- In `db/CMakeLists.txt`, guard the test target with `if(BUILD_TESTING)` and call both `add_executable()` and `catch_discover_tests()` in the same file.
- Verify with `ctest --test-dir build -N` (dry run, lists test names) immediately after relocation.
- The `include(Catch)` call that provides `catch_discover_tests()` must appear in or above the directory where it is called. Since Catch2 is fetched in `db/CMakeLists.txt` already (guarded by `if(NOT TARGET Catch2::Catch2)`), this should work, but the `include(Catch)` module path comes from the Catch2 package -- verify it resolves correctly.

**Detection:** Run `ctest --test-dir build -N | wc -l` before and after relocation. Must show the same test count (currently 284).

### Pitfall 5: Sync resumption cursors stored in PeerInfo are lost on reconnect

**What goes wrong:** The natural place to store per-peer sync cursors is in the `PeerInfo` struct, which is created in `on_peer_connected()` and destroyed in `on_peer_disconnected()`. Every reconnection resets cursors to zero, causing a full re-sync. For a node with thousands of blobs, this means every reconnect triggers a full hash-list exchange, defeating the purpose of resumption.

**Why it happens:** `PeerInfo` is connection-scoped by design (it tracks the live connection's state). Sync cursors need to survive across connections. But PeerInfo is keyed by connection pointer, not by peer identity.

**Consequences:** Sync resumption only helps for long-lived connections. For the common case (nodes that reconnect periodically, e.g., after network hiccups or restarts), every reconnect triggers a full sync. The optimization provides zero benefit in the most common scenario.

**Prevention:**
- Store cursors separately from PeerInfo, keyed by peer public key (the stable peer identity known after handshake).
- Use an in-memory map: `std::map<std::vector<uint8_t>, PeerSyncCursor> sync_cursors_` in PeerManager, keyed by peer_pubkey.
- On connect: look up existing cursor for this peer's pubkey. On disconnect: keep cursor alive.
- Optionally persist cursors to a new libmdbx sub-database for survival across node restarts.
- Bound the cursor map size (e.g., LRU eviction for peers not seen in 24h) to prevent unbounded memory growth from transient peers.

**Detection:** Log "full sync" vs "resumption sync" at sync start. If production logs show 100% full syncs, cursors are being lost on reconnect.

### Pitfall 6: Quota tracking with DARE requires decryption to count size

**What goes wrong:** Namespace storage quotas need to track the total size of blobs per namespace. But blobs are stored encrypted (DARE). The stored size includes the encryption envelope overhead (29 bytes: 1 version + 12 nonce + 16 tag). Counting stored bytes gives an inflated number. Counting original bytes requires decrypting every blob to check its size, which defeats the purpose of a fast quota check.

**Why it happens:** The current `used_bytes()` function returns `mi_geo.current` (total database file size), which is a single O(1) call. Per-namespace size tracking requires scanning all blobs in a namespace or maintaining a separate counter.

**Consequences:** If you use stored size, quotas are inaccurate (29 bytes overhead per blob). For many small blobs (e.g., 100-byte blobs), the overhead is 29% and quotas are meaningfully wrong. If you decrypt to check original size, quota checking becomes expensive and blocks writes.

**Prevention:**
- Maintain a per-namespace counter in a new libmdbx sub-database: `namespace_stats` with key `[namespace:32]` and value `[blob_count:u64BE][total_data_bytes:u64BE]`. Update atomically inside `store_blob()` and `delete_blob_data()` write transactions.
- Track original (pre-encryption) data size, not stored size. The blob data size is known at `store_blob()` time before encryption happens.
- Alternatively, track blob count only (simpler) and defer size quotas to a later milestone. Count quotas are simpler, cheaper, and sufficient for most use cases.
- Do NOT scan blobs_map to compute namespace sizes on demand. With 100 MiB blobs, even reading index entries is O(n).

**Detection:** Write a test that stores blobs, checks quota usage, then verifies the reported size matches the sum of original blob data sizes (not encrypted sizes).

### Pitfall 7: Removing the standalone benchmark binary breaks Docker benchmarks

**What goes wrong:** The cleanup pass identifies `bench/bench_main.cpp` as a "stale standalone benchmark" that should be removed since v0.6.0 added the Docker-based benchmark suite (`deploy/`). But the `chromatindb_bench` target may be referenced by other tooling, scripts, or manual workflows. Removing it without checking all references breaks things.

**Why it happens:** "Remove stale artifacts" is deceptively risky. The benchmark binary was there before the Docker suite and may have been used in ways not documented. The Dockerfile, compose files, or user scripts might reference it.

**Consequences:** Build scripts or CI that reference `chromatindb_bench` fail. Users who run local benchmarks (without Docker) lose the ability to do so.

**Prevention:**
- Before removing: `grep -r 'chromatindb_bench' .` to find all references.
- Check the Dockerfile for any `COPY` or `RUN` that references the bench binary.
- Check the `deploy/` scripts for any reference.
- If removing: remove from CMakeLists.txt, delete the source file, and update any references in a single atomic commit.
- Consider whether local (non-Docker) benchmarking is still valuable. If yes, keep it or migrate it.

**Detection:** CI build fails after removal. `grep -r` before removing catches this.

### Pitfall 8: Cleanup pass deletes code that tests depend on

**What goes wrong:** A cleanup pass removes unused code, updates headers, or reorganizes files. But "unused" is determined by searching the production code, not the test code. A helper function that appears unused in `db/` may be exercised by tests in `tests/`. Removing it breaks compilation only when `BUILD_TESTING=ON`.

**Why it happens:** Tests are in a separate directory (`tests/`) and link against `chromatindb_lib`. They can use any public API. A function that has no callers in `db/*.cpp` may have callers in `tests/*.cpp`. Cleanup tools and manual `grep` searches may only check the library source, not the test source.

**Consequences:** Production build succeeds, test build fails. If CI only builds without testing (or tests are temporarily disabled during relocation), the breakage is silent until someone enables tests.

**Prevention:**
- Always search both `db/` and `tests/` when checking for usage: `grep -r 'function_name' db/ tests/`.
- Build with `BUILD_TESTING=ON` after every cleanup change, not just at the end.
- Run the full test suite (`ctest --test-dir build`) after cleanup, not just compilation.
- In the test relocation phase, do relocation BEFORE cleanup. That way tests are in `db/tests/` and a search of `db/` catches test usage automatically.

**Detection:** CI with `BUILD_TESTING=ON` catches this immediately.

## Minor Pitfalls

Inconveniences and rough edges that waste time but are easy to fix.

### Pitfall 9: Seq_map gaps confuse sync cursor logic

**What goes wrong:** The current storage layer leaves gaps in seq_map when blobs are deleted via tombstones (`delete_blob_data()` erases the seq_map entry). If sync cursors are based on "last seq_num seen," a gap doesn't break anything -- the cursor just skips past deleted entries. But if the cursor tracks "highest seq_num successfully synced" and compares it to the peer's `latest_seq_num` (from `list_namespaces()`), gaps cause over-syncing: the cursor shows seq=45 but the latest is seq=50, so it tries to sync 5 blobs, but 3 of those seq entries are deleted, returning only 2 blobs. Not a bug, but the log says "synced 2/5 expected" which looks like an error.

**Prevention:** Treat seq gaps as normal. Log "synced N blobs from namespace X" without comparing to expected count. Document that seq_map has gaps by design.

### Pitfall 10: Thread pool for crypto must not hold libmdbx read transactions

**What goes wrong:** If crypto verification is offloaded to a thread pool and the verification function reads from storage (e.g., checking delegation existence via `storage_.has_valid_delegation()`), the read transaction opened on the thread pool thread can block libmdbx write transactions on the io_context thread. libmdbx read transactions prevent page recycling, and long-running reads on a thread pool (blocked behind a queue of verification jobs) can exhaust free database space.

**Prevention:** Never access storage from the thread pool. Read all needed data (delegation status, tombstone status) on the io_context thread before offloading to the thread pool. Pass the pre-read data as function arguments.

### Pitfall 11: db/ README update references old paths or APIs

**What goes wrong:** The `db/README.md` update during cleanup references file paths, API signatures, or configuration options that were changed during the same milestone. If README is updated early in the milestone, subsequent phases (quota addition, sync resumption) change the APIs and the README is stale before the milestone ships.

**Prevention:** Update README as the LAST task in the cleanup phase, after all other phases are complete. Or: do not update README until the milestone audit.

### Pitfall 12: Deletion benchmark adds load generator complexity

**What goes wrong:** Adding deletion benchmarks to the Docker suite requires the load generator to support a "delete after write" workflow. This means the loadgen needs to (1) store namespace keypairs to sign delete requests, (2) track which blobs it wrote so it can target them for deletion, and (3) handle the tombstone creation protocol (sign the tombstone, not just the blob). This is significantly more complex than the current "sign and send blob" flow.

**Prevention:** Keep the deletion benchmark simple: write N blobs, wait for sync, then delete all N. Do not try to interleave writes and deletes -- that requires tracking per-blob state in the loadgen. The sequential approach is easy to implement and still measures the deletion hot path (tombstone creation + sync propagation).

### Pitfall 13: Include path changes after test relocation

**What goes wrong:** Test files currently use includes like `#include "db/crypto/hash.h"`. If tests are moved from `tests/crypto/test_hash.cpp` to `db/tests/crypto/test_hash.cpp`, the include paths might break depending on how `target_include_directories` is configured. The library target exports `$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>` which resolves to the repo root -- includes starting with `db/` work from that root. But if the test target's include path is configured differently, paths break.

**Prevention:** Verify that the relocated test target has the same include directories as the original. The simplest approach: link the new test target against `chromatindb_lib` (which transitively provides the include path via PUBLIC) and `Catch2::Catch2WithMain`. Do NOT add extra `target_include_directories` -- the library dependency provides everything.

**Detection:** Compile the test target after relocation. If includes fail, the error is immediate and obvious.

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Mitigation |
|-------------|---------------|------------|
| Sync resumption | Cursor staleness after deletions (Pitfall 1) | Cursor as hint only, periodic full diff fallback |
| Sync resumption | Cursor lost on reconnect (Pitfall 5) | Key cursors by peer pubkey, not connection |
| Namespace quotas | Check-then-act race (Pitfall 2) | Enforce inside libmdbx write transaction |
| Namespace quotas | DARE size inflation (Pitfall 6) | Track pre-encryption size in separate sub-database |
| Crypto optimization | Thread pool nonce desync (Pitfall 3) | Post results back to io_context before protocol action |
| Crypto optimization | Thread pool reads blocking writes (Pitfall 10) | Pre-read all storage data before offloading |
| Test relocation | CTest discovery breaks (Pitfall 4) | Verify `ctest -N` count matches before/after |
| Test relocation | Include path changes (Pitfall 13) | Link against chromatindb_lib for transitive includes |
| Deletion benchmarks | Loadgen complexity explosion (Pitfall 12) | Sequential write-then-delete, no interleaving |
| Cleanup pass | Removing code tests depend on (Pitfall 8) | Search db/ AND tests/ for usage |
| Cleanup pass | Removing bench binary (Pitfall 7) | grep all references before deletion |
| Cleanup pass | README staleness (Pitfall 11) | Update README last, after all other phases |

## Integration Pitfalls

These emerge from feature interactions, not individual features.

### Sync resumption + namespace quotas

If a peer is over quota for a namespace and receives synced blobs for that namespace, the quota enforcement must reject the synced blobs. But the sync cursor might advance past those blobs, causing them to never be retried. Prevention: do not advance the cursor past rejected blobs. Only advance to the seq_num of the last successfully stored blob.

### Crypto optimization + sync resumption

If crypto verification is offloaded to a thread pool and sync uses cursor-based resumption, the cursor advance happens after verification completes (asynchronously). If two blobs from the same namespace are verified concurrently and the second finishes first, the cursor could advance past the first (still-pending) blob. Prevention: cursor advances must be serialized on the io_context thread, in blob order, not completion order.

### Cleanup + test relocation

If cleanup (removing stale code) and test relocation happen in the same milestone, the order matters critically. Relocate tests first, then clean up. If cleanup removes code that tests reference, and the tests haven't been relocated yet, the cleanup appears correct (tests are in a separate directory and may not be checked). After relocation, the removed code's absence breaks compilation.

### Namespace quotas + DARE + expiry

When a blob expires and is purged by `run_expiry_scan()`, the namespace quota counter must be decremented. But `run_expiry_scan()` currently does not know about namespace quotas -- it just deletes from blobs_map and expiry_map. The quota counter update must be added to the expiry scan path, not just the explicit deletion path.

## Sources

- [libmdbx cursor/transaction issues](https://github.com/erthink/libmdbx/issues/272) - Cursor invalidation when reusing across transactions
- [libmdbx documentation](https://libmdbx.dqdkfa.ru/intro.html) - Read transaction pitfalls: long-running reads prevent page recycling
- [Catch2 CMake integration](https://github.com/catchorg/Catch2/blob/devel/docs/cmake-integration.md) - catch_discover_tests requirements
- [Catch2 issue #2914](https://github.com/catchorg/Catch2/issues/2914) - "No tests were found" after CMake changes
- [Asio C++20 coroutine support](https://think-async.com/Asio/asio-1.22.0/doc/asio/overview/core/cpp20_coroutines.html) - co_spawn context model
- [Asio thread pool + coroutines](https://github.com/chriskohlhoff/asio/issues/1508) - Offloading CPU work from io_context
- [ML-DSA performance analysis](https://arxiv.org/pdf/2601.17785) - ML-DSA-87 verification throughput characteristics
- [PQMagic ML-DSA optimization](https://link.springer.com/chapter/10.1007/978-3-032-01806-9_9) - 2.04x verify speedup via instruction optimization
- [Quota enforcement in distributed storage](https://ieeexplore.ieee.org/abstract/document/4367965) - Asynchronous quota enforcement patterns
- chromatindb benchmark report (deploy/results/REPORT.md) - 15.3 blobs/sec at 1 MiB, 96.48% CPU on node2 during sync verification
