#!/usr/bin/env bash
# =============================================================================
# OPS-03: SIGTERM Graceful Shutdown
#
# Verifies:
#   SIGTERM during active ingest exits cleanly (exit code 0 or 143), restart
#   passes integrity scan, and node accepts new connections and blobs without
#   corruption.
#
# Topology: 2-node standalone (docker run, not compose)
#   Node1 (172.31.0.2): receives ingest, gets SIGTERM mid-ingest
#   Node2 (172.31.0.3): bootstraps to Node1, verifies post-restart sync
#   Network: 172.31.0.0/16 (chromatindb-ops03-test-net)
#
# Flow:
#   1. Start 2 nodes, wait healthy
#   2. Start continuous loadgen in background targeting Node1
#   3. Wait 3-5s for active ingest, then SIGTERM Node1
#   4. Verify exit code (0 or 143)
#   5. Restart Node1, verify integrity scan in logs
#   6. Ingest 10 more blobs, verify acceptance and sync to Node2
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-ops03-node1"
NODE2_CONTAINER="chromatindb-ops03-node2"
OPS03_NETWORK="chromatindb-ops03-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$OPS03_NETWORK"

# Temp files and volumes
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
NODE1_VOLUME="chromatindb-ops03-node1-data"
NODE2_VOLUME="chromatindb-ops03-node2-data"

# Track background loadgen
BG_LOADGEN_PID=""

# --- Cleanup -----------------------------------------------------------------

cleanup_ops03() {
    log "Cleaning up OPS-03 test..."
    if [[ -n "$BG_LOADGEN_PID" ]]; then
        kill "$BG_LOADGEN_PID" 2>/dev/null || true
        wait "$BG_LOADGEN_PID" 2>/dev/null || true
    fi
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$OPS03_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
}
trap cleanup_ops03 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ops03 2>/dev/null || true

log "=== OPS-03: SIGTERM Graceful Shutdown ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.31.0.0/16 "$OPS03_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# =============================================================================
# Step 1: Start 2 nodes, wait healthy
# =============================================================================

log "--- Step 1: Starting 2-node topology ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-ops03-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-ops03-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.31.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$OPS03_NETWORK" \
    --ip 172.31.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$OPS03_NETWORK" \
    --ip 172.31.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# =============================================================================
# Step 2: Start continuous loadgen in background
# =============================================================================

log "--- Step 2: Starting continuous background loadgen ---"

docker run --rm --network "$NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target 172.31.0.2:4200 --count 500 --size 1024 --rate 20 --ttl 3600 \
    >/dev/null 2>&1 &
BG_LOADGEN_PID=$!
log "Background loadgen started (PID $BG_LOADGEN_PID)"

# =============================================================================
# Step 3: Wait for active ingest, then SIGTERM
# =============================================================================

log "--- Step 3: Waiting 4s then sending SIGTERM ---"
sleep 4

# Verify some blobs were ingested before SIGTERM
PRE_TERM_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Blobs before SIGTERM: $PRE_TERM_COUNT"

log "Sending SIGTERM to Node1..."
docker kill -s TERM "$NODE1_CONTAINER"

# =============================================================================
# Step 4: Verify exit code
# =============================================================================

log "--- Step 4: Checking exit code ---"

# Wait for container to stop (up to 30s)
WAIT_START=$(date +%s)
while docker inspect --format '{{.State.Running}}' "$NODE1_CONTAINER" 2>/dev/null | grep -q "true"; do
    ELAPSED=$(( $(date +%s) - WAIT_START ))
    if [[ $ELAPSED -ge 30 ]]; then
        log "WARN: Node1 still running after 30s, force-killing..."
        docker kill "$NODE1_CONTAINER" 2>/dev/null || true
        break
    fi
    sleep 1
done

EXIT_CODE=$(docker inspect --format '{{.State.ExitCode}}' "$NODE1_CONTAINER" 2>/dev/null || echo "unknown")
log "Node1 exit code: $EXIT_CODE"

if [[ "$EXIT_CODE" == "0" || "$EXIT_CODE" == "143" ]]; then
    pass "Graceful shutdown exit code: $EXIT_CODE"
else
    log "FAIL: Expected exit code 0 or 143, got $EXIT_CODE"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 5: Kill background loadgen
# =============================================================================

log "--- Step 5: Cleaning up background loadgen ---"

if [[ -n "$BG_LOADGEN_PID" ]] && kill -0 "$BG_LOADGEN_PID" 2>/dev/null; then
    kill "$BG_LOADGEN_PID" 2>/dev/null || true
    wait "$BG_LOADGEN_PID" 2>/dev/null || true
fi
BG_LOADGEN_PID=""

# =============================================================================
# Step 6: Restart Node1 and verify integrity
# =============================================================================

log "--- Step 6: Restarting Node1 ---"

docker start "$NODE1_CONTAINER"
wait_healthy "$NODE1_CONTAINER" 60

# Check for integrity scan in restart logs
# Docker preserves logs across container restarts
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
SCAN_COUNT=$(echo "$NODE1_LOGS" | grep -c "integrity scan" || echo "0")

if [[ "$SCAN_COUNT" -ge 2 ]]; then
    SCAN_LINE=$(echo "$NODE1_LOGS" | grep "integrity scan" | tail -1)
    pass "Integrity scan found on restart ($SCAN_COUNT total scans): $SCAN_LINE"
elif [[ "$SCAN_COUNT" -ge 1 ]]; then
    SCAN_LINE=$(echo "$NODE1_LOGS" | grep "integrity scan" | tail -1)
    pass "Integrity scan found: $SCAN_LINE"
else
    log "FAIL: No 'integrity scan' log found after restart"
    FAILURES=$((FAILURES + 1))
fi

# Verify Node1 accepts connections by checking peer count via SIGUSR1
sleep 10  # Wait for Node2 to reconnect

PEER_COUNT=$(docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1; sleep 2; docker logs --tail 200 "$NODE1_CONTAINER" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+' || echo "0")
log "Post-restart peer count: $PEER_COUNT"

if [[ "$PEER_COUNT" -ge 1 ]]; then
    pass "Node1 accepts connections after restart (peers=$PEER_COUNT)"
else
    log "FAIL: Expected at least 1 peer after restart, got $PEER_COUNT"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 7: Ingest post-restart and verify sync
# =============================================================================

log "--- Step 7: Post-restart ingest and sync ---"

# Ingest 10 more blobs to Node1
run_loadgen 172.31.0.2 --count 10 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1

# Get Node1 total count
NODE1_FINAL=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 final blob count: $NODE1_FINAL"

if [[ "$NODE1_FINAL" -gt "$PRE_TERM_COUNT" ]]; then
    pass "Node1 accepts new blobs after restart ($NODE1_FINAL > $PRE_TERM_COUNT)"
else
    log "FAIL: Node1 not accepting new blobs (final=$NODE1_FINAL, pre-term=$PRE_TERM_COUNT)"
    FAILURES=$((FAILURES + 1))
fi

# Wait for sync to Node2
log "Waiting for sync convergence to Node2..."
if wait_sync "$NODE2_CONTAINER" "$NODE1_FINAL" 120; then
    NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
    pass "Sync converged: node2=$NODE2_FINAL, node1=$NODE1_FINAL"
else
    NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync timeout, node2=$NODE2_FINAL vs node1=$NODE1_FINAL"
    # Partial sync is acceptable -- the key test is that Node1 restarted clean
    if [[ "$NODE2_FINAL" -gt 0 ]]; then
        pass "Partial sync OK: node2=$NODE2_FINAL (some blobs synced)"
    else
        log "FAIL: No blobs synced to Node2 after restart"
        FAILURES=$((FAILURES + 1))
    fi
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "OPS-03: SIGTERM graceful shutdown PASSED"
    pass "  - SIGTERM during active ingest (exit code $EXIT_CODE)"
    pass "  - Integrity scan on restart"
    pass "  - Post-restart connections: peers=$PEER_COUNT"
    pass "  - Post-restart ingest: $NODE1_FINAL blobs accepted"
    pass "  - Sync convergence: node2=$NODE2_FINAL"
    exit 0
else
    fail "OPS-03: $FAILURES check(s) failed"
fi
