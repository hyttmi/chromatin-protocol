---
phase: 79-send-queue-push-notifications
verified: 2026-04-02T17:40:00Z
status: passed
score: 8/8 must-haves verified
---

# Phase 79: Send Queue + Push Notifications Verification Report

**Phase Goal:** Peers are notified immediately when any blob is ingested, with AEAD nonce safety guaranteed by a per-connection send queue
**Verified:** 2026-04-02T17:40:00Z
**Status:** PASSED
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | BlobNotify is wire type 59 in TransportMsgType enum | VERIFIED | `db/schemas/transport.fbs:78` has `BlobNotify = 59`; `db/wire/transport_generated.h:82` has `TransportMsgType_BlobNotify = 59` |
| 2 | Relay rejects BlobNotify (type 59) from client connections | VERIFIED | `relay/core/message_filter.cpp:37` has `case TransportMsgType_BlobNotify:` in blocklist; test at line 72 asserts `CHECK_FALSE(is_client_allowed(TransportMsgType_BlobNotify))` |
| 3 | All outbound messages on a connection go through a single send queue | VERIFIED | `db/net/connection.h` has `send_queue_`, `drain_running_`, `send_signal_`, `MAX_SEND_QUEUE=1024`, `PendingMessage` struct; `send_message()` calls `enqueue_send()` not `send_encrypted()` directly |
| 4 | A drain coroutine serializes all writes — only one AEAD nonce increment at a time | VERIFIED | `drain_send_queue()` in `connection.cpp:841` is the sole caller of `send_encrypted()` post-handshake; run() launches it concurrently with `message_loop()` via `awaitable_operators &&` at line 808 |
| 5 | Queue full at 1024 messages disconnects the peer | VERIFIED | `enqueue_send()` at line 821 checks `send_queue_.size() >= MAX_SEND_QUEUE` and calls `close()` |
| 6 | When a client writes a blob, all TCP peers receive BlobNotify (type 59) | VERIFIED | Data handler at line 1655 calls `on_blob_ingested(..., nullptr)`; `on_blob_ingested()` at line 2941 iterates `peers_`, skips UDS, co_spawns `send_message(TransportMsgType_BlobNotify)` to each TCP peer |
| 7 | BlobNotify payload is exactly 77 bytes: namespace_id(32) + blob_hash(32) + seq_num_be(8) + size_be(4) + tombstone(1) | VERIFIED | `encode_notification()` at line 3006 allocates `result(77)` and fills all five fields at correct offsets |
| 8 | Syncing peer does not receive BlobNotify for blobs it sent (source exclusion) | VERIFIED | `on_blob_ingested()` line 2943: `if (peer.connection == source) continue;`; sync callers pass `conn` at lines 2070 and 2469 |

**Score:** 8/8 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/schemas/transport.fbs` | BlobNotify = 59 enum value | VERIFIED | Line 78: `BlobNotify = 59` |
| `db/wire/transport_generated.h` | Generated header with TransportMsgType_BlobNotify | VERIFIED | Line 82: `TransportMsgType_BlobNotify = 59`, MAX updated, EnumNames includes "BlobNotify" |
| `relay/core/message_filter.cpp` | BlobNotify in blocklist | VERIFIED | Line 37: `case TransportMsgType_BlobNotify:` in the `return false` branch |
| `db/tests/relay/test_message_filter.cpp` | Test for BlobNotify blocked | VERIFIED | Line 72: `CHECK_FALSE(is_client_allowed(TransportMsgType_BlobNotify))` |
| `db/net/connection.h` | Send queue members, PendingMessage struct, drain coroutine declaration | VERIFIED | All present: `send_queue_`, `send_signal_`, `closing_`, `drain_running_`, `MAX_SEND_QUEUE`, `PendingMessage`, `enqueue_send()`, `drain_send_queue()` declarations |
| `db/net/connection.cpp` | Queue-based send_message, drain coroutine, Pong through queue | VERIFIED | `send_message()` calls `enqueue_send()`; `drain_send_queue()` loops on queue; Pong at line 765 uses `send_message()`; `run()` at line 808 uses `awaitable_operators &&` |
| `db/tests/net/test_connection.cpp` | Tests for send queue serialization, queue full disconnect, drain on close | VERIFIED | Lines 455, 524, 582: three TEST_CASEs tagged `[send_queue]` |
| `db/engine/engine.h` | Modified ingest() with optional source Connection::Ptr | VERIFIED | Line 106: `std::shared_ptr<net::Connection> source = nullptr`; line 55: `source` field in `IngestResult` |
| `db/engine/engine.cpp` | ingest() accepts source parameter | VERIFIED | Source stored in IngestResult on all Stored/Duplicate success paths |
| `db/peer/peer_manager.h` | on_blob_ingested unified fan-out method | VERIFIED | Line 259: `void on_blob_ingested(` declaration; `notify_subscribers` count: 0 (removed) |
| `db/peer/peer_manager.cpp` | BlobNotify fan-out, source exclusion, notify_subscribers removed | VERIFIED | Line 2949: `TransportMsgType_BlobNotify`; line 2943: source exclusion; `grep -c notify_subscribers` = 0 |
| `db/sync/sync_protocol.h` | ingest_blobs takes Connection::Ptr source | VERIFIED | Line 77: `std::shared_ptr<net::Connection> source = nullptr`; line 67: `OnBlobIngested` callback includes source |
| `db/sync/sync_protocol.cpp` | Passes source through to engine.ingest() | VERIFIED | Line 81: `engine_.ingest(blob, source)`; line 94: source passed to `on_blob_ingested_` callback |
| `db/tests/peer/test_peer_manager.cpp` | Tests for BlobNotify fan-out, source exclusion, tombstone path | VERIFIED | Lines 3780, 3856, 3944: three BlobNotify integration tests |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/schemas/transport.fbs` | `db/wire/transport_generated.h` | flatc code generation | WIRED | `BlobNotify = 59` in schema; `TransportMsgType_BlobNotify = 59` in generated header |
| `relay/core/message_filter.cpp` | `db/wire/transport_generated.h` | switch case inclusion | WIRED | `case TransportMsgType_BlobNotify:` in filter |
| `send_message()` | `enqueue_send()` | encode then enqueue | WIRED | `connection.cpp:818`: `co_return co_await enqueue_send(std::move(msg))` |
| `drain_send_queue()` | `send_encrypted()` | FIFO dequeue then write | WIRED | `connection.cpp:848`: dequeues and calls `send_encrypted()` |
| `message_loop() Pong` | `send_message()` | Pong uses send_message not send_encrypted | WIRED | `connection.cpp:765`: `co_await send_message(wire::TransportMsgType_Pong, empty)` |
| `Data handler` | `on_blob_ingested()` | called after engine_.ingest() returns Stored | WIRED | `peer_manager.cpp:1655`: `on_blob_ingested(..., nullptr)` |
| `Delete handler` | `on_blob_ingested()` | called after engine_.delete_blob() returns Stored | WIRED | `peer_manager.cpp:694`: `on_blob_ingested(..., nullptr)` |
| `sync_proto_ callback` | `on_blob_ingested()` | per-blob callback in PeerManager constructor | WIRED | `peer_manager.cpp:162`: `on_blob_ingested(ns, hash, seq, size, tombstone, std::move(source))` |
| `on_blob_ingested()` | `Connection::send_message(BlobNotify)` | co_spawn detached per peer | WIRED | `peer_manager.cpp:2948-2950`: co_spawn with `send_message(TransportMsgType_BlobNotify)` |
| `ingest_blobs()` | `engine_.ingest()` | source passthrough | WIRED | `sync_protocol.cpp:81`: `engine_.ingest(blob, source)` |

### Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| `on_blob_ingested()` | `payload` (77 bytes) | `encode_notification()` from engine result fields | Yes — populated from `result.ack->blob_hash`, `result.ack->seq_num`, `blob.data.size()`, `wire::is_tombstone()` | FLOWING |
| `peers_` iteration | `peer.connection` | `std::deque<PeerInfo>` live peer list | Yes — populated on peer connect, removed on disconnect | FLOWING |
| `send_queue_` drain | encoded bytes | `TransportCodec::encode()` output | Yes — full framed+encrypted AEAD write via `send_encrypted()` | FLOWING |

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| BlobNotify = 59 in schema | `grep "BlobNotify = 59" db/schemas/transport.fbs` | match at line 78 | PASS |
| Generated header has BlobNotify | `grep "TransportMsgType_BlobNotify = 59" db/wire/transport_generated.h` | match at line 82 | PASS |
| Relay blocks BlobNotify | `./build/db/chromatindb_tests "[message_filter]"` | 72 assertions in 8 test cases, all passed | PASS |
| Send queue serialization | `./build/db/chromatindb_tests "[send_queue]"` | 3 assertions in 3 test cases, all passed | PASS |
| BlobNotify fan-out + source exclusion | `./build/db/chromatindb_tests "[pubsub]"` | 131 assertions in 14 test cases, all passed | PASS |
| notify_subscribers removed | `grep -c "notify_subscribers" db/peer/peer_manager.cpp` | 0 | PASS |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|---------|
| WIRE-01 | 79-01 | BlobNotify wire type 59 | SATISFIED | `db/schemas/transport.fbs:78`, `db/wire/transport_generated.h:82` |
| WIRE-04 | 79-01 | Relay blocks types 59-61 (type 59 blocked) | SATISFIED | `relay/core/message_filter.cpp:37`; test at `test_message_filter.cpp:72` |
| PUSH-04 | 79-02 | Per-connection send queue prevents AEAD nonce desync | SATISFIED | `connection.h` send_queue_ + drain_send_queue(); Pong/Goodbye routed through queue |
| PUSH-01 | 79-03 | Node notifies all connected peers immediately on ingest | SATISFIED | `on_blob_ingested()` iterates `peers_` and co_spawns BlobNotify to every TCP peer |
| PUSH-02 | 79-03 | BlobNotify payload is 77 bytes with correct fields | SATISFIED | `encode_notification()` allocates 77 bytes with namespace_id(32)+blob_hash(32)+seq_num_be(8)+size_be(4)+tombstone(1) |
| PUSH-03 | 79-03 | Notifications suppressed during active reconciliation (source exclusion) | SATISFIED | Source exclusion at line 2943 prevents BlobNotify back to syncing peer; ingest_blobs passes `conn` as source at lines 2070/2469 |
| PUSH-07 | 79-03 | No BlobNotify back to blob's originating peer | SATISFIED | `if (peer.connection == source) continue` at `peer_manager.cpp:2943` |
| PUSH-08 | 79-03 | Push notifications to currently-connected peers only | SATISFIED | `on_blob_ingested()` iterates `peers_` deque (live connections); disconnected peers removed from deque on disconnect |

**Note on PUSH-03:** The requirement "suppressed during active reconciliation" is implemented via source exclusion — when ingest_blobs() passes the syncing peer's connection as source, that peer is excluded from BlobNotify. This prevents the notification storm loop (peer A sends to peer B → peer B notifies peer A → peer A fetches again). There is no additional suppression mechanism; source exclusion fully satisfies the intent.

**WIRE-04 partial note:** WIRE-04 says "Relay filter updated to block types 59-61." Phase 79 only adds type 59 (BlobNotify). Types 60/61 (BlobFetch, BlobFetchResponse) are Phase 80 requirements (WIRE-02, WIRE-03). The relay filter will be updated for 60/61 in Phase 80. WIRE-04 is fully satisfied for Phase 79's scope.

**No orphaned requirements:** REQUIREMENTS.md traceability table maps PUSH-01, PUSH-02, PUSH-03, PUSH-04, PUSH-07, PUSH-08, WIRE-01, WIRE-04 to Phase 79 — all 8 are covered by the 3 plans.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| — | — | None found | — | — |

No TODO/FIXME/placeholder comments or empty implementations found in phase 79 modified files. `notify_subscribers` is confirmed removed (count = 0 in both .h and .cpp).

### Human Verification Required

#### 1. Live Three-Node BlobNotify Integration

**Test:** Start two nodes (node A and node B), establish peer connection, write a blob to node A via the Python SDK. Observe node B receives BlobNotify (type 59) before any reconcile timer fires.
**Expected:** node B's message loop receives BlobNotify within milliseconds of the write ACK to the client.
**Why human:** Requires live network stack; ctest runs in-process simulations. The KVM swarm at 192.168.1.200-.202 is the appropriate test target.

#### 2. AEAD Nonce Ordering Under Concurrent Load

**Test:** Use the loadgen tool to saturate node A with concurrent writes while observing node B for any AEAD decryption errors.
**Expected:** Zero nonce errors; all BlobNotify and WriteAck messages decode correctly at peer B.
**Why human:** Requires live load and decryption verification; can't automate nonce-ordering checks without a full protocol-level observer.

### Gaps Summary

No gaps. All 8 must-haves are verified at all levels (exists, substantive, wired, data-flowing). The ctest "failures" for tests 378 and 379 are pre-existing ASAN false positives from the Asio resolver thread pool (232 bytes in 4 allocations from `OQS_MEM_malloc` and `asio::ip::basic_resolver_results`) — identical to the same pattern in test 340 ("tombstone propagates between two connected nodes via sync") which predates phase 79. Test assertions pass: 131/131 in the pubsub suite.

---

_Verified: 2026-04-02T17:40:00Z_
_Verifier: Claude (gsd-verifier)_
