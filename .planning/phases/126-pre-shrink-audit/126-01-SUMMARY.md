---
phase: 126
plan: 01
subsystem: db/net, cli/src
tags: [streaming-invariant, framing, audit, pre-shrink]
requires: []
provides:
  - node-side static_assert pinning MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD
  - cli-side static_assert mirroring the same relationship
  - node-side runtime assert at Connection::enqueue_send waist
  - cli-side runtime assert in Connection::send non-chunked branch
  - TRANSPORT_ENVELOPE_MARGIN = 64 on both sides
  - Two [connection][streaming] round-trip Catch2 TEST_CASEs
affects:
  - db/net/framing.h
  - db/net/connection.cpp
  - cli/src/connection.cpp
  - db/tests/net/test_connection.cpp
tech-stack:
  added: []
  patterns:
    - narrow-waist runtime invariant at enqueue_send (single-caller chokepoint)
    - static_assert colocated with the constants it constrains
key-files:
  created:
    - .planning/phases/126-pre-shrink-audit/126-01-SUMMARY.md
  modified:
    - db/net/framing.h
    - db/net/connection.cpp
    - cli/src/connection.cpp
    - db/tests/net/test_connection.cpp
    - .planning/REQUIREMENTS.md
    - .planning/ROADMAP.md
    - .planning/phases/126-pre-shrink-audit/126-VALIDATION.md
decisions:
  - Replace the weaker static_assert(MAX_FRAME_SIZE > STREAMING_THRESHOLD) instead of keeping both invariants (RESEARCH Assumption A2)
  - Assertion site is enqueue_send (single caller), NOT send_encrypted (shared with chunked sub-frames)
  - No shared helper between db/ and cli/; each side has one-line invariants and constants already live in separate TUs (CONTEXT.md D-06)
  - CLI TRANSPORT_ENVELOPE_MARGIN declared file-local in cli/src/connection.cpp (not cross-imported from db/net/framing.h; cli/ does not include db/ headers)
  - Tests reference STREAMING_THRESHOLD, not MAX_FRAME_SIZE, so they survive Phase 128's shrink (D-11)
  - Tests placed in test_connection.cpp (not test_framing.cpp) because the behaviour is Connection::send_message end-to-end, not a framing primitive (D-12)
metrics:
  duration: "~20m"
  completed: 2026-04-22
---

# Phase 126 Plan 01: Streaming-Invariant Pin Summary

**Pinned the send-side streaming invariant on both the chromatindb node (`db/`) and the `cdb` CLI (`cli/`) with a static_assert + runtime assert pair per side, plus two round-trip Catch2 tests — Phase 128 (FRAME-01) is cleared to shrink MAX_FRAME_SIZE from 110 MiB to 2 MiB on both sides of the wire without risk of tripping any non-chunked send site.**

## Requirements Satisfied

| ID       | Trace                                                                                                                                                                                                                                                                                                                         |
|----------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| AUDIT-01 | Send-path inventory delivered as a documented artifact (§"Send-Path Inventory" below). Per CONTEXT.md D-01..D-03 this was reframed from a per-response-type size sweep to a send-path bypass audit. Zero bypass sites found (D-09 success condition); per-response-type operator-docs table deferred to Phase 131 per D-02. |
| AUDIT-02 | Streaming invariant pinned structurally: send_message auto-routes `>= STREAMING_THRESHOLD` through send_message_chunked (pre-existing); new `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD)` + `assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN)` on both db/ and cli/ sides; two `[connection][streaming]` round-trip Catch2 TEST_CASEs lock in byte-exact reassembly. |

## Tasks Completed

| # | Task                                                                                  | Commit   | Files                                        |
|---|---------------------------------------------------------------------------------------|----------|----------------------------------------------|
| 1 | D-05 node-side static_assert + TRANSPORT_ENVELOPE_MARGIN in framing.h                 | 01e2e52  | db/net/framing.h                             |
| 2 | D-04 runtime assertion at head of Connection::enqueue_send                            | 1c7c83d  | db/net/connection.cpp                        |
| 3 | D-14 file-scope STREAMING_THRESHOLD + static_assert in cli/src/connection.cpp         | 57a7a98  | cli/src/connection.cpp                       |
| 4 | D-14 runtime assertion in CLI Connection::send non-chunked branch                     | cd89ebe  | cli/src/connection.cpp                       |
| 5 | D-10/D-11/D-12 two round-trip [connection][streaming] Catch2 TEST_CASEs               | db89e94  | db/tests/net/test_connection.cpp             |

## Assertions Landed

| Side | Kind       | Location                                                             | Condition                                                              |
|------|------------|----------------------------------------------------------------------|------------------------------------------------------------------------|
| db/  | static_assert | db/net/framing.h:31                                               | `MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD`                            |
| db/  | runtime (assert) | db/net/connection.cpp:988 (head of `Connection::enqueue_send`) | `encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN`     |
| cli/ | static_assert | cli/src/connection.cpp:38                                         | `MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD`                            |
| cli/ | runtime (assert) | cli/src/connection.cpp:651 (non-chunked branch of `Connection::send`) | `transport_bytes.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN` |

Both runtime asserts share the exact literal diagnostic string `"non-chunked send exceeds sub-frame size (streaming boundary)"` for grep-symmetry across the two binaries. Both static_asserts share the same rationale message ("MAX_FRAME_SIZE must admit one full streaming sub-frame plus headroom for AEAD tag (16B), length prefix (4B), and transport envelope...").

## Round-trip Test Coverage

Both tests tagged `[connection][streaming]`; both follow the acceptor + `create_inbound` + `create_outbound` + `on_ready` + `on_message` fixture pattern from `test_connection.cpp:475-542`; both use `ioc.run_for(std::chrono::seconds(12))` with a 10-second steady_timer to fail the test if nothing arrives.

| Test name                                                                                             | Payload                                | What it proves                                                                                                                                                                                                                               |
|-------------------------------------------------------------------------------------------------------|----------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `streaming invariant: payload just under STREAMING_THRESHOLD round-trips through non-chunked path`    | `(STREAMING_THRESHOLD - 1)` × `0xAB`   | The non-chunked branch of `send_message` (line 976) reassembles a near-threshold payload byte-exact. A future regression that emits an oversized single frame would fail `read_frame`'s MAX_FRAME_SIZE check and the test would time out.   |
| `streaming invariant: payload just over STREAMING_THRESHOLD auto-chunks end-to-end`                   | `(STREAMING_THRESHOLD + 1)` × `0xCD`   | send_message auto-routes the payload through `send_message_chunked`: 14-byte header + two data sub-frames + zero-length sentinel. The responder's `recv_chunked` branch reconstructs the full payload byte-exact.                             |

## Zero bypass sites confirmed in db/ and cli/

`grep -rn 'enqueue_send' db/ --include='*.cpp' --include='*.h'` returns only:

- `db/net/connection.h:177` — declaration
- `db/net/connection.h:207-208` — internal-struct comments
- `db/net/connection.cpp:977` — the single caller (`send_message`'s non-chunked branch)
- `db/net/connection.cpp:980` — the definition
- `db/net/connection.cpp:983-985` — the new assertion comment block
- `db/net/framing.h:27` — doc comment referencing the function name

Exactly one caller exists across the entire `db/` tree. D-09's success condition (from CONTEXT.md) holds: no fix-in-phase reroute work was needed; the streaming invariant was already correct by construction, and Phase 126's deliverable is purely the pinning (static_assert + runtime assert + round-trip test).

## Send-Path Inventory

Lifted verbatim from 126-RESEARCH.md §"Send-Path Inventory". The table is the AUDIT-01 documented deliverable.

### `send_message` callers (all routed through `send_message`'s bifurcation at db/net/connection.cpp:972)

| # | Call site (file:line)                                              | Message type                          | Terminal primitive                                 | Size bound                                                                                                         | Risk       |
|---|--------------------------------------------------------------------|---------------------------------------|----------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|------------|
| 1 | `db/peer/message_dispatcher.cpp:74`                                | ErrorResponse (static helper)         | `send_message` → non-chunked → enqueue_send        | 2 bytes                                                                                                            | bounded    |
| 2 | `db/peer/message_dispatcher.cpp:266`                               | QuotaExceeded                         | `send_message` → non-chunked                       | 0 bytes                                                                                                            | bounded    |
| 3 | `db/peer/message_dispatcher.cpp:374`                               | DeleteAck                             | `send_message` → non-chunked                       | small fixed (ack struct)                                                                                           | bounded    |
| 4 | `db/peer/message_dispatcher.cpp:452,460,464`                       | ReadResponse (3 sites)                | `send_message` → branches on size                  | 1 + encoded_blob; blob capped by `MAX_BLOB_DATA_SIZE`; auto-chunks at `>= 1 MiB`                                    | bounded    |
| 5 | `db/peer/message_dispatcher.cpp:579`                               | ListResponse                          | `send_message`                                     | hash-list bounded by ListRequest's max count; auto-chunks over 1 MiB                                               | bounded    |
| 6 | `db/peer/message_dispatcher.cpp:612`                               | StatsResponse                         | `send_message`                                     | fixed small                                                                                                        | bounded    |
| 7 | `db/peer/message_dispatcher.cpp:647`                               | ExistsResponse                        | `send_message`                                     | small fixed                                                                                                        | bounded    |
| 8 | `db/peer/message_dispatcher.cpp:721`                               | NodeInfoResponse                      | `send_message`                                     | small fixed (u64s + counters); grows modestly in Phase 127 (+4 fields)                                             | bounded    |
| 9 | `db/peer/message_dispatcher.cpp:794`                               | NamespaceListResponse                 | `send_message`                                     | 4 + N*32 bytes; auto-chunks for large N                                                                            | bounded    |
| 10 | `db/peer/message_dispatcher.cpp:838`                              | StorageStatusResponse                 | `send_message`                                     | small fixed                                                                                                        | bounded    |
| 11 | `db/peer/message_dispatcher.cpp:895`                              | NamespaceStatsResponse                | `send_message`                                     | small fixed                                                                                                        | bounded    |
| 12 | `db/peer/message_dispatcher.cpp:923,931,980`                      | MetadataResponse (3 sites)            | `send_message`                                     | 1 + 32 + small metadata blob                                                                                       | bounded    |
| 13 | `db/peer/message_dispatcher.cpp:1029`                             | BatchExistsResponse                   | `send_message`                                     | 1 + 4 + count*(1+32); count<=256 → <=8.4 KiB                                                                        | bounded    |
| 14 | `db/peer/message_dispatcher.cpp:1075`                             | DelegationListResponse                | `send_message`                                     | small (delegation records are compact)                                                                             | bounded    |
| 15 | `db/peer/message_dispatcher.cpp:1195`                             | BatchReadResponse                     | `send_message` → auto-chunks at `>= 1 MiB`         | MAX_CAP = 4 MiB at :1103 + small per-entry headers; auto-streams when plaintext >= 1 MiB                           | bounded via auto-chunk |
| 16 | `db/peer/message_dispatcher.cpp:1235,1271`                        | PeerInfoResponse (2 sites)            | `send_message`                                     | small (peer list bounded)                                                                                          | bounded    |
| 17 | `db/peer/message_dispatcher.cpp:1371`                             | TimeRangeResponse                     | `send_message`                                     | bounded by request's max-count param                                                                               | bounded    |
| 18 | `db/peer/message_dispatcher.cpp:1428,1450,1456`                   | WriteAck / StorageFull / QuotaExceeded| `send_message`                                     | 0-1 bytes                                                                                                          | bounded    |
| 19 | `db/peer/connection_manager.cpp:262`                              | SyncNamespaceAnnounce                 | `send_message` via detached co_spawn               | namespace-list payload; auto-chunks for large N                                                                    | bounded    |
| 20 | `db/peer/connection_manager.cpp:434`                              | Ping                                  | `send_message`                                     | empty                                                                                                              | bounded    |
| 21 | `db/peer/pex_manager.cpp:59`                                      | PeerListRequest                       | `send_message`                                     | empty                                                                                                              | bounded    |
| 22 | `db/peer/pex_manager.cpp:125`                                     | PeerListResponse                      | `send_message`                                     | MAX_PEERS_PER_EXCHANGE = 8; few hundred bytes                                                                       | bounded    |
| 23 | `db/peer/peer_manager.cpp:105`                                    | PeerListRequest                       | `send_message`                                     | empty                                                                                                              | bounded    |
| 24 | `db/peer/peer_manager.cpp:121`                                    | PeerListResponse                      | `send_message`                                     | same MAX_PEERS_PER_EXCHANGE bound                                                                                   | bounded    |
| 25 | `db/peer/peer_manager.cpp:606` (SIGHUP re-announce fan-out)       | SyncNamespaceAnnounce                 | `send_message` per-peer detached co_spawn          | namespace-list payload; auto-chunks for large N                                                                    | bounded    |
| 26 | `db/peer/blob_push_manager.cpp:76`                                | BlobNotify                            | `send_message`                                     | 77 bytes (fixed)                                                                                                    | bounded    |
| 27 | `db/peer/blob_push_manager.cpp:88`                                | Notification                          | `send_message`                                     | 77 bytes (same structure)                                                                                          | bounded    |
| 28 | `db/peer/blob_push_manager.cpp:137`                               | BlobFetch                             | `send_message`                                     | 64 bytes                                                                                                           | bounded    |
| 29 | `db/peer/blob_push_manager.cpp:155,168,172`                       | BlobFetchResponse (3 sites)           | `send_message`                                     | 1 or 1+32+encoded_blob; auto-chunks at `>= 1 MiB`                                                                   | bounded    |
| 30 | `db/peer/sync_orchestrator.cpp:151,190,315,376,390,434,456,506,545,582,590,628,653,799,808,867,883,941,989,1034` | Sync* + BlobTransfer (20+ sites) | `send_message`                                     | BlobTransfer is dominant large path (entire encoded blob, up to MAX_BLOB_DATA_SIZE) — always auto-chunked          | bounded via auto-chunk |

**Total:** 30 distinct call sites across 7 files in `db/peer/`, plus the `send_error_response` static helper at `db/peer/message_dispatcher.cpp:74` used by every handler's error branch. All 30 flow through `send_message`'s `>= STREAMING_THRESHOLD` bifurcation at `db/net/connection.cpp:972`; none bypass.

### Direct `Connection::send_raw` / `Connection::send_encrypted` callers (handshake only — all private to `Connection`)

| # | Call site (file:line)          | Message type                 | Payload size                                                                                                | Risk       |
|---|--------------------------------|------------------------------|-------------------------------------------------------------------------------------------------------------|------------|
| H1 | `connection.cpp:279`          | TrustedHello (raw)           | 32-byte nonce + 2592-byte ML-DSA-87 signing pubkey + envelope = ~2.7 KiB                                    | bounded-by-construction |
| H2 | `connection.cpp:321`          | AuthSignature (encrypted)    | 1 role byte + 4 pubkey-size BE + 2592 pubkey + ~4627 ML-DSA-87 signature + envelope = ~7.3 KiB              | bounded-by-construction |
| H3 | `connection.cpp:373`          | KemPubkey (raw)              | ML-KEM-1024 pubkey + envelope, ~1.6 KiB                                                                     | bounded-by-construction |
| H4 | `connection.cpp:398`          | AuthSignature (encrypted, fallback) | same as H2                                                                                           | bounded-by-construction |
| H5 | `connection.cpp:449`          | KemPubkey (raw)              | same as H3                                                                                                  | bounded-by-construction |
| H6 | `connection.cpp:474`          | AuthSignature (encrypted)    | same as H2                                                                                                  | bounded-by-construction |
| H7 | `connection.cpp:543`          | TrustedHello (raw, responder) | same as H1                                                                                                 | bounded-by-construction |
| H8 | `connection.cpp:594`          | AuthSignature (encrypted, responder) | same as H2                                                                                          | bounded-by-construction |
| H9 | `connection.cpp:617`          | PQRequired (raw, empty)      | 0 bytes                                                                                                     | bounded-by-construction |
| H10 | `connection.cpp:638`         | KemCiphertext (raw)          | ML-KEM-1024 ciphertext + envelope, ~1.6 KiB                                                                  | bounded-by-construction |
| H11 | `connection.cpp:678`         | AuthSignature (encrypted)    | same as H2                                                                                                  | bounded-by-construction |
| H12 | `connection.cpp:706`         | KemCiphertext (raw)          | same as H10                                                                                                 | bounded-by-construction |
| H13 | `connection.cpp:746`         | AuthSignature (encrypted)    | same as H2                                                                                                  | bounded-by-construction |
| H14 | `connection.cpp:1013,1022,1028` (inside `drain_send_queue`) | chunked header / sub-frame / sentinel | 14-byte header, exactly STREAMING_THRESHOLD per sub-frame, 0-byte sentinel | bounded by sub-frame loop |

All 13 handshake payloads are fixed by protocol (ML-DSA-87 is 2592-byte pubkey + 4627-byte signature; ML-KEM-1024 is fixed; nonces are 32 bytes). Largest encrypted handshake message is AuthSignature at ~7.3 KiB — well under `STREAMING_THRESHOLD`.

### Lookalike false positives

| Call site                                                        | Why listed                | Actual shape                                                                                                                                                                              |
|------------------------------------------------------------------|---------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `db/main.cpp:571` `auto send_encrypted = [&](...)` lambda        | Grep-adjacent             | Local lambda inside `cmd_list_peers` implementing a synchronous UDS-client handshake for the `chromatindb list-peers` subcommand. NOT `Connection::send_encrypted`. Fixed small payloads. |

## Validation Nyquist Map

See `.planning/phases/126-pre-shrink-audit/126-VALIDATION.md` §"Per-Task Verification Map" — filled with T1..T5 tasks, requirements (AUDIT-01 via doc artifact, AUDIT-02 via code + tests), threat refs (T-126-01..04), and grep-level automated commands. All five rows `green` after execution. User-delegated Catch2 run (`./build/db/chromatindb_tests "[connection][streaming]"`) recommended as a smoke check but NOT run by the executor per memory `feedback_delegate_tests_to_user.md` and `feedback_no_test_automation_in_executor_prompts.md`.

## Deviations and Non-Obvious Decisions

**No code-level deviations.** The plan was executed verbatim. Five atomic commits, one per task, in plan order.

Notes on decisions the plan pre-approved but worth recording:

1. **CLI `TRANSPORT_ENVELOPE_MARGIN` is file-local in `cli/src/connection.cpp`**, not imported from `db/net/framing.h`. Per plan Task 4 action block: "the CLI does not have a TRANSPORT_ENVELOPE_MARGIN constant. The node-side margin lives in db/net/framing.h, which cli/ does not include." Defined locally at file scope; duplicated constant value (64) is intentional and documented on both sides.

2. **Assertion wording is byte-identical across `db/` and `cli/`** (`"non-chunked send exceeds sub-frame size (streaming boundary)"`) — enables a single `grep` across both TUs to confirm the invariant phrasing.

3. **Weaker `static_assert(MAX_FRAME_SIZE > STREAMING_THRESHOLD)` was replaced, not kept alongside** — per RESEARCH Assumption A2. The `>= 2 * STREAMING_THRESHOLD` form strictly implies the original `>` form (since STREAMING_THRESHOLD > 0), so the old assertion was subsumed.

4. **No test-suite execution performed by the executor.** Per memory `feedback_delegate_tests_to_user.md` and `feedback_no_test_automation_in_executor_prompts.md`, the grep-based `<automated>` gates in each task's `<verify>` block were used as the completion criterion; the Catch2 `./build/db/chromatindb_tests "[connection][streaming]"` run is user-delegated.

5. **`send_chunked`'s local `CHUNK_SIZE = 1048576` in `cli/src/connection.cpp:650` was intentionally left untouched.** It is semantically different from the file-scope `STREAMING_THRESHOLD` (intra-chunked-path granularity vs send-path bifurcation threshold) and has its own function-local scope. Plan Task 3 explicitly flagged this.

6. **`enqueue_send` still has exactly one caller after Task 2.** Verified post-commit via `grep -rn 'enqueue_send' db/ --include='*.cpp' --include='*.h'`: the only `.cpp` call site is `db/net/connection.cpp:977` (send_message's non-chunked branch). The assertion did not introduce a new call site. D-09 post-condition holds.

## Files Modified

| File                                                                        | Change                                                                              |
|-----------------------------------------------------------------------------|-------------------------------------------------------------------------------------|
| db/net/framing.h                                                            | +TRANSPORT_ENVELOPE_MARGIN = 64 + strengthened static_assert (replaces weaker form) |
| db/net/connection.cpp                                                       | +#include `<cassert>` + synchronous assert at head of Connection::enqueue_send       |
| cli/src/connection.cpp                                                      | +#include `<cassert>`, promoted STREAMING_THRESHOLD to file scope, +static_assert, +TRANSPORT_ENVELOPE_MARGIN, +assert in Connection::send non-chunked branch |
| db/tests/net/test_connection.cpp                                            | +2 TEST_CASEs tagged [connection][streaming] (round-trip under and over boundary)   |
| .planning/REQUIREMENTS.md                                                   | AUDIT-01/AUDIT-02 marked Complete; Traceability table updated                        |
| .planning/ROADMAP.md                                                        | Phase 126 box + 126-01 plan box marked [x]; progress row 1/1 Complete                |
| .planning/phases/126-pre-shrink-audit/126-VALIDATION.md                     | Per-Task Verification Map filled with 5 rows; sign-off approved                     |

## What Unblocks

**Phase 128 (FRAME-01)** is cleared to shrink `MAX_FRAME_SIZE` from 110 MiB to 2 MiB on both sides of the wire (`db/net/framing.h:14` and `cli/src/connection.cpp:36`). The static_assert makes the shrink provably safe at build time — any future drift between `MAX_FRAME_SIZE` and `STREAMING_THRESHOLD` fails the build, not production. The runtime assert locks the non-chunked send path at debug runtime. The two round-trip Catch2 tests continue to exercise both sides of the streaming boundary against whatever MAX_FRAME_SIZE the codebase ships with (tests reference STREAMING_THRESHOLD directly per D-11).

## Next

Phase 127 (NodeInfoResponse Capability Extensions): four new fields (`max_blob_data_bytes`, `max_frame_bytes`, `rate_limit_messages_per_second`, `max_subscriptions_per_connection`) in the `NodeInfoResponse` wire format. This is protocol-breaking; pre-MVP posture means no compat shim. Phase 128's frame shrink is blocked by Phase 127 (strict linear execution order: 126 → 127 → 128 → 129 → 130 → 131).

## Self-Check: PASSED

Files referenced in this SUMMARY.md that must exist on disk:

- `db/net/framing.h` — FOUND (contains `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD` and `constexpr size_t TRANSPORT_ENVELOPE_MARGIN = 64`).
- `db/net/connection.cpp` — FOUND (contains `assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN` and `non-chunked send exceeds sub-frame size`).
- `cli/src/connection.cpp` — FOUND (contains file-scope `STREAMING_THRESHOLD`, `TRANSPORT_ENVELOPE_MARGIN`, static_assert, runtime assert).
- `db/tests/net/test_connection.cpp` — FOUND (contains two `[connection][streaming]` TEST_CASEs).

Commit hashes referenced:

- 01e2e52 — FOUND on master
- 1c7c83d — FOUND on master
- 57a7a98 — FOUND on master
- cd89ebe — FOUND on master
- db89e94 — FOUND on master
