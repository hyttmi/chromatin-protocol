---
phase: 120-request-pipelining
plan: 01
subsystem: cli-transport
tags: [cdb, cli, pipelining, connection, aead, pq-crypto, catch2, tdd]

# Dependency graph
requires:
  - phase: 117-blob-type-indexing-ls-filtering
    provides: extended ListResponse entry shape (orthogonal — not touched here)
provides:
  - Connection::send_async(type, payload, rid) — non-blocking up to PIPELINE_DEPTH, cooperative recv-pump for backpressure
  - Connection::recv_for(rid) — waits for a specific reply, stashing off-target replies in a per-Connection correlation map
  - cli/src/pipeline_pump.h — header-only chromatindb::cli::pipeline namespace with pump_recv_for and pump_one_for_backpressure helpers
  - PIPELINE_DEPTH = 8 constant (PIPE-03)
  - 8 Catch2 [pipeline] tests covering the correlation pump, backpressure, unknown-rid discard, and duplicate-rid overwrite paths
affects:
  - 120-02-PLAN (cmd::get and cmd::put migration to pipelined primitives)
  - any future phase that wants to pipeline over a single PQ/TrustedHello connection

# Tech tracking
tech-stack:
  added: []  # no new libraries — stdlib <unordered_map> only
  patterns:
    - "Cooperative recv-pump: single-threaded backpressure by draining one recv() per loop iteration"
    - "Header-only helper extraction for socket-free testing of routing logic"
    - "Source callable pattern: templated std::optional<DecodedTransport>() wraps any recv analog"

key-files:
  created:
    - cli/src/pipeline_pump.h
    - cli/tests/test_connection_pipelining.cpp
  modified:
    - cli/src/connection.h
    - cli/src/connection.cpp
    - cli/tests/CMakeLists.txt

key-decisions:
  - "PIPELINE_DEPTH hard-coded at 8 (D-07 / PIPE-03); no flag, no config key"
  - "Correlation map is std::unordered_map<uint32_t, DecodedTransport> bounded by depth; no eviction policy"
  - "Unknown-rid reply: spdlog::debug + stash (lingers until close()); bounded ⇒ no leak"
  - "Single-threaded cooperative pump — no new thread, coroutine, strand, mutex, or atomic"
  - "Pump logic extracted to header-only pipeline_pump.h so Catch2 tests don't need a real socket or PQ handshake"
  - "Connection::send_async / Connection::recv_for delegate to helpers via a lambda wrapping recv()"

patterns-established:
  - "Cooperative pump: single-reader drains replies into a correlation map on backpressure"
  - "Testable extraction: routing logic lifted out of a socket-holding class into a templated free function so it runs without I/O"
  - "Single-sender invariant enforcement by construction: helpers never invoke the send path"

requirements-completed:
  - PIPE-02
  - PIPE-03

# Metrics
duration: 10min
completed: 2026-04-18
---

# Phase 120 Plan 01: Pipelining Primitives Summary

**Connection gains send_async + recv_for with a header-only correlation-pump helper, preserving the AEAD single-sender invariant by construction and covered by 8 Catch2 [pipeline] tests.**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-04-18T15:11:33Z
- **Completed:** 2026-04-18T15:21:54Z
- **Tasks:** 2 (both completed)
- **Files created:** 2
- **Files modified:** 3

## Accomplishments

- Extended `Connection` with `send_async(type, payload, rid)` and `recv_for(rid)` as additive public surface — existing `send`/`recv`/`close`/`connect` are unchanged.
- Added file-static `PIPELINE_DEPTH = 8` constant (commented `// PIPE-03`) inside `connection.cpp`'s existing constants block.
- Added private `std::unordered_map<uint32_t, DecodedTransport> pending_replies_` and `size_t in_flight_ = 0` at the end of `Connection`'s trailing member block. Added `#include <unordered_map>` to the header.
- Extended `Connection::close()` with `pending_replies_.clear()` and `in_flight_ = 0` grouped with the existing per-session counter resets.
- Extracted the pure routing logic into `cli/src/pipeline_pump.h` (header-only, `chromatindb::cli::pipeline` namespace) so `pump_recv_for` and `pump_one_for_backpressure` are testable against a scripted source fixture — no socket, no PQ handshake.
- Refactored `Connection::send_async` and `Connection::recv_for` to delegate to the helpers via a lambda wrapping `recv()`. Connection bodies are now 9 and 7 lines respectively, with all decision branches covered by the Catch2 tests.
- Added 8 Catch2 test cases tagged `[pipeline]` covering: direct-hit lookup, send-8-receive-out-of-order correlation, pre-stashed fast path, source-dead returns nullopt, backpressure drains one reply per call, backpressure source-dead returns false, unknown-rid discard (D-04), duplicate-rid overwrite via `insert_or_assign`.

## Final shape of the primitives

```cpp
// Header — cli/src/connection.h
bool send_async(MsgType type, std::span<const uint8_t> payload, uint32_t request_id);
// Equivalent to send(), except blocks via cooperative recv-pump when
// PIPELINE_DEPTH are already in flight. Off-target replies drained during
// backpressure are stashed for the next recv_for().

std::optional<DecodedTransport> recv_for(uint32_t request_id);
// Returns stashed reply immediately; otherwise loops recv() and stashes
// off-target replies until the target arrives. nullopt on transport error.
```

## Correlation map bound

- Lives on `Connection` as `std::unordered_map<uint32_t, DecodedTransport> pending_replies_`.
- Bounded by `PIPELINE_DEPTH = 8` by construction: `send_async` only lets 8 requests be in flight, so at most 8 replies can ever land in the map.
- Cleared on `close()` alongside `send_counter_`/`recv_counter_` reset, so the map cannot outlive the session.

## Test counts

- **[pipeline] tests:** 8/8 passing
  - `pipeline: recv_for returns matching reply on direct hit`
  - `pipeline: send-8-receive-out-of-order correlation`
  - `pipeline: pre-stashed reply returns without calling source`
  - `pipeline: source-dead during pump returns nullopt`
  - `pipeline: backpressure drains one reply per call`
  - `pipeline: backpressure source-dead returns false`
  - `pipeline: unknown-rid reply is stashed not crashed (D-04)`
  - `pipeline: duplicate rid reply overwrites without growth`
- **Full cli_tests suite:** 49/49 passing (41 baseline + 8 new). Zero regressions.
- **cdb binary:** builds cleanly; no new warnings introduced by Connection or `pipeline_pump.h` code.

## Task Commits

1. **Task 1: Extend Connection with send_async + recv_for primitives + correlation map** — `c40585f9` (feat)
2. **Task 2 RED: Add failing [pipeline] tests for correlation pump** — `555da9c5` (test)
3. **Task 2 GREEN: Extract pipeline pump to header-only helper + refactor Connection delegates** — `452c8671` (feat)

_Note: Task 2 is TDD with a RED-then-GREEN split. Task 1 was committed as a single feat because the plan's direct acceptance tests for it live in Task 2's [pipeline] suite._

## Files Created/Modified

- `cli/src/connection.h` — added `#include <unordered_map>`, declared `send_async` and `recv_for`, added `pending_replies_` and `in_flight_` private members with D-04 comment.
- `cli/src/connection.cpp` — added `PIPELINE_DEPTH = 8` constant, added `#include "cli/src/pipeline_pump.h"`, implemented `send_async`/`recv_for` as thin delegates over the helpers, extended `close()` to reset pipelining state.
- `cli/src/pipeline_pump.h` — new header-only helper with `pump_recv_for` and `pump_one_for_backpressure` in `chromatindb::cli::pipeline` namespace; documents contracts and the single-sender invariant.
- `cli/tests/test_connection_pipelining.cpp` — new Catch2 file with 8 [pipeline] tests exercising the pump with a scripted source fixture (`ScriptedSource` dequeues replies and goes dead when exhausted).
- `cli/tests/CMakeLists.txt` — added `test_connection_pipelining.cpp` to `cli_tests` source list (alphabetical slot between `test_contacts.cpp` and the `../src/` files).

## Verification of Existing Callers

Grep confirmed zero changes to `cli/src/commands.cpp`. The existing sequential commands remain unchanged and continue to use `Connection::send` / `Connection::recv` directly:

- `cmd::ls`, `cmd::stats`, `cmd::info`, `cmd::exists`, `cmd::rm`, `cmd::reshare`, `cmd::publish`, `cmd::contact_*`, `cmd::delegations`, `cmd::revoke`

The new primitives are purely additive. Phase 120-02 will migrate `cmd::get` / `cmd::put` onto them.

## Single-Sender Invariant Verification (PIPE-02 / D-09)

`Connection::recv_for` body (post-refactor) has zero `send`, `send_chunked`, or `send_encrypted` calls — verified by reading the delegating lambda body (`[this] { return recv(); }`) and by `grep` of the refactored function. `Connection::send_async` has exactly one `send(` call: the post-backpressure delegation after the pump loop exits. The pump loop itself only drives `pump_one_for_backpressure`, which calls `source()` (the lambda wrapping `recv()`) — never the send path. The sole writer of `send_counter_` therefore remains the caller of `send()`, and AEAD nonce monotonicity is preserved by construction. The header docstring captures this contract (`T-120-01`, `T-120-06`).

## Decisions Made

None beyond what the plan specified. Followed plan as written:
- PIPELINE_DEPTH=8 hard-coded (D-07 / PIPE-03).
- Correlation map on Connection, bounded, no eviction (D-04).
- Cooperative single-threaded pump — no threads, coroutines, or strands (D-03, D-09).
- Header-only helper extraction (Task 2 option 1 per the plan).

## Deviations from Plan

### Minor: plan documentation referenced but not present

The plan references `.planning/phases/120-request-pipelining/120-PATTERNS.md`, but this file does not exist in the phase directory (only `120-CONTEXT.md`, `120-01-PLAN.md`, `120-02-PLAN.md`, `120-DISCUSSION-LOG.md` are present). All the required decisions (D-01 through D-09) are captured in `120-CONTEXT.md`, which was loaded and followed. No behavior change; flagged for the planner's awareness.

### Minor: recv_for spdlog::debug moved into pump helper vs. stripped

The plan's Task 1 step 2.2 included `spdlog::debug("recv_for: stashing off-target reply rid={} ...")` inside the recv_for body. Task 2 refactor (per the plan's Task 2 Step 2) moved the pump into a header-only helper and explicitly noted "The unknown-rid `spdlog::debug` line from Task 1 moves into the test-side observability comment. The header-only helper is silent on the off-target-stash path; if observability of the stash event is desired, the planner notes that callers can log around `recv_for` themselves."

I followed the plan's Task 2 guidance and kept the helper silent. The trade-off is documented inline in the helper header. This is not a deviation — the plan itself anticipated this, but I'm flagging it here so reviewers of the Task 1 commit don't wonder where the log line went (answer: superseded by the Task 2 refactor).

### Pre-existing unrelated warning

When building `cdb`, gcc 15.2 emits a `-Wfree-nonheap-object` warning inside `encode_auth_payload` at `cli/src/connection.cpp:57`. This line is pre-existing (part of the file before this plan) and unrelated to the pipelining changes. Per the scope boundary in the deviation rules, I did not touch it. Recommend filing a separate backlog item if the warning is to be addressed.

---

**Total deviations:** 0 auto-fixed (0 bugs, 0 missing critical, 0 blocking). The plan executed as written apart from a missing reference file (120-PATTERNS.md) whose content was already mirrored in 120-CONTEXT.md.
**Impact on plan:** No scope creep. No architectural changes.

## Issues Encountered

None. All primitives worked on first try after the `pipeline_pump.h` helper was created; the test suite was green on the first GREEN-phase build.

## User Setup Required

None — no external service configuration required. This is a pure code change inside the CLI.

## Next Phase Readiness

- **Ready for 120-02 (cmd::get / cmd::put migration).** The primitives are declared and tested; the next plan replaces the inner sequential loops in `cli/src/commands.cpp` with a two-pass pipelined shape (fire N, drain replies, fire one per drained reply).
- **No blockers.** The live test node at 192.168.1.73 remains exercisable via the existing sequential paths (`cmd::info`, etc.) because they are untouched. Phase 120-02 will need to verify end-to-end pipelined behavior against the same node.

## Self-Check: PASSED

Created files verified on disk:
- `cli/src/pipeline_pump.h` — FOUND
- `cli/tests/test_connection_pipelining.cpp` — FOUND

Modified files verified on disk:
- `cli/src/connection.h` — FOUND (adds send_async/recv_for + members)
- `cli/src/connection.cpp` — FOUND (adds PIPELINE_DEPTH, send_async/recv_for bodies, close() reset)
- `cli/tests/CMakeLists.txt` — FOUND (adds test_connection_pipelining.cpp)

Commits verified in git log:
- `c40585f9` (Task 1 feat) — FOUND
- `555da9c5` (Task 2 RED test) — FOUND
- `452c8671` (Task 2 GREEN feat) — FOUND

Verification commands executed and exit code 0:
- `cmake --build . --target cli_tests -j$(nproc)` — PASS (100% built)
- `ctest --test-dir . -R "pipeline" --output-on-failure` — PASS (8/8)
- `ctest --test-dir . --output-on-failure` — PASS (49/49)
- `cmake --build . --target cdb -j$(nproc)` — PASS

---
*Phase: 120-request-pipelining*
*Plan: 01*
*Completed: 2026-04-18*
