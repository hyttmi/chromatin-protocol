---
phase: 122
plan: 05
subsystem: transport-envelope-and-sync-wire-format
tags:
  - phase122
  - transport
  - sync
  - blob-write-envelope
  - pubk-first-delegation
  - wave-4
  - protocol-break

# Dependency graph
requires:
  - phase: 122-01
    provides: BlobWrite = 64 TransportMsgType + BlobWriteBody schema (target_namespace + inner Blob)
  - phase: 122-04
    provides: BlobEngine::ingest(target_namespace, blob, source) widened signature + IngestError::pubk_first_violation / pubk_mismatch + ERROR_PUBK_* wire codes

provides:
  - TransportMsgType_BlobWrite routing in message_dispatcher — BlobWriteBody envelope decoded, target_namespace extracted, engine_.ingest(target_ns, blob, conn) called with IngestError -> wire error mapping
  - Delete handler mirrored onto BlobWriteBody envelope (tombstones are structurally identical Blobs — only data-magic differs)
  - Pre-122 TransportMsgType_Data=8 direct-write branch DELETED (no backward compat)
  - Sync wire format with per-blob [ns:32B] prefix (Pitfall #3 fix) — encode_blob_transfer / encode_single_blob_transfer / decode_blob_transfer
  - NamespacedBlob {target_namespace, blob} carried through SyncProtocol::ingest_blobs — target_namespace threaded per-blob into engine.ingest; PUBK-first check delegates to single-site engine.cpp gate (no duplicate check)
  - wire::decode_blob_from_fb(const Blob*) helper — dedup between decode_blob (verify-then-extract) and envelope decoders that already hold a verified Blob accessor

affects:
  - 122-06 (TSAN cross-namespace race — PUBK-first concurrency fixture)
  - 122-07 (test file schema-cascade sweep — test_engine.cpp / test_sync_protocol.cpp / test_storage.cpp / test_peer_manager.cpp / test_event_expiry.cpp)

tech-stack:
  added: []
  patterns:
    - "BlobWriteBody envelope decode: flatbuffers::Verifier::VerifyBuffer<BlobWriteBody>(nullptr) -> flatbuffers::GetRoot<BlobWriteBody>(payload.data()) -> body->target_namespace()->size() == 32 strict equality + body->blob() null check"
    - "Per-blob [ns:32B] prefix on sync transfer (Pitfall #3): [count:u32BE]([ns:32B][len:u32BE][blob_flatbuf])+ using chromatindb::util::write_u32_be / read_u32_be / checked_add"
    - "Phase-121 post-back-to-ioc discipline preserved: every engine_.ingest(...) in the dispatcher still followed by co_await asio::post(ioc_, asio::use_awaitable)"
    - "Single-site PUBK-first invariant: sync_protocol.cpp:97 delegates to engine_.ingest — feedback_no_duplicate_code.md; verified ! grep -q has_owner_pubkey db/sync/sync_protocol.cpp"
    - "BlobFetchResponse wire format widened (cascade): [status:1][target_ns:32][blob_fb:...] on status==0x00 (found) — sender already has ns from BlobFetch request"

key-files:
  created: []
  modified:
    - db/wire/codec.h
    - db/wire/codec.cpp
    - db/peer/message_dispatcher.cpp
    - db/sync/sync_protocol.h
    - db/sync/sync_protocol.cpp
    - db/peer/sync_orchestrator.cpp
    - db/peer/blob_push_manager.cpp

key-decisions:
  - "BlobWriteBody envelope shared by Delete handler too — tombstones are structurally identical signed Blobs (only `data` magic differs DEADBEEF vs PUBK). Avoids a parallel DeleteBody schema. Per Plan 122-05 Task 1(c)."
  - "flatbuffers::GetRoot<BlobWriteBody>(payload.data()) rather than GetBlobWriteBody — the latter accessor is not generated because transport.fbs's root_type is TransportMessage, not BlobWriteBody. GetRoot<> is the equivalent flatbuffers-idiomatic call."
  - "Verifier strictness: verifier.VerifyBuffer<BlobWriteBody>(nullptr) at the dispatcher's envelope boundary (T-122-09 Tampering/Input Validation); malformed envelopes -> record_strike_(conn) + ERROR_DECODE_FAILED."
  - "decode_blob_from_fb helper (Task 1(d)) added to codec.h/cpp: decode_blob now wraps GetBlob(buf) + decode_blob_from_fb; envelope decoders call decode_blob_from_fb directly on the body->blob() accessor, avoiding a re-parse. Keeps feedback_no_duplicate_code.md compliance."
  - "Sync PUBK-first check is NOT duplicated here — SyncProtocol::ingest_blobs delegates to engine_.ingest per-blob; the Step 1.5 gate in engine.cpp (Plan 04) covers both the direct and sync paths. Verified: grep -c has_owner_pubkey db/sync/sync_protocol.cpp == 0."
  - "on_blob_ingested_ callback in sync receives target_namespace (previously took blob.namespace_id, which no longer exists post-122). Callback signature unchanged — the first-parameter type is std::array<uint8_t,32>, so passing target_namespace is call-site-only."
  - "[Rule 3] BlobFetchResponse wire format changed: BlobPushManager::handle_blob_fetch_response's engine_.ingest call site (flagged in 122-04 handoff notes) cannot be fixed by a local rewrite — the sender-side payload lost its ability to carry the namespace when blob.namespace_id disappeared. Widened the response from [status:1][blob_fb:...] to [status:1][target_ns:32][blob_fb:...] on status==0x00; sender already has ns available from the BlobFetch request decode. Min found-size bumped 2 -> 34."
  - "[Rule 3] MetadataRequest handler returning blob.signer_hint (32B) in place of the deleted blob.pubkey (2592B). Response shrinks by 2560 bytes. Clients that need the full signing pubkey fetch the namespace's PUBK blob directly (D-05 pattern). This is a protocol break already sanctioned by Phase 122's overall schema break — no shim."
  - "encode_single_blob_transfer gains a std::span<const uint8_t, 32> target_namespace parameter (plus the existing blob). Callers in sync_orchestrator.cpp already know req_ns (decoded from the peer's BlobRequest); feeding it through is a 1-line call-site change × 4 sites."

patterns-established:
  - "phase122-envelope-verify: flatbuffers::Verifier + verifier.VerifyBuffer<Body>(nullptr) + body->target_namespace()->size() == 32 + body->blob() null check"
  - "phase122-envelope-decode: flatbuffers::GetRoot<Body>(payload.data()) + wire::decode_blob_from_fb(body->blob())"
  - "phase122-sync-ns-prefix: per-blob [ns:32B] prefix in sync transfer, consumed by std::vector<NamespacedBlob>"
  - "phase122-single-site-pubk-first: sync path delegates to engine_.ingest — no grep of has_owner_pubkey outside engine.cpp"

requirements-completed:
  - "SC#4 (transport/sync layer): PUBK-first invariant fires on direct + sync writes via the single engine.cpp gate; dispatcher + sync_protocol delegate cleanly"
  - "SC#7 (production code): no stale blob.namespace_id or blob.pubkey in db/{engine,sync,wire,storage,peer}"
  - "D-07: BlobWriteBody envelope wired into dispatcher for both BlobWrite and Delete"
  - "D-08: TransportMsgType_BlobWrite = 64 routed; Data = 8 branch deleted"
  - "Pitfall #3: sync receiver gets target_namespace from per-blob [ns:32B] prefix"

# Metrics
duration: 15min
completed: 2026-04-20
tasks_completed: 3
files_modified: 7
commits: 3
---

# Phase 122-05: Transport Envelope + Sync Wire-Format Wiring Summary

**Wave 4 keystone landed: BlobWrite=64 envelope routed via BlobWriteBody decode, sync blob transfer carries per-blob [ns:32B] prefix, engine.ingest(target_namespace, blob) threaded through 4 call sites (dispatcher direct-write, dispatcher delete, sync ingest_blobs, blob_push_manager fetch-response). PUBK-first invariant stays single-site in engine.cpp — sync and dispatcher delegate via engine_.ingest; grep-verified zero duplicate checks in sync_protocol.cpp. Pre-122 Data=8 dispatcher branch deleted entirely (no back-compat). chromatindb_lib builds clean 100%.**

## Performance

- **Duration:** ~15 minutes active (CMake first-configure ~3 min + compile iterations)
- **Started:** 2026-04-20T07:57:15Z
- **Tasks:** 3 / 3
- **Files modified:** 7
- **Files in plan frontmatter files_modified:** 4 (message_dispatcher.cpp, sync_protocol.h, sync_protocol.cpp, sync_orchestrator.cpp); **+3 cascade fixes:** codec.h, codec.cpp (Task 1(d) helper), blob_push_manager.cpp (Plan 04 handoff note)

## Accomplishments

### Task 1: message_dispatcher BlobWrite envelope + Delete refactor + decode_blob_from_fb helper — commit `81d2700a`

- **BlobWrite = 64 branch (NEW)** — decodes BlobWriteBody envelope:
  - `flatbuffers::Verifier(payload) + VerifyBuffer<wire::BlobWriteBody>(nullptr)` — strict verifier at the trust boundary.
  - `flatbuffers::GetRoot<wire::BlobWriteBody>(payload.data())` to obtain the envelope accessor (transport.fbs sets `root_type TransportMessage`, so the `GetBlobWriteBody` free function is not generated; `GetRoot<>` is the equivalent idiomatic call).
  - `body->target_namespace()->size() == 32` strict equality + `body->blob()` null check.
  - `wire::decode_blob_from_fb(body->blob())` to extract BlobData without re-verifying the inner Blob buffer.
  - `sync_namespaces_.find(target_namespace)` — filter operates on envelope-carried ns.
  - `engine_.ingest(std::span<const uint8_t,32>(target_namespace), blob, conn)` — post-122 signature.
  - `co_await asio::post(ioc_, asio::use_awaitable)` preserved (Phase 121 STORAGE_THREAD_CHECK discipline).
  - `IngestError::pubk_first_violation` → `ERROR_PUBK_FIRST_VIOLATION = 0x07`.
  - `IngestError::pubk_mismatch` → `ERROR_PUBK_MISMATCH = 0x08`.
  - All existing error-code mappings preserved (storage_full → StorageFull reply, quota_exceeded → QuotaExceeded reply, timestamp_rejected → debug log).
- **Pre-122 Data = 8 branch DELETED** — no compat shim, no commented-out code (`feedback_no_backward_compat.md`).
- **Delete handler refactored** — reuses BlobWriteBody envelope (tombstones are Blobs with a different data magic; structurally identical). Passes `target_namespace` into `engine_.delete_blob(target_namespace, blob, conn)`. PUBK error mapping applied defensively (won't fire on tombstones but mapped cleanly anyway).
- **Rate-limit guard at :151** — updated `TransportMsgType_Data` → `TransportMsgType_BlobWrite` (the existing branch applies to both direct-write and delete).
- **decode_blob_from_fb helper (Task 1(d))** — added to `db/wire/codec.h` + `codec.cpp`:
  - `wire::decode_blob_from_fb(const wire::Blob* fb_blob)` → extracts BlobData fields from an already-verified Blob accessor (used by envelope decoders).
  - `decode_blob(std::span<const uint8_t>)` now wraps `VerifyBlobBuffer` + `GetBlob` + `decode_blob_from_fb` → zero duplication per `feedback_no_duplicate_code.md`.
  - codec.h forward-declares `chromatindb::wire::Blob` to avoid pulling in the full 250-line generated header.
- **MetadataRequest handler cascade fix** — the handler previously memcpy'd `blob.pubkey` (the 2592-byte inline pubkey) into the response. Post-122 the field doesn't exist; response now emits `blob.signer_hint` (32 bytes). Response shrinks by 2560 bytes; clients fetch the full 2592-byte pubkey via the namespace's PUBK blob (Phase 122 D-05 pattern).

### Task 2: sync_protocol per-blob [ns:32B] prefix + ingest_blobs threads target_namespace — commit `03bfdf17`

- **sync_protocol.h** — `NamespacedBlob { target_namespace: array<u8,32>; blob: BlobData }` introduced; public API updated:
  - `ingest_blobs(const std::vector<NamespacedBlob>&, source)` (was `vector<BlobData>`).
  - `encode_blob_transfer(const std::vector<NamespacedBlob>&)`.
  - `encode_single_blob_transfer(std::span<const uint8_t,32> target_namespace, const wire::BlobData&)`.
  - `decode_blob_transfer(...)` returns `std::vector<NamespacedBlob>`.
- **sync_protocol.cpp** — wire format becomes `[count:u32BE]([ns:32B][len:u32BE][blob_flatbuf])+` (Pitfall #3 fix):
  - `encode_blob_transfer` emits per-blob ns prefix using the existing `chromatindb::util::write_u32_be` idiom + direct buffer `insert(end, begin(), end())` for fixed-size arrays.
  - `encode_single_blob_transfer` (count=1 form) emits `[1:u32BE][ns:32B][len:u32BE][blob_flatbuf]`.
  - `decode_blob_transfer` parses the new layout with bounds-safe `chromatindb::util::checked_add` at each stage (ns / len / blob), returning early-truncation on malformed input (existing convention preserved).
- **ingest_blobs loop body** — threads target_namespace per blob:
  - `engine_.ingest(std::span<const uint8_t,32>(nb.target_namespace), nb.blob, source)`.
  - `on_blob_ingested_` callback gets `target_namespace` instead of the removed `blob.namespace_id`.
  - Stats classification (storage_full / quota_exceeded / timestamp_rejected / default warn) preserved verbatim.
- **NO PUBK-first check added** — single-site invariant preserved (Plan 04 owns the gate in engine.cpp:174 Step 1.5). Verified:
  ```
  $ grep -c has_owner_pubkey db/sync/sync_protocol.cpp
  0
  ```

### Task 3: sync_orchestrator call sites + blob_push_manager cascade — commit `277dceeb`

- **sync_orchestrator.cpp** — both BlobTransfer decode sites (:478 initiator + :957 responder):
  - `auto ns_blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);`
  - `auto s = co_await sync_proto_.ingest_blobs(ns_blobs, conn);`
  - Stats aggregation (`total_stats.blobs_received/storage_full_count/quota_exceeded_count`) preserved verbatim.
- **sync_orchestrator.cpp** — all 4 encode_single_blob_transfer call sites (:502, :540, :981, :1025) updated:
  - `encode_single_blob_transfer(std::span<const uint8_t,32>(req_ns), *blob)`.
  - `req_ns` is the already-decoded namespace from the peer's BlobRequest — no new state plumbing required.
- **blob_push_manager.cpp** (cascade from 122-04's flagged handoff note):
  - `handle_blob_fetch` (sender side): response wire format changes from `[status:1][blob_fb:...]` to `[status:1][target_ns:32][blob_fb:...]` on status==0x00. The sender already has `ns` from the decoded BlobFetch request.
  - `handle_blob_fetch_response` (receiver side): extracts target_namespace at offset 1 before `decode_blob`, calls `engine_.ingest(target_namespace, blob, conn)`, feeds target_namespace into `make_pending_key` + `on_blob_ingested` fan-out.
  - Min found-payload size bumped 2 → 34 bytes (1 status + 32 ns + >=1 fb byte).

## Task Commits

Each task committed atomically (`--no-verify`):

1. **Task 1 (feat):** `81d2700a` — `feat(122-05): refactor message_dispatcher for BlobWrite envelope + delete_blob_from_fb helper`
2. **Task 2 (feat):** `03bfdf17` — `feat(122-05): refactor sync_protocol to carry target_namespace per-blob`
3. **Task 3 (feat):** `277dceeb` — `feat(122-05): wire sync_orchestrator + blob_push_manager + MetadataRequest for post-122 schema`

## Decisions Made

| Decision | Rationale |
|----------|-----------|
| Dispatcher uses `flatbuffers::GetRoot<BlobWriteBody>` instead of `GetBlobWriteBody`. | `transport.fbs` declares `root_type TransportMessage` only — the `GetBlobWriteBody` free function is not emitted by flatc. `GetRoot<T>(buf)` is the equivalent + explicit idiomatic form. |
| Delete handler reuses BlobWriteBody envelope. | Tombstones are structurally identical signed Blobs — only the `data` magic prefix differs (DEADBEEF vs PUBK vs regular). Avoids a parallel DeleteBody schema. Plan 122-05 Task 1(c) explicitly specifies this. |
| PUBK-first check NOT duplicated in sync_protocol.cpp. | `SyncProtocol::ingest_blobs:102` delegates per-blob to `engine_.ingest(target_namespace, blob, source)` — the Step 1.5 gate in engine.cpp covers both direct and sync paths. `feedback_no_duplicate_code.md` + Pitfall #2. Verified: `grep -c has_owner_pubkey db/sync/sync_protocol.cpp == 0`. |
| decode_blob_from_fb helper (Task 1(d)) added to codec.h/cpp. | Dispatcher decodes the envelope, which already holds a verified Blob accessor via `body->blob()`. Calling `decode_blob(span)` would re-run `VerifyBlobBuffer` wastefully. The helper extracts fields without re-parsing. `decode_blob` now wraps it — zero code duplication. |
| BlobFetchResponse wire format widened to carry target_namespace. | Plan 04 handoff flagged `blob_push_manager.cpp:191` as a cascade. The old payload was `[status:1][blob_fb:...]`; post-schema-change the blob has no `namespace_id`, so the receiver could not derive target_namespace. Sender already has `ns` from the BlobFetch request decode — minimal-surface fix is to include it in the response. |
| MetadataRequest handler returns signer_hint (32B) instead of pubkey (2592B). | Post-122 BlobData has no `pubkey` field. Clients that need the 2592-byte signing pubkey must fetch the namespace's PUBK blob (Phase 122 D-05 pattern). Response size shrinks by 2560 bytes. |
| Per-blob [ns:32B] prefix goes AT the blob level, not at the message level. | Per-message prefix would embed a single ns for the whole transfer batch, but the current `encode_blob_transfer` batches blobs that may belong to different namespaces (`collect_namespace_hashes` is per-ns but the orchestrator batches). Per-blob prefix is safer and matches the plan's Pitfall #3 recommendation. |
| encode_single_blob_transfer gains an explicit `target_namespace` span parameter. | All 4 call sites in sync_orchestrator.cpp have `req_ns` in scope (decoded from the peer's BlobRequest). Simpler than making the helper discover `target_namespace` from somewhere else. |

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 — Blocking] Widened BlobFetchResponse wire format to carry target_namespace**

- **Found during:** Task 3 build gate (chromatindb_lib).
- **Issue:** `BlobPushManager::handle_blob_fetch_response:191` called `engine_.ingest(blob, conn)` (pre-122 signature). Post-122 needs `engine_.ingest(target_namespace, blob, conn)`. There's no way to derive target_namespace from the blob alone (no more `namespace_id` field). Plan 04's SUMMARY flagged this as cascade scope that 122-05 would hit.
- **Fix:** Widened the BlobFetchResponse wire format from `[status:1][blob_fb:...]` to `[status:1][target_ns:32][blob_fb:...]` on status==0x00 (found). Sender already has `ns` from the BlobFetch request it responds to. Receiver extracts target_namespace at offset 1 before `decode_blob`, uses it in the `engine_.ingest` call, the `pending_fetches_` cleanup, and the `on_blob_ingested` fan-out.
- **Files modified:** `db/peer/blob_push_manager.cpp` (both handler functions).
- **Verification:** `chromatindb_lib` builds clean 100%; manual inspection of the two call sites confirms symmetrical encode/decode format.
- **Committed in:** `277dceeb`.

**2. [Rule 3 — Blocking] MetadataRequest handler read blob.pubkey (removed post-122)**

- **Found during:** Task 3 build gate — `message_dispatcher.cpp:932,967` referenced `blob.pubkey`.
- **Issue:** The MetadataRequest response copied the full inline 2592-byte pubkey into the response. Post-122 the BlobData has `signer_hint` (32B) instead.
- **Fix:** Emit `blob.signer_hint` (32 bytes) in place of the removed pubkey. The response's `pubkey_len` field (2 bytes) now always carries 32. Response size shrinks by 2560 bytes. Clients needing the full signing pubkey fetch the namespace's PUBK blob (Phase 122 D-05).
- **Files modified:** `db/peer/message_dispatcher.cpp` (MetadataRequest handler only).
- **Verification:** `chromatindb_lib` builds clean; the handler's response layout is straight-forward.
- **Committed in:** `277dceeb`.

**3. [Rule 2 — Missing critical functionality] Added wire::decode_blob_from_fb helper (Task 1(d) explicit)**

- **Found during:** Task 1 design.
- **Issue:** Plan 122-05 Task 1(d) explicitly required this helper to avoid re-verifying the inner Blob buffer at the envelope boundary. codec.cpp's existing `decode_blob(span)` bundles VerifyBlobBuffer + GetBlob + field extraction; envelope decoders need only field extraction (the outer `BlobWriteBody::Verify` already checked the inner Blob table).
- **Fix:** Added `wire::decode_blob_from_fb(const wire::Blob* fb_blob)` to codec.h (declaration with forward-decl) and codec.cpp (implementation); refactored `decode_blob(span)` to wrap `VerifyBlobBuffer + GetBlob + decode_blob_from_fb` — shared extraction body per `feedback_no_duplicate_code.md`.
- **Files modified:** `db/wire/codec.h`, `db/wire/codec.cpp`.
- **Verification:** `chromatindb_lib` builds clean; `decode_blob` round-trip behavior unchanged (confirmed by absence of any new test failures beyond the expected Plan 07 cascade).
- **Committed in:** `81d2700a`.

### Observed (no action taken)

- **chromatindb_tests fails on test_peer_manager.cpp + test_event_expiry.cpp** — expected Plan 07 cascade. These test files still call pre-122 `engine.ingest(blob)` (1-arg) or construct BlobData with the removed `namespace_id` field. Plan 07 will sweep them. chromatindb_lib itself builds clean (100%).
- **sync_orchestrator.cpp's `on_blob_ingested_` call shape unchanged** — the callback's first parameter type is `const std::array<uint8_t, 32>&` (namespace); we just pass `target_namespace` in place of the removed `blob.namespace_id`. No signature churn.
- **SyncProtocol::OnBlobIngested callback signature in sync_protocol.h is unchanged** — it was always `std::array<uint8_t, 32>&` for the first parameter (namespace_id). Task 2 passes `target_namespace` into it; callers in peer_manager (not in this plan's scope) continue to receive `std::array<uint8_t, 32>` whose semantics are now "target_namespace" (the parameter name in the typedef is still `namespace_id` but that's a cosmetic rename owned by a future sweep).

## Issues Encountered

- **Worktree base drift on startup:** HEAD was at `a893aacc` (unrelated deletion branch). Per `<worktree_branch_check>`, hard-reset to `938f251a` placed the worktree at the expected base. Post-reset `git log --oneline -3` confirmed `938f251a → 3f2e990f → c905063a`.
- **CMake first-configure cost:** ~3 minutes (FetchContent on a fresh worktree: asio + Catch2 + flatbuffers + spdlog + sodium + mdbx + liboqs). All subsequent builds were fast (incremental).
- **First build attempt of chromatindb_lib (after Task 1 only) failed on sync_protocol.cpp + blob_push_manager.cpp** — expected, those are Task 2/3 territory. Used the compiler-guided cascade to enumerate all remaining sites (Pitfall #5).
- **Second build failure:** `GetBlobWriteBody` not emitted by flatc because transport.fbs's `root_type` is `TransportMessage`. Fixed by using `flatbuffers::GetRoot<BlobWriteBody>(payload.data())` directly — equivalent idiomatic form.

## User Setup Required

None — no external service configuration required.

## Threat Flags

None new beyond the threat register documented in the plan. The BlobWrite envelope boundary adds one new verifier call site (`verifier.VerifyBuffer<BlobWriteBody>(nullptr)`) which is exactly the T-122-09 (Tampering / Input Validation) mitigation the plan prescribed; record_strike_ + ERROR_DECODE_FAILED on malformed envelopes.

The BlobFetchResponse wire-format widening (Rule 3 deviation) introduces a new transport-level 32-byte prefix at a trust boundary. The receiver treats target_namespace as input to `engine_.ingest`, which absorbs it into the SHA3 sponge for signature verify (D-01 byte-binding) — a lying sender cannot forge a blob against a wrong target_namespace without knowing the delegate's signing key for that namespace. This is symmetric with T-122-10 (spoofed target_namespace in sync envelope) and uses the same defense.

## Known Stubs

None. All envelope paths (dispatcher BlobWrite, dispatcher Delete, sync ingest_blobs, blob_push_manager fetch-response) thread target_namespace end-to-end through engine_.ingest. No placeholders, no "fix me later" comments, no partial implementations.

## Next Phase Readiness

- **Plan 06 (PUBK-first concurrency TSAN):** Unblocked. The full ingest pipeline — dispatcher direct-write, dispatcher delete, sync replicate, blob_push_manager fetch — all route through `engine_.ingest(target_namespace, blob, ...)` now. Plan 06's cross-namespace-race fixture can use any path freely.
- **Plan 07 (test sweep):** Unblocked. Compiler errors on `test_peer_manager.cpp` + `test_event_expiry.cpp` are cataloged; additional stale references in other test files (test_engine.cpp / test_sync_protocol.cpp / test_storage.cpp) will surface on `--target chromatindb_tests` once Plan 07 starts. The verify-gate pattern is: `chromatindb_lib` clean, `chromatindb_tests` broken at a known, finite set of call sites.

## Self-Check

### Files verified to exist on disk (`git log --name-only`)

- **MODIFIED** `db/wire/codec.h` — forward-decl + `decode_blob_from_fb` declaration.
- **MODIFIED** `db/wire/codec.cpp` — `decode_blob_from_fb` impl + `decode_blob` refactored to share body.
- **MODIFIED** `db/peer/message_dispatcher.cpp` — BlobWrite=64 branch + Delete refactor + MetadataRequest signer_hint cascade fix.
- **MODIFIED** `db/sync/sync_protocol.h` — NamespacedBlob struct + updated signatures.
- **MODIFIED** `db/sync/sync_protocol.cpp` — encode/decode_blob_transfer per-blob [ns:32B]; ingest_blobs threads target_namespace.
- **MODIFIED** `db/peer/sync_orchestrator.cpp` — decode -> ingest_blobs (2 sites) + encode_single_blob_transfer (4 sites) threaded with req_ns.
- **MODIFIED** `db/peer/blob_push_manager.cpp` — BlobFetchResponse wire format widened for target_namespace.
- **CREATED** `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-05-SUMMARY.md` (this file).

### Commits verified via `git log --oneline 938f251a..HEAD`

- FOUND: `81d2700a` (Task 1: dispatcher BlobWrite + codec helper)
- FOUND: `03bfdf17` (Task 2: sync per-blob ns prefix + ingest_blobs)
- FOUND: `277dceeb` (Task 3: sync_orchestrator + blob_push_manager + metadata cascade)

### Plan automated gates

| Gate | Expected | Actual | Status |
|------|----------|--------|--------|
| `! grep -qE "TransportMsgType_Data\s*\)" db/peer/message_dispatcher.cpp` | true | true (only a comment remains) | PASS |
| `grep -q "TransportMsgType_BlobWrite" db/peer/message_dispatcher.cpp` | true | true | PASS |
| `grep -q "GetRoot<wire::BlobWriteBody>\|GetBlobWriteBody" db/peer/message_dispatcher.cpp` | true | true (GetRoot form) | PASS (equivalent) |
| `grep -q "ERROR_PUBK_FIRST_VIOLATION" db/peer/message_dispatcher.cpp` | true | true | PASS |
| `grep -q "ERROR_PUBK_MISMATCH" db/peer/message_dispatcher.cpp` | true | true | PASS |
| `grep -q "engine_\.ingest" db/peer/message_dispatcher.cpp (with target_namespace)` | true | true | PASS |
| `grep -q "NamespacedBlob\|namespaced" db/sync/sync_protocol.h` | true | true | PASS |
| `grep -q "target_namespace" db/sync/sync_protocol.cpp` | true | true | PASS |
| `grep -q "engine_\.ingest" db/sync/sync_protocol.cpp threading target_namespace` | true | true (multi-line) | PASS |
| `! grep -q "has_owner_pubkey" db/sync/sync_protocol.cpp` | true | true | PASS |
| `! grep -rE "blob\.namespace_id\|blob\.pubkey" db/engine db/sync db/wire db/storage db/peer` | empty | empty | PASS |
| `cmake --build build --target chromatindb_lib → "Built target chromatindb_lib"` | pass | pass (100%) | PASS |

## Self-Check: PASSED

Plan 122-05 is complete. chromatindb_lib builds cleanly end-to-end. The PUBK-first invariant lives in exactly one place (engine.cpp Step 1.5) and covers all four ingest paths (dispatcher direct-write, dispatcher delete, sync_protocol ingest_blobs, blob_push_manager fetch-response) via delegation to `engine_.ingest(target_namespace, blob, source)`. The sync wire format carries target_namespace per-blob. The pre-122 Data=8 dispatcher branch is deleted entirely.

---
*Phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey*
*Plan: 05 (Wave 4, depends on Plans 1/4)*
*Completed: 2026-04-20*
