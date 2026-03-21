#!/usr/bin/env bash
# =============================================================================
# TOPO-01: Connection Dedup
#
# Verifies:
#   1. Two nodes configured as mutual bootstrap peers produce exactly one
#      logical connection each (peers=1, not peers=2)
#   2. Sync works on the surviving (deduped) connection
#
# Topology: docker-compose.dedup.yml
#   Node1 (172.28.0.2): bootstrap_peers = [172.28.0.3:4200]
#   Node2 (172.28.0.3): bootstrap_peers = [172.28.0.2:4200]
#
# Method: Both nodes initiate connections simultaneously. The connection dedup
#   logic in on_peer_connected detects the duplicate and deterministically
#   closes one connection on each side. SIGUSR1 metrics confirm peers=1.
#   A loadgen ingest + sync verifies the surviving connection is functional.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_DEDUP="docker compose -f $SCRIPT_DIR/docker-compose.dedup.yml -p chromatindb-test"

# Override helpers.sh NETWORK for run_loadgen (compose project name creates this)
NETWORK="chromatindb-test_test-net"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

# --- Cleanup -----------------------------------------------------------------

cleanup_topo01() {
    log "Cleaning up TOPO-01 test..."
    $COMPOSE_DEDUP down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_topo01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_topo01 2>/dev/null || true

log "=== TOPO-01: Connection Dedup ==="

FAILURES=0

# =============================================================================
# Start mutual-peer topology
# =============================================================================

log "--- Starting mutual-peer topology ---"

$COMPOSE_DEDUP up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for both sides to attempt connections and for dedup to settle.
# Both nodes initiate outbound connections simultaneously. The dedup logic
# fires on the second connection arrival and closes one deterministically.
sleep 15

# =============================================================================
# Check 1: Node1 has exactly 1 peer
# =============================================================================

log "--- Check 1: Node1 peer count ---"

docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2
NODE1_PEERS=$(docker logs "$NODE1_CONTAINER" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+' || echo "0")
log "Node1 peers: $NODE1_PEERS"

if [[ "$NODE1_PEERS" -eq 1 ]]; then
    pass "Node1 has exactly 1 peer (dedup working)"
else
    log "FAIL: Expected peers=1 on Node1, got peers=$NODE1_PEERS"
    docker logs "$NODE1_CONTAINER" 2>&1 | grep -E "(duplicate|Connected|disconnected)" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node2 has exactly 1 peer
# =============================================================================

log "--- Check 2: Node2 peer count ---"

docker kill -s USR1 "$NODE2_CONTAINER" >/dev/null 2>&1 || true
sleep 2
NODE2_PEERS=$(docker logs "$NODE2_CONTAINER" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+' || echo "0")
log "Node2 peers: $NODE2_PEERS"

if [[ "$NODE2_PEERS" -eq 1 ]]; then
    pass "Node2 has exactly 1 peer (dedup working)"
else
    log "FAIL: Expected peers=1 on Node2, got peers=$NODE2_PEERS"
    docker logs "$NODE2_CONTAINER" 2>&1 | grep -E "(duplicate|Connected|disconnected)" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Dedup log message present
# =============================================================================

log "--- Check 3: Dedup log messages ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
NODE2_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)

DEDUP_COUNT=0
if echo "$NODE1_LOGS" | grep -q "duplicate connection from peer"; then
    DEDUP_COUNT=$((DEDUP_COUNT + 1))
fi
if echo "$NODE2_LOGS" | grep -q "duplicate connection from peer"; then
    DEDUP_COUNT=$((DEDUP_COUNT + 1))
fi

if [[ "$DEDUP_COUNT" -ge 1 ]]; then
    pass "Dedup detected on $DEDUP_COUNT node(s)"
else
    log "FAIL: No dedup log messages found on either node"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Sync works on the surviving connection
# =============================================================================

log "--- Check 4: Sync on deduped connection ---"

log "Ingesting 5 blobs to Node1..."
run_loadgen 172.28.0.2 --count 5 --size 256 --ttl 3600

log "Waiting for sync to Node2..."
wait_sync "$NODE2_CONTAINER" 5 60

BLOB_COUNT=$(get_blob_count "$NODE2_CONTAINER")
if [[ "$BLOB_COUNT" -ge 5 ]]; then
    pass "Sync works on deduped connection ($BLOB_COUNT blobs on Node2)"
else
    log "FAIL: Only $BLOB_COUNT/5 blobs synced on deduped connection"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "TOPO-01: Connection dedup PASSED"
    pass "  - Both nodes have peers=1 (not peers=2)"
    pass "  - Dedup log messages present"
    pass "  - Sync functional on surviving connection"
    exit 0
else
    fail "TOPO-01: $FAILURES check(s) failed"
fi
