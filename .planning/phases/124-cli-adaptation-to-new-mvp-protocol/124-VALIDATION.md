---
phase: 124
slug: cli-adaptation-to-new-mvp-protocol
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-21
---

# Phase 124 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 (header: `<catch2/catch_test_macros.hpp>`) |
| **Config file** | `cli/tests/CMakeLists.txt` (CTest target: `chromatindb_cli_tests`) |
| **Quick run command** | `cmake --build build -j$(nproc) --target chromatindb_cli_tests && ./build/cli/chromatindb_cli_tests "[wire]"` |
| **Full suite command** | `cmake --build build -j$(nproc) --target chromatindb_cli_tests && ./build/cli/chromatindb_cli_tests` |
| **Estimated runtime** | ~2s tag-filtered, ~6s full CLI suite |

---

## Sampling Rate

- **After every task commit:** Run the tag-filtered command for the touched module (`"[wire]"`, `"[pubk]"`, `"[chunked]"`, `"[bomb]"`).
- **After every plan wave:** Run the full CLI suite `./build/cli/chromatindb_cli_tests`.
- **Before `/gsd-verify-work`:** Full suite green + live E2E matrix (D-08 items 1–7) green on both local and home nodes.
- **Max feedback latency:** ~6 seconds for full CLI suite; live E2E runs on demand.

> Per `feedback_delegate_tests_to_user.md`, Claude delegates orchestrator-level full builds/tests to the user. Per-task tag-filtered runs remain in the agent.

---

## Per-Task Verification Map

> Task IDs are filled in after the planner produces PLAN.md files. Each row maps a Success-Criterion-level requirement to a measurable check.

| SC Ref | Wave | Requirement / Secure Behavior | Threat Ref | Test Type | Automated Command | File Exists | Status |
|--------|------|-------------------------------|------------|-----------|-------------------|-------------|--------|
| SC#1 | 1–2 | Auto-PUBK fires once per invocation per namespace on first owner-write | T-124-01 (auto-PUBK race) | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ W0 (new `test_auto_pubk.cpp` or tag in `test_wire.cpp`) | ⬜ pending |
| SC#1 | 1–2 | Probe sends `ListRequest` with `flags=0x02`, `type_filter=PUBK_MAGIC_CLI`, `limit=1` | — | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ W0 | ⬜ pending |
| SC#1 | 1–2 | Probe response parse: `count==0` → emit PUBK; `count>0` → skip | — | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ W0 | ⬜ pending |
| SC#1 | 1–2 | Second write to same namespace in same invocation skips the probe (cache hit) | — | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ W0 | ⬜ pending |
| SC#1 | 1–2 | Delegate write (`target_ns != SHA3(own_sk)`) never triggers auto-PUBK (D-01a) | T-124-02 (delegate-PUBK spoof) | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ W0 | ⬜ pending |
| SC#1 | 3 | Live E2E: fresh namespace on home node, first `cdb put` succeeds without manual `cdb publish` | — | e2e | `cdb --node home put /tmp/file.bin` after wipe+redeploy | manual (Claude runs) | ⬜ pending |
| SC#2 | 1 | `encode_blob`/`decode_blob` roundtrip with 5-field post-122 schema preserves `signer_hint`, `data`, `ttl`, `timestamp`, `signature` | T-124-03 (old-format blob accepted) | unit | `./build/cli/chromatindb_cli_tests "wire: encode_blob*"` | ✅ modify existing TEST_CASE at `cli/tests/test_wire.cpp:147-170` | ⬜ pending |
| SC#2 | 1 | Post-migration grep regression: `grep -rn "blob\.namespace_id\|blob\.pubkey" cli/src/` returns 0 hits | — | static | `grep` in phase verification | N/A | ⬜ pending |
| SC#2 | 1 | `MsgType::Data = 8` removed from `cli/src/wire.h` enum (D-04a) | — | static | `grep -n "Data\s*=\s*8" cli/src/wire.h` returns 0 hits | N/A | ⬜ pending |
| SC#3 | 1 | `build_signing_input` byte-output matches pre-rename for identical inputs (parameter rename only) | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ✅ extend existing case at `cli/tests/test_wire.cpp:189` | ⬜ pending |
| SC#3 | 1 | Golden-vector cross-check: CLI `build_signing_input` output matches node `db/wire/codec.cpp::build_signing_input` for one hardcoded `(target_ns, data, ttl, ts)` tuple | — | unit (golden) | `./build/cli/chromatindb_cli_tests "[wire][golden]"` | ❌ W0 (new TEST_CASE) | ⬜ pending |
| SC#4 | 1 | `build_owned_blob(id, own_ns, ...)` ⇒ `signer_hint == id.namespace_id()` | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ W0 | ⬜ pending |
| SC#4 | 1 | `build_owned_blob(id, other_ns, ...)` ⇒ `signer_hint == SHA3(id.signing_pubkey())` and `signer_hint != other_ns` | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ W0 | ⬜ pending |
| SC#4 | 1 | Signature emitted by `build_owned_blob` verifies with `id.signing_pubkey()` against `build_signing_input(target_ns, data, ttl, ts)` | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ W0 | ⬜ pending |
| SC#5 | 1 | `BlobWriteBody { target_namespace:[32], blob:Blob }` encode/decode roundtrip matches `db/schemas/transport.fbs:83-87` | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ W0 | ⬜ pending |
| SC#5 | 1 | `make_bomb_data` roundtrip (count field + 32-byte target concatenation) | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ W0 | ⬜ pending |
| SC#5 | 1 | `parse_name_payload` roundtrip (name length, target hash, magic mismatch rejection) | — | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ W0 | ⬜ pending |
| SC#5 | 2 | `cdb put --name foo <file>` ⇒ content + NAME blobs; `cdb get foo` resolves to content | — | integration (E2E) | Live run, both nodes (D-08 #1) | existing path (Phase 123) | ⬜ pending |
| SC#5 | 2 | Batched `cdb rm <h1> <h2> <h3>` emits single BOMB covering all three | — | integration | `cdb ls --type BOMB` shows one entry, `count==3` | existing | ⬜ pending |
| SC#5 | 2 | `cdb rm <chunked_manifest_hash>` (D-06) cascades chunk tombstones into the same BOMB; manifest + all referenced CDAT chunks are tombstoned | T-124-04 (orphan chunks after rm) | unit + integration | Unit: new `classify_rm_target` test. E2E: put chunked ≥500 MiB, rm manifest, verify chunks in `ls --raw --type TOMB` | ❌ W0 (unit), manual (E2E) | ⬜ pending |
| SC#6 | 1 | Pre-existing CLI tests (identity, envelope, contacts, chunked, pipelining) all pass under the new wire format | — | unit | `./build/cli/chromatindb_cli_tests` (full) | ✅ existing | ⬜ pending |
| SC#6 | 1 | All ~39 (corrected: 12 — see RESEARCH Q1) call sites migrated; zero residual `blob.namespace_id =` or `blob.pubkey =` assignments in `cli/src/` | — | static | `grep` assertion | N/A | ⬜ pending |
| SC#7 | 3 | D-08 E2E matrix items 1–7 all pass on both local and home (`192.168.1.73`) nodes post-redeploy | — | e2e | 7 documented runs captured in `124-E2E.md` | manual (Claude runs) | ⬜ pending |
| SC#7 | 3 | `ERROR_PUBK_FIRST_VIOLATION = 0x07` surfaces as D-05 user string, NOT raw bytes/phase codes | T-124-05 (info leak in error strings) | integration | Trigger via delegate write to uninitialized namespace on local node | manual | ⬜ pending |
| SC#7 | 3 | `ERROR_PUBK_MISMATCH = 0x08` surfaces as D-05 user string | T-124-05 | integration | Trigger via second identity writing PUBK to same namespace | manual | ⬜ pending |
| SC#7 | 3 | Phase 123 BOMB codes (`0x09`/`0x0A`/`0x0B` per RESEARCH Q7) also decoded by the D-05 error map (no generic "Error: node rejected request" regression) | — | integration | `grep "Error: node rejected request" cli/src/commands.cpp` = 0; trigger paths land user-facing string | manual (trigger) + static (grep) | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `cli/tests/test_wire.cpp` — extend with: `BlobWriteBody` envelope roundtrip, `build_owned_blob` helper (own-ns + delegate-ns + signature verify), `make_bomb_data` roundtrip, `parse_name_payload` roundtrip, golden-vector cross-check against node `build_signing_input`.
- [ ] `cli/tests/test_auto_pubk.cpp` **OR** `[pubk]`-tagged TEST_CASEs in `test_wire.cpp` — covers SC#1 probe + cache logic (new file if TEST_CASE count crosses ~5; planner picks).
- [ ] `cli/src/pubk_presence.{h,cpp}` — new module housing invocation-scoped auto-PUBK cache + `ensure_pubk(id, conn, target_ns)` helper (per RESEARCH Risks/Unknowns #1).
- [ ] `cli/src/wire.{h,cpp}` additions: `NamespacedBlob` struct, `build_owned_blob()` helper, `encode_blob_write_body()`, `MsgType::BlobWrite = 64` enum entry, `MsgType::Data = 8` deleted, `encode_blob`/`decode_blob` rewritten to post-122 5-field Blob.
- [ ] `cli/src/commands.cpp` — 9 blob-construction sites migrated to `build_owned_blob` + `BlobWrite=64` envelope; auto-PUBK probe wired at first owner-write per invocation; D-06 BOMB cascade added to `cmd::rm_batch`; D-05 error map decoding ErrorResponse payload bytes 0x07/0x08/0x09/0x0A/0x0B.
- [ ] `cli/src/chunked.cpp` — 3 blob-construction sites migrated to `build_owned_blob`.
- [ ] `124-E2E.md` — artifact capturing D-08 live-node matrix results (phase gate).
- [ ] Framework install: none needed (Catch2 v3 already vendored).

---

## Manual-Only Verifications

| Behavior | SC | Why Manual | Test Instructions |
|----------|----|------------|-------------------|
| Auto-PUBK end-to-end on fresh namespace | SC#1 | Requires redeployed post-122+123 node with wiped data dir | After user redeploy: `cdb --node home put /tmp/hello.txt`; then `cdb --node home ls --type PUBK` shows one PUBK; `cdb get <hash>` returns content |
| Cross-node sync propagation | SC#7 #2 | Requires two running nodes syncing over TCP | `cdb --node local put /tmp/f.txt`; `cdb --node home ls` includes blob; reverse direction works |
| BOMB propagation | SC#7 #3 | Two-node sync required | Multi-target `cdb --node local rm`; verify BOMB appears in `cdb --node home ls --type BOMB` |
| Chunked (≥500 MiB) | SC#7 #4 | Large file, network transfer | `cdb --node local put /tmp/big_750M.bin`; `cdb --node home get <hash>` after sync |
| Delegate write via `--share @contact` | SC#7 #5 | Contact DB + delegation_map must be live | From local with delegate id: `cdb put --share @owner /tmp/d.txt`; verify delivery on home |
| `--replace` BOMB-of-1 | SC#7 #6 | Full NAME + BOMB round-trip | `cdb put --name foo f1`; `cdb put --name foo --replace f2`; `cdb get foo` returns f2; `cdb ls --type BOMB` shows 1-target BOMB over f1 |
| D-06 cascade live | SC#7 #7 | Chunked blob + manifest fetch round-trip | `cdb put` ≥500 MiB; `cdb rm <manifest_hash>` in a batch with other targets; `cdb ls --raw --type TOMB` shows chunk tombstones |
| Error-string user-facing wording | SC#7 error cases | Requires specific node-state trigger | Delegate-first-write on fresh ns ⇒ "namespace not yet initialized …"; second-writer PUBK ⇒ "owned by a different key …" |

---

## Security Domain

Per RESEARCH `## Security Domain`: ASVS V2/V3/V4/V5/V6 all apply. Planner's `<threat_model>` block MUST address:

- **T-124-01** (auto-PUBK race — peer PUBK during our probe→emit window): synchronous probe + emit; `PUBK_MISMATCH` surfaces cleanly via D-05.
- **T-124-02** (delegate-PUBK spoof — D-01a boundary): `build_owned_blob` computes `signer_hint = SHA3(id.signing_pubkey())` regardless of `target_namespace`; D-01's helper only probes when `target_ns == SHA3(own_sk)`.
- **T-124-03** (old-format blob accepted): `MsgType::Data = 8` deleted from CLI enum (D-04a); node's pre-122 branch also deleted.
- **T-124-04** (orphan chunks after rm of chunked manifest): D-06 cascade unified into single BOMB per invocation.
- **T-124-05** (info leak in error strings — `feedback_no_phase_leaks_in_user_strings.md`): D-05 user-facing wording deliberately avoids `PUBK_FIRST_VIOLATION`/`PUBK_MISMATCH`/phase numbers; CLI tests assert exact user-facing strings.

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 10s for tag-filtered runs, < 60s for full suite
- [ ] `nyquist_compliant: true` set in frontmatter after planner populates task IDs

**Approval:** pending
