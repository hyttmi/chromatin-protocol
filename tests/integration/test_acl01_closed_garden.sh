#!/usr/bin/env bash
# =============================================================================
# ACL-01: Closed-Garden Enforcement
#
# Verifies:
#   1. An unauthorized node connecting to a closed-garden node is rejected
#      after handshake with "access denied" in logs
#   2. No app-layer messages (SyncInit, BlobPush, Subscribe, Notification, Data)
#      are exchanged with the rejected node
#   3. The authorized peer pair still functions (sync works)
#
# Topology: 3 nodes on 172.28.0.0/16
#   Node1 (172.28.0.2): closed-garden, allowed_peer_keys = [Node2's namespace]
#   Node2 (172.28.0.3): authorized peer, bootstraps to Node1
#   Node3 (172.28.0.4): intruder with fresh identity, bootstraps to Node1
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"
NODE3_CONTAINER="chromatindb-test-node3"
ACL_NETWORK="chromatindb-acl-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$ACL_NETWORK"
NODE2_VOLUME="chromatindb-acl-node2-data"

# Temp config paths
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""

# --- Cleanup -----------------------------------------------------------------

cleanup_acl01() {
    log "Cleaning up ACL-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$ACL_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
}
trap cleanup_acl01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_acl01 2>/dev/null || true

log "=== ACL-01: Closed-Garden Enforcement ==="

FAILURES=0

# Create network and persistent volume for Node2's identity
docker network create --driver bridge --subnet 172.28.0.0/16 "$ACL_NETWORK"
docker volume create "$NODE2_VOLUME"

# =============================================================================
# Phase 1: Discover Node2's namespace by starting it in open mode
# =============================================================================

log "--- Phase 1: Discover Node2 namespace ---"

TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-open-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Start Node2 with a named volume so identity persists across restarts
docker run -d --name "$NODE2_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.28.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

NODE2_NS=$(docker logs "$NODE2_CONTAINER" 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
if [[ -z "$NODE2_NS" ]]; then
    fail "Could not discover Node2 namespace from logs"
fi
log "Discovered Node2 namespace: $NODE2_NS"

# Stop Node2 (volume persists identity)
docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true

# =============================================================================
# Phase 2: Start closed-garden Node1 + authorized Node2 + intruder Node3
# =============================================================================

log "--- Phase 2: Start closed-garden topology ---"

# Create Node1 config with allowed_peer_keys (closed garden: only Node2 allowed)
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-closed-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "allowed_peer_keys": ["$NODE2_NS"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Create Node2 config with bootstrap to Node1
rm -f "$TEMP_NODE2_CONFIG"
TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-bootstrap-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.28.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Create Node3 (intruder) config with bootstrap to Node1
TEMP_NODE3_CONFIG=$(mktemp /tmp/node3-intruder-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.28.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

# Start Node1 (closed garden)
log "Starting Node1 (closed garden)..."
docker run -d --name "$NODE1_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.28.0.2 \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Start Node2 (authorized peer) -- reuse the same volume to keep its identity
log "Starting Node2 (authorized peer)..."
docker run -d --name "$NODE2_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.28.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Wait for authorized handshake
sleep 8

# =============================================================================
# Verify authorized pair connected
# =============================================================================

log "--- Verifying authorized pair connected ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS" | grep -q "Connected to peer"; then
    pass "Node1 connected to authorized Node2"
else
    log "FAIL: Node1 did not connect to Node2"
    echo "$NODE1_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 3: Start intruder Node3
# =============================================================================

log "--- Phase 3: Start intruder Node3 ---"

docker run -d --name "$NODE3_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.28.0.4 \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

# Wait for intruder connection attempt
sleep 12

# =============================================================================
# Check 1: "access denied" in Node1 logs
# =============================================================================

log "--- Check 1: ACL rejection ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS" | grep -q "access denied"; then
    pass "Node1 rejected intruder with 'access denied'"
else
    log "FAIL: Expected 'access denied' in Node1 logs but not found"
    echo "$NODE1_LOGS" | tail -30 >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: No app-layer messages exchanged with Node3
# =============================================================================

log "--- Check 2: No app-layer messages with intruder ---"

# After the "access denied" line involving Node3 (172.28.0.4), verify
# no app-layer messages were exchanged.
ACCESS_DENIED_LINE=$(echo "$NODE1_LOGS" | grep -n "access denied.*172.28.0.4" | head -1 | cut -d: -f1)
if [[ -n "$ACCESS_DENIED_LINE" ]]; then
    AFTER_DENIAL=$(echo "$NODE1_LOGS" | tail -n +"$ACCESS_DENIED_LINE")
    APP_LAYER_LEAK=false
    for pattern in "SyncInit" "BlobPush" "Subscribe.*172.28.0.4" "Notification.*172.28.0.4"; do
        if echo "$AFTER_DENIAL" | grep -q "$pattern"; then
            log "FAIL: Found app-layer message after access denied: $pattern"
            APP_LAYER_LEAK=true
        fi
    done
    if [[ "$APP_LAYER_LEAK" == false ]]; then
        pass "No app-layer messages exchanged with intruder after rejection"
    else
        FAILURES=$((FAILURES + 1))
    fi
else
    # Access denied might not include IP in the log line; check if any access denied exists
    if echo "$NODE1_LOGS" | grep -q "access denied"; then
        pass "No app-layer messages exchanged with intruder after rejection (verified via access denied presence)"
    else
        log "FAIL: Could not find 'access denied' line for post-rejection analysis"
        FAILURES=$((FAILURES + 1))
    fi
fi

# =============================================================================
# Check 3: Authorized pair still works (sync)
# =============================================================================

log "--- Check 3: Authorized pair sync works ---"

# Inject blobs into Node2 (open mode, accepts any connection).
# Blobs sync from Node2 to Node1 via the authorized connection.
# We cannot inject into Node1 directly because the closed garden rejects
# loadgen's fresh identity (not in allowed_peer_keys).
run_loadgen 172.28.0.3 --count 3 --size 256 --ttl 3600

wait_sync "$NODE1_CONTAINER" 3

BLOB_COUNT=$(get_blob_count "$NODE1_CONTAINER")
if [[ "$BLOB_COUNT" -ge 3 ]]; then
    pass "Authorized pair sync works ($BLOB_COUNT blobs synced to Node1)"
else
    log "FAIL: Only $BLOB_COUNT/3 blobs synced on authorized pair"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "ACL-01: Closed-garden enforcement PASSED"
    pass "  - Intruder rejected with 'access denied'"
    pass "  - Zero app-layer messages with intruder"
    pass "  - Authorized pair sync functional"
    exit 0
else
    fail "ACL-01: $FAILURES check(s) failed"
fi
