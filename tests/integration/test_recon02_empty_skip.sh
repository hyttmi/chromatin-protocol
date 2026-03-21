#!/usr/bin/env bash
# =============================================================================
# RECON-02: Identical Namespace Skip (Cursor Hit)
#
# Proves that identical namespaces skip reconciliation entirely via cursor match.
# When no new blobs are added, subsequent sync rounds produce cursor_hits
# (namespace skipped) rather than cursor_misses (reconciliation triggered).
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): seed node
#   Node2 (172.28.0.3): bootstrap from node1
#
# Flow:
#   1. Start 2-node topology
#   2. Ingest 100 blobs to node1
#   3. Wait for sync to node2 (100 blobs)
#   4. Wait for cursors to establish (2 sync intervals)
#   5. Record cursor_hits (baseline)
#   6. Wait for 3 more sync intervals (no changes)
#   7. Record cursor_hits again
#   8. Verify cursor_hits increased (syncs happening, namespaces skipped)
#   9. Verify cursor_misses did NOT increase (no changes = no misses)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"

# --- Cleanup -----------------------------------------------------------------

cleanup_recon02() {
    log "Cleaning up RECON-02 test..."
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_recon02 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_recon02 2>/dev/null || true

log "=== RECON-02: Identical Namespace Skip ==="

FAILURES=0

# =============================================================================
# Step 1: Start 2-node topology
# =============================================================================

log "--- Step 1: Start 2-node recon topology ---"

$COMPOSE_RECON up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# =============================================================================
# Step 2: Ingest 100 blobs
# =============================================================================

log "--- Step 2: Ingest 100 blobs ---"

run_loadgen 172.28.0.2 --count 100 --size 256 --rate 50 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to node2 (100 blobs)..."
if ! wait_sync "$NODE2_CONTAINER" 100 120; then
    NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
    fail "RECON-02: Sync failed -- node2 has $NODE2_COUNT/100 blobs"
fi

# =============================================================================
# Step 3: Wait for cursors to establish
# =============================================================================

log "--- Step 3: Wait for cursors to establish (15s) ---"

# Sync interval is 5s. Wait 3 intervals for cursors to be stored after sync.
sleep 15

# =============================================================================
# Step 4: Record baseline cursor_hits
# =============================================================================

log "--- Step 4: Record baseline cursor metrics ---"

docker kill -s USR1 "$NODE2_CONTAINER" >/dev/null 2>&1 || true
sleep 2

METRICS_LINE_1=$(docker logs "$NODE2_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
CURSOR_HITS_1=$(echo "$METRICS_LINE_1" | grep -oP 'cursor_hits=\K[0-9]+' || echo "0")
CURSOR_MISSES_1=$(echo "$METRICS_LINE_1" | grep -oP 'cursor_misses=\K[0-9]+' || echo "0")

log "Baseline metrics: cursor_hits=$CURSOR_HITS_1, cursor_misses=$CURSOR_MISSES_1"

# =============================================================================
# Step 5: Wait for 3 more sync intervals (no changes)
# =============================================================================

log "--- Step 5: Wait for 3 more sync intervals (20s, no changes) ---"

sleep 20

# =============================================================================
# Step 6: Record updated cursor_hits
# =============================================================================

log "--- Step 6: Record updated cursor metrics ---"

docker kill -s USR1 "$NODE2_CONTAINER" >/dev/null 2>&1 || true
sleep 2

METRICS_LINE_2=$(docker logs "$NODE2_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
CURSOR_HITS_2=$(echo "$METRICS_LINE_2" | grep -oP 'cursor_hits=\K[0-9]+' || echo "0")
CURSOR_MISSES_2=$(echo "$METRICS_LINE_2" | grep -oP 'cursor_misses=\K[0-9]+' || echo "0")

log "Updated metrics: cursor_hits=$CURSOR_HITS_2, cursor_misses=$CURSOR_MISSES_2"

# =============================================================================
# Check 1: cursor_hits increased
# =============================================================================

log "--- Check 1: cursor_hits increased ---"

if [[ "$CURSOR_HITS_2" -gt "$CURSOR_HITS_1" ]]; then
    HITS_DELTA=$((CURSOR_HITS_2 - CURSOR_HITS_1))
    pass "cursor_hits increased by $HITS_DELTA ($CURSOR_HITS_1 -> $CURSOR_HITS_2)"
    pass "  Syncs are happening but identical namespaces are being skipped"
else
    log "FAIL: cursor_hits did not increase ($CURSOR_HITS_1 -> $CURSOR_HITS_2)"
    log "  Expected cursor_hits to grow as sync rounds skip unchanged namespaces"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: cursor_misses did NOT increase
# =============================================================================

log "--- Check 2: cursor_misses stable ---"

if [[ "$CURSOR_MISSES_2" -eq "$CURSOR_MISSES_1" ]]; then
    pass "cursor_misses unchanged ($CURSOR_MISSES_1 -> $CURSOR_MISSES_2)"
    pass "  No reconciliation triggered (no new data)"
else
    MISSES_DELTA=$((CURSOR_MISSES_2 - CURSOR_MISSES_1))
    log "FAIL: cursor_misses increased by $MISSES_DELTA ($CURSOR_MISSES_1 -> $CURSOR_MISSES_2)"
    log "  Something triggered reconciliation despite no new data"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Blob count still correct
# =============================================================================

log "--- Check 3: Blob count unchanged ---"

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
if [[ "$NODE2_FINAL" -ge 100 ]]; then
    pass "Node2 blob count stable at $NODE2_FINAL"
else
    log "FAIL: Node2 blob count dropped to $NODE2_FINAL (expected >= 100)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "RECON-02: Identical namespace skip PASSED"
    pass "  - 100 blobs synced, then 3 idle sync intervals"
    pass "  - cursor_hits: $CURSOR_HITS_1 -> $CURSOR_HITS_2 (increasing = skipping)"
    pass "  - cursor_misses: $CURSOR_MISSES_1 -> $CURSOR_MISSES_2 (stable = no recon)"
    exit 0
else
    fail "RECON-02: $FAILURES check(s) failed"
fi
