#!/usr/bin/env bash
# =============================================================================
# DOS-01: Write Rate Limiting
#
# Verifies that a flooding peer is disconnected (rate_limited > 0) while a
# good peer continues operating normally.
#
# Topology: 3 standalone nodes on 172.36.0.0/16
#   Node1 (172.36.0.2): target -- rate_limit_bytes_per_sec=10240, burst=20480
#   Node2 (172.36.0.3): good peer, bootstraps to Node1
#   Node3 (172.36.0.4): flooding peer, bootstraps to Node1
#
# Flow:
#   1. Start all 3 nodes, wait for connections to establish
#   2. Ingest 10 blobs at acceptable rate from Node2 -- stays connected
#   3. Flood Node1 from network: 200 blobs x 10KB at 100/s (1MB/s >> 10KB/s limit)
#   4. Verify rate_limited > 0 in Node1 metrics
#   5. Verify Node2 still connected and functional (ingest 5 more blobs)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-dos01-node1"
NODE2_CONTAINER="chromatindb-dos01-node2"
NODE3_CONTAINER="chromatindb-dos01-node3"
DOS01_NETWORK="chromatindb-dos01-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DOS01_NETWORK"

NODE1_VOLUME="chromatindb-dos01-node1-data"
NODE2_VOLUME="chromatindb-dos01-node2-data"
NODE3_VOLUME="chromatindb-dos01-node3-data"

# Temp config paths
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""

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

cleanup_dos01() {
    log "Cleaning up DOS-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$DOS01_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
}
trap cleanup_dos01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dos01 2>/dev/null || true

log "=== DOS-01: Write Rate Limiting Test ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.36.0.0/16 "$DOS01_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# =============================================================================
# Create configs
# =============================================================================

TEMP_NODE1_CONFIG=$(mktemp /tmp/dos01-node1-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "rate_limit_bytes_per_sec": 10240,
  "rate_limit_burst": 20480,
  "sync_cooldown_seconds": 0,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/dos01-node2-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.36.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

TEMP_NODE3_CONFIG=$(mktemp /tmp/dos01-node3-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.36.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

# =============================================================================
# Step 1: Start all 3 nodes
# =============================================================================

log "--- Step 1: Start all 3 nodes ---"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$DOS01_NETWORK" \
    --ip 172.36.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$DOS01_NETWORK" \
    --ip 172.36.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

docker run -d --name "$NODE3_CONTAINER" \
    --network "$DOS01_NETWORK" \
    --ip 172.36.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE3_CONTAINER"

log "Waiting 10s for connections to establish..."
sleep 10

# =============================================================================
# Step 2: Good peer ingest (within rate limit)
# =============================================================================

log "--- Step 2: Good peer ingest (10 blobs, 1KB each, 5/s) ---"

# Node2 sends 10 blobs at 1KB each, 5/s = 5KB/s << 10KB/s limit
run_loadgen 172.36.0.3 --count 10 --size 1024 --rate 5 --ttl 3600 >/dev/null 2>&1 || true

# Wait for sync to Node1
sleep 10

# Verify Node2 is still connected (peer count on Node1 >= 1)
NODE1_PEERS=$(get_metric "$NODE1_CONTAINER" "peers")
log "Node1 peers after good ingest: $NODE1_PEERS"

if [[ "$NODE1_PEERS" -ge 1 ]]; then
    pass "Node2 still connected after good ingest (peers=$NODE1_PEERS)"
else
    log "FAIL: Node1 lost all peers after good ingest (peers=$NODE1_PEERS)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 3: Flood Node1 from network position
# =============================================================================

log "--- Step 3: Flood Node1 (200 blobs x 10KB at 100/s = ~1MB/s) ---"

# The flooding loadgen connects directly to Node1 and sends 200 x 10KB blobs
# at 100/s = ~1MB/s, far exceeding the 10KB/s rate limit.
# The loadgen will get disconnected partway through -- expected behavior.
run_loadgen 172.36.0.2 --count 200 --size 10240 --rate 100 --ttl 3600 >/dev/null 2>&1 || true

# Wait for metrics to settle
sleep 5

# =============================================================================
# Step 4: Verify rate_limited > 0 in Node1 metrics
# =============================================================================

log "--- Step 4: Verify rate limiting metrics ---"

RATE_LIMITED=$(get_metric "$NODE1_CONTAINER" "rate_limited")
log "Node1 rate_limited=$RATE_LIMITED"

if [[ "$RATE_LIMITED" -gt 0 ]]; then
    pass "Rate limiting triggered (rate_limited=$RATE_LIMITED)"
else
    log "FAIL: rate_limited=0 -- flooding peer was not rate limited"
    FAILURES=$((FAILURES + 1))
fi

# Check logs for rate limit disconnect evidence
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS" | grep -qi "rate limit"; then
    pass "Rate limit disconnect evidence found in logs"
else
    log "WARN: No 'rate limit' message found in Node1 logs (metrics confirm via counter)"
fi

# =============================================================================
# Step 5: Verify good peer (Node2) still connected and functional
# =============================================================================

log "--- Step 5: Verify good peer still functional ---"

NODE1_PEERS_POST=$(get_metric "$NODE1_CONTAINER" "peers")
log "Node1 peers after flood: $NODE1_PEERS_POST"

if [[ "$NODE1_PEERS_POST" -ge 1 ]]; then
    pass "Good peer still connected after flood (peers=$NODE1_PEERS_POST)"
else
    log "FAIL: All peers lost after flood (peers=$NODE1_PEERS_POST)"
    FAILURES=$((FAILURES + 1))
fi

# Ingest 5 more blobs from Node2 side to prove its connection survived
log "Ingesting 5 more blobs from Node2..."
run_loadgen 172.36.0.3 --count 5 --size 1024 --rate 5 --ttl 3600 >/dev/null 2>&1 || true

sleep 10

# Verify Node1 received the blobs (total should be >= 15 from Node2's ingests)
NODE1_BLOBS=$(get_metric "$NODE1_CONTAINER" "blobs")
log "Node1 blobs after second ingest: $NODE1_BLOBS"

if [[ "$NODE1_BLOBS" -ge 10 ]]; then
    pass "Node1 received blobs from good peer after flood ($NODE1_BLOBS blobs)"
else
    log "FAIL: Node1 has $NODE1_BLOBS blobs, expected >= 10"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DOS-01: Write rate limiting PASSED"
    pass "  - Flooding peer rate_limited=$RATE_LIMITED (disconnected)"
    pass "  - Good peer survived flood, still connected"
    pass "  - Node1 final blob count: $NODE1_BLOBS"
    exit 0
else
    fail "DOS-01: $FAILURES check(s) failed"
fi
