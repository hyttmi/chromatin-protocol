---
phase: 120-request-pipelining
plan: 02
subsystem: cli-commands
tags: [cdb, cli, pipelining, commands, get, put]

# Dependency graph
requires:
  - phase: 120-request-pipelining
    plan: 01
    provides: Connection::send_async, Connection::recv_for, pipeline_pump.h, pending_replies_
provides:
  - Pipelined cmd::put (two-phase greedy-fill + arrival-order drain)
  - Pipelined cmd::get (same shape, ReadRequest/ReadResponse)
  - Connection::kPipelineDepth public constexpr (promoted from file-static)
affects:
  - Phase 119 chunked large files (PIPE-01 is now the mechanism chunked
    downloads will use for near-wire-speed per-chunk pipelining)
  - Any future CLI batch command that wants to fan out over a single PQ
    connection (delegate @group, etc.)

# Tech tracking
tech-stack:
  added: []  # no new libraries; stdlib <unordered_map> only
  patterns:
    - "Two-phase pipelined batch drive: greedy-fill send_async window, drain replies via recv() in arrival order (D-08 completion order, not request order)"
    - "Batch-local std::unordered_map<uint32_t, size_t> rid_to_index map bounded by kPipelineDepth; no eviction policy"
    - "Shared kPipelineDepth on the connection class — single source of truth between send_async backpressure and batch helper window size"

key-files:
  created: []
  modified:
    - cli/src/commands.cpp
    - cli/src/connection.h
    - cli/src/connection.cpp

key-decisions:
  - "Picked plan option-1: exposed Connection::kPipelineDepth = 8 as a public constexpr, removed the file-static PIPELINE_DEPTH from connection.cpp. Rationale: the plan explicitly recommended option-1, and per project memory (no copy-pasted utilities) the magic 8 should not be duplicated between the backpressure pump and the batch correlation map. Commit 4868f404 as part of Task 1."
  - "recv() (not recv_for) for the arrival-order drain — matches D-08 (completion order, not request order); recv_for would serialize on a specific rid and defeat the pipelining."
  - "Per-pending-request transport-dead error reporting (one stderr line per still-pending rid on connection loss) rather than a single generic message. Matches the existing per-item error culture and gives the user actionable diagnostics."
  - "Added try/catch around parse_hash() in cmd::get. Pre-refactor, a bad hex would throw std::runtime_error and unwind the whole batch. Rule 2 fix: the plan's must-haves require 'one bad input does not sink the batch'."

requirements-completed:
  - PIPE-01  # multi-blob downloads pipelined over a single PQ connection (verified against live node)

# Metrics
duration: 8min  # code + build + tests
completed: 2026-04-19
checkpoint_status: approved  # Task 3 human-verify passed against 192.168.1.73
---

# Phase 120 Plan 02: cmd::get / cmd::put Pipelining Summary

**cmd::put and cmd::get now drive Connection::send_async with a batch-local rid_to_index map for arrival-order draining via recv(). Task 3 human-verify passed against live node 192.168.1.73 on 2026-04-19 — pipelined 8-blob get is 5.4×–8.4× faster than sequential, byte-identical outputs, failure-path and single-blob regressions clean.**

## Status

- **Tasks 1, 2, 3: complete.** Build clean, full cli_tests suite green (49/49, including the 8 [pipeline] tests from 120-01), no regressions. Live-node verification passed all seven checkpoint steps.

## Performance

- **Duration (code + build + tests):** ~8 min
- **Started:** 2026-04-18T15:29:06Z
- **Reached checkpoint:** 2026-04-18T15:37:10Z
- **Tasks fully committed:** 2 of 3 (Task 3 is human-verify)
- **Files modified:** 3
- **Files created:** 0

## Accomplishments

### Task 1 — cmd::put pipelined (commit `4868f404`)

- Exposed `Connection::kPipelineDepth = 8` as a public `static constexpr` (PIPE-03 / D-07) on the `Connection` class.
- Removed the file-static `PIPELINE_DEPTH` from `connection.cpp`; `send_async` now references `Connection::kPipelineDepth`. One definition shared by the backpressure pump and the batch helpers.
- Refactored `cmd::put`'s per-file loop into the two-phase pipelined shape:
  - **Phase A:** greedy-fill `send_async` up to `kPipelineDepth` in-flight `Data` writes; `send_async`'s internal recv-pump handles backpressure when `in_flight_ == kPipelineDepth`.
  - **Phase B:** `conn.recv()` drains one `WriteAck` at a time in arrival order (D-08), looks up which file via a batch-local `std::unordered_map<uint32_t, size_t> rid_to_index`.
- Transport-dead branch reports one stderr error per still-pending file so the user sees exactly which uploads are uncertain.
- Added `#include <unordered_map>` once at file top.
- Preserved verbatim: every existing stderr string, `errors` counter, `errors > 0 ? 1 : 0` exit code, `conn.close()`, multi-file label format (`hash  filename` when `files.size() > 1`). `opts.quiet` now suppresses the single-file hash line as well — previously the single-file fallback ignored `opts.quiet` (this matches the plan's explicit snippet and must-haves).

### Task 2 — cmd::get pipelined (commit `4af3ad44`)

- Refactored `cmd::get`'s per-hash loop into the same two-phase shape as `cmd::put`, but driving `ReadRequest`/`ReadResponse`.
- Wrapped `parse_hash()` in try/catch: pre-refactor, a bad hex would throw `std::runtime_error` out of the whole batch. The plan's must-haves require "one bad input does not sink the batch"; this is a Rule 2 correctness fix applied during the refactor.
- Preserved verbatim: every stderr string (`Error: failed to send ReadRequest`, `Error: bad response`, `Error: blob not found`, `%s: failed to decode blob`, `%s: cannot decrypt (not a recipient)`, `Error: %s already exists (use --force to overwrite)`, `Error: cannot write to %s`, `saved: %s`), the `to_stdout` / `output_dir` / `force_overwrite` branches, envelope-decrypt fallback (`envelope::is_envelope`), `parse_put_payload` error surface, and `opts.quiet` gating of the "saved:" info line.

## Final shape of the refactored functions

```cpp
// Both cmd::put and cmd::get share this shape (adapted for their respective
// request/response types and per-item bodies):

uint32_t rid = 1;
std::unordered_map<uint32_t, size_t> rid_to_index;  // batch-local
size_t next_to_send = 0;
size_t completed = 0;

while (completed < N) {
    // Phase A: greedy fill the window.
    if (next_to_send < N && rid_to_index.size() < Connection::kPipelineDepth) {
        // ... build payload ...
        uint32_t this_rid = rid++;
        if (!conn.send_async(TYPE, payload, this_rid)) {
            // per-item error path unchanged
            ++errors; ++completed; ++next_to_send;
            continue;
        }
        rid_to_index[this_rid] = next_to_send;
        ++next_to_send;
        continue;
    }

    // Phase B: drain one reply in arrival order (D-08).
    auto resp = conn.recv();
    if (!resp) {
        // transport dead: one error per still-pending request
        for (auto& [_, idx] : rid_to_index) { ++errors; ++completed; /* log */ }
        rid_to_index.clear();
        break;
    }
    auto it = rid_to_index.find(resp->request_id);
    if (it == rid_to_index.end()) {
        spdlog::debug(... unknown rid ...);
        continue;
    }
    size_t idx = it->second;
    rid_to_index.erase(it);
    ++completed;
    // ... existing per-item response validation + side effects ...
}
```

## Task Commits

| Task | Type | Commit | Summary |
|------|------|--------|---------|
| 1 | feat | `4868f404` | Pipeline cmd::put + expose Connection::kPipelineDepth |
| 2 | feat | `4af3ad44` | Pipeline cmd::get + parse_hash try/catch (Rule 2 batch-isolation fix) |
| 3 | checkpoint:human-verify (blocking) | — | **pending** — user verifies speedup + correctness against 192.168.1.73 |

## Verification of Other Commands (Unchanged)

Grep confirms zero changes outside the two refactored functions:

```
$ git diff 3e9c9dce..HEAD -- cli/src/commands.cpp | grep '^+' | grep -E '^\+int (ls|stats|info|exists|rm|reshare|publish|contact_|delegations|revoke)'
(no matches)
```

The other CLI commands continue to use `Connection::send` / `Connection::recv` directly:
`cmd::ls`, `cmd::stats`, `cmd::info`, `cmd::exists`, `cmd::rm`, `cmd::reshare`, `cmd::publish`, `cmd::contact_*`, `cmd::delegations`, `cmd::revoke`. They remain sequential and behaviorally identical — pipelining is scoped strictly to `put` and `get` per D-05.

## Automated Verification Results

- **Build — cdb binary:** `cmake --build build --target cdb -j$(nproc)` → exit 0. No new warnings introduced by this plan (the pre-existing `-Wfree-nonheap-object` warning in `encode_auth_payload` at `connection.cpp:63`, already flagged by 120-01, is unchanged).
- **Build — cli_tests:** `cmake --build build --target cli_tests -j$(nproc)` → exit 0.
- **Full cli_tests suite:** `ctest --output-on-failure` → **49/49 passed**, 0 failed. The 8 `[pipeline]` tests from 120-01 still pass; the 41 baseline tests (contacts, envelope, identity, wire) still pass.

## Acceptance-Criteria Grep Audit

| Criterion | Result |
|-----------|--------|
| `grep -n "conn\.send_async(MsgType::Data"` in cmd::put | matches line 585 ✓ |
| `grep -n "conn\.send_async(MsgType::ReadRequest"` in cmd::get | matches line 698 ✓ |
| `grep -c "rid_to_index"` in commands.cpp | 18 (put + get) ✓ |
| `grep -n "completion order\|D-08"` in commands.cpp | 3 matches in put, 1 in get ✓ |
| `grep -n "for (auto& f : files)"` in commands.cpp | no match (old loop replaced) ✓ |
| `grep -n "for (const auto& hash_hex : hash_hexes)"` in commands.cpp | no match (old loop replaced) ✓ |
| `grep -c "errors > 0 ? 1 : 0"` in commands.cpp | 4 (all exit-code paths preserved) ✓ |
| `grep -n "files.size() > 1"` in commands.cpp | 1 match (multi-file label branch preserved) ✓ |
| `grep -n "opts.quiet"` in commands.cpp | 10 matches, including cmd::put refactor ✓ |
| `grep -n "kPipelineDepth"` in connection.h | 1 match (`static constexpr size_t kPipelineDepth = 8;`) ✓ |

## Option 1 vs Option 2 on kPipelineDepth

**Picked: Option 1 — expose `Connection::kPipelineDepth` publicly.**

**Why:** the plan explicitly recommended option-1, and two structural reasons reinforced that:

1. **No magic-number duplication** — `send_async` and `cmd::put`/`cmd::get` both need the same depth. A file-static `PIPELINE_DEPTH` in `connection.cpp` would force commands.cpp to hard-code the literal `8` or duplicate the constant. Project memory: utilities go into shared headers, not inline — this is the same principle applied to a constant.
2. **Public intent** — `kPipelineDepth` is API-visible: callers need to know the upper bound on in-flight requests to size their correlation maps. Hiding it is API-hostile.

The cost was minimal: one new line in `connection.h` (the `static constexpr` declaration), two edits in `connection.cpp` (delete the file-static, update the `while (in_flight_ >= …)` condition to `Connection::kPipelineDepth`).

## Wall-Clock Numbers for Task 3 — PASSED

Measured against live node `192.168.1.73` on 2026-04-19 with 8 × 64 KiB freshly uploaded blobs:

| Scenario | Binary | Wall-clock (`real`) |
|----------|--------|---------------------|
| Sequential baseline, 8-blob `cdb get` (run 1) | commit `3e9c9dce` | **0.662 s** |
| Sequential baseline, 8-blob `cdb get` (run 2) | commit `3e9c9dce` | **1.026 s** |
| Pipelined 8-blob `cdb get` (run 1) | HEAD (`3e962d54`) | **0.122 s** |
| Pipelined 8-blob `cdb get` (run 2) | HEAD (`3e962d54`) | **0.122 s** |
| Pipelined single-blob `cdb get` | HEAD | **0.091 s** |

**Speedup:** pipelined wall-clock is ~18% of sequential (5.4×–8.4× faster) — well under the 50% threshold. Pipelined run variance was nil (both 0.122 s); sequential variance is node-load-dependent. Low LAN RTT (node is on the same subnet) meant replies often returned before the next request was sent, so per-item "saved:" lines happened to arrive in request order on this run — this is not a correctness concern: the code still drains via arrival-order `recv()` + rid_to_index, it's just that completion order == request order when RTT ≈ 0.

**Correctness (`diff -r`):**
- `diff -r /tmp/seq /tmp/pipe` → **no differences** (byte-identical) — run 1
- `diff -r /tmp/seq2 /tmp/pipe2` → **no differences** (byte-identical) — run 2
- Both runs printed 8 "saved: …" lines, exit code 0.

**Failure-path sanity (bad hash in batch of 8):**
- 7 good files saved ✓
- 1 stderr `Error: blob not found: deadbeef000…` line ✓
- Exit code **1** ✓
- Other blobs' successes were not affected.

**Single-blob regression (depth-1 path):**
- `cdb get <one_hash>` → 91 ms, exit 0, file saved ✓

**Conclusion:** all seven checkpoint steps passed. PIPE-01 delivered.

## Deviations from Plan

### Rule 2 auto-fix — parse_hash() try/catch in cmd::get

**Found during:** Task 2.
**Issue:** Pre-refactor, `parse_hash(hash_hex)` at the top of the per-hash loop body could throw `std::runtime_error` on a bad hex input, which the `for` loop did not catch — a single bad hash in a batch would unwind the whole `cmd::get` call before the `++errors; continue;` pattern had a chance to contain it. This violates the plan's must-have: `"one bad input does not sink the batch, exit code is 'errors > 0 ? 1 : 0'"`.
**Fix:** Wrapped the `parse_hash` call in try/catch inside Phase A, emitting `Error: invalid hash %s: %s\n` to stderr, `++errors; ++completed; ++next_to_send; continue;` — matching the existing per-item error culture. No other behavior change.
**Files modified:** `cli/src/commands.cpp` (cmd::get Phase A block).
**Commit:** `4af3ad44`.

### Documented intent — `opts.quiet` now honored in cmd::put single-file fallback

**Found during:** Task 1 (while transcribing the plan's suggested snippet).
**Issue:** The original cmd::put single-file success path printed the hash unconditionally via the `else` branch — it ignored `opts.quiet`. The plan's Task-1 `<action>` snippet corrects this with `else if (!opts.quiet)`, and the plan's must-haves explicitly require `opts.quiet still suppresses per-item info lines`.
**Fix:** Used `else if (!opts.quiet)` in the refactor, matching the plan snippet.
**Rationale:** This is a plan-directed minor behavior change, not strictly an auto-fix — the plan explicitly anticipated it. If the user disagrees at the checkpoint, it can be reverted trivially.
**Commit:** `4868f404`.

### Total deviations

- 1 Rule 2 auto-fix (parse_hash isolation).
- 1 plan-directed behavior change (opts.quiet in cmd::put single-file path).
- 0 blocking issues, 0 architectural changes.

## Checkpoint Task 3 — APPROVED

**Type:** `checkpoint:human-verify` (gate=blocking)
**Status:** Passed — 2026-04-19 against live node `192.168.1.73`

### Task summary

| Task | Name | Commit | Status |
|------|------|--------|--------|
| 1 | Pipeline cmd::put | `4868f404` | committed |
| 2 | Pipeline cmd::get | `4af3ad44` | committed |
| 3 | Human-verify (blocking) | — | APPROVED (see Wall-Clock Numbers section above) |

All seven checkpoint steps passed: 5.4×–8.4× pipelined speedup, byte-identical outputs via `diff -r` on two independent samples, failure-path with bad hash isolated correctly (7 saved + 1 error + exit 1), single-blob depth-1 path 91 ms / exit 0.

## Follow-ups for Phase 119 (Chunked Large Files)

The chunked download path in Phase 119 will reassemble a large blob from N chunks. Each chunk request is a separate `ReadRequest`/`ReadResponse` pair with its own `request_id`. With Task 1/2 of this plan landed, Phase 119 can:

- Use `Connection::send_async(MsgType::ReadRequest, chunk_payload, chunk_rid)` directly — no new primitives needed.
- Maintain its own `chunk_rid → chunk_index` map, identical in shape to `rid_to_index` here.
- Assemble chunks into output order after drain (since `recv()` yields them in arrival order, not request order — matches D-08 for single-blob chunked-reassembly with buffering).

No adapter layer required — the primitives extend to the chunked path naturally. Recommendation: Phase 119's executor reuses this plan's `while (completed < N) { Phase-A; Phase-B }` skeleton and substitutes the chunk-specific payload build / response handling.

## Known Stubs

None. Both cmd::put and cmd::get are fully wired to the pipelined primitives.

## Threat Flags

None. The changes route replies through existing AEAD-protected transport and use a locally-generated monotonic `rid` — no new network endpoints, no new trust boundaries, no schema changes. The mitigations from the plan's `<threat_model>` (T-120-08 through T-120-13) are satisfied by construction:

- T-120-08 (DoS via batch size): `rid_to_index.size() < Connection::kPipelineDepth` caps in-flight at 8 regardless of batch size.
- T-120-09 (rid spoofing): rid is locally generated (`uint32_t rid = 1; rid++`), server echoes only; unknown-rid replies hit the spdlog::debug + drop branch.
- T-120-10 (reply↔filename mix-up): `file_idx = rid_to_index[resp->request_id]` — filename always matches the rid the server replied to. Verifiable at the Task 3 checkpoint via `diff -r`.
- T-120-11 (arrival-order ≠ request-order): accepted per D-08, documented in code at both call sites.
- T-120-12 (send_async hang on withheld replies): residual risk, backlog 999.8 (unchanged from Plan 01).
- T-120-13 (mid-batch transport death): explicit per-pending error reporting; user sees exactly which requests are uncertain, exit code non-zero.

## Self-Check: PASSED

Modified files verified on disk:

- `cli/src/commands.cpp` — FOUND (put + get refactored, `#include <unordered_map>` added)
- `cli/src/connection.h` — FOUND (added `static constexpr size_t kPipelineDepth = 8;`)
- `cli/src/connection.cpp` — FOUND (removed file-static `PIPELINE_DEPTH`, `send_async` uses `Connection::kPipelineDepth`)

Commits verified in git log (run `git log --oneline -3`):

- `4868f404` — Task 1 feat (cmd::put + kPipelineDepth) — FOUND
- `4af3ad44` — Task 2 feat (cmd::get + parse_hash try/catch) — FOUND

Verification commands executed successfully (exit code 0):

- `cmake --build build --target cdb -j$(nproc)` — PASS
- `cmake --build build --target cli_tests -j$(nproc)` — PASS
- `ctest --output-on-failure` — PASS (49/49)

Grep acceptance audit — all criteria met (see table above).

---
*Phase: 120-request-pipelining*
*Plan: 02*
*Reached checkpoint: 2026-04-18 — Task 3 (human-verify) pending*
