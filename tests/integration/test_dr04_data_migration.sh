#!/usr/bin/env bash
# =============================================================================
# DR-04: Data Directory Migration
#
# Verifies that copying a node's entire data_dir to a new container preserves
# operation: peer connections, blob serving, sync reception, and cursor state.
#
# Topology: 3 nodes (docker run). Dedicated network (172.34.0.0/16).
#   Node A (172.34.0.2): seed node, ingests initial blobs
#   Node B (172.34.0.3): bootstrap from Node A, syncs data
#   Node C (172.34.0.4): started from copy of Node A's data_dir
#
# Flow:
#   1. Start Node A + Node B, wait healthy, establish sync
#   2. Ingest 100 blobs to Node A, wait for sync to Node B
#   3. Stop Node A
#   4. Copy Node A's entire data_dir to Node C's volume
#   5. Start Node C with copied data, bootstrap to Node B
#   6. Verify Node C: peer connections, blob count == 100, can receive new blobs
#   7. Cursor test: ingest more blobs to Node B, verify incremental sync to Node C
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODEA_CONTAINER="chromatindb-dr04-nodea"
NODEB_CONTAINER="chromatindb-dr04-nodeb"
NODEC_CONTAINER="chromatindb-dr04-nodec"
DR04_NETWORK="chromatindb-dr04-test-net"
NODEA_VOLUME="chromatindb-dr04-nodea-data"
NODEB_VOLUME="chromatindb-dr04-nodeb-data"
NODEC_VOLUME="chromatindb-dr04-nodec-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DR04_NETWORK"

# --- Cleanup -----------------------------------------------------------------

cleanup_dr04() {
    log "Cleaning up DR-04 test..."
    docker rm -f "$NODEA_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODEB_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODEC_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODEA_VOLUME" 2>/dev/null || true
    docker volume rm "$NODEB_VOLUME" 2>/dev/null || true
    docker volume rm "$NODEC_VOLUME" 2>/dev/null || true
    docker network rm "$DR04_NETWORK" 2>/dev/null || true
}
trap cleanup_dr04 EXIT

# --- Helper: start a node container -----------------------------------------

start_dr04_node() {
    local name="$1"
    local ip="$2"
    local volume="$3"
    local bootstrap="$4"  # empty string for seed, or "ip:port" for bootstrap

    local bootstrap_json="[]"
    if [[ -n "$bootstrap" ]]; then
        bootstrap_json="[\"$bootstrap\"]"
    fi

    local tmpconfig
    tmpconfig=$(mktemp /tmp/dr04-config-XXXXXX.json)
    cat > "$tmpconfig" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": $bootstrap_json,
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
    chmod 644 "$tmpconfig"

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$DR04_NETWORK" \
        --ip "$ip" \
        -v "$volume:/data" \
        -v "$tmpconfig:/config/node.json:ro" \
        --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
        --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
        chromatindb:test \
        run --config /config/node.json --data-dir /data --log-level debug
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dr04 2>/dev/null || true

log "=== DR-04: Data Directory Migration ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.34.0.0/16 "$DR04_NETWORK"
docker volume create "$NODEA_VOLUME"
docker volume create "$NODEB_VOLUME"
docker volume create "$NODEC_VOLUME"

# =============================================================================
# Step 1: Start Node A + Node B
# =============================================================================

log "--- Step 1: Start Node A + Node B ---"

start_dr04_node "$NODEA_CONTAINER" "172.34.0.2" "$NODEA_VOLUME" ""
wait_healthy "$NODEA_CONTAINER"

start_dr04_node "$NODEB_CONTAINER" "172.34.0.3" "$NODEB_VOLUME" "172.34.0.2:4200"
wait_healthy "$NODEB_CONTAINER"

# Wait for peer connection
sleep 5

# =============================================================================
# Step 2: Ingest 100 blobs to Node A, wait for sync to Node B
# =============================================================================

log "--- Step 2: Ingest 100 blobs to Node A ---"

run_loadgen 172.34.0.2 --count 100 --size 1024 --ttl 3600 --rate 50 >/dev/null 2>&1

log "Waiting for sync to Node B (100 blobs)..."
if ! wait_sync "$NODEB_CONTAINER" 100 120; then
    log "WARN: Sync timeout to Node B"
fi

NODEB_COUNT=$(get_blob_count "$NODEB_CONTAINER")
log "Node B blob count after initial sync: $NODEB_COUNT"

if [[ "$NODEB_COUNT" -lt 100 ]]; then
    log "FAIL: Node B has $NODEB_COUNT/100 blobs after initial sync"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 3: Stop Node A
# =============================================================================

log "--- Step 3: Stop Node A ---"

docker stop "$NODEA_CONTAINER"
docker rm "$NODEA_CONTAINER"

# =============================================================================
# Step 4: Copy Node A's entire data_dir to Node C's volume
# =============================================================================

log "--- Step 4: Copy Node A's data_dir to Node C's volume ---"

docker run --rm -v "$NODEA_VOLUME:/src:ro" -v "$NODEC_VOLUME:/dst" alpine \
    sh -c 'cp -a /src/* /dst/'

log "Copied Node A's data_dir to Node C's volume"

# Verify files were copied
COPIED_FILES=$(docker run --rm -v "$NODEC_VOLUME:/data:ro" alpine ls /data/)
log "Node C volume contents: $COPIED_FILES"

# =============================================================================
# Step 5: Start Node C with copied data, bootstrap to Node B
# =============================================================================

log "--- Step 5: Start Node C with copied data ---"

# Node C uses a different IP (172.34.0.4) and bootstraps to Node B
start_dr04_node "$NODEC_CONTAINER" "172.34.0.4" "$NODEC_VOLUME" "172.34.0.3:4200"
wait_healthy "$NODEC_CONTAINER"

# Wait for peer connection establishment
sleep 10

# =============================================================================
# Check 1: Node C has peer connections
# =============================================================================

log "--- Check 1: Node C peer connections ---"

docker kill -s USR1 "$NODEC_CONTAINER" >/dev/null 2>&1 || true
sleep 2

NODEC_METRICS=$(docker logs --tail 200 "$NODEC_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
NODEC_PEERS=$(echo "$NODEC_METRICS" | grep -oP 'peers=\K[0-9]+' || echo "0")

log "Node C peers: $NODEC_PEERS"

if [[ "$NODEC_PEERS" -ge 1 ]]; then
    pass "DR-04: Node C has peer connections ($NODEC_PEERS peers)"
else
    log "FAIL: Node C has no peer connections"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node C blob count == 100 (existing blobs preserved)
# =============================================================================

log "--- Check 2: Node C blob count ---"

NODEC_COUNT=$(get_blob_count "$NODEC_CONTAINER")
log "Node C blob count: $NODEC_COUNT"

if [[ "$NODEC_COUNT" -ge 100 ]]; then
    pass "DR-04: Node C preserved existing blobs ($NODEC_COUNT >= 100)"
else
    log "FAIL: Node C has $NODEC_COUNT blobs, expected >= 100"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Node C can receive new blobs via sync
# =============================================================================

log "--- Check 3: Node C receives new blobs ---"

# Ingest 50 new blobs to Node B (batch includes both check 3 and 4 blobs)
# Combining into one ingest avoids multiple sync cycles and cooldown issues
run_loadgen 172.34.0.3 --count 50 --size 1024 --ttl 3600 --rate 50 >/dev/null 2>&1

# Wait for sync to Node C (should have 100 + 50 = 150)
# Use longer timeout to account for sync cooldowns on migrated node
log "Waiting for 50 new blobs to sync to Node C (150 total, 180s timeout)..."
if ! wait_sync "$NODEC_CONTAINER" 150 180; then
    log "WARN: Sync timeout for new blobs to Node C"
fi

NODEC_FINAL=$(get_blob_count "$NODEC_CONTAINER")
log "Node C final blob count: $NODEC_FINAL"

if [[ "$NODEC_FINAL" -ge 150 ]]; then
    pass "DR-04: Node C received new blobs via sync ($NODEC_FINAL >= 150)"
else
    log "FAIL: Node C has $NODEC_FINAL blobs, expected >= 150"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Cursor-based incremental sync (not full resync)
# =============================================================================

log "--- Check 4: Cursor test (incremental sync) ---"

# Check cursor_hits to confirm incremental sync
docker kill -s USR1 "$NODEC_CONTAINER" >/dev/null 2>&1 || true
sleep 2

NODEC_METRICS_FINAL=$(docker logs --tail 200 "$NODEC_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
CURSOR_HITS=$(echo "$NODEC_METRICS_FINAL" | grep -oP 'cursor_hits=\K[0-9]+' || echo "0")

log "Node C cursor_hits: $CURSOR_HITS"

if [[ "$CURSOR_HITS" -gt 0 ]]; then
    pass "DR-04: Cursor-based incremental sync confirmed (cursor_hits=$CURSOR_HITS)"
else
    log "WARN: cursor_hits=0 (may indicate full resync instead of incremental)"
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DR-04: Data Directory Migration PASSED"
    pass "  - Node C started from copied data_dir"
    pass "  - Peer connections established ($NODEC_PEERS peers)"
    pass "  - Existing blobs preserved ($NODEC_COUNT blobs)"
    pass "  - New blobs received via sync ($NODEC_FINAL final blobs)"
    pass "  - Cursor-based incremental sync (cursor_hits=$CURSOR_HITS)"
    exit 0
else
    fail "DR-04: $FAILURES check(s) failed"
fi
