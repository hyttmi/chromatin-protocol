---
phase: 36-deletion-benchmarks
verified: 2026-03-18T16:15:00Z
status: passed
score: 9/9 must-haves verified
re_verification: false
---

# Phase 36: Deletion Benchmarks Verification Report

**Phase Goal:** Tombstone creation, sync propagation, and garbage collection performance are measured and baselined in the existing Docker benchmark suite
**Verified:** 2026-03-18T16:15:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

All truths drawn from must_haves in 36-01-PLAN.md and 36-02-PLAN.md.

#### Plan 01 Truths (loadgen/loadgen_main.cpp)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Loadgen --delete mode constructs tombstones and sends TransportMsgType_Delete (type 18) | VERIFIED | `msg_type = chromatindb::wire::TransportMsgType_Delete` at line 566; `make_tombstone_request()` calls `wire::make_tombstone_data()` at line 243 |
| 2 | Loadgen write mode outputs blob_hashes JSON array in stats for downstream delete consumption | VERIFIED | `j["blob_hashes"] = blob_hashes` in `stats_to_json()` at line 311; `blob_hashes_.push_back(hash_hex)` at line 577 |
| 3 | Loadgen --identity-save and --identity-file flags persist/reload the same keypair across runs | VERIFIED | `identity.save_to(cfg.identity_save_path)` at line 787; `NodeIdentity::load_from(cfg.identity_file_path)` at line 779; `--identity-save DIR` and `--identity-file DIR` printed in usage at lines 70-71 |
| 4 | Loadgen delete mode measures DeleteAck latency (not Notification) and reports same JSON stats format | VERIFIED | `handle_delete_ack()` at line 660 extracts 41-byte payload, computes latency, pushes to `latencies_`; subscription skipped in delete mode (line 497); `scenario = "delete"` set in `stats_to_json()` at line 287 |

#### Plan 02 Truths (deploy/run-benchmark.sh)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 5 | Docker benchmark suite runs tombstone creation scenarios for all 3 blob sizes and reports throughput | VERIFIED | `run_scenario_tombstone_creation()` at line 1136 iterates sizes=(1024 102400 1048576); invoked in `main()` at line 1434 |
| 6 | Tombstone sync propagation latency is measured across the 3-node chain (1-hop and 2-hop) | VERIFIED | `run_scenario_tombstone_sync()` at line 1197 calls `wait_for_convergence` for node2 (1-hop) and node3 (2-hop) at lines 1241-1256; convergence target = 400 (200 blobs + 200 tombstones) |
| 7 | Tombstone GC/expiry convergence is measured per-node after TTL=30 expiry | VERIFIED | `run_scenario_tombstone_gc()` at line 1284; uses `wait_for_gc_completion()` (storage-based, not seq_num) for each of node1, node2, node3; TTL=30 for both blobs and tombstones |
| 8 | REPORT.md contains three new sections: Tombstone Creation, Tombstone Sync Propagation, Tombstone GC/Expiry | VERIFIED | Lines 752, 767, 771 in script contain `## Tombstone Creation`, `## Tombstone Sync Propagation`, `## Tombstone GC/Expiry`; Executive Summary rows at lines 691-693 |
| 9 | benchmark-summary.json contains tombstone scenario keys | VERIFIED | Lines 846-850: `tombstone_creation_1k`, `tombstone_creation_100k`, `tombstone_creation_1m`, `tombstone_sync`, `tombstone_gc` in the `jq -n` output block |

**Score:** 9/9 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `loadgen/loadgen_main.cpp` | Extended loadgen with --delete, --identity-file, --identity-save, --hashes-from, blob_hashes output | VERIFIED | 833 lines; all flags present in Config struct and parse_args(); make_tombstone_request(), from_hex(), handle_delete_ack() all implemented |
| `deploy/run-benchmark.sh` | Three new tombstone scenario functions + report integration + GC polling | VERIFIED | 1452 lines; run_scenario_tombstone_creation/sync/gc(), get_storage_mib(), wait_for_gc_completion(), format_tombstone_row(), report sections all present |

### Key Link Verification

#### Plan 01 Key Links

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `loadgen/loadgen_main.cpp` | `db/wire/codec.h` | `make_tombstone_data()`, `build_signing_input()`, `encode_blob()`, `blob_hash()` | WIRED | Pattern `make_tombstone_data` found at line 243; all four functions called in tombstone construction path |
| `loadgen/loadgen_main.cpp` | `db/identity/identity.h` | `NodeIdentity::save_to()`, `NodeIdentity::load_from()` | WIRED | `save_to` at line 787, `load_from` at line 779; both on hot paths in main() |
| `loadgen/loadgen_main.cpp` | `db/wire/transport_generated.h` | `TransportMsgType_Delete` (18), `TransportMsgType_DeleteAck` (19) | WIRED | `TransportMsgType_Delete` used at line 566 for send; `TransportMsgType_DeleteAck` at line 614 for receive dispatch |

#### Plan 02 Key Links

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `deploy/run-benchmark.sh` | `loadgen/loadgen_main.cpp` | `run_loadgen_v` with `--delete --hashes-from stdin` | WIRED | Pattern found at lines 1175, 1232, 1327 — three call sites across the three tombstone scenarios |
| `deploy/run-benchmark.sh` | `deploy/results/REPORT.md` | `generate_report()` tombstone sections | WIRED | Tombstone Creation section at line 752, Tombstone Sync Propagation at 767, Tombstone GC/Expiry at 771 in the heredoc |
| `deploy/run-benchmark.sh` | `deploy/results/benchmark-summary.json` | tombstone scenario keys in JSON | WIRED | All 5 tombstone keys (creation_1k, creation_100k, creation_1m, sync, gc) present in `jq -n` block at lines 846-850 |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| BENCH-01 | 36-01, 36-02 | Tombstone creation performance scenario added to Docker benchmark suite | SATISFIED | `run_scenario_tombstone_creation()` iterates all 3 blob sizes; loadgen --delete mode sends TransportMsgType_Delete and measures DeleteAck latency; results written to scenario-tombstone-creation-{1k,100k,1m}.json |
| BENCH-02 | 36-02 | Tombstone sync propagation latency measured across multi-node topology | SATISFIED | `run_scenario_tombstone_sync()` times node2 (1-hop) and node3 (2-hop) convergence at expected_count = BLOB_COUNT * 2 |
| BENCH-03 | 36-02 | Tombstone GC/expiry performance measured under load | SATISFIED | `run_scenario_tombstone_gc()` uses storage-based polling (not seq_num high-water) via `get_storage_mib()` and `wait_for_gc_completion()` per node |

No orphaned requirements: all three BENCH requirements claimed by plans are accounted for.

### Anti-Patterns Found

None. No TODOs, FIXMEs, placeholder returns, or stub handlers found in either modified file.

### Human Verification Required

#### 1. Full Benchmark Run End-to-End

**Test:** Run `bash deploy/run-benchmark.sh` against a live Docker topology.
**Expected:** All three tombstone scenarios execute; REPORT.md has populated tombstone tables; benchmark-summary.json has non-null tombstone_* keys.
**Why human:** Requires Docker with a running compose topology. Script correctness is verified (bash -n passes) but runtime behavior — DeleteAck timing, GC completion, storage metric parsing — cannot be checked without live nodes.

#### 2. Identity Persistence Across Runs

**Test:** Run loadgen with `--identity-save /tmp/test-id`, then run again with `--identity-file /tmp/test-id`. Verify both runs use the same namespace_id (observable in log output: `namespace: <hex>`).
**Expected:** Namespace hex string is identical across both runs.
**Why human:** Requires running loadgen binary; cannot verify runtime key serialization correctness from source alone.

#### 3. DeleteAck Payload Size Assumption

**Test:** Confirm the node actually sends DeleteAck payloads of exactly 41 bytes (the size `handle_delete_ack()` expects at line 661).
**Expected:** No "unexpected DeleteAck payload size" warnings in loadgen logs during a benchmark run.
**Why human:** The 41-byte format is documented in the plan's interface contract and cross-referenced to the server implementation, but the actual wire format can only be confirmed by running the system.

### Gaps Summary

No gaps. All 9 truths verified, all 3 requirements satisfied, all 6 key links wired, no anti-patterns.

---

_Verified: 2026-03-18T16:15:00Z_
_Verifier: Claude (gsd-verifier)_
