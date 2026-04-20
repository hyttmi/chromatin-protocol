---
phase: 123
slug: tombstone-batching-and-name-tagged-overwrite
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-20
---

# Phase 123 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 (via FetchContent in `db/CMakeLists.txt`) — same as Phase 122 |
| **Config file** | `db/CMakeLists.txt` (tests registered via `catch_discover_tests`) |
| **Quick run command** | `cmake --build build -j$(nproc) --target chromatindb_tests && ./build/db/chromatindb_tests "[phase123]"` |
| **Full suite command** | `cmake --build build -j$(nproc) --target chromatindb_tests && ./build/db/chromatindb_tests` |
| **Estimated runtime** | ~120 seconds full suite; ~20 seconds for `[phase123]` subset |
| **CLI test runtime** | Deferred — CLI code lands on pre-122 wire; full E2E validation runs after Phase 124 adapts CLI |

---

## Sampling Rate

- **After every task commit:** Run `./build/db/chromatindb_tests "[phase123]"` (the subset tagged for this phase).
- **After every plan wave:** Run full Catch2 suite via user (per `feedback_delegate_tests_to_user.md` — orchestrator does not run the suite; the user runs it locally and reports results).
- **Before `/gsd-verify-work`:** Full node-side suite green. CLI compile-check green. CLI E2E deferred to Phase 124.
- **Max feedback latency:** 20 seconds for task-level `[phase123]` subset; 120 seconds full suite (user-run).

---

## Per-Task Verification Map

> Planner fills task IDs. Each task must have either an `<automated>` verify block OR a Wave 0 scaffold dependency.

| Test Anchor | Requirement (SC) | Secure Behavior | Test Type | Automated Command | File | Status |
|-------------|------------------|-----------------|-----------|-------------------|------|--------|
| `phase123/codec: NAME_MAGIC 0x4E414D45 defined` | SC#1 | Magic constant exists in both headers | unit | `grep -q "NAME_MAGIC.*0x4E.*0x41.*0x4D.*0x45" db/wire/codec.h cli/src/wire.h` | `db/wire/codec.h`, `cli/src/wire.h` | ⬜ pending |
| `phase123/codec: BOMB_MAGIC defined + is_bomb/is_name helpers` | SC#2, SC#1 | Both magics + helpers present | unit | `grep -q "is_bomb" db/wire/codec.h && grep -q "is_name" db/wire/codec.h` | `db/wire/codec.h` | ⬜ pending |
| `phase123/codec: NAME/BOMB payload encode/decode round-trip` | SC#1, SC#2 | Payloads serialize and parse byte-identical | unit | `./build/db/chromatindb_tests "[phase123][codec][name]"` & `[phase123][codec][bomb]` | `db/tests/wire/test_codec.cpp` (EXTEND) | ⬜ pending |
| `phase123/engine: BOMB ttl != 0 rejected` | SC#2 (ttl=0 invariant) | BOMB ttl!=0 is refused at ingest | integration | `./build/db/chromatindb_tests "[phase123][engine][bomb_ttl]"` | `db/tests/engine/test_bomb_validation.cpp` (NEW) | ⬜ pending |
| `phase123/engine: BOMB header sanity rejected` | D-13 | Malformed BOMB (wrong length) refused | unit | `./build/db/chromatindb_tests "[phase123][engine][bomb_sanity]"` | `db/tests/engine/test_bomb_validation.cpp` (NEW) | ⬜ pending |
| `phase123/engine: delegate BOMB rejected` | D-12 | Delegate-signed BOMB refused (mirrors tombstone-delegate rule) | integration | `./build/db/chromatindb_tests "[phase123][engine][bomb_delegate]"` | `db/tests/engine/test_bomb_validation.cpp` (NEW) | ⬜ pending |
| `phase123/engine: BOMB accepted with ttl=0 + valid header + owner source` | SC#2 | Happy-path BOMB ingest succeeds | integration | `./build/db/chromatindb_tests "[phase123][engine][bomb_accept]"` | `db/tests/engine/test_bomb_validation.cpp` (NEW) | ⬜ pending |
| `phase123/engine: BOMB side-effect tombstones N targets` | SC#6 | One BOMB deletes all listed content blobs | integration | `./build/db/chromatindb_tests "[phase123][engine][bomb_side_effect]"` | `db/tests/engine/test_bomb_side_effect.cpp` (NEW) | ⬜ pending |
| `phase123/engine: delegate NAME accepted` | D-11 | Delegate-signed NAME accepted (audit via signer_hint) | integration | `./build/db/chromatindb_tests "[phase123][engine][name_delegate]"` | `db/tests/engine/test_name_delegate.cpp` (NEW) | ⬜ pending |
| `phase123/transport: ListRequest type_filter returns NAME blobs` | D-10 (via reuse) | Existing ListRequest endpoint filters correctly by 4-byte magic | integration | `./build/db/chromatindb_tests "[phase123][transport][list]"` | `db/tests/peer/test_list_by_magic.cpp` (NEW or EXTEND existing list test) | ⬜ pending |
| `phase123/overwrite: NAME resolution picks max-timestamp winner` | SC#5 | Two NAME blobs with different timestamps → higher wins | integration | `./build/db/chromatindb_tests "[phase123][overwrite]"` | `db/tests/engine/test_name_overwrite.cpp` (NEW) | ⬜ pending |
| `phase123/overwrite: NAME tiebreak on equal timestamps = content_hash DESC` | D-02 | Two NAME blobs with equal timestamps → lex-larger content_hash wins | unit | `./build/db/chromatindb_tests "[phase123][overwrite][tiebreak]"` | `db/tests/engine/test_name_overwrite.cpp` (NEW) | ⬜ pending |
| `phase123/cli: cdb put --name flag parses` | SC#3 | `--name foo` accepted by argv parser | unit | `./build/cli/cdb put --name foo --help` shows new flag | `cli/src/main.cpp` + `commands.cpp` | ⬜ pending |
| `phase123/cli: cdb get subcommand exists` | SC#4 | `cdb get <name>` routes correctly | unit | `./build/cli/cdb get --help` | `cli/src/main.cpp` + `commands.cpp` | ⬜ pending |
| `phase123/cli: cdb rm multi-target argv parse` | SC#6 | `cdb rm A B C` accepted (was single-target) | unit | `./build/cli/cdb rm --help` shows multi-target | `cli/src/main.cpp` | ⬜ pending |
| `phase123/regression: no wire-format regressions from 122` | SC#7 | Post-122 invariants still hold | regression | `./build/db/chromatindb_tests "[phase122]"` still green | — | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/test_helpers.h` — add `make_name_blob(ns, signer_id, name, target_hash, ttl)` and `make_bomb_blob(ns, signer_id, target_hashes[], ttl)` helpers following the post-122 shape. Both emit the new Blob layout with the correct NAME/BOMB magic in `data`.
- [ ] New test file skeletons with tags:
  - `db/tests/engine/test_bomb_validation.cpp` — `[phase123][engine][bomb_ttl]`, `[...bomb_sanity]`, `[...bomb_delegate]`, `[...bomb_accept]`
  - `db/tests/engine/test_bomb_side_effect.cpp` — `[phase123][engine][bomb_side_effect]`
  - `db/tests/engine/test_name_delegate.cpp` — `[phase123][engine][name_delegate]`
  - `db/tests/engine/test_name_overwrite.cpp` — `[phase123][overwrite]`, `[...tiebreak]`
  - `db/tests/peer/test_list_by_magic.cpp` (if existing list test isn't reusable) — `[phase123][transport][list]`
- [ ] `db/CMakeLists.txt` — ensure new test files wire into `chromatindb_tests` (existing GLOB likely covers them; add explicit entries if needed).
- [ ] `db/wire/codec.h` and `cli/src/wire.h` — NAME_MAGIC + BOMB_MAGIC + is_name + is_bomb added (Wave 0 structural scaffold so later plans can grep for them).

*Existing infrastructure (Catch2, TSAN, cdb dev build) covers the framework; Wave 0 is helper + test-file scaffolding.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Live-node BOMB ttl=0 enforcement observed over the wire | SC#2 | Over-the-wire confirmation on the 192.168.1.73 node. **Deferred to post-124** since the CLI currently can't speak post-122 wire. | 1. After Phase 124 lands and binaries deploy, attempt `cdb rm A B C` from the CLI. 2. Observe a BOMB blob in the node logs with ttl=0. 3. Manually craft a BOMB with ttl=3600 (hex tool or scripted request) and confirm node rejects with protocol error. |
| `cdb put --name foo --replace` end-to-end overwrite + tombstone | SC#5 | CLI currently on pre-122 wire; E2E test waits on Phase 124. | After Phase 124: `cdb put --name foo file_v1; cdb get foo` → v1 content. Then `cdb put --name foo --replace file_v2; cdb get foo` → v2 content. Old content no longer retrievable (BOMB/tombstone applied). |
| Storage reduction ~200× vs N single tombstones | SC goal | Observational — confirm the amortization math in practice on non-trivial N. | After Phase 124: `cdb rm <50 hashes>`. Measure blob size of resulting BOMB vs 50 single tombstones. Expect ~1:50 ratio of BOMB-blob-size to cumulative-tombstone-blob-size. |

*All correctness-critical phase behaviors have automated Catch2 verification at the engine level. CLI E2E checks are explicitly deferred to post-Phase-124 deployment (consistent with CONTEXT.md `<domain>`).*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers helpers + new test file scaffolding
- [ ] No watch-mode flags
- [ ] Orchestrator does not spawn subagents just to run tests (per `feedback_delegate_tests_to_user.md`)
- [ ] `nyquist_compliant: true` set in frontmatter after planner fills per-task map

**Approval:** pending
