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

reset_topology_fastsync() {
    log "Resetting topology with fast-sync configs (down -v, up -d)..."
    local COMPOSE_FASTSYNC="docker compose -f $COMPOSE_FILE -f $SCRIPT_DIR/docker-compose.fastsync.yml -p chromatindb"
    $COMPOSE_FASTSYNC down -v 2>/dev/null || true
    if $COMPOSE_FASTSYNC up -d --wait 2>/dev/null; then
        log "Fast-sync topology ready (--wait succeeded)"
    else
        # Fallback: poll healthcheck status for each node
        log "Fallback: polling healthcheck status for fast-sync..."
        local timeout=30
        local start
        start=$(date +%s)
        for node in chromatindb-node1 chromatindb-node2 chromatindb-node3; do
            while true; do
                local status
                status=$(docker inspect --format '{{.State.Health.Status}}' "$node" 2>/dev/null || echo "missing")
                if [[ "$status" == "healthy" ]]; then
                    log "$node is healthy (fast-sync)"
                    break
                fi
                local elapsed=$(( $(date +%s) - start ))
                if [[ $elapsed -gt $timeout ]]; then
                    log "ERROR: Timeout waiting for $node to become healthy (fast-sync, status: $status)"
                    exit 1
                fi
                sleep 1
            done
        done
        log "Fast-sync topology ready (polling fallback)"
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
        --entrypoint chromatindb_loadgen \
        chromatindb:latest \
        --target "${target_host}:4200" "$@" \
        2>/dev/null
}

run_loadgen_v() {
    local volume_mount="$1"
    local target_host="$2"
    shift 2
    docker run --rm -i --network "$NETWORK" \
        -v "$volume_mount" \
        --entrypoint chromatindb_loadgen \
        chromatindb:latest \
        --target "${target_host}:4200" "$@" \
        2>/dev/null
}

get_storage_mib() {
    local container="$1"
    docker kill -s USR1 "$container" >/dev/null 2>&1 || true
    sleep 1
    docker logs "$container" 2>&1 | grep "metrics:" | tail -1 \
        | grep -oP 'storage=\K[0-9.]+' || echo "999"
}

wait_for_gc_completion() {
    local container="$1"
    local max_storage_mib="$2"
    local timeout_seconds="$3"
    local start_ns
    start_ns=$(date +%s%N)

    while true; do
        local storage_mib
        storage_mib=$(get_storage_mib "$container")
        local now_ns
        now_ns=$(date +%s%N)
        local elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))

        log "GC wait $container: storage=${storage_mib}MiB (target<=${max_storage_mib}) (${elapsed_ms}ms)"

        local below
        below=$(echo "$storage_mib <= $max_storage_mib" | bc -l)
        if [[ "$below" == "1" ]]; then
            echo "$elapsed_ms"
            return 0
        fi

        if [[ $elapsed_ms -gt $(( timeout_seconds * 1000 )) ]]; then
            log "ERROR: Timeout waiting for GC on $container (storage=${storage_mib}MiB after ${elapsed_ms}ms)"
            echo "$elapsed_ms"
            return 1
        fi

        sleep 3
    done
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

get_peak_stats() {
    local scenario="$1"
    local container="$2"
    local metric="$3"  # "cpu" or "mem"

    # Collect all stats files matching this scenario
    local stat_files=()
    for f in "$RESULTS_DIR"/docker-stats-"${scenario}"-*.json; do
        [[ -f "$f" ]] && stat_files+=("$f")
    done

    if [[ ${#stat_files[@]} -eq 0 ]]; then
        echo "N/A"
        return
    fi

    if [[ "$metric" == "cpu" ]]; then
        jq -s -r --arg c "$container" \
            '[.[][] | select(.container == $c) | .cpu | rtrimstr("%") | tonumber] | max // "N/A"' \
            "${stat_files[@]}" 2>/dev/null || echo "N/A"
    else
        # Return peak memory usage string (from the post-scenario snapshot)
        jq -s -r --arg c "$container" \
            '[.[][] | select(.container == $c) | .mem_usage | split(" / ")[0]] | last // "N/A"' \
            "${stat_files[@]}" 2>/dev/null || echo "N/A"
    fi
}

format_ingest_row() {
    local label="$1"
    local display="$2"
    local file="$RESULTS_DIR/scenario-ingest-${label}.json"

    if [[ ! -f "$file" ]]; then
        echo "| ${display} | [not run] | [not run] | [not run] | [not run] | [not run] |"
        return
    fi

    local blobs_sec mib_sec p50 p95 p99
    blobs_sec=$(jq -r '.blobs_per_sec | . * 10 | round / 10' "$file")
    mib_sec=$(jq -r '.mib_per_sec | . * 100 | round / 100' "$file")
    p50=$(jq -r '.latency_ms.p50 | . * 100 | round / 100' "$file")
    p95=$(jq -r '.latency_ms.p95 | . * 100 | round / 100' "$file")
    p99=$(jq -r '.latency_ms.p99 | . * 100 | round / 100' "$file")

    echo "| ${display} | ${blobs_sec} | ${mib_sec} | ${p50} | ${p95} | ${p99} |"
}

format_tombstone_row() {
    local label="$1"
    local display="$2"
    local file="$RESULTS_DIR/scenario-tombstone-creation-${label}.json"

    if [[ ! -f "$file" ]]; then
        echo "| ${display} | [not run] | [not run] | [not run] | [not run] |"
        return
    fi

    local blobs_sec p50 p95 p99
    blobs_sec=$(jq -r '.blobs_per_sec | . * 10 | round / 10' "$file")
    p50=$(jq -r '.latency_ms.p50 | . * 100 | round / 100' "$file")
    p95=$(jq -r '.latency_ms.p95 | . * 100 | round / 100' "$file")
    p99=$(jq -r '.latency_ms.p99 | . * 100 | round / 100' "$file")

    echo "| ${display} | ${blobs_sec} | ${p50} | ${p95} | ${p99} |"
}

compute_overhead() {
    local pq_val="$1"
    local trusted_val="$2"
    jq -n -r --argjson pq "$pq_val" --argjson tr "$trusted_val" \
        'if $tr == 0 then "N/A"
         else (($pq - $tr) / $tr * 100 | . * 10 | round / 10 | tostring) + "%"
         end'
}

format_resource_table() {
    local scenario="$1"
    local nodes=("chromatindb-node1" "chromatindb-node2" "chromatindb-node3")

    local has_stats=false
    for f in "$RESULTS_DIR"/docker-stats-"${scenario}"-*.json; do
        if [[ -f "$f" ]]; then has_stats=true; break; fi
    done

    if [[ "$has_stats" == false ]]; then
        echo "*No resource data available.*"
        return
    fi

    echo "| Node | Peak CPU | Memory |"
    echo "|------|----------|--------|"
    for node in "${nodes[@]}"; do
        local cpu mem
        cpu=$(get_peak_stats "$scenario" "$node" "cpu")
        mem=$(get_peak_stats "$scenario" "$node" "mem")
        echo "| ${node#chromatindb-} | ${cpu}% | ${mem} |"
    done
}

generate_report() {
    log "Generating benchmark report..."

    # Step 1: Provenance
    local timestamp commit version
    timestamp=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    commit=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    version=$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null || echo "dev")

    # Step 2: Hardware info
    local hw_cpu hw_cores hw_ram hw_disk hw_kernel hw_docker
    if [[ -f "$RESULTS_DIR/hardware-info.json" ]]; then
        hw_cpu=$(jq -r '.cpu_model // "unknown"' "$RESULTS_DIR/hardware-info.json")
        hw_cores=$(jq -r '.cores // "unknown"' "$RESULTS_DIR/hardware-info.json")
        hw_ram=$(jq -r '.ram_total // "unknown"' "$RESULTS_DIR/hardware-info.json")
        hw_disk=$(jq -r '.disk_type // "unknown"' "$RESULTS_DIR/hardware-info.json")
        hw_kernel=$(jq -r '.kernel // "unknown"' "$RESULTS_DIR/hardware-info.json")
        hw_docker=$(jq -r '.docker_version // "unknown"' "$RESULTS_DIR/hardware-info.json")
    else
        hw_cpu="unknown"; hw_cores="unknown"; hw_ram="unknown"
        hw_disk="unknown"; hw_kernel="unknown"; hw_docker="unknown"
    fi

    # Step 3: Executive summary values
    local peak_blobs_sec="[not run]"
    local worst_p99="[not run]"
    local pq_overhead="[not run]"
    local catchup_time="[not run]"

    # Peak ingest blobs/sec and worst p99 across ingest scenarios
    local max_blobs=0 max_p99=0 has_ingest=false
    for label in 1k 100k 1m; do
        local f="$RESULTS_DIR/scenario-ingest-${label}.json"
        if [[ -f "$f" ]]; then
            has_ingest=true
            local bs p99v
            bs=$(jq -r '.blobs_per_sec' "$f")
            p99v=$(jq -r '.latency_ms.p99' "$f")
            max_blobs=$(jq -n --argjson a "$max_blobs" --argjson b "$bs" 'if $b > $a then $b else $a end')
            max_p99=$(jq -n --argjson a "$max_p99" --argjson b "$p99v" 'if $b > $a then $b else $a end')
        fi
    done
    if [[ "$has_ingest" == true ]]; then
        peak_blobs_sec=$(jq -n "$max_blobs | . * 10 | round / 10" -r)
        worst_p99=$(jq -n "$max_p99 | . * 100 | round / 100" -r)
    fi

    # PQ overhead
    local tvp_file="$RESULTS_DIR/scenario-trusted-vs-pq.json"
    if [[ -f "$tvp_file" ]]; then
        local pq_p50 tr_p50
        pq_p50=$(jq -r '.pq_mode.latency_ms.p50' "$tvp_file")
        tr_p50=$(jq -r '.trusted_mode.latency_ms.p50' "$tvp_file")
        pq_overhead=$(compute_overhead "$pq_p50" "$tr_p50")
    fi

    # Late-joiner catch-up
    local lj_file="$RESULTS_DIR/scenario-latejoin.json"
    if [[ -f "$lj_file" ]]; then
        catchup_time=$(jq -r '.catchup_ms | . / 1000 | . * 10 | round / 10 | tostring + "s (" + (. * 100 | round / 100 | tostring) + "ms)"' "$lj_file" 2>/dev/null || echo "[error]")
        # Simpler: just show ms
        catchup_time=$(jq -r '.catchup_ms' "$lj_file")
        catchup_time="${catchup_time}ms"
    fi

    # Step 3b: Tombstone executive summary values
    local peak_tombstone_sec="[not run]"
    local tombstone_sync_2hop="[not run]"
    local gc_worst_ms="[not run]"

    # Peak tombstone throughput across creation scenarios
    local max_ts_blobs=0 has_ts=false
    for label in 1k 100k 1m; do
        local f="$RESULTS_DIR/scenario-tombstone-creation-${label}.json"
        if [[ -f "$f" ]]; then
            has_ts=true
            local ts_bs
            ts_bs=$(jq -r '.blobs_per_sec' "$f")
            max_ts_blobs=$(jq -n --argjson a "$max_ts_blobs" --argjson b "$ts_bs" 'if $b > $a then $b else $a end')
        fi
    done
    if [[ "$has_ts" == true ]]; then
        peak_tombstone_sec=$(jq -n "$max_ts_blobs | . * 10 | round / 10" -r)
    fi

    # Tombstone sync 2-hop
    local ts_sync_file="$RESULTS_DIR/scenario-tombstone-sync.json"
    if [[ -f "$ts_sync_file" ]]; then
        tombstone_sync_2hop=$(jq -r '.sync_2hop_ms' "$ts_sync_file")
        tombstone_sync_2hop="${tombstone_sync_2hop}ms"
    fi

    # GC worst-case
    local ts_gc_file="$RESULTS_DIR/scenario-tombstone-gc.json"
    if [[ -f "$ts_gc_file" ]]; then
        gc_worst_ms=$(jq -r '.gc_worst_ms' "$ts_gc_file")
        gc_worst_ms="${gc_worst_ms}ms"
    fi

    # Step 4: Build ingest rows
    local ingest_row_1k ingest_row_100k ingest_row_1m
    ingest_row_1k=$(format_ingest_row "1k" "1 KiB")
    ingest_row_100k=$(format_ingest_row "100k" "100 KiB")
    ingest_row_1m=$(format_ingest_row "1m" "1 MiB")

    # Step 5: Ingest resource summary
    local ingest_resources=""
    for label in 1k 100k 1m; do
        local res
        res=$(format_resource_table "ingest-${label}")
        ingest_resources="${ingest_resources}
**Ingest ${label}:**

${res}
"
    done

    # Step 6: Sync latency section
    local sync_section=""
    local sync_file="$RESULTS_DIR/scenario-sync-latency.json"
    if [[ -f "$sync_file" ]]; then
        local s1hop s2hop s_interval s1_mult s2_mult
        s1hop=$(jq -r '.sync_1hop_ms' "$sync_file")
        s2hop=$(jq -r '.sync_2hop_ms' "$sync_file")
        s_interval=$(jq -r '.sync_interval_seconds' "$sync_file")
        s1_mult=$(jq -n -r --argjson ms "$s1hop" --argjson iv "$s_interval" '$ms / ($iv * 1000) | . * 10 | round / 10')
        s2_mult=$(jq -n -r --argjson ms "$s2hop" --argjson iv "$s_interval" '$ms / ($iv * 1000) | . * 10 | round / 10')

        local sync_resources
        sync_resources=$(format_resource_table "sync")

        sync_section="| Metric | Value |
|--------|-------|
| 1-hop sync (node1 -> node2) | ${s1hop}ms |
| 2-hop sync (node1 -> node3) | ${s2hop}ms |
| Sync interval | ${s_interval}s |
| 1-hop as sync_interval multiple | ${s1_mult}x |
| 2-hop as sync_interval multiple | ${s2_mult}x |

**Resource Usage:**

${sync_resources}"
    else
        sync_section="*Scenario not run.*"
    fi

    # Step 7: Late-joiner section
    local lj_section=""
    if [[ -f "$lj_file" ]]; then
        local lj_ms lj_blobs
        lj_ms=$(jq -r '.catchup_ms' "$lj_file")
        lj_blobs=$(jq -r '.blob_count' "$lj_file")

        local lj_resources
        lj_resources=$(format_resource_table "latejoin")

        lj_section="| Metric | Value |
|--------|-------|
| Catch-up time | ${lj_ms}ms |
| Blob count | ${lj_blobs} |

**Resource Usage:**

${lj_resources}"
    else
        lj_section="*Scenario not run.*"
    fi

    # Step 8: Trusted vs PQ section
    local tvp_section=""
    if [[ -f "$tvp_file" ]]; then
        local pq_bps tr_bps pq_p50 pq_p95 pq_p99 tr_p50 tr_p95 tr_p99
        pq_bps=$(jq -r '.pq_mode.blobs_per_sec | . * 10 | round / 10' "$tvp_file")
        tr_bps=$(jq -r '.trusted_mode.blobs_per_sec | . * 10 | round / 10' "$tvp_file")
        pq_p50=$(jq -r '.pq_mode.latency_ms.p50 | . * 100 | round / 100' "$tvp_file")
        pq_p95=$(jq -r '.pq_mode.latency_ms.p95 | . * 100 | round / 100' "$tvp_file")
        pq_p99=$(jq -r '.pq_mode.latency_ms.p99 | . * 100 | round / 100' "$tvp_file")
        tr_p50=$(jq -r '.trusted_mode.latency_ms.p50 | . * 100 | round / 100' "$tvp_file")
        tr_p95=$(jq -r '.trusted_mode.latency_ms.p95 | . * 100 | round / 100' "$tvp_file")
        tr_p99=$(jq -r '.trusted_mode.latency_ms.p99 | . * 100 | round / 100' "$tvp_file")

        local d_bps d_p50 d_p95 d_p99
        local pq_bps_raw tr_bps_raw pq_p50_raw tr_p50_raw pq_p95_raw tr_p95_raw pq_p99_raw tr_p99_raw
        pq_bps_raw=$(jq -r '.pq_mode.blobs_per_sec' "$tvp_file")
        tr_bps_raw=$(jq -r '.trusted_mode.blobs_per_sec' "$tvp_file")
        pq_p50_raw=$(jq -r '.pq_mode.latency_ms.p50' "$tvp_file")
        tr_p50_raw=$(jq -r '.trusted_mode.latency_ms.p50' "$tvp_file")
        pq_p95_raw=$(jq -r '.pq_mode.latency_ms.p95' "$tvp_file")
        tr_p95_raw=$(jq -r '.trusted_mode.latency_ms.p95' "$tvp_file")
        pq_p99_raw=$(jq -r '.pq_mode.latency_ms.p99' "$tvp_file")
        tr_p99_raw=$(jq -r '.trusted_mode.latency_ms.p99' "$tvp_file")

        d_bps=$(compute_overhead "$pq_bps_raw" "$tr_bps_raw")
        d_p50=$(compute_overhead "$pq_p50_raw" "$tr_p50_raw")
        d_p95=$(compute_overhead "$pq_p95_raw" "$tr_p95_raw")
        d_p99=$(compute_overhead "$pq_p99_raw" "$tr_p99_raw")

        local pq_resources tr_resources
        pq_resources=$(format_resource_table "pq")
        tr_resources=$(format_resource_table "trusted")

        tvp_section="| Metric | PQ Mode | Trusted Mode | Delta |
|--------|---------|--------------|-------|
| Throughput (blobs/sec) | ${pq_bps} | ${tr_bps} | ${d_bps} |
| p50 latency (ms) | ${pq_p50} | ${tr_p50} | ${d_p50} |
| p95 latency (ms) | ${pq_p95} | ${tr_p95} | ${d_p95} |
| p99 latency (ms) | ${pq_p99} | ${tr_p99} | ${d_p99} |

**Resource Usage (PQ mode):**

${pq_resources}

**Resource Usage (Trusted mode):**

${tr_resources}"
    else
        tvp_section="*Scenario not run.*"
    fi

    # Step 8b: Build tombstone creation rows
    local tombstone_row_1k tombstone_row_100k tombstone_row_1m
    tombstone_row_1k=$(format_tombstone_row "1k" "1 KiB")
    tombstone_row_100k=$(format_tombstone_row "100k" "100 KiB")
    tombstone_row_1m=$(format_tombstone_row "1m" "1 MiB")

    # Step 8c: Tombstone creation resource summary
    local tombstone_creation_resources=""
    for label in 1k 100k 1m; do
        local res
        res=$(format_resource_table "tombstone-creation-${label}")
        tombstone_creation_resources="${tombstone_creation_resources}
**Tombstone creation ${label}:**

${res}
"
    done

    # Step 8d: Tombstone sync section
    local tombstone_sync_section=""
    if [[ -f "$ts_sync_file" ]]; then
        local ts_s1hop ts_s2hop ts_s_interval
        ts_s1hop=$(jq -r '.sync_1hop_ms' "$ts_sync_file")
        ts_s2hop=$(jq -r '.sync_2hop_ms' "$ts_sync_file")
        ts_s_interval=$(jq -r '.sync_interval_seconds' "$ts_sync_file")

        local ts_sync_resources
        ts_sync_resources=$(format_resource_table "tombstone-sync")

        tombstone_sync_section="| Metric | Value |
|--------|-------|
| 1-hop sync (node1 -> node2) | ${ts_s1hop}ms |
| 2-hop sync (node1 -> node3) | ${ts_s2hop}ms |
| Sync interval | ${ts_s_interval}s |
| Blob count | ${BLOB_COUNT} |

**Resource Usage:**

${ts_sync_resources}"
    else
        tombstone_sync_section="*Scenario not run.*"
    fi

    # Step 8e: Tombstone GC section
    local tombstone_gc_section=""
    if [[ -f "$ts_gc_file" ]]; then
        local gc_n1 gc_n2 gc_n3 gc_worst
        gc_n1=$(jq -r '.gc_node1_ms' "$ts_gc_file")
        gc_n2=$(jq -r '.gc_node2_ms' "$ts_gc_file")
        gc_n3=$(jq -r '.gc_node3_ms' "$ts_gc_file")
        gc_worst=$(jq -r '.gc_worst_ms' "$ts_gc_file")

        local gc_resources
        gc_resources=$(format_resource_table "tombstone-gc")

        tombstone_gc_section="| Node | GC Time (ms) |
|------|-------------|
| node1 | ${gc_n1} |
| node2 | ${gc_n2} |
| node3 | ${gc_n3} |
| **Worst** | **${gc_worst}** |

Expiry scan runs every 60 seconds. GC latency includes up to 60s of scheduling delay.
Both original blobs (TTL=30) and tombstones (TTL=30) are expired and reclaimed.

**Resource Usage:**

${gc_resources}"
    else
        tombstone_gc_section="*Scenario not run.*"
    fi

    # Step 9: Raw data file listing
    local raw_files
    raw_files=$(ls -1 "$RESULTS_DIR"/*.json 2>/dev/null | while read -r f; do echo "- \`$(basename "$f")\`"; done)
    if [[ -z "$raw_files" ]]; then
        raw_files="*No result files found.*"
    fi

    # Step 10: Write REPORT.md
    cat > "$RESULTS_DIR/REPORT.md" <<EOF
# chromatindb Benchmark Report

**Generated:** ${timestamp} | **Commit:** ${commit} | **Version:** ${version}

## Executive Summary

| Metric | Value |
|--------|-------|
| Peak ingest throughput | ${peak_blobs_sec} blobs/sec |
| Worst p99 latency | ${worst_p99}ms |
| PQ vs trusted overhead (p50) | ${pq_overhead} |
| Late-joiner catch-up | ${catchup_time} |
| Peak tombstone throughput | ${peak_tombstone_sec} blobs/sec |
| Tombstone sync (2-hop) | ${tombstone_sync_2hop} |
| GC reclamation (worst) | ${gc_worst_ms} |

## System Profile

| Property | Value |
|----------|-------|
| CPU | ${hw_cpu} |
| Cores | ${hw_cores} |
| RAM | ${hw_ram} |
| Disk | ${hw_disk} |
| Kernel | ${hw_kernel} |
| Docker | ${hw_docker} |

## Topology

\`\`\`
node1 --> node2 --> node3 --> [node4 late-joiner]
  ^
  |
loadgen
\`\`\`

- Chain topology: each node syncs to the next downstream peer
- Sync interval: 10s (timer-driven, not event-driven)
- node4 joins via \`--profile latejoin\` after data is loaded

## Benchmark Parameters

| Parameter | Value |
|-----------|-------|
| BLOB_COUNT | ${BLOB_COUNT} |
| RATE | ${RATE} blobs/sec |
| Drain timeout | 10s |
| Sync interval | 10s |

## Ingest Throughput

| Blob Size | blobs/sec | MiB/sec | p50 (ms) | p95 (ms) | p99 (ms) |
|-----------|-----------|---------|----------|----------|----------|
${ingest_row_1k}
${ingest_row_100k}
${ingest_row_1m}

**Resource Usage:**

${ingest_resources}

## Sync Latency

${sync_section}

## Late-Joiner Catch-Up

${lj_section}

## Trusted vs PQ Handshake

${tvp_section}

## Tombstone Creation

Tombstone data is always 36 bytes (4-byte magic + 32-byte target hash) regardless of original blob size.
Write phase creates blobs of the indicated size; delete phase creates 36-byte tombstones for those blobs.

| Original Blob Size | blobs/sec | p50 (ms) | p95 (ms) | p99 (ms) |
|-------------------|-----------|----------|----------|----------|
${tombstone_row_1k}
${tombstone_row_100k}
${tombstone_row_1m}

**Resource Usage:**

${tombstone_creation_resources}

## Tombstone Sync Propagation

${tombstone_sync_section}

## Tombstone GC/Expiry

${tombstone_gc_section}

## Caveats

- **Sync latency includes sync_interval:** Sync is timer-driven (10s interval), not event-driven. Measured latency includes scheduling delay. Real propagation time is lower.
- **Single run, no statistical significance:** These are single-run results establishing a baseline. Production benchmarking requires multiple runs with statistical analysis.
- **Docker overhead:** All nodes run in Docker containers with overlay networking. Bare-metal performance will differ (typically faster).
- **GC timing includes scan interval:** Expiry scan runs every 60 seconds. GC latency includes up to 60s of scheduling delay.

## Raw Data

${raw_files}
EOF

    log "Report written: $RESULTS_DIR/REPORT.md"

    # Step 11: Build benchmark-summary.json
    local hw_json="null"
    [[ -f "$RESULTS_DIR/hardware-info.json" ]] && hw_json=$(cat "$RESULTS_DIR/hardware-info.json")

    local ingest_1k="null" ingest_100k="null" ingest_1m="null"
    local sync_json="null" lj_json="null" tvp_json="null"
    local ts_creation_1k="null" ts_creation_100k="null" ts_creation_1m="null"
    local ts_sync="null" ts_gc="null"

    [[ -f "$RESULTS_DIR/scenario-ingest-1k.json" ]] && ingest_1k=$(cat "$RESULTS_DIR/scenario-ingest-1k.json")
    [[ -f "$RESULTS_DIR/scenario-ingest-100k.json" ]] && ingest_100k=$(cat "$RESULTS_DIR/scenario-ingest-100k.json")
    [[ -f "$RESULTS_DIR/scenario-ingest-1m.json" ]] && ingest_1m=$(cat "$RESULTS_DIR/scenario-ingest-1m.json")
    [[ -f "$RESULTS_DIR/scenario-sync-latency.json" ]] && sync_json=$(cat "$RESULTS_DIR/scenario-sync-latency.json")
    [[ -f "$RESULTS_DIR/scenario-latejoin.json" ]] && lj_json=$(cat "$RESULTS_DIR/scenario-latejoin.json")
    [[ -f "$RESULTS_DIR/scenario-trusted-vs-pq.json" ]] && tvp_json=$(cat "$RESULTS_DIR/scenario-trusted-vs-pq.json")
    [[ -f "$RESULTS_DIR/scenario-tombstone-creation-1k.json" ]] && ts_creation_1k=$(cat "$RESULTS_DIR/scenario-tombstone-creation-1k.json")
    [[ -f "$RESULTS_DIR/scenario-tombstone-creation-100k.json" ]] && ts_creation_100k=$(cat "$RESULTS_DIR/scenario-tombstone-creation-100k.json")
    [[ -f "$RESULTS_DIR/scenario-tombstone-creation-1m.json" ]] && ts_creation_1m=$(cat "$RESULTS_DIR/scenario-tombstone-creation-1m.json")
    [[ -f "$RESULTS_DIR/scenario-tombstone-sync.json" ]] && ts_sync=$(cat "$RESULTS_DIR/scenario-tombstone-sync.json")
    [[ -f "$RESULTS_DIR/scenario-tombstone-gc.json" ]] && ts_gc=$(cat "$RESULTS_DIR/scenario-tombstone-gc.json")

    jq -n \
        --arg generated "$timestamp" \
        --arg commit "$commit" \
        --arg version "$version" \
        --argjson hardware "$hw_json" \
        --argjson blob_count "$BLOB_COUNT" \
        --argjson rate "$RATE" \
        --argjson ingest_1k "$ingest_1k" \
        --argjson ingest_100k "$ingest_100k" \
        --argjson ingest_1m "$ingest_1m" \
        --argjson sync_latency "$sync_json" \
        --argjson latejoin "$lj_json" \
        --argjson trusted_vs_pq "$tvp_json" \
        --argjson tombstone_creation_1k "$ts_creation_1k" \
        --argjson tombstone_creation_100k "$ts_creation_100k" \
        --argjson tombstone_creation_1m "$ts_creation_1m" \
        --argjson tombstone_sync "$ts_sync" \
        --argjson tombstone_gc "$ts_gc" \
        '{
            generated: $generated,
            commit: $commit,
            version: $version,
            hardware: $hardware,
            parameters: {
                blob_count: $blob_count,
                rate: $rate,
                drain_timeout: 10,
                sync_interval_seconds: 10
            },
            scenarios: {
                ingest_1k: $ingest_1k,
                ingest_100k: $ingest_100k,
                ingest_1m: $ingest_1m,
                sync_latency: $sync_latency,
                latejoin: $latejoin,
                trusted_vs_pq: $trusted_vs_pq,
                tombstone_creation_1k: $tombstone_creation_1k,
                tombstone_creation_100k: $tombstone_creation_100k,
                tombstone_creation_1m: $tombstone_creation_1m,
                tombstone_sync: $tombstone_sync,
                tombstone_gc: $tombstone_gc
            }
        }' > "$RESULTS_DIR/benchmark-summary.json"

    log "Summary written: $RESULTS_DIR/benchmark-summary.json"
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

# --- Tombstone Scenario Functions ---------------------------------------------

run_scenario_tombstone_creation() {
    # Measures tombstone creation throughput (BENCH-01).
    # For each blob size: write blobs, then delete them.
    # Tombstone data is always 36 bytes regardless of original blob size.

    log "========================================="
    log "Scenario: Tombstone Creation (BENCH-01)"
    log "========================================="

    local sizes=(1024 102400 1048576)
    local labels=("1k" "100k" "1m")

    for i in "${!sizes[@]}"; do
        local size="${sizes[$i]}"
        local label="${labels[$i]}"

        log "--- Tombstone Creation: ${label} blobs (original size=${size} bytes) ---"

        reset_topology

        capture_stats "pre" "tombstone-creation-${label}"

        # Create temp identity dir for write/delete pipeline
        local identity_dir
        identity_dir=$(mktemp -d)

        # Phase 1: Write blobs (captures hashes, saves identity)
        log "Write phase: count=$BLOB_COUNT rate=$RATE size=$size"
        local write_output
        write_output=$(run_loadgen_v "$identity_dir:/tmp/bench-id" node1 \
            --count "$BLOB_COUNT" --rate "$RATE" --size "$size" \
            --ttl 3600 --drain-timeout 10 --identity-save /tmp/bench-id)

        # Phase 2: Delete blobs (measure tombstone creation throughput)
        local hashes
        hashes=$(echo "$write_output" | jq -r '.blob_hashes[]')
        log "Delete phase: ${BLOB_COUNT} tombstones"
        local delete_output
        delete_output=$(echo "$hashes" | run_loadgen_v "$identity_dir:/tmp/bench-id" node1 \
            --delete --hashes-from stdin --identity-file /tmp/bench-id --drain-timeout 10)

        capture_stats "post" "tombstone-creation-${label}"

        echo "$delete_output" > "$RESULTS_DIR/scenario-tombstone-creation-${label}.json"

        # Log summary
        local blobs_sec p50 p95 p99
        blobs_sec=$(echo "$delete_output" | jq -r '.blobs_per_sec // "N/A"')
        p50=$(echo "$delete_output" | jq -r '.latency_ms.p50 // "N/A"')
        p95=$(echo "$delete_output" | jq -r '.latency_ms.p95 // "N/A"')
        p99=$(echo "$delete_output" | jq -r '.latency_ms.p99 // "N/A"')

        log "Result [${label}]: ${blobs_sec} tombstones/sec, latency p50=${p50}ms p95=${p95}ms p99=${p99}ms"
        log "Written: $RESULTS_DIR/scenario-tombstone-creation-${label}.json"

        rm -rf "$identity_dir"
    done

    log "Tombstone creation scenario complete"
}

run_scenario_tombstone_sync() {
    # Measures tombstone sync propagation latency (BENCH-02).
    # Write 200 blobs to node1, wait for convergence, then delete all 200.
    # Tombstones get new seq_nums, so expected count = 200 + 200 = 400.

    log "========================================="
    log "Scenario: Tombstone Sync Propagation (BENCH-02)"
    log "========================================="

    reset_topology

    capture_stats "pre" "tombstone-sync"

    local identity_dir
    identity_dir=$(mktemp -d)

    # Write 200 blobs (1 KiB, TTL=3600) to node1
    log "Write phase: $BLOB_COUNT blobs (1 KiB) to node1..."
    local write_output
    write_output=$(run_loadgen_v "$identity_dir:/tmp/bench-id" node1 \
        --count "$BLOB_COUNT" --rate "$RATE" --size 1024 \
        --ttl 3600 --drain-timeout 10 --identity-save /tmp/bench-id)

    # Wait for all 3 nodes to converge at 200 blobs
    log "Waiting for blob convergence on all nodes..."
    wait_for_convergence chromatindb-node1 "$BLOB_COUNT" 120 >/dev/null || true
    wait_for_convergence chromatindb-node2 "$BLOB_COUNT" 120 >/dev/null || true
    wait_for_convergence chromatindb-node3 "$BLOB_COUNT" 120 >/dev/null || true
    log "All nodes converged at $BLOB_COUNT blobs"

    # Delete all 200 blobs on node1
    local hashes
    hashes=$(echo "$write_output" | jq -r '.blob_hashes[]')
    log "Delete phase: $BLOB_COUNT tombstones on node1..."
    run_loadgen_v "$identity_dir:/tmp/bench-id" node1 \
        --delete --hashes-from stdin --identity-file /tmp/bench-id \
        --drain-timeout 10 <<< "$hashes" >/dev/null

    # Tombstones get new seq_nums: 200 blobs + 200 tombstones = 400
    local expected_count=$(( BLOB_COUNT * 2 ))

    # Measure 1-hop: node2 convergence at 400
    log "Timing 1-hop tombstone sync (node2, target=$expected_count)..."
    local sync_1hop_ms
    if sync_1hop_ms=$(wait_for_convergence chromatindb-node2 "$expected_count" 120); then
        log "Node2 converged in ${sync_1hop_ms}ms (1-hop)"
    else
        log "WARNING: Node2 tombstone sync timed out"
        sync_1hop_ms=-1
    fi

    # Measure 2-hop: node3 convergence at 400
    log "Timing 2-hop tombstone sync (node3, target=$expected_count)..."
    local sync_2hop_ms
    if sync_2hop_ms=$(wait_for_convergence chromatindb-node3 "$expected_count" 120); then
        log "Node3 converged in ${sync_2hop_ms}ms (2-hop)"
    else
        log "WARNING: Node3 tombstone sync timed out"
        sync_2hop_ms=-1
    fi

    capture_stats "post" "tombstone-sync"

    # Build result JSON
    local result
    result=$(jq -n \
        --argjson blob_count "$BLOB_COUNT" \
        --argjson sync_1hop "$sync_1hop_ms" \
        --argjson sync_2hop "$sync_2hop_ms" \
        '{
            scenario: "tombstone-sync",
            blob_count: $blob_count,
            sync_1hop_ms: $sync_1hop,
            sync_2hop_ms: $sync_2hop,
            sync_interval_seconds: 10
        }')

    echo "$result" > "$RESULTS_DIR/scenario-tombstone-sync.json"

    log "Result: 1-hop=${sync_1hop_ms}ms, 2-hop=${sync_2hop_ms}ms"
    log "Written: $RESULTS_DIR/scenario-tombstone-sync.json"

    rm -rf "$identity_dir"

    log "Tombstone sync scenario complete"
}

run_scenario_tombstone_gc() {
    # Measures tombstone GC/expiry performance (BENCH-03).
    # Write 200 blobs with TTL=30, create tombstones with TTL=30.
    # Both blobs and tombstones expire and are GC'd.
    #
    # CRITICAL: get_blob_count() returns latest_seq_num (high-water, never decreases).
    # GC measurement uses get_storage_mib() which tracks actual disk usage.

    log "========================================="
    log "Scenario: Tombstone GC/Expiry (BENCH-03)"
    log "========================================="

    reset_topology

    capture_stats "pre" "tombstone-gc"

    # Capture baseline storage before any data
    local baseline_storage
    baseline_storage=$(get_storage_mib chromatindb-node1)
    log "Baseline storage (node1): ${baseline_storage}MiB"

    local identity_dir
    identity_dir=$(mktemp -d)

    # Write 200 blobs (1 KiB, TTL=30) to node1
    log "Write phase: $BLOB_COUNT blobs (1 KiB, TTL=30) to node1..."
    local write_output
    write_output=$(run_loadgen_v "$identity_dir:/tmp/bench-id" node1 \
        --count "$BLOB_COUNT" --rate "$RATE" --size 1024 \
        --ttl 30 --drain-timeout 10 --identity-save /tmp/bench-id)

    # Wait for all 3 nodes to converge at 200
    log "Waiting for blob convergence on all nodes..."
    wait_for_convergence chromatindb-node1 "$BLOB_COUNT" 120 >/dev/null || true
    wait_for_convergence chromatindb-node2 "$BLOB_COUNT" 120 >/dev/null || true
    wait_for_convergence chromatindb-node3 "$BLOB_COUNT" 120 >/dev/null || true
    log "All nodes converged at $BLOB_COUNT blobs"

    # Create 200 tombstones with TTL=30
    local hashes
    hashes=$(echo "$write_output" | jq -r '.blob_hashes[]')
    log "Delete phase: $BLOB_COUNT tombstones (TTL=30) on node1..."
    run_loadgen_v "$identity_dir:/tmp/bench-id" node1 \
        --delete --hashes-from stdin --identity-file /tmp/bench-id \
        --ttl 30 --drain-timeout 10 <<< "$hashes" >/dev/null

    # Wait for tombstone sync: 200 blobs + 200 tombstones = 400
    local expected_count=$(( BLOB_COUNT * 2 ))
    log "Waiting for tombstone convergence on all nodes (target=$expected_count)..."
    wait_for_convergence chromatindb-node1 "$expected_count" 120 >/dev/null || true
    wait_for_convergence chromatindb-node2 "$expected_count" 120 >/dev/null || true
    wait_for_convergence chromatindb-node3 "$expected_count" 120 >/dev/null || true
    log "All nodes converged at $expected_count entries"

    # Wait for GC: both blobs (TTL=30) and tombstones (TTL=30) should expire.
    # Expiry scan runs every 60s. Worst case: ~180 seconds from write.
    local gc_threshold
    gc_threshold=$(echo "$baseline_storage + 0.5" | bc -l)
    log "GC threshold: ${gc_threshold}MiB (baseline + 0.5)"

    log "Waiting for GC on all nodes (timeout=180s)..."
    local gc_ms_node1 gc_ms_node2 gc_ms_node3
    if gc_ms_node1=$(wait_for_gc_completion chromatindb-node1 "$gc_threshold" 180); then
        log "Node1 GC complete in ${gc_ms_node1}ms"
    else
        log "WARNING: Node1 GC timed out"
    fi
    if gc_ms_node2=$(wait_for_gc_completion chromatindb-node2 "$gc_threshold" 180); then
        log "Node2 GC complete in ${gc_ms_node2}ms"
    else
        log "WARNING: Node2 GC timed out"
    fi
    if gc_ms_node3=$(wait_for_gc_completion chromatindb-node3 "$gc_threshold" 180); then
        log "Node3 GC complete in ${gc_ms_node3}ms"
    else
        log "WARNING: Node3 GC timed out"
    fi

    capture_stats "post" "tombstone-gc"

    # Compute worst-case GC time
    local gc_worst_ms
    gc_worst_ms=$(jq -n --argjson a "$gc_ms_node1" --argjson b "$gc_ms_node2" --argjson c "$gc_ms_node3" \
        '[$a, $b, $c] | max')

    # Build result JSON
    local result
    result=$(jq -n \
        --argjson blob_count "$BLOB_COUNT" \
        --argjson gc_node1 "$gc_ms_node1" \
        --argjson gc_node2 "$gc_ms_node2" \
        --argjson gc_node3 "$gc_ms_node3" \
        --argjson gc_worst "$gc_worst_ms" \
        '{
            scenario: "tombstone-gc",
            blob_count: $blob_count,
            ttl: 30,
            gc_node1_ms: $gc_node1,
            gc_node2_ms: $gc_node2,
            gc_node3_ms: $gc_node3,
            gc_worst_ms: $gc_worst
        }')

    echo "$result" > "$RESULTS_DIR/scenario-tombstone-gc.json"

    log "Result: node1=${gc_ms_node1}ms, node2=${gc_ms_node2}ms, node3=${gc_ms_node3}ms, worst=${gc_worst_ms}ms"
    log "Written: $RESULTS_DIR/scenario-tombstone-gc.json"

    rm -rf "$identity_dir"

    log "Tombstone GC scenario complete"
}

# --- Reconciliation Scenario Functions ----------------------------------------

run_scenario_reconciliation_scaling() {
    # Measures reconciliation scaling behavior (SYNC-10).
    # Validates that sync time is proportional to delta size, not namespace size.
    # With O(diff) reconciliation, syncing 10 new blobs on a 1000-blob namespace
    # should take roughly the same time as syncing 10 blobs on an empty namespace.

    log "========================================="
    log "Scenario: Reconciliation Scaling (SYNC-10)"
    log "========================================="

    local LARGE_COUNT=1000
    local DELTA_COUNT=10

    # --- Phase 1: Preload ---
    log "--- Phase 1: Preload ($LARGE_COUNT blobs) ---"

    reset_topology_fastsync

    capture_stats "pre" "reconcile-scaling"

    log "Ingesting $LARGE_COUNT blobs (1 KiB) to node1 at rate 100..."
    run_loadgen node1 --count "$LARGE_COUNT" --rate 100 --size 1024 --drain-timeout 30 >/dev/null

    log "Waiting for node2 convergence at $LARGE_COUNT blobs..."
    local preload_convergence_ms
    if preload_convergence_ms=$(wait_for_convergence chromatindb-node2 "$LARGE_COUNT" 300); then
        log "Preload convergence in ${preload_convergence_ms}ms"
    else
        log "ERROR: Preload convergence timed out"
        return 1
    fi

    # --- Phase 2: Delta ---
    log "--- Phase 2: Delta ($DELTA_COUNT blobs on $LARGE_COUNT-blob namespace) ---"

    local target_count=$(( LARGE_COUNT + DELTA_COUNT ))

    log "Ingesting $DELTA_COUNT additional blobs (1 KiB) to node1..."
    run_loadgen node1 --count "$DELTA_COUNT" --rate 100 --size 1024 --drain-timeout 10 >/dev/null

    log "Timing delta sync to node2 (target=$target_count)..."
    local delta_sync_ms
    if delta_sync_ms=$(wait_for_convergence chromatindb-node2 "$target_count" 120); then
        log "Delta sync converged in ${delta_sync_ms}ms"
    else
        log "WARNING: Delta sync timed out"
        delta_sync_ms=-1
    fi

    capture_stats "post" "reconcile-scaling"

    # Build result JSON
    local result
    result=$(jq -n \
        --arg scenario "reconcile-scaling" \
        --argjson preload_blobs "$LARGE_COUNT" \
        --argjson delta_blobs "$DELTA_COUNT" \
        --argjson preload_convergence_ms "$preload_convergence_ms" \
        --argjson delta_sync_ms "$delta_sync_ms" \
        '{
            scenario: $scenario,
            preload_blobs: $preload_blobs,
            delta_blobs: $delta_blobs,
            preload_convergence_ms: $preload_convergence_ms,
            delta_sync_ms: $delta_sync_ms,
            sync_interval_seconds: 2
        }')

    echo "$result" > "$RESULTS_DIR/scenario-reconcile-scaling.json"

    # Calculate sync interval multiple
    local interval_mult
    interval_mult=$(jq -n -r --argjson ms "$delta_sync_ms" '($ms / 2000) | . * 10 | round / 10')

    log "Result: preload=${preload_convergence_ms}ms, delta=${delta_sync_ms}ms (${interval_mult}x sync_interval)"
    log "Written: $RESULTS_DIR/scenario-reconcile-scaling.json"

    # Cleanup: tear down fastsync topology
    local COMPOSE_FASTSYNC="docker compose -f $COMPOSE_FILE -f $SCRIPT_DIR/docker-compose.fastsync.yml -p chromatindb"
    $COMPOSE_FASTSYNC down -v 2>/dev/null || true

    log "Reconciliation scaling scenario complete"
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

    # Tombstone scenarios (Plan 36)
    run_scenario_tombstone_creation
    run_scenario_tombstone_sync
    run_scenario_tombstone_gc

    # Reconciliation scenario (Plan 41)
    run_scenario_reconciliation_scaling

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
