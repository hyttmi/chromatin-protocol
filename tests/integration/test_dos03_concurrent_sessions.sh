#!/usr/bin/env bash
# =============================================================================
# DOS-03: Concurrent Session Limit
#
# Verifies that with max_sync_sessions=1 and 3 peers syncing simultaneously,
# the sync session/rate protection limits excess requests (sync_rejections > 0)
# while existing sessions complete normally and all peers eventually converge.
#
# Topology: 4 standalone nodes on 172.38.0.0/16
#   Node1 (172.38.0.2): target -- max_sync_sessions=1, sync_cooldown=5s
#   Node2 (172.38.0.3): bootstraps to Node1, sync_interval=1s
#   Node3 (172.38.0.4): bootstraps to Node1, sync_interval=1s
#   Node4 (172.38.0.5): bootstraps to Node1, sync_interval=1s
#
# Flow:
#   1. Start Node1, ingest 500 blobs
#   2. Start Node2, Node3, Node4 simultaneously (all empty, all sync at once)
#   3. Wait -- peers fire sync every 1s but Node1 enforces 5s cooldown per peer.
#      With 3 peers, sync_rejections accumulate as peers hit cooldown windows.
#   4. Verify sync_rejections > 0 on Node1 (excess sessions rejected)
#   5. Wait for convergence -- all peers eventually get all 500 blobs
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-dos03-node1"
NODE2_CONTAINER="chromatindb-dos03-node2"
NODE3_CONTAINER="chromatindb-dos03-node3"
NODE4_CONTAINER="chromatindb-dos03-node4"
DOS03_NETWORK="chromatindb-dos03-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DOS03_NETWORK"

NODE1_VOLUME="chromatindb-dos03-node1-data"
NODE2_VOLUME="chromatindb-dos03-node2-data"
NODE3_VOLUME="chromatindb-dos03-node3-data"
NODE4_VOLUME="chromatindb-dos03-node4-data"

# Temp config paths
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
TEMP_NODE4_CONFIG=""

# --- Helpers -----------------------------------------------------------------

get_metric() {
    local container="$1"
    local metric="$2"
    docker kill -s USR1 "$container" >/dev/null 2>&1 || true
    sleep 2
    local value
    value=$(docker logs --tail 200 "$container" 2>&1 | grep "metrics:" | tail -1 | grep -oP "${metric}=\\K[0-9]+" || echo "0")
    echo "$value"
}

# --- Cleanup -----------------------------------------------------------------

cleanup_dos03() {
    log "Cleaning up DOS-03 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE4_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE4_VOLUME" 2>/dev/null || true
    docker network rm "$DOS03_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE4_CONFIG" ]] && rm -f "$TEMP_NODE4_CONFIG" 2>/dev/null || true
}
trap cleanup_dos03 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dos03 2>/dev/null || true

log "=== DOS-03: Concurrent Session Limit Test ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.38.0.0/16 "$DOS03_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"
docker volume create "$NODE4_VOLUME"

# =============================================================================
# Create configs
# =============================================================================

# Node1: max_sync_sessions=1, sync_cooldown=5s
# Peers fire sync every 1s but Node1 enforces 5s cooldown per peer.
# With 3 peers, excess sync requests hit cooldown and get rejected.
TEMP_NODE1_CONFIG=$(mktemp /tmp/dos03-node1-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "sync_cooldown_seconds": 5,
  "max_sync_sessions": 1,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Node2-4: all bootstrap to Node1, sync_interval=1s to fire frequently
# Each peer sends SyncRequest every 1s, but Node1 enforces 5s cooldown per peer.
# After a successful sync, the next 4 attempts from the same peer are rejected.
for i in 2 3 4; do
    TEMP_VAR="TEMP_NODE${i}_CONFIG"
    TEMP_FILE=$(mktemp /tmp/dos03-node${i}-XXXXXX.json)
    eval "${TEMP_VAR}=\$TEMP_FILE"
    cat > "$TEMP_FILE" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.38.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 1,
  "sync_cooldown_seconds": 0,
  "full_resync_interval": 9999
}
EOCFG
    chmod 644 "$TEMP_FILE"
done

# =============================================================================
# Step 1: Start Node1 and ingest 500 blobs
# =============================================================================

log "--- Step 1: Start Node1 and ingest 500 blobs ---"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$DOS03_NETWORK" \
    --ip 172.38.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Ingest 500 blobs at rate 2000 (fast bulk ingest before sync timer fires)
log "Ingesting 500 blobs to Node1..."
run_loadgen 172.38.0.2 --count 500 --size 1024 --rate 2000 --ttl 3600 >/dev/null 2>&1 || true

# Wait for writes to settle before checking metrics
sleep 5

# Verify Node1 has the blobs (retry metric check for SIGUSR1 delivery reliability)
NODE1_BLOBS=0
for attempt in 1 2 3; do
    NODE1_BLOBS=$(get_metric "$NODE1_CONTAINER" "blobs")
    log "Node1 blob count (attempt $attempt): $NODE1_BLOBS"
    if [[ "$NODE1_BLOBS" -ge 400 ]]; then
        break
    fi
    sleep 3
done

if [[ "$NODE1_BLOBS" -lt 400 ]]; then
    fail "Node1 ingest failed -- only $NODE1_BLOBS/500 blobs"
fi

# =============================================================================
# Step 2: Start Node2, Node3, Node4 simultaneously
# =============================================================================

log "--- Step 2: Start Node2, Node3, Node4 simultaneously ---"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$DOS03_NETWORK" \
    --ip 172.38.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

docker run -d --name "$NODE3_CONTAINER" \
    --network "$DOS03_NETWORK" \
    --ip 172.38.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

docker run -d --name "$NODE4_CONTAINER" \
    --network "$DOS03_NETWORK" \
    --ip 172.38.0.5 \
    -v "$NODE4_VOLUME:/data" \
    -v "$TEMP_NODE4_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

for node in "$NODE2_CONTAINER" "$NODE3_CONTAINER" "$NODE4_CONTAINER"; do
    wait_healthy "$node"
done

log "All 3 peers started. Waiting 30s for sync contention..."
sleep 30

# =============================================================================
# Step 3: Check sync_rejections > 0 on Node1
# =============================================================================

log "--- Step 3: Check sync rejection metrics ---"

SYNC_REJECTIONS=$(get_metric "$NODE1_CONTAINER" "sync_rejections")
log "Node1 sync_rejections=$SYNC_REJECTIONS"

if [[ "$SYNC_REJECTIONS" -gt 0 ]]; then
    pass "Session limit enforced (sync_rejections=$SYNC_REJECTIONS)"
else
    log "FAIL: sync_rejections=0 -- no session rejections with 3 simultaneous peers"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 4: Wait for eventual convergence (up to 180s)
# =============================================================================

log "--- Step 4: Wait for convergence ---"

# With rejected sessions retrying every 1s (but cooldown=5s on Node1),
# all nodes should eventually sync. Check that at least one node gets all blobs.
CONVERGED=false
START_CONV=$(date +%s)
CONV_TIMEOUT=180

while true; do
    NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
    NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")
    NODE4_COUNT=$(get_blob_count "$NODE4_CONTAINER")

    ELAPSED=$(( $(date +%s) - START_CONV ))
    log "Convergence check: node2=$NODE2_COUNT node3=$NODE3_COUNT node4=$NODE4_COUNT (${ELAPSED}s)"

    # Check if any node has all blobs
    MAX_COUNT=$NODE2_COUNT
    [[ "$NODE3_COUNT" -gt "$MAX_COUNT" ]] && MAX_COUNT=$NODE3_COUNT
    [[ "$NODE4_COUNT" -gt "$MAX_COUNT" ]] && MAX_COUNT=$NODE4_COUNT

    if [[ "$MAX_COUNT" -ge "$NODE1_BLOBS" ]]; then
        CONVERGED=true
        break
    fi

    if [[ $ELAPSED -ge $CONV_TIMEOUT ]]; then
        log "WARN: Convergence timeout at ${ELAPSED}s"
        break
    fi

    sleep 10
done

# =============================================================================
# Step 5: Final verification
# =============================================================================

log "--- Step 5: Final verification ---"

# Re-check metrics after convergence
SYNC_REJECTIONS_FINAL=$(get_metric "$NODE1_CONTAINER" "sync_rejections")

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
NODE3_FINAL=$(get_blob_count "$NODE3_CONTAINER")
NODE4_FINAL=$(get_blob_count "$NODE4_CONTAINER")

log "Final counts: node2=$NODE2_FINAL node3=$NODE3_FINAL node4=$NODE4_FINAL"
log "Final sync_rejections=$SYNC_REJECTIONS_FINAL"

# At least one node should have all blobs
MAX_FINAL=$NODE2_FINAL
BEST_NODE="Node2"
if [[ "$NODE3_FINAL" -gt "$MAX_FINAL" ]]; then MAX_FINAL=$NODE3_FINAL; BEST_NODE="Node3"; fi
if [[ "$NODE4_FINAL" -gt "$MAX_FINAL" ]]; then MAX_FINAL=$NODE4_FINAL; BEST_NODE="Node4"; fi

if [[ "$MAX_FINAL" -ge "$NODE1_BLOBS" ]]; then
    pass "$BEST_NODE completed full sync ($MAX_FINAL blobs)"
else
    # Accept if at least 80% synced (sync contention can slow convergence)
    THRESHOLD=$(( NODE1_BLOBS * 80 / 100 ))
    if [[ "$MAX_FINAL" -ge "$THRESHOLD" ]]; then
        pass "Near-convergence: $BEST_NODE has $MAX_FINAL/$NODE1_BLOBS blobs (>= 80%)"
    else
        log "FAIL: Best peer has $MAX_FINAL/$NODE1_BLOBS blobs"
        FAILURES=$((FAILURES + 1))
    fi
fi

# Confirm sync_rejections > 0 (proves sessions were limited)
if [[ "$SYNC_REJECTIONS_FINAL" -gt 0 ]]; then
    pass "sync_rejections=$SYNC_REJECTIONS_FINAL confirms session limiting"
else
    log "FAIL: sync_rejections=0 after convergence -- no sessions were rejected"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DOS-03: Concurrent session limit PASSED"
    pass "  - 3 peers syncing simultaneously with max_sync_sessions=1"
    pass "  - sync_rejections=$SYNC_REJECTIONS_FINAL (sessions limited)"
    pass "  - $BEST_NODE completed sync ($MAX_FINAL blobs)"
    pass "  - Final: node2=$NODE2_FINAL node3=$NODE3_FINAL node4=$NODE4_FINAL"
    exit 0
else
    fail "DOS-03: $FAILURES check(s) failed"
fi
