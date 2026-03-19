# Phase 41: Benchmark Validation - Research

**Researched:** 2026-03-19
**Domain:** Docker benchmark orchestration, performance measurement, sync protocol validation
**Confidence:** HIGH

## Summary

Phase 41 validates three v0.8.0 changes via the existing Docker benchmark infrastructure: (1) set reconciliation delivers O(diff) sync scaling, (2) thread pool crypto offload improves large-blob throughput, and (3) small namespaces show no regression. The existing `deploy/run-benchmark.sh` (1453 lines) already orchestrates a 3-node Docker Compose topology with loadgen, measures ingest throughput/latency across blob sizes, sync convergence timing, and late-joiner catch-up. The v0.6.0 baseline (pre-v0.8.0) is captured in `deploy/results/benchmark-summary.json`.

The primary gap is that the existing benchmark does NOT measure sync wire traffic or reconciliation efficiency. All existing scenarios measure ingest throughput (blobs/sec, latency percentiles) and convergence timing (how long until all nodes have the same blob count). To validate SYNC-10, we need a new scenario that specifically measures O(diff) behavior: preload a large namespace (1000+ blobs), add a small delta (10 blobs), trigger sync, and measure that sync traffic/time is proportional to the delta, not the total namespace size.

**Primary recommendation:** Add 2-3 new benchmark scenarios to the existing `run-benchmark.sh` script, update the report generator to include reconciliation-specific metrics, and run the full suite to produce a comparative report against the v0.6.0 baseline. No new binaries or infrastructure needed -- the existing loadgen, Docker topology, and reporting framework cover everything.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SYNC-10 | Docker benchmark confirms O(diff) improvement over hash-list baseline and no regression for small namespaces | New "reconciliation scaling" scenario measuring sync time/convergence for large namespace + small delta; existing ingest scenarios provide throughput comparison; small-namespace sync latency scenario validates no regression |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Docker Compose v2 | Latest | Multi-node topology orchestration | Already used in deploy/docker-compose.yml |
| bash/jq | System | Benchmark orchestration and JSON result processing | Already used in deploy/run-benchmark.sh |
| chromatindb_loadgen | Current build | Protocol-compliant load generation | Already built in Dockerfile, full CLI with --count/--rate/--size/--ttl |
| SIGUSR1 metrics | Built-in | Runtime metrics dump (blob count, storage MiB) | Already used by get_blob_count() and get_storage_mib() helpers |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| docker stats | Docker CLI | Container CPU/memory snapshots | Already captured via capture_stats() helper |
| bc | System | Floating point comparisons in bash | Already used in wait_for_gc_completion() |

## Architecture Patterns

### Existing Benchmark Infrastructure

```
deploy/
  docker-compose.yml        # 3-node chain topology (node1->node2->node3) + node4 latejoin
  docker-compose.trusted.yml # Override for trusted-mode benchmarks
  run-benchmark.sh          # 1453-line orchestration script (all scenarios + report)
  configs/
    node1.json              # bind, sync_interval=10
    node2.json              # bootstrap=node1, sync_interval=10
    node3.json              # bootstrap=node2, sync_interval=10
    node4-latejoin.json     # bootstrap=node3, sync_interval=10
  results/
    benchmark-summary.json  # v0.6.0 baseline (15.3 blobs/sec for 1M)
    REPORT.md               # v0.6.0 report (markdown)
    scenario-*.json          # Per-scenario raw results
```

### Existing Scenario Pattern

Every benchmark scenario follows the same pattern:
1. `reset_topology` -- `docker compose down -v` then `up -d --wait` (fresh state)
2. `capture_stats "pre" "label"` -- docker stats snapshot before
3. `run_loadgen nodeN --count N --rate N --size N` -- generate load, capture JSON stdout
4. `wait_for_convergence container expected_count timeout` -- poll SIGUSR1 blob count
5. `capture_stats "post" "label"` -- docker stats snapshot after
6. Write result JSON to `$RESULTS_DIR/scenario-{name}.json`

### Pattern: Reconciliation Scaling Scenario (NEW)

**What:** Preload a large namespace, add small delta, measure sync time proportional to delta only.
**When to use:** Validating O(diff) behavior of the reconciliation protocol.
**How it works:**
1. Reset topology with short sync_interval (e.g., 2s) and sync_cooldown_seconds=0
2. Ingest 1000+ blobs to node1 at high rate
3. Wait for full convergence on node2 (all 1000+ blobs)
4. Ingest 10 additional blobs to node1
5. Measure time for node2 to converge at 1010+ blobs
6. Compare this time against a "full re-sync" baseline where no cursors exist

**Key insight:** The sync_interval_seconds dominates convergence timing in Docker benchmarks (timer-driven, not event-driven). With sync_interval=2s, the floor is ~2s regardless of O(diff) vs O(N). The meaningful metric is that sync completes within 1-2 sync intervals for a small delta, even with 1000+ blobs total.

### Pattern: Throughput Comparison (EXISTING, rerun)

**What:** Rerun the existing ingest_1m scenario to measure thread pool improvement.
**Baseline:** v0.6.0 had 15.3 blobs/sec for 1M blobs (96% CPU on node2, serial crypto).
**Expected:** Thread pool offload should improve throughput measurably (parallel crypto on worker threads).
**Measurement:** Same loadgen command, compare blobs_per_sec and CPU distribution.

### Anti-Patterns to Avoid
- **Measuring sync interval instead of protocol cost:** Sync is timer-driven. Measuring "time from write to convergence" will always include 0-N seconds of sync interval. Use SHORT sync intervals (1-2s) to minimize this noise.
- **Comparing wall-clock sync times at different blob counts without controlling for interval:** A 10-blob sync and a 1000-blob sync both take "about sync_interval" seconds in wall-clock terms because sync is periodic. The meaningful comparison is: does a large-namespace sync with small delta behave like a small-namespace sync (vs taking much longer)?
- **Changing too many variables:** The existing benchmark runs scenarios independently with fresh topologies. Do not try to measure reconciliation and thread pool in the same scenario.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Topology orchestration | Custom container management | Existing docker-compose.yml + reset_topology() | Already handles healthcheck, volume cleanup, wait-for-healthy |
| Load generation | Raw TCP client | chromatindb_loadgen binary | Protocol-compliant (PQ handshake, signed blobs), JSON output |
| Blob counting | mdbx direct inspection | SIGUSR1 -> docker logs grep "metrics:" | Already works, non-invasive |
| JSON aggregation | Custom parser | jq (already used throughout) | Entire report generation uses jq |
| Report formatting | Custom formatter | Existing generate_report() pattern | Append new sections using same format_*_row() pattern |

## Common Pitfalls

### Pitfall 1: Sync Cooldown Blocks Rapid Re-sync
**What goes wrong:** Default sync_cooldown_seconds=30 means after one sync round, the next sync request is rejected for 30 seconds. Benchmark needs rapid syncs.
**Why it happens:** Phase 40 added sync rate limiting with 30s default cooldown.
**How to avoid:** Set `"sync_cooldown_seconds": 0` in ALL benchmark node configs. This disables the cooldown check entirely.
**Warning signs:** Sync convergence taking 30+ seconds instead of ~sync_interval.

### Pitfall 2: Cursor Skip Masking Reconciliation Cost
**What goes wrong:** Sync cursors from v0.7.0 skip unchanged namespaces entirely. If the benchmark only adds blobs to one namespace, the cursor indicates "new data exists" and reconciliation runs -- but the cursor_hits metric inflates and the test might not actually exercise the reconciliation path.
**Why it happens:** Cursor-based skip is the first check; reconciliation only runs for namespaces where cursor indicates changes.
**How to avoid:** For the O(diff) test, ensure blobs are added AFTER initial convergence so the cursor correctly triggers reconciliation. The `full_resync_interval` config (default=10) forces a full reconciliation every Nth round regardless of cursor.
**Warning signs:** cursor_hits increasing in SIGUSR1 output when expecting reconciliation to run.

### Pitfall 3: Docker Build Cache Serving Stale Binary
**What goes wrong:** Docker layer caching may serve a pre-v0.8.0 binary if the source hash hasn't changed enough.
**Why it happens:** The Dockerfile uses `--mount=type=cache,target=/src/build/_deps` for FetchContent deps, and CMake incremental builds might not pick up all changes.
**How to avoid:** Always run with fresh build (`docker build --no-cache` or ensure `run-benchmark.sh` does a clean build). The existing script calls `docker build -t chromatindb:latest "$REPO_ROOT"` which should rebuild from source changes.
**Warning signs:** Thread pool not active, old sync protocol messages in logs.

### Pitfall 4: Rate Limiting Interfering with Benchmark
**What goes wrong:** The default rate_limit_bytes_per_sec may throttle loadgen or sync traffic.
**Why it happens:** v0.4.0 added rate limiting; benchmark configs may not disable it.
**How to avoid:** Check that benchmark node configs have `"rate_limit_bytes_per_sec": 0` (disabled). Current configs do not set this, so it defaults to 0 (disabled) -- which is correct.
**Warning signs:** rate_limited counter incrementing in SIGUSR1 output.

### Pitfall 5: Measuring Ingest Rate Instead of Sync Rate for Thread Pool
**What goes wrong:** The loadgen measures ingest throughput (time from send to ACK notification), not sync throughput. Thread pool improvement mainly affects the RECEIVING side during sync.
**Why it happens:** Ingest goes to node1 (direct). Sync replicates to node2/node3 (where verify + hash offload matters).
**How to avoid:** For thread pool validation, the meaningful metric is the INGEST_1M scenario at the loadgen level (since loadgen sends at max rate and measures actual achieved throughput) AND the sync convergence time on downstream nodes. The v0.6.0 baseline of 15.3 blobs/sec for 1M was the loadgen rate, which was bottlenecked by node1's serial crypto processing.
**Warning signs:** Comparing metrics at different measurement points.

## Code Examples

### Adding a New Scenario Function

Based on the existing pattern in run-benchmark.sh:

```bash
# Source: deploy/run-benchmark.sh (existing pattern)
run_scenario_reconciliation_scaling() {
    log "========================================="
    log "Scenario: Reconciliation Scaling (SYNC-10)"
    log "========================================="

    # Use short sync interval + no cooldown for fast convergence
    # Need custom configs with sync_interval=2, sync_cooldown_seconds=0

    reset_topology  # Fresh nodes

    capture_stats "pre" "reconcile-scaling"

    # Phase 1: Preload large namespace (1000+ blobs, 1K each)
    log "Preloading $LARGE_COUNT blobs to node1..."
    local preload_output
    preload_output=$(run_loadgen node1 --count "$LARGE_COUNT" --rate 50 --size 1024 --drain-timeout 30)

    # Phase 2: Wait for full convergence on node2
    log "Waiting for node2 convergence at $LARGE_COUNT..."
    wait_for_convergence chromatindb-node2 "$LARGE_COUNT" 300 >/dev/null

    # Phase 3: Add small delta
    log "Adding $DELTA_COUNT new blobs to node1..."
    local delta_start_ns=$(date +%s%N)
    run_loadgen node1 --count "$DELTA_COUNT" --rate 50 --size 1024 --drain-timeout 10 >/dev/null

    # Phase 4: Measure convergence time for delta only
    local expected_total=$(( LARGE_COUNT + DELTA_COUNT ))
    local delta_sync_ms
    delta_sync_ms=$(wait_for_convergence chromatindb-node2 "$expected_total" 120)

    capture_stats "post" "reconcile-scaling"

    # Build result JSON
    jq -n --argjson large "$LARGE_COUNT" --argjson delta "$DELTA_COUNT" \
           --argjson sync_ms "$delta_sync_ms" \
        '{ scenario: "reconcile-scaling",
           preload_blobs: $large, delta_blobs: $delta,
           delta_sync_ms: $sync_ms }' \
        > "$RESULTS_DIR/scenario-reconcile-scaling.json"
}
```

### Extracting Sync Metrics from Docker Logs

```bash
# Source: existing get_blob_count() pattern in deploy/run-benchmark.sh
get_sync_metrics() {
    local container="$1"
    docker kill -s USR1 "$container" >/dev/null 2>&1 || true
    sleep 1
    # Extract cursor_hits, cursor_misses, syncs from latest metrics line
    docker logs "$container" 2>&1 | grep "metrics:" | tail -1
}
```

### Config for Benchmark Nodes (short sync interval, no cooldown)

```json
{
  "bind_address": "0.0.0.0:4200",
  "sync_interval_seconds": 2,
  "sync_cooldown_seconds": 0,
  "full_resync_interval": 0,
  "log_level": "info"
}
```

Note: `full_resync_interval=0` means every sync round uses reconciliation (no forced full-range). This is correct for benchmarking reconciliation efficiency.

### Comparing Baseline vs Current in Report

```bash
# Pattern for comparing v0.6.0 baseline vs current
local baseline_1m_bps=15.3  # From deploy/results/benchmark-summary.json
local current_1m_bps=$(jq -r '.blobs_per_sec' "$RESULTS_DIR/scenario-ingest-1m.json")
local improvement=$(jq -n --argjson base "$baseline_1m_bps" --argjson curr "$current_1m_bps" \
    '(($curr - $base) / $base * 100) | . * 10 | round / 10')
```

## State of the Art

| Old Approach (pre-v0.8.0) | Current Approach (v0.8.0) | When Changed | Impact on Benchmarks |
|---------------------------|---------------------------|--------------|---------------------|
| O(N) hash list exchange (HashList type 12) | O(diff) XOR-fingerprint reconciliation (ReconcileInit/Ranges/Items types 27/28/29) | Phase 39 | Sync time for large namespace + small delta should be dramatically lower |
| Serial ML-DSA-87 verify on event loop | Thread pool offload (asio::thread_pool) | Phase 38 | 1M blob ingest/sync throughput should improve (event loop unblocked) |
| No sync rate limiting | Cooldown + session limit + byte accounting | Phase 40 | Must disable cooldown in benchmarks (sync_cooldown_seconds=0) |
| HashList message type 12 | REMOVED -- replaced by types 27/28/29 | Phase 39 | Wire protocol incompatible with pre-v0.8.0 nodes |

## Key Design Decisions for Benchmarks

### 1. Three-Scenario Approach

The phase has three distinct success criteria mapping to three measurements:

| Success Criterion | Scenario | Metric |
|-------------------|----------|--------|
| O(diff) scaling | reconcile-scaling | Delta sync time << full namespace sync time |
| 1M throughput improvement | ingest-1m (rerun) | blobs/sec > 15.3 baseline |
| No small-namespace regression | ingest-1k + sync-latency (rerun) | Within 5% of baseline |

### 2. Baseline Data Already Captured

The v0.6.0 baseline is fully captured in `deploy/results/benchmark-summary.json`:
- ingest_1k: 50.2 blobs/sec, p50=21.82ms
- ingest_100k: 50.2 blobs/sec, p50=24.21ms
- ingest_1m: 15.3 blobs/sec, p50=4823.76ms
- sync_latency: 1-hop=13210ms, 2-hop=1059ms (for 200 1K blobs)
- latejoin: 1036ms (200 blobs)

### 3. Config Requirements for Benchmark

For the reconciliation scaling scenario, nodes need faster sync and no cooldown:
- `sync_interval_seconds: 2` -- faster convergence measurement
- `sync_cooldown_seconds: 0` -- no rate limiting interference
- `full_resync_interval: 0` -- disable forced full resyncs (let cursors work naturally)

For the existing scenarios (ingest throughput, sync latency), keep `sync_interval_seconds: 10` for direct baseline comparison.

### 4. Report Sections to Add

The reconciliation-specific results need new sections in the report:
- Reconciliation Scaling: preload count, delta count, delta sync time
- Throughput Comparison: v0.6.0 baseline vs v0.8.0 current, improvement percentage
- Regression Check: small-namespace metrics within 5% of baseline

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (unit tests) + Docker benchmark suite (integration) |
| Config file | db/CMakeLists.txt (Catch2) + deploy/run-benchmark.sh (Docker) |
| Quick run command | `bash deploy/run-benchmark.sh --skip-build` (reuses existing image) |
| Full suite command | `bash deploy/run-benchmark.sh` (rebuilds image + all scenarios) |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SYNC-10 (O(diff)) | Reconciliation scaling with large namespace + small delta | integration (Docker) | `bash deploy/run-benchmark.sh` (new scenario) | Partially -- need new reconcile-scaling scenario |
| SYNC-10 (throughput) | 1M blob ingest improvement over 15.3 baseline | integration (Docker) | `bash deploy/run-benchmark.sh` (existing ingest-1m scenario) | Yes -- scenario exists |
| SYNC-10 (regression) | Small namespace no regression within 5% | integration (Docker) | `bash deploy/run-benchmark.sh` (existing ingest-1k + sync scenarios) | Yes -- scenarios exist |

### Sampling Rate
- **Per task commit:** Manual verification of script changes via `bash -n deploy/run-benchmark.sh` (syntax check)
- **Per wave merge:** Full benchmark run: `bash deploy/run-benchmark.sh`
- **Phase gate:** Benchmark report shows all three criteria met

### Wave 0 Gaps
- [ ] New reconcile-scaling scenario function in `deploy/run-benchmark.sh`
- [ ] Benchmark node configs for short sync interval + no cooldown
- [ ] Report generation updates for reconciliation scaling and comparison sections
- [ ] The full benchmark run takes ~10-15 minutes (Docker build + all scenarios)

## Open Questions

1. **Wire traffic measurement granularity**
   - What we know: Docker logs contain per-sync metadata (cursor_hits, cursor_misses, syncs count). There is no per-sync byte counter in NodeMetrics.
   - What's unclear: Whether we need to add wire-level byte counting or if sync convergence TIME is a sufficient proxy for O(diff) behavior.
   - Recommendation: Use convergence time as the primary metric. For a large namespace (1000 blobs) with a small delta (10 blobs), if reconciliation-based sync converges in ~1 sync interval (like a small namespace), that demonstrates O(diff). Adding per-sync byte counters would require code changes beyond the benchmark scope and is not needed to prove the scaling claim.

2. **Exact blob count for "large namespace"**
   - What we know: Success criteria says 1000+. Loadgen supports arbitrary counts.
   - What's unclear: Whether 1000 blobs is large enough to show a measurable difference, and how long preloading takes at 50 blobs/sec (~20s for 1000 1K blobs).
   - Recommendation: Use 1000 blobs for the preload (manageable within Docker resource constraints, 20s ingest time). This is large enough that O(N) hash list exchange would have been ~32KB wire overhead per sync, while O(diff) with 10-blob delta is ~320 bytes.

3. **Whether to keep or archive the v0.6.0 baseline**
   - What we know: `deploy/results/benchmark-summary.json` contains the v0.6.0 baseline. Running the full benchmark suite will overwrite it.
   - What's unclear: Whether to archive the v0.6.0 baseline before overwriting.
   - Recommendation: Copy the existing results to `deploy/results/v0.6.0-baseline/` before running the new benchmark, then reference those values in the comparison report.

## Sources

### Primary (HIGH confidence)
- `deploy/run-benchmark.sh` -- 1453-line benchmark orchestration script (read in full)
- `deploy/results/benchmark-summary.json` -- v0.6.0 baseline data
- `deploy/results/REPORT.md` -- v0.6.0 benchmark report format
- `deploy/docker-compose.yml` -- 3-node topology definition
- `loadgen/loadgen_main.cpp` -- Load generator implementation (833 lines)
- `db/sync/reconciliation.h` / `db/sync/reconciliation.cpp` -- Reconciliation module (415 lines)
- `db/crypto/thread_pool.h` -- Thread pool offload helper
- `db/config/config.h` -- Config struct with all relevant fields
- `db/peer/peer_manager.h` -- NodeMetrics struct, sync flow
- `.planning/REQUIREMENTS.md` -- SYNC-10 requirement definition
- `.planning/ROADMAP.md` -- Phase 41 success criteria

### Secondary (MEDIUM confidence)
- `db/PROTOCOL.md` -- Full wire protocol documentation (Phase B reconciliation details)
- `db/README.md` -- Config documentation, SIGUSR1 metrics format

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - all tools already exist and are proven in v0.6.0 benchmarks
- Architecture: HIGH - existing benchmark infrastructure is well-understood (1453 lines read)
- Pitfalls: HIGH - identified from actual config defaults and code inspection

**Research date:** 2026-03-19
**Valid until:** 2026-04-19 (30 days -- stable infrastructure, no external dependencies)
