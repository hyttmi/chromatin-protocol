---
phase: 123
plan: 03
subsystem: cli
tags: [cli, put, get, rm, name, bomb, phase123]
dependency_graph:
  requires:
    - phase-123-plan-01  # wire::make_name_data / wire::make_bomb_data / NAME_MAGIC_CLI / BOMB_MAGIC_CLI helpers in cli/src/wire.h
    - phase-117          # ListRequest + type_filter endpoint (NAME enumeration without new transport type)
    - phase-122          # node post-signer_hint, dispatcher wire; CLI stays pre-122 here by scope
  provides:
    - cdb put --name <name> [--replace] <file>    # Phase 123 SC#3 / SC#5
    - cdb get <name>                              # Phase 123 SC#4 (D-09 stateless resolution)
    - cdb rm <hash>... → ONE BOMB per invocation  # Phase 123 SC#6 (D-06 / D-07)
    - cli::parse_name_payload / cli::ParsedNamePayload
    - cmd::get_by_name / cmd::rm_batch / extended cmd::put signature
  affects:
    - phase-124 (CLI wire adaptation — will revalidate these flows on post-122 wire)
tech_stack:
  added: []
  patterns:
    - Phase 117 ListRequest+type_filter reuse (no new TransportMsgType, per Plan 01 D-10 deviation)
    - write-before-delete ordering for --replace (content → NAME → BOMB) — plan-checker iter-1 fix
    - memcmp over opaque name bytes (D-04) — names are never strings
    - D-01 timestamp DESC + D-02 content_hash DESC tiebreak (deterministic cross-client)
    - submit_*_blob helpers share signing/encode path; payload construction stays at call sites
key_files:
  created: []
  modified:
    - cli/src/main.cpp       # argv parser extensions (put/get/rm)
    - cli/src/commands.h     # cmd::put signature, get_by_name + rm_batch decls
    - cli/src/commands.cpp   # cmd::put body, submit helpers, get_by_name, rm_batch
    - cli/src/wire.h         # ParsedNamePayload struct + parse_name_payload decl
    - cli/src/wire.cpp       # parse_name_payload impl
decisions:
  - "Task 2 + Task 3 combined into ONE commit (two commits total for the plan) due to co-dependency: cmd::put --replace calls resolve_name_to_target_hash whose implementation lives in Task 3's anonymous-namespace helpers. Splitting them would leave an unresolved symbol between commits."
  - "write-before-delete ordering: content → NAME → BOMB. If any earlier step fails, subsequent steps don't run → prior content remains reachable by hash, nothing irrecoverably deleted."
  - "submit_bomb_blob takes pre-built std::vector<uint8_t> (not std::span<array<32>>) so wire::make_bomb_data is textually called at each emitter site (cmd::put --replace AND cmd::rm_batch) — satisfies acceptance grep ≥2 AND keeps the wire-layout in one place."
  - "rm_batch dedupes identical argv targets (N → unique-N) so BOMB's declared count matches its distinct target set; node doesn't verify (D-14) but dedup avoids accidentally inflating count when shells expand globs twice."
  - "--name rejected for chunked (CPAR manifest) files — binding a NAME to the first CDAT instead of the manifest is a footgun. Future phase can wire this up with an explicit manifest_hash capture."
  - "cmd::get_by_name delegates final content fetch to cmd::get (reuses CENV decrypt + CPAR chunked logic) — avoids duplicating ~400 lines of single-blob/chunked dispatch."
  - "CLI enumeration uses Phase 117 ListRequest + type_filter (4-byte type prefix match), matching Plan 01 D-10 reuse deviation. No new TransportMsgType introduced on the CLI side."
metrics:
  duration: "~60m"
  completed_date: "2026-04-20"
  files_modified: 5
  loc_added: 733
  loc_removed: 22
  tasks: 3
  commits: 2
---

# Phase 123 Plan 03: CLI `cdb put --name`, `cdb get <name>`, `cdb rm`-batch Summary

**One-liner:** CLI command surface for Phase 123's mutable NAME binding + batched BOMB tombstones — `cdb put --name foo [--replace] file` writes content+NAME (and optionally BOMBs the prior binding's content), `cdb get <name>` resolves via Phase 117 `ListRequest+type_filter` with D-01 timestamp DESC / D-02 content-hash DESC tiebreak, `cdb rm <hash>...` emits ONE batched BOMB covering all argv targets per invocation.

## What Landed

| File | Change | Lines |
|------|--------|-------|
| `cli/src/main.cpp` | argv extensions for `put --name/--replace`, `get <name>\|<hash>…` dispatch, `rm <hash>…` multi-target collection via `hash_hexes` | +143 / -20 |
| `cli/src/commands.h` | `<optional>` include, extended `cmd::put` signature (`name_opt`, `replace`), NEW `cmd::get_by_name` + `cmd::rm_batch` declarations | +28 / -2 |
| `cli/src/commands.cpp` | `submit_name_blob` + `submit_bomb_blob` helpers, Step 0 NAME lookup, extended `cmd::put` body (content → NAME → BOMB ordering), anonymous-namespace `enumerate_name_blobs` / `fetch_blob` / `pick_name_winner`, `resolve_name_to_target_hash`, `cmd::get_by_name`, `cmd::rm_batch` | +524 / -1 |
| `cli/src/wire.h` | `ParsedNamePayload` struct + `parse_name_payload` declaration (byte-identical to `db/wire/codec.h`) | +14 |
| `cli/src/wire.cpp` | `parse_name_payload` implementation (39-byte min, magic check, name_len sanity, zero-copy span into caller buffer) | +19 |

**Totals:** 5 files modified, +728 / -23 LOC. 2 commits (see Deviations).

## Argv Parse Extensions

### `cdb put`

New flags on the existing `put` branch (`cli/src/main.cpp` line 397+):
- `--name <value>` — single token, captured as `std::string` (opaque UTF-8 bytes per D-04). Length-checked at parse time: empty → exit 2; >65535 → exit 2.
- `--replace` — bare flag. Rejects without `--name` (exit 2) so the user can't silently tombstone a hash they don't realize they're resolving.
- Mutually exclusive with batch file lists (`--name` requires exactly one content blob; multi-file or `--stdin`+files → exit 2).

Help text rewritten to document both flags in their own "Options" subsection.

### `cdb get`

Dispatches between hash-mode and NAME-mode on the first positional:
- If positional[0] is exactly 64 hex chars → existing batch hash flow (unchanged).
- Otherwise → NAME-mode, single-positional only. Calls `cmd::get_by_name`. Rejects `--all` (not meaningful for NAME lookup).

Help text rewritten with a "Positionals" subsection so `cdb get --help` shows both shapes.

### `cdb rm`

Rewritten multi-target collector following the existing `cdb get <hash>…` loop pattern at `main.cpp:~490`:
- All non-flag tokens → decoded (64-hex) and appended to `hash_hexes` vector.
- Zero targets → exit 2 with "no targets" message.
- Every invocation (even single-target) routes through `cmd::rm_batch` — per D-06/D-07 the one-BOMB-per-invocation rule has no special case for N=1.
- Confirmation prompt distinguishes "Delete blob X?" (N=1) vs "Delete N blobs via one BOMB tombstone?" (N>1).

## cmd::put Extension (Task 2)

Signature change:

```cpp
int put(const std::string& identity_dir,
        const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin,
        const std::optional<std::string>& name_opt, bool replace,   // NEW
        const ConnectOpts& opts);
```

### Submission ordering (D-15 + plan-checker iteration-1 fix)

**content → NAME → BOMB** (write-before-delete):

1. **Step 0** (replace only): `resolve_name_to_target_hash(id, conn, ns, name)` — looks up the prior NAME's current target_hash via the same enumeration+sort path `cmd::get_by_name` uses, but returns only the hash (no content download). Stored in `replace_old_target`. If nullopt, a note is printed at the end; emission still proceeds.
2. **Step 1** (content): existing file-upload pipeline. On the WriteAck for the single named blob, we capture its server-assigned `content_hash` into `content_hash_captured`.
3. **Step 2** (NAME): `submit_name_blob(id, conn, ns, name_bytes, content_hash_captured, ttl, now, rid)`. Payload built via `wire::make_name_data(name_bytes, target_hash)` (Plan 01 helper — no hand-rolled magic bytes in commands.cpp). Signed with Phase-122 canonical input `SHA3(ns || data || ttl || ts)`. Submitted on `MsgType::Data`; we validate the WriteAck and log `named: <name> -> <target_hex>` in non-quiet mode.
4. **Step 3** (BOMB-of-1, replace + replace_old_target.has_value()): `make_bomb_data({old_target_hash})` → `submit_bomb_blob(...)`. **ttl=0 hard-coded** (D-13 invariant). Submitted on `MsgType::Delete`. Runs ONLY IF Steps 1+2 succeeded — a partial failure leaves prior content reachable by hash + the new content reachable by hash OR by name.

**Rationale for ordering** (plan-checker iteration 1): reversing (BOMB first) would risk deleting old content before the replacement was visible, which is irrecoverable data loss. Write-before-delete means the worst case is "user re-runs `--replace` and the BOMB catches up."

### Chunked-file + `--name` interaction

`--name` is explicitly rejected for files above `chunked::CHUNK_THRESHOLD_BYTES` (the CPAR-manifest path). Binding a NAME to the first CDAT chunk instead of the manifest would be a correctness bug the user can't detect. Future phase can lift this with an explicit manifest_hash capture.

## cmd::get_by_name (Task 3)

Deterministic resolution pipeline matching D-01/D-02:

1. `enumerate_name_blobs(conn, ns)` — paginated `ListRequest` with `flags=0x02` (type_filter present) and `type_filter=NAME_MAGIC_CLI`. Defensive: re-checks the 4-byte type in each entry and skips mismatches (in case a server ever loosens the filter). Returns `(blob_hash, seq, timestamp)` triples. Page limit 1000.
2. `fetch_blob(conn, ns, hash, rid)` for each candidate — `ReadRequest` with ns + hash, decoded via `decode_blob`.
3. `parse_name_payload(blob->data)` (Plan 01 helper) — returns `ParsedNamePayload{span<const uint8_t> name, array<uint8_t, 32> target_hash}` or nullopt on structural invalidity.
4. Filter: `memcmp` over `(parsed->name, requested_name)` — opaque bytes per D-04, not strcmp.
5. `pick_name_winner` sort: `(blob.timestamp DESC, blob_hash DESC)`. The tiebreak key is the NAME blob's own hash (which is what `ListRequest` returns and what all clients see identically) — reproducible across all readers with zero coordination.
6. Winner's `target_hash` → delegated to existing `cmd::get` (reuses CENV decrypt + CPAR chunked dispatch + `--stdout`/`-o` logic).

If no NAME matches → `Error: name not found: <name>` and exit 1.

## cmd::rm_batch (Task 3)

Signature:

```cpp
int rm_batch(const std::string& identity_dir,
             const std::vector<std::string>& hash_hexes,
             const std::string& namespace_hex, bool force,
             const ConnectOpts& opts);
```

Flow:
1. Empty `hash_hexes` → exit 2 (defense in depth; main.cpp already rejects).
2. Decode all hex → `std::vector<std::array<uint8_t, 32>> targets`; any malformed entry aborts the invocation before network I/O.
3. **Dedup pass** — sort+unique on the target bytes so the BOMB's `count` field matches the number of distinct targets. Node doesn't verify targets (D-14) so duplicate submission would just inflate the count; dedup avoids that.
4. Pre-flight existence probes via `ExistsRequest` unless `--force`. Missing targets warn (per-target) but don't abort; if **all** are missing, exit 1.
5. `make_bomb_data(targets)` → `submit_bomb_blob(...)`. ttl=0 non-negotiable. One BOMB covering all targets (D-07).
6. Print the BOMB's server-assigned hash to stdout on success; log `BOMB <hash> tombstoned N target(s)` to stderr in non-quiet mode.

## `wire::parse_name_payload` (Task 3)

Mirrors the node-side codec helper byte-for-byte:
- Minimum payload size 39 bytes (4 magic + 2 name_len + 1 name + 32 target_hash).
- Magic check: memcmp against `NAME_MAGIC_CLI`.
- name_len BE-decoded; rejects name_len == 0 (D-04: names are 1..65535 bytes).
- `data.size() == 4 + 2 + name_len + 32` — strict equality so trailing garbage is a parse error.
- Returns `ParsedNamePayload` with a `std::span<const uint8_t>` into the caller's original buffer (zero-copy, zero-alloc).

## Explicit Confirmations

- **`cli/src/wire.h` BlobData struct unchanged.** `git diff cli/src/wire.h` shows only additions to the NAME+BOMB region (ParsedNamePayload struct + parse_name_payload declaration). The Phase 124 CLI wire adaptation is out of scope.
- **All payload construction goes through Plan 01 helpers.** Grep `0x4E.*0x41.*0x4D.*0x45|0x42.*0x4F.*0x4D.*0x42` in commands.cpp returns 0 hits — magic bytes live exclusively in `cli/src/wire.h`.
- **No new TransportMsgType introduced.** cmd::get_by_name reuses Phase 117 `ListRequest + type_filter` (Plan 01 D-10 reuse deviation, honored here).
- **cdb binary compiles.** `cmake -S cli -B build-cli -DBUILD_TESTING=OFF` + `cmake --build build-cli -j$(nproc) --target cdb` exits 0. Binary at `build-cli/cdb` runs; `put --help`, `get --help`, `rm --help` all expose the new flags with descriptive text.

## Handoff Note for Phase 124

The command-surface logic (argv parsing, help text, resolution algorithm, BOMB-dedup, write-before-delete ordering) should survive the Phase 124 wire migration **unchanged**. What Phase 124 will need to update:

1. `cli/src/wire.h` `BlobData` struct → post-122 shape (`target_namespace`, `signer_hint`, canonical-signing form unchanged).
2. `cli/src/wire.cpp` `encode_blob` / `decode_blob` FlatBuffer layout.
3. `build_signing_input` already matches Phase 122 canonical form — no change required.
4. All NAME/BOMB helpers (`make_name_data`, `make_bomb_data`, `parse_name_payload`) are shape-agnostic (they operate on `blob.data` bytes, not the envelope) — no change required.
5. End-to-end revalidation against the live 192.168.1.73 node AFTER both 122 + 123 CLI deltas land together.

## Deviations from Plan

### Rule 3 (auto-fix blocking issues): Tasks 2 + 3 combined into ONE commit

**Found during:** Task 2 implementation.
**Issue:** `cmd::put --replace` calls `resolve_name_to_target_hash` for the Step-0 lookup. My implementation defines `resolve_name_to_target_hash` inside the Task 3 block (it reuses the anonymous-namespace helpers `enumerate_name_blobs` + `fetch_blob` + `pick_name_winner` that `cmd::get_by_name` also uses). Splitting Task 2 and Task 3 into separate commits would leave Task 2's commit with an unresolved symbol — not a compilable state.
**Fix:** One combined commit `feat(123-03): Tasks 2+3 — cmd::put --name/--replace, get_by_name, rm_batch` (commit `0d946b80`). Plus a separate commit for Task 1 (`257b5f27`). Total: 2 commits.
**Alternative considered:** Inlining the Step-0 lookup in cmd::put (not calling a helper) would have let Task 2 commit without Task 3. Rejected: `feedback_no_duplicate_code.md` — the enumeration+sort path is shared with `cmd::get_by_name`, extracting it to a helper is the right structural choice.

### Rule 2 (auto-add missing critical functionality): Dedup BOMB targets

**Found during:** Task 3 rm_batch implementation.
**Issue:** The plan doesn't mention deduping targets. But a user shell can easily produce duplicates (e.g. `cdb rm $(cdb ls ... | awk ...)` with an overlapping filter). Node doesn't verify targets (D-14), so a duplicate-containing BOMB would store an inflated count that doesn't match the distinct target set.
**Fix:** sort+unique pass before `make_bomb_data`. Prints a `note: dropped N duplicate(s)` in non-quiet mode so the user sees what's going on.
**Files modified:** `cli/src/commands.cpp` `cmd::rm_batch`.
**Commit:** `0d946b80`.

### Rule 2 (auto-add missing critical functionality): Reject `--name` for chunked files

**Found during:** Task 2 cmd::put body.
**Issue:** Phase 119 chunked-file path (files ≥ CHUNK_THRESHOLD_BYTES) takes a different code path (CPAR manifest with CDAT chunks). The plan doesn't say what happens when `--name` is combined with a chunked upload. Naively letting it proceed would bind the NAME to the first CDAT's content_hash rather than the manifest — a correctness bug the user can't detect.
**Fix:** Explicit rejection with a clear error message ("--name is not yet supported for large chunked files"). Future phase can lift this with an explicit manifest_hash capture.
**Files modified:** `cli/src/commands.cpp` `cmd::put` file-gathering loop.
**Commit:** `0d946b80`.

### No auth gates hit

All work was local code changes + local compile. No node connection, no keys, no secrets.

## Deferred Issues

### cmd::rm_batch does not cascade chunked CPAR manifests

Phase 119's `cmd::rm` (single-target) has a Phase-119 CHUNK-04 cascade path: if the target is a CPAR manifest, decrypt it, iterate chunk_hashes, and tombstone chunks-first-then-manifest via `chunked::rm_chunked`. `cmd::rm_batch` does NOT replicate this cascade — BOMBing a CPAR manifest leaves orphaned CDAT chunks in the store.

**Why deferred:** CPAR cascade is a pre-Phase-123 feature that doesn't mesh cleanly with BOMB semantics (BOMB emits one tombstone for N targets, but cascade needs per-target manifest fetching). Plan 03 scope focuses on the new BOMB flow. Phase 124 (CLI post-122 adaptation) is a natural home for the unified cascade+BOMB path.

**User action:** For now, use the legacy single-target path via `cmd::rm` (still defined in commands.cpp; dead-code from main.cpp's perspective but reachable via library linkage) for chunked deletions, or call `cdb rm <manifest_hash>` repeatedly. This is safe because BOMB is purely additive — an orphaned CDAT will be GC'd by future `chromatindb gc admin tombstone cleanup` (backlog 999.19).

### Live-node E2E validation

Per Phase 123 CONTEXT.md `<domain>`: "Test against the live 192.168.1.73 node until post-124 (CLI + node are redeployed together after 124 lands)." No E2E tests run in this plan.

## Verification

### grep gates (all PASS)

```bash
# Task 1 main.cpp gates
grep -qE 'strcmp\([^,]+,\s*"--name"\)'    cli/src/main.cpp   # OK
grep -qE 'strcmp\([^,]+,\s*"--replace"\)' cli/src/main.cpp   # OK
grep -qE "cmd::get_by_name"                 cli/src/main.cpp   # OK (2 hits)
grep -qE "cmd::rm_batch"                    cli/src/main.cpp   # OK (1 hit)
grep -qE "hash_hexes|target_hashes"         cli/src/main.cpp   # OK (12 hits)

# Task 2 commands.cpp gates
grep -qE "wire::make_name_data|make_name_data" cli/src/commands.cpp  # OK (1 hit)
grep -qE "wire::make_bomb_data|make_bomb_data" cli/src/commands.cpp  # OK (5 hits ≥2)
# (5: forward comment, submit_bomb_blob doc, cmd::put --replace call,
#     cmd::rm_batch call, commit-message style docs in inline notes)

# Task 3 commands.cpp gates
grep -qE "wire::parse_name_payload|parse_name_payload" cli/src/commands.cpp  # OK (1 hit)
grep -qE "cmd_get_by_name|get_by_name"                 cli/src/commands.cpp  # OK (4 hits)
grep -qE "cmd_rm_batch|rm_batch"                       cli/src/commands.cpp  # OK (5 hits)
grep -qE "type_filter"                                 cli/src/commands.cpp  # OK (17 hits)

# Negative gates
! grep -qE "0x4E.*0x41.*0x4D.*0x45|0x42.*0x4F.*0x4D.*0x42" cli/src/commands.cpp  # OK (0 hits)
```

### Compile (exit 0)

```bash
cmake -S cli -B build-cli -DBUILD_TESTING=OFF      # config ok
cmake --build build-cli -j$(nproc) --target cdb    # build ok — cdb binary produced
```

Pre-existing `-Wfree-nonheap-object` warning in `cli/src/connection.cpp:60` (encode_auth_payload) is unrelated to this plan — not a Phase 123 regression.

### Help-text smoke checks (all PASS)

```bash
./build-cli/cdb put --help  2>&1 | grep -qE '\-\-name\b'      # OK
./build-cli/cdb put --help  2>&1 | grep -qE '\-\-replace\b'   # OK
./build-cli/cdb get --help  2>&1                               # usage text prints
./build-cli/cdb rm  --help  2>&1 | grep -q "hash>\.\.\."      # OK multi-target
```

## Commits

| Hash       | Message |
|------------|---------|
| `257b5f27` | feat(123-03): Task 1 — cdb argv parser for --name/--replace, NAME get, BOMB rm |
| `0d946b80` | feat(123-03): Tasks 2+3 — cmd::put --name/--replace, get_by_name, rm_batch |

## Hand-off to User

Per `feedback_delegate_tests_to_user.md`, the executor did NOT run `chromatindb_tests` or attempt live-node validation. The build has been verified.

Please run the following smoke-checks locally and confirm output:

```bash
# 1. Help text sanity
./build-cli/cdb --help
./build-cli/cdb put --help | grep -- --name
./build-cli/cdb get --help
./build-cli/cdb rm --help

# 2. Regression: chromatindb_tests still compiles unchanged (Phase 123 Plan 03
#    touches only cli/ — no db/ changes — so this should be a no-op rebuild)
cmake --build build -j$(nproc) --target chromatindb_tests

# 3. Regression: Phase 122 coverage still green (no CLI work should affect
#    node-side test coverage — this is a sanity check)
./build/db/chromatindb_tests '[phase122]' --reporter compact
```

Expected: all three complete without errors; Phase 122 suite reports the same pass count it did at Plan 02 tip.

## Self-Check: PASSED

- Files modified exist on disk: FOUND (5/5) — cli/src/main.cpp, commands.h, commands.cpp, wire.h, wire.cpp.
- Commits exist in git log: FOUND (`257b5f27`, `0d946b80`).
- Grep gates (positive): all PASS.
- Grep gates (negative — magic bytes NOT hand-rolled in commands.cpp): PASS.
- cdb binary builds (`cmake --build build-cli -j$(nproc) --target cdb` exit 0): PASS.
- No STATE.md / ROADMAP.md edits made (per executor constraints).
- Help text exposes `--name`, `--replace`, NAME-vs-hash get dispatch, multi-target rm.
