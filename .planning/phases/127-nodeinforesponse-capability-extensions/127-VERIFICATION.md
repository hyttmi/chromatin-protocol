---
phase: 127-nodeinforesponse-capability-extensions
verified: 2026-04-22T15:00:00Z
status: passed
score: 7/7 must-haves verified
overrides_applied: 0
---

# Phase 127: NodeInfoResponse Capability Extensions — Verification Report

**Phase Goal:** Extend NodeInfoResponse wire format with 4 capability fields (max_blob_data_bytes, max_frame_bytes, rate_limit_bytes_per_sec, max_subscriptions_per_connection) so clients and peers can discover node caps in a single round-trip. Node encodes, CLI decodes/renders, test covers byte-exact wire layout and boundary cases.
**Verified:** 2026-04-22T15:00:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #   | Truth | Status | Evidence |
| --- | ----- | ------ | -------- |
| 1   | NodeInfoResponse wire format carries all 4 new fields at byte-exact positions in the fixed section, inserted BEFORE types_count | VERIFIED | `db/peer/message_dispatcher.cpp:720-734` — 4 store_*_be calls between storage_max (line 717) and response[off++] = types_count (line 736) |
| 2   | Field order and types match spec: blob(u64 BE) → frame(u32 BE) → rate(u64 BE) → subs(u32 BE), +24 bytes total | VERIFIED | Lines 721/725/729/733 strictly increasing; `resp_size` row at line 691-694 adds `+ 8 + 4 + 8 + 4`; wire-size delta assertion in test: `CHECK(info_response.size() == 1 + version.size() + 8+4+4+8+8+8 + 24 + 1 + types_count)` |
| 3   | Fields are sourced correctly: blob/frame from framing.h constants, rate from rate_limit_bytes_per_sec_ member, subs from max_subscriptions_ member | VERIFIED | Encoder references `chromatindb::net::MAX_BLOB_DATA_SIZE`, `chromatindb::net::MAX_FRAME_SIZE`, `rate_limit_bytes_per_sec_`, `max_subscriptions_` — all read-only member accesses, no new constructor params |
| 4   | CLI decodes all 4 fields and renders them after Quota line with zero-value branches | VERIFIED | `cli/src/commands.cpp:2279-2306` — 4 read_*() calls followed by 4 printf blocks; `rate_limit_bytes_per_sec == 0` → "unlimited", `max_subscriptions == 0` → "unlimited"; no phase-leak tokens in format strings |
| 5   | Stale git_hash layout comment removed from CLI; wire layout comment updated to post-Phase-127 reality | VERIFIED | `grep -c 'git_hash' cli/src/commands.cpp` == 0; comment at lines 2233-2243 documents full 11-field post-127 layout |
| 6   | VERI-02: encode/decode round-trip tests cover all 4 new fields with default, zero, and max boundary scenarios | VERIFIED | 3 TEST_CASEs at test_peer_manager.cpp lines 2774/2949/3042 under `[peer][nodeinfo]` tag; 45 assertions pass per targeted Catch2 run; max boundary asserts UINT64_MAX/UINT32_MAX round-trip |
| 7   | REQUIREMENTS.md NODEINFO-03 text updated to reflect approved rename from rate_limit_messages_per_second (u32) to rate_limit_bytes_per_sec (u64 BE) | VERIFIED | REQUIREMENTS.md line 26: "rate_limit_bytes_per_sec (u64 BE) — renamed + retyped from the original (u32 BE) spec per Phase 127 CONTEXT.md D-03"; zero occurrences of rate_limit_messages_per_second remaining |

**Score:** 7/7 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
| -------- | -------- | ------ | ------- |
| `db/peer/message_dispatcher.cpp` | Extended NodeInfoResponse encoder with 4 new capability fields | VERIFIED | +20/-2 lines; include `db/net/framing.h` at line 12; 4 store_*_be calls at lines 721/725/729/733; resp_size extended with `+ 8 + 4 + 8 + 4` group; commit d418b667 |
| `cli/src/commands.cpp` | Updated cdb info decoder + renderer for 4 new fields | VERIFIED | +27/-5 lines; 4 read_*() calls at lines 2279-2282; 4 printf blocks at lines 2295-2306; zero-value branches present; git_hash comment removed; commit 376055f7 |
| `db/tests/peer/test_peer_manager.cpp` | Extended [peer][nodeinfo] TEST_CASE with new-field assertions + 2 boundary TEST_CASEs | VERIFIED | +227 lines; #include "db/net/framing.h" at line 16; wire-size delta CHECK with `+ 24` at lines 2938-2942; 3 TEST_CASEs with tag [peer][nodeinfo]; commits 81bcdddf + dbe172d2 |
| `.planning/REQUIREMENTS.md` | NODEINFO-03 line updated to rate_limit_bytes_per_sec (u64 BE) | VERIFIED | Line 26 rewritten; old name has zero occurrences; commit 2a94f3b3 |

### Key Link Verification

| From | To | Via | Status | Details |
| ---- | -- | --- | ------ | ------- |
| `db/peer/message_dispatcher.cpp` NodeInfoRequest handler | `db/net/framing.h` MAX_BLOB_DATA_SIZE, MAX_FRAME_SIZE | `#include "db/net/framing.h"` at line 12 + namespace-qualified `chromatindb::net::` references | WIRED | Include present exactly once; both constants referenced in encoder at lines 721/725 |
| `db/peer/message_dispatcher.cpp` NodeInfoRequest handler | MessageDispatcher state members rate_limit_bytes_per_sec_ and max_subscriptions_ | Plain member access inside handler | WIRED | Lines 729/733 reference members set by PeerManager via set_rate_limits()/set_max_subscriptions(); no new wiring needed |
| `cli/src/commands.cpp` info() | NodeInfoResponse payload bytes (post-Phase-127 wire layout) | read_u64()/read_u32() calls inserted between storage_max read and printf block | WIRED | Reads at 2279-2282 decode exactly the 4 fields written by the encoder in the expected order |
| `db/tests/peer/test_peer_manager.cpp` [peer][nodeinfo] TEST_CASE | `db/peer/message_dispatcher.cpp` NodeInfoResponse encoder | Real NodeInfoRequest fired through PeerManager; response bytes walked manually | WIRED | Test asserts `chromatindb::net::MAX_BLOB_DATA_SIZE`, `MAX_FRAME_SIZE`, config rate+subs values round-trip byte-exact |

### Data-Flow Trace (Level 4)

Level 4 not applicable: the artifacts produce wire bytes from pre-existing node state (config members, framing.h constants). No component renders dynamic data from a store/DB query. The test fires a real NodeInfoRequest and asserts values from the response payload — that is the data flow being validated, and it passes per the targeted Catch2 run.

### Behavioral Spot-Checks

Per project guardrails: targeted Catch2 run (`./build-debug/db/chromatindb_tests "[peer][nodeinfo]"`) reported "All tests passed (45 assertions in 3 test cases)" per 127-04-SUMMARY.md. Both `chromatindb` and `cdb` targets built clean. Full-suite runs are delegated to the user. No in-process runnable entry points to spot-check further without starting the server.

| Behavior | Evidence | Status |
| -------- | -------- | ------ |
| NodeInfoResponse encodes 4 new fields at correct offsets with correct values | 45 Catch2 assertions pass across 3 TEST_CASEs per SUMMARY; actual code verified matches assertions | PASS |
| CLI decoder reads fields in correct order without truncation | Field reads at lines 2279-2282 strictly increasing; bounds-checked lambdas (read_u32/read_u64) throw on truncation | PASS |
| Zero-value sentinel rendering | Code at lines 2297-2303 branches on ==0 to "unlimited" | PASS |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
| ----------- | ----------- | ----------- | ------ | -------- |
| NODEINFO-01 | 127-01, 127-03, 127-04 | NodeInfoResponse adds max_blob_data_bytes (u64 BE) | SATISFIED | Encoder line 721; CLI read line 2279; test assertion at line 2900 |
| NODEINFO-02 | 127-01, 127-03, 127-04 | NodeInfoResponse adds max_frame_bytes (u32 BE) | SATISFIED | Encoder line 725; CLI read line 2280; test assertion at line 2907 |
| NODEINFO-03 | 127-01, 127-02, 127-03, 127-04 | NodeInfoResponse adds rate_limit_bytes_per_sec (u64 BE) — renamed per D-03 | SATISFIED | Encoder line 729; REQUIREMENTS.md updated; CLI read line 2281; test assertions at lines 2914/3031/3122 |
| NODEINFO-04 | 127-01, 127-03, 127-04 | NodeInfoResponse adds max_subscriptions_per_connection (u32 BE) | SATISFIED | Encoder line 733; CLI read line 2282; test assertions at lines 2921/3037/3128 |
| VERI-02 | 127-04 | Unit tests for NodeInfoResponse encode/decode covering the four new fields | SATISFIED | 3 TEST_CASEs under [peer][nodeinfo]; 45 assertions pass; default/zero/max boundary coverage |

**Note on ROADMAP SC-1 wording:** ROADMAP.md Phase 127 success criterion 1 uses the stale field name `rate_limit_messages_per_second` (u32 BE). This is an approved deviation — CONTEXT.md D-03 documents the rename decision, REQUIREMENTS.md was updated in Plan 127-02, and ROADMAP.md narrative refresh is explicitly deferred to Phase 131 (DOCS-03). The implemented wire format (`rate_limit_bytes_per_sec` u64 BE) is the correct, approved specification.

### Anti-Patterns Found

| File | Pattern | Severity | Impact |
| ---- | ------- | -------- | ------ |
| `db/tests/peer/test_peer_manager.cpp:2841` | Single-line layout comment in base TEST_CASE documents pre-Phase-127 wire layout (missing 4 new fields) | Info | Cosmetic — per-field inline comments added at lines 2892+ are accurate; readers won't be misled |
| `db/tests/peer/test_peer_manager.cpp:3051` | Max-boundary TEST_CASE sets rate_limit_bytes_per_sec=UINT64_MAX but leaves rate_limit_burst=0; test passes because NodeInfoRequest has empty payload (0 > 0 is false, bucket always passes) | Info | Test fragility — if a future change adds per-message rate cost or extends this test with a non-empty message, it will fail for unrelated reasons. Fix: also set `cfg.rate_limit_burst = UINT64_MAX`. |
| `db/tests/peer/test_peer_manager.cpp:3025` | Zero-boundary TEST_CASE duplicates the base case's rate==0 assertion (default config is already 0) | Info | Cosmetic redundancy — coverage is correct; only the subs=0 assertion is novel in this case |

All three anti-patterns are info-level (sourced from the phase code review in 127-REVIEW.md). None block the phase goal.

### Human Verification Required

None required for automated goal verification. All four wire fields are verified by the Catch2 integration test. CLI rendering is verified by code inspection (correct format strings, zero-value branches, no phase-leak tokens).

The only omitted verification is a live end-to-end `cdb info` call against a real node — but CONTEXT.md D-16 explicitly defers live-node testing to Phase 130 (VERI-06), since Phase 127 is protocol-breaking and the live node at 192.168.1.73 remains on the pre-v4.2.0 wire format until a coordinated redeploy.

### Gaps Summary

No gaps. All 5 requirements (NODEINFO-01..04, VERI-02) are structurally satisfied. Encoder, decoder, and tests are wired and verified. Three info-level anti-patterns noted (stale test comment, max-boundary burst omission, zero-boundary redundancy) are cosmetic and do not block the phase goal.

---

_Verified: 2026-04-22T15:00:00Z_
_Verifier: Claude (gsd-verifier)_
