---
phase: 124
plan: "02"
subsystem: cli
tags: [pubk, auto-publish, presence-check, d-01, d-01a, sc-124-1, sc-124-6]
requires:
  - 124-01  # BlobData post-122 shape, build_owned_blob, encode_blob_write_body,
            # MsgType::BlobWrite=64, MsgType::WriteAck=30, PUBKEY_MAGIC,
            # make_pubkey_data all produced by plan 01.
provides:
  - cli/src/pubk_presence.h :: ensure_pubk(const Identity&, Connection&,
    std::span<const uint8_t, 32>, uint32_t&)
  - cli/src/pubk_presence.h :: ensure_pubk_impl<Sender, Receiver>(Identity,
    ns_span, Sender&&, Receiver&&, uint32_t&) -- template; transport-agnostic
    probe+emit core for test harness use.
  - cli/src/pubk_presence.h :: reset_pubk_presence_cache_for_tests()
  - cli/src/pubk_presence.cpp :: file-scope pubk_cache static + Connection
    wrapper that applies D-01a delegate-skip before delegating to
    ensure_pubk_impl.
affects:
  - cli/CMakeLists.txt           (cdb executable now links pubk_presence.cpp)
  - cli/tests/CMakeLists.txt     (cli_tests now links pubk_presence.cpp + test_auto_pubk.cpp)
  - cli/tests/test_auto_pubk.cpp (new -- 7 [pubk]-tagged TEST_CASEs)
tech-stack:
  added: []
  patterns:
    - "Option A template extraction: transport-agnostic probe+emit loop exposed
       as a function template so unit tests drive it with CapturingSender +
       ScriptedSource; production Connection wrapper is a thin lambda binding."
    - "Process-lifetime std::unordered_set<array<uint8_t,32>> cache with
       XOR-fold hash; reset hook for test isolation."
    - "49-byte ListRequest + type_filter=PUBKEY_MAGIC, limit=1 (byte-for-byte
       the find_pubkey_blob probe layout from commands.cpp:150-198)."
    - "Strictly synchronous conn.send + conn.recv for PUBK emit (RESEARCH
       Pitfall #7) -- no send_async / recv_for / co_await in the helper."
key-files:
  created:
    - cli/src/pubk_presence.h
    - cli/src/pubk_presence.cpp
    - cli/tests/test_auto_pubk.cpp
  modified:
    - cli/CMakeLists.txt
    - cli/tests/CMakeLists.txt
decisions:
  - "Option A chosen: extract probe+emit core into ensure_pubk_impl<Sender,
     Receiver> template; tests call it directly. Option B (loopback asio
     in-test) rejected for complexity."
  - "Cache lookup + delegate-skip live in the wrapper, NOT the template.
     Template is stateless; wrapper owns the invocation-scoped cache."
  - "D-01a delegate-skip short-circuits at target_ns != id.namespace_id()
     -- helper NEVER emits a PUBK on behalf of a delegate (T-124-02 mitigation)."
metrics:
  tasks_completed: 2
  files_created: 3
  files_modified: 2
  duration_minutes: 25
  completed_date: "2026-04-21"
---

# Phase 124 Plan 02: pubk_presence module + [pubk] test coverage -- Summary

Invocation-scoped PUBK presence module shipped: `ensure_pubk` probes via
`ListRequest+type_filter=PUBK_MAGIC+limit=1` and emits a PUBK under
`BlobWrite=64` when absent, strictly synchronously, with a process-lifetime
cache and a structural delegate-skip that makes T-124-02 (delegate spoof)
impossible.

## Objective

Build the cooperative half of the node-enforced PUBK-first invariant (memory
`project_phase122_pubk_first_invariant.md`): before the first owner-write
from a `cdb` invocation to a given namespace, check whether the node holds a
PUBK for it; emit one if not. Subsequent same-namespace writes in the same
process are zero-wire-op cache hits. Delegate writes are a structural no-op
(D-01a) -- the helper never emits on behalf of someone else. Everything
unit-tested against `ScriptedSource` + `CapturingSender` mocks -- no asio
in the test harness.

## Deliverables

### New files

| File                                  | Purpose                                                                                                      |
|---------------------------------------|--------------------------------------------------------------------------------------------------------------|
| `cli/src/pubk_presence.h`             | `ensure_pubk` wrapper declaration + `ensure_pubk_impl<Sender,Receiver>` template + test-only cache reset.    |
| `cli/src/pubk_presence.cpp`           | File-scope `pubk_cache()` static + delegate-skip + Connection-binding wrapper that calls `ensure_pubk_impl`. |
| `cli/tests/test_auto_pubk.cpp`        | 7 `[pubk]`-tagged TEST_CASEs covering probe/emit/cache/delegate/error/golden-bytes.                          |

### Modified files

| File                        | Change                                                     |
|-----------------------------|------------------------------------------------------------|
| `cli/CMakeLists.txt`        | `src/pubk_presence.cpp` added to `cdb` executable sources. |
| `cli/tests/CMakeLists.txt`  | `test_auto_pubk.cpp` + `pubk_presence.cpp` added to `cli_tests`. |

## Commits (2 atomic)

| Hash       | Type   | Subject                                                          |
|------------|--------|------------------------------------------------------------------|
| `6e9711e4` | feat   | `feat(124-02): add pubk_presence module for auto-PUBK probe+emit` |
| `ab9a03ae` | test   | `test(124-02): add 7 [pubk] TEST_CASEs for auto-PUBK semantics`   |

## TEST_CASE Coverage ([pubk] tag)

All 7 TEST_CASEs PASSED (49 assertions):

| # | Name                                                           | Asserts                                                         |
|---|----------------------------------------------------------------|-----------------------------------------------------------------|
| 1 | `probe sees count>0 and skips emit`                             | returns true, 1 send (ListRequest only), rid advances by 1      |
| 2 | `probe sees count==0 and emits PUBK via BlobWrite`              | returns true, 2 sends (ListRequest + BlobWrite), rid +=2, envelope >4196 B |
| 3 | `second call for same namespace is cache hit zero wire ops`    | zero sends + rid unchanged (simulated wrapper-skip)             |
| 4 | `different namespace re-probes template is stateless`          | 2nd call re-runs probe; count=1 response → 1 send, skip emit    |
| 5 | `delegate vs owner — template never silently skips`            | empty source + own_ns → returns false; probe WAS attempted (1 send) |
| 6 | `probe transport error returns false`                          | sender.ok=false → returns false immediately                     |
| 7 | `golden ListRequest payload bytes`                             | 49B, [0..31]=own_ns, [32..39]=0, [40..43]=0x00000001, [44]=0x02, [45..48]="PUBK" |

## Deviations from Plan

None. Plan executed as written; Option A template extraction adopted per plan
guidance. Two minor formatting adjustments:

1. Two TEST_CASE declarations were originally written with a line break between
   the name and `"[pubk]"` tag; reformatted onto single lines so the acceptance
   grep `TEST_CASE.*\[pubk\]` returns 7 as specified. No semantic change —
   Catch2 discovers the same 7 cases either way.
2. The `reset_pubk_presence_cache_for_tests` decl count check expected exactly
   1 in the header; delivered exactly 1. The `bool ensure_pubk` grep returns
   2 because `ensure_pubk_impl` also matches the prefix regex — this is a
   natural consequence of Option A (template lives in header) and is
   documented in the plan's `<acceptance_criteria>` via the separate
   `ensure_pubk_impl` grep clause.

## Authentication Gates

None — plan 02 is unit-test-only; no network / node / auth interaction required.

## Grep Verification Block

```
$ grep -c "bool ensure_pubk"              cli/src/pubk_presence.h            -> 2 (ensure_pubk + ensure_pubk_impl)
$ grep -c "reset_pubk_presence_cache_for_tests" cli/src/pubk_presence.h      -> 1
$ grep -c "pubk_cache()"                  cli/src/pubk_presence.cpp          -> 4 (accessor + calls)
$ grep -nE "conn\.send_async|conn\.recv_for|co_await" cli/src/pubk_presence.cpp -> 0 (synchronous-only, Pitfall #7 enforced)
$ grep -cE "list_payload\[44\]\s*=\s*0x02" cli/src/pubk_presence.h           -> 1 (flags byte)
$ grep "PUBKEY_MAGIC\.data\(\), 4"       cli/src/pubk_presence.h             -> 1 line (type_filter bytes)
$ grep "test_auto_pubk.cpp"              cli/tests/CMakeLists.txt            -> 1
$ grep "pubk_presence.cpp"               cli/tests/CMakeLists.txt            -> 1
$ grep -cE "TEST_CASE.*\[pubk\]"         cli/tests/test_auto_pubk.cpp        -> 7
$ grep -cE "ensure_pubk_impl\("          cli/tests/test_auto_pubk.cpp        -> 8 (7 TEST_CASEs + declaration reference)
$ grep -cE "asio::io_context"            cli/tests/test_auto_pubk.cpp        -> 0 (mock-only testing)
$ grep -cE "ensure_pubk_impl"            cli/src/pubk_presence.h             -> (≥1 declared)
```

## Verification

- `./build/cli/tests/cli_tests "[pubk]"` — **PASSED** (7 TEST_CASEs, 49 assertions)
- `./build/cli/tests/cli_tests "[wire]"` — **PASSED** (23 cases, 568 assertions) — no regression from plan 01 changes
- `./build/cli/tests/cli_tests` full suite — **PASSED** (95 cases, 197595 assertions) — no regressions

## Threat Model Outcomes

| ID           | Category               | Disposition | Status                                                                                                 |
|--------------|------------------------|-------------|--------------------------------------------------------------------------------------------------------|
| T-124-01     | Spoofing (race)        | mitigate    | Mitigated: strictly synchronous `conn.send` + `conn.recv`; no async variants (grep-verified, 0 hits).  |
| T-124-02     | Spoofing (delegate)    | mitigate    | Mitigated: wrapper short-circuits at `memcmp(own_ns, target_ns) != 0` before any wire op.              |
| T-124-03     | Tampering (old format) | mitigate    | Mitigated: emit path routes via `build_owned_blob` + `encode_blob_write_body` → `MsgType::BlobWrite`.  |
| T-124-cache-01 | Tampering (cache)    | accept      | Cache is process-lifetime, no disk persistence; attacker with memory access has already escalated.     |
| T-124-timing-01 | Info disclosure     | accept      | Probe timing is the feature's observable effect; no amplification beyond user's own behavior.          |

## D-XX Traceability

- **D-01** (probe+emit before first owner-write, invocation-scoped cache): implemented in `pubk_presence.{h,cpp}`, exercised by tests 1-4.
- **D-01a** (delegate structural no-op): wrapper short-circuit in `pubk_presence.cpp:ensure_pubk`, covered by test 5 (inverse: with own_ns the template attempts the probe).
- **D-03** (helper composition — `build_owned_blob`): consumed by `ensure_pubk_impl` emit path.
- **D-04** (BlobWrite envelope): consumed by `ensure_pubk_impl` emit path; golden payload test 7 locks the probe layout (ListRequest is a pre-existing primitive, unchanged).
- **D-09** (new test file for auto-PUBK coverage): `test_auto_pubk.cpp` with 7 `[pubk]`-tagged cases.

## Known Stubs

None. Plan 02 delivers a complete, production-wired module. Consumers
(`cmd::put`, `cmd::delegate`, `chunked::put_chunked`, etc.) call
`ensure_pubk` in plans 03-04.

## Threat Flags

None. No new security-relevant surface introduced beyond the threat register
already covered by the plan's `<threat_model>` block.

## Handoff to Plan 03

Plan 03 migrates the 20 TEMP-124 stubs in `commands.cpp` + `chunked.cpp` to
use `build_owned_blob` + `encode_blob_write_body`, and at the same time
inserts `ensure_pubk(id, conn, ns_span, rid_counter)` calls immediately after
`conn.connect(...)` succeeds in `cmd::put`, `cmd::delegate`, `cmd::reshare`,
`cmd::rm`, `cmd::rm_batch`, `cmd::revoke`, and `chunked::put_chunked`. The
helper is ready to drop in -- signature, cache semantics, and wire layout
match the plan 03 integration points exactly.

`cmd::publish` remains a bypass of the helper (it IS the PUBK writer);
plan 03 will populate the wrapper's cache manually on successful WriteAck
from `cmd::publish`.

## Self-Check: PASSED

- `cli/src/pubk_presence.h` — FOUND
- `cli/src/pubk_presence.cpp` — FOUND
- `cli/tests/test_auto_pubk.cpp` — FOUND
- `cli/CMakeLists.txt` modified (cdb links pubk_presence.cpp) — CONFIRMED
- `cli/tests/CMakeLists.txt` modified (cli_tests includes both test + source) — CONFIRMED
- Commit `6e9711e4` — FOUND in git log
- Commit `ab9a03ae` — FOUND in git log
- `./build/cli/tests/cli_tests "[pubk]"` exits 0, 7 TEST_CASEs PASSED — CONFIRMED
- `./build/cli/tests/cli_tests` (full suite) exits 0, 95 cases PASSED — CONFIRMED
