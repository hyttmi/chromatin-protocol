---
phase: 122
slug: schema-signing-cleanup-strip-namespace-and-compress-pubkey
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-20
---

# Phase 122 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 (via FetchContent in `db/CMakeLists.txt`) |
| **Config file** | `db/CMakeLists.txt` (tests registered via `catch_discover_tests`) |
| **Quick run command** | `cmake --build build -j$(nproc) --target chromatindb_tests && ./build/chromatindb_tests "[phase122]"` |
| **Full suite command** | `cmake --build build -j$(nproc) --target chromatindb_tests && ./build/chromatindb_tests` |
| **Estimated runtime** | ~90 seconds full suite; ~15 seconds for `[phase122]` subset |

---

## Sampling Rate

- **After every task commit:** Run `./build/chromatindb_tests "[phase122]"` (the subset tagged for this phase's new tests + any directly edited pre-existing tests).
- **After every plan wave:** Run full suite (`./build/chromatindb_tests`) including TSAN build for the concurrency test.
- **Before `/gsd-verify-work`:** Full suite green on both normal and TSAN configurations. Manual two-node sanity (live node + local) confirming a non-PUBK first write is rejected over the wire.
- **Max feedback latency:** 15 seconds for task-level sampling; 120 seconds (including TSAN build) for wave-level.

---

## Per-Task Verification Map

> Filled in by planner. Each task in each PLAN.md must carry either an `<automated>` verify block or a Wave 0 dependency. The table below lists the test anchors that MUST exist by end-of-phase; planner assigns task IDs.

| Test Anchor | Requirement (Success Criterion) | Secure Behavior | Test Type | Automated Command | File | Status |
|-------------|----------------------------------|-----------------|-----------|-------------------|------|--------|
| `phase122/schema: Blob has no namespace_id field` | SC#1 | Schema cannot carry redundant namespace_id | unit (FlatBuffers introspection) | `./build/chromatindb_tests "[phase122][schema]"` | `db/tests/test_schema_phase122.cpp` (NEW) | ⬜ pending |
| `phase122/schema: Blob has signer_hint [32]` | SC#2 | 32-byte hint replaces 2592-byte pubkey | unit | `./build/chromatindb_tests "[phase122][schema]"` | `db/tests/test_schema_phase122.cpp` (NEW) | ⬜ pending |
| `phase122/codec: build_signing_input absorbs target_namespace byte-identical` | SC#3 | Signing sponge commits to target namespace; byte-identical to pre-122 digest given same namespace bytes | unit | `./build/chromatindb_tests "[phase122][codec]"` | `db/tests/test_codec_signing_input.cpp` (EXTEND) | ⬜ pending |
| `phase122/engine: PUBK-first rejects non-PUBK on fresh namespace` | SC#4 (a) | Ingest without registered owner_pubkey is rejected before sig-verify | integration | `./build/chromatindb_tests "[phase122][pubk_first][engine]"` | `db/tests/engine/test_pubk_first.cpp` (NEW) | ⬜ pending |
| `phase122/sync: PUBK-first rejects sync-replicated non-PUBK on fresh namespace` | SC#4 (sync path) | Sync ingest cannot bypass PUBK-first | integration | `./build/chromatindb_tests "[phase122][pubk_first][sync]"` | `db/tests/sync/test_pubk_first_sync.cpp` (NEW) | ⬜ pending |
| `phase122/engine: PUBK after PUBK idempotent when signing key matches` | SC#4 (d) | KEM rotation accepted; signing-identity stable | unit | `./build/chromatindb_tests "[phase122][pubk_first]"` | `db/tests/engine/test_pubk_first.cpp` (NEW) | ⬜ pending |
| `phase122/engine: PUBK with different signing key rejected with PUBK_MISMATCH` | SC#4 (e) | First PUBK wins; namespace identity immutable | unit | `./build/chromatindb_tests "[phase122][pubk_first]"` | `db/tests/engine/test_pubk_first.cpp` (NEW) | ⬜ pending |
| `phase122/engine: non-PUBK after PUBK succeeds` | SC#4 (c) | Normal writes permitted once namespace is established | unit | `./build/chromatindb_tests "[phase122][pubk_first]"` | `db/tests/engine/test_pubk_first.cpp` (NEW) | ⬜ pending |
| `phase122/tsan: cross-namespace PUBK race first-wins` | SC#4 (f) | Concurrent PUBK registrations for same namespace: exactly one wins | tsan-integration | `cmake --build build-tsan && ./build-tsan/chromatindb_tests "[phase122][tsan][pubk_first]"` | `db/tests/storage/test_pubk_first_tsan.cpp` (NEW, reuses `test_storage_concurrency_tsan.cpp` fixture) | ⬜ pending |
| `phase122/storage: owner_pubkeys DBI register/get/has/count` | SC#5 | Four new Storage methods carry STORAGE_THREAD_CHECK + behave idempotently | unit | `./build/chromatindb_tests "[phase122][storage][owner_pubkeys]"` | `db/tests/storage/test_owner_pubkeys.cpp` (NEW) | ⬜ pending |
| `phase122/engine: verify path resolves pubkey via signer_hint` | SC#6 | owner_pubkeys lookup → ML-DSA-87 verify succeeds; old `derived_ns == blob.namespace_id` check gone | integration | `./build/chromatindb_tests "[phase122][engine][verify]"` | `db/tests/engine/test_verify_signer_hint.cpp` (NEW or EXTEND test_engine.cpp) | ⬜ pending |
| `phase122/engine: delegate-replay cross-namespace rejected` | SC#6 / D-13 | Delegate signature for N_A submitted as N_B fails signature verify | integration | `./build/chromatindb_tests "[phase122][engine][delegate]"` | `db/tests/engine/test_delegate_replay.cpp` (NEW) | ⬜ pending |
| `phase122/regression: no stale references to blob.namespace_id or blob.pubkey` | SC#7 | grep for stale field accesses returns zero results across production code | grep sanity | `! grep -rn "blob\\.namespace_id\\|blob\\.pubkey" db/engine db/sync db/wire db/storage 2>/dev/null` | N/A (grep-only) | ⬜ pending |
| `phase122/regression: max_maps bumped to ≥ 10` | D-supplement (Pitfall #1) | MDBX env has headroom after adding owner_pubkeys DBI | unit / static check | `grep -n "max_maps" db/storage/storage.cpp` + `./build/chromatindb_tests "[phase122][storage]"` | `db/storage/storage.cpp:176` | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/test_helpers.h` — add `make_pubk_blob(target_namespace, signing_pk, kem_pk, ...)` helper **AND** update existing `make_signed_blob / make_tombstone_blob / make_delegation_blob / make_delegate_blob` to emit the new Blob shape (no namespace_id, no inline pubkey, with signer_hint). This is the schema-change cascade anchor: once test_helpers compiles, the rest of the cascade (77 engine + 33 sync + 105 storage TEST_CASEs) is guided by compiler errors.
- [ ] `db/tests/test_helpers.h` — add `make_blob_write_envelope(target_namespace, blob)` (or equivalent) so tests can construct the new transport envelope without duplicating wire-level plumbing.
- [ ] New test file skeletons (empty, with tags) so the planner's acceptance-criteria greps succeed early:
  - `db/tests/test_schema_phase122.cpp` — tags `[phase122][schema]`
  - `db/tests/engine/test_pubk_first.cpp` — tags `[phase122][pubk_first][engine]`
  - `db/tests/sync/test_pubk_first_sync.cpp` — tags `[phase122][pubk_first][sync]`
  - `db/tests/storage/test_pubk_first_tsan.cpp` — tags `[phase122][tsan][pubk_first]` (TSAN target only)
  - `db/tests/storage/test_owner_pubkeys.cpp` — tags `[phase122][storage][owner_pubkeys]`
  - `db/tests/engine/test_verify_signer_hint.cpp` — tags `[phase122][engine][verify]`
  - `db/tests/engine/test_delegate_replay.cpp` — tags `[phase122][engine][delegate]`
- [ ] `db/CMakeLists.txt` — ensure new test sources are globbed/added (verify existing `file(GLOB_RECURSE ...)` covers `db/tests/**/*.cpp` OR add explicit entries).
- [ ] TSAN build target confirmed green (no pre-existing TSAN failures block Phase 122 TSAN test). Re-use existing `sanitizers/` config.

*Existing infrastructure covers the Catch2 + TSAN framework; Wave 0 is schema-cascade scaffolding and test-file bootstrapping.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Two-node live wire sanity: PUBK-first rejection observed over the network | SC#4 | Automated tests exercise in-process; over-the-wire behavior with a real live peer is worth confirming once per protocol break. The user operates 2 dev nodes (1 local + live `192.168.1.73`). | 1. Wipe both nodes' data dirs. 2. Start both nodes. 3. From cdb, attempt a non-PUBK write to a fresh namespace. 4. Expect protocol-level error (not a silent accept). 5. Publish PUBK, retry the write — expect success. |
| Blob size reduction confirmed ~35% smaller on the wire | SC (goal) | Size reduction is the phase's raison d'être; a byte-count sanity check on a real transfer is easy and catches any accidental bloat. | 1. Record `encode_blob_transfer` output size for a minimal write (1-byte data) on a pre-122 node. 2. Repeat post-122. 3. Confirm size drops by ≥ 2592 bytes (the removed inline pubkey). |

*All correctness-critical phase behaviors have automated Catch2 verification; manual checks are observational sanity, not gate conditions.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references (schema cascade + new test files)
- [ ] No watch-mode flags (Catch2 is one-shot; TSAN build cached)
- [ ] Feedback latency < 15s task-level; < 120s wave-level
- [ ] `nyquist_compliant: true` set in frontmatter after planner fills per-task map

**Approval:** pending
