#!/usr/bin/env bash
# =============================================================================
# TTL-01: Tombstone Propagation (100 Blobs)
#
# Verifies:
#   1. 100 tombstones propagate to all 3 peers (all nodes reach 200 blobs)
#   2. Tombstone map entries confirmed on node2 via integrity scan restart
#   3. Original blobs are effectively dead (tombstones tracked in storage)
#
# Topology: 3-node open-mode cluster via standalone docker run
#   Node1 (172.39.0.2): owner ingests 100 blobs, then deletes all via tombstone
#   Node2 (172.39.0.3): bootstraps to Node1
#   Node3 (172.39.0.4): bootstraps to Node1
#   Network: 172.39.0.0/16 (chromatindb-ttl01-test-net)
#
# Flow:
#   1. Start node1, generate owner identity, ingest 100 blobs
#   2. Start node2 + node3 bootstrapping to node1, wait sync to 100
#   3. Owner deletes all 100 blobs via tombstones
#   4. Wait for tombstones to sync (200 blobs = 100 original + 100 tombstones)
#   5. Verify all 3 nodes have 200 blobs
#   6. Verify tombstone map entries >= 100 on node2 via restart integrity scan
#   7. Verify tombstones tracked in storage
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-ttl01-node1"
NODE2_CONTAINER="chromatindb-ttl01-node2"
NODE3_CONTAINER="chromatindb-ttl01-node3"
TTL01_NETWORK="chromatindb-ttl01-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$TTL01_NETWORK"

# Temp files and volumes
OWNER_DIR=""
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
NODE1_VOLUME="chromatindb-ttl01-node1-data"
NODE2_VOLUME="chromatindb-ttl01-node2-data"
NODE3_VOLUME="chromatindb-ttl01-node3-data"

# --- Cleanup -----------------------------------------------------------------

cleanup_ttl01() {
    log "Cleaning up TTL-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$TTL01_NETWORK" 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
}
trap cleanup_ttl01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ttl01 2>/dev/null || true

log "=== TTL-01: Tombstone Propagation (100 Blobs) ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.39.0.0/16 "$TTL01_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# Create host temp dir for identity persistence
OWNER_DIR=$(mktemp -d /tmp/ttl01-owner-XXXXXX)
chmod 777 "$OWNER_DIR"

# Create config files
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-ttl01-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-ttl01-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.39.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

TEMP_NODE3_CONFIG=$(mktemp /tmp/node3-ttl01-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.39.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

# =============================================================================
# Step 1: Start node1, generate owner identity, ingest 100 blobs
# =============================================================================

log "--- Step 1: Start node1 and ingest 100 blobs ---"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$TTL01_NETWORK" \
    --ip 172.39.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"

# Ingest 100 blobs with saved identity
INGEST_OUTPUT=$(docker run --rm --network "$TTL01_NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.39.0.2:4200 --identity-save /identity \
    --count 100 --size 256 --rate 100 --ttl 3600 \
    2>/tmp/ttl01-ingest-stderr.txt)

INGEST_ERRORS=$(echo "$INGEST_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
INGEST_TOTAL=$(echo "$INGEST_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Ingest: $INGEST_TOTAL blobs, $INGEST_ERRORS errors"

if [[ ! -f "$OWNER_DIR/node.pub" ]]; then
    fail "Owner identity not saved"
fi

# Capture blob hashes for later deletion
BLOB_HASHES=$(echo "$INGEST_OUTPUT" | jq -r '.blob_hashes[]' 2>/dev/null)
HASH_COUNT=$(echo "$BLOB_HASHES" | wc -l)
log "Captured $HASH_COUNT blob hashes for deletion"

# Verify node1 has 100 blobs
NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count after ingest: $NODE1_COUNT"

# =============================================================================
# Step 2: Start node2 + node3, wait for sync to 100 blobs
# =============================================================================

log "--- Step 2: Start node2 + node3, sync to 100 blobs ---"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$TTL01_NETWORK" \
    --ip 172.39.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

docker run -d --name "$NODE3_CONTAINER" \
    --network "$TTL01_NETWORK" \
    --ip 172.39.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE2_CONTAINER"
wait_healthy "$NODE3_CONTAINER"

# Wait for peer connections to establish
sleep 10

log "Waiting for sync of 100 blobs to node2 and node3..."
wait_sync "$NODE2_CONTAINER" 100 180 || true
wait_sync "$NODE3_CONTAINER" 100 180 || true

NODE2_PRE=$(get_blob_count "$NODE2_CONTAINER")
NODE3_PRE=$(get_blob_count "$NODE3_CONTAINER")
log "Pre-delete: Node2=$NODE2_PRE, Node3=$NODE3_PRE blobs"

# =============================================================================
# Step 3: Owner deletes all 100 blobs via tombstones
# =============================================================================

log "--- Step 3: Owner deletes all 100 blobs via tombstones ---"

DELETE_OUTPUT=$(echo "$BLOB_HASHES" | docker run --rm -i --network "$TTL01_NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.39.0.2:4200 --identity-file /identity \
    --delete --hashes-from stdin --ttl 0 \
    2>/dev/null) || true

DELETE_TOTAL=$(echo "$DELETE_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
DELETE_ERRORS=$(echo "$DELETE_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Delete: $DELETE_TOTAL tombstones sent, $DELETE_ERRORS errors"

# =============================================================================
# Step 4: Wait for tombstones to sync (200 total = 100 original + 100 tombstones)
# =============================================================================

log "--- Step 4: Waiting for tombstone sync (target: 200 blobs) ---"

wait_sync "$NODE1_CONTAINER" 200 120 || true
wait_sync "$NODE2_CONTAINER" 200 120 || true
wait_sync "$NODE3_CONTAINER" 200 120 || true

# =============================================================================
# Check 1: All 3 nodes have 200 blobs (originals + tombstones)
# =============================================================================

log "--- Check 1: All nodes have 200 blobs ---"

NODE1_FINAL=$(get_blob_count "$NODE1_CONTAINER")
NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
NODE3_FINAL=$(get_blob_count "$NODE3_CONTAINER")
log "Final counts: Node1=$NODE1_FINAL, Node2=$NODE2_FINAL, Node3=$NODE3_FINAL"

if [[ "$NODE1_FINAL" -ge 200 && "$NODE2_FINAL" -ge 200 && "$NODE3_FINAL" -ge 200 ]]; then
    pass "All 3 nodes have >= 200 blobs (100 original + 100 tombstones)"
else
    log "FAIL: Expected >= 200 blobs on all nodes: Node1=$NODE1_FINAL, Node2=$NODE2_FINAL, Node3=$NODE3_FINAL"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Tombstone map entries >= 100 on node2 (restart triggers integrity scan)
# =============================================================================

log "--- Check 2: Verify tombstone map entries on node2 ---"

# Restart node2 to trigger integrity_scan at startup
docker restart "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER" 60

# Give the startup integrity scan time to complete
sleep 5

# Check node2 logs for integrity scan tombstone count
NODE2_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)
TOMBSTONE_COUNT=$(echo "$NODE2_LOGS" | grep -oP 'tombstone=\K[0-9]+' | tail -1 || echo "0")
log "Node2 integrity scan tombstone entries: $TOMBSTONE_COUNT"

if [[ "$TOMBSTONE_COUNT" -ge 100 ]]; then
    pass "Node2 tombstone_map has $TOMBSTONE_COUNT entries (>= 100)"
else
    log "FAIL: Expected >= 100 tombstone entries, got $TOMBSTONE_COUNT"
    echo "$NODE2_LOGS" | grep "integrity" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Verify tombstones are tracked in storage on node3
# =============================================================================

log "--- Check 3: Verify tombstones tracked on node3 ---"

# Also restart node3 for integrity scan
docker restart "$NODE3_CONTAINER"
wait_healthy "$NODE3_CONTAINER" 60
sleep 5

NODE3_LOGS=$(docker logs "$NODE3_CONTAINER" 2>&1)
NODE3_TOMBSTONE=$(echo "$NODE3_LOGS" | grep -oP 'tombstone=\K[0-9]+' | tail -1 || echo "0")
log "Node3 integrity scan tombstone entries: $NODE3_TOMBSTONE"

if [[ "$NODE3_TOMBSTONE" -ge 100 ]]; then
    pass "Node3 tombstone_map has $NODE3_TOMBSTONE entries (>= 100)"
else
    log "FAIL: Expected >= 100 tombstone entries on Node3, got $NODE3_TOMBSTONE"
    echo "$NODE3_LOGS" | grep "integrity" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "TTL-01: Tombstone propagation (100 blobs) PASSED"
    pass "  - 100 tombstones sent by owner"
    pass "  - All 3 nodes reached 200 blobs (originals + tombstones)"
    pass "  - Node2 tombstone_map: $TOMBSTONE_COUNT entries"
    pass "  - Node3 tombstone_map: $NODE3_TOMBSTONE entries"
    exit 0
else
    fail "TTL-01: $FAILURES check(s) failed"
fi
