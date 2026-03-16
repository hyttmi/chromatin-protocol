---
phase: 30-benchmark-scenarios
verified: 2026-03-16T15:45:00Z
status: passed
score: 5/5 must-haves verified
re_verification: false
---

# Phase 30: Benchmark Scenarios Verification Report

**Phase Goal:** All core performance scenarios are measured with resource profiling
**Verified:** 2026-03-16T15:45:00Z
**Status:** PASSED
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths (from ROADMAP.md success_criteria)

| #   | Truth                                                                                  | Status     | Evidence                                                                                             |
| --- | -------------------------------------------------------------------------------------- | ---------- | ---------------------------------------------------------------------------------------------------- |
| 1   | Ingest throughput measured at multiple blob sizes with blobs/sec, MiB/sec, p50/p95/p99 | ✓ VERIFIED | `run_scenario_ingest` loops over 1K/100K/1M, calls loadgen, parses and writes JSON per-size to `deploy/results/scenario-ingest-${label}.json` (lines 199-238) |
| 2   | Sync latency (1-hop) and multi-hop propagation (2-hop) measured as wall-clock times    | ✓ VERIFIED | `run_scenario_sync` times node2 (1-hop) and node3 (2-hop) convergence via `wait_for_convergence`, writes `scenario-sync-latency.json` (lines 240-307) |
| 3   | Late-joiner catch-up time measured (new node joins after data loaded)                  | ✓ VERIFIED | `run_scenario_latejoin` gates on node3 convergence, starts node4, times catch-up with 300s timeout, writes `scenario-latejoin.json` (lines 309-383) |
| 4   | Trusted vs PQ handshake overhead compared with same workload under both modes          | ✓ VERIFIED | `run_scenario_trusted_vs_pq` runs full PQ topology, then down/up with compose override for trusted, writes `scenario-trusted-vs-pq.json` (lines 385-472) |
| 5   | CPU, memory, and disk I/O per container captured via docker stats during each scenario | ✓ VERIFIED | `capture_stats` runs `docker stats --no-stream --format json` for all 3 nodes, saves to `docker-stats-${label}-${phase}.json` (lines 91-102). Called pre/post in all 4 scenario functions. |

**Score:** 5/5 truths verified

---

### Required Artifacts

| Artifact                                    | Expected                                              | Status      | Details                                                                                   |
| ------------------------------------------- | ----------------------------------------------------- | ----------- | ----------------------------------------------------------------------------------------- |
| `deploy/run-benchmark.sh`                   | Complete benchmark orchestration with all 5 scenarios | ✓ VERIFIED  | 513 lines, executable (-rwxr-xr-x), passes `bash -n` syntax check. Contains all 13 named functions. |
| `deploy/docker-compose.trusted.yml`         | Compose override mounting trusted-mode configs        | ✓ VERIFIED  | 23 lines. Correctly overrides volumes for node1/node2/node3 with node*-trusted.json mounts, re-specifies data volumes as required by compose override semantics. |
| `deploy/results/scenario-ingest-1k.json`    | Ingest throughput results for 1 KiB blobs             | ✓ VERIFIED  | Runtime artifact — code path at line 223 writes `scenario-ingest-${label}.json` with label="1k". Script creates `deploy/results/` via `mkdir -p`. |
| `deploy/results/scenario-sync-latency.json` | Sync latency and multi-hop propagation results        | ✓ VERIFIED  | Runtime artifact — code path at line 302 writes `scenario-sync-latency.json`. JSON includes `sync_1hop_ms`, `sync_2hop_ms`, embedded loadgen output. |
| `deploy/results/scenario-latejoin.json`     | Late-joiner catch-up timing                           | ✓ VERIFIED  | Runtime artifact — code path at line 378 writes `scenario-latejoin.json`. JSON includes `catchup_ms` with note field. |
| `deploy/results/scenario-trusted-vs-pq.json`| PQ vs trusted handshake comparison                   | ✓ VERIFIED  | Runtime artifact — code path at line 460 writes `scenario-trusted-vs-pq.json`. JSON embeds full loadgen output for both PQ and trusted modes. |

Note: `deploy/results/` is a runtime directory created by the script via `mkdir -p "$RESULTS_DIR"` (line 485). It does not exist in the repository before execution — this is expected and correct.

---

### Key Link Verification

**Plan 30-01 key links:**

| From                      | To                      | Via                                    | Status    | Details                                                                             |
| ------------------------- | ----------------------- | -------------------------------------- | --------- | ----------------------------------------------------------------------------------- |
| `deploy/run-benchmark.sh` | `chromatindb_loadgen`   | `docker run --rm --network $NETWORK`   | ✓ WIRED   | Lines 147-150: `docker run --rm --network "$NETWORK" chromatindb:latest chromatindb_loadgen --target ...` |
| `deploy/run-benchmark.sh` | `docker stats`          | `docker stats --no-stream --format json` | ✓ WIRED | Line 96: `docker stats --no-stream` with JSON format template (full format string at lines 97-98) |
| `deploy/run-benchmark.sh` | SIGUSR1 metrics         | `docker kill -s USR1 + docker logs grep` | ✓ WIRED | Line 106: `docker kill -s USR1 "$container"`, line 109: `docker logs | grep "metrics:" | tail -1 | grep -oP 'blobs=\K[0-9]+'` |

**Plan 30-02 key links:**

| From                      | To                              | Via                                              | Status    | Details                                                                                          |
| ------------------------- | ------------------------------- | ------------------------------------------------ | --------- | ------------------------------------------------------------------------------------------------ |
| `deploy/run-benchmark.sh` | `deploy/docker-compose.yml`     | `--profile latejoin` for node4                  | ✓ WIRED   | Line 334: `$COMPOSE --profile latejoin up -d node4`                                             |
| `deploy/run-benchmark.sh` | `deploy/docker-compose.trusted.yml` | `docker compose -f ... -f ...` override      | ✓ WIRED   | Line 414: `docker compose -f "$COMPOSE_FILE" -f "$SCRIPT_DIR/docker-compose.trusted.yml" -p chromatindb up -d --wait` |
| `deploy/run-benchmark.sh` | `deploy/configs/node*-trusted.json` | `docker inspect.*IPAddress` runtime resolution | ✓ WIRED   | Lines 159-161: `docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}'` for node1/2/3 |

---

### Requirements Coverage

| Requirement | Source Plan | Description                                                                                                    | Status       | Evidence                                                                  |
| ----------- | ----------- | -------------------------------------------------------------------------------------------------------------- | ------------ | ------------------------------------------------------------------------- |
| PERF-01     | 30-01       | Ingest throughput measured at multiple blob sizes (blobs/sec, MiB/sec, p50/p95/p99 latency)                   | ✓ SATISFIED  | `run_scenario_ingest` iterates 3 sizes (1024, 102400, 1048576), parses blobs_per_sec, mib_per_sec, latency_ms from loadgen JSON |
| PERF-02     | 30-01       | Sync/replication latency measured (wall-clock time from write on node A to availability on node B)             | ✓ SATISFIED  | `run_scenario_sync` measures node2 (1-hop) wall-clock convergence in ms via SIGUSR1 polling |
| PERF-03     | 30-01       | Multi-hop propagation time measured (A->B->C chain, no direct A-C link)                                        | ✓ SATISFIED  | Same `run_scenario_sync` measures node3 (2-hop) convergence; comment notes sync_interval delay is expected |
| OBS-01      | 30-01       | Resource profiling captures CPU, memory, disk I/O per container via docker stats during benchmark runs         | ✓ SATISFIED  | `capture_stats` captures CPUPerc, MemUsage, MemPerc, NetIO, BlockIO, PIDs per container before and after every scenario |
| PERF-04     | 30-02       | Late-joiner catch-up time measured (new node joins after data loaded, measures convergence)                    | ✓ SATISFIED  | `run_scenario_latejoin` loads data, converges node3, starts node4, polls blob count until $BLOB_COUNT reached |
| PERF-05     | 30-02       | Trusted vs PQ handshake comparison run with same workload under both modes                                     | ✓ SATISFIED  | `run_scenario_trusted_vs_pq` runs full down/up for each mode, uses compose override with runtime-resolved IPs |
| LOAD-04     | 30-02       | run-benchmark.sh script builds images, starts topology, runs all scenarios, collects metrics, generates report  | ✓ SATISFIED  | `main` calls check_deps, build_image, all 4 scenario functions, final `$COMPOSE down -v`, result count summary |

**All 7 requirements satisfied. No orphaned requirements.**

REQUIREMENTS.md mapping table confirms all 7 IDs are assigned to Phase 30 with status "Complete".

---

### Anti-Patterns Found

None. No TODO/FIXME/PLACEHOLDER comments. No stub implementations. No empty return values. All functions implement substantive logic.

---

### Human Verification Required

The following items cannot be verified programmatically. They require a live Docker environment.

#### 1. Ingest scenario produces valid numeric results

**Test:** Run `bash deploy/run-benchmark.sh --skip-build` (requires `chromatindb:latest` built and Docker running). Inspect `deploy/results/scenario-ingest-1k.json`.
**Expected:** JSON with `blobs_per_sec`, `mib_per_sec`, `latency_ms.p50/p95/p99` as positive numbers; `total_blobs` equals 200; `errors` equals 0.
**Why human:** Runtime output depends on Docker, the loadgen binary, and live node health. Cannot verify numerics from static analysis.

#### 2. SIGUSR1 blob-count polling works correctly

**Test:** During a run, observe stderr log output showing "Waiting for chromatindb-nodeX: N/200 blobs..." lines incrementing toward 200.
**Expected:** Convergence polling completes without timeout (sync_1hop_ms and sync_2hop_ms are positive, not -1).
**Why human:** SIGUSR1 delivery, spdlog flush timing, and docker log availability depend on runtime container state.

#### 3. Late-joiner scenario correctly isolates catch-up timing

**Test:** Review `deploy/results/scenario-latejoin.json` after a run.
**Expected:** `catchup_ms` is a plausible catch-up time (greater than one sync_interval at 10s = ~10000ms) and blob_count matches 200.
**Why human:** The catch-up window starts from when node4 is healthy, which requires observing the Docker healthcheck state machine at runtime.

#### 4. Trusted vs PQ comparison yields a valid latency delta

**Test:** Review `deploy/results/scenario-trusted-vs-pq.json`. Compare `pq_mode.latency_ms.p50` vs `trusted_mode.latency_ms.p50`.
**Expected:** Both modes produce non-zero latency measurements. Trusted mode should show equal or lower handshake overhead (PQ ML-KEM-1024 negotiation is heavier).
**Why human:** The comparison is meaningful only if both compose restarts completed cleanly and both loadgen runs hit the same node. Requires runtime inspection.

---

### Gaps Summary

No gaps found. All 5 success criteria from ROADMAP.md are implemented and all wiring is substantive. The script is 513 lines, passes syntax check, is executable, and all code paths to result files are present.

The only items flagged are runtime-execution human tests — standard for an orchestration script where the actual measurements require a live Docker environment.

---

_Verified: 2026-03-16T15:45:00Z_
_Verifier: Claude (gsd-verifier)_
