---
phase: 119-chunked-large-files
slug: chunked-large-files
date: 2026-04-19
source: gap-closure replan (plan 119-03)
---

# Phase 119 Validation Architecture — Plan 119-03 Gap Closure Scope

This document captures the validation architecture for the 119-03 gap-closure plan
(CR-01 `in_flight_` leak, WR-02 backoff index, WR-03 fd/file leak on exception,
WR-04 single-blob-batch leak, IN-03 silent config catch). It complements the
broader Phase-119 validation architecture in
`119-RESEARCH.md § Validation Architecture` (line 927); the generic test map there
covers CHUNK-01..05. This doc covers the **correctness gap** layered on top of
those, as discovered in 119-REVIEW.md and 119-VERIFICATION.md.

## Validation Layers

### Unit Layer (fast, deterministic, sub-millisecond per case)

Host file: `cli/tests/test_connection_pipelining.cpp` (co-located with the existing
`pump_recv_for` tests; reuses the shared `ScriptedSource` / `make_reply` fixture
that, per plan task 11, has been extracted to `cli/tests/pipeline_test_support.h`).

| Case | Covers | Assertion |
|------|--------|-----------|
| pump_recv_any — pending empty + source returns | CR-01 decrement path | `in_flight` drops by 1; `src.call_count == 1` |
| pump_recv_any — pending non-empty drains first | CR-01 fast path (stashed replies) | `in_flight` drops by 1; `src.call_count == 0` |
| pump_recv_any — source nullopt with empty pending | CR-01 error semantic | `in_flight` unchanged |
| pump_recv_any — in_flight=0 edge | T-119-GAP-02 (underflow defense) | `in_flight` stays 0, no wrap to SIZE_MAX |

Each case runs in <1 ms. Sampling continuity (Nyquist) is preserved because the
per-task automated verify (`cmake --build ... && cli_tests`) runs every time a
task lands; 49+4 = 53 ms per-task sample is well above the Phase-119 per-commit
sampling primitive.

### Integration Layer (Catch2, in-process fake transport)

Host file: `cli/tests/test_chunked.cpp` (host of the existing [chunked] suite).
Uses the same shared fixture via `#include "cli/tests/pipeline_test_support.h"`
(no fixture duplication — see `feedback_no_duplicate_code.md`).

| Case | Covers | Assertion |
|------|--------|-----------|
| CR-01 regression — 8 sends + 8 recv_next drains | CR-01 at the exact shape that hung on the live node | `in_flight == 0` after 8 drains; 9th on empty source returns nullopt and leaves `in_flight == 0` |
| CR-01 regression — pending non-empty fast path | CR-01 + backpressure-stash handoff | `in_flight` drops via fast path; `src.call_count == 0`; `pending` empty |

### Compilation Sentinel (every implementation task)

Every task under 119-03 has
`<automated>cmake --build build/cli -j$(nproc) && build/cli/tests/cli_tests "[…]"</automated>`
as its `<verify>` body. A compile failure OR a test regression breaks the task
immediately. Incremental build + focused suite completes in <30 s on a warm
cache — this is the Nyquist sampling primitive for the phase (per-commit).

### System Layer (live-node E2E, human-verify gate)

Task 14 — blocking `checkpoint:human-verify` against BOTH
192.168.1.73 (`--node home`) and 127.0.0.1 (`--node local`). Scope:

- 420 MiB `cdb put` completes without hang (CR-01 observable signature: the
  46-minute hang on 8 chunks no longer reproduces).
- `cdb get <manifest_hash>` reassembles byte-identical (`diff` exits 0).
- `cdb rm <manifest_hash>` tombstones every chunk + manifest with zero orphans
  (`cdb ls --raw --type CDAT | wc -l` returns the expected count).

## Nyquist Sampling Frequency

RESEARCH.md § Validation Architecture does NOT specify a dedicated Nyquist
frequency constant. The sampling primitive for 119-03 is:

- **Per task (most frequent):** automated verify command runs `cli_tests` in
  <30 s. Every commit triggers it.
- **Per phase (once):** live-node checkpoint.
- **Cross-phase (each future cli change):** full `cli_tests` regression.

Per-task sampling at <30 s is well above the worst-case regression signal
frequency (any bug introduced lands within one task commit and surfaces within
one task verify cycle). Continuity is preserved.

## Coverage Matrix

| Issue | Unit | Integration | Live | Task Ref |
|-------|------|-------------|------|----------|
| CR-01 (in_flight_ counter leak) | 4 pump_recv_any cases | 2 [chunked] regression cases | 420 MiB put/get/rm | Tasks 1, 3, 4, 5, 12, 14 |
| WR-02 (retry backoff index) | covered by existing [chunked] retry tests (if present); otherwise regression test in Task 8's acceptance — a fresh call path exercising `RETRY_BACKOFF_MS[0]=250ms` as the first retry | — | part of live-node checkpoint (indirectly — retries may fire on 192.168.1.73 under load) | Task 8 |
| WR-03 (fd / file leak on exception) | — | existing "sha3 mismatch unlinks" test passes via guard (Task 9 acceptance); new exception-injection test optional (not required — the existing test proves the guard path) | live-node `cdb get` clean-exit path | Task 9 |
| WR-04 (cmd::put / cmd::get leak) | covered by pump_recv_any unit tests (same code path) | same 2 regression cases (both sites call pump_recv_any through `Connection::recv_next`) | 10 MiB small-file regression in checkpoint | Tasks 6, 7 |
| IN-03 (silent config catch) | — | — | manual smoke test in Task 10 acceptance criteria (writing `not-valid-json` into config) | Task 10 |

## Cross-Reference

- `119-RESEARCH.md § Validation Architecture` (line 927) — generic CHUNK-01..05 test map.
- `119-REVIEW.md § CR-01` — authoritative source of the CR-01 bug description.
- `119-VERIFICATION.md § gaps` — the 5th must-have truth that 119-03 closes.
- `119-03-PLAN.md § tasks` — the 14-task plan this doc validates.
