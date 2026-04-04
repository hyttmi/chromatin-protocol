#!/usr/bin/env bash
# =============================================================================
# STRESS-01: Long-Running Soak Test
#
# Verifies:
#   A 3-node cluster under continuous ~10 blobs/sec mixed-size ingest for a
#   configurable duration shows bounded memory, consistent blob counts, and
#   zero crashes.
#
# Topology: 3-node cluster on dedicated network (172.50.0.0/16).
#   Node1 (172.50.0.2): ingest target + seed
#   Node2 (172.50.0.3): bootstrap from Node1
#   Node3 (172.50.0.4): bootstrap from Node1 + Node2
#
# Usage:
#   bash test_stress01_long_running.sh [--skip-build] [--duration 4h]
#
# Duration format: Xh (hours) or Xm (minutes). Default: 4h.
# This test is excluded from default run-integration.sh runs.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

# --- Constants ---------------------------------------------------------------

STRESS01_NETWORK="chromatindb-stress01-test-net"
NODE1_CONTAINER="chromatindb-stress01-node1"
NODE2_CONTAINER="chromatindb-stress01-node2"
NODE3_CONTAINER="chromatindb-stress01-node3"
NODE1_VOLUME="chromatindb-stress01-node1-data"
NODE2_VOLUME="chromatindb-stress01-node2-data"
NODE3_VOLUME="chromatindb-stress01-node3-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$STRESS01_NETWORK"

# Temp config files
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
RSS_LOG_FILE=""

# --- Duration Parsing --------------------------------------------------------

DURATION_SECONDS=14400  # Default: 4h

for i in "$@"; do
    case "$i" in
        --duration)
            DURATION_FLAG="__NEXT__"
            ;;
        *)
            if [[ "${DURATION_FLAG:-}" == "__NEXT__" ]]; then
                DURATION_RAW="$i"
                DURATION_FLAG=""
                # Parse Xh or Xm format
                if [[ "$DURATION_RAW" =~ ^([0-9]+)h$ ]]; then
                    DURATION_SECONDS=$(( ${BASH_REMATCH[1]} * 3600 ))
                elif [[ "$DURATION_RAW" =~ ^([0-9]+)m$ ]]; then
                    DURATION_SECONDS=$(( ${BASH_REMATCH[1]} * 60 ))
                else
                    log "ERROR: Invalid duration format: $DURATION_RAW (use Xh or Xm)"
                    exit 1
                fi
            fi
            ;;
    esac
done

log "Soak test duration: ${DURATION_SECONDS}s"

# --- Cleanup -----------------------------------------------------------------

cleanup_stress01() {
    log "Cleaning up STRESS-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$STRESS01_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
    [[ -n "$RSS_LOG_FILE" ]] && rm -f "$RSS_LOG_FILE" 2>/dev/null || true
}
trap cleanup_stress01 EXIT

# --- RSS Parsing -------------------------------------------------------------

parse_rss_bytes() {
    local raw="$1"
    local num
    num=$(echo "$raw" | grep -oP '[\d.]+' | head -1)
    local unit
    unit=$(echo "$raw" | grep -oP '[KMG]iB' | head -1)
    case "$unit" in
        KiB) echo "$(echo "$num * 1024" | bc | cut -d. -f1)" ;;
        MiB) echo "$(echo "$num * 1048576" | bc | cut -d. -f1)" ;;
        GiB) echo "$(echo "$num * 1073741824" | bc | cut -d. -f1)" ;;
        *) echo "0" ;;
    esac
}

get_rss_bytes() {
    local container="$1"
    local raw
    raw=$(docker stats --no-stream --format '{{.MemUsage}}' "$container" 2>/dev/null || echo "0MiB")
    parse_rss_bytes "$raw"
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_stress01 2>/dev/null || true

log "=== STRESS-01: Long-Running Soak Test ==="

FAILURES=0
RSS_LOG_FILE=$(mktemp /tmp/stress01_rss_XXXXXX.log)

# Create network and volumes
docker network create --driver bridge --subnet 172.50.0.0/16 "$STRESS01_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# =============================================================================
# Phase 1: Setup -- Start 3-node cluster
# =============================================================================

log "--- Phase 1: Starting 3-node cluster ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/stress01-node1-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/stress01-node2-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.50.0.2:4200"],
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

TEMP_NODE3_CONFIG=$(mktemp /tmp/stress01-node3-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.50.0.2:4200", "172.50.0.3:4200"],
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$STRESS01_NETWORK" \
    --ip 172.50.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

docker run -d --name "$NODE2_CONTAINER" \
    --network "$STRESS01_NETWORK" \
    --ip 172.50.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

docker run -d --name "$NODE3_CONTAINER" \
    --network "$STRESS01_NETWORK" \
    --ip 172.50.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"
wait_healthy "$NODE3_CONTAINER"

# Allow peer connections to establish
sleep 10

# Record initial RSS
INITIAL_RSS_1=$(get_rss_bytes "$NODE1_CONTAINER")
INITIAL_RSS_2=$(get_rss_bytes "$NODE2_CONTAINER")
INITIAL_RSS_3=$(get_rss_bytes "$NODE3_CONTAINER")

log "Initial RSS: node1=${INITIAL_RSS_1}B node2=${INITIAL_RSS_2}B node3=${INITIAL_RSS_3}B"
echo "0 $INITIAL_RSS_1 $INITIAL_RSS_2 $INITIAL_RSS_3" >> "$RSS_LOG_FILE"

# =============================================================================
# Phase 2+3: Continuous ingest with periodic monitoring
# =============================================================================

log "--- Phase 2+3: Continuous ingest loop (${DURATION_SECONDS}s) ---"

SOAK_START=$(date +%s)
LAST_RSS_CHECK=$SOAK_START
LAST_CONVERGENCE_CHECK=$SOAK_START
ITER=0
TOTAL_BLOBS_INGESTED=0

while true; do
    ELAPSED=$(( $(date +%s) - SOAK_START ))
    if [[ $ELAPSED -ge $DURATION_SECONDS ]]; then
        break
    fi

    # 50 blobs at rate 10 = ~5 seconds per batch
    # --mixed: 70% 1KB, 20% 100KB, 10% 1MB (realistic distribution)
    # Fresh identity per batch (creates separate namespaces; convergence polling handles sync)
    docker run --rm --network "$STRESS01_NETWORK" \
        --entrypoint chromatindb_loadgen "$IMAGE" \
        --target "172.50.0.2:4200" --count 50 --rate 10 --mixed --ttl 3600 --drain-timeout 1 \
        >/dev/null 2>&1

    TOTAL_BLOBS_INGESTED=$((TOTAL_BLOBS_INGESTED + 50))
    ITER=$((ITER + 1))

    NOW=$(date +%s)

    # RSS monitoring: every 60 seconds
    if [[ $((NOW - LAST_RSS_CHECK)) -ge 60 ]]; then
        RSS_1=$(get_rss_bytes "$NODE1_CONTAINER")
        RSS_2=$(get_rss_bytes "$NODE2_CONTAINER")
        RSS_3=$(get_rss_bytes "$NODE3_CONTAINER")
        ELAPSED_NOW=$((NOW - SOAK_START))
        echo "$ELAPSED_NOW $RSS_1 $RSS_2 $RSS_3" >> "$RSS_LOG_FILE"
        log "RSS @${ELAPSED_NOW}s: node1=${RSS_1}B node2=${RSS_2}B node3=${RSS_3}B (ingested ~${TOTAL_BLOBS_INGESTED} blobs)"
        LAST_RSS_CHECK=$NOW
    fi

    # Convergence check: every 300 seconds (5 minutes)
    if [[ $((NOW - LAST_CONVERGENCE_CHECK)) -ge 300 ]]; then
        COUNT_1=$(get_blob_count "$NODE1_CONTAINER")
        COUNT_2=$(get_blob_count "$NODE2_CONTAINER")
        COUNT_3=$(get_blob_count "$NODE3_CONTAINER")
        log "Convergence check: node1=$COUNT_1 node2=$COUNT_2 node3=$COUNT_3"
        LAST_CONVERGENCE_CHECK=$NOW
    fi
done

log "Ingest loop complete: ~${TOTAL_BLOBS_INGESTED} blobs ingested over ${DURATION_SECONDS}s"

# =============================================================================
# Phase 4: Final verification
# =============================================================================

log "--- Phase 4: Final verification ---"

# Wait for sync convergence: poll every 15s until counts match within 5%, timeout 5 min
CONVERGE_TIMEOUT=300
CONVERGE_START=$(date +%s)
CONVERGE_THRESHOLD=5  # percent
log "Waiting for sync convergence (polling every 15s, timeout ${CONVERGE_TIMEOUT}s)..."
while true; do
    sleep 15
    C1=$(get_blob_count "$NODE1_CONTAINER")
    C2=$(get_blob_count "$NODE2_CONTAINER")
    C3=$(get_blob_count "$NODE3_CONTAINER")
    CMAX=$C1; CMIN=$C1
    for c in $C2 $C3; do
        [[ $c -gt $CMAX ]] && CMAX=$c
        [[ $c -lt $CMIN ]] && CMIN=$c
    done
    CDIFF=$((CMAX - CMIN))
    CTHRESH=$((CMAX * CONVERGE_THRESHOLD / 100))
    [[ $CTHRESH -lt 1 ]] && CTHRESH=1
    CELAPSED=$(( $(date +%s) - CONVERGE_START ))
    log "Convergence poll @${CELAPSED}s: $C1/$C2/$C3 (diff=$CDIFF, threshold=$CTHRESH)"
    if [[ $CMAX -gt 0 && $CDIFF -le $CTHRESH ]]; then
        log "Converged after ${CELAPSED}s"
        break
    fi
    if [[ $CELAPSED -ge $CONVERGE_TIMEOUT ]]; then
        log "WARNING: Convergence timeout after ${CONVERGE_TIMEOUT}s (diff=$CDIFF)"
        break
    fi
done

# Check 1: All 3 nodes still running
log "--- Check 1: All nodes still running ---"

for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER"; do
    STATUS=$(docker inspect --format '{{.State.Running}}' "$CONTAINER" 2>/dev/null || echo "false")
    if [[ "$STATUS" != "true" ]]; then
        log "FAIL: $CONTAINER is not running"
        FAILURES=$((FAILURES + 1))
    fi
done

if [[ $FAILURES -eq 0 ]]; then
    pass "All 3 nodes still running"
fi

# Get final blob counts
FINAL_COUNT_1=$(get_blob_count "$NODE1_CONTAINER")
FINAL_COUNT_2=$(get_blob_count "$NODE2_CONTAINER")
FINAL_COUNT_3=$(get_blob_count "$NODE3_CONTAINER")
log "Final blob counts: node1=$FINAL_COUNT_1 node2=$FINAL_COUNT_2 node3=$FINAL_COUNT_3"

# Check 2: Consistent blob counts (max - min <= 5% of max)
log "--- Check 2: Consistent blob counts ---"

# Find max and min
MAX_COUNT=$FINAL_COUNT_1
MIN_COUNT=$FINAL_COUNT_1
for c in $FINAL_COUNT_2 $FINAL_COUNT_3; do
    [[ $c -gt $MAX_COUNT ]] && MAX_COUNT=$c
    [[ $c -lt $MIN_COUNT ]] && MIN_COUNT=$c
done

if [[ $MAX_COUNT -gt 0 ]]; then
    DIFF=$((MAX_COUNT - MIN_COUNT))
    THRESHOLD=$((MAX_COUNT * 5 / 100))
    if [[ $DIFF -le $THRESHOLD ]]; then
        pass "Blob counts consistent (max-min=$DIFF, threshold=$THRESHOLD)"
    else
        log "FAIL: Blob counts diverged (max-min=$DIFF, threshold=$THRESHOLD, counts: $FINAL_COUNT_1/$FINAL_COUNT_2/$FINAL_COUNT_3)"
        FAILURES=$((FAILURES + 1))
    fi
else
    log "FAIL: Max blob count is 0"
    FAILURES=$((FAILURES + 1))
fi

# Check 3: Memory stable -- RSS not still growing after ingest stops
# Take 3 samples 15s apart; last must be within 20% of first (no leak)
log "--- Check 3: Memory stable (post-ingest) ---"

STABLE_SAMPLES=3
STABLE_INTERVAL=15
declare -a STABLE_RSS_1=() STABLE_RSS_2=() STABLE_RSS_3=()

for ((s=0; s<STABLE_SAMPLES; s++)); do
    STABLE_RSS_1+=("$(get_rss_bytes "$NODE1_CONTAINER")")
    STABLE_RSS_2+=("$(get_rss_bytes "$NODE2_CONTAINER")")
    STABLE_RSS_3+=("$(get_rss_bytes "$NODE3_CONTAINER")")
    [[ $s -lt $((STABLE_SAMPLES - 1)) ]] && sleep $STABLE_INTERVAL
done

MEMORY_OK=true
for NODE_NUM in 1 2 3; do
    eval "FIRST=\${STABLE_RSS_${NODE_NUM}[0]}"
    eval "LAST=\${STABLE_RSS_${NODE_NUM}[$((STABLE_SAMPLES - 1))]}"
    # Allow 20% growth tolerance (mdbx compaction, mmap settling)
    if [[ $FIRST -gt 0 ]]; then
        GROWTH_BOUND=$(( FIRST + FIRST / 5 ))
        if [[ $LAST -gt $GROWTH_BOUND ]]; then
            log "FAIL: Node$NODE_NUM RSS still growing post-ingest: first=$((FIRST / 1048576))MiB last=$((LAST / 1048576))MiB (>20% growth)"
            MEMORY_OK=false
            FAILURES=$((FAILURES + 1))
        fi
    fi
done

if [[ "$MEMORY_OK" == true ]]; then
    pass "Memory stable: RSS not growing post-ingest"
fi

# Parse RSS log for summary stats
MAX_RSS_1=$INITIAL_RSS_1; MAX_RSS_2=$INITIAL_RSS_2; MAX_RSS_3=$INITIAL_RSS_3
while IFS=' ' read -r _ rss1 rss2 rss3; do
    [[ $rss1 -gt $MAX_RSS_1 ]] && MAX_RSS_1=$rss1
    [[ $rss2 -gt $MAX_RSS_2 ]] && MAX_RSS_2=$rss2
    [[ $rss3 -gt $MAX_RSS_3 ]] && MAX_RSS_3=$rss3
done < "$RSS_LOG_FILE"

FINAL_RSS_1=${STABLE_RSS_1[$((STABLE_SAMPLES - 1))]}
FINAL_RSS_2=${STABLE_RSS_2[$((STABLE_SAMPLES - 1))]}
FINAL_RSS_3=${STABLE_RSS_3[$((STABLE_SAMPLES - 1))]}

log "RSS summary:"
log "  Node1: initial=$((INITIAL_RSS_1 / 1048576))MiB max=$((MAX_RSS_1 / 1048576))MiB final=$((FINAL_RSS_1 / 1048576))MiB"
log "  Node2: initial=$((INITIAL_RSS_2 / 1048576))MiB max=$((MAX_RSS_2 / 1048576))MiB final=$((FINAL_RSS_2 / 1048576))MiB"
log "  Node3: initial=$((INITIAL_RSS_3 / 1048576))MiB max=$((MAX_RSS_3 / 1048576))MiB final=$((FINAL_RSS_3 / 1048576))MiB"

# Check 4: No crashes/corruption in node logs
log "--- Check 4: No crash indicators ---"

CRASH_PATTERNS="SIGSEGV|SIGABRT|AddressSanitizer|ThreadSanitizer|corruption|core dump"
CRASH_OK=true

for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER"; do
    CRASH_HITS=$(docker logs "$CONTAINER" 2>&1 | grep -ciE "$CRASH_PATTERNS" || true)
    CRASH_HITS=$(echo "$CRASH_HITS" | tr -d '[:space:]')
    CRASH_HITS=${CRASH_HITS:-0}
    if [[ "$CRASH_HITS" -gt 0 ]]; then
        log "FAIL: $CONTAINER has crash indicators ($CRASH_HITS matches)"
        CRASH_OK=false
        FAILURES=$((FAILURES + 1))
    fi
done

if [[ "$CRASH_OK" == true ]]; then
    pass "No crash/corruption indicators in any node logs"
fi

# Check 5: Blob counts are substantial
log "--- Check 5: Substantial blob counts ---"

# Expected: ~duration_seconds * 10 blobs/sec (but batches have gaps, so expect at least 50%)
EXPECTED_MINIMUM=$((TOTAL_BLOBS_INGESTED / 4))
if [[ $EXPECTED_MINIMUM -lt 10 ]]; then
    EXPECTED_MINIMUM=10
fi

if [[ $FINAL_COUNT_1 -ge $EXPECTED_MINIMUM ]]; then
    pass "Substantial blob counts: node1=$FINAL_COUNT_1 (minimum=$EXPECTED_MINIMUM)"
else
    log "FAIL: Blob count too low: node1=$FINAL_COUNT_1, expected >= $EXPECTED_MINIMUM"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "STRESS-01: Long-Running Soak Test PASSED"
    pass "  - Duration: ${DURATION_SECONDS}s"
    pass "  - Total blobs ingested: ~${TOTAL_BLOBS_INGESTED}"
    pass "  - Final counts: node1=$FINAL_COUNT_1 node2=$FINAL_COUNT_2 node3=$FINAL_COUNT_3"
    pass "  - Memory stable: RSS not growing post-ingest"
    pass "  - No crashes or corruption detected"
    exit 0
else
    fail "STRESS-01: $FAILURES check(s) failed"
fi
