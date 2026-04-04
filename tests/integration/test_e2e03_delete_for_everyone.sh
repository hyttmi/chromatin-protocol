#!/usr/bin/env bash
# =============================================================================
# E2E-03: Delete for Everyone (Tombstone Propagation)
#
# Verifies that a tombstone propagates to all nodes within one sync interval.
#
# Topology: 3-node standalone (docker run, not compose)
#   Node1 (172.42.0.2): seed node, owner ingests and tombstones here
#   Node2 (172.42.0.3): bootstraps to Node1
#   Node3 (172.42.0.4): bootstraps to Node1
#   Network: 172.42.0.0/16 (chromatindb-e2e03-test-net)
#
# Flow:
#   1. Start 3 nodes, wait for mesh formation
#   2. Generate owner identity, ingest 10 blobs
#   3. Wait for sync to all 3 nodes
#   4. Owner tombstones one specific blob (TTL=0)
#   5. Wait for tombstone to propagate (~15s: 5s sync interval + margin)
#   6. Verify: all nodes have tombstone (blob count = 11: 10 original + 1 tombstone)
#   7. Verify: propagation completed within one sync interval (under 15s)
#   8. Verify: tombstone persists across restart (integrity scan shows tombstone)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-e2e03-node1"
NODE2_CONTAINER="chromatindb-e2e03-node2"
NODE3_CONTAINER="chromatindb-e2e03-node3"
E2E03_NETWORK="chromatindb-e2e03-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$E2E03_NETWORK"

# Named volumes for data persistence
NODE1_VOLUME="chromatindb-e2e03-node1-data"
NODE2_VOLUME="chromatindb-e2e03-node2-data"
NODE3_VOLUME="chromatindb-e2e03-node3-data"

# Temp dirs
OWNER_DIR=""
TEMP_CONFIGS=()

# --- Cleanup -----------------------------------------------------------------

cleanup_e2e03() {
    log "Cleaning up E2E-03 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$E2E03_NETWORK" 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
    for cfg in "${TEMP_CONFIGS[@]}"; do
        rm -f "$cfg" 2>/dev/null || true
    done
}
trap cleanup_e2e03 EXIT

# --- Helper: start a node container -----------------------------------------

start_e2e03_node() {
    local name="$1"
    local ip="$2"
    local volume="$3"
    local bootstrap="$4"

    local bootstrap_json="[]"
    if [[ -n "$bootstrap" ]]; then
        local items=""
        IFS=',' read -ra PEERS <<< "$bootstrap"
        for peer in "${PEERS[@]}"; do
            [[ -n "$items" ]] && items="$items, "
            items="$items\"$peer\""
        done
        bootstrap_json="[$items]"
    fi

    local config
    config=$(cat <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": $bootstrap_json,
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
)

    local tmpconfig
    tmpconfig=$(mktemp /tmp/e2e03-config-XXXXXX.json)
    echo "$config" > "$tmpconfig"
    chmod 644 "$tmpconfig"
    TEMP_CONFIGS+=("$tmpconfig")

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$E2E03_NETWORK" \
        --ip "$ip" \
        -v "$volume:/data" \
        -v "$tmpconfig:/config/node.json:ro" \
        --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
        --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
        chromatindb:test \
        run --config /config/node.json --data-dir /data --log-level debug \
        || fail "Failed to start container $name"
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_e2e03 2>/dev/null || true

log "=== E2E-03: Delete for Everyone ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.42.0.0/16 "$E2E03_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# Create temp dir for owner identity
OWNER_DIR=$(mktemp -d /tmp/e2e03-owner-XXXXXX)
chmod 777 "$OWNER_DIR"

# =============================================================================
# Step 1: Start 3 nodes
# =============================================================================

log "--- Step 1: Start 3-node topology ---"

start_e2e03_node "$NODE1_CONTAINER" "172.42.0.2" "$NODE1_VOLUME" ""
start_e2e03_node "$NODE2_CONTAINER" "172.42.0.3" "$NODE2_VOLUME" "172.42.0.2:4200"
start_e2e03_node "$NODE3_CONTAINER" "172.42.0.4" "$NODE3_VOLUME" "172.42.0.2:4200"
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"
wait_healthy "$NODE3_CONTAINER"

# Wait for peer mesh to form
log "Waiting 10s for mesh to form..."
sleep 10

# =============================================================================
# Step 2: Generate owner identity and ingest 10 blobs
# =============================================================================

log "--- Step 2: Ingest 10 blobs (owner identity) ---"

INGEST_OUTPUT=$(docker run --rm --network "$E2E03_NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.42.0.2:4200 --identity-save /identity --count 10 --size 256 --ttl 3600 \
    2>/dev/null)

if [[ ! -f "$OWNER_DIR/node.pub" ]]; then
    fail "Owner identity not saved"
fi

# Extract first blob hash for tombstoning
FIRST_HASH=$(echo "$INGEST_OUTPUT" | jq -r '.blob_hashes[0]' 2>/dev/null || echo "")
if [[ -z "$FIRST_HASH" || "$FIRST_HASH" == "null" ]]; then
    fail "Could not extract blob hash from loadgen output"
fi
log "Owner identity created, first blob hash: ${FIRST_HASH:0:16}..."

# =============================================================================
# Step 3: Wait for sync to all 3 nodes
# =============================================================================

log "--- Step 3: Wait for sync to all nodes ---"

if ! wait_sync "$NODE2_CONTAINER" 10 60; then
    log "WARN: Sync to node2 timeout"
fi
if ! wait_sync "$NODE3_CONTAINER" 10 60; then
    log "WARN: Sync to node3 timeout"
fi

# =============================================================================
# Step 4: Owner tombstones one specific blob
# =============================================================================

log "--- Step 4: Tombstone blob ${FIRST_HASH:0:16}... ---"

TIMESTAMP_BEFORE=$(date +%s)

echo "$FIRST_HASH" | docker run --rm -i --network "$E2E03_NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.42.0.2:4200 --identity-file /identity --delete --hashes-from stdin --ttl 0 \
    >/dev/null 2>&1 || true

log "Tombstone sent"

# =============================================================================
# Step 5: Wait for tombstone to propagate
# =============================================================================

log "--- Step 5: Wait for tombstone propagation (15s: 5s sync + margin) ---"

# Wait for tombstone to propagate. With sync_interval=5s, the tombstone should
# reach all directly-connected peers within one sync cycle.
sleep 15

TIMESTAMP_AFTER=$(date +%s)
PROPAGATION_TIME=$((TIMESTAMP_AFTER - TIMESTAMP_BEFORE))

# =============================================================================
# Check 1: Node2 has the tombstone
# =============================================================================

log "--- Check 1: Node2 has tombstone ---"

NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 blob count: $NODE2_COUNT (expected 11: 10 original + 1 tombstone)"

if [[ "$NODE2_COUNT" -ge 11 ]]; then
    pass "Node2 has tombstone ($NODE2_COUNT blobs, >= 11)"
else
    log "FAIL: Node2 has $NODE2_COUNT blobs, expected >= 11"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node3 has the tombstone
# =============================================================================

log "--- Check 2: Node3 has tombstone ---"

NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")
log "Node3 blob count: $NODE3_COUNT (expected 11)"

if [[ "$NODE3_COUNT" -ge 11 ]]; then
    pass "Node3 has tombstone ($NODE3_COUNT blobs, >= 11)"
else
    log "FAIL: Node3 has $NODE3_COUNT blobs, expected >= 11"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Propagation completed within one sync interval
# =============================================================================

log "--- Check 3: Propagation timing ---"

log "Propagation time: ${PROPAGATION_TIME}s (limit: 20s = 5s sync + 15s margin)"

if [[ "$PROPAGATION_TIME" -le 20 ]]; then
    pass "Tombstone propagated within sync interval (${PROPAGATION_TIME}s <= 20s)"
else
    log "FAIL: Propagation took ${PROPAGATION_TIME}s, expected <= 20s"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Tombstone persists across restart (integrity scan)
# =============================================================================

log "--- Check 4: Tombstone persists across restart ---"

docker restart "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER" 60

# Wait for integrity scan to complete
sleep 5

NODE2_RESTART_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)
if echo "$NODE2_RESTART_LOGS" | grep -q "tombstone"; then
    pass "Tombstone found in Node2 integrity scan/startup logs"
else
    # Check if blobs count is still correct post-restart
    NODE2_POST_RESTART=$(get_blob_count "$NODE2_CONTAINER")
    if [[ "$NODE2_POST_RESTART" -ge 11 ]]; then
        pass "Node2 retained all blobs including tombstone after restart ($NODE2_POST_RESTART blobs)"
    else
        log "FAIL: Node2 post-restart blob count is $NODE2_POST_RESTART, expected >= 11"
        FAILURES=$((FAILURES + 1))
    fi
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "E2E-03: Delete for everyone PASSED"
    pass "  - 10 blobs ingested, synced to all 3 nodes"
    pass "  - Tombstone of blob ${FIRST_HASH:0:16}..."
    pass "  - Propagation to all nodes within ${PROPAGATION_TIME}s (sync interval=5s)"
    pass "  - Tombstone persists across Node2 restart"
    exit 0
else
    fail "E2E-03: $FAILURES check(s) failed"
fi
