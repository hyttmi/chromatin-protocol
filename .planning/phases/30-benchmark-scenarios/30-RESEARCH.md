# Phase 30: Benchmark Scenarios - Research

**Researched:** 2026-03-16
**Domain:** Bash benchmark scripting, Docker stats resource profiling, chromatindb load generation orchestration
**Confidence:** HIGH

## Summary

Phase 30 creates the `run-benchmark.sh` orchestration script that builds images, starts the Docker Compose topology, runs five benchmark scenarios (ingest throughput, sync latency, multi-hop propagation, late-joiner catch-up, trusted vs PQ handshake), captures resource metrics via `docker stats`, and outputs per-scenario JSON results. All infrastructure exists: the load generator (Phase 28) outputs JSON to stdout, the 3-node chain topology (Phase 29) is ready, and the Dockerfile (Phase 27) builds all binaries.

The central challenge is measuring sync latency and propagation time. The chromatindb node does not expose a query API -- there is no "check if blob exists" RPC. The measurement strategy uses the SIGUSR1 metrics dump: send blobs to node1, then poll node3's `blobs=N` metric via `docker kill -s USR1` + log parsing until the blob count matches the expected total. Wall-clock time from loadgen completion to convergence gives sync latency. For the trusted-vs-PQ comparison (PERF-05), alternate config files with `trusted_peers` set to Docker container IPs are needed, requiring a startup-time IP resolution step.

**Primary recommendation:** Build a single `deploy/run-benchmark.sh` script that runs 5 scenarios sequentially, captures `docker stats --no-stream` snapshots before/during/after each scenario, and writes per-scenario JSON files to `results/`. A small helper function uses SIGUSR1 + log grep to poll blob counts on target nodes. Trusted-mode configs are generated at runtime by resolving container IPs.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| PERF-01 | Ingest throughput measured at multiple blob sizes (blobs/sec, MiB/sec, p50/p95/p99 latency) | Loadgen already outputs all these metrics as JSON. Run 3 loadgen invocations (1K, 100K, 1M fixed sizes) against node1 via host port 4201, capture JSON output. |
| PERF-02 | Sync/replication latency measured (wall-clock from write on A to availability on B) | Write N blobs to node1, then poll node2 blob count via SIGUSR1 + log grep. Wall-clock delta from loadgen finish to node2 convergence = sync latency. |
| PERF-03 | Multi-hop propagation time (A->B->C chain, no direct A-C link) | Same as PERF-02 but poll node3 (2-hop). Chain topology ensures node3 only gets blobs via node2. |
| PERF-04 | Late-joiner catch-up time (new node joins after data loaded, measures convergence) | Load blobs to node1, wait for node3 convergence, then start node4 (latejoin profile), poll node4 blob count until convergence. |
| PERF-05 | Trusted vs PQ handshake comparison with same workload | Run same loadgen workload twice: first with default PQ configs, then with configs containing `trusted_peers` (container IPs). Compare latencies. |
| OBS-01 | Resource profiling captures CPU, memory, disk I/O per container via docker stats | `docker stats --no-stream --format json` captures all metrics in one call. Run before, during (background), and after each scenario. |
| LOAD-04 | run-benchmark.sh script builds images, starts topology, runs all scenarios, collects metrics, generates report | Single bash script in deploy/ that orchestrates everything. Phase 31 will handle the markdown report generation. |
</phase_requirements>

## Standard Stack

### Core
| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| bash | 5.x | Benchmark orchestration script | Available in all environments; no dependencies |
| docker compose | v2 (built-in) | Topology management | Already set up in Phase 29 |
| chromatindb_loadgen | (project) | Load generation with JSON output | Built in Phase 28, already in Docker image |
| docker stats | (built-in) | CPU/memory/disk I/O per container | The requirement (OBS-01) specifies this tool |
| jq | 1.7+ | JSON parsing of loadgen output and docker stats | Standard CLI tool; may need install in script |

### Supporting
| Tool | Version | Purpose | When to Use |
|------|---------|---------|-------------|
| date | GNU coreutils | Wall-clock timestamps for sync latency | Used in timing measurements |
| grep | GNU | Parse SIGUSR1 metrics from container logs | Used to extract blob counts |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| SIGUSR1 + log grep for blob count | Add HTTP API to node | Violates project constraint (no HTTP API); SIGUSR1 is the existing observability mechanism |
| jq for JSON parsing | bash string manipulation | jq is cleaner and less error-prone; worth the dependency |
| Sequential scenarios | Parallel scenarios | Sequential is simpler and prevents resource contention between scenarios |

**Installation:**
```bash
# jq is the only potential dependency not already present
apt-get install -y jq  # Or: brew install jq (macOS)
```

## Architecture Patterns

### Recommended Project Structure
```
deploy/
  run-benchmark.sh           # Main orchestration script (LOAD-04)
  docker-compose.yml         # Existing (Phase 29)
  configs/
    node1.json               # Existing
    node2.json               # Existing
    node3.json               # Existing
    node4-latejoin.json      # Existing
    node1-trusted.json       # Generated at runtime (PERF-05)
    node2-trusted.json       # Generated at runtime (PERF-05)
    node3-trusted.json       # Generated at runtime (PERF-05)
  results/                   # Created by script, holds per-scenario JSON
    scenario-ingest-1k.json
    scenario-ingest-100k.json
    scenario-ingest-1m.json
    scenario-sync-latency.json
    scenario-multihop.json
    scenario-latejoin.json
    scenario-trusted-vs-pq.json
    docker-stats-*.json
```

### Pattern 1: Scenario Runner Structure
**What:** Each benchmark scenario is a bash function that follows a consistent pattern: setup, run, measure, teardown, output JSON.
**When to use:** Every scenario.
**Example:**
```bash
run_scenario_ingest() {
    local size="$1" label="$2"
    log "=== Scenario: Ingest $label ==="

    # Clean state
    reset_topology
    wait_healthy

    # Capture baseline docker stats
    capture_stats "pre" "$label"

    # Run loadgen (connects to node1 via host port 4201)
    local output
    output=$(docker run --rm --network deploy_chromatindb-net \
        chromatindb:latest \
        chromatindb_loadgen --target node1:4200 \
        --count "$BLOB_COUNT" --rate "$RATE" --size "$size" \
        --drain-timeout 10 2>/dev/null)

    # Capture post-scenario stats
    capture_stats "post" "$label"

    # Write result
    echo "$output" > "results/scenario-ingest-${label}.json"
}
```

### Pattern 2: Blob Count Polling via SIGUSR1
**What:** To measure when a node has received all expected blobs, send SIGUSR1 and parse the metrics log line for `blobs=N`.
**When to use:** PERF-02, PERF-03, PERF-04 (sync latency, multi-hop, late-joiner).
**Example:**
```bash
get_blob_count() {
    local container="$1"
    # Send SIGUSR1 to trigger metrics dump
    docker kill -s USR1 "$container" 2>/dev/null
    sleep 0.5
    # Parse the most recent metrics line for blob count
    docker logs "$container" 2>&1 | grep "metrics:" | tail -1 | \
        grep -oP 'blobs=\K[0-9]+'
}

wait_for_convergence() {
    local container="$1" expected="$2" timeout="$3"
    local start elapsed count
    start=$(date +%s%N)
    while true; do
        count=$(get_blob_count "$container")
        if [[ "$count" -ge "$expected" ]]; then
            elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
            echo "$elapsed"  # milliseconds
            return 0
        fi
        elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
        if [[ "$elapsed" -gt "$((timeout * 1000))" ]]; then
            echo "$elapsed"
            return 1  # timeout
        fi
        sleep 1
    done
}
```

### Pattern 3: Docker Stats JSON Capture
**What:** `docker stats --no-stream --format '{{json .}}'` captures a single snapshot of CPU/memory/disk I/O for all running containers in JSON format.
**When to use:** Before, during, and after each scenario for OBS-01.
**Example:**
```bash
capture_stats() {
    local phase="$1" label="$2"
    docker stats --no-stream --format '{"container":"{{.Name}}","cpu":"{{.CPUPerc}}","mem_usage":"{{.MemUsage}}","mem_perc":"{{.MemPerc}}","net_io":"{{.NetIO}}","block_io":"{{.BlockIO}}","pids":"{{.PIDs}}"}' \
        chromatindb-node1 chromatindb-node2 chromatindb-node3 \
        > "results/docker-stats-${label}-${phase}.json"
}
```

### Pattern 4: Trusted Config Generation at Runtime
**What:** For PERF-05, generate config files with `trusted_peers` containing the actual Docker container IP addresses. Docker assigns IPs dynamically, so resolve them after containers are running.
**When to use:** PERF-05 trusted vs PQ comparison only.
**Example:**
```bash
generate_trusted_configs() {
    local ip1 ip2 ip3
    ip1=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' chromatindb-node1)
    ip2=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' chromatindb-node2)
    ip3=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' chromatindb-node3)

    # Generate node1 trusted config
    cat > deploy/configs/node1-trusted.json <<EOF
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "info",
  "sync_interval_seconds": 10,
  "trusted_peers": ["$ip2", "$ip3"]
}
EOF
    # Similar for node2, node3...
}
```

### Pattern 5: Loadgen via docker run on the Compose Network
**What:** Run the loadgen as a temporary container on the same Docker network as the nodes, so it can reach them by service name.
**When to use:** All scenarios. Running loadgen inside the Docker network avoids host port mapping issues and gives direct access to all nodes.
**Example:**
```bash
docker run --rm --network deploy_chromatindb-net \
    chromatindb:latest \
    chromatindb_loadgen --target node1:4200 \
    --count 100 --rate 50 --size 1024 --drain-timeout 10 \
    2>/dev/null
```
Note: The network name follows Docker Compose convention: `{project}_{network}`. With `docker compose -f deploy/docker-compose.yml`, the project defaults to `deploy`, so the network is `deploy_chromatindb-net`.

### Anti-Patterns to Avoid
- **Running loadgen from host via host ports:** While possible (ports 4201-4204 are mapped), running loadgen in the Docker network avoids NAT overhead and keeps measurements consistent with inter-node communication.
- **Parsing spdlog colored output:** Container logs may include ANSI color codes if spdlog detects a TTY. The `docker logs` output is typically plain text, but use `grep -oP` with the uncolored pattern `blobs=\d+` to be safe.
- **Hardcoding container IPs:** Docker container IPs are dynamic. Always resolve at runtime via `docker inspect` for trusted_peers configs.
- **Restarting containers between every scenario:** Only restart when the scenario requires clean state (e.g., ingest benchmarks need empty storage). Replication scenarios can reuse loaded data.
- **Running loadgen with `--mixed` for PERF-01:** PERF-01 requires measurements "at multiple blob sizes" -- use separate fixed-size runs (1K, 100K, 1M) for clean per-size metrics, not mixed mode.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON output formatting | Manual bash string concatenation | jq for construction and parsing | Handles escaping, quoting, nesting correctly |
| Container resource metrics | Manual /proc parsing inside containers | `docker stats --no-stream --format json` | Built-in, standardized, captures CPU/mem/disk/net |
| Blob count verification | Custom RPC/API to query node state | SIGUSR1 + log grep for `blobs=N` | Existing mechanism, no code changes needed |
| Timing measurement | External time command | bash `date +%s%N` for nanosecond precision | Portable, no dependency |
| Config file templating | sed/awk on JSON | cat heredoc with variable interpolation | JSON is too structured for sed; heredoc is clean |

**Key insight:** The benchmark script is pure orchestration. All measurement happens in existing tools: loadgen outputs latency/throughput JSON, docker stats outputs resource JSON, SIGUSR1 outputs blob counts. The script just sequences operations and captures timing.

## Common Pitfalls

### Pitfall 1: Docker Network Name Mismatch
**What goes wrong:** `docker run --network deploy_chromatindb-net` fails because the network name doesn't match.
**Why it happens:** Docker Compose network name is `{project}_{network}`. The project name defaults to the directory name of the compose file, which is `deploy` when using `-f deploy/docker-compose.yml`.
**How to avoid:** Use `docker compose -f deploy/docker-compose.yml` consistently (sets project to `deploy`), or explicitly set `--project-name` / use `docker network ls` to discover the actual name. Alternatively, pass `-p chromatindb` to docker compose for a predictable project name.
**Warning signs:** "network deploy_chromatindb-net not found" errors.

### Pitfall 2: SIGUSR1 Timing Window
**What goes wrong:** `docker kill -s USR1` sends the signal, but the log line is not immediately available. `docker logs | grep` immediately after may miss it.
**Why it happens:** spdlog buffers output; Docker log driver has latency.
**How to avoid:** Add a 0.5-1 second sleep between SIGUSR1 and log parsing. Use `tail -1` on the filtered output to get the most recent metrics line.
**Warning signs:** Blob count reads as stale (previous value) or empty.

### Pitfall 3: Sync Interval Dominates Latency
**What goes wrong:** Measured sync latency is always close to 10 seconds (the sync interval), making it seem like a constant rather than measuring actual sync speed.
**Why it happens:** Sync is timer-driven at 10-second intervals. A blob written at t=1 won't be synced until the next sync round fires at ~t=10.
**How to avoid:** This is expected behavior -- document that sync latency includes the sync interval wait time. The measurement captures real-world end-to-end latency, which includes this scheduling delay. For the benchmark report, note the sync interval alongside the measurements.
**Warning signs:** All sync latency values cluster around 10s (1-hop) or 20s (2-hop). This is correct.

### Pitfall 4: Stale Data Between Scenarios
**What goes wrong:** A scenario measures ingest performance, but the storage already contains blobs from the previous scenario. This affects latency (more data = slower indexing) and makes blob count verification unreliable.
**Why it happens:** Named Docker volumes persist across container restarts.
**How to avoid:** Between scenarios that need clean state, call `docker compose down -v` to remove volumes, then `docker compose up -d` to restart fresh. Volume removal is fast for small datasets.
**Warning signs:** Blob counts higher than expected at scenario start.

### Pitfall 5: Loadgen Connects Before Node Handshake Ready
**What goes wrong:** Loadgen container starts before the target node is fully healthy. Connection fails or handshake times out.
**Why it happens:** `docker run` doesn't wait for compose service health.
**How to avoid:** After `docker compose up -d`, run `docker compose -f deploy/docker-compose.yml wait node1 node2 node3` (Docker Compose v2.31+) or poll healthcheck status with a loop before starting loadgen.
**Warning signs:** "failed to connect" or "handshake failed" in loadgen stderr.

### Pitfall 6: Trusted Peers Config with Docker DNS Names
**What goes wrong:** Setting `trusted_peers: ["node1", "node2"]` in config -- trusted_peers requires IP addresses, not hostnames.
**Why it happens:** Docker Compose uses DNS names for service discovery, but chromatindb validates trusted_peers as IP addresses at config load time.
**How to avoid:** Use `docker inspect` to get actual container IPs. Generate trusted config files at runtime after containers are started.
**Warning signs:** "Invalid trusted_peer: not a valid IP address" in node logs on startup.

### Pitfall 7: PQ vs Trusted Comparison Requires Full Restart
**What goes wrong:** SIGHUP can reload trusted_peers at runtime, but existing connections keep their handshake type. New connections use the updated trust list, but previously established connections don't re-handshake.
**Why it happens:** Handshake happens once at connection establishment.
**How to avoid:** For a fair comparison, do a full `docker compose down -v` + `docker compose up -d` between PQ and trusted runs. This ensures fresh handshakes under each mode.
**Warning signs:** Mixed handshake types in the same measurement window.

## Code Examples

### Complete Scenario: Ingest Throughput at Multiple Sizes (PERF-01)
```bash
BLOB_COUNT=200
RATE=50
COMPOSE_FILE="deploy/docker-compose.yml"
COMPOSE="docker compose -f $COMPOSE_FILE -p chromatindb"
NETWORK="chromatindb_chromatindb-net"

for size_bytes in 1024 102400 1048576; do
    label=$(format_size_label "$size_bytes")  # "1k", "100k", "1m"

    # Clean state
    $COMPOSE down -v 2>/dev/null
    $COMPOSE up -d --wait

    # Capture pre-stats
    capture_stats "pre" "ingest-${label}"

    # Run loadgen inside the Docker network
    result=$(docker run --rm --network "$NETWORK" \
        chromatindb:latest \
        chromatindb_loadgen --target node1:4200 \
        --count "$BLOB_COUNT" --rate "$RATE" --size "$size_bytes" \
        --drain-timeout 10 2>/dev/null)

    # Capture post-stats
    capture_stats "post" "ingest-${label}"

    echo "$result" > "results/scenario-ingest-${label}.json"
done
```

### Complete Scenario: Sync Latency + Multi-Hop (PERF-02, PERF-03)
```bash
# Start fresh topology
$COMPOSE down -v 2>/dev/null
$COMPOSE up -d --wait

# Load blobs to node1
start_ns=$(date +%s%N)
docker run --rm --network "$NETWORK" \
    chromatindb:latest \
    chromatindb_loadgen --target node1:4200 \
    --count "$BLOB_COUNT" --rate "$RATE" --size 1024 \
    --drain-timeout 10 2>/dev/null > results/scenario-sync-loadgen.json

# Measure time until node2 has all blobs (1-hop sync latency)
sync_1hop_ms=$(wait_for_convergence "chromatindb-node2" "$BLOB_COUNT" 120)

# Measure time until node3 has all blobs (2-hop propagation)
sync_2hop_ms=$(wait_for_convergence "chromatindb-node3" "$BLOB_COUNT" 120)

# Record results
jq -n \
    --arg blobs "$BLOB_COUNT" \
    --arg sync_1hop "$sync_1hop_ms" \
    --arg sync_2hop "$sync_2hop_ms" \
    '{scenario:"sync-latency", blob_count:($blobs|tonumber), sync_1hop_ms:($sync_1hop|tonumber), sync_2hop_ms:($sync_2hop|tonumber)}' \
    > results/scenario-sync-latency.json
```

### Complete Scenario: Late-Joiner Catch-Up (PERF-04)
```bash
# Topology already has blobs from sync scenario (or load fresh)
# Start late-joiner node4
$COMPOSE --profile latejoin up -d node4
docker compose -f "$COMPOSE_FILE" -p chromatindb wait node4 2>/dev/null || sleep 15

start_ns=$(date +%s%N)
catchup_ms=$(wait_for_convergence "chromatindb-node4" "$BLOB_COUNT" 300)

jq -n \
    --arg blobs "$BLOB_COUNT" \
    --arg catchup "$catchup_ms" \
    '{scenario:"late-joiner", blob_count:($blobs|tonumber), catchup_ms:($catchup|tonumber)}' \
    > results/scenario-latejoin.json
```

### Complete Scenario: Trusted vs PQ (PERF-05)
```bash
# Run 1: PQ mode (default -- no trusted_peers)
$COMPOSE down -v 2>/dev/null
$COMPOSE up -d --wait

pq_result=$(docker run --rm --network "$NETWORK" \
    chromatindb:latest \
    chromatindb_loadgen --target node1:4200 \
    --count "$BLOB_COUNT" --rate "$RATE" --size 1024 \
    --drain-timeout 10 2>/dev/null)

# Run 2: Trusted mode
$COMPOSE down -v 2>/dev/null
generate_trusted_configs  # Creates node*-trusted.json with container IPs
$COMPOSE up -d --wait     # After modifying compose or using override

trusted_result=$(docker run --rm --network "$NETWORK" \
    chromatindb:latest \
    chromatindb_loadgen --target node1:4200 \
    --count "$BLOB_COUNT" --rate "$RATE" --size 1024 \
    --drain-timeout 10 2>/dev/null)

# Combine into comparison JSON
jq -n --argjson pq "$pq_result" --argjson trusted "$trusted_result" \
    '{scenario:"trusted-vs-pq", pq_mode:$pq, trusted_mode:$trusted}' \
    > results/scenario-trusted-vs-pq.json
```

### Docker Stats JSON Capture (OBS-01)
```bash
capture_stats() {
    local phase="$1" label="$2"
    docker stats --no-stream \
        --format '{"container":"{{.Name}}","cpu":"{{.CPUPerc}}","mem_usage":"{{.MemUsage}}","mem_perc":"{{.MemPerc}}","net_io":"{{.NetIO}}","block_io":"{{.BlockIO}}","pids":"{{.PIDs}}"}' \
        chromatindb-node1 chromatindb-node2 chromatindb-node3 \
        2>/dev/null | jq -s '.' > "results/docker-stats-${label}-${phase}.json"
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| docker-compose exec for commands | docker run --rm --network for one-shot tools | Docker Compose v2 | Cleaner for transient workloads like loadgen |
| docker stats streaming + awk | docker stats --no-stream --format json | Docker 20.10+ | Single snapshot, JSON output, no streaming complexity |
| Manual timing with `time` command | `date +%s%N` for nanosecond bash timestamps | GNU coreutils | Sub-millisecond timing in bash without external tools |
| docker compose wait (polling) | `docker compose wait` (v2.31+) | Dec 2024 | Native wait for service health; falls back to polling loop on older versions |

**Deprecated/outdated:**
- `docker-compose` (hyphenated): Use `docker compose` (space-separated, v2 CLI)

## Open Questions

1. **Blob count for sync verification**
   - What we know: SIGUSR1 dumps `blobs=N` which sums `latest_seq_num` across all namespaces. Each loadgen run uses a fresh ephemeral identity (new namespace), so blob_count = total blobs ever stored on that node.
   - What's unclear: Does `latest_seq_num` count only current (non-expired, non-tombstoned) blobs, or does it include all historical seq nums? Since blobs have TTL=3600 and benchmarks run for seconds, expiry is not a concern.
   - Recommendation: Use `blobs=N` metric directly. For benchmark durations (<5 minutes), no TTL expiry will occur.

2. **Trusted mode config delivery for PERF-05**
   - What we know: trusted_peers requires IP addresses. Container IPs are assigned at startup.
   - Two approaches: (a) Start nodes with default configs, resolve IPs, generate trusted configs, SIGHUP to reload. (b) Use a docker-compose override file that mounts trusted configs, requiring a full restart.
   - Recommendation: Approach (a) is simpler -- SIGHUP reloads trusted_peers without restart. But existing connections keep their handshake type, so we still need a restart of all inter-node connections. Full restart (approach b) is cleaner for a fair comparison. Use approach (b): generate trusted configs, `docker compose down -v`, mount trusted configs via an override, `docker compose up -d`.

3. **docker compose wait availability**
   - What we know: `docker compose wait` was added in v2.31 (Dec 2024). May not be available on all systems.
   - Recommendation: Use a fallback polling function: check `docker inspect --format '{{.State.Health.Status}}'` in a loop if `docker compose wait` is not available.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Manual bash execution + output inspection |
| Config file | None (bash script) |
| Quick run command | `bash deploy/run-benchmark.sh` (runs all scenarios) |
| Full suite command | Same as quick run |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| PERF-01 | Ingest throughput at multiple sizes | integration | `bash deploy/run-benchmark.sh` then verify `results/scenario-ingest-*.json` exist and contain blobs_per_sec, mib_per_sec, latency_ms | No -- Wave 0 |
| PERF-02 | Sync latency (A to B) | integration | Verify `results/scenario-sync-latency.json` contains sync_1hop_ms > 0 | No -- Wave 0 |
| PERF-03 | Multi-hop propagation (A to C) | integration | Verify `results/scenario-sync-latency.json` contains sync_2hop_ms > sync_1hop_ms | No -- Wave 0 |
| PERF-04 | Late-joiner catch-up | integration | Verify `results/scenario-latejoin.json` contains catchup_ms > 0 | No -- Wave 0 |
| PERF-05 | Trusted vs PQ comparison | integration | Verify `results/scenario-trusted-vs-pq.json` contains both pq_mode and trusted_mode | No -- Wave 0 |
| OBS-01 | Docker stats capture per container | integration | Verify `results/docker-stats-*.json` files exist and contain CPU/mem data | No -- Wave 0 |
| LOAD-04 | run-benchmark.sh orchestrates everything | integration | Script exits 0 and results/ directory is populated | No -- Wave 0 |

### Sampling Rate
- **Per task commit:** Script syntax check (`bash -n deploy/run-benchmark.sh`)
- **Per wave merge:** Run the full benchmark suite and verify results/ output
- **Phase gate:** All 7 result JSON files present with valid content

### Wave 0 Gaps
- [ ] `deploy/run-benchmark.sh` -- main benchmark script
- [ ] `deploy/results/` directory -- created by script
- [ ] `jq` availability check -- script should verify jq is installed

## Sources

### Primary (HIGH confidence)
- **Codebase analysis** -- loadgen/loadgen_main.cpp (JSON output format verified), deploy/docker-compose.yml (topology verified), db/config/config.h (trusted_peers field verified), db/peer/peer_manager.cpp (SIGUSR1 metrics format verified: `blobs=N` pattern)
- **Docker CLI** -- `docker stats --help` (confirmed --no-stream, --format json support)
- **Phase 28 research** -- Loadgen outputs JSON to stdout with blobs_per_sec, mib_per_sec, latency_ms.p50/p95/p99
- **Phase 29 research** -- Chain topology (node1->node2->node3), latejoin profile, 10s sync interval

### Secondary (MEDIUM confidence)
- Docker Compose v2 `wait` command -- available since v2.31 (Dec 2024), may require fallback for older versions

### Tertiary (LOW confidence)
- None. All findings are from direct codebase and tool inspection.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all tools are bash, docker, jq (standard CLI tooling)
- Architecture: HIGH -- loadgen JSON output verified, SIGUSR1 metrics format verified, docker stats format verified
- Pitfalls: HIGH -- identified from actual infrastructure: network naming, SIGUSR1 timing, trusted_peers IP requirement, sync interval dominance
- Measurement approach: HIGH for PERF-01 (direct loadgen output), MEDIUM for PERF-02/03/04 (SIGUSR1 polling has ~1s granularity)

**Research date:** 2026-03-16
**Valid until:** 2026-04-16 (stable -- internal project infrastructure)
