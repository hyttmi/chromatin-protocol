#!/usr/bin/env bash
# =============================================================================
# OPS-01: max_peers SIGHUP Hot Reload
#
# Verifies:
#   Phase 1: Node1 accepts 2 peers (max_peers=2), both connect
#   Phase 2: SIGHUP reduces max_peers to 1 -- existing 2 peers stay connected
#            (no mass disconnect), warning logged about excess (D-12)
#   Phase 3: Stop both peers, restart Node2 (connects, count=1=limit),
#            restart Node3 (should be refused -- max_peers=1)
#   Phase 4: SIGHUP increases max_peers to 5 -- Node3 can now connect
#
# Topology: 3-node standalone
#   Node1 (172.32.0.2): writable config, max_peers=2 initially
#   Node2 (172.32.0.3): bootstraps to Node1
#   Node3 (172.32.0.4): bootstraps to Node1
#
# Method: Volume-mounted writable config (NOT :ro) so host edits propagate.
#   SIGHUP via `docker kill -s HUP`. Verification via log inspection + SIGUSR1
#   metrics.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-ops01mp-node1"
NODE2_CONTAINER="chromatindb-ops01mp-node2"
NODE3_CONTAINER="chromatindb-ops01mp-node3"
OPS01MP_NETWORK="chromatindb-ops01mp-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$OPS01MP_NETWORK"

# Temp config files
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
NODE1_VOLUME="chromatindb-ops01mp-node1-data"
NODE2_VOLUME="chromatindb-ops01mp-node2-data"
NODE3_VOLUME="chromatindb-ops01mp-node3-data"

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

cleanup_ops01mp() {
    log "Cleaning up OPS-01 max_peers test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$OPS01MP_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
}
trap cleanup_ops01mp EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ops01mp 2>/dev/null || true

log "=== OPS-01: max_peers SIGHUP Hot Reload ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.32.0.0/16 "$OPS01MP_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# =============================================================================
# Phase 1: Start nodes with max_peers=2, verify both peers connect
# =============================================================================

log "--- Phase 1: Initial connection acceptance (max_peers=2) ---"

# Node1 config: max_peers=2, writable mount
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-ops01mp-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "max_peers": 2,
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Node2 config: bootstrap to Node1
TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-ops01mp-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.32.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Node3 config: bootstrap to Node1
TEMP_NODE3_CONFIG=$(mktemp /tmp/node3-ops01mp-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.32.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

# Start Node1 with writable config mount (no :ro)
docker run -d --name "$NODE1_CONTAINER" \
    --network "$OPS01MP_NETWORK" \
    --ip 172.32.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Start Node2
docker run -d --name "$NODE2_CONTAINER" \
    --network "$OPS01MP_NETWORK" \
    --ip 172.32.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Start Node3
docker run -d --name "$NODE3_CONTAINER" \
    --network "$OPS01MP_NETWORK" \
    --ip 172.32.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE3_CONTAINER"

# Wait for peer connections to establish
sleep 10

# Verify Node1 has 2 peers
INITIAL_PEERS=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 1 peer count: $INITIAL_PEERS"

if [[ "$INITIAL_PEERS" -eq 2 ]]; then
    pass "Phase 1: Node1 accepted both peers (peer_count=2)"
else
    log "FAIL: Expected 2 peers, got $INITIAL_PEERS"
    docker logs "$NODE1_CONTAINER" 2>&1 | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 2: Reduce max_peers via SIGHUP -- existing peers stay connected (D-12)
# =============================================================================

log "--- Phase 2: SIGHUP reduces max_peers to 1 (D-12: no mass disconnect) ---"

# Edit Node1's config to reduce max_peers
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "max_peers": 1,
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG

# Send SIGHUP to Node1
log "Sending SIGHUP to Node1..."
docker kill -s HUP "$NODE1_CONTAINER"

# Wait for config reload
sleep 5

# Verify SIGHUP was received and processed
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS" | grep -q "SIGHUP received"; then
    pass "Phase 2: SIGHUP received by Node1"
else
    log "FAIL: SIGHUP not received"
    echo "$NODE1_LOGS" | tail -10 >&2
    FAILURES=$((FAILURES + 1))
fi

# Verify the excess warning was logged (key D-12 validation)
if echo "$NODE1_LOGS" | grep -q "excess will drain naturally"; then
    pass "Phase 2: Excess peers warning logged (D-12 validated)"
else
    log "FAIL: Expected 'excess will drain naturally' in logs"
    echo "$NODE1_LOGS" | grep "config reload" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# Verify BOTH peers are still connected (no mass disconnect)
PHASE2_PEERS=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 2 peer count after SIGHUP: $PHASE2_PEERS"

if [[ "$PHASE2_PEERS" -eq 2 ]]; then
    pass "Phase 2: Both peers still connected after max_peers reduction (D-12)"
else
    log "FAIL: Expected 2 peers still connected, got $PHASE2_PEERS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 3: Verify new connections are refused when at limit
# =============================================================================

log "--- Phase 3: New connections refused when at limit ---"

# Stop both Node2 and Node3
docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true

# Wait for Node1 to detect disconnections
sleep 5

# Verify Node1 has 0 peers now
PHASE3_PEERS_BEFORE=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 3 peer count after stopping peers: $PHASE3_PEERS_BEFORE"

if [[ "$PHASE3_PEERS_BEFORE" -eq 0 ]]; then
    pass "Phase 3: Node1 at 0 peers after stopping Node2 and Node3"
else
    log "WARN: Expected 0 peers, got $PHASE3_PEERS_BEFORE (proceeding)"
fi

# Restart Node2 (should connect, count goes to 1 = max_peers)
docker run -d --name "$NODE2_CONTAINER" \
    --network "$OPS01MP_NETWORK" \
    --ip 172.32.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Wait for Node2 to connect to Node1
sleep 8

PHASE3_PEERS_MID=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 3 peer count after Node2 restart: $PHASE3_PEERS_MID"

if [[ "$PHASE3_PEERS_MID" -eq 1 ]]; then
    pass "Phase 3: Node2 reconnected (peer_count=1, at max_peers limit)"
else
    log "FAIL: Expected 1 peer after Node2 restart, got $PHASE3_PEERS_MID"
    FAILURES=$((FAILURES + 1))
fi

# Restart Node3 (should be refused -- Node1 already at max_peers=1)
docker run -d --name "$NODE3_CONTAINER" \
    --network "$OPS01MP_NETWORK" \
    --ip 172.32.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

# Wait for Node3's connection attempt to be rejected
sleep 8

# Verify Node1 rejected the connection
NODE1_LOGS_PHASE3=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS_PHASE3" | grep -q "rejected inbound connection (max peers reached)"; then
    pass "Phase 3: Node3 connection rejected (max peers reached)"
else
    log "FAIL: Expected 'rejected inbound connection (max peers reached)' in Node1 logs"
    echo "$NODE1_LOGS_PHASE3" | grep -i "reject\|max.peer\|refused" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# Verify Node1 still has only 1 peer (Node2)
PHASE3_PEERS_AFTER=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 3 peer count after Node3 rejected: $PHASE3_PEERS_AFTER"

if [[ "$PHASE3_PEERS_AFTER" -eq 1 ]]; then
    pass "Phase 3: Node1 still at 1 peer (Node3 was refused)"
else
    log "FAIL: Expected 1 peer, got $PHASE3_PEERS_AFTER"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 4: Increase max_peers via SIGHUP -- new connections accepted
# =============================================================================

log "--- Phase 4: SIGHUP increases max_peers to 5 ---"

# Stop Node3 so we can restart it cleanly after SIGHUP
docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
sleep 2

# Edit Node1's config to increase max_peers
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "max_peers": 5,
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG

# Send SIGHUP to Node1
log "Sending SIGHUP to Node1..."
docker kill -s HUP "$NODE1_CONTAINER"

# Wait for config reload
sleep 3

# Verify the reload log
NODE1_LOGS_PHASE4=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS_PHASE4" | grep -q "config reload: max_peers=5"; then
    pass "Phase 4: max_peers increased to 5"
else
    log "FAIL: Expected 'config reload: max_peers=5' in logs"
    echo "$NODE1_LOGS_PHASE4" | grep "config reload" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# Restart Node3 -- should now be accepted
docker run -d --name "$NODE3_CONTAINER" \
    --network "$OPS01MP_NETWORK" \
    --ip 172.32.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE3_CONTAINER"

# Wait for connection
sleep 8

# Verify Node1 now has 2 peers
PHASE4_PEERS=$(get_peer_count "$NODE1_CONTAINER")
log "Phase 4 peer count: $PHASE4_PEERS"

if [[ "$PHASE4_PEERS" -eq 2 ]]; then
    pass "Phase 4: Node1 accepted Node3 after max_peers increase (peer_count=2)"
else
    log "FAIL: Expected 2 peers after max_peers increase, got $PHASE4_PEERS"
    docker logs "$NODE1_CONTAINER" 2>&1 | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "OPS-01: max_peers SIGHUP hot reload PASSED"
    pass "  - Phase 1: Both peers accepted (max_peers=2)"
    pass "  - Phase 2: Excess warning logged, no mass disconnect (D-12)"
    pass "  - Phase 3: New connections refused at limit (max_peers=1)"
    pass "  - Phase 4: New connections accepted after increase (max_peers=5)"
    exit 0
else
    fail "OPS-01: $FAILURES check(s) failed"
fi
