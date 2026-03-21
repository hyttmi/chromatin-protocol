#!/usr/bin/env bash
# =============================================================================
# NET-02: Split-Brain Independent Writes Merge
#
# Verifies that a 4-node cluster partitioned into two halves with independent
# writes to each half merges to the union of all blobs after healing.
#
# Topology: 4 nodes using two networks for split-brain simulation
#   NET_A (172.30.0.0/16): group A connectivity
#   NET_B (172.31.0.0/16): group B internal connectivity
#
#   Node1 (NET_A: 172.30.0.2): group A seed
#   Node2 (NET_A: 172.30.0.3): group A, bootstrap from node1
#   Node3 (NET_A: 172.30.0.4, NET_B: 172.31.0.4): bridge between groups
#   Node4 (NET_A: 172.30.0.5, NET_B: 172.31.0.5): group B, bootstrap from node3 on NET_B
#
# Partition: disconnect node3/node4 from NET_A -> group B communicates on NET_B
# Heal: reconnect node3/node4 to NET_A -> full mesh restored
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1="chromatindb-test-node1"
NODE2="chromatindb-test-node2"
NODE3="chromatindb-test-node3"
NODE4="chromatindb-test-node4"

NET_A="chromatindb-split-test-net"
NET_B="chromatindb-split-b-net"
IMAGE="chromatindb:test"

TEMP_CONFIGS=()
VOL1="chromatindb-split-node1-data"
VOL2="chromatindb-split-node2-data"
VOL3="chromatindb-split-node3-data"
VOL4="chromatindb-split-node4-data"

# Robust blob count getter
get_blob_count_reliable() {
    local container="$1"
    local retries="${2:-3}"
    local count=0
    for attempt in $(seq 1 "$retries"); do
        docker kill -s USR1 "$container" >/dev/null 2>&1 || true
        sleep 3
        count=$(docker logs "$container" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'blobs=\K[0-9]+' || echo "0")
        if [[ "$count" -gt 0 ]]; then break; fi
    done
    echo "$count"
}

# --- Cleanup -----------------------------------------------------------------

cleanup_net02() {
    log "Cleaning up NET-02 test..."
    docker rm -f "$NODE1" "$NODE2" "$NODE3" "$NODE4" 2>/dev/null || true
    docker volume rm "$VOL1" "$VOL2" "$VOL3" "$VOL4" 2>/dev/null || true
    docker network rm "$NET_A" "$NET_B" 2>/dev/null || true
    for cfg in "${TEMP_CONFIGS[@]}"; do rm -f "$cfg" 2>/dev/null || true; done
}
trap cleanup_net02 EXIT

# --- Helper: create config file ----------------------------------------------

make_config() {
    local bootstrap="$1"
    local cfg
    cfg=$(mktemp /tmp/chromatindb-net02-XXXXXX.json)
    TEMP_CONFIGS+=("$cfg")
    cat > "$cfg" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": $bootstrap,
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
    chmod 644 "$cfg"
    echo "$cfg"
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_net02 2>/dev/null || true

log "=== NET-02: Split-Brain Test ==="

FAILURES=0

# =============================================================================
# 1. Create networks and start nodes
# =============================================================================

log "--- Setting up 4-node topology ---"

docker network create --driver bridge --subnet 172.30.0.0/16 "$NET_A"
docker network create --driver bridge --subnet 172.31.0.0/16 "$NET_B"
docker volume create "$VOL1"
docker volume create "$VOL2"
docker volume create "$VOL3"
docker volume create "$VOL4"

# Node1: seed on NET_A only
# Node2: bootstrap from node1 on NET_A
# Node3: bootstrap from node1 on NET_A AND from node4 on NET_B
# Node4: bootstrap from node3 on NET_B
CFG1=$(make_config '[]')
CFG2=$(make_config '["172.30.0.2:4200"]')
CFG3=$(make_config '["172.30.0.2:4200", "172.31.0.5:4200"]')
CFG4=$(make_config '["172.31.0.4:4200"]')

# Start node1 and node2 on NET_A only
docker run -d --name "$NODE1" \
    --network "$NET_A" --ip 172.30.0.2 \
    -v "$VOL1:/data" -v "$CFG1:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    "$IMAGE" run --config /config/node.json --data-dir /data --log-level debug

docker run -d --name "$NODE2" \
    --network "$NET_A" --ip 172.30.0.3 \
    -v "$VOL2:/data" -v "$CFG2:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    "$IMAGE" run --config /config/node.json --data-dir /data --log-level debug

# Start node3 on NET_A first, then add to NET_B
docker run -d --name "$NODE3" \
    --network "$NET_A" --ip 172.30.0.4 \
    -v "$VOL3:/data" -v "$CFG3:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    "$IMAGE" run --config /config/node.json --data-dir /data --log-level debug
docker network connect --ip 172.31.0.4 "$NET_B" "$NODE3"

# Start node4 on NET_B first (its bootstrap is on NET_B), then add to NET_A
docker run -d --name "$NODE4" \
    --network "$NET_B" --ip 172.31.0.5 \
    -v "$VOL4:/data" -v "$CFG4:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    "$IMAGE" run --config /config/node.json --data-dir /data --log-level debug
docker network connect --ip 172.30.0.5 "$NET_A" "$NODE4"

wait_healthy "$NODE1"
wait_healthy "$NODE2"
wait_healthy "$NODE3"
wait_healthy "$NODE4"

log "Waiting 15s for mesh to form..."
sleep 15

# =============================================================================
# 2. Verify full connectivity: ingest 50 blobs, sync to node4
# =============================================================================

log "--- Verifying full connectivity ---"

NETWORK="$NET_A"
run_loadgen 172.30.0.2 --count 50 --size 256 --rate 50 --ttl 3600

if ! wait_sync "$NODE4" 50 120; then
    log "FAIL: Initial sync failed (node4 did not reach 50)"
    FAILURES=$((FAILURES + 1))
else
    pass "Initial sync: all 4 nodes have >= 50 blobs"
fi

# =============================================================================
# 3. Create partition: disconnect node3/node4 from NET_A
#    Group A = {node1, node2} on NET_A
#    Group B = {node3, node4} on NET_B
# =============================================================================

log "--- Creating partition ---"

PRE_PART_1=$(get_blob_count_reliable "$NODE1")
PRE_PART_2=$(get_blob_count_reliable "$NODE2")
PRE_PART_3=$(get_blob_count_reliable "$NODE3")
PRE_PART_4=$(get_blob_count_reliable "$NODE4")
log "Pre-partition counts: node1=$PRE_PART_1 node2=$PRE_PART_2 node3=$PRE_PART_3 node4=$PRE_PART_4"

docker network disconnect "$NET_A" "$NODE3"
docker network disconnect "$NET_A" "$NODE4"

log "Partition created: A={node1,node2} on NET_A | B={node3,node4} on NET_B"

# Wait for connections to stabilize
sleep 10

# =============================================================================
# 4. Write to Group A: 30 blobs to node1
# =============================================================================

log "--- Writing 30 blobs to Group A ---"

NETWORK="$NET_A"
run_loadgen 172.30.0.2 --count 30 --size 256 --rate 50 --ttl 3600

if ! wait_sync "$NODE2" 80 60; then
    NODE2_COUNT=$(get_blob_count "$NODE2")
    log "FAIL: Group A sync failed (node2=$NODE2_COUNT/80)"
    FAILURES=$((FAILURES + 1))
else
    pass "Group A: node2 synced to >= 80 blobs"
fi

# =============================================================================
# 5. Write to Group B: 20 blobs to node3 (via NET_B)
# =============================================================================

log "--- Writing 20 blobs to Group B ---"

NETWORK="$NET_B"
run_loadgen 172.31.0.4 --count 20 --size 256 --rate 50 --ttl 3600

if ! wait_sync "$NODE4" 70 60; then
    NODE4_COUNT=$(get_blob_count "$NODE4")
    log "FAIL: Group B sync failed (node4=$NODE4_COUNT/70)"
    FAILURES=$((FAILURES + 1))
else
    pass "Group B: node4 synced to >= 70 blobs"
fi

# =============================================================================
# 6. Verify partition isolation
# =============================================================================

log "--- Verifying partition isolation ---"

NODE1_ISO=$(get_blob_count_reliable "$NODE1")
NODE2_ISO=$(get_blob_count_reliable "$NODE2")
NODE3_ISO=$(get_blob_count_reliable "$NODE3")
NODE4_ISO=$(get_blob_count_reliable "$NODE4")

log "Isolation: A={node1=$NODE1_ISO, node2=$NODE2_ISO} B={node3=$NODE3_ISO, node4=$NODE4_ISO}"

ISOLATION_OK=true
for pair in "1:$PRE_PART_1:$NODE1_ISO" "2:$PRE_PART_2:$NODE2_ISO"; do
    IFS=: read -r num pre post <<< "$pair"
    DELTA=$((post - pre))
    if [[ "$DELTA" -ge 50 ]]; then
        log "FAIL: Node$num in A leaked: gained $DELTA blobs"
        ISOLATION_OK=false
    fi
done
for pair in "3:$PRE_PART_3:$NODE3_ISO" "4:$PRE_PART_4:$NODE4_ISO"; do
    IFS=: read -r num pre post <<< "$pair"
    DELTA=$((post - pre))
    if [[ "$DELTA" -ge 50 ]]; then
        log "FAIL: Node$num in B leaked: gained $DELTA blobs"
        ISOLATION_OK=false
    fi
done

if [[ "$ISOLATION_OK" == true ]]; then
    pass "Partition isolation verified"
else
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# 7. Heal partition: reconnect node3/node4 to NET_A
# =============================================================================

log "--- Healing partition ---"

docker network connect --ip 172.30.0.4 "$NET_A" "$NODE3"
docker network connect --ip 172.30.0.5 "$NET_A" "$NODE4"

log "Partition healed: all nodes on both networks"

# Auto-reconnect will fire within MAX_BACKOFF_SEC (60s).
# The existing reconnect loops for bootstrap peers (172.30.0.2 for node3)
# have been running with backoff during partition. Once NET_A connectivity
# is restored, the next attempt will succeed.
log "Waiting for auto-reconnect and convergence..."

NETWORK="$NET_A"

# =============================================================================
# 8. Wait for convergence: all 4 nodes >= 100 blobs (50+30+20)
# =============================================================================

log "--- Waiting for convergence ---"

# Allow 180s: up to 60s for reconnect backoff + sync time
for node in "$NODE1" "$NODE2" "$NODE3" "$NODE4"; do
    if ! wait_sync "$node" 100 180; then
        COUNT=$(get_blob_count "$node")
        log "FAIL: $node did not converge ($COUNT/100)"
        FAILURES=$((FAILURES + 1))
    fi
done

log "--- Final convergence check ---"

for node in "$NODE1" "$NODE2" "$NODE3" "$NODE4"; do
    COUNT=$(get_blob_count "$node")
    log "Final count $node: $COUNT"
    if [[ "$COUNT" -ge 100 ]]; then
        pass "$node converged ($COUNT blobs)"
    else
        log "FAIL: $node has $COUNT blobs, expected >= 100"
        FAILURES=$((FAILURES + 1))
    fi
done

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "NET-02: Split-brain merge PASSED"
    pass "  - 4-node cluster partitioned into {1,2} | {3,4}"
    pass "  - Group A: 30 independent blobs written"
    pass "  - Group B: 20 independent blobs written"
    pass "  - Partition isolation verified"
    pass "  - Healed partition: all 4 nodes converged to >= 100 blobs (union)"
    exit 0
else
    fail "NET-02: $FAILURES check(s) failed"
fi
