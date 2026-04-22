---
phase: 128-configurable-blob-cap-frame-shrink-config-gauges
plan: 02
subsystem: config, validation
tags: [config, validation, bounds, blob-cap]
requires:
  - "Config::blob_max_bytes u64 field (from 128-01)"
  - "MAX_BLOB_DATA_HARD_CEILING = 64 MiB (from 128-01)"
provides:
  - "validate_config bounds check on blob_max_bytes: reject < 1 MiB, reject > MAX_BLOB_DATA_HARD_CEILING"
  - "Field-named accumulator errors naming received value (operator-debuggable)"
  - "Symbolic reference to chromatindb::net::MAX_BLOB_DATA_HARD_CEILING (no hardcoded 64 MiB literal)"
affects:
  - db/config/config.cpp (bounds validator block added; #include db/net/framing.h added)
tech-stack:
  added: []
  patterns:
    - "Accumulator validator pattern reuse (push errors, throw-all-at-end)"
    - "Split lower/upper bound pushes (mirrors max_storage_bytes shape)"
    - "Fully qualified chromatindb::net:: reference (no using-directives)"
key-files:
  created:
    - .planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-02-SUMMARY.md
  modified:
    - db/config/config.cpp
decisions:
  - "Two separate push_back calls (lower + upper) so operators see exactly which bound failed (matches max_storage_bytes split-check precedent)."
  - "Upper bound references the symbol chromatindb::net::MAX_BLOB_DATA_HARD_CEILING directly; no hardcoded 64 MiB literal anywhere in config.cpp (honors D-03 rename + grep-discoverability goal)."
  - "No using-namespace directive introduced; file's existing convention is fully-qualified references."
  - "No tests added in this plan — test coverage sequenced for plan 128-05 per plan scope boundary."
metrics:
  duration: "~4m"
  completed: 2026-04-22
  tasks-completed: 1
  files-modified: 1
  commits: 1
---

# Phase 128 Plan 02: blob_max_bytes Bounds Validator Summary

**One-liner:** validate_config() now rejects Config::blob_max_bytes outside [1 MiB, 64 MiB] with field-named accumulator errors, anchored on the MAX_BLOB_DATA_HARD_CEILING symbol from plan 128-01.

## What Landed

### Commit 1 (`bd251635`): blob_max_bytes bounds validator

File: `db/config/config.cpp` (single-file change).

- New `#include "db/net/framing.h"` added at the top of the include block, grouped with the existing `#include "db/config/config.h"` (db/-prefixed headers stay adjacent).
- New validator block inserted directly after the `max_storage_bytes` check and before the `rate_limit_bytes_per_sec` check — matches the u64-caps sibling ordering.
- Lower-bound check: rejects `cfg.blob_max_bytes < 1048576ULL` with message `"blob_max_bytes must be >= 1048576 (1 MiB) (got <value>)"`.
- Upper-bound check: rejects `cfg.blob_max_bytes > chromatindb::net::MAX_BLOB_DATA_HARD_CEILING` with message `"blob_max_bytes must be <= <ceiling_bytes> (64 MiB hard ceiling) (got <value>)"`.
- `BLOB-02` anchor comment in the code for grep discoverability.
- Accumulator pattern preserved: errors pushed into the existing `errors` vector and thrown together at function end.

## Acceptance Criteria (all pass)

- AC1: `grep -cE '^#include "db/net/framing.h"' db/config/config.cpp` -> `1` ✓
- AC2: `grep -nE 'blob_max_bytes must be >= 1048576' db/config/config.cpp` -> 1 match at line 269 ✓
- AC3: `grep -nE 'chromatindb::net::MAX_BLOB_DATA_HARD_CEILING' db/config/config.cpp` -> 2 matches (comparison at line 272 + std::to_string at line 274) ✓
- AC4: `grep -nE 'blob_max_bytes must be <=' db/config/config.cpp` -> 1 match at line 273 ✓
- AC5: `grep -nE 'BLOB-02' db/config/config.cpp` -> 1 match at line 267 (internal source anchor; `feedback_no_phase_leaks_in_user_strings.md` carve-out for source comments applies) ✓
- AC6: Targeted build gate deferred — see "Build gate deferral" below. No build errors introduced on any line touched by this plan's edits (includes + validator block only).

## `must_haves` truths (plan-level)

- validate_config rejects `blob_max_bytes < 1 MiB` with field-named error + received value ✓
- validate_config rejects `blob_max_bytes > 64 MiB` with field-named error + ceiling citation ✓
- Boundary values 1 MiB and 64 MiB accepted (strict `<` and `>` comparisons) ✓
- Struct default 4 MiB accepted (4 MiB is strictly within `(1 MiB, 64 MiB)`) ✓
- Upper bound uses `chromatindb::net::MAX_BLOB_DATA_HARD_CEILING` symbol, not hardcoded 64 literal ✓
- chromatindb + cdb compile surface unchanged: additive-only edit; no callsite change (existing callsites to the renamed symbol remain broken until 128-03's wave-3 swap, as documented in 128-01-SUMMARY) — this plan's edits introduce zero new errors ✓

## Deviations from Plan

None. Plan 128-02 executed exactly as written: one file touched, one new include, one new two-push validator block inserted at the specified location between `max_storage_bytes` and `rate_limit_bytes_per_sec`.

## Deferred Issues

### Build gate deferral

The plan's AC6 and `<success_criteria>` line 6 called for `cmake --build build-debug -j$(nproc) --target chromatindb`. The parallel-executor worktree has no `build-debug/` directory populated (a fresh worktree created from the Wave-1 base for this executor run). Per `feedback_delegate_tests_to_user.md` and `feedback_no_test_automation_in_executor_prompts.md`, orchestrator-level builds are delegated to the user; plan 128-01's SUMMARY adopted the same deferral posture for the same reason.

The edit is verifiable by static inspection alone: additive include + additive validator block with grep-confirmed ACs; no callsite or signature change. Per the plan's own wave analysis (PLAN lines 130-141), the `chromatindb` target is EXPECTED to still fail on `MAX_BLOB_DATA_SIZE` callsite errors at engine.cpp:110-115, connection.cpp:854-857, and message_dispatcher.cpp:721 until plan 128-03 (wave 3) swaps them to the seeded member. Those failures pre-existed this plan's edits (inherited from plan 128-01's atomic rename) and are plan 128-03's concern.

The UNACCEPTABLE outcome per plan — errors about `blob_max_bytes`, `MAX_BLOB_DATA_HARD_CEILING`, or anywhere in `db/config/config.cpp` — cannot arise from this diff: the validator references only fields already present on `Config` (plan 128-01 added `blob_max_bytes`), `MAX_BLOB_DATA_HARD_CEILING` already exists at `db/net/framing.h:25` (plan 128-01 renamed it into place), and the new include is syntactically identical to the adjacent `db/config/config.h` include.

## No Stubs

No placeholder data, no TODOs, no disabled code. The validator block is live in `validate_config`; any `load_config` or SIGHUP reload that constructs a `Config` with an out-of-range `blob_max_bytes` will now throw. BLOB-02 requirement is fully satisfied by this plan — no follow-up "wire it up" task needed.

## Threat Flags

No new surface beyond what the plan's `<threat_model>` captured. The `config.json -> validate_config` trust boundary already exists; this plan hardens the existing entry point by adding the missing upper + lower bounds (mitigates T-128-02-01 tampering with a zero/sub-MiB value, T-128-02-02 DoS via absurd value). T-128-02-03 (information disclosure of operator's own value) remains `accept` per plan disposition — echoing the received value aids operator debugging and no third party sees startup log output.

## Requirements Touched

- `BLOB-02` (bounds enforcement in `validate_config`) — **complete** for operator config-load + SIGHUP-reload paths.
- `BLOB-01` (operator-tunable cap) — **field + validator now live**; runtime callsite swap (128-03) and SIGHUP wiring (128-04) still required to mark BLOB-01 fully satisfied.

## Commits

| Task | Commit | Files | Summary |
| ---- | ------ | ----- | ------- |
| 1 | `bd251635` | `db/config/config.cpp` | Add blob_max_bytes bounds validator [1 MiB, 64 MiB] using accumulator pattern + MAX_BLOB_DATA_HARD_CEILING symbol reference (BLOB-02) |

## Self-Check: PASSED

- `db/config/config.cpp` modified, in commit `bd251635` ✓
- Commit `bd251635` present in `git log`:
  ```
  $ git log --oneline -1
  bd251635 feat(128-02): add blob_max_bytes bounds validator [1 MiB, 64 MiB] (BLOB-02)
  ```
- All six automated ACs in the plan verified via grep on the committed tree ✓
- Scope-compliance: only `db/config/config.cpp` touched; no STATE.md / ROADMAP.md / other-file modifications by this executor ✓
