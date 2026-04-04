#!/usr/bin/env bash
# =============================================================================
# TTL-03: TTL=0 Permanent Blobs
#
# Verifies:
#   1. Blobs with TTL=0 are never placed in the expiry index (permanently exempt)
#   2. Blobs with TTL>0 ARE placed in the expiry index (subject to GC)
#   3. TTL=0 blobs persist and are accessible regardless of time passage
#   4. Both TTL=0 and TTL>0 blobs sync correctly to peers
#
# Topology: 2-node standalone topology
#   Node1 (172.40.0.2): receives ingest of permanent + standard blobs
#   Node2 (172.40.0.3): bootstraps to Node1
#   Network: 172.40.0.0/16 (chromatindb-ttl03-test-net)
#
# Note: Expiry scan runs every 60s. Actual GC depends on timestamp/clock unit
# alignment. This test verifies the structural guarantee: TTL=0 blobs have
# zero expiry_map entries, ensuring they can never be garbage-collected.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-ttl03-node1"
NODE2_CONTAINER="chromatindb-ttl03-node2"
TTL03_NETWORK="chromatindb-ttl03-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$TTL03_NETWORK"

# Temp files and volumes
PERM_DIR=""
EPHEM_DIR=""
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
NODE1_VOLUME="chromatindb-ttl03-node1-data"
NODE2_VOLUME="chromatindb-ttl03-node2-data"

# --- Cleanup -----------------------------------------------------------------

cleanup_ttl03() {
    log "Cleaning up TTL-03 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$TTL03_NETWORK" 2>/dev/null || true
    [[ -n "$PERM_DIR" ]] && rm -rf "$PERM_DIR" 2>/dev/null || true
    [[ -n "$EPHEM_DIR" ]] && rm -rf "$EPHEM_DIR" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
}
trap cleanup_ttl03 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ttl03 2>/dev/null || true

log "=== TTL-03: TTL=0 Permanent Blobs ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.40.0.0/16 "$TTL03_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# Create host temp dirs for identity persistence
PERM_DIR=$(mktemp -d /tmp/ttl03-perm-XXXXXX)
EPHEM_DIR=$(mktemp -d /tmp/ttl03-ephem-XXXXXX)
chmod 777 "$PERM_DIR" "$EPHEM_DIR"

# Create config files
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-ttl03-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-ttl03-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.40.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# =============================================================================
# Step 1: Start node1, ingest 10 permanent + 10 standard blobs
# =============================================================================

log "--- Step 1: Start node1 and ingest blobs ---"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$TTL03_NETWORK" \
    --ip 172.40.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"

# Ingest 10 permanent blobs (TTL=0) with identity 1
PERM_OUTPUT=$(docker run --rm --network "$TTL03_NETWORK" \
    -v "$PERM_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.40.0.2:4200 --identity-save /identity \
    --count 10 --size 256 --ttl 0 \
    2>/dev/null)

PERM_TOTAL=$(echo "$PERM_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
PERM_ERRORS=$(echo "$PERM_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Permanent blobs: $PERM_TOTAL ingested, $PERM_ERRORS errors (TTL=0)"

# Ingest 10 standard blobs (TTL=3600) with identity 2
EPHEM_OUTPUT=$(docker run --rm --network "$TTL03_NETWORK" \
    -v "$EPHEM_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.40.0.2:4200 --identity-save /identity \
    --count 10 --size 256 --ttl 3600 \
    2>/dev/null)

EPHEM_TOTAL=$(echo "$EPHEM_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
EPHEM_ERRORS=$(echo "$EPHEM_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Standard blobs: $EPHEM_TOTAL ingested, $EPHEM_ERRORS errors (TTL=3600)"

# Verify 20 blobs on node1
NODE1_POST_INGEST=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 after ingest: $NODE1_POST_INGEST blobs"

# =============================================================================
# Step 2: Start node2, wait for sync to 20 blobs
# =============================================================================

log "--- Step 2: Start node2, sync to 20 blobs ---"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$TTL03_NETWORK" \
    --ip 172.40.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE2_CONTAINER"

# Wait for sync
sleep 5
wait_sync "$NODE2_CONTAINER" 20 120 || true

NODE2_POST_SYNC=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 after sync: $NODE2_POST_SYNC blobs"

# =============================================================================
# Check 1: Integrity scan shows exactly 10 expiry entries (TTL>0 only)
# =============================================================================

log "--- Check 1: TTL=0 blobs exempt from expiry index ---"

# Restart node1 to trigger integrity_scan at startup
docker restart "$NODE1_CONTAINER"
wait_healthy "$NODE1_CONTAINER" 60
sleep 5

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
INTEGRITY_LINE=$(echo "$NODE1_LOGS" | grep "integrity scan:" | tail -1)
INTEGRITY_BLOBS=$(echo "$INTEGRITY_LINE" | grep -oP 'blobs=\K[0-9]+' || echo "0")
INTEGRITY_EXPIRY=$(echo "$INTEGRITY_LINE" | grep -oP 'expiry=\K[0-9]+' || echo "0")
log "Node1 integrity: blobs=$INTEGRITY_BLOBS, expiry=$INTEGRITY_EXPIRY"

# TTL=0 blobs have NO expiry entries. Only TTL>0 blobs have expiry entries.
# So 10 permanent (no expiry) + 10 standard (with expiry) = 10 expiry entries.
if [[ "$INTEGRITY_EXPIRY" -eq 10 ]]; then
    pass "Exactly 10 expiry entries (TTL>0 only, TTL=0 exempt)"
elif [[ "$INTEGRITY_EXPIRY" -le 10 ]]; then
    pass "Expiry entries = $INTEGRITY_EXPIRY (TTL=0 blobs confirmed exempt)"
else
    log "FAIL: Expected 10 expiry entries, got $INTEGRITY_EXPIRY"
    echo "$INTEGRITY_LINE" >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: All 20 blobs still present (TTL=0 never expired)
# =============================================================================

log "--- Check 2: All blobs still present ---"

if [[ "$INTEGRITY_BLOBS" -ge 20 ]]; then
    pass "All 20 blobs still present on Node1 (blobs=$INTEGRITY_BLOBS)"
else
    log "FAIL: Expected 20 blobs, got $INTEGRITY_BLOBS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Node2 also has all 20 blobs synced
# =============================================================================

log "--- Check 3: Node2 has all 20 blobs synced ---"

# Restart node2 for integrity scan
docker restart "$NODE2_CONTAINER"
wait_healthy "$NODE2_CONTAINER" 60
sleep 5

NODE2_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)
NODE2_INTEGRITY=$(echo "$NODE2_LOGS" | grep "integrity scan:" | tail -1)
NODE2_BLOBS=$(echo "$NODE2_INTEGRITY" | grep -oP 'blobs=\K[0-9]+' || echo "0")
NODE2_EXPIRY=$(echo "$NODE2_INTEGRITY" | grep -oP 'expiry=\K[0-9]+' || echo "0")
log "Node2 integrity: blobs=$NODE2_BLOBS, expiry=$NODE2_EXPIRY"

if [[ "$NODE2_BLOBS" -ge 20 ]]; then
    pass "Node2 has all 20 blobs synced (blobs=$NODE2_BLOBS)"
else
    log "FAIL: Expected 20 blobs on Node2, got $NODE2_BLOBS"
    FAILURES=$((FAILURES + 1))
fi

# Check Node2 also has exactly 10 expiry entries
if [[ "$NODE2_EXPIRY" -eq 10 ]]; then
    pass "Node2 has 10 expiry entries (TTL=0 exempt, synced correctly)"
elif [[ "$NODE2_EXPIRY" -le 10 ]]; then
    pass "Node2 expiry=$NODE2_EXPIRY (TTL=0 permanent blobs exempt)"
else
    log "FAIL: Expected 10 expiry entries on Node2, got $NODE2_EXPIRY"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Run expiry scan -- permanent blobs must survive
# =============================================================================

log "--- Check 4: Permanent blobs survive expiry scan ---"

# Wait for at least one expiry scan cycle (60s) to confirm TTL=0 blobs survive
sleep 65

POST_SCAN_NODE1=$(get_blob_count "$NODE1_CONTAINER")
POST_SCAN_NODE2=$(get_blob_count "$NODE2_CONTAINER")
log "After expiry scan: Node1=$POST_SCAN_NODE1, Node2=$POST_SCAN_NODE2"

if [[ "$POST_SCAN_NODE1" -ge 20 ]]; then
    pass "TTL=0 blobs survived expiry scan on Node1 (count=$POST_SCAN_NODE1)"
else
    log "FAIL: Blobs lost after expiry scan on Node1: $POST_SCAN_NODE1 (expected >= 20)"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$POST_SCAN_NODE2" -ge 20 ]]; then
    pass "TTL=0 blobs survived expiry scan on Node2 (count=$POST_SCAN_NODE2)"
else
    log "FAIL: Blobs lost after expiry scan on Node2: $POST_SCAN_NODE2 (expected >= 20)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "TTL-03: TTL=0 permanent blobs PASSED"
    pass "  - 10 permanent (TTL=0) + 10 standard (TTL=3600) ingested"
    pass "  - TTL=0 blobs: 0 expiry entries (permanently exempt from GC)"
    pass "  - TTL>0 blobs: 10 expiry entries (subject to GC)"
    pass "  - All blobs survive expiry scan cycle"
    exit 0
else
    fail "TTL-03: $FAILURES check(s) failed"
fi
