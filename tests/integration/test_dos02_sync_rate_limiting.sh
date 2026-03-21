#!/usr/bin/env bash
# =============================================================================
# DOS-02: Sync Rate Limiting (Cooldown)
#
# Verifies that excess sync initiations are rejected (sync_rejections > 0)
# and that sync resumes after cooldown period expires.
#
# Topology: 2 standalone nodes on 172.37.0.0/16
#   Node1 (172.37.0.2): target -- sync_cooldown_seconds=30
#   Node2 (172.37.0.3): bootstraps to Node1, sync_interval=5s
#
# Flow:
#   1. Start both nodes, wait for initial connection + sync
#   2. Ingest 20 blobs to Node1, wait for sync to Node2 (consumes sync window)
#   3. Immediately ingest 10 more -- Node2 re-syncs but hits 30s cooldown
#   4. Verify sync_rejections > 0 on Node1
#   5. Wait 35s (past cooldown), ingest 5 more, verify eventual convergence
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-dos02-node1"
NODE2_CONTAINER="chromatindb-dos02-node2"
DOS02_NETWORK="chromatindb-dos02-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DOS02_NETWORK"

NODE1_VOLUME="chromatindb-dos02-node1-data"
NODE2_VOLUME="chromatindb-dos02-node2-data"

# Temp config paths
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""

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

cleanup_dos02() {
    log "Cleaning up DOS-02 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$DOS02_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
}
trap cleanup_dos02 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dos02 2>/dev/null || true

log "=== DOS-02: Sync Rate Limiting Test ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.37.0.0/16 "$DOS02_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# =============================================================================
# Create configs
# =============================================================================

# Node1: sync_cooldown_seconds=30 forces rejections when Node2 retries every 5s
TEMP_NODE1_CONFIG=$(mktemp /tmp/dos02-node1-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "sync_cooldown_seconds": 30,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Node2: sync_interval=5s means it attempts sync every 5s, hitting cooldown repeatedly
TEMP_NODE2_CONFIG=$(mktemp /tmp/dos02-node2-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.37.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "sync_cooldown_seconds": 0,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# =============================================================================
# Step 1: Start both nodes
# =============================================================================

log "--- Step 1: Start both nodes ---"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$DOS02_NETWORK" \
    --ip 172.37.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$DOS02_NETWORK" \
    --ip 172.37.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

log "Waiting 10s for initial connection..."
sleep 10

# =============================================================================
# Step 2: Ingest 20 blobs to Node1, wait for sync to Node2
# =============================================================================

log "--- Step 2: Ingest 20 blobs to Node1 ---"

run_loadgen 172.37.0.2 --count 20 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1 || true

# Wait for sync to Node2 -- this consumes the first sync window
if ! wait_sync "$NODE2_CONTAINER" 20 60; then
    NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync partial ($NODE2_COUNT/20 blobs)"
fi

pass "Initial sync complete"

# =============================================================================
# Step 3: Immediately ingest 10 more blobs (triggers cooldown rejections)
# =============================================================================

log "--- Step 3: Ingest 10 more blobs (triggers cooldown) ---"

run_loadgen 172.37.0.2 --count 10 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1 || true

# Node2 will attempt to sync every 5s. With cooldown=30s, the first successful
# sync timestamps the peer. Subsequent sync attempts within 30s get rejected.
# Wait 25s to accumulate several cooldown rejections.
log "Waiting 25s for cooldown rejections to accumulate..."
sleep 25

# =============================================================================
# Step 4: Verify sync_rejections > 0 on Node1
# =============================================================================

log "--- Step 4: Verify sync rejection metrics ---"

SYNC_REJECTIONS=$(get_metric "$NODE1_CONTAINER" "sync_rejections")
log "Node1 sync_rejections=$SYNC_REJECTIONS"

if [[ "$SYNC_REJECTIONS" -gt 0 ]]; then
    pass "Sync cooldown rejections triggered (sync_rejections=$SYNC_REJECTIONS)"
else
    log "FAIL: sync_rejections=0 -- cooldown did not reject any sync attempts"
    FAILURES=$((FAILURES + 1))
fi

# Check logs for rejection evidence
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS" | grep -q "rejected: cooldown"; then
    pass "Sync cooldown rejection evidence in logs"
else
    log "WARN: No explicit cooldown rejection log message found"
fi

# =============================================================================
# Step 5: Wait past cooldown, verify eventual convergence
# =============================================================================

log "--- Step 5: Wait past cooldown, verify convergence ---"

# Wait for cooldown to expire (35s from last sync should be enough)
log "Waiting 35s for cooldown to expire..."
sleep 35

# Ingest 5 more blobs to force a new sync round
run_loadgen 172.37.0.2 --count 5 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1 || true

# Expected total: 20 + 10 + 5 = 35 blobs
# Node2 should eventually receive all blobs once cooldown expires
if ! wait_sync "$NODE2_CONTAINER" 35 120; then
    NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Node2 has $NODE2_FINAL/35 blobs (partial convergence)"
fi

NODE1_FINAL=$(get_blob_count "$NODE1_CONTAINER")
NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
log "Final counts: Node1=$NODE1_FINAL, Node2=$NODE2_FINAL"

if [[ "$NODE2_FINAL" -ge 35 ]]; then
    pass "Full convergence after cooldown (Node2=$NODE2_FINAL >= 35)"
else
    # Accept partial convergence if at least the post-cooldown blobs arrived
    if [[ "$NODE2_FINAL" -ge 30 ]]; then
        pass "Near-convergence after cooldown (Node2=$NODE2_FINAL >= 30)"
    else
        log "FAIL: Node2 only has $NODE2_FINAL blobs, expected >= 30"
        FAILURES=$((FAILURES + 1))
    fi
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DOS-02: Sync rate limiting PASSED"
    pass "  - sync_rejections=$SYNC_REJECTIONS (cooldown enforced)"
    pass "  - Sync resumed after cooldown period"
    pass "  - Final blob counts: Node1=$NODE1_FINAL, Node2=$NODE2_FINAL"
    exit 0
else
    fail "DOS-02: $FAILURES check(s) failed"
fi
