# Phase 126: Pre-shrink Audit - Research

**Researched:** 2026-04-22
**Domain:** Send-side AEAD frame pipeline / streaming invariant
**Confidence:** HIGH (all claims verified by direct read of db/net/connection.{h,cpp}, db/net/framing.{h,cpp}, and every caller under db/peer/ and db/main.cpp)

## Summary

The streaming invariant is already correct by construction in the current code. `Connection::send_message` at `db/net/connection.cpp:968-977` is the single public send API; it auto-routes any payload `>= STREAMING_THRESHOLD (1 MiB)` into `send_message_chunked`, which splits plaintext into `STREAMING_THRESHOLD`-sized sub-frames before any AEAD write. No non-chunked send path can produce a single ciphertext frame larger than `STREAMING_THRESHOLD + AEAD_TAG + envelope_overhead` (~1 MiB + 16 + ≤64 bytes). After Phase 128 shrinks `MAX_FRAME_SIZE` to 2 MiB, that upper bound still fits with ~1 MiB of headroom.

A complete `grep` over `db/` confirms **zero** external callers of `enqueue_send` (only caller is `send_message` itself) and **zero** callers of `send_encrypted` outside `Connection`'s own methods (one unrelated lambda by the same name exists in `db/main.cpp:571` for the `list-peers` UDS client; it is not `Connection::send_encrypted` and only transmits small fixed-size handshake payloads). Every higher-level send path in `db/peer/` (message_dispatcher, sync_orchestrator, blob_push_manager, pex_manager, peer_manager, connection_manager) routes through `conn->send_message(...)`.

**Primary recommendation:** Put the runtime assertion at the head of `Connection::enqueue_send` (`db/net/connection.cpp:979`) — it is the narrowest unambiguous waist through which every non-chunked send must pass, and it has exactly one caller. Pair with `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD + FRAME_OVERHEAD_MARGIN)` in `db/net/framing.h`. Add one round-trip test in `db/tests/net/test_connection.cpp` that exercises the boundary on both sides (`STREAMING_THRESHOLD - 1` and `STREAMING_THRESHOLD + 1`). No bypass fixes needed — the inventory found zero bypass sites (D-09 success condition).

## User Constraints (from CONTEXT.md)

### Locked Decisions

- **D-01** The audit subject is the send-side streaming invariant: every large payload must go through `Connection::send_message`'s `>= STREAMING_THRESHOLD` branch, which sub-frames at 1 MiB plaintext. No code path may construct a single frame with ciphertext `>= MAX_FRAME_SIZE`.
- **D-02** The original "per-response-type worst-case size table" work is deferred to Phase 131 (Documentation Reconciliation) as an operator-facing documentation deliverable. It is not a gate, because under the streaming invariant no response can produce an oversized frame by construction.
- **D-03** Reframe rationale: `db/net/connection.cpp:972` auto-streams any payload `>= STREAMING_THRESHOLD = 1 MiB` before the frame cap is consulted. So `MAX_FRAME_SIZE` is not protecting us from oversized responses — it is a DoS bound on the 4-byte length prefix (`u32 BE` allows declared sizes up to 4 GiB). The audit must pin the streaming invariant, not measure payloads.
- **D-04** Add a runtime assertion in the single-frame send primitive (`Connection::enqueue_send`'s non-chunked branch, or the lower-level `send_encrypted` — researcher to pick the exact site) that asserts `payload.size() < STREAMING_THRESHOLD`. Any future bypass produces a loud test failure.
- **D-05** Add a compile-time invariant in `db/net/framing.h`: `static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD)`. Trivially holds today (110 MiB >= 2 MiB); still holds after Phase 128 shrinks `MAX_FRAME_SIZE` to 2 MiB. Pins the numeric relationship so a future tweak to one without the other fails the build.
- **D-06** Do not adopt an AST/grep CI check for `enqueue_send` call sites. D-04 and D-05 together cover the call-site concern without the maintenance burden of pattern-matching.
- **D-07** The audit inventories every send-side code path that reaches the AEAD frame write — `send_message`, `send_message_chunked`, `enqueue_send`, `send_encrypted`, and any direct byte-pusher that skips the `send_message` wrapper. Each is documented with the path it takes.
- **D-08** If the inventory surfaces a call site that can emit a single frame `>= STREAMING_THRESHOLD` bypassing `send_message`, fix in this phase by rerouting through `send_message`. Expected to be a one-line change per site.
- **D-09** If the inventory surfaces zero bypass sites, that is the success condition — no fix work, just the assertions and the unit test. Phase 128's frame shrink lands with the invariant already pinned.
- **D-10** One round-trip unit test that covers both sides of the streaming boundary: a `STREAMING_THRESHOLD - 1` payload sent via `send_message` produces a single frame under the cap; a `STREAMING_THRESHOLD + 1` payload auto-streams into multiple sub-frames each under the cap. Both decrypt and decode correctly.
- **D-11** Tests reference `STREAMING_THRESHOLD` directly for the boundary value — not `MAX_FRAME_SIZE` — so the test is meaningful today (under the 110 MiB cap) and still meaningful after Phase 128 shrinks the cap.
- **D-12** Test location: colocated with existing framing tests (`db/tests/net/test_framing.cpp` or `db/tests/net/test_connection.cpp`). Researcher picks the better fit.
- **D-13** `MAX_FRAME_SIZE = 2 MiB` is NOT changed in Phase 126. The actual constant change lives in Phase 128 (FRAME-01). Phase 126's deliverable is "the streaming invariant is pinned and the audit found no bypass sites" — a green light for Phase 128 to shrink the constant safely.

### Claude's Discretion

- Exact location of the runtime assertion (`enqueue_send` vs `send_encrypted` vs a shared helper) — pick whichever is the narrowest waist through which every non-chunked send must pass.
- Exact phrasing of the `static_assert` message — clear enough that a reader understands the relationship.
- Whether to add a separate debug-only build-time trace/log for the first sub-frame of every chunked send (nice-to-have, not required).
- Naming of the new test — something along the lines of `streaming_invariant_single_frame_bound` and `streaming_invariant_auto_chunks_large_payload`.

### Deferred Ideas (OUT OF SCOPE)

- Per-response-type worst-case size table as operator documentation -> Phase 131 (DOCS-02 or new DOCS item if the table doesn't fit existing scope).
- Observability: counter `chromatindb_send_frame_bytes_histogram` for continuous monitoring of actual frame sizes in production — defer to operator tooling milestone (post-MVP).
- Changing `MAX_FRAME_SIZE` from 110 MiB to 2 MiB — that's Phase 128 (FRAME-01).
- CI-time AST/grep scans — rejected by D-06 as maintenance-heavy and redundant with D-04.
- Any change to `STREAMING_THRESHOLD`. It stays 1 MiB.
- Changing `BatchReadResponse.MAX_CAP` — during discuss-phase it was flagged as possibly redundant at 4 MiB under the new blob cap, but it does not break the frame invariant and Phase 131 can document the relationship.

## Phase Requirements

| ID       | Description                                                                                                                                                                         | Research Support                                                                                                                                                             |
|----------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| AUDIT-01 | Inventory every non-chunked single-frame response type and document its worst-case payload size at existing request-level caps.                                                     | D-02/D-03 reframed this requirement: the deliverable is the **send-path inventory** (below) showing every response routes through `send_message`, not a size table.         |
| AUDIT-02 | Any response whose worst-case payload exceeds 2 MiB after AEAD + framing overhead either gets its request-level cap lowered or is moved to the streaming path; unit tests gate the 2 MiB invariant going forward. | Satisfied structurally by D-01/D-04/D-05: `send_message` already auto-streams, the runtime assert catches future bypass, the `static_assert` catches future constant drift. |

## Architectural Responsibility Map

| Capability                                               | Primary Tier      | Secondary Tier | Rationale                                                                                                                                     |
|----------------------------------------------------------|-------------------|----------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| Runtime assertion on non-chunked plaintext size          | `db/net/` (Connection) | —              | The assertion lives at the send-pipeline waist; `db/peer/` handlers are blind to framing.                                                    |
| Compile-time `MAX_FRAME_SIZE` vs `STREAMING_THRESHOLD`   | `db/net/framing.h`  | —              | Both constants are declared in `framing.h`; the `static_assert` must sit alongside them.                                                     |
| Round-trip boundary test harness                         | `db/tests/net/`   | —              | Colocated with `test_connection.cpp` per D-12; reuses the acceptor/initiator/on_ready loopback fixture established at `test_connection.cpp:475,602`. |
| Call-site inventory (documentation artifact)             | Research + plan doc | —              | No code change; the table in this file and the resulting plan are the deliverable.                                                           |

## Standard Stack

No new dependencies. The phase uses only existing project infrastructure.

| Library / Component             | Version       | Purpose                                      | Why Standard                                                                                   |
|---------------------------------|---------------|----------------------------------------------|------------------------------------------------------------------------------------------------|
| Catch2                          | existing      | Unit test framework                          | Already used by every file in `db/tests/net/`; `TEST_CASE` / `SECTION` / `REQUIRE` idioms.    |
| asio (standalone)               | existing      | io_context / coroutines                      | Every existing test in `test_connection.cpp` spins a loopback `asio::io_context` with `co_spawn`. |
| `chromatindb::crypto::AEAD`     | existing      | AEAD round-trip verification                 | Already referenced by `test_framing.cpp`.                                                     |
| `chromatindb::net::Connection`  | existing      | Send / recv pipeline under test              | Public API: `send_message`, `create_inbound/outbound`, `on_ready`, `on_message`, `run`.       |
| `<cassert>` `assert` macro      | standard      | Runtime invariant check                      | Simplest; `spdlog::critical` + `std::abort()` is an alternative if release-mode behavior matters (see Open Question Q1). |

## Architecture Patterns

### System Architecture Diagram

Send-pipeline data flow, with the three callable entry points at the top and the one TCP write at the bottom:

```
                 caller                      handshake code paths
                 (db/peer/*.cpp)            (db/net/connection.cpp lines
                 conn->send_message(...)      279, 373, 449, 543, 617,
                                              638, 706 -> send_raw direct;
                                              321, 398, 474, 594, 678,
                                              746 -> send_encrypted direct)
                          |
                          v
                send_message (line 968)
                 -- size branch --
                 /               \
     < STREAMING_           >= STREAMING_
     THRESHOLD              THRESHOLD
         |                       |
         v                       v
   TransportCodec::         send_message_chunked (line 894)
   encode(type,             --builds 14-byte chunked header--
   payload, req_id)          pushes PendingMessage{is_chunked=true}
         |                    into send_queue_
         v                       |
   enqueue_send (line 979)       |
   [NARROW WAIST]                |
   push into send_queue_         |
         \                      /
          \                    /
           v                  v
       drain_send_queue (line 999)  [single-reader coroutine]
       per PendingMessage:
       - is_chunked? -> send_encrypted(header)
                     -> loop: send_encrypted(chunk<=STREAMING_THRESHOLD)
                     -> send_encrypted(empty sentinel)
       - else:        -> send_encrypted(encoded)
                          |
                          v
              send_encrypted (line 152)
              [AEAD encrypt + send_raw]
                          |
                          v
              send_raw (line 111)
              [4B BE length prefix + ciphertext via asio::async_write]
                          |
                          v
                  socket_ (TCP or UDS)
```

The diagram makes the narrow-waist choice obvious:

- `enqueue_send` has EXACTLY ONE caller in the entire codebase (the non-chunked branch of `send_message` at `db/net/connection.cpp:976`). Verified by `grep -rn "enqueue_send" db/`.
- `send_encrypted` has MANY direct callers (handshake, chunked header, chunked data sub-frames of size `STREAMING_THRESHOLD`, chunked sentinel, non-chunked via drain). An assertion here cannot say `< STREAMING_THRESHOLD` without tripping on legitimate `STREAMING_THRESHOLD`-sized sub-frames.
- `send_raw` is below AEAD and carries ciphertext — wrong layer for a plaintext-size assertion.

### Recommended Project Structure

No structural change. Code edits are localized:

```
db/
├── net/
│   ├── framing.h        # ADD: static_assert in the existing constants block
│   └── connection.cpp   # ADD: assertion at head of enqueue_send() body
└── tests/
    └── net/
        └── test_connection.cpp  # ADD: new TEST_CASE for streaming boundary
```

### Pattern 1: Narrow-waist runtime invariant

**What:** Put the assertion at the single choke-point through which every relevant code path must pass, so one assertion covers every caller past and future.

**When to use:** Exactly this situation — a policy ("non-chunked sends cannot be big") needs to be enforced across many callers.

**Example (shape, not final wording — actual strings MUST NOT embed phase numbers per memory `feedback_no_phase_leaks_in_user_strings`):**

```cpp
// In db/net/connection.cpp at the head of enqueue_send(...)
// Source: narrow-waist of the non-chunked send path
asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    // Streaming invariant: send_message routes payloads >= STREAMING_THRESHOLD
    // to the chunked path; anything reaching this function is a non-chunked
    // transport message and MUST be small enough to fit in one sub-frame.
    // A small envelope margin covers the FlatBuffer TransportMessage overhead.
    assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN
           && "streaming invariant broken: non-chunked send exceeds sub-frame size");
    ...existing body...
}
```

Where `TRANSPORT_ENVELOPE_MARGIN` is a new small constant (recommend `64`, see Code Examples section; the worst-case FlatBuffer overhead for `TransportMessage{type:1, payload:[ubyte], request_id:u32}` is around 20-32 bytes). Place the constant in `framing.h` next to the other framing constants, or as a `static constexpr` inside `connection.cpp`.

### Pattern 2: `static_assert` at constant-declaration site

**What:** Co-locate the `static_assert` with the constants it relates so any future edit sees it.

**Example (shape):**

```cpp
// In db/net/framing.h immediately after MAX_FRAME_SIZE / STREAMING_THRESHOLD
static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD,
    "frame cap must be at least 2x the streaming threshold: one full sub-frame "
    "plus headroom for AEAD tag, length prefix, and transport envelope");
```

Today: `110 MiB >= 2 * 1 MiB` -- trivially true. After FRAME-01: `2 MiB >= 2 * 1 MiB` -- exactly equal; holds. The existing `static_assert(MAX_FRAME_SIZE > STREAMING_THRESHOLD, ...)` at `framing.h:24-25` should stay or be subsumed.

### Anti-Patterns to Avoid

- **Asserting at `send_encrypted`:** chunked sub-frames legitimately pass `STREAMING_THRESHOLD` plaintext bytes (see `connection.cpp:1017-1022`). An assertion `plaintext.size() < STREAMING_THRESHOLD` there fires on every big chunked send. An assertion `<=` is useless (it's the physical chunk limit, not the invariant we want to pin).
- **Asserting in `send_message` before encode:** Tempting because `payload.size()` is crisp there. But it's redundant — the branch at line 972 already bifurcates on that value; putting an `assert(payload.size() < STREAMING_THRESHOLD)` in the else branch just restates the `if` condition. `enqueue_send` catches a stronger class of bugs: future misuse where a caller bypasses `send_message` entirely and calls `enqueue_send` directly with an oversized encoded message.
- **Adding a new helper called from both sides:** D-06 and the "no new abstractions" guardrail reject this. The existing narrow waist is sufficient.
- **Making the assertion release-only via `spdlog::error` + early return:** A streaming invariant breach is a programmer error, not a runtime condition. `assert()` (compiled out in release) plus the `static_assert` for the constant relationship is the honest contract. See Open Question Q1 if release-mode behavior matters.

## Send-Path Inventory (AUDIT-01 deliverable, D-07)

**How to read this table:** Each row is a call site that can cause bytes to flow into the AEAD frame pipeline. The "Terminal primitive" column shows the lowest-level function the call chain reaches. "Routed?" = does this site flow through `send_message`'s `>= STREAMING_THRESHOLD` branch? "Size bound" = the worst-case plaintext this site can produce, verified by direct reading of the code.

The planner should lift this table into `PLAN.md` as the audit artifact.

### Inventory rows — `send_message` callers (all correctly routed)

| # | Call site (file:line)                                              | Message type                          | Terminal primitive                                 | Routed through send_message? | Size bound                                                                                                         | Risk       |
|---|--------------------------------------------------------------------|---------------------------------------|---------------------------------------------------|------------------------------|--------------------------------------------------------------------------------------------------------------------|------------|
| 1 | `db/peer/message_dispatcher.cpp:74`                                | ErrorResponse (static helper)         | `send_message` -> non-chunked -> enqueue_send     | yes                          | 2 bytes payload                                                                                                    | bounded    |
| 2 | `db/peer/message_dispatcher.cpp:266`                               | QuotaExceeded                         | `send_message` -> non-chunked                     | yes                          | 0 bytes                                                                                                            | bounded    |
| 3 | `db/peer/message_dispatcher.cpp:374`                               | DeleteAck                             | `send_message` -> non-chunked                     | yes                          | small fixed (ack struct)                                                                                           | bounded    |
| 4 | `db/peer/message_dispatcher.cpp:452,460,464`                       | ReadResponse (3 sites)                | `send_message` -> branches on size                | yes                          | 1 + encoded_blob; blob capped by `MAX_BLOB_DATA_SIZE` (500 MiB today, 4 MiB default after Phase 128). Auto-chunks when >= 1 MiB. | bounded    |
| 5 | `db/peer/message_dispatcher.cpp:579`                               | ListResponse                          | `send_message`                                    | yes                          | hash-list response; bounded by ListRequest's max count; typical under 1 MiB but may exceed — send_message auto-chunks. | bounded    |
| 6 | `db/peer/message_dispatcher.cpp:612`                               | StatsResponse                         | `send_message`                                    | yes                          | fixed small stats struct                                                                                           | bounded    |
| 7 | `db/peer/message_dispatcher.cpp:647`                               | ExistsResponse                        | `send_message`                                    | yes                          | small fixed                                                                                                        | bounded    |
| 8 | `db/peer/message_dispatcher.cpp:721`                               | NodeInfoResponse                      | `send_message`                                    | yes                          | small fixed (u64s and counters); grows modestly in Phase 127 (adds 4 fields)                                       | bounded    |
| 9 | `db/peer/message_dispatcher.cpp:794`                               | NamespaceListResponse                 | `send_message`                                    | yes                          | 4 + N*32 bytes; auto-chunks for very large N                                                                       | bounded    |
| 10 | `db/peer/message_dispatcher.cpp:838`                              | StorageStatusResponse                 | `send_message`                                    | yes                          | small fixed                                                                                                        | bounded    |
| 11 | `db/peer/message_dispatcher.cpp:895`                              | NamespaceStatsResponse                | `send_message`                                    | yes                          | small fixed                                                                                                        | bounded    |
| 12 | `db/peer/message_dispatcher.cpp:923,931,980`                      | MetadataResponse (3 sites)            | `send_message`                                    | yes                          | 1 + 32 + small metadata blob                                                                                       | bounded    |
| 13 | `db/peer/message_dispatcher.cpp:1029`                             | BatchExistsResponse                   | `send_message`                                    | yes                          | 1 + 4 + count*(1+32); count<=256 -> <=8.4 KiB                                                                      | bounded    |
| 14 | `db/peer/message_dispatcher.cpp:1075`                             | DelegationListResponse                | `send_message`                                    | yes                          | small (delegation records are compact)                                                                             | bounded    |
| 15 | `db/peer/message_dispatcher.cpp:1195`                             | **BatchReadResponse**                 | `send_message` -> auto-chunks when size >= 1 MiB  | yes                          | `MAX_CAP = 4 MiB` enforced at `message_dispatcher.cpp:1103` plus small per-entry headers (5 + count*(1+32+8))**. This value >= STREAMING_THRESHOLD, so responses auto-stream whenever they exceed 1 MiB. | bounded via auto-chunk |
| 16 | `db/peer/message_dispatcher.cpp:1235,1271`                        | PeerInfoResponse (2 sites)            | `send_message`                                    | yes                          | small (peer list is bounded)                                                                                       | bounded    |
| 17 | `db/peer/message_dispatcher.cpp:1371`                             | TimeRangeResponse                     | `send_message`                                    | yes                          | bounded by request's max-count param                                                                               | bounded    |
| 18 | `db/peer/message_dispatcher.cpp:1428,1450,1456`                   | WriteAck / StorageFull / QuotaExceeded| `send_message`                                    | yes                          | 0-1 byte                                                                                                           | bounded    |
| 19 | `db/peer/connection_manager.cpp:262`                              | SyncNamespaceAnnounce                 | `send_message` via detached co_spawn              | yes                          | namespace-list payload, 4 + N*32 bytes; auto-chunks for large N                                                    | bounded    |
| 20 | `db/peer/connection_manager.cpp:434`                              | Ping                                  | `send_message`                                    | yes                          | empty                                                                                                              | bounded    |
| 21 | `db/peer/pex_manager.cpp:59`                                      | PeerListRequest                       | `send_message`                                    | yes                          | empty                                                                                                              | bounded    |
| 22 | `db/peer/pex_manager.cpp:125`                                     | PeerListResponse                      | `send_message`                                    | yes                          | `MAX_PEERS_PER_EXCHANGE = 8` (see `pex_manager.h:23`); few hundred bytes                                           | bounded    |
| 23 | `db/peer/peer_manager.cpp:105`                                    | PeerListRequest                       | `send_message`                                    | yes                          | empty                                                                                                              | bounded    |
| 24 | `db/peer/peer_manager.cpp:121`                                    | PeerListResponse                      | `send_message`                                    | yes                          | same `MAX_PEERS_PER_EXCHANGE` bound                                                                                | bounded    |
| 25 | `db/peer/peer_manager.cpp:606` (SIGHUP re-announce fan-out)       | SyncNamespaceAnnounce                 | `send_message` via per-peer detached co_spawn     | yes                          | namespace-list payload; auto-chunks for large N                                                                    | bounded    |
| 26 | `db/peer/blob_push_manager.cpp:76`                                | BlobNotify                            | `send_message`                                    | yes                          | 77 bytes (fixed per `blob_push_manager.cpp:60`)                                                                    | bounded    |
| 27 | `db/peer/blob_push_manager.cpp:88`                                | Notification                          | `send_message`                                    | yes                          | 77 bytes (same structure)                                                                                          | bounded    |
| 28 | `db/peer/blob_push_manager.cpp:137`                               | BlobFetch                             | `send_message`                                    | yes                          | 64 bytes                                                                                                           | bounded    |
| 29 | `db/peer/blob_push_manager.cpp:155,168,172`                       | BlobFetchResponse (3 sites)           | `send_message`                                    | yes                          | 1 or 1+32+encoded_blob; auto-chunks when >= 1 MiB                                                                  | bounded    |
| 30 | `db/peer/sync_orchestrator.cpp:151,190,315,376,390,434,456,506,545,582,590,628,653,799,808,867,883,941,989,1034` | Sync* + BlobTransfer (20+ sites) | `send_message`                                    | yes                          | **BlobTransfer** is the dominant large path; single-blob transfers carry an entire encoded blob (up to `MAX_BLOB_DATA_SIZE`) -- always >= 1 MiB for big blobs, always auto-chunked | bounded via auto-chunk |

**Total sites calling `Connection::send_message`:** 30 distinct call sites across 7 files in `db/peer/`, plus the `send_error_response` static helper at `db/peer/message_dispatcher.cpp:74` used by every handler's error branch. All 30 flow through the `send_message` bifurcation at `db/net/connection.cpp:972`; none bypass.

### Inventory rows — direct `Connection::send_raw` / `Connection::send_encrypted` callers (handshake only)

These are PRIVATE methods of `Connection`; the only external callers are its own handshake sub-paths. Confirmed by grep.

| # | Call site (file:line)          | Message type                 | Payload size                                                                                                | Routed? | Risk       |
|---|--------------------------------|------------------------------|-------------------------------------------------------------------------------------------------------------|---------|------------|
| H1 | `connection.cpp:279`          | TrustedHello (raw, pre-AEAD) | 32-byte nonce + 2592-byte ML-DSA-87 signing pubkey + FlatBuffer envelope = ~2.7 KiB                         | handshake only, fixed | bounded-by-construction |
| H2 | `connection.cpp:321`          | AuthSignature (encrypted)    | 1 role byte + 4 pubkey-size BE + 2592 pubkey + ~4627 ML-DSA-87 signature + envelope = ~7.3 KiB              | handshake only, fixed | bounded-by-construction |
| H3 | `connection.cpp:373`          | KemPubkey (raw)              | `HandshakeInitiator::start()` output — ML-KEM-1024 pubkey + envelope, ~1.6 KiB                              | handshake only, fixed | bounded-by-construction |
| H4 | `connection.cpp:398`          | AuthSignature (encrypted, fallback) | same as H2                                                                                           | handshake only, fixed | bounded-by-construction |
| H5 | `connection.cpp:449`          | KemPubkey (raw)              | same as H3                                                                                                  | handshake only, fixed | bounded-by-construction |
| H6 | `connection.cpp:474`          | AuthSignature (encrypted)    | same as H2                                                                                                  | handshake only, fixed | bounded-by-construction |
| H7 | `connection.cpp:543`          | TrustedHello (raw, responder) | same as H1                                                                                                | handshake only, fixed | bounded-by-construction |
| H8 | `connection.cpp:594`          | AuthSignature (encrypted, responder) | same as H2                                                                                         | handshake only, fixed | bounded-by-construction |
| H9 | `connection.cpp:617`          | PQRequired (raw, empty)      | 0 bytes                                                                                                     | handshake only, fixed | bounded-by-construction |
| H10 | `connection.cpp:638`         | KemCiphertext (raw)          | ML-KEM-1024 ciphertext + envelope, ~1.6 KiB                                                                 | handshake only, fixed | bounded-by-construction |
| H11 | `connection.cpp:678`         | AuthSignature (encrypted)    | same as H2                                                                                                  | handshake only, fixed | bounded-by-construction |
| H12 | `connection.cpp:706`         | KemCiphertext (raw)          | same as H10                                                                                                 | handshake only, fixed | bounded-by-construction |
| H13 | `connection.cpp:746`         | AuthSignature (encrypted)    | same as H2                                                                                                  | handshake only, fixed | bounded-by-construction |
| H14 | `connection.cpp:1013,1022,1028` (inside `drain_send_queue`) | chunked header / sub-frame / sentinel | 14-byte header, exactly `STREAMING_THRESHOLD` per sub-frame, 0-byte sentinel | intra-class (chunked path) | bounded by sub-frame loop |

All H-rows are fixed-size handshake payloads or the chunked-send machinery itself. None can grow unbounded. None need rerouting.

### Inventory rows — unrelated lookalikes

| # | Call site (file:line)                             | Why listed               | Actual risk                                                                                                                                                                                  |
|---|---------------------------------------------------|--------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| X1 | `db/main.cpp:571` `auto send_encrypted = [&](...)` lambda | Grep-adjacent false positive | This is a **local lambda** inside `cmd_list_peers` that implements a synchronous UDS client handshake for the `chromatindb list-peers` subcommand. It is NOT `Connection::send_encrypted`. It transmits only small fixed-size handshake payloads (TrustedHello ~2.7 KiB at `main.cpp:683-692`, AuthSignature ~7.3 KiB same block, PeerInfoRequest empty at `main.cpp:730-733`). Not a risk for the streaming invariant. No action needed. |

### Inventory outcome

Every non-chunked send path either:

(a) goes through `conn->send_message(...)`, which automatically routes `>= STREAMING_THRESHOLD` payloads into `send_message_chunked` at `db/net/connection.cpp:972`, **or**

(b) is a fixed-size handshake payload of at most ~7.3 KiB (AuthSignature with ML-DSA-87 signature is the largest), which is well under `STREAMING_THRESHOLD` and can never grow because the ML-DSA-87 signature size is a protocol constant.

**Zero bypass sites found.** This matches D-09's success condition: no fix work needed, only the assertions and the round-trip test.

## Runtime Assertion Site Selection (D-04)

### Evaluation

| Candidate                  | Location                                  | Caller count          | Invariant fires on                                                                                                                                   | Recommendation |
|----------------------------|-------------------------------------------|-----------------------|------------------------------------------------------------------------------------------------------------------------------------------------------|----------------|
| `Connection::enqueue_send` | `db/net/connection.cpp:979`              | 1 (only `send_message`) | Any future caller that enqueues an encoded message >= `STREAMING_THRESHOLD + envelope` bytes. Covers both "someone bypasses `send_message`" and "someone tweaks the bifurcation threshold incorrectly." | **PICK THIS** |
| `Connection::send_encrypted` | `db/net/connection.cpp:152`             | 13 handshake sites + 4 from drain_send_queue | Chunked sub-frames legitimately pass exactly `STREAMING_THRESHOLD` plaintext bytes (line 1018-1022). An assertion strictly `< STREAMING_THRESHOLD` would trip; `<=` is merely a physical chunk-size check, not the streaming invariant. Assertion is either over-triggering or meaningless. | reject         |
| New shared helper          | new function called by both paths         | 0 (does not exist)    | Requires introducing an abstraction; D-06 and the project's "no new abstractions" guardrail reject this. | reject         |

### Recommendation: `Connection::enqueue_send`

The assertion lives at the head of the function body at `db/net/connection.cpp:979`. Because `enqueue_send` is `private` and has a single caller inside the same translation unit, the invariant reads as a local contract between `send_message`'s non-chunked branch and the send queue.

**Sample placement** (shape only; final wording is planner's call, with the memory-note guardrail that the `assert` message string MUST NOT embed phase numbers or GSD identifiers):

```cpp
asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    // Streaming invariant: send_message bifurcates at STREAMING_THRESHOLD;
    // anything reaching enqueue_send is a non-chunked transport message and
    // its encoded size must fit inside one sub-frame with envelope margin.
    assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN
           && "non-chunked send bypassed streaming boundary");
    if (closed_ || closing_) co_return false;
    ...existing body unchanged...
}
```

`TRANSPORT_ENVELOPE_MARGIN` is a new named constant (see Code Examples). Recommend `64`.

## Compile-time invariant (D-05)

### Overhead accounting for `MAX_FRAME_SIZE` vs `STREAMING_THRESHOLD`

The relationship should capture "one full sub-frame plus room for AEAD + framing overhead, times a safety factor." Exact overhead components:

| Component                                          | Size (bytes)                | Source                                                                |
|----------------------------------------------------|-----------------------------|-----------------------------------------------------------------------|
| Plaintext sub-frame (chunked path max)             | `STREAMING_THRESHOLD` = 1 048 576 | `db/net/connection.cpp:1018` (`std::min(STREAMING_THRESHOLD, ...)`) |
| AEAD tag (ChaCha20-Poly1305, appended)             | 16                          | `db/crypto/aead.h:18` (`TAG_SIZE`)                                    |
| Length prefix (4-byte BE u32)                      | 4                           | `db/net/framing.h:28` (`FRAME_HEADER_SIZE`)                           |
| Transport envelope (FlatBuffer `TransportMessage`) | 0 for chunked sub-frames    | sub-frames are raw bytes, not encoded TransportMessage (see `connection.cpp:1022`) |
| Transport envelope (FlatBuffer `TransportMessage`) | ~20-32 typical, <=64 worst | `db/schemas/transport.fbs` + `db/net/protocol.cpp:7-20`. FlatBuffer root table + vtable + vector prefix. Applies ONLY to non-chunked sends; chunked sub-frames are raw. |
| AEAD nonce (not on wire; nonce is derived)         | 0                           | `db/net/framing.cpp:10` (derived from counter, not sent)              |

**On-wire size of a maximal chunked data sub-frame:** `4 (prefix) + STREAMING_THRESHOLD (plaintext) + 16 (tag) = 1 048 596 bytes ≈ 1 MiB + 20`.

**On-wire size of a maximal non-chunked encrypted message:** `4 + (STREAMING_THRESHOLD - 1 + 64 envelope) + 16 = STREAMING_THRESHOLD + 83` worst-case, comfortably under `2 * STREAMING_THRESHOLD`.

### Recommended `static_assert` shape

Most honest relationship: `MAX_FRAME_SIZE` must be at least large enough to hold one full chunked sub-frame (plaintext + tag + length prefix) with headroom. `2 * STREAMING_THRESHOLD` is the cleanest round bound that:

- Trivially holds today: `110 * 1024 * 1024 >= 2 * 1048576` -> `115 343 360 >= 2 097 152`, true.
- Still holds after Phase 128: `2 * 1024 * 1024 >= 2 * 1048576` -> `2 097 152 >= 2 097 152`, true (exact equality).
- Gives `STREAMING_THRESHOLD` of headroom over the worst-case on-wire sub-frame (`STREAMING_THRESHOLD + ~20`).

**Placement:** `db/net/framing.h`, right after line 22 where `STREAMING_THRESHOLD` is declared (or replacing the existing weaker `static_assert` at lines 24-25).

**Shape:**

```cpp
// In db/net/framing.h immediately after STREAMING_THRESHOLD
static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD,
    "MAX_FRAME_SIZE must admit one full streaming sub-frame plus headroom "
    "for AEAD tag, length prefix, and protocol envelope. Shrinking one "
    "constant without re-checking the other breaks the invariant.");
```

This is the D-05 artifact. It survives the Phase 128 shrink and catches the "tweaked STREAMING_THRESHOLD alone" and "tweaked MAX_FRAME_SIZE alone" bugs.

**Note:** the existing `static_assert(MAX_FRAME_SIZE > STREAMING_THRESHOLD, "frame must accommodate streaming chunks")` at `framing.h:24-25` is weaker (only requires strict `>`). The planner can choose to either (a) replace it with the `2x` form, or (b) keep it and add the `2x` form alongside. Recommend (a) — one invariant is cleaner than two.

## Round-trip Test Harness (D-10, D-11, D-12)

### Fixture pattern

`db/tests/net/test_connection.cpp` already establishes the pattern: acceptor on random port, responder `Connection::create_inbound`, initiator `Connection::create_outbound`, use `on_ready` to trigger the send from a coroutine that spawns inside the initiator's event loop. See `test_connection.cpp:475-542` (`"Send queue: multiple concurrent sends"`) for the canonical shape. That same fixture can be lifted for the streaming boundary test.

### File location decision: `test_connection.cpp` (not `test_framing.cpp`)

- `test_framing.cpp` exercises the pure `write_frame` / `read_frame` primitives, no `Connection`, no socket, no handshake. Its existing "1 MiB payload" test at `test_framing.cpp:207-216` uses `write_frame` directly against an `AEAD::keygen()` key — it doesn't test the streaming-path bifurcation at all.
- `test_connection.cpp` already has the acceptor + initiator + `on_ready` + `on_message` fixture, already exercises `send_message` end-to-end, and already receives `TransportMsgType_BlobWrite` on the responder side for correctness checks.

D-10's requirement ("a `STREAMING_THRESHOLD - 1` payload sent via `send_message` produces a single frame under the cap; a `STREAMING_THRESHOLD + 1` payload auto-streams into multiple sub-frames") is a `send_message`-level integration test, not a framing primitive test. **Place the new TEST_CASE in `test_connection.cpp`**.

### Existing caps that need thinking about

- The existing MITM test at `test_connection.cpp:795` has `if (len > 110 * 1024 * 1024) co_return std::nullopt;` — this is inside a private mock reader and will need updating when Phase 128 shrinks the constant. Flagged for Phase 128, not for this phase. Not a blocker for the new streaming-boundary test because the new test goes through real `Connection`s, not this mock.
- The `AEAD::encrypt` / `AEAD::decrypt` path has no hidden payload cap beyond what libsodium imposes (none at this scale). Confirmed by `db/crypto/aead.h`.

### Payload construction

For a payload of `STREAMING_THRESHOLD + 1 = 1 048 577` bytes, use `std::vector<uint8_t>(STREAMING_THRESHOLD + 1, 0xAB)` with a distinctive fill pattern so the responder can verify byte-exact round-trip. Memory cost: ~1 MiB per test case, acceptable. A 110 MiB cap is not at risk; the test is well under 2 MiB total.

### Suggested test case skeleton

Two related `SECTION`s under one `TEST_CASE`, or two adjacent `TEST_CASE`s — planner's choice. Sketch:

```cpp
TEST_CASE("streaming invariant: single-frame bound under STREAMING_THRESHOLD",
          "[connection][streaming]") {
    // Fixture: loopback initiator+responder, handshake via run().
    // Mirror the "Send queue: multiple concurrent sends" pattern at line 475.

    // Arrange:
    //   std::vector<uint8_t> payload(STREAMING_THRESHOLD - 1, 0xAB);
    //
    // Act (on initiator's on_ready callback):
    //   co_await conn->send_message(TransportMsgType_BlobWrite, payload);
    //
    // Assert (on responder's on_message callback):
    //   received_payload.size() == STREAMING_THRESHOLD - 1
    //   received_payload[i] == 0xAB for all i
    //
    // Implicit frame-size check: the existing read path enforces
    // MAX_FRAME_SIZE on every frame (framing.cpp:49). If a future regression
    // makes the non-chunked branch emit a single frame larger than that,
    // recv_raw at connection.cpp:135 drops the connection and the test
    // times out without receiving the payload.
}

TEST_CASE("streaming invariant: payload above STREAMING_THRESHOLD auto-chunks",
          "[connection][streaming]") {
    // Arrange:
    //   std::vector<uint8_t> payload(STREAMING_THRESHOLD + 1, 0xCD);
    //
    // Act: same send_message call with the larger payload.
    //
    // Assert: on_message fires with:
    //   received_payload.size() == STREAMING_THRESHOLD + 1
    //   received_payload[i] == 0xCD for all i
    //
    // The fact that the message survives end-to-end means the chunked
    // sub-frame path was exercised: first a 14-byte chunked header, then
    // two sub-frames (STREAMING_THRESHOLD bytes + 1 byte), then the
    // zero-length sentinel. Each sub-frame is a separate AEAD frame
    // well under MAX_FRAME_SIZE.
}
```

**Test-naming note:** per memory `feedback_no_phase_leaks_in_user_strings`, the TEST_CASE names above avoid "Phase 126" / "v4.2.0" / "AUDIT-01" markers. Suggested tags `[connection][streaming]` follow the existing tag convention used throughout `test_connection.cpp` (`[connection]`, `[connection][nonce]`, `[connection][send_queue]`, etc.).

### Sampling and feedback

Per memory `feedback_delegate_tests_to_user`, the RESEARCH.md does NOT prescribe an orchestrator-level `chromatindb_tests` run. The tests get added; they run when the user runs them.

## Risk Areas Reviewed (from CONTEXT.md "Risk Areas for audit attention")

Each of these was specifically called out in CONTEXT.md. Findings:

| CONTEXT.md risk item                                            | Verified? | Finding                                                                                                                                                                                      |
|-----------------------------------------------------------------|-----------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `send_message_chunked` builds chunked header separately via `send_encrypted` | yes      | `db/net/connection.cpp:907-917`: header is exactly `CHUNKED_HEADER_SIZE = 14` bytes plus optional `extra_metadata` span. `extra_metadata` is only passed by one caller (`send_message` at line 973 with `{}` default), so its size is 0 in practice. Well under `STREAMING_THRESHOLD`. Bounded by construction. |
| `drain_send_queue`'s chunked branch sends sub-frames via `send_encrypted` | yes      | `db/net/connection.cpp:1017-1023`: sub-frame size = `std::min(STREAMING_THRESHOLD, remaining)`. Exactly 1 MiB plaintext worst-case -> ciphertext 1 MiB + 16 tag -> on-wire 1 MiB + 20. This is the tightest legitimate upper bound and it is below `MAX_FRAME_SIZE` with >= 1 MiB of headroom after Phase 128. Bounded by the loop constant. |
| Handshake / HELLO / AuthSignature paths                         | yes      | 13 direct `send_raw` / `send_encrypted` call sites (rows H1-H13 above). All payloads are fixed by protocol: ML-DSA-87 pubkey is 2592 bytes, signature is 4627 bytes, KEM artifacts are ~1.6 KiB, nonces are 32 bytes. Largest encrypted handshake message is AuthSignature at ~7.3 KiB. All bounded-by-construction. |
| PEX announce / SyncNamespaceAnnounce / BlobNotify broadcasts    | yes      | All confirmed to route via `conn->send_message(...)` through the per-peer detached co_spawn pattern (`connection_manager.cpp:262`, `peer_manager.cpp:606`, `blob_push_manager.cpp:76`). None use `send_encrypted` or `enqueue_send` directly. BlobNotify payload is fixed 77 bytes. PEX responses capped at `MAX_PEERS_PER_EXCHANGE = 8`. SyncNamespaceAnnounce auto-chunks for large namespace lists. |

No risk area contains a bypass.

## Environment Availability

Phase 126 adds tests and assertions; no new external dependencies are required. All tooling is already present:

| Dependency                      | Required By                      | Available | Version     | Fallback |
|---------------------------------|----------------------------------|-----------|-------------|----------|
| Catch2                          | new TEST_CASE                    | yes       | existing    | —        |
| asio (standalone)               | loopback fixture                 | yes       | existing    | —        |
| libsodium (ChaCha20-Poly1305)   | AEAD round-trip in the test      | yes       | existing    | —        |
| CMake + compiler supporting C++20 coroutines | build                 | yes       | existing    | —        |

## Validation Architecture

### Test Framework

| Property              | Value                                                                                  |
|-----------------------|----------------------------------------------------------------------------------------|
| Framework             | Catch2 (existing)                                                                      |
| Config file           | `db/CMakeLists.txt` lines 238-239 register `tests/net/test_connection.cpp` and `test_framing.cpp` |
| Quick run command     | Delegated to user per memory `feedback_delegate_tests_to_user`. User's local invocation is the chromatindb_tests binary with the specific tag, e.g. `./build/db/chromatindb_tests "[connection][streaming]"`. |
| Full suite command    | `./build/db/chromatindb_tests` (user-run)                                              |

### Phase Requirements -> Test Map

| Req ID   | Behavior                                                                                                  | Test Type              | Automated Command                                                                 | File Exists? |
|----------|-----------------------------------------------------------------------------------------------------------|------------------------|-----------------------------------------------------------------------------------|---|
| AUDIT-01 | Send-path inventory: every call site documented                                                           | Documentation artifact | No test — inventory table in PLAN.md is the artifact                              | N/A (doc) |
| AUDIT-02 | Single-frame under-threshold payload decodes correctly                                                    | Integration / loopback | `./build/db/chromatindb_tests "[connection][streaming]"` (first TEST_CASE)        | exists (`test_connection.cpp`) |
| AUDIT-02 | Auto-chunked payload above threshold decodes correctly                                                    | Integration / loopback | `./build/db/chromatindb_tests "[connection][streaming]"` (second TEST_CASE)       | exists |
| AUDIT-02 | Compile-time invariant holds                                                                              | Build                  | Any C++ compile of the project (the `static_assert` fires at compile time)        | exists (`framing.h`) |
| AUDIT-02 | Runtime assertion fires on synthetic bypass                                                               | Implicit               | The assertion lives on the hot path; if a regression introduces a bypass, either an existing test OR the two new TEST_CASEs will trip it via `assert()`. No dedicated synthetic-bypass test is required (D-04's success is "loud test failure"). | — |

### Sampling Rate

- **Per task commit:** `./build/db/chromatindb_tests "[streaming]"` (quick, user-run).
- **Per wave merge:** `./build/db/chromatindb_tests` (full suite, user-run).
- **Phase gate:** Full suite green plus a successful build (compile-time `static_assert` passes).

### Wave 0 Gaps

- None. All infrastructure exists: Catch2 is registered, `test_connection.cpp` already uses the loopback fixture, `framing.h` already has a `static_assert` block to extend.

## Common Pitfalls

### Pitfall 1: Assertion at the wrong layer fires on legitimate chunked sub-frames

**What goes wrong:** Putting `assert(plaintext.size() < STREAMING_THRESHOLD)` in `send_encrypted` (`db/net/connection.cpp:152`). The chunked path calls `send_encrypted(chunk)` with `chunk.size() == STREAMING_THRESHOLD` (`connection.cpp:1018-1022`). The assertion fires on every big chunked send, which is the normal case.

**Why it happens:** `send_encrypted` looks like "the lowest-level AEAD primitive" and is tempting. But it is shared between chunked and non-chunked paths, and the chunked path legitimately saturates `STREAMING_THRESHOLD`.

**How to avoid:** Put the assertion at `enqueue_send`, which ONLY the non-chunked path reaches. Verified by `grep -rn "enqueue_send" db/` -> exactly one caller.

**Warning signs:** Every `./build/db/chromatindb_tests` run fails on the first BlobTransfer test after the assertion is added. Or, subtler: the chunked header (14 bytes) passes but the first data sub-frame (1 MiB) fails.

### Pitfall 2: Running tests from the orchestrator context instead of delegating

**What goes wrong:** Spending Claude tokens on a `./build/db/chromatindb_tests` run that the user could do locally for free.

**Why it happens:** Reflex — "my change added a test, therefore I should run it."

**How to avoid:** Per memory `feedback_delegate_tests_to_user`, ask the user to run the test and paste output. Exceptions: grep-level checks, subagent executor scope.

### Pitfall 3: Phase number leaks into assertion strings / test names

**What goes wrong:** Naming a test `"Phase 126 streaming invariant"` or putting `"FRAME-01 will shrink this"` in an `assert` message.

**Why it happens:** Phase numbers feel like natural anchors when writing code during a GSD phase.

**How to avoid:** Per memory `feedback_no_phase_leaks_in_user_strings`, test names and `assert()` message strings describe WHAT the feature is (streaming invariant, sub-frame boundary), not WHEN it was added. Code comments are fine to reference phase numbers for developer context; user-visible strings are not. Grep gate: `grep -rn "Phase [0-9]" db/net/ db/tests/net/ --include='*.cpp' --include='*.h'` should return no hits inside string literals after this phase lands.

### Pitfall 4: New coroutine suspension points inside the assertion

**What goes wrong:** A runtime "bypass detector" that does async logging or `asio::post` before aborting. Introduces a new suspension point mid-`enqueue_send`, which changes the send-queue ordering guarantees.

**Why it happens:** "Let me log the bypass details nicely before aborting."

**How to avoid:** The assertion is synchronous — `assert(...)` (or `if (!cond) std::abort()`). No `co_await` inside the check. Matches PROJECT.md's "recv_sync_msg executor transfer after offload()" decision: coroutine lifetime rules forbid new suspension points on this path.

**Warning signs:** TSAN flags a new race, or the send queue reorders messages under stress.

### Pitfall 5: Treating `BatchReadResponse.MAX_CAP = 4 MiB` as a bypass

**What goes wrong:** The research looks at `message_dispatcher.cpp:1103` (`MAX_CAP = 4194304`), notices `4 MiB > 2 MiB frame cap`, and flags it as "the response can exceed the frame cap and needs lowering."

**Why it happens:** Intuition from the pre-reframe version of the audit, and the `BatchReadResponse.MAX_CAP` note in CONTEXT.md's Out of Scope block.

**How to avoid:** `BatchReadResponse` goes through `send_message`, which auto-streams any payload `>= 1 MiB` at `connection.cpp:972`. The 4 MiB cap means BatchReadResponse CAN produce a 4 MiB plaintext, and then it is split into 4 sub-frames of 1 MiB each — not a single 4 MiB frame. The 4 MiB cap is about per-response memory, not per-frame bytes. CONTEXT.md Out-of-Scope explicitly confirms this is not a Phase 126 concern.

## Code Examples

### A. Runtime assertion in enqueue_send

```cpp
// db/net/framing.h — near STREAMING_THRESHOLD declaration
/// Worst-case overhead of the TransportMessage FlatBuffer envelope around a
/// plaintext payload. Root table + vtable + vector prefix typically sum to
/// ~20-32 bytes; 64 is a conservative upper bound for the enqueue_send
/// streaming invariant assertion.
constexpr size_t TRANSPORT_ENVELOPE_MARGIN = 64;
```

```cpp
// db/net/connection.cpp — at the top of enqueue_send body (line 979)
asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    // Streaming invariant (see framing.h): send_message routes any payload
    // >= STREAMING_THRESHOLD through send_message_chunked. Anything reaching
    // here is a non-chunked transport message whose encoded size must fit
    // inside one sub-frame with envelope headroom.
    assert(encoded.size() < STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN
           && "non-chunked send exceeds sub-frame size (streaming boundary)");

    if (closed_ || closing_) co_return false;
    // ...rest of the existing function body unchanged...
}
```

### B. Compile-time invariant in framing.h

```cpp
// db/net/framing.h — after STREAMING_THRESHOLD (replaces the existing
// weaker static_assert at lines 24-25).
static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD,
    "MAX_FRAME_SIZE must admit one full streaming sub-frame plus headroom "
    "for AEAD tag (16B), length prefix (4B), and transport envelope. "
    "Shrinking either constant without re-checking the other breaks the "
    "invariant.");
```

### C. Round-trip test skeleton (sketch, not final)

```cpp
// db/tests/net/test_connection.cpp — new TEST_CASEs tagged [connection][streaming].
// Mirror the acceptor + initiator + on_ready pattern from
// "Send queue: multiple concurrent sends complete without crash" (line 475).

TEST_CASE("streaming invariant: payload just under STREAMING_THRESHOLD "
          "round-trips through non-chunked path",
          "[connection][streaming]") {
    // (1) Create acceptor on random port.
    // (2) Spawn responder coroutine: on_message captures payload type and bytes.
    // (3) Spawn initiator coroutine: on_ready schedules
    //     co_spawn(ioc, conn->send_message(TransportMsgType_BlobWrite,
    //                                       payload(STREAMING_THRESHOLD - 1, 0xAB)),
    //              asio::detached);
    // (4) Timeout timer cancels both if nothing arrives.
    // (5) Run ioc_for(sufficient_timeout).
    // (6) REQUIRE(received.size() == STREAMING_THRESHOLD - 1);
    // (7) REQUIRE(received[0] == 0xAB && received.back() == 0xAB);
}

TEST_CASE("streaming invariant: payload just over STREAMING_THRESHOLD "
          "auto-chunks end-to-end",
          "[connection][streaming]") {
    // Same fixture, payload(STREAMING_THRESHOLD + 1, 0xCD).
    // Same assertions but size == STREAMING_THRESHOLD + 1, fill 0xCD.
    // Successful receipt implicitly proves:
    //   - send_message routed into send_message_chunked;
    //   - drain_send_queue emitted: 14B header, two data sub-frames
    //     (STREAMING_THRESHOLD + 1 byte), zero-length sentinel;
    //   - recv_chunked reassembled all sub-frames;
    //   - on_message fired with the full payload.
}
```

## State of the Art

Not applicable — this is an invariant-pinning / audit phase, not an adoption of a new pattern. The streaming-sub-frame pattern is already state-of-the-art for this codebase.

## Assumptions Log

| # | Claim                                                                                                       | Section                            | Risk if Wrong |
|---|-------------------------------------------------------------------------------------------------------------|------------------------------------|---------------|
| A1 | `TRANSPORT_ENVELOPE_MARGIN = 64` bytes is a safe upper bound for the FlatBuffer `TransportMessage` envelope. | Runtime Assertion Site Selection, Code Examples A | If a future FlatBuffer schema extension adds many fields, overhead could grow. Mitigation: the assertion expression uses the named constant; revising it is a one-line change. FlatBuffer table overhead is bounded by number of fields, and `TransportMessage` has only 3 fields. 64 is conservative. |
| A2 | The existing `static_assert(MAX_FRAME_SIZE > STREAMING_THRESHOLD, ...)` at `framing.h:24-25` should be REPLACED by the stronger `>= 2 * STREAMING_THRESHOLD` form, not kept alongside. | Compile-time invariant (D-05) | If the user/planner prefers both invariants, keeping the old one is harmless. Recommendation is stylistic. |
| A3 | The CLI (`cli/src/connection.cpp`) is out of scope for this phase even though it has a structurally identical send pipeline with its own duplicate `MAX_FRAME_SIZE`, `STREAMING_THRESHOLD`, `send_encrypted`, `send_chunked` (verified at cli/src/connection.cpp:35, 293, 633, 643). | Open Questions (Q2 below) | CONTEXT.md and ROADMAP Phase 126 text reference only `db/net/...` paths; Phase 128 FRAME-01 explicitly touches both `db/net/framing.h` and `cli/src/connection.cpp`. If the planner wants symmetry, the same runtime assertion + `static_assert` could be added to the CLI side. Flagged so the user can decide. |

## Open Questions

1. **Release-mode behavior of the runtime assertion.**
   - What we know: `assert()` is compiled out in release builds. If the invariant breaks in production, there is no detection.
   - What's unclear: does the project want a release-safe check (`if (cond) { spdlog::critical(...); std::abort(); }`) or is debug-only sufficient?
   - Recommendation: start with `assert()` (debug only). The invariant is tested in CI via the new TEST_CASE (run under debug builds by default), and the `static_assert` survives release builds. A release-mode abort would be heavier and CONTEXT.md doesn't request it. If the planner disagrees, the change is mechanical.

2. **Should the CLI-side pipeline (`cli/src/connection.cpp`) receive the same treatment?**
   - What we know: The CLI has a structurally identical send pipeline at `cli/src/connection.cpp:628-669` (`send()` bifurcates on `STREAMING_THRESHOLD`, `send_chunked` splits into 1 MiB sub-frames). CONTEXT.md does not mention the CLI. Phase 128 FRAME-01 explicitly touches both `db/net/framing.h` AND `cli/src/connection.cpp` (see REQUIREMENTS.md line 19).
   - What's unclear: does Phase 126 pin the invariant only on the node (db/) or also on the CLI? D-01 says "every large payload must go through Connection::send_message's >= STREAMING_THRESHOLD branch" — "Connection" is the `db/net` class, not the CLI's `Connection` class.
   - Recommendation: the planner should decide. Cleanest position: Phase 126 is node-side only (follows CONTEXT.md verbatim); CLI-side mirroring is a planner-discretion add-on or a deferred item. Adding it now is ~3 small edits (one assertion, one `static_assert` against CLI's local `MAX_FRAME_SIZE`, one test if the CLI has a test harness). Scope-wise, the user explicitly asked the audit to cover "the send side" — that may or may not include the client.

3. **Where does `TRANSPORT_ENVELOPE_MARGIN` live?**
   - What we know: The constant is used by the runtime assertion in `enqueue_send`. `framing.h` is a natural home because it houses related framing constants. Alternative: a `static constexpr` inside `connection.cpp`.
   - What's unclear: project style preference.
   - Recommendation: put it in `framing.h` next to `FRAME_HEADER_SIZE`. That keeps all per-frame overhead constants in one place and matches the pattern of `MAX_FRAME_SIZE` / `STREAMING_THRESHOLD` already living there.

## Sources

### Primary (HIGH confidence — direct code read)

- `db/net/framing.h` (full file read) — MAX_FRAME_SIZE, STREAMING_THRESHOLD, FRAME_HEADER_SIZE, existing `static_assert`.
- `db/net/framing.cpp` (full file read) — read_frame enforcement at line 49, write_frame at lines 18-35, AEAD wrap with 16-byte tag.
- `db/net/connection.h` (full file read) — public send_message API, private enqueue_send / send_encrypted / send_raw, MAX_SEND_QUEUE, ReassembledChunked struct.
- `db/net/connection.cpp` — read lines 1-180, 280-760, 880-1099 (covers send_raw, send_encrypted, every handshake branch, message_loop, recv_chunked, send_message_chunked, send_message, enqueue_send, drain_send_queue, close_gracefully).
- `db/net/protocol.cpp` (full file read) — TransportCodec::encode envelope construction.
- `db/schemas/transport.fbs` (full file read) — TransportMessage schema (3 fields: type, payload, request_id).
- `db/crypto/aead.h` (lines 1-42) — TAG_SIZE = 16, NONCE_SIZE = 12.
- `db/peer/message_dispatcher.cpp` — full scan of every `send_message` call site (lines 74, 266, 374, 452, 460, 464, 579, 612, 647, 721, 794, 838, 895, 923, 931, 980, 1029, 1075, 1195, 1235, 1271, 1371, 1428, 1450, 1456), BatchReadResponse MAX_CAP at line 1103.
- `db/peer/sync_orchestrator.cpp` — 20+ send_message call sites confirmed at lines 151, 190, 315, 376, 390, 434, 456, 506, 545, 582, 590, 628, 653, 799, 808, 867, 883, 941, 989, 1034.
- `db/peer/blob_push_manager.cpp` (lines 60-176) — BlobNotify/Notification/BlobFetch/BlobFetchResponse paths; fixed 77-byte notification payload.
- `db/peer/peer_manager.cpp` (lines 105, 121, 606) — PEX + SIGHUP re-announce fan-out.
- `db/peer/pex_manager.cpp` (lines 50-131) — PEX request/response, MAX_PEERS_PER_EXCHANGE = 8 cap.
- `db/peer/connection_manager.cpp` (lines 253-285) — announce_and_sync SyncNamespaceAnnounce path.
- `db/sync/sync_protocol.cpp` (lines 260-272) — encode_single_blob_transfer wraps an encoded blob.
- `db/tests/net/test_connection.cpp` — fixture patterns at lines 32-218 (basic handshake + send), 475-542 (send_queue fan-out), 602-676 (Pong-via-send_message), 747-959 (mock-MITM + nonce exhaustion).
- `db/tests/net/test_framing.cpp` (lines 1-236) — primitive-level round-trip patterns, existing 1 MiB test at lines 207-216, MAX_FRAME_SIZE rejection test at 149-160.
- `db/main.cpp` (lines 450-750) — confirmation that the `send_encrypted` lambda at line 571 is a local UDS-client helper, not Connection::send_encrypted.
- `cli/src/connection.cpp` (lines 625-695) — confirmation of structurally parallel CLI send pipeline.
- `cli/src/connection.h` (lines 88-91) — CLI's send_encrypted declaration.
- `grep -rn "enqueue_send" db/` — one caller.
- `grep -rn "send_encrypted\|enqueue_send\|send_raw" db/` excluding connection.cpp — zero external callers.

### Secondary (MEDIUM confidence — project docs cited)

- `.planning/REQUIREMENTS.md` — AUDIT-01, AUDIT-02 requirement text.
- `.planning/ROADMAP.md` lines 730-762 — Phase 126 goal and success criteria.
- `.planning/STATE.md` — confirms Phase 126 is current, no prior plans.
- `.planning/PROJECT.md` line 372-373 — "Silent SyncRequest drop" and "recv_sync_msg executor transfer" decisions informing Pitfall 4.

### Tertiary (LOW confidence — none)

None. The research is entirely codebase-grounded.

## Metadata

**Confidence breakdown:**

- Send-path inventory: HIGH — every call site was read directly. 30+ `send_message` sites in `db/peer/`; 13 handshake direct-primitive sites in `db/net/connection.cpp`; zero bypass sites.
- Assertion site selection: HIGH — `grep -rn enqueue_send db/` confirms single caller; chunked path at `connection.cpp:1017-1022` confirms the `send_encrypted` alternative over-triggers.
- Static-assert shape: HIGH — overhead components verified against `framing.h`, `framing.cpp`, `aead.h`. `2 * STREAMING_THRESHOLD` is tight enough to catch drift and loose enough to survive Phase 128's exact-equality case.
- Test harness: HIGH — fixture pattern lifted directly from existing passing tests in `test_connection.cpp`.
- Pitfalls: HIGH — driven by code reading and explicit memory-note constraints.

**Research date:** 2026-04-22
**Valid until:** 2026-05-22 (30 days; the code under audit is stable and has no planned changes before Phase 128)
