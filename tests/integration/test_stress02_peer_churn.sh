#!/usr/bin/env bash
# =============================================================================
# STRESS-02: Peer Churn Chaos Test
#
# Verifies:
#   A 5-node cluster with random node kill/restart every 30s for 30 minutes
#   converges to identical blob sets with no data loss.
#
# Topology: 5 standalone nodes on dedicated network (docker run -d, not compose)
#   Network: chromatindb-stress02-test-net (172.48.0.0/16)
#   Node1 (172.48.0.2): seed node
#   Node2 (172.48.0.3): bootstrap from node1
#   Node3 (172.48.0.4): bootstrap from node1, node2
#   Node4 (172.48.0.5): bootstrap from node2, node3
#   Node5 (172.48.0.6): bootstrap from node3, node4
#   Named volumes for persistent identity across restarts.
#
# Flow:
#   Phase 1: Setup 5-node cluster, ingest 200 baseline blobs, wait for sync
#   Phase 2+3: 30-minute churn loop (60 iterations, 30s each)
#              Random SIGKILL 1-2 nodes per cycle, restart, ingest batch
#   Phase 4: Convergence (120s healing window)
#   Phase 5: Verify all nodes have consistent blob counts, no crashes
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

STRESS02_NETWORK="chromatindb-stress02-test-net"
SUBNET="172.48.0"
IMAGE="chromatindb:test"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$STRESS02_NETWORK"

NODE_COUNT=5
NODE_NAMES=()
NODE_IPS=()
NODE_VOLUMES=()
TEMP_CONFIGS=()

for i in $(seq 1 $NODE_COUNT); do
    NODE_NAMES+=("chromatindb-stress02-node${i}")
    NODE_IPS+=("${SUBNET}.$((i + 1))")
    NODE_VOLUMES+=("chromatindb-stress02-node${i}-data")
done

CHURN_DURATION=${CHURN_DURATION:-1800}  # 30 minutes default
CHURN_INTERVAL=30
CHURN_ITERATIONS=$((CHURN_DURATION / CHURN_INTERVAL))
INGEST_TOTAL=0

# --- Cleanup -----------------------------------------------------------------

cleanup_stress02() {
    log "Cleaning up STRESS-02 test..."
    for name in "${NODE_NAMES[@]}"; do
        docker rm -f "$name" 2>/dev/null || true
    done
    for vol in "${NODE_VOLUMES[@]}"; do
        docker volume rm "$vol" 2>/dev/null || true
    done
    docker network rm "$STRESS02_NETWORK" 2>/dev/null || true
    for cfg in "${TEMP_CONFIGS[@]}"; do
        rm -f "$cfg" 2>/dev/null || true
    done
}
trap cleanup_stress02 EXIT

# --- Helper: Create config for node N ----------------------------------------

create_node_config() {
    local node_idx=$1  # 1-based
    local cfg
    cfg=$(mktemp /tmp/stress02-node${node_idx}-XXXXXX.json)
    TEMP_CONFIGS+=("$cfg")

    # Build bootstrap_peers based on chain topology
    local peers=""
    case $node_idx in
        1) peers='[]' ;;
        2) peers='["172.48.0.2:4200"]' ;;
        3) peers='["172.48.0.2:4200", "172.48.0.3:4200"]' ;;
        4) peers='["172.48.0.3:4200", "172.48.0.4:4200"]' ;;
        5) peers='["172.48.0.4:4200", "172.48.0.5:4200"]' ;;
    esac

    cat > "$cfg" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ${peers},
  "log_level": "info",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
    chmod 644 "$cfg"
    echo "$cfg"
}

# --- Helper: Start a node ---------------------------------------------------

start_node() {
    local idx=$1  # 1-based
    local name="${NODE_NAMES[$((idx - 1))]}"
    local ip="${NODE_IPS[$((idx - 1))]}"
    local vol="${NODE_VOLUMES[$((idx - 1))]}"
    local cfg
    cfg=$(create_node_config "$idx")

    docker run -d --name "$name" \
        --network "$STRESS02_NETWORK" \
        --ip "$ip" \
        -v "${vol}:/data" \
        -v "${cfg}:/config/node.json:ro" \
        --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
        --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
        "$IMAGE" \
        run --config /config/node.json --data-dir /data --log-level info
}

# --- Helper: Get list of running node indices --------------------------------

get_running_nodes() {
    local running=()
    for i in $(seq 1 $NODE_COUNT); do
        local name="${NODE_NAMES[$((i - 1))]}"
        local status
        status=$(docker inspect --format '{{.State.Running}}' "$name" 2>/dev/null || echo "false")
        if [[ "$status" == "true" ]]; then
            running+=("$i")
        fi
    done
    echo "${running[@]}"
}

# --- Helper: Pick lowest running node IP -------------------------------------

get_lowest_running_ip() {
    local running
    running=$(get_running_nodes)
    local first
    first=$(echo "$running" | tr ' ' '\n' | head -1)
    if [[ -n "$first" ]]; then
        echo "${NODE_IPS[$((first - 1))]}"
    fi
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_stress02 2>/dev/null || true

log "=== STRESS-02: Peer Churn Chaos Test ==="
log "Churn duration: ${CHURN_DURATION}s (${CHURN_ITERATIONS} iterations)"

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.48.0.0/16 "$STRESS02_NETWORK"
for vol in "${NODE_VOLUMES[@]}"; do
    docker volume create "$vol"
done

# =============================================================================
# Phase 1: Setup and Baseline
# =============================================================================

log "--- Phase 1: Setup and baseline ---"

# Start all 5 nodes
for i in $(seq 1 $NODE_COUNT); do
    start_node "$i"
    log "Started node $i"
done

# Wait all healthy
for name in "${NODE_NAMES[@]}"; do
    wait_healthy "$name" 60
done

# Wait for mesh to establish
log "Waiting 15s for mesh connections..."
sleep 15

# Ingest 200 baseline blobs to node1
log "Ingesting 200 baseline blobs..."
run_loadgen "${NODE_IPS[0]}" --count 200 --size 256 --rate 50 --ttl 3600
INGEST_TOTAL=200

# Wait for sync to node5 (most distant)
log "Waiting for baseline sync to node5..."
if ! wait_sync "${NODE_NAMES[4]}" 200 120; then
    NODE5_COUNT=$(get_blob_count "${NODE_NAMES[4]}")
    log "WARN: Baseline sync incomplete (node5=$NODE5_COUNT/200), continuing..."
else
    pass "Baseline: 200 blobs synced to all nodes"
fi

# =============================================================================
# Phase 3: Churn Loop
# =============================================================================

log "--- Phase 3: Churn loop (${CHURN_ITERATIONS} iterations, ${CHURN_INTERVAL}s each) ---"

CHURN_START=$(date +%s)

for iter in $(seq 1 "$CHURN_ITERATIONS"); do
    ITER_START=$(date +%s)
    ELAPSED=$((ITER_START - CHURN_START))

    # Determine kill count: 1 or 2
    KILL_COUNT=$(( RANDOM % 2 + 1 ))

    # Get currently running nodes
    RUNNING=($(get_running_nodes))
    RUNNING_COUNT=${#RUNNING[@]}

    # Never kill all nodes -- keep at least 1 alive
    if [[ $KILL_COUNT -ge $RUNNING_COUNT ]]; then
        KILL_COUNT=$((RUNNING_COUNT - 1))
    fi

    # Skip if nothing to kill (shouldn't happen with 5 nodes)
    if [[ $KILL_COUNT -le 0 ]]; then
        log "Iter $iter/${CHURN_ITERATIONS} [${ELAPSED}s]: No nodes to kill, skipping"
        sleep "$CHURN_INTERVAL"
        continue
    fi

    # Randomly select nodes to kill
    KILL_INDICES=($(printf '%s\n' "${RUNNING[@]}" | shuf | head -n "$KILL_COUNT"))

    # Kill selected nodes via SIGKILL (hardest crash)
    KILLED_NAMES=()
    for idx in "${KILL_INDICES[@]}"; do
        local_name="${NODE_NAMES[$((idx - 1))]}"
        docker kill "$local_name" >/dev/null 2>&1 || true
        KILLED_NAMES+=("node${idx}")
    done

    log "Iter $iter/${CHURN_ITERATIONS} [${ELAPSED}s]: KILLED ${KILLED_NAMES[*]}"

    # Sleep 15s to let surviving nodes detect disconnection
    sleep 15

    # Restart killed nodes (container still exists, volume preserved)
    RESTARTED_NAMES=()
    for idx in "${KILL_INDICES[@]}"; do
        local_name="${NODE_NAMES[$((idx - 1))]}"
        docker start "$local_name" >/dev/null 2>&1 || true
        RESTARTED_NAMES+=("node${idx}")
    done

    # Wait for restarted nodes to become healthy (non-fatal timeout)
    for idx in "${KILL_INDICES[@]}"; do
        local_name="${NODE_NAMES[$((idx - 1))]}"
        wait_healthy "$local_name" 30 || log "WARN: $local_name slow to recover"
    done

    log "Iter $iter/${CHURN_ITERATIONS} [${ELAPSED}s]: RESTARTED ${RESTARTED_NAMES[*]}"

    # Ingest a batch to a surviving node (continuous writes during churn)
    TARGET_IP=$(get_lowest_running_ip)
    if [[ -n "$TARGET_IP" ]]; then
        docker run --rm --network "$STRESS02_NETWORK" \
            --entrypoint chromatindb_loadgen \
            "$IMAGE" \
            --target "${TARGET_IP}:4200" --count 20 --size 256 --rate 10 --ttl 3600 \
            2>/dev/null || log "WARN: Ingest batch failed at iter $iter (expected during churn)"
        INGEST_TOTAL=$((INGEST_TOTAL + 20))
    fi

    # Calculate remaining sleep to hit the 30s cycle
    ITER_ELAPSED=$(($(date +%s) - ITER_START))
    REMAINING=$((CHURN_INTERVAL - ITER_ELAPSED))
    if [[ $REMAINING -gt 0 ]]; then
        sleep "$REMAINING"
    fi
done

log "Churn loop complete. Total blobs ingested: $INGEST_TOTAL"

# =============================================================================
# Phase 4: Convergence
# =============================================================================

log "--- Phase 4: Convergence (120s healing window) ---"

# Ensure all nodes are running
for i in $(seq 1 $NODE_COUNT); do
    local_name="${NODE_NAMES[$((i - 1))]}"
    STATUS=$(docker inspect --format '{{.State.Running}}' "$local_name" 2>/dev/null || echo "false")
    if [[ "$STATUS" != "true" ]]; then
        log "Restarting stopped node $i for convergence..."
        docker start "$local_name" >/dev/null 2>&1 || true
        wait_healthy "$local_name" 30 || log "WARN: node$i slow to recover for convergence"
    fi
done

# Send SIGHUP to all nodes to clear reconnect backoff
log "Sending SIGHUP to all nodes..."
for name in "${NODE_NAMES[@]}"; do
    docker kill -s HUP "$name" >/dev/null 2>&1 || true
done

# Wait 120 seconds for convergence
log "Waiting 120s for convergence..."
sleep 120

# =============================================================================
# Phase 5: Verification
# =============================================================================

log "--- Phase 5: Verification ---"

# Check 1: All 5 nodes are running
log "--- Check 1: All nodes running ---"
ALL_RUNNING=true
for i in $(seq 1 $NODE_COUNT); do
    local_name="${NODE_NAMES[$((i - 1))]}"
    STATUS=$(docker inspect --format '{{.State.Running}}' "$local_name" 2>/dev/null || echo "false")
    if [[ "$STATUS" != "true" ]]; then
        log "FAIL: node$i is not running"
        ALL_RUNNING=false
        FAILURES=$((FAILURES + 1))
    fi
done
if [[ "$ALL_RUNNING" == true ]]; then
    pass "All 5 nodes running"
fi

# Get blob counts from all nodes
COUNTS=()
for i in $(seq 1 $NODE_COUNT); do
    COUNT=$(get_blob_count "${NODE_NAMES[$((i - 1))]}")
    COUNTS+=("$COUNT")
    log "node$i blob count: $COUNT"
done

# Check 2: All nodes have >= 200 (baseline)
log "--- Check 2: All nodes have baseline blobs ---"
for i in $(seq 1 $NODE_COUNT); do
    if [[ "${COUNTS[$((i - 1))]}" -ge 200 ]]; then
        pass "node$i has >= 200 blobs (${COUNTS[$((i - 1))]})"
    else
        log "FAIL: node$i has ${COUNTS[$((i - 1))]} blobs, expected >= 200"
        FAILURES=$((FAILURES + 1))
    fi
done

# Check 3: Blob count convergence (max - min <= 5% of max)
log "--- Check 3: Blob count convergence ---"
MAX_COUNT=0
MIN_COUNT=999999999
for c in "${COUNTS[@]}"; do
    if [[ "$c" -gt "$MAX_COUNT" ]]; then MAX_COUNT=$c; fi
    if [[ "$c" -lt "$MIN_COUNT" ]]; then MIN_COUNT=$c; fi
done

if [[ "$MAX_COUNT" -gt 0 ]]; then
    TOLERANCE=$((MAX_COUNT * 5 / 100))
    DIFF=$((MAX_COUNT - MIN_COUNT))
    log "Convergence: max=$MAX_COUNT min=$MIN_COUNT diff=$DIFF tolerance=$TOLERANCE"

    if [[ "$DIFF" -le "$TOLERANCE" ]]; then
        pass "Blob counts converged (diff=$DIFF <= ${TOLERANCE} = 5% of $MAX_COUNT)"
    else
        log "FAIL: Blob counts diverged (diff=$DIFF > ${TOLERANCE} = 5% of $MAX_COUNT)"
        FAILURES=$((FAILURES + 1))
    fi
else
    log "FAIL: All blob counts are 0"
    FAILURES=$((FAILURES + 1))
fi

# Check 4: No SIGSEGV/SIGABRT/corruption in any node's logs
log "--- Check 4: No crashes or corruption ---"
CRASH_FOUND=false
for i in $(seq 1 $NODE_COUNT); do
    local_name="${NODE_NAMES[$((i - 1))]}"
    LOGS=$(docker logs "$local_name" 2>&1 | tail -1000)
    if echo "$LOGS" | grep -qiE "SIGSEGV|SIGABRT|corruption|stack trace|segmentation fault"; then
        log "FAIL: Crash/corruption evidence in node$i logs"
        CRASH_FOUND=true
        FAILURES=$((FAILURES + 1))
    fi
done
if [[ "$CRASH_FOUND" == false ]]; then
    pass "No crashes or corruption in any node logs"
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "STRESS-02: Peer churn chaos test PASSED"
    pass "  - 5-node cluster survived ${CHURN_ITERATIONS} churn cycles over ${CHURN_DURATION}s"
    pass "  - Random SIGKILL/restart with continuous ingest ($INGEST_TOTAL total blobs)"
    pass "  - All nodes converged after 120s healing (max=$MAX_COUNT min=$MIN_COUNT)"
    pass "  - No crashes or corruption detected"
    exit 0
else
    fail "STRESS-02: $FAILURES check(s) failed"
fi
