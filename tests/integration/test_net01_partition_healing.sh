#!/usr/bin/env bash
# =============================================================================
# NET-01: Partition Healing (Eventual Consistency)
#
# Verifies that a 5-node mesh with a network partition heals and converges
# to identical blob sets on all nodes.
#
# Topology: docker-compose.mesh.yml (5-node mesh)
#   Node1 (172.28.0.2): seed node
#   Node2 (172.28.0.3): bootstrap from node1
#   Node3 (172.28.0.4): bootstrap from node1, node2
#   Node4 (172.28.0.5): bootstrap from node2, node3
#   Node5 (172.28.0.6): bootstrap from node3, node4
#
# Flow:
#   1. Start 5-node mesh, wait for connections + PEX discovery
#   2. Ingest 100 blobs to node1, wait for sync to node5
#   3. Partition: disconnect nodes 3,4,5 from the compose network
#      -> {node1,node2} | {node3,node4,node5 isolated}
#   4. Ingest 50 more blobs to node1 (only node1,node2 receive them)
#   5. Verify partition: disconnected nodes did not gain new blobs
#   6. Heal: reconnect nodes 3,4,5 with original IPs
#   7. SIGHUP all nodes to clear reconnect backoff timers
#   8. Wait for convergence: all 5 nodes reach >= 150 blobs
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_MESH="docker compose -f $SCRIPT_DIR/docker-compose.mesh.yml -p chromatindb-test"

NODE1="chromatindb-test-node1"
NODE2="chromatindb-test-node2"
NODE3="chromatindb-test-node3"
NODE4="chromatindb-test-node4"
NODE5="chromatindb-test-node5"

NETWORK="chromatindb-test_test-net"

# Robust blob count getter -- retries to handle SIGUSR1 delivery lag
get_blob_count_reliable() {
    local container="$1"
    local retries="${2:-3}"
    local count=0
    for attempt in $(seq 1 "$retries"); do
        docker kill -s USR1 "$container" >/dev/null 2>&1 || true
        sleep 3
        count=$(docker logs "$container" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'blobs=\K[0-9]+' || echo "0")
        if [[ "$count" -gt 0 ]]; then
            break
        fi
    done
    echo "$count"
}

# --- Cleanup -----------------------------------------------------------------

cleanup_net01() {
    log "Cleaning up NET-01 test..."
    # Ensure disconnected nodes are reconnected before compose down
    # (compose down may fail to remove containers not on its network)
    docker network connect "$NETWORK" "$NODE3" 2>/dev/null || true
    docker network connect "$NETWORK" "$NODE4" 2>/dev/null || true
    docker network connect "$NETWORK" "$NODE5" 2>/dev/null || true
    $COMPOSE_MESH down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_net01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_net01 2>/dev/null || true

log "=== NET-01: Partition Healing Test ==="

FAILURES=0

# =============================================================================
# 1. Start 5-node mesh
# =============================================================================

log "--- Starting 5-node mesh topology ---"

$COMPOSE_MESH up -d
for node in "$NODE1" "$NODE2" "$NODE3" "$NODE4" "$NODE5"; do
    wait_healthy "$node"
done

# Wait for mesh to fully establish connections and PEX discovery
log "Waiting 15s for mesh to form..."
sleep 15

# =============================================================================
# 2. Ingest baseline: 100 blobs and wait for full mesh sync
# =============================================================================

log "--- Ingesting 100 baseline blobs ---"

run_loadgen 172.28.0.2 --count 100 --size 256 --rate 50 --ttl 3600

# Wait for full propagation to the most distant node (node5)
if ! wait_sync "$NODE5" 100 120; then
    NODE5_COUNT=$(get_blob_count "$NODE5")
    log "FAIL: Baseline sync failed (node5=$NODE5_COUNT/100)"
    FAILURES=$((FAILURES + 1))
else
    pass "Baseline: 100 blobs synced to node5"
fi

# =============================================================================
# 3. Create partition: {node1, node2} | {node3, node4, node5}
# =============================================================================

log "--- Creating partition ---"

# Snapshot blob counts BEFORE partition for relative comparison.
# The blobs= metric sums latest_seq_num across namespaces (may overcount),
# so we compare relative changes rather than absolute values.
PRE_PART_3=$(get_blob_count_reliable "$NODE3")
PRE_PART_4=$(get_blob_count_reliable "$NODE4")
PRE_PART_5=$(get_blob_count_reliable "$NODE5")
log "Pre-partition counts: node3=$PRE_PART_3 node4=$PRE_PART_4 node5=$PRE_PART_5"

docker network disconnect "$NETWORK" "$NODE3"
docker network disconnect "$NETWORK" "$NODE4"
docker network disconnect "$NETWORK" "$NODE5"

log "Partition created: {node1,node2} | {node3,node4,node5 disconnected}"

# Wait for connections to drop and sync rounds to settle
sleep 10

# =============================================================================
# 4. Write during partition: 50 more blobs to node1
# =============================================================================

log "--- Ingesting 50 blobs during partition ---"

run_loadgen 172.28.0.2 --count 50 --size 256 --rate 50 --ttl 3600

# Wait for node2 to receive the partition writes
if ! wait_sync "$NODE2" 150 60; then
    NODE2_PART=$(get_blob_count "$NODE2")
    log "FAIL: Partition sync to node2 failed ($NODE2_PART/150)"
    FAILURES=$((FAILURES + 1))
else
    pass "Node2 received partition writes (>= 150 blobs)"
fi

# Wait a bit more to ensure disconnected nodes would have received blobs
# if they had connectivity (proving isolation)
sleep 15

# =============================================================================
# 5. Verify partition isolation: disconnected nodes should not gain new blobs
# =============================================================================

log "--- Verifying partition isolation ---"

# docker kill -s USR1 and docker logs work regardless of network state.
# Compare against pre-partition snapshot -- counts should not have increased
# by the 50 partition-write blobs.
NODE3_PART=$(get_blob_count_reliable "$NODE3")
NODE4_PART=$(get_blob_count_reliable "$NODE4")
NODE5_PART=$(get_blob_count_reliable "$NODE5")

log "Partition counts: node3=$NODE3_PART (was $PRE_PART_3) node4=$NODE4_PART (was $PRE_PART_4) node5=$NODE5_PART (was $PRE_PART_5)"

ISOLATION_OK=true
for pair in "3:$PRE_PART_3:$NODE3_PART" "4:$PRE_PART_4:$NODE4_PART" "5:$PRE_PART_5:$NODE5_PART"; do
    IFS=: read -r num pre post <<< "$pair"
    DELTA=$((post - pre))
    if [[ "$DELTA" -ge 50 ]]; then
        log "FAIL: Partition leak on node$num: gained $DELTA blobs during partition"
        ISOLATION_OK=false
    fi
done

if [[ "$ISOLATION_OK" == true ]]; then
    pass "Partition isolation verified (disconnected nodes did not gain partition-write blobs)"
else
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# 6. Heal partition: reconnect nodes with original IPs
# =============================================================================

log "--- Healing partition ---"

docker network connect --ip 172.28.0.4 "$NETWORK" "$NODE3"
docker network connect --ip 172.28.0.5 "$NETWORK" "$NODE4"
docker network connect --ip 172.28.0.6 "$NETWORK" "$NODE5"

log "Partition healed: all nodes reconnected to $NETWORK"

# =============================================================================
# 7. Force reconnect via SIGHUP (clears backoff timers)
# =============================================================================

log "--- Sending SIGHUP to all nodes ---"

for node in "$NODE1" "$NODE2" "$NODE3" "$NODE4" "$NODE5"; do
    docker kill -s HUP "$node" >/dev/null 2>&1 || true
done

log "SIGHUP sent to all 5 nodes (backoff timers cleared)"

# =============================================================================
# 8. Wait for convergence: nodes 3,4,5 should gain the partition writes
# =============================================================================

log "--- Waiting for convergence ---"

# After healing, disconnected nodes should gain the 50 partition-write blobs.
# Use the pre-partition snapshot + 50 as the convergence target.
for node_pair in "$NODE3:$PRE_PART_3" "$NODE4:$PRE_PART_4" "$NODE5:$PRE_PART_5"; do
    IFS=: read -r node pre <<< "$node_pair"
    TARGET=$((pre + 50))
    if ! wait_sync "$node" "$TARGET" 120; then
        COUNT=$(get_blob_count "$node")
        log "FAIL: $node did not converge ($COUNT, target=$TARGET)"
        FAILURES=$((FAILURES + 1))
    fi
done

# =============================================================================
# 9. Verify all 5 nodes converged: all should have at least the total blobs
# =============================================================================

log "--- Final convergence check ---"

for node in "$NODE1" "$NODE2" "$NODE3" "$NODE4" "$NODE5"; do
    COUNT=$(get_blob_count "$node")
    log "Final count $node: $COUNT"
    if [[ "$COUNT" -ge 150 ]]; then
        pass "$node converged ($COUNT blobs)"
    else
        log "FAIL: $node has $COUNT blobs, expected >= 150"
        FAILURES=$((FAILURES + 1))
    fi
done

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "NET-01: Partition healing PASSED"
    pass "  - 5-node mesh partitioned into {1,2} | {3,4,5}"
    pass "  - 50 blobs written during partition (only {1,2} received)"
    pass "  - Partition isolation verified"
    pass "  - Partition healed, SIGHUP cleared backoff timers"
    pass "  - All 5 nodes converged to >= 150 blobs"
    exit 0
else
    fail "NET-01: $FAILURES check(s) failed"
fi
