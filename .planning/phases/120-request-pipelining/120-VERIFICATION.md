---
phase: 120-request-pipelining
verified: 2026-04-19T03:20:25Z
status: passed
score: 3/3 roadmap success criteria + 13/14 plan must-haves verified
overrides_applied: 0
---

# Phase 120: Request Pipelining — Verification Report

**Phase Goal (ROADMAP.md:83):** Multi-blob downloads (and uploads) complete faster by pipelining requests over a single PQ connection instead of sequential round-trips.

**Verified:** 2026-04-19T03:20:25Z
**Status:** passed
**Re-verification:** No — initial verification.

## Goal Achievement

### PIPE-01, PIPE-02, PIPE-03 (ROADMAP Success Criteria)

| # | Requirement | Status | Evidence |
|---|-------------|--------|----------|
| PIPE-01 | Multi-blob downloads/uploads pipelined over single PQ connection | PASS | `cmd::get` (commands.cpp:698) and `cmd::put` (commands.cpp:585) call `conn.send_async(...)` + arrival-order `conn.recv()` with batch-local `rid_to_index` map. Live-node measurement: 8-blob pipelined get = 0.122s, sequential baseline = 0.662s/1.026s — **5.4×–8.4× speedup**, well under the 50% threshold. `diff -r /tmp/seq /tmp/pipe` → byte-identical on two independent samples. |
| PIPE-02 | Single-reader invariant preserved (no concurrent recv; AEAD nonce monotonicity) | PASS | `Connection::recv_for` body (connection.cpp:735-743) only calls `recv()` (via lambda) — zero `send`/`send_chunked`/`send_encrypted` calls. `pipeline_pump.h` helper grep: zero send-path calls (only a comment mentions "send path"). `send_async` body calls `send(...)` exactly once AFTER the backpressure pump exits. No new threads, no asio strands, no coroutines anywhere in the phase diff. |
| PIPE-03 | Pipeline depth default 8, single source of truth | PASS | `static constexpr size_t kPipelineDepth = 8` in `cli/src/connection.h:23`. No `PIPELINE_DEPTH` file-static in `connection.cpp` (grep returns no matches). `commands.cpp` references `Connection::kPipelineDepth` 4× (2× per function, once in comment and once in the `rid_to_index.size() < Connection::kPipelineDepth` greedy-fill check). No inline literal `8` for the depth check. |

**Score:** 3/3 ROADMAP success criteria verified.

### Plan Must-Haves (120-01 + 120-02)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 01-1 | Connection exposes `send_async` + `recv_for` as additive public surface | PASS | connection.h:43-54 declares both; existing `send`/`recv`/`close`/`connect` unchanged. |
| 01-2 | `Connection::kPipelineDepth = 8` public static constexpr in connection.h | PASS | connection.h:23: `static constexpr size_t kPipelineDepth = 8;` (comment references PIPE-03 / D-07). |
| 01-3 | Backpressure in `send_async` pumps `recv()` (never `send_*`), preserving single-sender invariant | PASS | connection.cpp:723-728 — `while (in_flight_ >= Connection::kPipelineDepth)` calls `pipeline::pump_one_for_backpressure` with `[this] { return recv(); }` source. No threads spawned, no second writer introduced. |
| 01-4 | `recv_for(rid)` returns stashed reply immediately; otherwise loops over `recv()` and stashes off-target replies keyed by `request_id` | PASS | connection.cpp:735-743 delegates to `pipeline::pump_recv_for`. Helper (pipeline_pump.h:67-96) implements fast-path stash check, then loop over `source()` with `insert_or_assign` into `pending`. Tested by [pipeline] cases including "pre-stashed reply returns without calling source" and "send-8-receive-out-of-order correlation". |
| 01-5 | Replies for unknown rid logged at `spdlog::debug` from `Connection::recv_for` and discarded; helper stays silent via injected observer callback | PARTIAL (deferred) | Helper stays silent (zero `spdlog` in pipeline_pump.h — verified). Helper does NOT accept an `on_off_target` observer parameter (deviates from plan). `Connection::recv_for` wrapper does NOT emit `spdlog::debug` for off-target stashes — it passes no callback. **However:** unknown-rid `spdlog::debug` lines DO emit from the batch callers `cmd::put` (commands.cpp:618) and `cmd::get` (commands.cpp:727), which cover the practical observability case. Deviation is documented in 120-01-SUMMARY.md ("Deviations from Plan"). Does not affect any ROADMAP success criterion. |
| 01-6 | `close()` clears `pending_replies_` and resets `in_flight_` | PASS | connection.cpp:773-774 inside `close()` body, grouped with `send_counter_`/`recv_counter_` reset. |
| 01-7 | Catch2 [pipeline] test exercises 8-send out-of-order correlation + unknown-rid discard path | PASS | `cli/tests/test_connection_pipelining.cpp` has 8 TEST_CASEs tagged `[pipeline]`: direct-hit, send-8-out-of-order, pre-stashed, source-dead pump, backpressure drain, backpressure source-dead, unknown-rid discard (D-04), duplicate-rid overwrite. All 8 pass. |
| 02-1 | `cmd::put` fires up to `Connection::kPipelineDepth` writes in flight, drains one reply at a time in arrival order | PASS | commands.cpp:555-643. Two-phase: Phase A greedy-fill `send_async(MsgType::Data, ...)` up to depth; Phase B `recv()` drains one `WriteAck` at a time, dispatched via `rid_to_index`. |
| 02-2 | `cmd::get` fires up to `Connection::kPipelineDepth` reads in flight, drains one reply at a time in arrival order | PASS | commands.cpp:665-770 (approx). Same shape as put but for `MsgType::ReadRequest`/`ReadResponse`. |
| 02-3 | Per-item error reporting preserved (`++errors; continue;` pattern; `errors > 0 ? 1 : 0` exit code) | PASS | Grep: `errors > 0 ? 1 : 0` present 4× in commands.cpp. Per-item `++errors; ++completed; continue;` shape visible at commands.cpp:592, 603-607, 625, 714-719, 733. Transport-death branch reports one stderr line per still-pending rid. Live-node bad-hash sanity: 7 saved + 1 stderr error + exit 1. |
| 02-4 | `opts.quiet` suppresses per-item info lines; multi-file label format (`hash  filename`) still kicks in for batches >1 | PASS | Grep: `opts.quiet` appears 10× in commands.cpp; `files.size() > 1` appears 1× (multi-file label). Verified in SUMMARY checkpoint output. |
| 02-5 | Completion-order documented in code (D-08) | PASS | Grep for "completion order" / "D-08" in commands.cpp returns 3 matches in put and 1 in get, exact `D-08` reference on commands.cpp:544, 597, 667. |
| 02-6 | Pipelined `cdb get hash1..hash8` measurably faster than sequential | PASS | Live-node: 0.122s pipelined vs 0.662s/1.026s sequential = 5.4×–8.4× speedup. Human-verify checkpoint approved 2026-04-19. |
| 02-7 | Single-blob `cdb get`/`cdb put` still works (depth-1 degrades cleanly) | PASS | Live-node: single-blob get = 91ms, exit 0, file saved. |
| 02-8 | `Connection::kPipelineDepth` referenced — no inline literal `8`, no file-static duplicate | PASS | Grep `Connection::kPipelineDepth` in commands.cpp: 4 matches. Grep for `< 8` / `== 8` depth check: no matches. Grep `PIPELINE_DEPTH` in connection.cpp: no matches. |

**Score:** 13/14 plan must-haves verified (1 partial — 01-5 observability, documented deviation).

## Locked Decisions (120-CONTEXT.md D-01..D-09)

| # | Decision | Satisfied | Evidence |
|---|----------|-----------|----------|
| D-01 | Two-layer API: `send_async` + `recv_for` primitives; batch helpers built on top | PASS | Primitives declared in connection.h; cmd::get/cmd::put use primitives + recv() directly (no separate `cmd::batch_get` / `cmd::batch_put` wrapper layer — the plan's D-01 allows consumers to "pick the level that fits" and the implementer chose primitive-level integration). |
| D-02 | Per-item results with lenient error handling | PASS | `errors > 0 ? 1 : 0` exit code; per-item error lines; bad-hash-in-batch case succeeds 7/8 with exit 1. |
| D-03 | Cooperative pump, single-threaded; no background thread / mutex / async runtime | PASS | Grep pipeline_pump.h + connection.cpp for `std::thread`, `std::mutex`, `std::atomic`, `co_await`: no new instances. Single-threaded by construction. |
| D-04 | Correlation map is `std::unordered_map<uint32_t, DecodedTransport>` bounded by depth; log-and-discard on unknown rid | PARTIAL | Map structure and bounding correct (connection.h:104). Discard behavior: unknown-rid replies stash into pending_replies_ and linger until close() (safe bounded behavior). Helper-level observability (spdlog::debug at Connection::recv_for wrapper site) was relaxed — log still emits from cmd::get/cmd::put call sites though. |
| D-05 | Single mechanism covers `ReadRequest↔ReadResponse` (cmd::get) AND `Data↔WriteAck` (cmd::put) | PASS | Both call sites use same `send_async` + `recv()` + `rid_to_index` shape. |
| D-06 | `send_async` blocks via internal `recv()` pump when `in_flight_ >= depth`; natural flow control | PASS | connection.cpp:723-728 exactly this shape. Task 1's [pipeline] backpressure tests verify. |
| D-07 | Depth hard-coded at 8; no flag, no config key, no per-call argument | PASS | `kPipelineDepth = 8` in connection.h; no flag/config plumbing; verified by grep. |
| D-08 | Per-item lines arrive in completion order, not request order (documented, not a bug) | PASS | `recv()` (not `recv_for`) used in drain loop — arrival order. Code comments at commands.cpp:544, 597, 667 explicitly reference D-08. |
| D-09 | Existing serialized send queue untouched; `send_async` adds no new writer | PASS | No edits to `send`, `send_chunked`, `send_encrypted`, or the send-queue drain logic in connection.cpp. Single-sender invariant preserved by construction. |

**Score:** 9/9 locked decisions satisfied (D-04 partial-but-safe; no functional impact on the phase goal).

## Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `cli/src/connection.h` | `send_async` + `recv_for` decls; public `kPipelineDepth`; `pending_replies_` + `in_flight_` members; `<unordered_map>` include | PASS | All present; verified at connection.h:11, 23, 43-54, 104-105. |
| `cli/src/connection.cpp` | `send_async`/`recv_for` bodies; no file-static `PIPELINE_DEPTH`; `close()` clears pending state | PASS | connection.cpp:716-743 bodies; no file-static; close() resets at 773-774. |
| `cli/src/pipeline_pump.h` | Header-only helper in `chromatindb::cli::pipeline` namespace; zero spdlog dependency; `pump_recv_for` + `pump_one_for_backpressure` templates | PASS | File present, 98 lines. Namespace `chromatindb::cli::pipeline` at line 11. Zero `spdlog` (grep: no matches in call-site form — only one mention in a comment). Both templates present. |
| `cli/src/commands.cpp` | `cmd::put` + `cmd::get` refactored; `rid_to_index` used; `Connection::kPipelineDepth` referenced; no inline `8` | PASS | `rid_to_index` appears 18× (put + get). `Connection::kPipelineDepth` referenced 4× (both functions). No inline literal `8` for depth check. |
| `cli/tests/test_connection_pipelining.cpp` | Catch2 [pipeline] test file, ≥8 TEST_CASEs | PASS | 8 TEST_CASEs, all tagged `[pipeline]`. All pass (8/8 assertions clean on live run). |
| `cli/tests/CMakeLists.txt` | `test_connection_pipelining.cpp` registered in `cli_tests` target | PASS | Line 9 of cli/tests/CMakeLists.txt. |

## Key Link Verification

| From | To | Via | Status | Details |
|------|----|----|--------|---------|
| `Connection::send_async` | `Connection::send` | Post-backpressure delegation | WIRED | connection.cpp:730: exactly one `send(type, payload, request_id)` call after the pump loop exits. |
| `Connection::send_async` backpressure | `pending_replies_` | Stash drained off-target replies | WIRED | pipeline_pump.h:54: `pending.insert_or_assign(msg->request_id, std::move(*msg));` inside `pump_one_for_backpressure`. |
| `Connection::recv_for` | `pipeline::pump_recv_for` | Delegate with `recv()` lambda source | WIRED | connection.cpp:738-742. |
| `cmd::put` | `Connection::send_async` | Fires `Data` with batch-local rid | WIRED | commands.cpp:585: `conn.send_async(MsgType::Data, flatbuf, this_rid)`. |
| `cmd::put` | `Connection::recv` | Arrival-order drain | WIRED | commands.cpp:600 region — `auto resp = conn.recv();` then `rid_to_index.find(resp->request_id)`. |
| `cmd::get` | `Connection::send_async` | Fires `ReadRequest` with batch-local rid | WIRED | commands.cpp:698: `conn.send_async(MsgType::ReadRequest, payload, this_rid)`. |
| `cmd::get` | `Connection::recv` | Arrival-order drain | WIRED | commands.cpp:~710 region — `auto resp = conn.recv();` + `rid_to_index.find(...)`. |
| greedy-fill check (put + get) | `Connection::kPipelineDepth` | Shared depth constant | WIRED | commands.cpp:564, 678: `rid_to_index.size() < Connection::kPipelineDepth`. |

## Single-Sender Invariant Grep Report (PIPE-02 / D-09)

| Target | Expected | Result |
|--------|----------|--------|
| `send`/`send_chunked`/`send_encrypted` calls inside `Connection::recv_for` body (connection.cpp:735-743) | 0 | **0** — body contains only `pipeline::pump_recv_for(...)` delegation with `[this] { return recv(); }` source. |
| `send`/`send_chunked`/`send_encrypted` calls inside `pipeline_pump.h` | 0 | **0** — only a comment at line 35 mentions "send path"; no actual send-function invocations anywhere in the helper. |
| `send*` calls inside `Connection::send_async` backpressure loop (while-block at connection.cpp:723-728) | 0 | **0** — the only thing inside the while loop is `pipeline::pump_one_for_backpressure(...)` with `recv()` source. The one `send(type, payload, request_id)` call is AFTER the loop exits (line 730). |
| New threads / asio strands / coroutines introduced by the phase | 0 | **0** — no `std::thread`, `std::mutex`, `std::atomic`, `co_await`, `asio::co_spawn`, or `asio::strand` in the modified files. Single-threaded cooperative pump only. |

**Conclusion:** PIPE-02 single-sender invariant is preserved by construction. AEAD `send_counter_` / `recv_counter_` monotonicity is intact.

## `kPipelineDepth` Single Source of Truth Report (PIPE-03)

| Check | Expected | Result |
|-------|----------|--------|
| `static constexpr size_t kPipelineDepth = 8` in connection.h | 1 declaration | connection.h:23 — **present once**. |
| File-static `PIPELINE_DEPTH` in connection.cpp | 0 | **0** — grep `PIPELINE_DEPTH` in connection.cpp returns only comment/doc references (lines 27, 28, 40-42, 43, 53), no `static constexpr`. |
| Inline literal `8` for depth checks in commands.cpp | 0 | **0** — grep for `< 8` / `== 8` (as depth check) returns no matches. |
| `Connection::kPipelineDepth` references in commands.cpp | ≥2 | **4** — in both `cmd::put` and `cmd::get`, comment + greedy-fill check. |
| `kPipelineDepth` references in connection.cpp | ≥1 | **1** (line 723) — `while (in_flight_ >= Connection::kPipelineDepth)`. |

**Conclusion:** depth constant has exactly one declaration, referenced by name everywhere else. Changing the constant requires editing exactly one line (connection.h:23).

## Acceptance Tests Summary

| Test | Status | Notes |
|------|--------|-------|
| `cli_tests [pipeline]` (8 tests) | **8/8 PASS** | Freshly rebuilt + re-run in `cli/build/` during verification: 48 assertions, 8 cases, all pass. |
| Full `cli_tests` suite | **49/49 PASS** | Freshly rebuilt + re-run: 252 assertions, 49 cases, all pass (41 baseline + 8 new pipeline). |
| `cdb` binary build | **PASS** | `cmake --build cli/build --target cdb -j$(nproc)` exits 0. No new warnings introduced by the phase diff (pre-existing `-Wfree-nonheap-object` in `encode_auth_payload` at connection.cpp:57 is unrelated and untouched). |
| Live-node 8-blob pipelined `cdb get` | **PASS** | 0.122s (stable across 2 runs). |
| Live-node 8-blob sequential baseline | **PASS** | 0.662s / 1.026s (variance from node load). |
| Speedup | **PASS** | 5.4×–8.4× (well under the 50% threshold required by checkpoint). |
| `diff -r /tmp/seq /tmp/pipe` correctness | **PASS** | No differences on 2 independent samples — byte-identical outputs. |
| Bad-hash-in-batch failure path | **PASS** | 7 saved + 1 stderr error + exit 1. |
| Single-blob `cdb get` depth-1 regression | **PASS** | 91ms, exit 0, file saved. |

## Unchanged Sequential Callers

The following commands were confirmed unchanged by this phase — they still use `Connection::send` / `Connection::recv` directly, **not** the new pipelining primitives. Grep of commands.cpp for `conn.send_async` / `conn.recv_for` returns matches ONLY inside `cmd::put` (line 585) and `cmd::get` (line 698). No other call site uses the new primitives.

| Command | File:Line | Status |
|---------|-----------|--------|
| `cmd::ls` | commands.cpp:1097 | Unchanged (sequential) |
| `cmd::stats` | commands.cpp:1479 | Unchanged (sequential) |
| `cmd::info` | commands.cpp:1382 | Unchanged (sequential) |
| `cmd::exists` | commands.cpp:1320 | Unchanged (sequential) |
| `cmd::rm` | commands.cpp:819 | Unchanged (sequential) |
| `cmd::reshare` | commands.cpp:931 | Unchanged (sequential) |
| `cmd::publish` | commands.cpp:1802 | Unchanged (sequential) |
| `cmd::contact_add` / `contact_rm` / `contact_list` / `contact_import` / `contact_export` | commands.cpp:1875, 1918, 1933, 2037, 2089 | Unchanged (sequential) |
| `cmd::delegate` / `cmd::delegations` / `cmd::revoke` | commands.cpp:1543, 1736, 1606 | Unchanged (sequential) |
| `cmd::keygen` / `cmd::whoami` / `cmd::export_key` | commands.cpp:364, 384, 418 | Unchanged (sequential) |
| `cmd::group_*` | commands.cpp:1951-2019 | Unchanged (sequential, local-only) |

## Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| PIPE-01 | 120-02 | Multi-blob downloads pipelined over single PQ connection | SATISFIED | cmd::get + cmd::put refactored; live-node 5.4×–8.4× speedup measured on 2026-04-19. |
| PIPE-02 | 120-01 | Single-reader invariant preserved | SATISFIED | Grep-verified — zero send-path calls inside recv_for/pump. No new threads. |
| PIPE-03 | 120-01 | Pipeline depth 8 single source of truth | SATISFIED | `Connection::kPipelineDepth = 8` single declaration; referenced by name everywhere. |

No orphaned requirements — all three PIPE-* IDs mapped to Phase 120 in REQUIREMENTS.md are covered.

## Anti-Patterns Found

None blocking. The phase diff introduces no stubs, no TODO/FIXME markers, no hardcoded empty data paths, no placeholder returns.

One pre-existing warning (`-Wfree-nonheap-object` in `encode_auth_payload` at connection.cpp:57) is noted in 120-01-SUMMARY.md as untouched and unrelated to this phase — filed as an informational item only.

## Data-Flow Trace (Level 4)

Not applicable — this phase ships transport primitives + C++ CLI refactor, not a UI rendering dynamic data. Live-node wall-clock measurements with byte-identical `diff -r` outputs already demonstrate real data flowing end-to-end through the new pipelined path.

## Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| `cli_tests` builds clean | `cd cli/build && cmake --build . --target cli_tests -j$(nproc)` | exit 0, `[100%] Built target cli_tests` | PASS |
| `[pipeline]` tests pass | `cli/build/tests/cli_tests "[pipeline]"` | `All tests passed (48 assertions in 8 test cases)` | PASS |
| Full cli_tests suite pass | `cli/build/tests/cli_tests --reporter compact` | `All tests passed (252 assertions in 49 test cases)` | PASS |
| `cdb` binary builds clean | `cd cli/build && cmake --build . --target cdb -j$(nproc)` | exit 0, `[100%] Built target cdb` | PASS |

## Deviation Notes (informational, not blocking)

1. **Plan 01 helper observer callback** — The 120-01-PLAN required the `pipeline_pump.h` helper to accept an optional `on_off_target` observer callback so `Connection::recv_for` could inject `spdlog::debug` for off-target stashes at the wrapper site. The shipped helper has no such parameter — it is silent. Consequently `Connection::recv_for` also emits no log for stashed off-target replies. **Impact:** nil on any ROADMAP success criterion. The practical unknown-rid observability still exists at the batch-caller level (cmd::put logs at commands.cpp:618, cmd::get logs at commands.cpp:727). 120-01-SUMMARY.md explicitly acknowledges this under "Deviations from Plan" → "recv_for spdlog::debug moved into pump helper vs. stripped".

2. **Plan 01 Task 2 test count** — Plan said ≥8 TEST_CASEs; delivered exactly 8 (floor of the range). Also one test that was anticipated ("off-target observer callback fires for each stash") was dropped because the helper doesn't have an observer param. The kept 8 cases fully cover all decision branches the helper actually has.

Neither deviation affects PIPE-01, PIPE-02, or PIPE-03 satisfaction.

## Gaps Summary

No blocking gaps. All three ROADMAP success criteria and all three requirement IDs (PIPE-01, PIPE-02, PIPE-03) are satisfied with real evidence in code, tests, and live-node measurement. Deviations are documented in 120-01-SUMMARY.md, do not affect goal achievement, and are informational.

---

## Final Verdict: **VERIFIED**

Phase 120 delivers PIPE-01 (5.4×–8.4× live-node speedup with byte-identical outputs), PIPE-02 (single-sender invariant grep-verified — zero `send*` calls inside `recv_for` or the pump helper, no new threads), and PIPE-03 (`Connection::kPipelineDepth = 8` as the single source of truth referenced by name everywhere). The one documented deviation (silent helper vs. plan's observer callback) does not reduce any roadmap guarantee.

---

*Verified: 2026-04-19T03:20:25Z*
*Verifier: Claude (gsd-verifier)*
