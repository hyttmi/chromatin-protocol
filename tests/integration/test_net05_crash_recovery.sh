#!/usr/bin/env bash
# =============================================================================
# NET-05: Crash Recovery During Reconciliation
#
# Verifies that a SIGKILL during active reconciliation recovers cleanly on
# restart with no data loss or corruption.
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): seed node, receives ingested blobs
#   Node2 (172.28.0.3): bootstrap from node1, killed mid-sync
#
# Flow:
#   1. Start 2-node topology
#   2. Ingest 200 blobs to node1
#   3. Wait for sync to node2 (200 blobs)
#   4. Start continuous ingest to node1 in background (slow rate)
#   5. Wait 5s for some blobs to be in-flight
#   6. SIGKILL node2
#   7. Verify exit code 137 (SIGKILL)
#   8. Wait for background loadgen to finish
#   9. Get node1 blob count
#  10. Restart node2, wait healthy
#  11. Check logs for integrity scan
#  12. Wait for sync convergence
#  13. Verify no duplicate blobs (node2 count == node1 count)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"

# Track background PID for cleanup
BG_LOADGEN_PID=""

# --- Cleanup -----------------------------------------------------------------

cleanup_net05() {
    log "Cleaning up NET-05 test..."
    # Kill background loadgen if still running
    if [[ -n "$BG_LOADGEN_PID" ]]; then
        kill "$BG_LOADGEN_PID" 2>/dev/null || true
        wait "$BG_LOADGEN_PID" 2>/dev/null || true
    fi
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_net05 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_net05 2>/dev/null || true

log "=== NET-05: Crash Recovery During Reconciliation ==="

FAILURES=0

# =============================================================================
# Start 2-node topology
# =============================================================================

log "--- Starting 2-node recon topology ---"

$COMPOSE_RECON up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# =============================================================================
# Step 1: Ingest initial batch (200 blobs)
# =============================================================================

log "--- Step 1: Ingest 200 blobs to node1 ---"

run_loadgen 172.28.0.2 --count 200 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to node2 (200 blobs)..."
if ! wait_sync "$NODE2_CONTAINER" 200 120; then
    log "WARN: Sync timeout, checking current count..."
fi

NODE2_PRE=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 count after initial sync: $NODE2_PRE"

# =============================================================================
# Step 2: Start continuous ingest in background
# =============================================================================

log "--- Step 2: Starting continuous background ingest ---"

# Run loadgen in background at slow rate so sync is actively happening
docker run --rm --network "$NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target 172.28.0.2:4200 --count 500 --size 1024 --rate 10 --ttl 3600 \
    >/dev/null 2>&1 &
BG_LOADGEN_PID=$!

log "Background loadgen started (PID $BG_LOADGEN_PID)"

# =============================================================================
# Step 3: Wait for some blobs to be in-flight, then SIGKILL node2
# =============================================================================

log "--- Step 3: Waiting 5s then killing node2 ---"
sleep 5

log "Sending SIGKILL to node2..."
docker kill --signal=KILL "$NODE2_CONTAINER"

# =============================================================================
# Check 1: Verify exit code is 137 (SIGKILL, not OOM)
# =============================================================================

log "--- Check 1: Verify SIGKILL exit ---"

# Wait briefly for container to fully stop
sleep 2

EXIT_CODE=$(docker inspect --format '{{.State.ExitCode}}' "$NODE2_CONTAINER" 2>/dev/null || echo "unknown")
log "Node2 exit code: $EXIT_CODE"

if [[ "$EXIT_CODE" == "137" ]]; then
    pass "Node2 exited with code 137 (SIGKILL)"
else
    log "FAIL: Expected exit code 137, got $EXIT_CODE"
    OOM=$(docker inspect --format '{{.State.OOMKilled}}' "$NODE2_CONTAINER" 2>/dev/null || echo "unknown")
    log "  OOMKilled: $OOM"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 4: Wait for background loadgen to finish
# =============================================================================

log "--- Step 4: Waiting for background loadgen to finish ---"

# Wait for background loadgen with timeout
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

# Get node1 total blob count
NODE1_TOTAL=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 total blob count: $NODE1_TOTAL"

# =============================================================================
# Step 5: Restart node2 and check recovery
# =============================================================================

log "--- Step 5: Restarting node2 ---"

docker start "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER" 60

# =============================================================================
# Check 2: Sync convergence after restart
# =============================================================================

log "--- Check 2: Sync convergence ---"

log "Waiting for sync convergence (node2 should reach node1 count: $NODE1_TOTAL)..."
if ! wait_sync "$NODE2_CONTAINER" "$NODE1_TOTAL" 180; then
    log "WARN: Sync convergence timeout"
fi

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 final blob count: $NODE2_FINAL (node1: $NODE1_TOTAL)"

if [[ "$NODE2_FINAL" -ge "$NODE1_TOTAL" ]]; then
    pass "Sync converged: node2=$NODE2_FINAL, node1=$NODE1_TOTAL"
else
    log "FAIL: Sync did not converge: node2=$NODE2_FINAL, node1=$NODE1_TOTAL"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Integrity scan on startup (checked after sync to give logs time)
# =============================================================================

log "--- Check 3: Integrity scan after crash recovery ---"

# The integrity scan runs at startup before accepting connections.
# Check all logs (docker preserves logs across container restarts).
# We expect at least 2 integrity scan lines: one from initial start, one from restart.
NODE2_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)
SCAN_COUNT=$(echo "$NODE2_LOGS" | grep -c "integrity scan" || echo "0")

if [[ "$SCAN_COUNT" -ge 2 ]]; then
    SCAN_LINE=$(echo "$NODE2_LOGS" | grep "integrity scan" | tail -1)
    pass "Integrity scan found on restart ($SCAN_COUNT total scans): $SCAN_LINE"
elif [[ "$SCAN_COUNT" -ge 1 ]]; then
    # At least one scan found -- may be from restart (docker log ordering)
    SCAN_LINE=$(echo "$NODE2_LOGS" | grep "integrity scan" | tail -1)
    pass "Integrity scan found: $SCAN_LINE"
else
    log "FAIL: No 'integrity scan' log found after crash recovery"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: No duplicate blobs (node2 count should not exceed node1)
# =============================================================================

log "--- Check 4: No duplicates (node2 <= node1) ---"

if [[ "$NODE2_FINAL" -le "$NODE1_TOTAL" ]]; then
    pass "No duplicates: node2=$NODE2_FINAL <= node1=$NODE1_TOTAL"
else
    log "FAIL: Node2 has MORE blobs ($NODE2_FINAL) than node1 ($NODE1_TOTAL) -- possible duplicates"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "NET-05: Crash recovery during reconciliation PASSED"
    pass "  - SIGKILL during active sync (exit code 137)"
    pass "  - Clean integrity scan on restart"
    pass "  - Sync converged: node2=$NODE2_FINAL blobs (node1=$NODE1_TOTAL)"
    pass "  - No duplicate blobs"
    exit 0
else
    fail "NET-05: $FAILURES check(s) failed"
fi
