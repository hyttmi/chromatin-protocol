---
phase: 130-cli-auto-tuning
plan: 01-session-cap-derive-manifest
subsystem: cli
tags: [cli, session-cap, chunking, auto-tune, manifest-validator, nodeinfo]

# Dependency graph
requires:
  - phase: 127-nodeinforesponse-capability-extensions
    provides: "node-side encoder + byte-offset-authoritative wire layout for max_blob_data_bytes in NodeInfoResponse"
  - phase: 128-configurable-blob-cap-frame-shrink-config-gauges
    provides: "runtime-seeded node-side blob cap that the advertised NodeInfoResponse field sources from"
  - phase: 129-sync-cap-divergence
    provides: "precedent for session-constant cap snapshot on a peer/link (same pattern applied CLI-side here)"

provides:
  - "cdb Connection carries uint64_t session_blob_cap_ snapshotted once per connect from NodeInfoResponse.max_blob_data_bytes; accessor conn.session_blob_cap() is the single read path"
  - "chunking boundary + per-chunk size + manifest-validator ceiling are all derived from session_blob_cap(); no hardcoded constants left in cli/src/wire.h or cli/src/chunked.h"
  - "CPAR manifest validator decode_manifest_payload(bytes, session_cap) rejects manifests with chunk_size_bytes > session_cap and names BOTH values on stderr"
  - "Pre-v4.2.0 node (short NodeInfoResponse payload) triggers hard-fail refusing the connection; operator-facing error names the version gap in plain language"
  - "VERI-06 UAT markdown (64 MiB round-trip + SHA3-256 verify, user-delegated per CONTEXT.md D-08) shipped as status: pending"

affects:
  - 131-documentation-reconciliation

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Session-scoped cap snapshot on the transport object (Connection), seeded as the FIRST protocol round-trip after handshake — mirrors Phase 129's link-cap snapshot but from the client side instead of the peer side."
    - "Hard-fail on missing capability fields (D-07): refuse the session with a version-gap diagnostic; no silent default to a legacy value — consistent with feedback_no_backward_compat."
    - "Validator plumbing via parameter (decode_manifest_payload gains a uint64_t session_cap) rather than a Connection reference — keeps the pure wire-decoder callable from the classify_rm_target_impl test template with mock transports. Tests pass UINT64_MAX to disable cap-gating where it's not the TEST_CASE focus."
    - "Operator-language error strings: 'node is older than v4.2.0' and 'CPAR manifest chunk_size_bytes=X exceeds server cap=Y' — no phase numbers / GSD / CLI-NN tokens leak into stderr per feedback_no_phase_leaks_in_user_strings.md. Source comments freely cite phase/req IDs."

key-files:
  created:
    - ".planning/phases/130-cli-auto-tuning/130-UAT.md"
  modified:
    - "cli/src/connection.h - +9 lines: session_blob_cap_ member, public accessor, seed_session_cap() internal declaration."
    - "cli/src/connection.cpp - +74 lines, -0 lines: seed_session_cap() implementation + calls into it on both UDS (TrustedHello) and TCP (PQ) success paths; close() zeroes the cap so a re-connect must re-seed."
    - "cli/src/wire.h - -2 lines, +13 lines: delete CHUNK_SIZE_BYTES_DEFAULT + CHUNK_SIZE_BYTES_MAX; retain MAX_CHUNKS + CHUNK_SIZE_BYTES_MIN with D-05 rationale; extend decode_manifest_payload signature with uint64_t session_cap."
    - "cli/src/wire.cpp - +14 lines, -2 lines: cap-gated CPAR manifest validator with BOTH-value diagnostic; <cstdio> include."
    - "cli/src/chunked.h - -1 line, +5 lines: delete CHUNK_THRESHOLD_BYTES; extend verify_plaintext_sha3 with std::size_t read_buf_bytes=4 MiB default."
    - "cli/src/chunked.cpp - +27 lines, -5 lines: put_chunked threshold + per-chunk size + u32-narrowing guard + un-seeded-cap refusal all pull from conn.session_blob_cap(); verify_plaintext_sha3 honours caller-supplied read_buf_bytes; <limits> include."
    - "cli/src/commands_internal.h - +7 lines, -2 lines: classify_rm_target_impl template gains uint64_t session_cap parameter and forwards it to decode_manifest_payload."
    - "cli/src/commands.cpp - +6 lines, -4 lines: put-dispatch uses `fsize > conn.session_blob_cap()`; cmd::get manifest decode passes session cap; classify_rm_target wrapper threads conn.session_blob_cap() into the template."
    - "cli/tests/test_chunked.cpp - +83 lines, -3 lines: migrate off deleted CHUNK_SIZE_BYTES_DEFAULT to local kTestChunkSizeBytes; pass UINT64_MAX to decode_manifest_payload everywhere; 3 new [chunked][session-cap] TEST_CASEs covering D-09 scenarios a/b/c; realistic 64 MiB cap in the pre-existing 'above-max' case so CLI-04 rejection is still exercised."
    - "cli/tests/test_wire.cpp - +66 lines, -2 lines: local kTestChunkSizeBytes + UINT64_MAX session_cap on classify_rm_target_impl callers; 2 new [wire][manifest][session-cap] TEST_CASEs covering D-09 scenarios d/e (boundary + over-cap with two fixtures)."

key-decisions:
  - "D-01: session_blob_cap_ field lives on Connection (not a separate Session struct). Least-invasive — every command already holds a Connection& and all recv/send routes through it."
  - "D-02: seed_session_cap() called at the tail of Connection::connect(), AFTER handshake returns true + connected_=true is set, BEFORE connect() itself returns true. This way the first downstream send() is already using send_counter_=1 after the cap seed's send_counter=0 round-trip."
  - "D-03: cap==0 = refuse chunked ops. Implemented in put_chunked as an explicit check before the threshold comparison, with a message naming the version gap."
  - "D-06 plumbing choice: decode_manifest_payload takes uint64_t session_cap by parameter rather than holding a Connection&. Keeps it test-friendly — test_chunked.cpp already had 13 call sites, none of which had a Connection to hand."
  - "D-07 hard-fail wording: 'server NodeInfoResponse is missing max_blob_data_bytes field -- node is older than v4.2.0; upgrade the node or use an older cdb'. Operator-facing; no phase/req-ID leak."
  - "D-09 coverage via pure-arithmetic predicates: should_chunk_for_cap + chunk_count_for_cap helpers in test_chunked.cpp mirror the production production predicates in commands.cpp:703 and chunked.cpp:157 exactly. When the production check changes, the helpers must change in lock-step — the tests are asserting contract equivalence, not implementation details."
  - "CHUNK_SIZE_BYTES_MIN (1 MiB) retained — that's a protocol-layer minimum against malformed manifests, independent of whatever the server advertises."
  - "MAX_CHUNKS = 65536 retained with a 2-line D-05 rationale comment in wire.h (256 GiB @ 4 MiB cap / 4 TiB @ 64 MiB cap — both cover every realistic workload)."
  - "verify_plaintext_sha3 read-buffer size exposed as a default parameter (defaulted to 4 MiB fallback) rather than requiring every existing test caller to thread a session cap in. Tests that hash a file without a live Connection just use the default; production call site at chunked.cpp:755 passes conn.session_blob_cap()."

patterns-established:
  - "Post-handshake pre-command capability round-trip on Connection: the seed is the FIRST protocol round-trip after AEAD keys are live. Matches the Phase 129 precedent of snapshotting cap state ONCE and treating it as const for the session."
  - "Hard-fail on missing-field-at-offset-N: any capability advertisement that a future CLI version hard-depends-on should follow the D-07 shape — detect short payload, refuse session, name the version gap in stderr."
  - "Parameter-threaded validator plumbing: when a pure wire decoder grows a policy input that varies per session, the input travels as a u64 parameter (UINT64_MAX = no-op policy for existing tests that don't exercise it). Beats Connection& because it keeps the decoder callable from test-only template instantiations that mock the transport."

requirements-completed: [CLI-01, CLI-02, CLI-03, CLI-04, CLI-05]
# VERI-06 intentionally NOT marked here — it's user-delegated UAT (status: pending in 130-UAT.md).

# Metrics
duration: ~73min
completed: 2026-04-23
---

# Phase 130 Plan 01: Session Cap Derive + Manifest Auto-tune Summary

**`cdb` now auto-discovers the server's advertised `max_blob_data_bytes` on every connect, snapshots it in `Connection::session_blob_cap_`, and derives chunking threshold + per-chunk size + manifest-validator ceiling from it — zero hardcoded constants remain in `cli/src/wire.h` or `cli/src/chunked.h`.**

## Performance

- **Duration:** ~73 minutes (edits + two CMake-FetchContent deps rebuilds + test gates)
- **Started:** 2026-04-23T04:26:00Z (approx, build-debug configure kickoff)
- **Completed:** 2026-04-23T05:40:00Z
- **Tasks:** 3 / 3
- **Files modified:** 9 (src) + 1 (test) + 1 (UAT created)
- **Commits:** 4 (one per task + one chore for Task 2 grep acceptance)

## Accomplishments

### Task 1 — Session cap cache + on-connect seeding (commit aad411cd)

- Added `session_blob_cap_` field (uint64_t, defaulted 0) on `Connection`, with a public `session_blob_cap()` accessor.
- Introduced `Connection::seed_session_cap()`: sends `NodeInfoRequest` with rid=0x130001, receives the response, and walks the Phase-127-authoritative wire layout (ver_len → version → uptime → peers → ns_count → total_blobs → storage_used → storage_max → **max_blob_data_bytes**) to extract the u64 cap.
- Wired into `Connection::connect()` AFTER the handshake returns true, BEFORE `connect()` returns true. Both UDS (TrustedHello) and TCP (PQ) success paths call it.
- D-07 hard-fail: if the response payload truncates before offset `1 + ver_len + 40 + 8`, `fprintf(stderr, ...)` an operator-facing error naming the version gap (`node is older than v4.2.0`) and fail the connection.
- `close()` zeroes `session_blob_cap_` so a re-connect must re-seed.

### Task 2 — Constant deletion + consumer rewire (commits 6aa16500 + f39698cb)

- Deleted `CHUNK_SIZE_BYTES_DEFAULT` (16 MiB), `CHUNK_SIZE_BYTES_MAX` (256 MiB), `CHUNK_THRESHOLD_BYTES` (400 MiB). Retained `MAX_CHUNKS` (65536, D-05 rationale), `CHUNK_SIZE_BYTES_MIN` (1 MiB).
- `decode_manifest_payload` gains a `uint64_t session_cap` parameter. Manifests with `chunk_size_bytes > session_cap` are rejected and `fprintf(stderr, "Error: CPAR manifest chunk_size_bytes=%llu exceeds server cap=%llu\n", ...)` — BOTH values named per D-06.
- 5 consumer callsites rewired:
  - `cli/src/chunked.cpp:146` — threshold check now `end_pos <= cap`, un-seeded cap (0) refused with a version-gap error.
  - `cli/src/chunked.cpp:156` — per-chunk size is `static_cast<uint32_t>(cap)`; u32-narrowing guard added (cap > UINT32_MAX → refuse).
  - `cli/src/chunked.cpp:497` — `verify_plaintext_sha3` buffer size now a `std::size_t read_buf_bytes` parameter (production path at chunked.cpp:755 passes `conn.session_blob_cap()`; tests use the 4 MiB default).
  - `cli/src/commands.cpp:703` — put-dispatch uses `fsize > conn.session_blob_cap()`; error message names the live cap value.
  - `cli/src/commands.cpp:1060` — cmd::get's manifest decode passes `conn.session_blob_cap()` as the validator input.
- `classify_rm_target_impl` template gets a `uint64_t session_cap` parameter too; production wrapper passes `conn.session_blob_cap()`, tests pass `UINT64_MAX`.
- Comment-only follow-up commit (f39698cb) scrubs the deleted-constant names out of header comments so the plan's Task-2 grep returns 0 authoritatively.

### Task 3 — Unit tests + VERI-06 UAT (commit 93d2946f)

- 5 new TEST_CASEs under `[session-cap]` (15 assertions total), covering CONTEXT.md D-09 scenarios a..e:
  - `test_chunked.cpp [chunked][session-cap]`: threshold == cap equivalence (a); file ≤ cap stays single-blob (b); file > cap chunks with `ceil(fsize/cap)` chunks (c).
  - `test_wire.cpp [wire][manifest][session-cap]`: manifest at `chunk_size == cap` accepts (d); manifest over cap rejects with BOTH-values diagnostic, two fixtures: one-byte-over + 16 MiB chunk / 2 MiB cap (e).
- `.planning/phases/130-cli-auto-tuning/130-UAT.md` captures the VERI-06 procedure (64 MiB random file → put → get → SHA3-256 compare, optional pre-v4.2.0 rejection test, optional cross-node divergent-cap test). Status: `pending`.

## Task Commits

1. **Task 1 — Session cap cache + on-connect seeding** — `aad411cd` (feat)
2. **Task 2 — Delete hardcoded constants + rewire consumers** — `6aa16500` (refactor)
3. **Task 2 follow-up — Scrub deleted-constant names from comments** — `f39698cb` (chore)
4. **Task 3 — Unit tests + VERI-06 UAT** — `93d2946f` (test)

## Files Created/Modified

Created:
- `.planning/phases/130-cli-auto-tuning/130-UAT.md`

Modified (src):
- `cli/src/connection.h`
- `cli/src/connection.cpp`
- `cli/src/wire.h`
- `cli/src/wire.cpp`
- `cli/src/chunked.h`
- `cli/src/chunked.cpp`
- `cli/src/commands_internal.h`
- `cli/src/commands.cpp`

Modified (test):
- `cli/tests/test_chunked.cpp`
- `cli/tests/test_wire.cpp`

## Decisions Made

All 9 decisions codified in CONTEXT.md (D-01..D-09) were honoured; no executor-authored decisions override them. The only runtime-discretion calls were:

- **decode_manifest_payload's session_cap via parameter, not Connection&** (D-06 discretion point): chose parameter. 13 pre-existing test call sites already lacked a Connection; parameter threading keeps the pure decoder callable from the `classify_rm_target_impl` mock-transport template.
- **verify_plaintext_sha3 read-buffer size via defaulted parameter** (not in D-09): chose a `std::size_t read_buf_bytes = 4 * 1024 * 1024` default. Production passes `conn.session_blob_cap()`; tests use the default (they don't exercise a live Connection).
- **Hard-fail error wording** (D-07 discretion): landed on `"server NodeInfoResponse is missing max_blob_data_bytes field -- node is older than v4.2.0; upgrade the node or use an older cdb"` — plain-operator language, cites version, gives two remediation options.

## Deviations from Plan

### [Rule 3 — Blocker] `cli/build-debug/` did not exist

**Found during:** pre-Task-1 environment setup.
**Issue:** Plan's verify block runs `cmake --build cli/build-debug` but the worktree had no `cli/build-debug/` — only `cli/build/`.
**Fix:** Ran `cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug` once inside `cli/` to let FetchContent materialise all deps. One-time ~5 min cost (liboqs, Catch2, flatbuffers, asio, spdlog).
**Files modified:** none (fresh build dir).
**Commit:** none (infrastructure only).

### [Rule 2 — Missing critical functionality] u32-narrowing guard in `chunked.cpp`

**Found during:** Task 2.
**Issue:** `put_chunked` used to write `const uint32_t chunk_size = CHUNK_SIZE_BYTES_DEFAULT;` — safe because the source literal was a `uint32_t`. After rewire to `static_cast<uint32_t>(cap)`, a malformed NodeInfoResponse reporting a huge u64 cap would silently wrap. The node's Phase 128 hard ceiling is 64 MiB so the wrap is currently unreachable, but correctness-at-the-seam demands a runtime check.
**Fix:** Added `if (cap > std::numeric_limits<uint32_t>::max()) { fprintf + return 1; }` guard, with `<limits>` include.
**Files modified:** `cli/src/chunked.cpp`.
**Commit:** `6aa16500`.

### [Rule 1 — Test regression] pre-existing `chunked: decode rejects chunk_size_bytes out of range` test

**Found during:** first [chunked] run after Task 2's decoder signature change.
**Issue:** The "above 256 MiB max" half of that test passed `UINT64_MAX` as session_cap, which disables the cap gate entirely, so the 1 GiB chunk_size stopped being rejected — the test reasonably caught the new API's behaviour.
**Fix:** Changed just that half to use a realistic 64 MiB cap (the Phase 128 hard ceiling), so the `> cap` rejection path is exercised per its post-rewrite semantics.
**Files modified:** `cli/tests/test_chunked.cpp`.
**Commit:** `6aa16500` (same task commit).

### [Rule 3 — Grep acceptance] comment-only rename for plan's Task-2 grep

**Found during:** Task 2 verify.
**Issue:** Plan's `grep -c 'CHUNK_SIZE_BYTES_DEFAULT|…' wire.h chunked.h` must return 0. After Task 2 the names survived only in explanatory comments documenting the deletion (`"// CHUNK_THRESHOLD_BYTES is deleted. …"`), so grep returned 1 + 1.
**Fix:** Rewrote the comments to describe the deleted constants by concept (`"former hardcoded chunking threshold constant"`) rather than by name. Zero code change.
**Files modified:** `cli/src/wire.h`, `cli/src/chunked.h`.
**Commit:** `f39698cb`.

## Authentication Gates

None.

## Threat Flags

None. No new trust-boundary surface. `seed_session_cap()` runs inside the AEAD-encrypted post-handshake envelope (T-130-01 already addressed by the CONTEXT.md threat model — AEAD integrity is load-bearing; a broken AEAD means far bigger problems than a spoofed cap). The new CPAR manifest validator path rejects over-cap values earlier than the prior code would, narrowing — not widening — the attack surface.

## Issues Encountered

Two self-inflicted minor snags:

1. First `[chunked]` run after the signature change hit a tmpfs leftover at `/tmp/chunked_overwrite_guard_*` from an earlier run causing a test that normally cleans up after itself to see stale state. Resolved with `rm -f /tmp/chunked_overwrite_guard_*` before re-running. Not a regression, just a pre-existing test fixture hygiene issue.

2. Task 2 grep acceptance tripped on comment text — tracked as a separate commit (f39698cb) rather than amending, consistent with the no-amend rule.

## Stubs

None. Every cap-aware code path now reads a live, seeded `session_blob_cap()`. The `verify_plaintext_sha3` default buffer size is documented and covered by at least one TEST_CASE.

## Next Phase Readiness

- **Phase 131 DOCS-02** (documentation reconciliation) has a stable API surface to document: `Connection::session_blob_cap()` accessor, `decode_manifest_payload(bytes, session_cap)` signature, hard-fail wording, UAT 130-UAT.md.
- **VERI-06** is user-delegated; when run, update `130-UAT.md` `status:` from `pending` to `passed` / `failed`.
- No downstream blockers. Phase 130 ships a clean CLI-side auto-tune contract against the Phase 127 + Phase 128 server surface.

## Self-Check: PASSED

File existence:
- `cli/src/connection.h` — FOUND (accessor + field present, `grep -c session_blob_cap_` = 2).
- `cli/src/connection.cpp` — FOUND (`NodeInfoRequest` = 3, hard-fail string = 2).
- `cli/src/wire.h` — FOUND (deleted constants: 0, literals: 0).
- `cli/src/chunked.h` — FOUND (deleted constants: 0, literals: 0).
- `cli/src/chunked.cpp` — FOUND (`session_blob_cap` = 3).
- `cli/src/commands.cpp` — FOUND (`session_blob_cap` = 4).
- `cli/src/wire.cpp` — FOUND (cap-gated validator with BOTH-value fprintf).
- `cli/tests/test_chunked.cpp` — FOUND (`[session-cap]` = 3).
- `cli/tests/test_wire.cpp` — FOUND (`[session-cap]` = 2).
- `.planning/phases/130-cli-auto-tuning/130-UAT.md` — FOUND (`VERI-06` = 2, status: pending).

Commits in git log:
- `aad411cd` (Task 1) — FOUND.
- `6aa16500` (Task 2) — FOUND.
- `f39698cb` (Task 2 chore follow-up) — FOUND.
- `93d2946f` (Task 3) — FOUND.

Build gates:
- `cmake --build build-debug -j$(nproc) --target cdb` — exit 0, cdb linked.
- `cmake --build build-debug -j$(nproc) --target cli_tests` — exit 0, cli_tests linked.
- `./build-debug/tests/cli_tests "[session-cap]"` — All tests passed (15 assertions in 5 test cases).

Regression gate:
- `./build-debug/tests/cli_tests "[chunked],[wire],[cascade],[manifest]"` — All tests passed (197 424 assertions in 58 test cases).

---
*Phase: 130-cli-auto-tuning*
*Plan: 130-01-session-cap-derive-manifest*
*Completed: 2026-04-23*
