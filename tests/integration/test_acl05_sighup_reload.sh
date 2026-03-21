#!/usr/bin/env bash
# =============================================================================
# ACL-05: SIGHUP ACL Hot-Reload
#
# Verifies:
#   Phase 1 (add key): After SIGHUP with a newly-added key in allowed_keys,
#     a previously-rejected node connects successfully.
#   Phase 2 (remove key): After SIGHUP with the key removed, the connected
#     node is disconnected.
#
# Topology: 3 nodes on 172.29.0.0/16
#   Node1 (172.29.0.2): closed-garden, config edited at runtime via SIGHUP
#   Node2 (172.29.0.3): always-authorized peer (in allowed_keys from start)
#   Node3 (172.29.0.4): initially unauthorized, added via SIGHUP, then removed
#
# Method: Uses volume-mounted config files (NOT :ro) so host edits propagate.
#   SIGHUP via `docker kill -s HUP`. Verification via log inspection + SIGUSR1
#   peer count.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"
NODE3_CONTAINER="chromatindb-test-node3"
ACL_NETWORK="chromatindb-acl05-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$ACL_NETWORK"

# Temp files
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
NODE2_VOLUME="chromatindb-acl05-node2-data"
NODE3_VOLUME="chromatindb-acl05-node3-data"

# Helper: extract peer count from SIGUSR1 metrics dump
get_peer_count() {
    local container="$1"
    docker kill -s USR1 "$container" >/dev/null 2>&1 || true
    sleep 2
    local all_logs
    all_logs=$(docker logs "$container" 2>&1)
    local count
    count=$(echo "$all_logs" | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+') || count="0"
    echo "$count"
}

# --- Cleanup -----------------------------------------------------------------

cleanup_acl05() {
    log "Cleaning up ACL-05 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$ACL_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
}
trap cleanup_acl05 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_acl05 2>/dev/null || true

log "=== ACL-05: SIGHUP ACL Hot-Reload ==="

FAILURES=0

# Create network and persistent volumes
docker network create --driver bridge --subnet 172.29.0.0/16 "$ACL_NETWORK"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# =============================================================================
# Discover Node2 and Node3 namespaces by starting them briefly
# =============================================================================

log "--- Discovering Node2 and Node3 namespaces ---"

# Node2 open-mode config
TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-acl05-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Node3 open-mode config
TEMP_NODE3_CONFIG=$(mktemp /tmp/node3-acl05-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

# Start Node2 to discover its namespace
docker run -d --name "$NODE2_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.29.0.3 \
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

# Stop Node2 (volume preserves identity)
docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true

# Start Node3 to discover its namespace
docker run -d --name "$NODE3_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.29.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE3_CONTAINER"

NODE3_NS=$(docker logs "$NODE3_CONTAINER" 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
if [[ -z "$NODE3_NS" ]]; then
    fail "Could not discover Node3 namespace from logs"
fi
log "Discovered Node3 namespace: $NODE3_NS"

# Stop Node3 (volume preserves identity)
docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true

# =============================================================================
# Start Node1 (closed-garden) with Node2 allowed, Node3 NOT allowed
# =============================================================================

log "--- Starting closed-garden topology ---"

# Node1 config: allowed_keys = [Node2] only. Writable mount for SIGHUP.
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-acl05-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "allowed_keys": ["$NODE2_NS"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Start Node1 with writable config mount (no :ro)
docker run -d --name "$NODE1_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.29.0.2 \
    -v "$TEMP_NODE1_CONFIG:/config/node.json" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Start Node2 (authorized) with bootstrap to Node1
rm -f "$TEMP_NODE2_CONFIG"
TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-acl05-bootstrap-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.29.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

docker run -d --name "$NODE2_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.29.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Start Node3 (unauthorized) with bootstrap to Node1
rm -f "$TEMP_NODE3_CONFIG"
TEMP_NODE3_CONFIG=$(mktemp /tmp/node3-acl05-bootstrap-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.29.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

docker run -d --name "$NODE3_CONTAINER" \
    --network "$ACL_NETWORK" \
    --ip 172.29.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

# Wait for Node2 to connect and Node3 to be rejected
sleep 12

# Verify initial state: Node1 has 1 peer (Node2), Node3 was rejected
log "--- Verifying initial state ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS" | grep -q "Connected to peer"; then
    pass "Node1 accepted Node2 connection"
else
    log "FAIL: Node1 did not connect to Node2"
    echo "$NODE1_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

if echo "$NODE1_LOGS" | grep -q "access denied"; then
    pass "Node1 rejected Node3 (access denied)"
else
    log "FAIL: Expected 'access denied' for Node3 but not found"
    FAILURES=$((FAILURES + 1))
fi

# Check initial peer count = 1
INITIAL_PEERS=$(get_peer_count "$NODE1_CONTAINER")
log "Initial peer count: $INITIAL_PEERS"

if [[ "$INITIAL_PEERS" -eq 1 ]]; then
    pass "Node1 has exactly 1 peer (Node2 only)"
else
    log "FAIL: Expected 1 peer, got $INITIAL_PEERS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 1: Add Node3 key via SIGHUP
# =============================================================================

log "--- Phase 1: Add Node3 key via SIGHUP ---"

# Edit Node1's config to include Node3's namespace
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "allowed_keys": ["$NODE2_NS", "$NODE3_NS"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG

# Send SIGHUP to Node1
log "Sending SIGHUP to Node1..."
docker kill -s HUP "$NODE1_CONTAINER"

# Wait for Node3's auto-reconnect to establish connection
sleep 15

# Check Node1 logs for reload confirmation
NODE1_LOGS_AFTER=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS_AFTER" | grep -q "SIGHUP received"; then
    pass "SIGHUP received by Node1"
else
    log "FAIL: SIGHUP not received"
    log "  DEBUG: last 10 log lines:"
    echo "$NODE1_LOGS_AFTER" | tail -10 >&2
    FAILURES=$((FAILURES + 1))
fi

if echo "$NODE1_LOGS_AFTER" | grep -q "config reload: +1 keys"; then
    pass "Config reload added 1 key"
else
    log "FAIL: Expected 'config reload: +1 keys'"
    echo "$NODE1_LOGS_AFTER" | grep "config reload" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# Check that Node3 is now connected (peer count should be 2)
PHASE1_PEERS=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 1 peer count: $PHASE1_PEERS"

if [[ "$PHASE1_PEERS" -eq 2 ]]; then
    pass "Phase 1: Node1 has 2 peers after adding Node3 key"
else
    log "FAIL: Expected 2 peers after adding Node3 key, got $PHASE1_PEERS"
    docker logs "$NODE1_CONTAINER" 2>&1 | tail -30 >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 2: Remove Node3 key via SIGHUP
# =============================================================================

log "--- Phase 2: Remove Node3 key via SIGHUP ---"

# Edit Node1's config to remove Node3's namespace
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "allowed_keys": ["$NODE2_NS"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG

# Send SIGHUP to Node1
log "Sending SIGHUP to Node1..."
docker kill -s HUP "$NODE1_CONTAINER"

# Wait for disconnect
sleep 8

# Check Node1 logs for revocation
NODE1_LOGS_AFTER2=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS_AFTER2" | grep -q "revoking peer"; then
    pass "Node3 revoked after key removal"
else
    log "FAIL: Expected 'revoking peer' in logs"
    log "  DEBUG: last 10 log lines:"
    echo "$NODE1_LOGS_AFTER2" | tail -10 >&2
    FAILURES=$((FAILURES + 1))
fi

# Check peer count should be back to 1
PHASE2_PEERS=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 2 peer count: $PHASE2_PEERS"

if [[ "$PHASE2_PEERS" -eq 1 ]]; then
    pass "Phase 2: Node1 back to 1 peer after removing Node3 key"
else
    log "FAIL: Expected 1 peer after removing Node3 key, got $PHASE2_PEERS"
    docker logs "$NODE1_CONTAINER" 2>&1 | tail -30 >&2
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "ACL-05: SIGHUP ACL hot-reload PASSED"
    pass "  - SIGHUP received and config reloaded"
    pass "  - Phase 1: Added key -> new connection established (peers: 1 -> 2)"
    pass "  - Phase 2: Removed key -> connection dropped (peers: 2 -> 1)"
    exit 0
else
    fail "ACL-05: $FAILURES check(s) failed"
fi
