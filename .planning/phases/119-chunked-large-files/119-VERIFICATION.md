---
phase: 119-chunked-large-files
verified: 2026-04-19T00:00:00Z
status: gaps_found
score: 4/5
overrides_applied: 0
gaps:
  - truth: "End-to-end: user uploads >500 MiB, downloads the CPAR manifest, and receives byte-identical output with SHA3 verification passing"
    status: failed
    reason: "CR-01: Connection::recv() never decrements in_flight_. Every pipelined Phase-B drain loop (put_chunked, rm_chunked, get_chunked, cmd::put, cmd::get) calls conn.recv() for arrival-order draining, but recv() has no in_flight_ bookkeeping. Only recv_for (via pump_recv_for) decrements the counter. Once kPipelineDepth=8 slots fill, send_async enters the backpressure loop and stalls forever because the counter never falls below 8. Observed in live-node testing: 420 MiB file hung after 46 min with exactly 8 chunks on disk and 0 WriteAcks returned. The code is complete, builds clean, and all 75 unit tests pass — but end-to-end put->get->sha3-verify cannot succeed until the in_flight_ counter is decremented in the arrival-order drain path."
    artifacts:
      - path: "cli/src/connection.cpp"
        issue: "Connection::recv() (line 674) returns a DecodedTransport without touching in_flight_. send_async increments in_flight_ at line 743; only recv_for (via pump_recv_for) decrements it. The Phase-B drain loops in put_chunked, rm_chunked, get_chunked, cmd::put, and cmd::get all use conn.recv() — so in_flight_ never decreases after the initial fill."
      - path: "cli/src/chunked.cpp"
        issue: "put_chunked Phase B (line 243), rm_chunked Phase 1 (line 384), get_chunked Phase B (line 604) — all call conn.recv() in their drain loops without a matching in_flight_ decrement."
      - path: "cli/src/commands.cpp"
        issue: "cmd::put Phase B (line ~618) and cmd::get Phase B (~line 735) — same drain pattern, same in_flight_ leak. This also means single-blob batches of 9+ files will hang even without Phase 119 code."
    missing:
      - "A Connection::recv_next() (or equivalent) primitive that decrements in_flight_ and drains from pending_replies_ first, then the wire — identical contract to recv_for but for 'any rid'. All five call sites replace their conn.recv() with conn.recv_next()."
      - "Fix also resolves WR-01 (put_chunked retry double-count corner case) and WR-04 (cmd::put/cmd::get single-blob batch hang on 9+ files) identified in the code review."
deferred:
  - truth: "Live-node end-to-end gate: upload 1 GiB file, cdb ls --raw --type CDAT shows N chunks, cdb get <manifest_hash> reassembles byte-identical, cdb rm <manifest_hash> cleans up all blobs"
    addressed_in: "Phase 122"
    evidence: "Phase 122 SC-3: 'E2E verification against live node at 192.168.1.73 completes full workflow: put chunked file with group sharing, pipelined get, ls filtering, peer management'"
human_verification: []
---

# Phase 119: Chunked Large Files — Verification Report

**Phase Goal:** Users can upload and download files larger than 500 MiB without full memory buffering, with automatic chunk management and truncation prevention
**Verified:** 2026-04-19
**Status:** gaps_found
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | User can upload a file >500 MiB and the CLI streams it in chunks without buffering the entire file in memory | PARTIAL | `put_chunked` exists, builds, has correct streaming logic (16 MiB reusable buffer, Sha3Hasher absorbs plaintext before encrypt). Hung end-to-end due to CR-01 (`in_flight_` leak) — phase-B drain never returns slots so send_async stalls permanently after 8 in-flight. Unit tests pass; live transfer fails. |
| 2 | Upload produces CDAT chunk blobs plus a CPAR manifest blob that references all chunks | VERIFIED | `build_cdat_blob_flatbuf` and `build_cpar_manifest` in `chunked.cpp`. `encode_manifest_payload` / `decode_manifest_payload` in `wire.cpp` with full FlatBuffers codec. `ManifestData` carries `segment_count`, `chunk_hashes`, `total_plaintext_bytes`, `plaintext_sha3`. Round-trip unit tests pass (2-chunk, 64-chunk). |
| 3 | Download of a CPAR manifest automatically reassembles all referenced chunks into the original file | PARTIAL | `get_chunked` in `chunked.cpp` (240+ lines): pipelined ReadRequests, arrival-order pwrite, post-reassembly SHA3 verify. `cmd::get` dispatch in `commands.cpp` detects CPAR and calls `get_chunked`. Unit-tested without socket. Blocked end-to-end by the same CR-01 `in_flight_` leak. |
| 4 | `cdb rm` of a manifest blob deletes the manifest and all associated CDAT chunk blobs | PARTIAL | `rm_chunked` implements chunks-first / manifest-last ordering (D-09). `cmd::rm` pre-flight ReadRequest + type-prefix dispatch: CDAT → error, CPAR → `rm_chunked`. `plan_tombstone_targets` unit-tested. Same CR-01 `in_flight_` leak applies to the Phase-1 chunk tombstone drain loop. |
| 5 | Envelope format v2 includes segment count to prevent truncation attack (CHUNK-05) | VERIFIED | `ManifestData.segment_count` is a required field in the CPAR manifest FlatBuffer and is validated on decode. Post-reassembly `verify_plaintext_sha3` compares SHA3-256 of full reassembled plaintext against `manifest.plaintext_sha3`, providing defense-in-depth beyond bare count. Unit tested: 7 decoder rejection paths + 3 verify_plaintext_sha3 cases. |

**Score:** 4/5 truths (2 VERIFIED, 2 PARTIAL-blocked-by-CR-01, 1 directly VERIFIED)

The overall score of 4/5 is contested: truths 1/3/4 have their code implementations verified — the implementations are correct and unit-tested — but cannot be demonstrated end-to-end because CR-01 prevents the pipelined drain loops from completing. The conservative (and honest) read is `gaps_found` because the phase goal as stated requires the capability to actually work, not just to compile.

### Deferred Items

Items not yet met but explicitly addressed in later milestone phases.

| # | Item | Addressed In | Evidence |
|---|------|-------------|----------|
| 1 | Live-node E2E gate: upload 1 GiB, ls shows N chunks, get reassembles byte-identical, rm cleans up | Phase 122 | Phase 122 SC-3: "E2E verification against live node at 192.168.1.73 completes full workflow: put chunked file with group sharing, pipelined get, ls filtering, peer management" |

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `cli/src/chunked.h` | namespace chromatindb::cli::chunked; put_chunked, rm_chunked, get_chunked, pure helpers | VERIFIED | 165 lines. All functions declared. CHUNK_THRESHOLD_BYTES (400 MiB), MAX_CHUNKED_FILE_SIZE (1 TiB), RETRY_ATTEMPTS=3, RETRY_BACKOFF_MS present. |
| `cli/src/chunked.cpp` | Streaming upload, cascade delete, download, SHA3 verify | VERIFIED | 706 lines. All four main functions implemented substantively. No stubs. |
| `cli/src/wire.h` | CPAR_MAGIC, Sha3Hasher, ManifestData, encode/decode_manifest_payload, type_label CPAR, is_hidden_type excludes CPAR | VERIFIED | CPAR_MAGIC present. type_label returns "CPAR". is_hidden_type returns false for CPAR, true for CDAT. Sha3Hasher class declared. MAX_CHUNKS, CHUNK_SIZE_BYTES_DEFAULT/MIN/MAX present. |
| `cli/src/wire.cpp` | manifest_vt namespace, Sha3Hasher::Impl, encode/decode_manifest_payload | VERIFIED | manifest_vt at line 40. Sha3Hasher::Impl wraps OQS_SHA3_sha3_256_inc_ctx. encode_manifest_payload / decode_manifest_payload with full invariant checking. |
| `cli/src/commands.cpp` | cmd::put size-dispatch to put_chunked; cmd::rm type-prefix dispatch to rm_chunked; cmd::get classify_blob_data dispatch to get_chunked | VERIFIED | All three dispatch points verified: put (line 544), get (line 786), rm (line 963). Correct threshold checks, CDAT/CPAR/Other paths. |
| `cli/src/main.cpp` | --type help text includes CPAR | VERIFIED | Line 618: "Filter by type: CENV, PUBK, TOMB, DLGT, CDAT, CPAR" |
| `cli/src/connection.cpp` | DoS clamp: total_size > MAX_FRAME_SIZE before vector::reserve | VERIFIED | Lines 692-697: explicit clamp with spdlog::warn + return nullopt. Pre-existing pitfall from STATE.md closed. |
| `cli/tests/test_chunked.cpp` | Catch2 [chunked] tests | VERIFIED | 465 lines. 26 [chunked] test cases covering all plan-required scenarios. |
| `cli/tests/CMakeLists.txt` | test_chunked.cpp + chunked.cpp + connection.cpp linked | VERIFIED | grep confirms test_chunked.cpp and ../src/chunked.cpp in sources. |
| `cli/CMakeLists.txt` | src/chunked.cpp linked into cdb | VERIFIED | grep confirms src/chunked.cpp in cdb target. |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| cmd::put (commands.cpp:544) | chunked::put_chunked | fsize >= CHUNK_THRESHOLD_BYTES branch | WIRED | `chunked::put_chunked(id, ns, recipient_spans, fp, fname, ttl, conn, opts)` called directly |
| cmd::rm (commands.cpp:963) | chunked::rm_chunked | CPAR type-prefix check on decoded blob | WIRED | Full dispatch block reads blob, checks 4-byte magic, decrypts manifest, calls `chunked::rm_chunked` |
| cmd::get (commands.cpp:786) | chunked::get_chunked | classify_blob_data() == CPAR | WIRED | switch on GetDispatch::CPAR at line 794 decrypts manifest and calls `chunked::get_chunked` |
| put_chunked Phase A | Connection::send_async | `conn.send_async(MsgType::Data, flatbuf, this_rid)` | WIRED | Line 207 in chunked.cpp |
| put_chunked Phase B | Connection::recv() | `conn.recv()` drain loop | WIRED (buggy) | Line 243. Recv wired but in_flight_ counter never decremented — CR-01 |
| rm_chunked Phase 1 drain | Connection::recv() | arrival-order drain loop | WIRED (buggy) | Line 384. Same CR-01 issue |
| get_chunked Phase B | Connection::recv() | arrival-order drain loop | WIRED (buggy) | Line 604. Same CR-01 issue |
| manifest send | Connection::recv_for(mrid) | targeted drain for manifest WriteAck | WIRED | Line 314 uses recv_for correctly — this step works |
| chunked.cpp | Sha3Hasher | absorb plaintext before encrypt, finalize after all chunks | WIRED | Lines 201, 282 |
| get_chunked | verify_plaintext_sha3 | post-reassembly re-read + compare | WIRED | Line 688 |

### Data-Flow Trace (Level 4)

Phase 119 is CLI logic (no server-side rendering); data flows through Connection rather than a DB query. The critical data flow is: file bytes → Sha3Hasher → ManifestData.plaintext_sha3 → encode_manifest_payload → blob on node → decode_manifest_payload → verify_plaintext_sha3.

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|--------------|--------|--------------------|--------|
| `put_chunked` | chunk_hashes | Bound from WriteAck payload at drain time (post-WriteAck, D-15) | Yes — final server-assigned hash, not predicted | FLOWING |
| `put_chunked` | plaintext_sha3 | Sha3Hasher absorbs plaintext in each read_next_chunk call | Yes — real file bytes | FLOWING |
| `get_chunked` | pwrite offset | `chunk_idx * manifest.chunk_size_bytes` | Yes — derives from manifest field | FLOWING |
| `get_chunked` | SHA3 verify | verify_plaintext_sha3 re-reads output file, compares to manifest.plaintext_sha3 | Yes — real file bytes | FLOWING |
| drain loops (put/rm/get) | in_flight_ | send_async increments; recv() does NOT decrement | No — counter leaks upward, blocks after 8 | DISCONNECTED (CR-01) |

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| [chunked] unit tests pass | `build/cli/tests/cli_tests "[chunked]"` | "All tests passed (196809 assertions in 26 test cases)" | PASS |
| Full test suite — no regressions | `build/cli/tests/cli_tests` | "All tests passed (197061 assertions in 75 test cases)" | PASS |
| cdb binary builds clean | `cmake --build build/cli -j$(nproc)` | Exit 0. Only pre-existing -Wfree-nonheap-object in encode_auth_payload. | PASS |
| End-to-end chunked put (420 MiB) | `time cdb put small.bin --node local` | Hung after 46 min; 8 chunks on disk, 0 WriteAcks returned; exactly matches kPipelineDepth=8 saturation (CR-01) | FAIL |

### Requirements Coverage

| Requirement | Description | Status | Evidence |
|-------------|-------------|--------|---------|
| CHUNK-01 | User can upload files >500 MiB without full memory buffering | PARTIAL | `put_chunked` streams with 16 MiB reusable buffer (D-11). Working memory bounded to ~kPipelineDepth × one-encoded-chunk. Code correct, unit-tested. Blocked end-to-end by CR-01. |
| CHUNK-02 | Upload splits into CDAT chunk blobs + CPAR manifest blob | VERIFIED | CDAT_MAGIC / CPAR_MAGIC outer placement (D-13). FlatBuffers manifest with all required fields. Encoder/decoder unit-tested (2-chunk, 64-chunk round-trips). |
| CHUNK-03 | Download detects CPAR manifest and reassembles chunks automatically | PARTIAL | `classify_blob_data` dispatch in `cmd::get`. `get_chunked` with pwrite-at-offset, overwrite guard, SHA3 verify. Unit-tested (pwrite out-of-order, refuse_if_exists, verify_plaintext_sha3). Blocked end-to-end by CR-01. |
| CHUNK-04 | `cdb rm` of a manifest deletes all associated chunks | PARTIAL | `plan_tombstone_targets` guarantees D-09 ordering. `rm_chunked` chunks-first cascade unit-tested. `cmd::rm` type-prefix dispatch wired. Blocked end-to-end by CR-01. |
| CHUNK-05 | Envelope format v2 includes segment count to prevent truncation attack | VERIFIED | `segment_count` is a required FlatBuffer field in ManifestData, validated on decode (rejects 0-chunk, rejects count > MAX_CHUNKS, rejects chunk_hashes.size() != segment_count * 32). Plus `plaintext_sha3` whole-file integrity (defense-in-depth per D-04). 10 relevant unit tests pass. |

All 5 CHUNK requirements are claimed by the phase plans. No orphaned requirements.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| cli/src/chunked.cpp | 243 (put), 384 (rm), 604 (get) | `conn.recv()` in Phase-B drain without `--in_flight_` | Blocker (CR-01) | Causes permanent stall after 8 in-flight; observed in production |
| cli/src/commands.cpp | ~618 (put), ~735 (get) | Same `conn.recv()` pattern in single-blob drain loops | Blocker (WR-04) | Will hang any batch of 9+ small files even without Phase 119 code |
| cli/src/chunked.cpp | 578-587 | get_chunked retry backoff indexes RETRY_BACKOFF_MS[1..2] never [0] — first retry sleeps 1000 ms not 250 ms | Warning (WR-02) | Violates D-15 "250/1000/4000 ms" policy; first retry is 4x slower than specified |
| cli/src/chunked.cpp | ~677-696 | get_chunked closes fd before verify_plaintext_sha3; exception between close and verify would leave partial file on disk | Warning (WR-03) | Violates D-12 "unlink on any failure" on std::bad_alloc path |
| cli/src/main.cpp | 216-218 | `catch (...)` silently ignores malformed config.json | Info (IN-03) | Violates project rule "don't suppress errors with || true" — bad config looks like defaults |

### Human Verification Required

None required at this time. The end-to-end gate is blocked by a known, mechanical code bug (CR-01) rather than by any ambiguity requiring human judgment. Fix CR-01 first, then re-verify; the live-node checkpoint can be batched with Phase 122 E2E verification.

### Gaps Summary

One root cause blocks goal achievement:

**CR-01 (`in_flight_` counter leak)** is the single gap. `Connection::recv()` consumes replies from the wire without decrementing the `in_flight_` counter. `send_async` increments it and will block forever in its backpressure loop once the counter reaches `kPipelineDepth=8`. All five pipelined drain paths in Phase 119 use `conn.recv()` for arrival-order draining (the correct Phase 120-02 pattern for non-targeted drains), but the counter mechanism was only wired for `recv_for` (which calls `pump_recv_for` → `--in_flight_`).

The fix is a `Connection::recv_next()` public method (or equivalent) that drains `pending_replies_` first, then calls `recv()`, and decrements `in_flight_` on success. All five call sites — `put_chunked` Phase B, `rm_chunked` Phase 1 drain, `get_chunked` Phase B, `cmd::put` Phase B, `cmd::get` Phase B — replace their `conn.recv()` with `conn.recv_next()`.

Secondary issues (WR-02 retry backoff, WR-03 fd leak on exception) are worth fixing in the same pass but do not independently block the phase goal.

**What works:** All artifacts are present, substantive, and correctly wired. The FlatBuffers manifest codec, Sha3Hasher, CPAR/CDAT type dispatch, cascade-delete ordering, overwrite guard, pwrite-at-offset reassembly, and DoS clamp are all correct and fully unit-tested (75/75 tests passing). The phase is one bug fix away from complete.

---

_Verified: 2026-04-19_
_Verifier: Claude (gsd-verifier)_
