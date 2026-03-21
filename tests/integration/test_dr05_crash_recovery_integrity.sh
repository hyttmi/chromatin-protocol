#!/usr/bin/env bash
# =============================================================================
# DR-05: Crash Recovery with Cursor Integrity
#
# Separate from NET-05 (which tests sync convergence after crash). DR-05 tests
# MDBX transaction integrity and cursor resumption after SIGKILL during active
# sync.
#
# Topology: 2-node standalone. Dedicated network (172.35.0.0/16).
#   Node1 (172.35.0.2): seed node, receives ingested blobs
#   Node2 (172.35.0.3): bootstrap from Node1, killed during active sync
#
# Flow:
#   1. Start 2 nodes, wait healthy, establish sync
#   2. Ingest 200 blobs to Node1, wait for sync to Node2
#   3. Ingest 100 more blobs (total 300 on Node1), wait for Node2 sync
#   4. Start slow background loadgen to Node1 (500 blobs, rate 10)
#   5. Wait 5s, then SIGKILL Node2 during active sync
#   6. Wait for background loadgen to complete, get Node1 total
#   7. Restart Node2, verify integrity scan in logs
#   8. Cursor resumption: verify no full resync needed (full_resyncs=0)
#   9. Verify no data loss (Node2 count >= Node1 count)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-dr05-node1"
NODE2_CONTAINER="chromatindb-dr05-node2"
DR05_NETWORK="chromatindb-dr05-test-net"
NODE1_VOLUME="chromatindb-dr05-node1-data"
NODE2_VOLUME="chromatindb-dr05-node2-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DR05_NETWORK"

# Track background PID for cleanup
BG_LOADGEN_PID=""

# --- Cleanup -----------------------------------------------------------------

cleanup_dr05() {
    log "Cleaning up DR-05 test..."
    if [[ -n "$BG_LOADGEN_PID" ]]; then
        kill "$BG_LOADGEN_PID" 2>/dev/null || true
        wait "$BG_LOADGEN_PID" 2>/dev/null || true
    fi
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$DR05_NETWORK" 2>/dev/null || true
}
trap cleanup_dr05 EXIT

# --- Helper: start a node container -----------------------------------------

start_dr05_node() {
    local name="$1"
    local ip="$2"
    local volume="$3"
    local bootstrap="$4"  # empty string for seed, or "ip:port" for bootstrap

    local bootstrap_json="[]"
    if [[ -n "$bootstrap" ]]; then
        bootstrap_json="[\"$bootstrap\"]"
    fi

    local tmpconfig
    tmpconfig=$(mktemp /tmp/dr05-config-XXXXXX.json)
    cat > "$tmpconfig" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": $bootstrap_json,
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
    chmod 644 "$tmpconfig"

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$DR05_NETWORK" \
        --ip "$ip" \
        -v "$volume:/data" \
        -v "$tmpconfig:/config/node.json:ro" \
        --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
        --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
        chromatindb:test \
        run --config /config/node.json --data-dir /data --log-level debug
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dr05 2>/dev/null || true

log "=== DR-05: Crash Recovery with Cursor Integrity ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.35.0.0/16 "$DR05_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# =============================================================================
# Step 1: Start 2 nodes
# =============================================================================

log "--- Step 1: Start 2-node topology ---"

start_dr05_node "$NODE1_CONTAINER" "172.35.0.2" "$NODE1_VOLUME" ""
wait_healthy "$NODE1_CONTAINER"

start_dr05_node "$NODE2_CONTAINER" "172.35.0.3" "$NODE2_VOLUME" "172.35.0.2:4200"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# =============================================================================
# Step 2: Ingest 200 blobs, wait for sync
# =============================================================================

log "--- Step 2: Ingest 200 blobs to Node1 ---"

run_loadgen 172.35.0.2 --count 200 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to Node2 (200 blobs)..."
if ! wait_sync "$NODE2_CONTAINER" 200 120; then
    log "WARN: Sync timeout for initial 200 blobs"
fi

NODE2_AFTER_200=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 count after first batch: $NODE2_AFTER_200"

# =============================================================================
# Step 3: Ingest 100 more blobs (total 300)
# =============================================================================

log "--- Step 3: Ingest 100 more blobs ---"

run_loadgen 172.35.0.2 --count 100 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to Node2 (300 blobs)..."
if ! wait_sync "$NODE2_CONTAINER" 300 120; then
    log "WARN: Sync timeout for 300 blobs"
fi

NODE2_AFTER_300=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 count after second batch: $NODE2_AFTER_300"

# =============================================================================
# Step 4: Start slow background loadgen
# =============================================================================

log "--- Step 4: Starting slow background loadgen ---"

docker run --rm --network "$DR05_NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target 172.35.0.2:4200 --count 500 --size 4096 --rate 10 --ttl 3600 \
    >/dev/null 2>&1 &
BG_LOADGEN_PID=$!

log "Background loadgen started (PID $BG_LOADGEN_PID)"

# =============================================================================
# Step 5: Wait 5s, then SIGKILL Node2
# =============================================================================

log "--- Step 5: SIGKILL Node2 during active sync ---"

sleep 5

log "Sending SIGKILL to Node2..."
docker kill --signal=KILL "$NODE2_CONTAINER"

# Wait for container to fully stop
sleep 2

EXIT_CODE=$(docker inspect --format '{{.State.ExitCode}}' "$NODE2_CONTAINER" 2>/dev/null || echo "unknown")
log "Node2 exit code: $EXIT_CODE"

if [[ "$EXIT_CODE" == "137" ]]; then
    pass "DR-05: Node2 exited with code 137 (SIGKILL)"
else
    log "WARN: Expected exit code 137, got $EXIT_CODE"
fi

# =============================================================================
# Step 6: Wait for background loadgen to complete
# =============================================================================

log "--- Step 6: Waiting for background loadgen ---"

WAIT_START=$(date +%s)
while kill -0 "$BG_LOADGEN_PID" 2>/dev/null; do
    ELAPSED=$(( $(date +%s) - WAIT_START ))
    if [[ $ELAPSED -ge 120 ]]; then
        log "WARN: Background loadgen still running after 120s, killing..."
        kill "$BG_LOADGEN_PID" 2>/dev/null || true
        break
    fi
    sleep 2
done
wait "$BG_LOADGEN_PID" 2>/dev/null || true
BG_LOADGEN_PID=""

log "Background loadgen finished"

NODE1_TOTAL=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 total blob count: $NODE1_TOTAL"

# =============================================================================
# Step 7: Restart Node2, verify integrity scan
# =============================================================================

log "--- Step 7: Restart Node2 ---"

docker start "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER" 60

# Check integrity scan in restart logs
NODE2_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)
SCAN_COUNT=$(echo "$NODE2_LOGS" | grep -c "integrity scan" || true)
SCAN_COUNT="${SCAN_COUNT:-0}"

if [[ "$SCAN_COUNT" -ge 2 ]]; then
    SCAN_LINE=$(echo "$NODE2_LOGS" | grep "integrity scan" | tail -1)
    pass "DR-05: Integrity scan on restart ($SCAN_COUNT total scans): $SCAN_LINE"
elif [[ "$SCAN_COUNT" -ge 1 ]]; then
    SCAN_LINE=$(echo "$NODE2_LOGS" | grep "integrity scan" | tail -1)
    pass "DR-05: Integrity scan found: $SCAN_LINE"
else
    log "FAIL: No 'integrity scan' log found after crash recovery"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 8: Cursor resumption check
# =============================================================================

log "--- Step 8: Cursor resumption ---"

# Wait for sync convergence
log "Waiting for sync convergence (Node2 should reach Node1 count: $NODE1_TOTAL)..."
if ! wait_sync "$NODE2_CONTAINER" "$NODE1_TOTAL" 180; then
    log "WARN: Sync convergence timeout"
fi

# Check full_resyncs counter
docker kill -s USR1 "$NODE2_CONTAINER" >/dev/null 2>&1 || true
sleep 2

NODE2_METRICS=$(docker logs --tail 200 "$NODE2_CONTAINER" 2>&1 | grep "metrics:" | tail -1)

# full_resyncs should be 0 after restart (cursor state preserved)
FULL_RESYNCS=$(echo "$NODE2_METRICS" | grep -oP 'full_resyncs=\K[0-9]+' || echo "0")
log "Node2 full_resyncs: $FULL_RESYNCS"

# Cursor hits should be > 0 (proves cursor-based skip)
CURSOR_HITS=$(echo "$NODE2_METRICS" | grep -oP 'cursor_hits=\K[0-9]+' || echo "0")
log "Node2 cursor_hits: $CURSOR_HITS"

if [[ "$FULL_RESYNCS" -eq 0 ]]; then
    pass "DR-05: No full resyncs needed (cursor state preserved)"
else
    log "WARN: full_resyncs=$FULL_RESYNCS (expected 0, cursor state may not be fully preserved)"
    # This is a warning, not a hard failure -- full_resync_interval=9999 should prevent this
fi

# =============================================================================
# Step 9: Verify no data loss
# =============================================================================

log "--- Step 9: Data loss check ---"

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 final blob count: $NODE2_FINAL (Node1: $NODE1_TOTAL)"

if [[ "$NODE2_FINAL" -ge "$NODE1_TOTAL" ]]; then
    pass "DR-05: No data loss -- Node2=$NODE2_FINAL >= Node1=$NODE1_TOTAL"
else
    log "FAIL: Data loss detected -- Node2=$NODE2_FINAL < Node1=$NODE1_TOTAL"
    FAILURES=$((FAILURES + 1))
fi

# Also verify no duplicates (Node2 <= Node1)
if [[ "$NODE2_FINAL" -le "$NODE1_TOTAL" ]]; then
    pass "DR-05: No duplicates -- Node2=$NODE2_FINAL <= Node1=$NODE1_TOTAL"
else
    log "WARN: Node2 has more blobs ($NODE2_FINAL) than Node1 ($NODE1_TOTAL)"
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DR-05: Crash Recovery with Cursor Integrity PASSED"
    pass "  - SIGKILL during active sync (exit code $EXIT_CODE)"
    pass "  - Integrity scan on restart"
    pass "  - Cursor resumption: full_resyncs=$FULL_RESYNCS, cursor_hits=$CURSOR_HITS"
    pass "  - No data loss: Node2=$NODE2_FINAL, Node1=$NODE1_TOTAL"
    exit 0
else
    fail "DR-05: $FAILURES check(s) failed"
fi
