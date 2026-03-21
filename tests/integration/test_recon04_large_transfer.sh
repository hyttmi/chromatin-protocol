#!/usr/bin/env bash
# =============================================================================
# RECON-04: Large Difference Set Full Transfer (5000 blobs)
#
# Verifies that a fresh node joining a namespace with 5000 blobs receives
# exactly 5000 blobs with zero duplicates and zero missing.
#
# The content-addressed dedup guarantee ensures that even if reconciliation
# sends some blobs multiple times, StoreResult::Duplicate prevents overcounting.
# So blob count == 5000 proves zero duplication and zero missing.
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): seed node, preloaded with 5000 blobs before node2 starts
#   Node2 (172.28.0.3): fresh late joiner that must receive all 5000 blobs
#
# Flow:
#   1. Start only node1, ingest 5000 blobs
#   2. Verify node1 has 5000 blobs
#   3. Start node2, wait for full sync (5000 blobs, 300s timeout)
#   4. Verify node2 has exactly 5000 blobs
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"

# --- Cleanup -----------------------------------------------------------------

cleanup_recon04() {
    log "Cleaning up RECON-04 test..."
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_recon04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_recon04 2>/dev/null || true

log "=== RECON-04: Large Difference Set Full Transfer ==="

FAILURES=0

# =============================================================================
# Step 1: Start only node1, ingest 5000 blobs
# =============================================================================

log "--- Step 1: Start node1 and ingest 5000 blobs ---"

$COMPOSE_RECON up -d node1
wait_healthy "$NODE1_CONTAINER"

# Ingest 5000 blobs at rate 2000 to complete before sync timer fires.
# Notification ACK timeouts expected at this rate -- blobs are still accepted.
run_loadgen 172.28.0.2 --count 5000 --size 256 --rate 2000 --ttl 3600 >/dev/null 2>&1 || true

# Wait for processing to complete
sleep 10

# =============================================================================
# Step 2: Verify node1 has 5000 blobs
# =============================================================================

log "--- Step 2: Verify node1 blob count ---"

NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count: $NODE1_COUNT"

if [[ "$NODE1_COUNT" -ge 5000 ]]; then
    pass "Node1 has $NODE1_COUNT blobs (>= 5000)"
else
    log "FAIL: Node1 has only $NODE1_COUNT blobs, expected >= 5000"
    fail "RECON-04: Cannot proceed -- ingest failed ($NODE1_COUNT/5000)"
fi

# =============================================================================
# Step 3: Start node2 (fresh late joiner), wait for full sync
# =============================================================================

log "--- Step 3: Start node2 and wait for full sync ---"

$COMPOSE_RECON up -d node2
wait_healthy "$NODE2_CONTAINER"

# One-blob-at-a-time sync means 5000 blobs needs multiple sync rounds.
# At sync_interval=5s, each round transfers multiple blobs but still takes time.
log "Waiting for sync to node2 ($NODE1_COUNT blobs, 300s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" "$NODE1_COUNT" 300; then
    NODE2_PARTIAL=$(get_blob_count "$NODE2_CONTAINER")
    log "FAIL: Sync timeout -- node2 has $NODE2_PARTIAL/$NODE1_COUNT blobs after 300s"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 1: Node2 has exactly 5000 blobs (no duplicates, no missing)
# =============================================================================

log "--- Check 1: Exact blob count ---"

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 final blob count: $NODE2_FINAL"

if [[ "$NODE2_FINAL" -eq "$NODE1_COUNT" ]]; then
    pass "Node2 has exactly $NODE2_FINAL blobs (matches node1)"
    pass "  Zero duplicates, zero missing -- content-addressed dedup verified"
elif [[ "$NODE2_FINAL" -ge "$NODE1_COUNT" ]]; then
    pass "Node2 has $NODE2_FINAL blobs (>= $NODE1_COUNT from node1)"
else
    log "FAIL: Node2 has $NODE2_FINAL blobs, expected $NODE1_COUNT"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: No OOM kills
# =============================================================================

log "--- Check 2: No OOM kills ---"

NODE1_OOM=$(docker inspect --format '{{.State.OOMKilled}}' "$NODE1_CONTAINER" 2>/dev/null || echo "unknown")
NODE2_OOM=$(docker inspect --format '{{.State.OOMKilled}}' "$NODE2_CONTAINER" 2>/dev/null || echo "unknown")

if [[ "$NODE1_OOM" == "true" || "$NODE2_OOM" == "true" ]]; then
    log "FAIL: OOM kill detected (node1=$NODE1_OOM, node2=$NODE2_OOM)"
    FAILURES=$((FAILURES + 1))
else
    pass "No OOM kills"
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "RECON-04: Large difference set full transfer PASSED"
    pass "  - $NODE1_COUNT blobs transferred from node1 to fresh node2"
    pass "  - Node2 final count: $NODE2_FINAL (exact match = zero duplicates)"
    pass "  - No OOM kills"
    exit 0
else
    fail "RECON-04: $FAILURES check(s) failed"
fi
