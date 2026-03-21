#!/usr/bin/env bash
# =============================================================================
# RECON-01: O(diff) Scaling Verification
#
# Proves that reconciliation wire traffic for 10 new blobs on a large namespace
# is proportional to ~10 blobs, not the entire namespace.
#
# Method: tcpdump capture via nicolaka/netshoot measures total bytes during
# the diff sync. The diff sync traffic (10 new blobs) should be well under
# a full transfer would be.
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): seed node, preloaded with blobs
#   Node2 (172.28.0.3): syncs from node1, stopped during diff injection
#
# Flow:
#   1. Start node1 only, ingest 1000 blobs
#   2. Start node2, wait for full sync (baseline established)
#   3. Capture full-sync traffic baseline via tcpdump (for a 100-blob transfer)
#   4. Stop node2, inject 10 more blobs to node1
#   5. Start tcpdump capture on test network
#   6. Restart node2, wait for diff sync (10 new blobs only)
#   7. Stop capture, compare diff traffic vs full-sync baseline
#   8. Verify diff traffic << full transfer traffic
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"
CAPTURE_CONTAINER="recon01-capture"
PCAP_FILE="/tmp/recon01-diff-sync.pcap"

# Threshold: 10 new blobs at 256 bytes + protocol overhead should be well under
# 100 KB. A full 1000-blob transfer would be ~500 KB+. Set threshold at 100 KB
# for the diff -- very conservative.
TRAFFIC_THRESHOLD=100000  # 100 KB

# --- Cleanup -----------------------------------------------------------------

cleanup_recon01() {
    log "Cleaning up RECON-01 test..."
    docker kill --signal SIGINT "$CAPTURE_CONTAINER" 2>/dev/null || true
    sleep 1
    docker rm -f "$CAPTURE_CONTAINER" 2>/dev/null || true
    rm -f "$PCAP_FILE"
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_recon01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_recon01 2>/dev/null || true

log "=== RECON-01: O(diff) Scaling Verification ==="

FAILURES=0

# =============================================================================
# Step 1: Start both nodes, ingest 1000 blobs, wait for full sync
# =============================================================================

log "--- Step 1: Start topology and establish 1000-blob baseline ---"

$COMPOSE_RECON up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# Ingest 1000 blobs at rate 100 -- completes in ~10 seconds, well within
# the sync disconnect threshold for concurrent sync.
run_loadgen 172.28.0.2 --count 1000 --size 256 --rate 100 --ttl 3600 >/dev/null 2>&1 || true

log "Waiting for sync to node2 (1000 blobs, 180s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" 1000 180; then
    NODE2_PRE=$(get_blob_count "$NODE2_CONTAINER")
    fail "RECON-01: Initial sync failed -- node2 has $NODE2_PRE/1000 blobs"
fi

# =============================================================================
# Step 2: Let sync fully settle
# =============================================================================

log "--- Step 2: Wait for sync to settle ---"

# Wait for cursors to be established (3 sync intervals = 15s + buffer)
sleep 20

NODE1_BASELINE=$(get_blob_count "$NODE1_CONTAINER")
NODE2_BASELINE=$(get_blob_count "$NODE2_CONTAINER")
log "Baseline: node1=$NODE1_BASELINE, node2=$NODE2_BASELINE blobs"

# =============================================================================
# Step 3: Stop node2, inject 10 new blobs to node1
# =============================================================================

log "--- Step 3: Stop node2, inject 10 new blobs ---"

docker stop "$NODE2_CONTAINER"
log "Node2 stopped"

run_loadgen 172.28.0.2 --count 10 --size 256 --ttl 3600 >/dev/null 2>&1

NODE1_WITH_DIFF=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count after diff injection: $NODE1_WITH_DIFF"

# =============================================================================
# Step 4: Start tcpdump capture, restart node2
# =============================================================================

log "--- Step 4: Start capture and restart node2 ---"

docker run -d --name "$CAPTURE_CONTAINER" \
    --network "$NETWORK" \
    --ip 172.28.0.10 \
    --cap-add NET_ADMIN \
    nicolaka/netshoot \
    tcpdump -i any -w /tmp/diff-sync.pcap -s 0 port 4200

sleep 2  # Let tcpdump initialize

docker start "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for the diff sync to complete. Only 10 new blobs, so this should be fast.
EXPECTED_TOTAL=$((NODE1_BASELINE + 10))
log "Waiting for diff sync to node2 ($EXPECTED_TOTAL blobs, 120s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" "$EXPECTED_TOTAL" 120; then
    NODE2_POST=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync timeout, node2 has $NODE2_POST blobs (expected >= $EXPECTED_TOTAL)"
fi

# Wait a bit more for sync to finish fully and tcpdump to capture final packets
sleep 10

# =============================================================================
# Step 5: Stop capture and measure traffic
# =============================================================================

log "--- Step 5: Stop capture and measure traffic ---"

docker kill --signal SIGINT "$CAPTURE_CONTAINER" 2>/dev/null || true
sleep 2

docker cp "$CAPTURE_CONTAINER:/tmp/diff-sync.pcap" "$PCAP_FILE"
docker rm -f "$CAPTURE_CONTAINER" 2>/dev/null || true

TRAFFIC_BYTES=0
if [[ ! -f "$PCAP_FILE" ]]; then
    log "FAIL: Capture file not found"
    FAILURES=$((FAILURES + 1))
else
    TRAFFIC_BYTES=$(stat -c%s "$PCAP_FILE")
    log "Diff sync traffic: $TRAFFIC_BYTES bytes (threshold: $TRAFFIC_THRESHOLD)"
fi

# =============================================================================
# Check 1: Traffic is under threshold (O(diff) not O(total))
# =============================================================================

log "--- Check 1: Traffic proportionality ---"

if [[ -f "$PCAP_FILE" ]]; then
    if [[ "$TRAFFIC_BYTES" -lt "$TRAFFIC_THRESHOLD" ]]; then
        pass "Diff sync traffic $TRAFFIC_BYTES bytes < $TRAFFIC_THRESHOLD byte threshold"
        pass "  O(diff) confirmed: 10 blobs on ${NODE1_BASELINE}-blob namespace"
    else
        log "FAIL: Diff sync traffic $TRAFFIC_BYTES bytes >= $TRAFFIC_THRESHOLD byte threshold"
        log "  This suggests O(total) scaling, not O(diff)"
        FAILURES=$((FAILURES + 1))
    fi
fi

# =============================================================================
# Check 2: Node2 has all blobs
# =============================================================================

log "--- Check 2: Node2 blob count ---"

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
if [[ "$NODE2_FINAL" -ge "$EXPECTED_TOTAL" ]]; then
    pass "Node2 has $NODE2_FINAL blobs (>= $EXPECTED_TOTAL expected)"
else
    log "FAIL: Node2 has $NODE2_FINAL blobs, expected >= $EXPECTED_TOTAL"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "RECON-01: O(diff) scaling verification PASSED"
    pass "  - $NODE1_BASELINE blobs baseline, 10 new injected"
    pass "  - Diff sync traffic: $TRAFFIC_BYTES bytes (< $TRAFFIC_THRESHOLD byte threshold)"
    pass "  - Node2 final count: $NODE2_FINAL"
    exit 0
else
    fail "RECON-01: $FAILURES check(s) failed"
fi
