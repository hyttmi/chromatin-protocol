---
phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
verified: 2026-04-20T15:01:27Z
status: passed
score: 7/7 must-haves verified
overrides_applied: 0
re_verification:
  previous_status: null
  previous_score: null
  gaps_closed: []
  gaps_remaining: []
  regressions: []
human_verification:
  - test: "Two-node live wire sanity — PUBK-first rejection observed over the network"
    expected: "cdb attempting a non-PUBK write to a fresh namespace on a freshly-wiped node receives a protocol-level error (ERROR_PUBK_FIRST_VIOLATION). After publishing PUBK, retry succeeds."
    why_human: "Automated tests exercise in-process; over-the-wire behavior with a real peer at 192.168.1.73 is declared a manual check in VALIDATION.md — node-to-node framing and error surfacing via TCP cannot be asserted in the current harness without spinning a live daemon. Deferred to user because it is classified as 'observational sanity, not gate conditions' in VALIDATION.md."
  - test: "Blob size reduction confirmed ~35% smaller on the wire"
    expected: "encode_blob_transfer output for a minimal 1-byte write is ≥2592 bytes smaller post-122 than pre-122."
    why_human: "Byte-count sanity on a real transfer requires running a node; the size-shrink invariant test_schema_phase122.cpp covers the encoded-blob size at the codec layer (<500 bytes minimum vs 2750+ pre-122), but the end-to-end wire-level drop across the transport envelope is declared manual in VALIDATION.md."
---

# Phase 122: Schema + Signing Cleanup — Verification Report

**Phase Goal:** One coordinated protocol-breaking change that shrinks every signed blob by ~2592 bytes (~35%) and removes redundant fields from the schema before the v1 freeze.

**Verified:** 2026-04-20T15:01:27Z
**Status:** passed (with manual live-wire sanity checks deferred to user per VALIDATION.md)
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths (Success Criteria)

| # | Truth (Success Criterion) | Status | Evidence |
|---|---------------------------|--------|----------|
| 1 | `Blob.namespace_id` field removed from the schema | VERIFIED | `db/schemas/blob.fbs` lines 3-9 declare only `{signer_hint, data, ttl, timestamp, signature}`. Regenerated `db/wire/blob_generated.h` has no `namespace_id()` accessor; grep confirms zero matches for `namespace_id|pubkey\s*\(` in generated header. |
| 2 | Per-blob `pubkey` (2592 bytes) replaced by `signer_hint` (32 bytes) resolved via new `owner_pubkeys` DBI or `delegation_map` | VERIFIED | `blob.fbs:3` declares `signer_hint:[ubyte]` only (no pubkey). `BlobData` struct in `codec.h:17-23` holds `std::array<uint8_t, 32> signer_hint`. Engine Step 2 (`engine.cpp:197-264`) resolves via `storage_.get_owner_pubkey(blob.signer_hint)` (owner) then `storage_.get_delegate_pubkey_by_hint(target_namespace, blob.signer_hint)` (delegate). |
| 3 | Signing canonical form updated: `SHA3(target_namespace \|\| data \|\| ttl \|\| timestamp)` per D-01 | VERIFIED | `codec.h:55-59` + `codec.cpp:92-127` declare `build_signing_input(target_namespace, data, ttl, timestamp)` with incremental SHA3-256 sponge order `target_namespace → data → ttl_be32 → timestamp_be64`. Golden-vector test in `test_codec.cpp:177` pins byte output to `9cca5c30990ceaddb06f9e6019578162a4c9bbb2bbc6b72ea6ba1737d1836e9f` (D-01 byte-identity). Cross-namespace differs test at `test_codec.cpp:143` locks D-13. |
| 4 | PUBK-first invariant enforced at node protocol level on every ingest path (direct + sync-replicated) with six sub-cases | VERIFIED | Step 1.5 gate at `engine.cpp:181-195` runs BEFORE any `crypto::offload`: `!storage_.has_owner_pubkey(target_namespace) && !wire::is_pubkey_blob(blob.data) → pubk_first_violation`. Sync path delegates through `sync_protocol.cpp:102 engine_.ingest(...)`; grep `has_owner_pubkey` in sync_protocol.cpp returns 0 (single-site architecture). Sub-cases (a/b/c/d/e/f) all covered: see test table below. |
| 5 | New `owner_pubkeys` DBI populated on PUBK ingest | VERIFIED | `storage.cpp:194` creates `owner_pubkeys` DBI alongside 7 other sub-DBIs; `max_maps=10` at `storage.cpp:177`. Step 4.5 at `engine.cpp:368-383` calls `storage_.register_owner_pubkey(blob.signer_hint, embedded_sk)` AFTER successful Step 3 verify (T-122-07 register-ordering invariant satisfied). `register_owner_pubkey` implementation at `storage.cpp:1389-1417` is idempotent on matching bytes and throws `std::runtime_error` on D-04 mismatch. |
| 6 | Verify path: receive blob → lookup signer pubkey via `signer_hint` → verify ML-DSA-87 signature; old `derived_ns == blob.namespace_id` check removed | VERIFIED | `grep derived_ns db/engine/` returns zero hits. New Step 2 (`engine.cpp:197-264`) runs owner_pubkeys lookup (plus integrity cross-check `SHA3(resolved_pubkey) == target_namespace`, T-122-07) → delegation_map fallback → `no_delegation`. Step 3 at `engine.cpp:326-347` invokes `build_signing_input(target_namespace, …)` + `Signer::verify(si, blob.signature, resolved_pubkey)`. `test_verify_signer_hint.cpp` has 3 TEST_CASEs covering owner-branch, delegate-branch, and no_delegation paths. |
| 7 | All node ingest/read paths updated; no references to removed fields | VERIFIED | `grep -rn "blob\\.namespace_id\\|blob\\.pubkey" db/engine db/sync db/wire db/storage db/peer` returns empty. `TransportMsgType_Data=8` dispatcher branch deleted (`grep TransportMsgType_Data\\)` in message_dispatcher.cpp returns zero functional hits). `BlobWrite=64` envelope routed; Delete handler reuses BlobWriteBody. Sync wire format carries per-blob `[ns:32B]` prefix (`sync_protocol.h:31` NamespacedBlob struct). BlobFetchResponse widened to `[status:1][target_ns:32][blob_fb:…]`. MetadataRequest returns 32-byte signer_hint (not 2592-byte pubkey). |

**Score:** 7/7 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/schemas/blob.fbs` | Post-122 Blob shape (signer_hint, no namespace_id, no pubkey) | VERIFIED | 5 fields exactly: signer_hint, data, ttl, timestamp, signature. |
| `db/schemas/transport.fbs` | BlobWrite=64 + BlobWriteBody envelope | VERIFIED | `BlobWrite = 64` at line 70; `BlobWriteBody {target_namespace:[ubyte], blob:Blob}` at lines 83-86. |
| `db/wire/blob_generated.h` | Regenerated with `signer_hint()` accessor, no `namespace_id()`/`pubkey()` | VERIFIED | `signer_hint()` at line 42; zero hits for `namespace_id` or `pubkey(` accessor patterns. |
| `db/wire/transport_generated.h` | Regenerated with `BlobWriteBody` + `TransportMsgType_BlobWrite=64` | VERIFIED | Confirmed by dispatcher compile + `message_dispatcher.cpp:1379` uses `TransportMsgType_BlobWrite`. |
| `db/wire/codec.{h,cpp}` | BlobData post-122 shape + build_signing_input(target_namespace,…) + extract_pubk_signing_pk + encode_blob_write_envelope + decode_blob_from_fb | VERIFIED | All declared and implemented; zero references to removed fields. |
| `db/engine/engine.{h,cpp}` | ingest + delete_blob widened to take target_namespace; IngestError adds pubk_first_violation + pubk_mismatch; Step 1.5 gate + Step 4.5 register present | VERIFIED | `engine.h:38-39` IngestError codes; `engine.cpp:105-426` ingest; `engine.cpp:428-582` delete_blob; both carry target_namespace as 1st param. |
| `db/storage/storage.{h,cpp}` | owner_pubkeys DBI + 4 methods (register/get/has/count) + 1 delegate resolver (get_delegate_pubkey_by_hint); max_maps=10; D-10 namespace_id→ns rename | VERIFIED | 5 methods declared at `storage.h:258-282` + implemented at `storage.cpp:1319-1466`. All 5 carry `STORAGE_THREAD_CHECK()`. `max_maps=10` at `storage.cpp:177`. `create_map("owner_pubkeys")` at `storage.cpp:194`. D-04 throw-on-mismatch at `storage.cpp:1406-1408`. |
| `db/peer/error_codes.h` | ERROR_PUBK_FIRST_VIOLATION=0x07 + ERROR_PUBK_MISMATCH=0x08 | VERIFIED | Lines 16-17 define constants; lines 29-30 emit string names. |
| `db/peer/message_dispatcher.cpp` | BlobWrite=64 branch + Delete reuses BlobWriteBody + Data=8 deleted + PUBK error-code mapping | VERIFIED | BlobWrite handler at line 1379; Verifier + GetRoot<BlobWriteBody> at lines 1388-1395; PUBK error mappings at 1460 + 1465. Zero `TransportMsgType_Data\)` hits. |
| `db/sync/sync_protocol.{h,cpp}` | NamespacedBlob + per-blob [ns:32B] prefix + engine_.ingest threaded target_namespace + NO duplicate PUBK-first check | VERIFIED | `NamespacedBlob` at `sync_protocol.h:31`; `engine_.ingest(target_namespace, blob, source)` at `sync_protocol.cpp:102-104`; `grep has_owner_pubkey db/sync/sync_protocol.cpp` returns zero. |
| `db/peer/sync_orchestrator.cpp` | encode_single_blob_transfer call sites updated with req_ns | VERIFIED (inferred from summary) | Plan 05 verification stated 4 call sites updated; no runtime behavior for me to re-verify beyond compile. |
| `db/peer/blob_push_manager.cpp` | BlobFetchResponse wire format carries target_namespace | VERIFIED (inferred from summary) | Plan 05 SUMMARY + 122-05 commit `277dceeb`. |
| `db/tests/test_helpers.h` | make_pubk_blob + updated make_signed_blob/tombstone/delegation for post-122 shape | VERIFIED | `make_pubk_blob` at test_helpers.h; signer_hint set from SHA3(identity.public_key()); `register_pubk` helper + `ns_span` helper present. |
| `db/tests/test_schema_phase122.cpp` | 4 TEST_CASEs under [phase122][schema] | VERIFIED | 4 TEST_CASEs (shape lock, FB accessor surface, size-shrink invariant, PUBK body size pin). |
| `db/tests/wire/test_codec.cpp` | Golden-vector + cross-namespace differs + missing-signer_hint | VERIFIED | Golden hex literal `9cca5c30…6e9f` at line 167; cross-namespace differs TEST_CASE; decode_blob rejects missing signer_hint. |
| `db/tests/storage/test_owner_pubkeys.cpp` | 7 TEST_CASEs for owner_pubkeys API (including D-04 throw) | VERIFIED | 7 TEST_CASEs; REQUIRE_THROWS_AS covers D-04 mismatch. |
| `db/tests/engine/test_pubk_first.cpp` | 6 TEST_CASEs covering D-12 a/c/d/e | VERIFIED | 6 TEST_CASEs covering fresh-namespace reject, PUBK-then-regular, KEM rotation, storage-direct mismatch, engine end-to-end mismatch (pubk_mismatch), long sequence. |
| `db/tests/engine/test_delegate_replay.cpp` | 1 TEST_CASE for D-13 cross-namespace replay | VERIFIED | TEST_CASE "delegate-signed blob for N_A submitted as N_B is rejected" with baseline-accept / fresh-payload-reject pattern. |
| `db/tests/sync/test_pubk_first_sync.cpp` | 2 TEST_CASEs for D-12(b) sync path | VERIFIED | Sync-replicated non-PUBK-first rejection + PUBK-first-then-regular happy path. |
| `db/tests/storage/test_pubk_first_tsan.cpp` | 1 TEST_CASE for D-12(f) cross-namespace PUBK race | VERIFIED | Concurrent 4-coroutine PUBK ingest with shared io_context + thread pool; post-condition `count_owner_pubkeys() == 1`. |
| `db/tests/engine/test_verify_signer_hint.cpp` | 3 TEST_CASEs covering SC#6 verify path (owner / delegate / no_delegation) | VERIFIED | All 3 TEST_CASEs tagged `[phase122][engine][verify]`. |
| `db/CMakeLists.txt` | Wires all 7 new test files | VERIFIED | Lines 230-254 add all 7 test sources to chromatindb_tests. |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|----|--------|---------|
| `engine.cpp::ingest` | `storage.cpp::has_owner_pubkey` | Step 1.5 PUBK-first gate | WIRED | `engine.cpp:187` calls `storage_.has_owner_pubkey(target_namespace)` before `crypto::offload` |
| `engine.cpp::ingest` | `storage.cpp::get_owner_pubkey` | Step 2 owner lookup | WIRED | `engine.cpp:207` calls `storage_.get_owner_pubkey(blob.signer_hint)` |
| `engine.cpp::ingest` | `storage.cpp::get_delegate_pubkey_by_hint` | Step 2 delegation fallback | WIRED | `engine.cpp:252` |
| `engine.cpp::ingest` | `storage.cpp::register_owner_pubkey` | Step 4.5 PUBK registration | WIRED | `engine.cpp:376` — fires AFTER Step 3 verify (`verify_ok` check at :342) and AFTER tombstone handling. Correctly ordered per T-122-07. |
| `engine.cpp::ingest` | `codec.cpp::build_signing_input` | Step 3 verify | WIRED | `engine.cpp:333` — uses `target_namespace` as first sponge input (D-01 / D-13 defense). |
| `sync_protocol.cpp::ingest_blobs` | `engine.cpp::ingest` | Per-blob sync delegation | WIRED | `sync_protocol.cpp:102-104` threads NamespacedBlob.target_namespace through to engine.ingest; no parallel PUBK-first check. |
| `message_dispatcher.cpp::BlobWrite` | `engine.cpp::ingest` | Dispatcher direct-write | WIRED | `message_dispatcher.cpp:1379-1471` decodes BlobWriteBody envelope, extracts target_namespace, calls engine_.ingest. |
| `message_dispatcher.cpp::Delete` | `engine.cpp::delete_blob` | Dispatcher delete | WIRED | `message_dispatcher.cpp:330-400` uses BlobWriteBody envelope + `engine_.delete_blob(target_namespace, blob, conn)`. |
| `blob_push_manager.cpp::handle_blob_fetch_response` | `engine.cpp::ingest` | BlobFetchResponse wire format widened with target_namespace | WIRED | Per 122-05 SUMMARY + commit 277dceeb. |
| Dispatcher | `ERROR_PUBK_FIRST_VIOLATION` / `ERROR_PUBK_MISMATCH` | Wire-level error surfaces | WIRED | message_dispatcher.cpp:1460,1465 + 393-395. |

### Data-Flow Trace (Level 4)

Not applicable — this phase is a pure C++ backend protocol refactor with no dynamic rendering / UI data flow. Data path correctness is verified by the Key Link Verification table plus the 10+ integration TEST_CASEs that exercise ingest→storage→verify→register flow end-to-end.

### Behavioral Spot-Checks

Per verifier-context instruction ("Do NOT run the test suite yourself — the user will run it"), runtime assertions are left to the user.

Grep-based spot-checks (completed):

| Check | Expected | Result | Status |
|-------|----------|--------|--------|
| `grep -rn "blob\\.namespace_id\\|blob\\.pubkey" db/engine db/sync db/wire db/storage db/peer` | empty | empty | PASS |
| `grep "max_maps" db/storage/storage.cpp \| grep "= 10"` | matches | `operate_params.max_maps = 10` | PASS |
| `grep "create_map(\"owner_pubkeys\")" db/storage/storage.cpp` | present | line 194 | PASS |
| `grep "has_owner_pubkey" db/sync/sync_protocol.cpp` | 0 hits (single-site) | 0 hits | PASS |
| `grep "derived_ns" db/engine/` | 0 hits (check removed) | 0 hits | PASS |
| `grep "TransportMsgType_Data\\)" db/peer/message_dispatcher.cpp` | 0 functional hits | 0 hits | PASS |
| Step ordering: verify_ok check (line 342) precedes register_owner_pubkey (line 376) | true | verified | PASS |
| All 7 new Phase 122 test files wired in CMakeLists.txt | present | lines 230-254 | PASS |
| 4 TEST_CASEs in test_schema_phase122.cpp | >= 4 | 4 | PASS |
| 7 TEST_CASEs in test_owner_pubkeys.cpp | >= 7 | 7 | PASS |
| 6 TEST_CASEs in test_pubk_first.cpp | >= 6 (D-12 a/c/d/e) | 6 | PASS |
| 2 TEST_CASEs in test_pubk_first_sync.cpp (D-12b) | >= 2 | 2 | PASS |
| 1 TEST_CASE in test_pubk_first_tsan.cpp (D-12f) | >= 1 | 1 | PASS |
| 3 TEST_CASEs in test_verify_signer_hint.cpp (SC#6) | >= 3 | 3 | PASS |
| 1 TEST_CASE in test_delegate_replay.cpp (D-13) | >= 1 | 1 | PASS |
| Claimed commits exist | 20+ commits | all found | PASS |

### Security Threat Model Coverage

| Threat | Mitigation | Test Coverage | Status |
|--------|-----------|---------------|--------|
| T-122-01 PUBK_MISMATCH (different signing key for already-owned ns) | Step 4.5 catches storage::runtime_error → IngestError::pubk_mismatch | test_owner_pubkeys.cpp Test 3 (storage-direct) + test_pubk_first.cpp Test 5 (engine end-to-end collusion scenario) | COVERED |
| T-122-02 Sync bypass of PUBK-first gate | Single-site gate in engine.cpp:181; sync delegates via engine_.ingest | test_pubk_first_sync.cpp TEST_CASE 1 — sync-replicated non-PUBK rejected | COVERED |
| T-122-03 Cross-namespace replay by delegate with multi-namespace authority | D-01 target_namespace absorbed into signing sponge; Step 3 verify fails on wrong-ns submission | test_codec.cpp "cross-namespace differs" + test_delegate_replay.cpp D-13 test | COVERED |
| T-122-04 TSAN race: two peers concurrent PUBK for same ns | STORAGE_THREAD_CHECK + MDBX write-txn serialization; first-wins semantics | test_pubk_first_tsan.cpp | COVERED |
| T-122-07 Register-ordering invariant (Step 4.5 AFTER Step 3 verify) | Code ordering verified; embedded_sk integrity cross-check at Step 2 fresh-namespace branch also enforces SHA3(embedded_sk) == target_namespace BEFORE verify | Code inspection: engine.cpp:326 (Step 3) → :342 (verify_ok gate) → :368 (Step 4.5). Plus `test_pubk_first.cpp` Test 4/5 verify the ordering by observing that a bad-signature PUBK never reaches Step 4.5 | COVERED |
| T-122-09 Tampering / input validation on BlobWrite envelope | `Verifier::VerifyBuffer<BlobWriteBody>` + `target_namespace()->size() == 32` + `body->blob()` null check; record_strike_ on malformed | Dispatcher code inspection at message_dispatcher.cpp:1388-1400 | COVERED |

### Requirements Coverage

VALIDATION.md test anchors:

| Test Anchor | SC | Status |
|-------------|----|----|
| Blob has no namespace_id field | SC#1 | SATISFIED — schema + test_schema_phase122.cpp |
| Blob has signer_hint [32] | SC#2 | SATISFIED — schema + codec + test_schema_phase122.cpp |
| build_signing_input absorbs target_namespace byte-identical | SC#3 / D-01 | SATISFIED — test_codec.cpp golden-vector |
| PUBK-first rejects non-PUBK on fresh namespace | SC#4(a) | SATISFIED — test_pubk_first.cpp |
| PUBK-first rejects sync-replicated non-PUBK | SC#4(b) | SATISFIED — test_pubk_first_sync.cpp |
| PUBK after PUBK idempotent when signing key matches | SC#4(d) / KEM rotation | SATISFIED — test_pubk_first.cpp Test 3 |
| PUBK with different signing key rejected with PUBK_MISMATCH | SC#4(e) | SATISFIED — test_pubk_first.cpp Tests 4+5 |
| non-PUBK after PUBK succeeds | SC#4(c) | SATISFIED — test_pubk_first.cpp Tests 2+6 |
| Cross-namespace PUBK race first-wins (TSAN) | SC#4(f) | SATISFIED — test_pubk_first_tsan.cpp |
| owner_pubkeys DBI register/get/has/count | SC#5 | SATISFIED — test_owner_pubkeys.cpp (7 cases) |
| Verify path resolves pubkey via signer_hint | SC#6 | SATISFIED — test_verify_signer_hint.cpp (3 cases) |
| Delegate-replay cross-namespace rejected | D-13 | SATISFIED — test_delegate_replay.cpp |
| No stale blob.namespace_id or blob.pubkey references | SC#7 regression | SATISFIED — grep gates empty |
| max_maps bumped to ≥ 10 | D-supplement Pitfall #1 | SATISFIED — storage.cpp:177 |

### Anti-Patterns Found

None. Scans across `db/engine/`, `db/storage/`, `db/wire/`, `db/sync/` for TODO/FIXME/XXX/HACK/placeholder keywords returned empty. No stub returns, no empty handlers, no hardcoded empty data paths in production code. The only "test data" arrays (`rotated.data.insert(...)` in test_pubk_first.cpp) are legitimate test-scenario constructions, not stubs.

### Human Verification Required

Two VALIDATION.md-declared manual checks remain (explicitly non-gating per the plan, but surfaced per verifier protocol):

1. **Two-node live wire sanity — PUBK-first rejection observed over the network.**
   - **Test:** Wipe both nodes' data dirs (local + 192.168.1.73). Start both. From cdb, attempt a non-PUBK write to a fresh namespace. Expect protocol-level `ERROR_PUBK_FIRST_VIOLATION` (not silent accept). Publish PUBK first, retry — expect success.
   - **Why human:** Node-to-node framing cannot be exercised in the in-process Catch2 harness; user is asked to confirm the protocol-level rejection is visible over TCP.

2. **Blob size reduction confirmed ~35% smaller on the wire.**
   - **Test:** Record `encode_blob_transfer` output size for a minimal 1-byte write post-122; confirm ≥2592 bytes smaller than pre-122.
   - **Why human:** End-to-end wire-level size drop requires a running daemon; the codec-layer size-shrink invariant is already automated in `test_schema_phase122.cpp` but the full transport-envelope drop is declared manual-only.

Both checks are classified in VALIDATION.md as "observational sanity, not gate conditions" — reporting them for transparency, not blocking phase completion.

### Gaps Summary

None. All 7 Success Criteria verified. All claimed artifacts exist on disk at the paths documented in SUMMARY.md files. All key links wired correctly (Step 1.5 gate → storage.has_owner_pubkey; Step 2 resolution → get_owner_pubkey / get_delegate_pubkey_by_hint; Step 3 verify → build_signing_input + Signer::verify; Step 4.5 register → register_owner_pubkey with D-04 throw translation). All D-12 (a/b/c/d/e/f) and D-13 sub-cases have concrete TEST_CASE coverage. All regression gates (grep for stale `blob.namespace_id` / `blob.pubkey`; max_maps=10; single-site sync PUBK-first invariant) hold.

The register-ordering invariant (T-122-07 — Step 4.5 fires AFTER Step 3 signature verify) is preserved in code: `engine.cpp:342` returns early on `!verify_ok` before line 368 where `register_owner_pubkey` is called. The fresh-namespace PUBK branch (lines 225-247) additionally verifies `SHA3(embedded_sk) == target_namespace` before proceeding to Step 3, defending against a PUBK claiming a namespace it does not hash to.

The full Catch2 suite's runtime green state (726/726 cases, 3562/3562 assertions per 122-07 SUMMARY; 736/736 cases, 3620/3620 per 122-06 SUMMARY) is reported by the plan authors but not independently re-run by this verifier (per instruction). The user is expected to validate the test suite runs green before closing the phase.

---

_Verified: 2026-04-20T15:01:27Z_
_Verifier: Claude (gsd-verifier)_
