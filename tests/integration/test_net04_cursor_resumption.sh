#!/usr/bin/env bash
# =============================================================================
# NET-04: Sync Cursor Resumption
#
# Verifies that a stopped/restarted node syncs only new blobs via cursor
# resumption, not a full re-sync. Proves cursor persistence across restarts.
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): seed node, receives ingested blobs
#   Node2 (172.28.0.3): bootstrap from node1, stopped and restarted mid-test
#
# Flow:
#   1. Start 2-node topology
#   2. Ingest 500 blobs to node1
#   3. Wait for sync to node2 (500 blobs)
#   4. Stop node2
#   5. Ingest 100 more blobs to node1
#   6. Start node2, wait healthy
#   7. Wait for sync to node2 (600 blobs)
#   8. Verify cursor_hits > 0 via SIGUSR1 metrics
#   9. Verify blob count >= 600
#
# CRITICAL: full_resync_interval must be high (9999) in configs to prevent
#   periodic full resyncs from defeating cursor-based skip detection.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"

# --- Cleanup -----------------------------------------------------------------

cleanup_net04() {
    log "Cleaning up NET-04 test..."
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_net04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_net04 2>/dev/null || true

log "=== NET-04: Sync Cursor Resumption ==="

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
# Step 1: Ingest initial batch (500 blobs)
# =============================================================================

log "--- Step 1: Ingest 500 blobs to node1 ---"

run_loadgen 172.28.0.2 --count 500 --size 256 --rate 100 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to node2 (500 blobs, 120s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" 500 120; then
    log "WARN: Sync timeout, checking current count..."
fi

NODE2_PRE=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 count after initial sync: $NODE2_PRE"

if [[ "$NODE2_PRE" -lt 500 ]]; then
    log "FAIL: Node2 only has $NODE2_PRE/500 blobs after initial sync"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 2: Stop node2
# =============================================================================

log "--- Step 2: Stopping node2 ---"

docker stop "$NODE2_CONTAINER"
log "Node2 stopped"

# =============================================================================
# Step 3: Ingest additional batch (100 more blobs)
# =============================================================================

log "--- Step 3: Ingest 100 more blobs to node1 ---"

run_loadgen 172.28.0.2 --count 100 --size 256 --rate 100 --ttl 3600 >/dev/null 2>&1

NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 total blob count: $NODE1_COUNT"

# =============================================================================
# Step 4: Restart node2 and wait for sync
# =============================================================================

log "--- Step 4: Restarting node2 ---"

docker start "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

log "Waiting for sync to node2 (600 blobs, 120s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" 600 120; then
    log "WARN: Sync timeout, checking current count..."
fi

NODE2_POST=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 count after restart sync: $NODE2_POST"

# =============================================================================
# Check 1: All blobs synced
# =============================================================================

log "--- Check 1: Blob count after restart ---"

if [[ "$NODE2_POST" -ge 600 ]]; then
    pass "Node2 has $NODE2_POST blobs (>= 600 expected)"
else
    log "FAIL: Node2 has $NODE2_POST blobs, expected >= 600"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Cursor hits > 0 (proves cursor-based resumption)
# =============================================================================

log "--- Check 2: Cursor hits metric ---"

# Trigger metrics dump on node2
docker kill -s USR1 "$NODE2_CONTAINER" >/dev/null 2>&1 || true
sleep 2

# Parse cursor_hits from the latest metrics line
CURSOR_HITS=$(docker logs "$NODE2_CONTAINER" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'cursor_hits=\K[0-9]+' || echo "0")
log "Node2 cursor_hits: $CURSOR_HITS"

if [[ "$CURSOR_HITS" -gt 0 ]]; then
    pass "Cursor hits > 0 ($CURSOR_HITS) -- cursor-based resumption confirmed"
else
    log "FAIL: cursor_hits=$CURSOR_HITS, expected > 0 (cursor resumption not detected)"
    log "  This could mean full_resync_interval is too low or cursors are not persisting"
    docker logs "$NODE2_CONTAINER" 2>&1 | grep -E "(cursor|resync|full.resync)" | tail -10 >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "NET-04: Sync cursor resumption PASSED"
    pass "  - Initial sync: $NODE2_PRE blobs"
    pass "  - After stop/restart: $NODE2_POST blobs (100 new synced)"
    pass "  - cursor_hits=$CURSOR_HITS (cursor-based skip confirmed)"
    exit 0
else
    fail "NET-04: $FAILURES check(s) failed"
fi
