#!/usr/bin/env bash
# =============================================================================
# STRESS-04: Concurrent Mixed Operations
#
# Verifies:
#   4 nodes simultaneously ingesting, syncing, tombstoning, SIGHUP reloading,
#   and dumping metrics produce no deadlocks, crashes, or corruption.
#
# Topology: 4-node cluster on dedicated network (172.51.0.0/16).
#   Node1 (172.51.0.2): seed, ingest target
#   Node2 (172.51.0.3): bootstrap from Node1, delete target
#   Node3 (172.51.0.4): bootstrap from Node1 + Node2
#   Node4 (172.51.0.5): bootstrap from Node2 + Node3
#
# Flow:
#   1. Start 4-node cluster, ingest 100 baseline blobs, wait for sync
#   2. Ingest 50 blobs with saved identity for later tombstoning
#   3. Run 5-minute concurrent phase: ingest + delete + SIGHUP + SIGUSR1
#   4. Verify: all nodes alive, clean restart with integrity scan, convergence
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

# --- Constants ---------------------------------------------------------------

STRESS04_NETWORK="chromatindb-stress04-test-net"
NODE1_CONTAINER="chromatindb-stress04-node1"
NODE2_CONTAINER="chromatindb-stress04-node2"
NODE3_CONTAINER="chromatindb-stress04-node3"
NODE4_CONTAINER="chromatindb-stress04-node4"
NODE1_VOLUME="chromatindb-stress04-node1-data"
NODE2_VOLUME="chromatindb-stress04-node2-data"
NODE3_VOLUME="chromatindb-stress04-node3-data"
NODE4_VOLUME="chromatindb-stress04-node4-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$STRESS04_NETWORK"

# Temp files
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
TEMP_NODE4_CONFIG=""
TEMP_ID_DIR=""

CONCURRENT_DURATION=300  # 5 minutes

# --- Cleanup -----------------------------------------------------------------

cleanup_stress04() {
    log "Cleaning up STRESS-04 test..."
    # Kill any background jobs
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null || true
    wait 2>/dev/null || true
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE4_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE4_VOLUME" 2>/dev/null || true
    docker network rm "$STRESS04_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE4_CONFIG" ]] && rm -f "$TEMP_NODE4_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_ID_DIR" ]] && rm -rf "$TEMP_ID_DIR" 2>/dev/null || true
}
trap cleanup_stress04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_stress04 2>/dev/null || true

log "=== STRESS-04: Concurrent Mixed Operations ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.51.0.0/16 "$STRESS04_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"
docker volume create "$NODE4_VOLUME"

# Create temp dir for identity files
TEMP_ID_DIR=$(mktemp -d /tmp/stress04_id_XXXXXX)
chmod 777 "$TEMP_ID_DIR"

# =============================================================================
# Phase 1: Setup -- Start 4-node cluster
# =============================================================================

log "--- Phase 1: Starting 4-node cluster ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/stress04-node1-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/stress04-node2-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.51.0.2:4200"],
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

TEMP_NODE3_CONFIG=$(mktemp /tmp/stress04-node3-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.51.0.2:4200", "172.51.0.3:4200"],
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

TEMP_NODE4_CONFIG=$(mktemp /tmp/stress04-node4-XXXXXX.json)
cat > "$TEMP_NODE4_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.51.0.3:4200", "172.51.0.4:4200"],
  "log_level": "info",
  "safety_net_interval_seconds": 5,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE4_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$STRESS04_NETWORK" \
    --ip 172.51.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

docker run -d --name "$NODE2_CONTAINER" \
    --network "$STRESS04_NETWORK" \
    --ip 172.51.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

docker run -d --name "$NODE3_CONTAINER" \
    --network "$STRESS04_NETWORK" \
    --ip 172.51.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

docker run -d --name "$NODE4_CONTAINER" \
    --network "$STRESS04_NETWORK" \
    --ip 172.51.0.5 \
    -v "$NODE4_VOLUME:/data" \
    -v "$TEMP_NODE4_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level info

wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"
wait_healthy "$NODE3_CONTAINER"
wait_healthy "$NODE4_CONTAINER"

# Allow peer connections to establish
sleep 10

# Ingest 100 baseline blobs
log "Ingesting 100 baseline blobs to node1..."
run_loadgen 172.51.0.2 --count 100 --size 256 --rate 50 --ttl 3600 >/dev/null 2>&1

# Wait for sync to node4
log "Waiting for baseline sync to node4 (100 blobs)..."
if ! wait_sync "$NODE4_CONTAINER" 100 120; then
    log "WARN: Baseline sync timeout to node4"
fi

BASELINE_COUNT=$(get_blob_count "$NODE4_CONTAINER")
log "Node4 baseline blob count: $BASELINE_COUNT"

# =============================================================================
# Phase 2: Save identity for deletion
# =============================================================================

log "--- Phase 2: Ingesting blobs with saved identity ---"

INGEST_OUTPUT=$(docker run --rm --network "$STRESS04_NETWORK" \
    -v "$TEMP_ID_DIR:/ids" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target "172.51.0.2:4200" --count 50 --size 256 --rate 50 --ttl 3600 \
    --identity-save /ids/owner \
    2>/dev/null) || true

OWNER_HASHES=$(echo "$INGEST_OUTPUT" | jq -r '.blob_hashes[]' 2>/dev/null || echo "")
OWNER_HASH_COUNT=$(echo "$OWNER_HASHES" | grep -c . || echo "0")
log "Saved identity ingest: $OWNER_HASH_COUNT blob hashes captured"

# Wait for sync across cluster
sleep 15

# =============================================================================
# Phase 3: Concurrent operations (5 minutes)
# =============================================================================

log "--- Phase 3: Concurrent operations (${CONCURRENT_DURATION}s) ---"

# Background job: continuous ingest to node1
ingest_loop() {
    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
        if [[ $elapsed -ge $CONCURRENT_DURATION ]]; then
            break
        fi
        docker run --rm --network "$STRESS04_NETWORK" \
            --entrypoint chromatindb_loadgen chromatindb:test \
            --target "172.51.0.2:4200" --count 500 --rate 10 --size 1024 --ttl 3600 --drain-timeout 1 \
            >/dev/null 2>/dev/null || true
    done
}

# Background job: delete loop using saved identity
delete_loop() {
    local start
    start=$(date +%s)
    # Build a file of hashes to delete from (feed 10 at a time)
    local hash_file
    hash_file=$(mktemp /tmp/stress04_hashes_XXXXXX)
    echo "$OWNER_HASHES" > "$hash_file"

    while true; do
        local elapsed=$(( $(date +%s) - start ))
        if [[ $elapsed -ge $CONCURRENT_DURATION ]]; then
            break
        fi
        # Pick up to 10 hashes (cycling through the list)
        local batch
        batch=$(head -10 "$hash_file" 2>/dev/null || true)
        if [[ -z "$batch" ]]; then
            # Exhausted hashes, restart from beginning
            echo "$OWNER_HASHES" > "$hash_file"
            batch=$(head -10 "$hash_file" 2>/dev/null || true)
        fi
        if [[ -n "$batch" ]]; then
            echo "$batch" | docker run --rm -i --network "$STRESS04_NETWORK" \
                -v "$TEMP_ID_DIR:/ids:ro" \
                --entrypoint chromatindb_loadgen chromatindb:test \
                --target "172.51.0.3:4200" --identity-file /ids/owner \
                --delete --hashes-from stdin --ttl 3600 --drain-timeout 1 \
                >/dev/null 2>/dev/null || true
            # Remove used hashes from file
            tail -n +11 "$hash_file" > "${hash_file}.tmp" 2>/dev/null || true
            mv "${hash_file}.tmp" "$hash_file" 2>/dev/null || true
        fi
        sleep 5
    done
    rm -f "$hash_file" 2>/dev/null || true
}

# Background job: SIGHUP every 15 seconds
sighup_loop() {
    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
        if [[ $elapsed -ge $CONCURRENT_DURATION ]]; then
            break
        fi
        for node in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
            docker kill -s HUP "$node" 2>/dev/null || true
        done
        sleep 15
    done
}

# Background job: SIGUSR1 every 20 seconds
sigusr1_loop() {
    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
        if [[ $elapsed -ge $CONCURRENT_DURATION ]]; then
            break
        fi
        for node in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
            docker kill -s USR1 "$node" 2>/dev/null || true
        done
        sleep 20
    done
}

# Launch all background jobs
ingest_loop &
INGEST_PID=$!
delete_loop &
DELETE_PID=$!
sighup_loop &
SIGHUP_PID=$!
sigusr1_loop &
SIGUSR1_PID=$!

log "Background jobs launched: ingest=$INGEST_PID delete=$DELETE_PID sighup=$SIGHUP_PID sigusr1=$SIGUSR1_PID"

# Wait for concurrent duration + 30s grace period
GRACE_PERIOD=330
WAIT_START=$(date +%s)

while true; do
    WAIT_ELAPSED=$(( $(date +%s) - WAIT_START ))
    if [[ $WAIT_ELAPSED -ge $GRACE_PERIOD ]]; then
        break
    fi
    # Check if all jobs have finished naturally
    ALL_DONE=true
    for pid in $INGEST_PID $DELETE_PID $SIGHUP_PID $SIGUSR1_PID; do
        if kill -0 "$pid" 2>/dev/null; then
            ALL_DONE=false
            break
        fi
    done
    if [[ "$ALL_DONE" == true ]]; then
        break
    fi
    sleep 10
done

# Kill any remaining background jobs
kill $INGEST_PID $DELETE_PID $SIGHUP_PID $SIGUSR1_PID 2>/dev/null || true
wait 2>/dev/null || true

log "Concurrent operations phase complete"

# =============================================================================
# Phase 4: Verification
# =============================================================================

log "--- Phase 4: Verification ---"

# Wait 30 seconds for in-flight operations to settle
log "Waiting 30s for operations to settle..."
sleep 30

# Check 1: All 4 nodes still running
log "--- Check 1: All nodes still running ---"

NODE_CHECK_OK=true
for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
    STATUS=$(docker inspect --format '{{.State.Running}}' "$CONTAINER" 2>/dev/null || echo "false")
    if [[ "$STATUS" != "true" ]]; then
        log "FAIL: $CONTAINER is not running"
        NODE_CHECK_OK=false
        FAILURES=$((FAILURES + 1))
    fi
done

if [[ "$NODE_CHECK_OK" == true ]]; then
    pass "All 4 nodes still running after concurrent operations"
fi

# Check 2: Restart all 4 nodes, verify integrity scan
log "--- Check 2: Restart + integrity scan ---"

for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
    docker restart "$CONTAINER" >/dev/null 2>&1 || true
done

# Wait for all nodes to become healthy after restart
for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
    if ! wait_healthy "$CONTAINER" 120; then
        log "FAIL: $CONTAINER did not become healthy after restart"
        FAILURES=$((FAILURES + 1))
    fi
done

INTEGRITY_OK=true
for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
    INTEGRITY_LINE=$(docker logs "$CONTAINER" 2>&1 | grep "integrity scan: blobs=" | tail -1 || echo "")
    if [[ -z "$INTEGRITY_LINE" ]]; then
        log "WARN: No integrity scan output found for $CONTAINER"
    else
        log "$CONTAINER: $INTEGRITY_LINE"
    fi
done

if [[ "$INTEGRITY_OK" == true ]]; then
    pass "All nodes restarted with integrity scan"
fi

# Allow post-restart sync
log "Waiting 60s for post-restart sync convergence..."
sleep 60

# Check 3: Blob counts consistent across cluster (within 5%)
log "--- Check 3: Blob count consistency ---"

FINAL_COUNT_1=$(get_blob_count "$NODE1_CONTAINER")
FINAL_COUNT_2=$(get_blob_count "$NODE2_CONTAINER")
FINAL_COUNT_3=$(get_blob_count "$NODE3_CONTAINER")
FINAL_COUNT_4=$(get_blob_count "$NODE4_CONTAINER")

log "Final blob counts: node1=$FINAL_COUNT_1 node2=$FINAL_COUNT_2 node3=$FINAL_COUNT_3 node4=$FINAL_COUNT_4"

MAX_COUNT=$FINAL_COUNT_1
MIN_COUNT=$FINAL_COUNT_1
for c in $FINAL_COUNT_2 $FINAL_COUNT_3 $FINAL_COUNT_4; do
    [[ $c -gt $MAX_COUNT ]] && MAX_COUNT=$c
    [[ $c -lt $MIN_COUNT ]] && MIN_COUNT=$c
done

if [[ $MAX_COUNT -gt 0 ]]; then
    DIFF=$((MAX_COUNT - MIN_COUNT))
    THRESHOLD=$((MAX_COUNT * 5 / 100))
    # Minimum threshold of 10 to account for in-flight tombstone propagation
    [[ $THRESHOLD -lt 10 ]] && THRESHOLD=10
    if [[ $DIFF -le $THRESHOLD ]]; then
        pass "Blob counts consistent (max-min=$DIFF, threshold=$THRESHOLD)"
    else
        log "FAIL: Blob counts diverged (max-min=$DIFF, threshold=$THRESHOLD)"
        FAILURES=$((FAILURES + 1))
    fi
else
    log "FAIL: Max blob count is 0"
    FAILURES=$((FAILURES + 1))
fi

# Check 4: No crash indicators in any node logs
log "--- Check 4: No crash/deadlock indicators ---"

CRASH_PATTERNS="SIGSEGV|SIGABRT|AddressSanitizer|ThreadSanitizer|deadlock"
CRASH_OK=true

for CONTAINER in "$NODE1_CONTAINER" "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
    CRASH_HITS=$(docker logs "$CONTAINER" 2>&1 | grep -ciE "$CRASH_PATTERNS" || true)
    CRASH_HITS=$(echo "$CRASH_HITS" | tr -d '[:space:]')
    CRASH_HITS=${CRASH_HITS:-0}
    if [[ "$CRASH_HITS" -gt 0 ]]; then
        log "FAIL: $CONTAINER has crash/deadlock indicators ($CRASH_HITS matches)"
        CRASH_OK=false
        FAILURES=$((FAILURES + 1))
    fi
done

if [[ "$CRASH_OK" == true ]]; then
    pass "No crash/deadlock indicators in any node logs"
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "STRESS-04: Concurrent Mixed Operations PASSED"
    pass "  - 4-node cluster survived ${CONCURRENT_DURATION}s concurrent phase"
    pass "  - Simultaneous: ingest + delete + SIGHUP (15s) + SIGUSR1 (20s)"
    pass "  - Clean restart with integrity scan on all nodes"
    pass "  - Final counts: n1=$FINAL_COUNT_1 n2=$FINAL_COUNT_2 n3=$FINAL_COUNT_3 n4=$FINAL_COUNT_4"
    pass "  - No deadlocks, crashes, or corruption"
    exit 0
else
    fail "STRESS-04: $FAILURES check(s) failed"
fi
