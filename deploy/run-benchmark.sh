#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# chromatindb Benchmark Orchestration Script
#
# Runs performance scenarios against a Docker Compose multi-node topology.
# Produces per-scenario JSON results in deploy/results/.
#
# Usage:
#   bash deploy/run-benchmark.sh [--skip-build] [--report-only]
#
# Prerequisites:
#   - Docker with Compose v2
#   - jq
#   - chromatindb Dockerfile at repo root
# ============================================================================

# --- Configuration -----------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"
COMPOSE="docker compose -f $COMPOSE_FILE -p chromatindb"
NETWORK="chromatindb_chromatindb-net"
RESULTS_DIR="$SCRIPT_DIR/results"
BLOB_COUNT=200
RATE=50
SKIP_BUILD=false
REPORT_ONLY=false

# --- Utility Functions -------------------------------------------------------

log() {
    echo "[$(date +%H:%M:%S)] $*" >&2
}

check_deps() {
    local missing=()
    command -v docker >/dev/null 2>&1 || missing+=("docker")
    docker compose version >/dev/null 2>&1 || missing+=("docker compose")
    command -v jq >/dev/null 2>&1 || missing+=("jq")

    if [[ ${#missing[@]} -gt 0 ]]; then
        log "ERROR: Missing dependencies: ${missing[*]}"
        exit 1
    fi
    log "Dependencies OK: docker, docker compose, jq"
}

build_image() {
    if [[ "$SKIP_BUILD" == true ]]; then
        log "Skipping image build (--skip-build)"
        return 0
    fi
    log "Building chromatindb:latest image..."
    docker build -t chromatindb:latest "$REPO_ROOT"
    log "Image build complete"
}

reset_topology() {
    log "Resetting topology (down -v, up -d)..."
    $COMPOSE down -v 2>/dev/null || true
    if $COMPOSE up -d --wait 2>/dev/null; then
        log "Topology ready (--wait succeeded)"
    else
        # Fallback: poll healthcheck status for each node
        log "Fallback: polling healthcheck status..."
        local timeout=30
        local start
        start=$(date +%s)
        for node in chromatindb-node1 chromatindb-node2 chromatindb-node3; do
            while true; do
                local status
                status=$(docker inspect --format '{{.State.Health.Status}}' "$node" 2>/dev/null || echo "missing")
                if [[ "$status" == "healthy" ]]; then
                    log "$node is healthy"
                    break
                fi
                local elapsed=$(( $(date +%s) - start ))
                if [[ $elapsed -gt $timeout ]]; then
                    log "ERROR: Timeout waiting for $node to become healthy (status: $status)"
                    exit 1
                fi
                sleep 1
            done
        done
        log "Topology ready (polling fallback)"
    fi
}

capture_stats() {
    local phase="$1"
    local label="$2"
    local outfile="$RESULTS_DIR/docker-stats-${label}-${phase}.json"

    docker stats --no-stream \
        --format '{"container":"{{.Name}}","cpu":"{{.CPUPerc}}","mem_usage":"{{.MemUsage}}","mem_perc":"{{.MemPerc}}","net_io":"{{.NetIO}}","block_io":"{{.BlockIO}}","pids":"{{.PIDs}}"}' \
        chromatindb-node1 chromatindb-node2 chromatindb-node3 \
        2>/dev/null | jq -s '.' > "$outfile"

    log "Docker stats captured: $outfile"
}

collect_hardware_info() {
    log "Collecting hardware info..."
    local cpu_model cores ram_total kernel docker_ver disk_type

    cpu_model=$(lscpu 2>/dev/null | grep 'Model name' | sed 's/.*:\s*//' || echo "unknown")
    cores=$(nproc 2>/dev/null || echo "unknown")
    ram_total=$(free -h 2>/dev/null | awk '/^Mem:/ {print $2}' || echo "unknown")
    kernel=$(uname -r 2>/dev/null || echo "unknown")
    docker_ver=$(docker version --format '{{.Server.Version}}' 2>/dev/null || echo "unknown")
    disk_type=$(lsblk -d -o NAME,ROTA 2>/dev/null | awk 'NR>1 && $2=="0" {print "SSD"; exit} NR>1 && $2=="1" {print "HDD"; exit}' || echo "unknown")
    [[ -z "$disk_type" ]] && disk_type="unknown"

    jq -n \
        --arg cpu "$cpu_model" \
        --arg cores "$cores" \
        --arg ram "$ram_total" \
        --arg kernel "$kernel" \
        --arg docker "$docker_ver" \
        --arg disk "$disk_type" \
        '{cpu_model: $cpu, cores: ($cores | tonumber // $cores), ram_total: $ram, kernel: $kernel, docker_version: $docker, disk_type: $disk}' \
        > "$RESULTS_DIR/hardware-info.json"

    log "Hardware info written: $RESULTS_DIR/hardware-info.json"
}

get_blob_count() {
    local container="$1"
    docker kill -s USR1 "$container" >/dev/null 2>&1 || true
    sleep 1
    local count
    count=$(docker logs "$container" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'blobs=\K[0-9]+' || echo "0")
    echo "$count"
}

wait_for_convergence() {
    local container="$1"
    local expected_count="$2"
    local timeout_seconds="$3"
    local start_ns
    start_ns=$(date +%s%N)

    while true; do
        local count
        count=$(get_blob_count "$container")
        local now_ns
        now_ns=$(date +%s%N)
        local elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))

        log "Waiting for $container: $count/$expected_count blobs... (${elapsed_ms}ms)"

        if [[ "$count" -ge "$expected_count" ]]; then
            echo "$elapsed_ms"
            return 0
        fi

        if [[ $elapsed_ms -gt $(( timeout_seconds * 1000 )) ]]; then
            log "ERROR: Timeout waiting for $container convergence ($count/$expected_count after ${elapsed_ms}ms)"
            echo "$elapsed_ms"
            return 1
        fi

        sleep 2
    done
}

run_loadgen() {
    local target_host="$1"
    shift
    docker run --rm --network "$NETWORK" \
        chromatindb:latest \
        chromatindb_loadgen --target "${target_host}:4200" "$@" \
        2>/dev/null
}

# --- Helper Functions --------------------------------------------------------

generate_trusted_configs() {
    # Resolve container IPs at runtime for trusted_peers configuration.
    # trusted_peers requires IP addresses, NOT Docker DNS names.
    local ip1 ip2 ip3
    ip1=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' chromatindb-node1)
    ip2=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' chromatindb-node2)
    ip3=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' chromatindb-node3)

    log "Resolved IPs: node1=$ip1 node2=$ip2 node3=$ip3"

    cat > "$SCRIPT_DIR/configs/node1-trusted.json" <<TJEOF
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "info",
  "sync_interval_seconds": 10,
  "trusted_peers": ["$ip2", "$ip3"]
}
TJEOF

    cat > "$SCRIPT_DIR/configs/node2-trusted.json" <<TJEOF
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node1:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10,
  "trusted_peers": ["$ip1", "$ip3"]
}
TJEOF

    cat > "$SCRIPT_DIR/configs/node3-trusted.json" <<TJEOF
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node2:4200"],
  "log_level": "info",
  "sync_interval_seconds": 10,
  "trusted_peers": ["$ip1", "$ip2"]
}
TJEOF

    log "Generated trusted configs in $SCRIPT_DIR/configs/"
}

# --- Report Functions --------------------------------------------------------

generate_report() {
    log "Report generation..."
}

# --- Scenario Functions ------------------------------------------------------

run_scenario_ingest() {
    log "========================================="
    log "Scenario: Ingest Throughput (PERF-01)"
    log "========================================="

    local sizes=(1024 102400 1048576)
    local labels=("1k" "100k" "1m")

    for i in "${!sizes[@]}"; do
        local size="${sizes[$i]}"
        local label="${labels[$i]}"

        log "--- Ingest: ${label} blobs (size=${size} bytes) ---"

        reset_topology

        capture_stats "pre" "ingest-${label}"

        log "Running loadgen: count=$BLOB_COUNT rate=$RATE size=$size"
        local output
        output=$(run_loadgen node1 --count "$BLOB_COUNT" --rate "$RATE" --size "$size" --drain-timeout 10)

        capture_stats "post" "ingest-${label}"

        echo "$output" > "$RESULTS_DIR/scenario-ingest-${label}.json"

        # Log summary from the JSON output
        local blobs_sec mib_sec p50 p95 p99
        blobs_sec=$(echo "$output" | jq -r '.blobs_per_sec // "N/A"')
        mib_sec=$(echo "$output" | jq -r '.mib_per_sec // "N/A"')
        p50=$(echo "$output" | jq -r '.latency_ms.p50 // "N/A"')
        p95=$(echo "$output" | jq -r '.latency_ms.p95 // "N/A"')
        p99=$(echo "$output" | jq -r '.latency_ms.p99 // "N/A"')

        log "Result [${label}]: ${blobs_sec} blobs/sec, ${mib_sec} MiB/sec, latency p50=${p50}ms p95=${p95}ms p99=${p99}ms"
        log "Written: $RESULTS_DIR/scenario-ingest-${label}.json"
    done

    log "Ingest scenario complete"
}

run_scenario_sync() {
    # Measures sync latency (PERF-02) and multi-hop propagation (PERF-03)
    # in a single scenario run.
    #
    # NOTE: Sync latency includes the sync_interval_seconds (10s) scheduling
    # delay. This is expected real-world behavior -- sync is timer-driven,
    # not event-driven. A blob written at t=0 may not sync until t=10.
    # Multi-hop (2-hop) latency includes two sync intervals (~20s).

    log "========================================="
    log "Scenario: Sync Latency + Multi-Hop (PERF-02, PERF-03)"
    log "========================================="

    reset_topology

    capture_stats "pre" "sync"

    log "Ingesting $BLOB_COUNT blobs (1 KiB) to node1..."
    local loadgen_output
    loadgen_output=$(run_loadgen node1 --count "$BLOB_COUNT" --rate "$RATE" --size 1024 --drain-timeout 10)

    # Timing starts from loadgen completion -- we measure how long after
    # writes finish until replication converges on downstream nodes.
    local load_done_ns
    load_done_ns=$(date +%s%N)

    log "Loadgen complete. Waiting for node2 convergence (1-hop)..."
    local sync_1hop_ms
    if sync_1hop_ms=$(wait_for_convergence chromatindb-node2 "$BLOB_COUNT" 120); then
        log "Node2 converged in ${sync_1hop_ms}ms (1-hop)"
    else
        log "WARNING: Node2 convergence timed out"
        sync_1hop_ms=-1
    fi

    log "Waiting for node3 convergence (2-hop)..."
    local sync_2hop_ms
    if sync_2hop_ms=$(wait_for_convergence chromatindb-node3 "$BLOB_COUNT" 120); then
        log "Node3 converged in ${sync_2hop_ms}ms (2-hop)"
    else
        log "WARNING: Node3 convergence timed out"
        sync_2hop_ms=-1
    fi

    capture_stats "post" "sync"

    # Build result JSON
    local result
    result=$(jq -n \
        --arg blobs "$BLOB_COUNT" \
        --arg sync_1hop "$sync_1hop_ms" \
        --arg sync_2hop "$sync_2hop_ms" \
        --argjson loadgen "$loadgen_output" \
        '{
            scenario: "sync-latency",
            blob_count: ($blobs | tonumber),
            sync_1hop_ms: ($sync_1hop | tonumber),
            sync_2hop_ms: ($sync_2hop | tonumber),
            sync_interval_seconds: 10,
            loadgen: $loadgen
        }')

    echo "$result" > "$RESULTS_DIR/scenario-sync-latency.json"

    log "Result: 1-hop=${sync_1hop_ms}ms, 2-hop=${sync_2hop_ms}ms"
    log "Written: $RESULTS_DIR/scenario-sync-latency.json"
    log "Sync scenario complete"
}

run_scenario_latejoin() {
    # Measures late-joiner catch-up time (PERF-04).
    # node4 joins after data is fully loaded and converged on node1-3.
    # Catch-up time = from node4 healthy to full convergence.

    log "========================================="
    log "Scenario: Late-Joiner Catch-Up (PERF-04)"
    log "========================================="

    reset_topology

    log "Ingesting $BLOB_COUNT blobs (1 KiB) to node1..."
    run_loadgen node1 --count "$BLOB_COUNT" --rate "$RATE" --size 1024 --drain-timeout 10 >/dev/null

    log "Waiting for node3 convergence (ensures full chain has data)..."
    local chain_ms
    if ! chain_ms=$(wait_for_convergence chromatindb-node3 "$BLOB_COUNT" 120); then
        log "ERROR: Chain did not converge before late-joiner test"
        return 1
    fi
    log "Chain converged in ${chain_ms}ms"

    capture_stats "pre" "latejoin"

    log "Starting node4 (late-joiner)..."
    $COMPOSE --profile latejoin up -d node4

    # Wait for node4 healthcheck to pass
    log "Waiting for node4 to become healthy..."
    local timeout=30
    local start
    start=$(date +%s)
    while true; do
        local status
        status=$(docker inspect --format '{{.State.Health.Status}}' chromatindb-node4 2>/dev/null || echo "missing")
        if [[ "$status" == "healthy" ]]; then
            log "node4 is healthy"
            break
        fi
        local elapsed=$(( $(date +%s) - start ))
        if [[ $elapsed -gt $timeout ]]; then
            log "ERROR: Timeout waiting for node4 to become healthy (status: $status)"
            return 1
        fi
        sleep 1
    done

    log "Timing node4 catch-up..."
    local catchup_ms
    if catchup_ms=$(wait_for_convergence chromatindb-node4 "$BLOB_COUNT" 300); then
        log "node4 caught up in ${catchup_ms}ms"
    else
        log "WARNING: node4 catch-up timed out"
    fi

    capture_stats "post" "latejoin"

    # Build result JSON
    local result
    result=$(jq -n \
        --arg blobs "$BLOB_COUNT" \
        --arg catchup "$catchup_ms" \
        '{
            scenario: "late-joiner",
            blob_count: ($blobs | tonumber),
            catchup_ms: ($catchup | tonumber),
            note: "Time from node4 healthy to full convergence"
        }')

    echo "$result" > "$RESULTS_DIR/scenario-latejoin.json"

    log "Result: catch-up=${catchup_ms}ms for $BLOB_COUNT blobs"
    log "Written: $RESULTS_DIR/scenario-latejoin.json"
    log "Late-joiner scenario complete"
}

run_scenario_trusted_vs_pq() {
    # Compares PQ (default) vs trusted handshake overhead (PERF-05).
    # Full compose down/up between modes for fair comparison (fresh connections).

    log "========================================="
    log "Scenario: Trusted vs PQ Handshake (PERF-05)"
    log "========================================="

    # --- RUN 1: PQ mode (default) ---
    log "--- RUN 1: PQ mode (default handshake) ---"
    reset_topology

    capture_stats "pre" "pq"

    log "Running loadgen (PQ mode): count=$BLOB_COUNT rate=$RATE size=1024"
    local pq_result
    pq_result=$(run_loadgen node1 --count "$BLOB_COUNT" --rate "$RATE" --size 1024 --drain-timeout 10)

    capture_stats "post" "pq"

    # --- Generate trusted configs with resolved IPs ---
    log "--- Generating trusted configs ---"
    generate_trusted_configs

    # --- RUN 2: Trusted mode ---
    log "--- RUN 2: Trusted mode (lightweight handshake) ---"

    # Full restart with trusted config override
    $COMPOSE down -v 2>/dev/null || true
    docker compose -f "$COMPOSE_FILE" -f "$SCRIPT_DIR/docker-compose.trusted.yml" -p chromatindb up -d --wait 2>/dev/null || {
        # Fallback: poll healthcheck status
        log "Fallback: polling healthcheck status for trusted mode..."
        local timeout=30
        local start
        start=$(date +%s)
        for node in chromatindb-node1 chromatindb-node2 chromatindb-node3; do
            while true; do
                local status
                status=$(docker inspect --format '{{.State.Health.Status}}' "$node" 2>/dev/null || echo "missing")
                if [[ "$status" == "healthy" ]]; then
                    log "$node is healthy (trusted mode)"
                    break
                fi
                local elapsed=$(( $(date +%s) - start ))
                if [[ $elapsed -gt $timeout ]]; then
                    log "ERROR: Timeout waiting for $node to become healthy in trusted mode"
                    exit 1
                fi
                sleep 1
            done
        done
    }

    capture_stats "pre" "trusted"

    log "Running loadgen (trusted mode): count=$BLOB_COUNT rate=$RATE size=1024"
    local trusted_result
    trusted_result=$(run_loadgen node1 --count "$BLOB_COUNT" --rate "$RATE" --size 1024 --drain-timeout 10)

    capture_stats "post" "trusted"

    # Cleanup: restore default topology (remove trusted volumes)
    $COMPOSE down -v 2>/dev/null || true

    # Build comparison JSON
    local result
    result=$(jq -n \
        --argjson pq "$pq_result" \
        --argjson trusted "$trusted_result" \
        '{
            scenario: "trusted-vs-pq",
            pq_mode: $pq,
            trusted_mode: $trusted
        }')

    echo "$result" > "$RESULTS_DIR/scenario-trusted-vs-pq.json"

    # Log comparison summary
    local pq_p50 pq_p95 trusted_p50 trusted_p95
    pq_p50=$(echo "$pq_result" | jq -r '.latency_ms.p50 // "N/A"')
    pq_p95=$(echo "$pq_result" | jq -r '.latency_ms.p95 // "N/A"')
    trusted_p50=$(echo "$trusted_result" | jq -r '.latency_ms.p50 // "N/A"')
    trusted_p95=$(echo "$trusted_result" | jq -r '.latency_ms.p95 // "N/A"')

    log "Comparison: PQ p50=${pq_p50}ms p95=${pq_p95}ms | Trusted p50=${trusted_p50}ms p95=${trusted_p95}ms"
    log "Written: $RESULTS_DIR/scenario-trusted-vs-pq.json"
    log "Trusted vs PQ scenario complete"
}

# --- Main --------------------------------------------------------------------

main() {
    # Parse arguments
    for arg in "$@"; do
        case "$arg" in
            --skip-build) SKIP_BUILD=true ;;
            --report-only) REPORT_ONLY=true ;;
            *) log "Unknown argument: $arg"; exit 1 ;;
        esac
    done

    mkdir -p "$RESULTS_DIR"

    log "chromatindb Benchmark Suite"
    log "BLOB_COUNT=$BLOB_COUNT RATE=$RATE"
    log "Results directory: $RESULTS_DIR"

    check_deps
    collect_hardware_info

    if [[ "$REPORT_ONLY" == true ]]; then
        generate_report
        exit 0
    fi

    build_image

    # Core scenarios (Plan 30-01)
    run_scenario_ingest
    run_scenario_sync

    # Additional scenarios (Plan 30-02)
    run_scenario_latejoin
    run_scenario_trusted_vs_pq

    # Generate report while containers are still available
    generate_report

    # Final cleanup
    $COMPOSE down -v 2>/dev/null || true

    # Summary
    local result_count
    result_count=$(find "$RESULTS_DIR" -name '*.json' -type f | wc -l)
    log "========================================="
    log "Benchmark complete: $result_count result files in $RESULTS_DIR"
    log "========================================="
}

main "$@"
