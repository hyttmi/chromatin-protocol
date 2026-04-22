---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
plan: 01
subsystem: framing, config
tags: [framing, config, invariants, shrink, blob-cap]
requires: []
provides:
  - "MAX_FRAME_SIZE = 2 MiB (both db and cli sides)"
  - "MAX_BLOB_DATA_HARD_CEILING = 64 MiB (renamed from MAX_BLOB_DATA_SIZE)"
  - "Paired framing invariant static_asserts (lower + upper bound)"
  - "TRANSPORT_ENVELOPE_MARGIN constexpr referenced by upper-bound assert"
  - "Config::blob_max_bytes u64 field with 4 MiB default"
  - "JSON parse + known_keys recognition for blob_max_bytes"
affects:
  - db/engine/engine.cpp (will not compile until 128-03 swaps callsites)
  - db/net/connection.cpp (will not compile until 128-03 swaps callsites)
  - db/peer/message_dispatcher.cpp (will not compile until 128-03 swaps callsites)
  - db/tests/net/test_framing.cpp (assertions target old symbol, 128-03/05 rewrites)
  - db/tests/engine/test_engine.cpp (oversize tests reference old symbol, 128-03/05 rewrites)
  - db/tests/peer/test_peer_manager.cpp (NodeInfoResponse assertions, 128-03 rewrites)
tech-stack:
  added: []
  patterns:
    - "Atomic cross-file commit for header invariants (D-10)"
    - "Paired static_assert pair to pin bidirectional compile-time invariant (D-11)"
    - "Field-declaration-only commit; validator and wiring in downstream plans (D-01/D-03)"
key-files:
  created: []
  modified:
    - db/net/framing.h
    - cli/src/connection.cpp
    - db/config/config.h
    - db/config/config.cpp
decisions:
  - "Renamed MAX_BLOB_DATA_SIZE -> MAX_BLOB_DATA_HARD_CEILING to disambiguate build-time invariant from operational cap (D-03/D-05)."
  - "2 MiB frame size shipped atomically on both db and cli sides (D-10) — no two-step, no trailing binary."
  - "Upper-bound static_assert wording pins the relationship MAX_FRAME_SIZE <= 2 * STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN so future authors cannot silently raise the frame ceiling (D-11)."
  - "blob_max_bytes default is 4 MiB; bounds [1 MiB, 64 MiB] will be enforced in plan 128-02's validate_config extension (D-01)."
  - "Transient build breakage accepted per D-10 atomic-commit posture; callsite swap is wave-3 (plan 128-03) concern."
metrics:
  duration: "~3m (220s)"
  completed: 2026-04-22
  tasks-completed: 2
  files-modified: 4
  commits: 2
---

# Phase 128 Plan 01: FRAME Shrink + Config Field Summary

**One-liner:** Atomic 2 MiB frame shrink + MAX_BLOB_DATA_SIZE -> MAX_BLOB_DATA_HARD_CEILING rename + paired static_assert + Config::blob_max_bytes field declaration.

## What Landed

### Commit 1 (`999f5d5b`): FRAME shrink + rename + paired invariant
Files: `db/net/framing.h`, `cli/src/connection.cpp` (single commit per D-10).

- `MAX_FRAME_SIZE`: 110 MiB -> 2 MiB in both `db/net/framing.h` and `cli/src/connection.cpp`.
- `MAX_BLOB_DATA_SIZE` (500 MiB) removed, replaced with `MAX_BLOB_DATA_HARD_CEILING` (64 MiB) per D-03/D-05.
- New upper-bound `static_assert` pins `MAX_FRAME_SIZE <= 2 * STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN` (D-11). Existing lower-bound `static_assert` (>= 2 * STREAMING_THRESHOLD) preserved intact per 126-01-SUMMARY §"Assertions Landed".
- `TRANSPORT_ENVELOPE_MARGIN` already lived in `framing.h` from Phase 126 (D-12 target is the static_assert wording, not a new promotion). The new upper-bound assert references it.
- Inline comment at `cli/src/connection.cpp:700` updated from `MAX_FRAME_SIZE (110 MiB)` to `MAX_FRAME_SIZE (2 MiB)` so in-code documentation stays consistent.
- CLI side: no CLI-side upper-bound static_assert added — per D-13 the CLI keeps the current hand-mirror level; shared-header unification is explicitly deferred.

### Commit 2 (`5ced28bf`): Config::blob_max_bytes field declaration
Files: `db/config/config.h`, `db/config/config.cpp`.

- `Config::blob_max_bytes` u64 field inserted between `max_storage_bytes` and `rate_limit_bytes_per_sec`. Default: `4ULL * 1024 * 1024` (4 MiB). Comment carries the `BLOB-01/02` anchor + target bounds `[1 MiB, 64 MiB]` for grep discoverability.
- `load_config` parses `"blob_max_bytes"` from JSON via the same `j.value(key, default)` pattern used for sibling u64 fields.
- `known_keys` set updated so the warn-on-unknown-key path recognizes the new field.
- `validate_config` NOT modified — bounds enforcement lands in plan 128-02 per scope boundary.

## Acceptance Criteria (all pass)

### Task 1 — Framing
- AC1: `MAX_FRAME_SIZE = 2 * 1024 * 1024` at framing.h:16 ✓
- AC2: `MAX_BLOB_DATA_HARD_CEILING = 64ULL * 1024 * 1024` at framing.h:25 ✓
- AC3: `grep -c 'MAX_BLOB_DATA_SIZE' db/net/framing.h` returns 0 ✓
- AC4: Upper-bound `static_assert` at framing.h:44 ✓
- AC5: Lower-bound `static_assert` at framing.h:38 (unmodified) ✓
- AC6: `TRANSPORT_ENVELOPE_MARGIN` constexpr at framing.h:36 ✓
- AC7: CLI `MAX_FRAME_SIZE = 2 * 1024 * 1024` at cli/src/connection.cpp:36 ✓
- AC8: No `MAX_FRAME_SIZE = 110` literal in CLI ✓
- AC9: CLI lower-bound static_assert intact at cli/src/connection.cpp:38 ✓
- AC10: No `MAX_FRAME_SIZE (110 MiB)` comment residue in CLI ✓

### Task 2 — Config field
- AC1: `uint64_t blob_max_bytes = 4ULL * 1024 * 1024` at config.h:26 ✓
- AC2: BLOB-01 anchor in comment ✓
- AC3: `cfg.blob_max_bytes = j.value("blob_max_bytes", ...)` at config.cpp:41 ✓
- AC4: `"blob_max_bytes"` appears 2x in config.cpp (parse + known_keys) ✓
- AC5: No validator (`blob_max_bytes must be ...`) yet — deferred to 128-02 ✓
- AC6: Targeted build gate NOT executed — see "Build gate deferral" below.

## Deviations from Plan

### Rule 1 - Consistency fix

**1. [Rule 1 - Consistency] Reworded comment in framing.h to avoid naming the old symbol**

- **Found during:** Task 1 verification (AC3).
- **Issue:** The plan's step 2 replacement text for the `MAX_BLOB_DATA_HARD_CEILING` comment block included the phrase `MAX_BLOB_DATA_SIZE (Phase 128 D-03/D-05)` as historical documentation. But the plan's own AC3 (`! grep -n 'MAX_BLOB_DATA_SIZE' db/net/framing.h`) and the `must_haves` truth ("no symbol named MAX_BLOB_DATA_SIZE remains in db/net/framing.h") both require zero occurrences of that token anywhere in the file. Using the plan-provided comment verbatim would have made AC3 fail.
- **Fix:** Reworded the comment to read "Renamed in Phase 128 (D-03/D-05) from the previous ambiguous 'data size' name" — preserves the documentary intent while satisfying AC3 literally and the `must_haves` truth.
- **Files modified:** `db/net/framing.h` (comment text only).
- **Commit:** `999f5d5b` (folded into the atomic Task 1 commit).

### Build gate deferral (documented per PLAN success criteria line: "Transient build breakage on chromatindb target is EXPECTED and DOCUMENTED")

The plan's Task 2 AC6 called for `cmake --build build-debug -j$(nproc) --target config_obj || cmake --build build-debug -j$(nproc) --target chromatindb`. No `build-debug/` directory exists in this worktree (fresh parallel-executor env). Per `feedback_delegate_tests_to_user.md` and `feedback_no_test_automation_in_executor_prompts.md`, orchestrator-level builds are user-delegated. The plan itself acknowledges (line 206-208): "Task 2 here DOES NOT attempt to build — the chromatindb build gate is deferred to plan 128-03, which owns the callsite-swap that re-greens the tree. This is the explicit wave architecture." Build gate deferral is consistent with that posture, not a deviation.

## Expected Transient Breakage

Per PLAN line 198-206, the following callsites will fail to compile until plan 128-03 swaps them from `chromatindb::net::MAX_BLOB_DATA_SIZE` to the seeded `Config::blob_max_bytes`:

- `db/engine/engine.cpp:110, 112, 115`
- `db/net/connection.cpp:854, 855, 856, 857`
- `db/peer/message_dispatcher.cpp:721`
- `db/tests/net/test_framing.cpp:187-194`
- `db/tests/engine/test_engine.cpp:484, 488, 499, 503, 526, 541, 549`
- `db/tests/peer/test_peer_manager.cpp:2895, 2900, 2907`

This is the explicit wave architecture. Plan 128-03 (wave 3) owns the re-greening.

## No Stubs

No placeholder data, no TODOs left behind. The `Config::blob_max_bytes` field defaults to `4 MiB` — a working default per BLOB-01 requirement, not a stub. The bounds validator (plan 128-02) and runtime callsite swap (plan 128-03) are sequenced follow-ups, not stubs.

## Threat Flags

No new surface beyond what the plan's `<threat_model>` already captured. The header constants are build-time, not runtime I/O; the new `Config::blob_max_bytes` field crosses the existing `config.json` boundary already enumerated in `T-128-01-02`. No threat flags to raise.

## Requirements Touched

- `FRAME-01` (MAX_FRAME_SIZE shrink to 2 MiB) — **complete** for this plan's scope.
- `FRAME-02` (paired static_assert invariant) — **complete**.
- `BLOB-01` (operator-tunable cap field) — **field declaration complete**; validator (128-02) + runtime read (128-03) + SIGHUP (128-04) follow. Mark `BLOB-01` fully satisfied only after 128-04 lands.

## Commits

| Task | Commit | Files | Summary |
| ---- | ------ | ----- | ------- |
| 1 | `999f5d5b` | `db/net/framing.h`, `cli/src/connection.cpp` | FRAME shrink + MAX_BLOB_DATA_HARD_CEILING rename + paired static_assert (atomic per D-10) |
| 2 | `5ced28bf` | `db/config/config.h`, `db/config/config.cpp` | Config::blob_max_bytes field declaration + JSON parse + known_keys entry |

## Self-Check: PASSED

- `db/net/framing.h` ✓ modified, in commit `999f5d5b`
- `cli/src/connection.cpp` ✓ modified, in commit `999f5d5b`
- `db/config/config.h` ✓ modified, in commit `5ced28bf`
- `db/config/config.cpp` ✓ modified, in commit `5ced28bf`
- Commit `999f5d5b` ✓ present in `git log`
- Commit `5ced28bf` ✓ present in `git log`
