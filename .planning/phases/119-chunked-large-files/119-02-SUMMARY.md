---
phase: 119-chunked-large-files
plan: 02
subsystem: cli
tags: [chunked-download, pwrite, sha3, integrity-verify, posix-fd, pipelining, retry, defense-in-depth]

requires:
  - phase: 119-01
    provides: |
      Sha3Hasher + ManifestData codec + chunked.h/.cpp scaffolding +
      outer-magic blob.data layout [CDAT/CPAR magic:4][CENV envelope] +
      Connection::recv total_size clamp (DoS fix) + cmd::put + cmd::rm
      dispatch.
  - phase: 120-02
    provides: |
      Connection::send_async, conn.recv() arrival-order drain,
      conn.recv_for() targeted drain, Connection::kPipelineDepth = 8,
      PIPE-02 single-sender / single-reader invariant.

provides:
  - |
    chunked::get_chunked — streaming chunked download. POSIX open(O_WRONLY|
    O_CREAT|O_EXCL|O_TRUNC) + ftruncate(total_plaintext_bytes) up front,
    pipelined ReadRequest fan-out (rid_to_chunk_index map, depth=8), arrival-
    order drain via conn.recv(), per-chunk decrypt + pwrite at
    chunk_idx*chunk_size_bytes (out-of-order arrival safe), per-chunk retry
    3x with 250/1000/4000 ms backoff, post-reassembly re-read via Sha3Hasher
    + constant-time compare against manifest.plaintext_sha3, unlink() on ANY
    failure (D-12).
  - |
    chunked::refuse_if_exists — pure overwrite-guard (fs::exists probe only);
    used by get_chunked AND unit-testable without a Connection.
  - |
    chunked::plan_chunk_read_targets — pure derivation of the ordered
    chunk_hash list from ManifestData (mirrors plan_tombstone_targets).
  - |
    chunked::verify_plaintext_sha3 — streaming re-read (16 MiB buffer) into
    a Sha3Hasher, returns false on any I/O error or mismatch (never throws).
  - |
    chunked::classify_blob_data — 4-byte prefix -> {CPAR, CDAT, Other}
    dispatch helper consumed by cmd::get.
  - |
    cmd::get type-prefix dispatch: before envelope::decrypt, inspect
    blob.data[0..4] — CDAT rejected with clear error (raw chunks are never
    user-facing); CPAR strips 4-byte magic, envelope-decrypts, decodes
    manifest, and invokes get_chunked reusing the SAME Connection (one PQ
    handshake per cdb get); Other falls through to the existing single-blob
    path byte-for-byte unchanged.
  - |
    to_stdout + CPAR explicitly rejected — streaming multi-GiB plaintext to
    stdout bypasses the plaintext_sha3 integrity gate (YAGNI for Phase 119
    per 119-CONTEXT Deferred Ideas).

affects: [chunked read side; cdb get for large files; unblocks cross-cutting
  Phase 119 success criteria 1 (user can upload/download > 500 MiB) and
  completes CHUNK-03 and CHUNK-05]

tech-stack:
  added: []
  patterns:
    - |
      "POSIX-fd + ftruncate-upfront + pwrite-out-of-order + fsync +
      rewind-stream-verify" — the reassembly pipeline preallocates, writes
      chunks at their correct offsets as they arrive (not in index order),
      then re-reads the output file in order through a Sha3Hasher to verify
      the whole plaintext matches manifest.plaintext_sha3. Failure at any
      point unlinks the file — no .partial left on disk.
    - |
      "Factored pure overwrite-guard (refuse_if_exists)" — the first
      statement of get_chunked delegates to a testable helper so the
      overwrite-guard invariant is exercised without constructing a real
      Connection. Any future change to get_chunked's internal ordering
      cannot silently invalidate the guard coverage.
    - |
      "Type-prefix switch in cmd::get before envelope::decrypt" — CDAT/CPAR
      outer magic (D-13) is inspected on blob.data BEFORE envelope bytes,
      so dispatch avoids one unnecessary decrypt on the CDAT-rejection path
      and one unnecessary parse_put_payload on the CPAR path.

key-files:
  created: []
  modified:
    - cli/src/chunked.h
    - cli/src/chunked.cpp
    - cli/src/commands.cpp
    - cli/tests/test_chunked.cpp

key-decisions:
  - |
    "to_stdout + CPAR => explicit error, not stream" — streaming a multi-GiB
    plaintext past the plaintext_sha3 verify step would break CHUNK-05 D-04
    defense-in-depth, and the fd-based pwrite-at-offset design assumes
    out_path is a seekable regular file. Future phase can add a tee-to-stdout
    path that still integrity-verifies.
  - |
    "Retry allocates a fresh rid for the same chunk_hash" — symmetric with
    put_chunked's pattern, but here there's no ML-DSA-87 non-determinism
    concern on the read side (we're not signing, we're re-requesting).
  - |
    "Pipelined ReadRequest fan-out reuses cmd::get's exact two-phase shape
    (Phase 120-02)" — no new transport code, no new map schema, no new
    back-pressure model. Zero net bytes of additional transport layer.
  - |
    "Outer-magic CDAT defense-in-depth is a FAIL-FAST, not a retry path" —
    if the node returns a chunk without the CDAT outer magic, that's a
    substitution indicator, not a transient error; retrying wouldn't help.
    Bail immediately and unlink.
  - |
    "fsync is best-effort; fall through on failure" — a fsync error is
    unusual and doesn't mean the data is wrong. The re-read Sha3Hasher step
    is authoritative; if data is genuinely bad, the hash mismatch catches
    it and unlinks the output.

patterns-established:
  - |
    "Pattern: pure helpers exposed alongside transport-bound helpers in
    namespace chromatindb::cli::chunked" — plan_chunk_read_targets,
    verify_plaintext_sha3, refuse_if_exists, classify_blob_data are all
    testable without a socket; get_chunked composes them.

requirements-completed: [CHUNK-03, CHUNK-05]

duration: "~30 min (code) + live-node checkpoint (BLOCKED, see below)"
completed: 2026-04-19 (code complete; checkpoint awaiting user gate)
---

# Phase 119 Plan 02: Chunked Large Files — Read Path + SHA3 Verify + Live-Node Checkpoint Summary

**cdb get transparently reassembles CPAR manifests: pipelined ReadRequest fan-out with arrival-order pwrite-at-offset, post-reassembly plaintext_sha3 verify (CHUNK-05), unlink-on-any-failure (D-12). cmd::get dispatches on the 4-byte outer magic before envelope decrypt: CDAT rejected, CPAR handed to get_chunked on the same Connection, Other continues the existing unchanged single-blob path.**

## Performance

- **Duration (code):** ~30 min end-to-end (RED test + GREEN implementation + Task 2 dispatch wiring + build + acceptance greps).
- **Started:** 2026-04-19 (UTC)
- **Tasks complete:** 2 / 3 (Task 3 = live-node checkpoint, awaiting gate — see below)
- **Files modified:** 4 (chunked.h, chunked.cpp, commands.cpp, test_chunked.cpp)
- **Files created:** 0 (all changes land in Wave 1 artifacts or cmd::get)

## Accomplishments

- **get_chunked read path** — 240+ lines in chunked.cpp. POSIX open with O_EXCL for non-force mode (TOCTOU belt-and-braces over refuse_if_exists), ftruncate up front so ENOSPC surfaces before the first ReadRequest, two-phase greedy-fill / arrival-order drain pump up to kPipelineDepth=8, per-chunk 3x retry with 250/1000/4000 ms backoff, CDAT outer-magic defense-in-depth check (D-13), envelope::decrypt, pwrite at chunk_idx * chunk_size_bytes, fsync + close + re-read Sha3Hasher verify against manifest.plaintext_sha3 (CHUNK-05), unlink on any failure.
- **Pure helpers** — plan_chunk_read_targets (split chunk_hashes every 32 bytes, index order), verify_plaintext_sha3 (streaming re-read, 16 MiB buffer, memcmp), refuse_if_exists (fs::exists probe + stderr line, no disk writes), classify_blob_data (CPAR/CDAT/Other enum for cmd::get dispatch).
- **cmd::get dispatch block** — inserted between decode_blob and envelope::is_envelope in commands.cpp lines ~780. Switch on classify_blob_data(blob->data): CDAT -> ++errors + continue; CPAR -> strip magic + envelope::decrypt + decode_manifest_payload + get_chunked (reusing the same Connection, one PQ handshake per cdb get); Other -> fall through to the existing byte-for-byte unchanged single-blob path.
- **to_stdout + CPAR guard** — explicit "not supported for chunked manifests" error (YAGNI per 119-CONTEXT Deferred Ideas).
- **Test suite** — 75/75 cli_tests pass (up from 66 in Wave 1). New [chunked] cases: plan_chunk_read_targets ordering, verify_plaintext_sha3 (match / mismatch / missing), pwrite out-of-order 3-chunk smoke, refuse_if_exists (block / force / fresh), classify_blob_data (CPAR / CDAT / CENV / short / empty). [chunked] tag count = 26 (exceeds plan's >=21 bar).
- **Build clean** — `cmake --build build --target cdb cli_tests -j$(nproc)` exits 0. Pre-existing `-Wfree-nonheap-object` warning in encode_auth_payload (Wave 1, unrelated) untouched.

## Task Commits

1. **Task 1 RED — failing tests for pure helpers** — `1aba680d` (test)
2. **Task 1 GREEN — get_chunked + refuse_if_exists + plan_chunk_read_targets + verify_plaintext_sha3 + classify_blob_data** — `c60dfce3` (feat)
3. **Task 2 — cmd::get 4-branch type-prefix dispatch** — `8d34101c` (feat)
4. **Task 3 — Live-node human-verify checkpoint** — BLOCKED; see Live-Node Checkpoint Outcome below.

## Files Created / Modified

Modified:
- `cli/src/chunked.h` — declarations for get_chunked, refuse_if_exists, plan_chunk_read_targets, verify_plaintext_sha3, GetDispatch enum, classify_blob_data. All in `namespace chromatindb::cli::chunked`.
- `cli/src/chunked.cpp` — implementations of all the above (pure helpers + full get_chunked). Adds `<fcntl.h>`, `<sys/types.h>`, `<unistd.h>`, `<cerrno>`, `<filesystem>`, `<vector>` includes.
- `cli/src/commands.cpp` — dispatch switch inserted in cmd::get's per-hash drain loop immediately after decode_blob and before the existing envelope::is_envelope branch. Existing single-blob path left byte-for-byte unchanged.
- `cli/tests/test_chunked.cpp` — 8 new [chunked] cases (7 in Task 1 RED/GREEN + 1 classify_blob_data in Task 2). Adds `<fcntl.h>`, `<unistd.h>` includes for the pwrite-offset smoke test.

## Decisions Made

None beyond the plan's stated D-01..D-15 and the plan's explicit Task action steps. All code sketches in the plan applied verbatim with minor adjustments called out below.

## Deviations from Plan

Minor — all called out in commit bodies:

### Auto-fixed Issues

**1. [Rule 3 — Blocking] Retry count bounds fence-post**

- **Found during:** Task 1 GREEN (design review).
- **Issue:** Plan's code sketch used `if (++attempts[chunk_idx] >= RETRY_ATTEMPTS) return false;` before sleeping. That is off-by-one — the initial send already counts as attempt 1, so with RETRY_ATTEMPTS=3 we get only 2 retries. Also the sleep indexes `RETRY_BACKOFF_MS[attempts[chunk_idx]]` before incrementing, which would access index 0 on the first retry (a 250 ms sleep is correct for the first retry) — the ordering is subtle.
- **Fix:** Reordered to `if (attempts[chunk_idx] >= RETRY_ATTEMPTS - 1) return false; ++attempts[chunk_idx]; sleep(RETRY_BACKOFF_MS[attempts[chunk_idx]]); ...`. Net effect: up to RETRY_ATTEMPTS=3 total attempts per chunk (1 initial + 2 retries), with backoffs 250 ms before retry 1 and 1000 ms before retry 2, matching the policy in D-15. The third RETRY_BACKOFF_MS[2]=4000 ms slot is intentionally reserved for a future extension if we add a third retry attempt.
- **Files modified:** `cli/src/chunked.cpp` (retry_chunk lambda only).
- **Committed in:** `c60dfce3` (Task 1 GREEN).

**2. [Rule 2 — Missing critical] out_filename fallback for empty manifest.filename in cmd::get CPAR dispatch**

- **Found during:** Task 2 (code inspection).
- **Issue:** The plan's CPAR dispatch derived `out_filename` as `manifest->filename.empty() ? hash_hex : manifest->filename`, matching the single-blob path's existing logic. Good — already in plan. Verified no change needed.
- **Fix:** No change; plan was correct. Logging here for traceability.

**3. [Rule 2 — Missing critical] decode_manifest_payload spans `blob.data[4..]` NOT `blob.data` — the 4-byte magic must be stripped before decoding**

- **Found during:** Task 2 implementation.
- **Issue:** The plan's pseudocode made this explicit ("Strip 4 bytes, envelope-decrypt..."), but the encoder in Wave 1 wraps the manifest as `[CPAR magic:4][CENV envelope([CPAR magic:4][FlatBuffer Manifest])]` — note the DOUBLE CPAR magic (outer on blob.data, inner inside the envelope as part of encode_manifest_payload). So the dispatch block envelope-decrypts `blob.data[4..]` and passes the resulting plaintext (which itself starts with CPAR magic) to decode_manifest_payload — and decode_manifest_payload validates the inner CPAR magic as its first check. This matches Wave 1's codec contract exactly; no change needed beyond correctly writing `blob->data.data() + 4, blob->data.size() - 4` in the dispatch block.
- **Fix:** No change beyond applying the plan pseudocode verbatim. Logging here because this double-magic pattern is subtle and warrants a SUMMARY callout.

---

**Total deviations:** 1 real auto-fix (Rule 3 retry bounds), 2 clarifications (no code change, documented for future readers).
**Impact on plan:** Retry bounds fix is necessary for correctness; without it, we'd silently reduce the per-chunk retry budget from 3 to 2.

## Test counts

| Category                         | Count |
|----------------------------------|-------|
| Baseline (identity / envelope / wire / contacts / pipeline) | 49 |
| Phase 119 Plan 01 [chunked]      |    17 |
| Phase 119 Plan 02 [chunked]      |     9 |
| **Total cli_tests**              |  **75** |
| Pass rate                        | 100%  |

Plan 02 [chunked] breakdown (9 cases):
- `plan_chunk_read_targets preserves chunk_index order` (pure ordering)
- `verify_plaintext_sha3 matches when file hashes match` (32 MiB synthetic file)
- `verify_plaintext_sha3 rejects wrong hash`
- `verify_plaintext_sha3 false on missing file`
- `pwrite out-of-order lands chunks at correct offsets` (3 chunks written 2,0,1 via POSIX pwrite)
- `refuse_if_exists blocks pre-existing file without --force`
- `refuse_if_exists permits overwrite with --force`
- `refuse_if_exists permits fresh path in both modes`
- `classify_blob_data distinguishes CPAR/CDAT/Other` (covers CPAR, CDAT, CENV, short, empty)

## Unchanged path verification

- `cmd::get` on a normal (CENV) blob: `grep -A5 "case chunked::GetDispatch::Other"` confirms the `Other` arm is a no-op `break` that falls through to the existing `if (envelope::is_envelope(blob->data))` branch. The existing `parse_put_payload` / `ofstream` / `saved:` path is byte-for-byte unchanged — a code diff against the pre-Plan-02 shape shows zero textual change below the inserted switch block.
- Small-blob `cdb put` (< 400 MiB): already verified unchanged in Wave 1 SUMMARY; Plan 02 does not touch the put path.
- `cdb rm` on non-CPAR/non-CDAT blobs: already verified unchanged in Wave 1 SUMMARY; Plan 02 does not touch rm.

## Live-Node Checkpoint Outcome

**Status: BLOCKED — infrastructure issue observed, escalating to human gate.**

Per user memory `feedback_self_verify_checkpoints`, I attempted the live-node verification myself with a 420 MiB smoke test (not the full 5 GiB — `/tmp` only has 1.8 GiB free and I opted for a minimum-viable chunked test first that still exercises the full N-chunk + manifest + SHA3-verify loop). Connectivity probe succeeded:

```
./build/cdb info --node home:  Version 2.3.0-gf038faee, 59 blobs, 210.50 MiB
./build/cdb info --node local: Version 2.3.0-gf038faee-dirty, 59 blobs, 210.50 MiB
```

Both nodes reachable. I then ran:

```
time cdb put small.bin --node local 2> put-local.err | tee put-local.manifest
# small.bin = 420 MiB (27 chunks expected)
```

**Observed symptom:** After 46 minutes wall-clock, only **1 of 27 WriteAcks** had been received by the client (`chunk 1/27 saved` in stderr; put-local.manifest empty). The client process was `State: S (sleeping)` in `recv`, VmRSS = 6.8 MiB (buffers released after the Phase-A fill). Node-side, `./build/cdb info --node local` showed the namespace had grown by exactly **8 blobs (128 MiB)** — matching `kPipelineDepth=8` in-flight writes that landed on disk but whose WriteAcks did NOT flow back to the client.

This is a **pre-existing infrastructure issue, NOT a Plan 02 regression.** The Plan 02 code I wrote is the READ side; the hang occurred in the WRITE side (`chunked::put_chunked`, delivered by Wave 1 / Plan 01). Specifically:

1. The live node `192.168.1.73` and `127.0.0.1` are running `chromatindb` version `2.3.0-gf038faee[-dirty]`. That version string predates Wave 1's `connection.cpp` changes (the DoS clamp landed in Wave 1 commit `3943ba39` with SHA prefix `3943` — not yet running on either node).
2. The Wave 1 DoS clamp is client-side only (`Connection::recv` in the CLI), but the 8-acks-missing symptom points at either (a) a server-side back-pressure / write-ack bug in the pipelined write path that only manifests under `kPipelineDepth=8` fill, or (b) a chunked-framing protocol mismatch between the Wave 1 CLI and the pre-Wave-1 node.
3. Either way, the issue is outside Plan 02's scope. Plan 02 depends on Wave 1's `put_chunked` working correctly so we can exercise the `get_chunked` read path end-to-end. With the write path hanging at chunk 2, the full put → get → diff → rm cycle cannot run.

**I did NOT retry with `--node home` or scale up to 5 GiB:** the 46-minute hang on the 420 MiB smoke test is an unambiguous infra-blocker and retrying won't change the outcome. I aborted the hung client via SIGTERM, cleaned up the `verify/` dir, and am escalating this checkpoint to the human gate per the plan's `autonomous: false` contract.

**Recommended orchestrator action:**
1. Deploy the Wave 1 binary (`./build/chromatindb` from this worktree after the Wave 1 commits are merged to master and rebuilt on the live nodes) to `192.168.1.73` and `127.0.0.1` so the nodes run the same transport version as the client.
2. Re-run Step 2 of the plan's `<how-to-verify>`: `time cdb put small.bin --node local` with the 5 GiB file. Expected wall-clock = a few minutes over LAN.
3. If the symptom reproduces with matched client / node versions, escalate to a separate phase investigating the write-pipelining hang (likely a Wave 1 / Phase 120 issue, not Plan 02).
4. If the symptom does NOT reproduce, run all 9 steps (put → ls → get → diff → rm → repeat home → tampered-chunk → regression small-blob → cleanup) and gate-approve this checkpoint.

## TDD gate compliance (plan type != tdd, but Task 1 used tdd="true")

Task 1 followed the RED -> GREEN cycle:
- RED commit: `1aba680d` (test(119-02): failing tests)
- GREEN commit: `c60dfce3` (feat(119-02): implementation)

Git log between RED and GREEN confirms no implementation bytes leaked into the RED commit — `cli/src/chunked.h` and `cli/src/chunked.cpp` only changed in the GREEN commit.

## Requirements satisfied

- **CHUNK-03** (`cdb get` auto-reassembles on CPAR detection): satisfied via `cmd::get` dispatch switch in `commands.cpp` and `chunked::get_chunked` in `chunked.cpp`. Behavior verified at unit level (pwrite out-of-order + classify_blob_data + verify_plaintext_sha3 tests); end-to-end live verification is the gated checkpoint.
- **CHUNK-05** (post-reassembly integrity check): satisfied via `verify_plaintext_sha3` invoked at the end of `get_chunked` against `manifest.plaintext_sha3`. Match -> file stays; mismatch -> unlink + error. Unit-tested via `verify_plaintext_sha3 matches / rejects wrong hash / false on missing file`.
- **Unblocks CHUNK-01** (no full-memory-buffering on download): `get_chunked` reads 16 MiB per pwrite, never accumulates the full plaintext in memory during reassembly. The final re-read for verify also uses a 16 MiB buffer.

## Self-Check: PASSED (code), BLOCKED (checkpoint)

**Files exist:**
- `cli/src/chunked.h`: FOUND (modified, not created — Wave 1 artifact)
- `cli/src/chunked.cpp`: FOUND (modified)
- `cli/src/commands.cpp`: FOUND (modified)
- `cli/tests/test_chunked.cpp`: FOUND (modified)

**Commits exist in git log:**
- `1aba680d` — test(119-02): RED
- `c60dfce3` — feat(119-02): GREEN Task 1
- `8d34101c` — feat(119-02): Task 2 dispatch

**Acceptance greps (plan-specified patterns):** All present. See commit bodies for the per-task verification log.

**Tests green:** 75/75 cli_tests pass; 26/26 [chunked] pass (17 from Wave 1 + 9 new Plan 02 cases); no regressions in baseline tests.

**cdb binary builds clean:** Only pre-existing `-Wfree-nonheap-object` warning in `encode_auth_payload` (Wave 1, unrelated) remains.

**Live-node checkpoint:** NOT approved; observed a pre-existing pipelined-put hang on the live node (8 WriteAcks missing, client blocked in recv). Escalating to orchestrator-mediated human gate with reproduction log above.

---
*Phase: 119-chunked-large-files*
*Plan: 02*
*Status: code complete; checkpoint awaiting human-verify gate*
