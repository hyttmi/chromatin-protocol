---
phase: 127-nodeinforesponse-capability-extensions
plan: 03-cli-decoder
subsystem: cli-protocol
tags: [cli, nodeinfo, wire-decoder, capability-discovery, cdb-info]

# Dependency graph
requires:
  - phase: 127-nodeinforesponse-capability-extensions
    plan: 01-encoder
    provides: "node-side encoder writing the 4 new fixed-width caps (blob u64 / frame u32 / rate u64 / subs u32) between storage_max and types_count in NodeInfoResponse"
provides:
  - "CLI decoder half of the v4.2.0 capability-discovery contract: `cdb info` now reads the 4 new NodeInfoResponse fields and prints 4 new human-readable lines (Max blob, Max frame, Rate limit, Max subs) after the existing Quota line"
  - "Stale `[git_hash_len:1][git_hash_str]` layout comment removed from cli/src/commands.cpp — comment now documents the full post-Phase-127 wire layout (D-12 passing fix)"
affects:
  - 127-04-encoder-tests
  - 130-cli-auto-tuning
  - 131-documentation-reconciliation

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "CLI wire-decoder extension: reuse the existing `read_u8` / `read_u32` / `read_u64` lambdas (bounds-checked via payload size, throw on truncation) and the existing `humanize_bytes` helper — no new inline byte-reader, no hand-rolled humanizer (feedback_no_duplicate_code.md)."
    - "Zero-value render policy: operator-facing 0 sentinels for uncapped resources print `unlimited` rather than `0 bytes` or `0` — rate limit and subscription cap follow the same convention already used by `Quota:` line."

key-files:
  created: []
  modified:
    - "cli/src/commands.cpp - Three edits inside info() at lines 2188-2308: (1) rewrote 5-line comment block at 2233-2237 to document full post-Phase-127 wire layout (dropped dead git_hash entries, +7 lines); (2) inserted 4 new `read_*()` calls after `auto storage_max = read_u64();` at 2278 (+4 lines); (3) inserted 4 new `std::printf` lines after the `if (storage_max == 0) { ... }` block at 2294 (+12 lines). Net: +27 / -5."

key-decisions:
  - "Field read order mirrors encoder: blob(u64) -> frame(u32) -> rate(u64) -> subs(u32) per D-02. Inserted as a contiguous block between `storage_max` read and the first printf to keep the parse phase monolithic."
  - "Render order identical to read order, appended after `Quota:` line per D-06. No reordering of existing lines."
  - "Zero-value handling (D-07): `rate_limit_bytes_per_sec == 0` and `max_subscriptions == 0` both branch to `unlimited`; non-zero rate renders with `/s` suffix via `humanize_bytes`, non-zero subs renders as plain `%u` integer."
  - "No session caching into `ConnectOpts` / `Session` (D-08 carve-out): decoder PRINTS the values; Phase 130 owns the CLI auto-tune cache."
  - "No new Catch2 test at CLI layer per D-11. Correctness verified transitively by Plan 127-04's byte-level integration test."
  - "Build target is `cli/build-debug` (separate CMake project from the node's root CMakeLists.txt); plan AC10 wording of `cmake --build build-debug --target cdb` resolves to `cmake --build cli/build-debug --target cdb` because the cli/ CMakeLists.txt (project `cdb`) is standalone — same arrangement Plan 125-04 used when building cli_tests (cli/build-debug)."

patterns-established:
  - "CLI-side NodeInfoResponse decoder mirrors the node's encoder field-for-field, in the same order, using the same width (u64 vs u32). Any future wire extension SHOULD follow this pattern: encoder grows at db/peer/message_dispatcher.cpp, decoder grows at cli/src/commands.cpp info() in lockstep."

requirements-completed: [NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04]

# Metrics
duration: 10min
completed: 2026-04-22
---

# Phase 127 Plan 03: CLI NodeInfoResponse Decoder Summary

**`cdb info` now decodes and renders the 4 new NodeInfoResponse capability fields shipped by Plan 127-01 — Max blob (u64, humanized), Max frame (u32, humanized), Rate limit (u64, humanized with `/s` or `unlimited`), Max subs (u32, plain integer or `unlimited`) — printed in that order after the existing Quota line. The stale `git_hash` layout comment is gone (D-12 passing fix).**

## Performance

- **Duration:** ~10 min (edit + CMake configure + targeted build). CMake configuration of the standalone cli/ project took ~3.5 min one-time in this fresh worktree (liboqs + flatbuffers + asio FetchContent).
- **Started:** 2026-04-22T11:39:38Z
- **Completed:** 2026-04-22T11:49:51Z
- **Tasks:** 1 / 1
- **Files modified:** 1 (cli/src/commands.cpp)

## Accomplishments

- `cli/src/commands.cpp` info() function extended with the 4 new reads and 4 new printf lines between `storage_max` handling and the `return 0` tail — all three edit positions (Edit A: comment at 2233-2237; Edit B: reads at 2278; Edit C: printf lines at 2294) landed atomically in one commit.
- Stale `[git_hash_len:1][git_hash_str]` references (pre-existing dead comment from an early NodeInfo draft that never matched node-side encoder reality — see Plan 127-01 SUMMARY confirming `grep -c 'git_hash' db/peer/message_dispatcher.cpp == 0`) fully removed from the CLI. `grep -c 'git_hash' cli/src/commands.cpp == 0` post-edit.
- Layout comment now documents the full post-Phase-127 wire layout in 11 lines (one line per fixed-width field + the variable-length types_count tail).
- Existing lambdas `read_u8` / `read_u32` / `read_u64` and the file-local `humanize_bytes` / `humanize_uptime` helpers reused — no new `#include` added, no duplicate byte-reader, no hand-rolled humanizer (feedback_no_duplicate_code.md respected).
- No user-facing string contains `Phase`, `127`, `NODEINFO`, `D-XX`, `v4.2.0`, `rate_limit_bytes_per_sec`, `MAX_FRAME`, or `framing.h` tokens — all user-visible labels are short human-readable strings (`Max blob:`, `Max frame:`, `Rate limit:`, `Max subs:`), observing feedback_no_phase_leaks_in_user_strings.md.
- `cdb` executable built clean via `cmake --build cli/build-debug -j$(nproc) --target cdb` (exit 0, 20.6 MB Debug binary linked at cli/build-debug/cdb).
- No touch to: `cli/src/wire.h` (MsgType::NodeInfoResponse=40 unchanged), `cli/src/main.cpp`, `cli/src/chunked.*`, `cli/src/connection.*`, or any other CLI source — exactly one file in the diff (`cli/src/commands.cpp`).
- No touch to: `db/` at all, `.planning/STATE.md`, `.planning/ROADMAP.md`, or any other shared file (per worktree executor rule).

## Task Commits

1. **Task 1: Rewrite stale layout comment + add 4 new read_*() calls + 4 new printf lines in cdb info** — `376055f7` (feat)

## Files Created/Modified

- `cli/src/commands.cpp` — +27 / -5 lines. Three contiguous edits inside the `info()` function (lines 2188-2308 post-plan):
  1. Lines 2233-2243 (was 2233-2237): 5-line stale comment replaced with an 11-line comment documenting the full post-Phase-127 wire layout.
  2. Line 2278 (after the pre-existing `auto storage_max = read_u64();`): 4 new `read_*()` calls (`auto max_blob_data_bytes = read_u64();` + `auto max_frame_bytes = read_u32();` + `auto rate_limit_bytes_per_sec = read_u64();` + `auto max_subscriptions = read_u32();`).
  3. Lines 2295-2306 (after the pre-existing `if (storage_max == 0) { ... } else { ... }` block): 4 new `std::printf` statements including the two zero-value branches for rate limit and subs.

## Decisions Made

None beyond the plan's codified decisions (D-02 field order, D-06 render order, D-07 zero-value handling, D-08 no session cache, D-11 no CLI-level Catch2 test, D-12 stale-comment rewrite). Plan executed verbatim.

One infrastructure note (not a plan-level decision, just a build-path clarification): The acceptance criteria AC10 wording `cmake --build build-debug -j$(nproc) --target cdb` references the cli/ standalone CMake project, which lives at `cli/build-debug` rather than the root-level `build-debug` (that root build tree only contains `chromatindb` + `chromatindb_lib` + `chromatindb_tests`; the `cli/` tree is a separate top-level CMake project with `project(cdb ...)`). The build was run as `cmake --build cli/build-debug -j$(nproc) --target cdb` (exit 0). This matches the historical pattern established by e.g. Phase 125-04's cli_tests build and is consistent with how `cdb` is invoked everywhere else in the repo.

## Deviations from Plan

None — plan executed exactly as written. All 10 acceptance criteria pass:

- **AC1** `grep -c 'git_hash' cli/src/commands.cpp` == 0 — PASS
- **AC2** All 4 new field comment lines present with expected `:N BE` width suffixes — PASS (4 × grep ==1)
- **AC3** All 4 new `read_*()` assignment statements present — PASS (4 × grep ==1)
- **AC4** All 6 expected printf label forms present (2 for rate: `unlimited` + `%s/s`; 2 for subs: `unlimited` + `%u`; 1 each for Max blob + Max frame) — PASS (6 × grep -F wc -l == 1)
- **AC5** `grep -En 'std::printf\(.*"(Phase|127|NODEINFO|D-[0-9]|v4\.2\.0|rate_limit_bytes_per_sec|MAX_FRAME|framing\.h)' cli/src/commands.cpp` — zero matching lines — PASS
- **AC6** Zero-value branches present for both rate and subs, with corresponding `unlimited` strings — PASS (4 × grep ==1)
- **AC7** Field-read order strictly increasing: blob@2279 < frame@2280 < rate@2281 < subs@2282 — PASS
- **AC8** `git diff cli/src/commands.cpp | grep -c '^+#include'` == 0 — PASS (no new includes)
- **AC9** `git diff --stat` shows only `cli/src/commands.cpp` modified — PASS (1 file changed, 27 insertions(+), 5 deletions(-))
- **AC10** `cmake --build cli/build-debug -j$(nproc) --target cdb` exit 0 — PASS (binary linked at cli/build-debug/cdb, 20.6 MB Debug)

## Issues Encountered

None on the code side. Build infrastructure note: the worktree had no pre-existing `cli/build-debug`, so a one-time CMake configure (~3.5 min for FetchContent of liboqs, flatbuffers, asio, spdlog, json, Catch2) was required before the targeted build could run. The actual incremental compile of `commands.cpp` + link of `cdb` was fast. No warnings emitted on `commands.cpp` by gcc's Debug flags.

## Threat Model Compliance

Plan PLAN.md `<threat_model>` assigns `mitigate` dispositions to:
- **T-127-05 (Tampering, CLI decoder bounds-check):** Mitigated. The 4 new reads all use the pre-existing `read_u32` / `read_u64` lambdas at cli/src/commands.cpp:2257-2268, which bounds-check against `p.size()` and throw `std::runtime_error("NodeInfoResponse truncated")` on short payload. No raw pointer arithmetic introduced.
- **T-127-06 (Information Disclosure via format string):** Mitigated. All 4 new `std::printf` calls use static format-string literals with explicit `%s` / `%u` conversion specifiers. Decoded values fed in are: `humanize_bytes(...).c_str()` (a C string from a static-lifetime helper) and `max_subscriptions` (uint32_t). No user-controlled format string path exists.
- **T-127-07 (DoS via malformed response):** Mitigated. Inherited from the existing lambdas — a short/truncated NodeInfoResponse propagates `std::runtime_error` up the call stack, `cdb` exits non-zero with a visible error. No infinite loop, no memory exhaustion, no new failure mode introduced by this plan.

Additional threat surface scan (threat_flags): No new network endpoints, auth paths, file access patterns, or trust-boundary schema changes are introduced. The decoder strictly CONSUMES bytes already produced by the node encoder landed in Plan 127-01. No `threat_flag:` entries to report.

## Known Stubs

None. The decoded values are rendered directly via `std::printf`. No empty/placeholder data path, no "coming soon" text, no TODO markers in the added lines. The plan explicitly defers session caching of these values to Phase 130 (D-08) — that is a scope decision, not a stub: the 4 values are USED (printed to stdout) as soon as they are decoded, satisfying Phase 127's deliverable completely.

## Next Phase Readiness

- **Plan 127-04** (Catch2 integration tests, Wave 2 parallel with this plan) has a stable CLI decoder to exercise transitively. Any byte-offset regression in the encoder OR decoder surfaces as a decode failure in 127-04's test. No blocker introduced.
- **Phase 130** (CLI auto-tuning) can now consume `max_blob_data_bytes` / `max_frame_bytes` by extending this decoder to POPULATE a `ConnectOpts` / `Session` cache instead of (or in addition to) printing. The Phase 130 delta will be one additional line per field after each `read_*()` call, keeping the print path intact.
- **Phase 131** (documentation reconciliation) owns PROTOCOL.md refresh for the NodeInfoResponse wire table (D-13). No CLI-side doc changes landed in Phase 127 per plan scope.
- No blockers for downstream plans. Protocol-breaking posture (D-15) means a pre-v4.2.0 `cdb` talking to a v4.2.0 node will fail `cdb info` decode at the types_count position — expected per `feedback_no_backward_compat.md`.

## Self-Check: PASSED

- File `cli/src/commands.cpp` — FOUND (modified; diff stat `1 file changed, 27 insertions(+), 5 deletions(-)`; key markers verified post-edit: Edit A comment at lines 2233-2243, Edit B reads at lines 2278-2282, Edit C printf block at lines 2295-2306).
- Commit `376055f7` — FOUND (`git log --oneline | grep 376055f7` returns `376055f7 feat(127-03): extend cdb info decoder with 4 new NodeInfoResponse caps`).
- Acceptance criteria AC1-AC10 — all pass (per Deviations section above).
- Build acceptance `cmake --build cli/build-debug -j$(nproc) --target cdb` — exit 0, `cdb` executable linked (20.6 MB, 2026-04-22 14:49 local).
- SUMMARY.md (this file) — will be committed via the final metadata commit immediately below.

---
*Phase: 127-nodeinforesponse-capability-extensions*
*Plan: 127-03-cli-decoder*
*Completed: 2026-04-22*
