---
phase: 119-chunked-large-files
plan: 01
subsystem: cli
tags: [flatbuffers, liboqs, sha3, ml-dsa-87, envelope, pipelining, streaming-io, chunked-upload, cascade-delete]

requires:
  - phase: 120-request-pipelining
    provides: Connection::send_async + conn.recv arrival-order drain + kPipelineDepth = 8
  - phase: 117-blob-type-indexing-ls-filtering
    provides: 4-byte outer blob.data type prefix + type_label + is_hidden_type conventions

provides:
  - CPAR_MAGIC (0x43 0x50 0x41 0x52) + type_label/is_hidden_type updates (CPAR visible, CDAT hidden)
  - Sha3Hasher RAII class for streaming whole-file SHA3-256 over OQS incremental API
  - ManifestData struct + encode_manifest_payload + decode_manifest_payload (hand-coded FlatBuffers matching blob_vt style, full validation of every schema invariant)
  - chunked::put_chunked free function — streaming 16 MiB read + envelope + sign + pipelined send_async + arrival-order drain + per-chunk 3x retry + drain-time chunk_index -> final_hash binding + manifest assembly and ship
  - chunked::rm_chunked free function — plan_tombstone_targets-driven cascade delete (chunks-first, manifest-last per D-09)
  - Transport-level DoS fix in Connection::recv — clamps chunked-framing total_size against MAX_FRAME_SIZE before vector::reserve (STATE.md pre-existing flag)
  - cmd::put dispatcher for files in [400 MiB, 1 TiB]; >1 TiB rejected up-front
  - cmd::rm dispatcher with type-prefix check (CDAT error, CPAR cascade, other unchanged)

affects: [119-02 read path (reassembly + plaintext_sha3 verification + live-node checkpoint)]

tech-stack:
  added: []
  patterns:
    - "Hand-coded FlatBuffers codec with named VTable offset namespace (manifest_vt alongside blob_vt/transport_vt) — no flatc in the CLI build"
    - "RAII PIMPL wrapper over liboqs incremental SHA3 state (Sha3Hasher::Impl hides OQS_SHA3_sha3_256_inc_ctx from the header)"
    - "Phase 120-02 two-phase pipelined pump reused verbatim: Phase A greedy-fill up to Connection::kPipelineDepth, Phase B drain via conn.recv() in arrival order with batch-local rid_to_chunk_index map"
    - "Outer-magic blob.data layout [TYPE magic:4][CENV envelope] — Phase 117 type index sees the role without decrypting the payload"
    - "Pure plan helper (plan_tombstone_targets) returning a deterministic target order so the cascade order is unit-testable without a socket"

key-files:
  created:
    - cli/src/chunked.h
    - cli/src/chunked.cpp
    - cli/tests/test_chunked.cpp
  modified:
    - cli/src/wire.h
    - cli/src/wire.cpp
    - cli/src/commands.cpp
    - cli/src/main.cpp
    - cli/src/connection.cpp
    - cli/CMakeLists.txt
    - cli/tests/CMakeLists.txt

key-decisions:
  - "Outer-magic placement (blob.data = [CDAT or CPAR magic:4][CENV envelope]) locked; Phase 117 type indexing keeps working without server-side changes"
  - "CPAR visible by default in cdb ls (not in is_hidden_type), CDAT stays hidden — manifest is the user-facing handle"
  - "Hand-coded manifest_vt matching blob_vt style; no flatc invocation added to the CLI build (project convention)"
  - "One timestamp + one ttl captured at top of put_chunked, applied uniformly to every CDAT and the CPAR manifest (P-119-05 mitigation)"
  - "chunk_index -> final blob_hash bound at drain time (post-WriteAck), never at send time — accommodates ML-DSA-87 non-determinism so retries produce fresh signatures/hashes (D-15)"
  - "cmd::rm pays a one-round-trip pre-flight ReadRequest + decode_blob to inspect the 4-byte outer prefix before dispatching; accepted cost vs. adding a new wire-protocol message (would violate D-08 dumb-DB principle)"
  - "rid_counter variable in cmd::rm replaces hardcoded rid=1/rid=2; the newly inserted pre-flight ReadRequest needs its own rid and collisions must be avoided"
  - "Transport DoS clamp (total_size > MAX_FRAME_SIZE → return nullopt + spdlog::warn) matches the existing single-frame clamp at connection.cpp:282 — one consistent policy, no new constant"

patterns-established:
  - "Pattern 1: namespace chromatindb::cli::chunked header-only + free-function helpers exposed for both production (put_chunked/rm_chunked) and test (read_next_chunk, plan_tombstone_targets)"
  - "Pattern 2: D-09 cascade-delete ordering guaranteed by a pure plan function (plan_tombstone_targets) — testable without a socket"
  - "Pattern 3: per-operation retry loop inside a pipelined send path reads the source of truth (disk) afresh and re-signs, then resends on the same rid — bounded by RETRY_ATTEMPTS + RETRY_BACKOFF_MS"

requirements-completed: [CHUNK-01, CHUNK-02, CHUNK-04, CHUNK-05]

duration: "~45 min"
completed: 2026-04-19
---

# Phase 119 Plan 01: Chunked Large Files — Write Path, Cascade Delete, DoS Clamp Summary

**cdb put streams files up to 1 TiB as N × 16 MiB CDAT chunks + one CPAR manifest over the Phase 120 pipelining substrate; cdb rm walks the manifest and tombstones every chunk before the manifest (D-09); a pre-existing unchecked `total_size` -> `vector::reserve` in the transport chunked-reassembly path is clamped to MAX_FRAME_SIZE.**

## Performance

- **Duration:** ~45 min (including one full cmake configure from a cold worktree)
- **Started:** 2026-04-19 (UTC)
- **Completed:** 2026-04-19T05:53:27Z
- **Tasks:** 2
- **Files created:** 3 (chunked.h, chunked.cpp, test_chunked.cpp)
- **Files modified:** 7 (wire.h, wire.cpp, commands.cpp, main.cpp, connection.cpp, cli/CMakeLists.txt, cli/tests/CMakeLists.txt)

## Accomplishments

- Streaming chunked upload (`chunked::put_chunked`) with bounded working memory per D-11 — 16 MiB reusable read buffer + at most kPipelineDepth (8) encoded chunks in flight. ML-DSA-87 non-determinism handled by binding chunk_index → final blob_hash at drain time (D-15).
- Cascade-delete (`chunked::rm_chunked`) with chunks-first / manifest-last ordering guaranteed by the pure `plan_tombstone_targets` helper (D-09). Idempotent on retry per D-10.
- CPAR manifest wire codec implemented as a hand-coded FlatBuffers encoder in `cli/src/wire.cpp`, mirroring `blob_vt`; the decoder enforces every schema invariant called out in P-119-04 / T-119-06, closing the OOM-on-malicious-manifest DoS before the bytes ever reach the reassembly path.
- `Sha3Hasher` RAII class wraps the liboqs incremental SHA3-256 API — absorb plaintext in 16 MiB increments during the streaming read, finalize once for `plaintext_sha3` (CHUNK-05 defense-in-depth).
- Transport-level DoS clamp: `Connection::recv()` now rejects chunked-framing headers whose `total_size` exceeds `MAX_FRAME_SIZE` (110 MiB) before `vector::reserve`. Closes P-119-06 / T-119-06.
- `cmd::put` dispatcher: files in [400 MiB, 1 TiB] → `put_chunked`; >1 TiB → clean error before any transfer; <400 MiB → existing single-blob path **unchanged** (verified by reading commands.cpp — the `files.push_back` branch only runs below the threshold).
- `cmd::rm` dispatcher: pre-flight ReadRequest + 4-byte outer prefix check. CDAT → error (defense-in-depth). CPAR → envelope-decrypt + decode_manifest_payload + `rm_chunked`. Other types (CENV, PUBK, TOMB, DLGT) → existing single-tombstone path **unchanged**.
- Full test suite green: 66/66 cli_tests pass (49 baseline + 17 [chunked]).

## Task Commits

Each task followed the TDD cycle (RED → GREEN):

1. **Task 1 RED — failing tests for Sha3Hasher + Manifest codec + CPAR** — `ac951692` (test)
2. **Task 1 GREEN — CPAR_MAGIC, Sha3Hasher, ManifestData codec** — `4e7ff4d1` (feat)
3. **Task 2 RED — failing tests for chunked.h free functions** — `f83ebcd7` (test)
4. **Task 2 GREEN — chunked upload + rm cascade + DoS clamp** — `3943ba39` (feat)

_Plan metadata commit + worktree merge commit are added by the orchestrator after this summary is committed._

## Files Created/Modified

Created:
- `cli/src/chunked.h` — free-function helpers in `namespace chromatindb::cli::chunked`. Declares `put_chunked`, `rm_chunked`, `read_next_chunk`, `plan_tombstone_targets`; public constants `CHUNK_THRESHOLD_BYTES` (400 MiB), `MAX_CHUNKED_FILE_SIZE` (1 TiB), `RETRY_ATTEMPTS` (3), `RETRY_BACKOFF_MS` ({250, 1000, 4000}).
- `cli/src/chunked.cpp` — streaming upload with retry; cascade-delete driven by `plan_tombstone_targets`. Builds `[CDAT magic:4][CENV envelope]` chunk blobs and `[CPAR magic:4][CENV envelope([CPAR magic:4][FlatBuffer])]` manifest blobs (outer-magic layering). Signs every blob with a shared `timestamp` + `ttl` captured once at the top of `put_chunked` (P-119-05).
- `cli/tests/test_chunked.cpp` — 17 Catch2 cases tagged `[chunked]`: Sha3Hasher incremental-vs-one-shot equivalence (×2), manifest codec round-trip (2 chunks, 64 chunks), decoder rejection paths (missing magic, 0-chunk, oversized `segment_count`, truncated `chunk_hashes`, `chunk_size_bytes` out of range, version mismatch, total_plaintext_bytes overflow, plaintext_sha3 wrong size), type_label + is_hidden_type CPAR/CDAT classification, outer-magic placement round-trip (no network), `read_next_chunk` streaming (45 B / 16 B chunks → 16/16/13/0), `plan_tombstone_targets` ordering.

Modified:
- `cli/src/wire.h` — `CPAR_MAGIC` constant; `type_label` CPAR branch; doc comment on `is_hidden_type` explaining why CPAR is deliberately **not** in the hide-list; public constants `MANIFEST_VERSION_V1`, `MAX_CHUNKS`, `CHUNK_SIZE_BYTES_DEFAULT/MIN/MAX`; `Sha3Hasher` RAII class declaration; `ManifestData` struct + `encode_manifest_payload` + `decode_manifest_payload` declarations; `#include <memory>` for `std::unique_ptr`.
- `cli/src/wire.cpp` — `manifest_vt` namespace with VTable offsets 4/6/8/10/12/14/16 (one more slot than `blob_vt` to carry `filename`); `Sha3Hasher::Impl` wrapping `OQS_SHA3_sha3_256_inc_ctx` with a `finalized` flag so the destructor releases exactly once; `encode_manifest_payload` mirroring `encode_blob` (StartTable / AddElement / AddOffset / EndTable); `decode_manifest_payload` enforcing every schema invariant.
- `cli/src/commands.cpp` — `#include "cli/src/chunked.h"`; CPAR arm in the type-filter map at the existing CENV/PUBK/TOMB/DLGT/CDAT site with matching error-message update; `cmd::put` dispatches [400 MiB, 1 TiB] to `chunked::put_chunked` and rejects >1 TiB; `cmd::rm` gains a `rid_counter` variable replacing hardcoded rid=1/rid=2, runs a pre-flight ReadRequest + decode_blob, errors on CDAT targets, cascades CPAR targets, and falls through for all other types unchanged.
- `cli/src/main.cpp` — `--type` help text and error message list CPAR alongside CENV/PUBK/TOMB/DLGT/CDAT.
- `cli/src/connection.cpp` — `Connection::recv()` clamps chunked-framing `total_size` against `MAX_FRAME_SIZE` before `vector::reserve` (with `spdlog::warn`). Closes the pre-existing DoS STATE.md explicitly flagged for Phase 119.
- `cli/CMakeLists.txt` — `src/chunked.cpp` linked into the `cdb` executable.
- `cli/tests/CMakeLists.txt` — `test_chunked.cpp` + `../src/chunked.cpp` + `../src/connection.cpp` linked into `cli_tests` (connection.cpp was already required transitively by chunked.cpp's uses of `Connection::send_async` / `recv` / `recv_for` / `kPipelineDepth`).

## Decisions Made

None beyond the plan's explicit decisions (D-01..D-15) and the PATTERNS gap-filling choices called out in the plan's `<action>` steps. The plan and the PATTERNS document converged on one implementation per capability; executor applied them verbatim.

## Deviations from Plan

None materially. Minor adjustments to the plan's code sketches:

### Auto-fixed Issues

**1. [Rule 3 — Blocking] cli_tests binary also needs to link `../src/connection.cpp`**
- **Found during:** Task 2 (GREEN build — linker errors in `cli_tests`).
- **Issue:** `chunked.cpp` uses `Connection::send_async` / `recv` / `recv_for` / `kPipelineDepth`. The test binary wasn't linking `connection.cpp`, so the linker failed with undefined references. The plan's action step for `cli/tests/CMakeLists.txt` only listed `../src/chunked.cpp`; `connection.cpp` was implicit.
- **Fix:** Added `${CMAKE_CURRENT_SOURCE_DIR}/../src/connection.cpp` to the `cli_tests` source list in `cli/tests/CMakeLists.txt`.
- **Files modified:** `cli/tests/CMakeLists.txt`
- **Verification:** `cli_tests` links clean, all 66 tests pass.
- **Committed in:** `3943ba39` (Task 2 GREEN).

**2. [Rule 2 — Missing critical] `ifstream::seekg` after `read()` hit EOF requires `clear()`**
- **Found during:** Task 2 (GREEN design).
- **Issue:** On the retry path, `put_chunked` re-reads a file slice after a prior failed send. `std::ifstream::read()` sets `eofbit` when it hits the last short chunk; subsequent `seekg` on a stream with `eofbit` set silently does nothing on some implementations. Without `f.clear()` the second/third retry attempts would read nothing.
- **Fix:** Added `f.clear()` before every `seekg` in the greedy-fill Phase A (both the first-send path and the retry path).
- **Files modified:** `cli/src/chunked.cpp`
- **Verification:** Logic reviewed; read_next_chunk unit test covers the full/full/short/EOF pattern separately.
- **Committed in:** `3943ba39` (Task 2 GREEN).

---

**Total deviations:** 2 auto-fixed (1 blocking, 1 missing critical).
**Impact on plan:** Both necessary for correctness. No scope creep.

## Issues Encountered

- **Pre-existing warning in `encode_auth_payload`** (connection.cpp:60, `-Wfree-nonheap-object`) — out of scope for Phase 119; the plan's `<verification>` explicitly calls it out as pre-existing and untouched. Not fixed here.
- **Worktree started from an older base commit** (`f038faee`). The `<worktree_branch_check>` step reset to the correct base (`615dd805`) before any work. No loss — the branch had no unique commits.

## Phase 120 primitive reuse

Verified: the plan ships zero new transport code. Every primitive — `Connection::send_async`, `conn.recv()` (arrival-order), `conn.recv_for(rid)` (targeted), `Connection::kPipelineDepth`, the `pending_replies_` map, the single-sender / single-reader (PIPE-02) invariant — comes straight from Phase 120 and is consumed unchanged.

## Transport DoS clamp (STATE.md pre-existing pitfall)

The STATE.md flag was on `cli/src/connection.cpp:~685`, unchecked `uint64_t total_size` feeding `std::vector<uint8_t>::reserve` inside the chunked-framing reassembly in `Connection::recv()`. This plan clamps `total_size > MAX_FRAME_SIZE` with `spdlog::warn` + `return std::nullopt` **before** the `reserve`. The same cap applies to the non-chunked single-frame path at line 282, so the policy is now uniform across the receive path.

## Test counts

| Category                   | Count |
|----------------------------|-------|
| Baseline (Phase 120 + etc) |    49 |
| Phase 119 [chunked]        |    17 |
| **Total cli_tests**        |  **66** |
| Pass rate                  | 100%  |

`[chunked]` breakdown (17 cases):
- Sha3Hasher: 2 (incremental vs one-shot, short-input equivalence)
- Manifest codec: 2 (round-trip 2 chunks, round-trip 64 chunks)
- Manifest decoder rejections: 7 (missing magic, 0-chunk, oversized segment_count, truncated chunk_hashes, chunk_size range, version mismatch, total_plaintext overflow)
- plaintext_sha3 wrong size (hand-rolled bad FlatBuffer): 1
- Type classification: 2 (CPAR visible, CDAT hidden)
- Task 2 additions: 3 (outer-magic placement, read_next_chunk streaming, plan_tombstone_targets ordering)

## Unchanged path verification

- `cdb put /tmp/small.bin` (< 400 MiB) still hits the `files.push_back({fp, fname, read_file_bytes(fp)})` branch (commands.cpp:552) unchanged — the new chunked-dispatch `continue` above fires only when `fsize >= chunked::CHUNK_THRESHOLD_BYTES`.
- `cdb rm <hash>` on non-CPAR/non-CDAT blobs (CENV user files, PUBK, etc.) falls through the type-prefix block and hits the existing single-tombstone code path unchanged. The only new cost is one extra round-trip per `cdb rm` (the pre-flight ReadRequest).

## Handoff for Plan 02

Plan 02 (read path + integrity + live-node checkpoint) can consume the primitives landed here:

1. `cmd::get` dispatcher inspects `blob.data[0..4]`:
   - `CDAT` → error "cannot fetch a chunk directly".
   - `CPAR` → envelope::decrypt `blob.data[4..]` → `decode_manifest_payload(*plain)` → hand off to the new `get_chunked` helper in chunked.cpp.
   - else → existing envelope-then-parse_put_payload path unchanged.
2. `get_chunked` pipelines `ReadRequest`s for every `chunk_hashes[i]` with a batch-local `rid → chunk_index` map, drains via `conn.recv()` in arrival order, strips the `CDAT` outer magic, envelope::decrypts, and `pwrite`s at `chunk_idx * chunk_size_bytes`. Out-of-order landings are naturally handled.
3. Post-reassembly: re-read the output file in order, feed every byte to a fresh `Sha3Hasher`, finalize, compare to `manifest.plaintext_sha3`. On mismatch → `unlink` + error (D-12).
4. Preallocate output via `ftruncate(fd, manifest.total_plaintext_bytes)` after `open(O_WRONLY|O_CREAT|O_EXCL)`; mid-download failures → `unlink` (D-12).
5. Live-node checkpoint: upload 1 GiB file against `192.168.1.73`, `cdb ls --raw --type CDAT` shows ~64 chunks, `cdb get <manifest_hash>` reassembles byte-identical, `cdb rm <manifest_hash>` cleans up all 65 blobs.

Everything Plan 02 needs is in place: the codec, the chunk magic, the envelope helpers, the pipelining primitives, and the streaming primitives all have unit tests driving them.

## Self-Check: PASSED

**Files exist:**
- `cli/src/chunked.h`: FOUND
- `cli/src/chunked.cpp`: FOUND
- `cli/tests/test_chunked.cpp`: FOUND

**Modified files exist and were committed:**
- `cli/src/wire.h`, `cli/src/wire.cpp`: modified in `4e7ff4d1`
- `cli/src/main.cpp`, `cli/src/commands.cpp`: modified in `4e7ff4d1` + `3943ba39`
- `cli/src/connection.cpp`: modified in `3943ba39`
- `cli/CMakeLists.txt`, `cli/tests/CMakeLists.txt`: modified in `ac951692` + `3943ba39`

**Commits exist in git log:**
- `ac951692` — test(119-01): RED Task 1
- `4e7ff4d1` — feat(119-01): GREEN Task 1
- `f83ebcd7` — test(119-01): RED Task 2
- `3943ba39` — feat(119-01): GREEN Task 2

**Acceptance grep checks:** All 34 plan-specified patterns present (verified inline during Task 2 GREEN).

**Tests green:** 66/66 cli_tests pass; 17/17 [chunked] pass; no regressions in baseline [pipeline] / [wire] / [envelope] / [identity] / [contacts].

**cdb binary builds clean:** Only pre-existing `-Wfree-nonheap-object` warning in unrelated `encode_auth_payload` (untouched).

**STATE.md pre-existing pitfall:** closed — `total_size > MAX_FRAME_SIZE` clamp present in `connection.cpp`.

---
*Phase: 119-chunked-large-files*
*Plan: 01*
*Completed: 2026-04-19*
