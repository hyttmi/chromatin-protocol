#!/usr/bin/env bash
# =============================================================================
# CRYPT-06: Trusted Peer Lightweight Handshake Bypass
#
# Verifies:
#   1. Trusted peer lightweight handshake succeeds without KEM exchange
#      (TrustedHello sent, no PQRequired reply, no KEM messages)
#   2. Sync works over the trusted connection
#   3. Wrong identity key on a trusted IP is still rejected
#      (identity verification is enforced even for trusted peers)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_TRUSTED="docker compose -f $SCRIPT_DIR/docker-compose.trusted.yml -p chromatindb-test"
IMPOSTOR_CONTAINER="chromatindb-test-node3-impostor"

# Temp config paths (set before cleanup so trap can reference them)
TEMP_CONFIG=""
IMPOSTOR_CONFIG=""

# --- Cleanup -----------------------------------------------------------------

cleanup_crypt06() {
    log "Cleaning up CRYPT-06 test..."
    docker rm -f "$IMPOSTOR_CONTAINER" 2>/dev/null || true
    docker rm -f chromatindb-test-node1 2>/dev/null || true
    $COMPOSE_TRUSTED down -v --remove-orphans 2>/dev/null || true
    if [[ -n "$TEMP_CONFIG" ]]; then
        rm -f "$TEMP_CONFIG" 2>/dev/null || true
    fi
    if [[ -n "$IMPOSTOR_CONFIG" ]]; then
        rm -f "$IMPOSTOR_CONFIG" 2>/dev/null || true
    fi
}
trap cleanup_crypt06 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_crypt06 2>/dev/null || true

log "=== CRYPT-06: Trusted Peer Bypass ==="

FAILURES=0

# =============================================================================
# Part 1: Trusted handshake succeeds without KEM
# =============================================================================

log "--- Part 1: Lightweight handshake (no KEM) ---"

# Start trusted topology
log "Starting trusted topology (node1 + node2 with mutual trust)..."
$COMPOSE_TRUSTED up -d
wait_healthy chromatindb-test-node1
wait_healthy chromatindb-test-node2

# Wait for handshake to complete
sleep 5

# Check node2 (initiator) used lightweight handshake
NODE2_LOGS=$(docker logs chromatindb-test-node2 2>&1)

if echo "$NODE2_LOGS" | grep -q "handshake complete (initiator, lightweight)"; then
    pass "Node2 used lightweight handshake (TrustedHello)"
else
    log "FAIL: Node2 did not use lightweight handshake"
    log "  Node2 logs (last 20 lines):"
    echo "$NODE2_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# Check node1 (responder) used lightweight handshake
NODE1_LOGS=$(docker logs chromatindb-test-node1 2>&1)

if echo "$NODE1_LOGS" | grep -q "handshake complete (responder, lightweight)"; then
    pass "Node1 used lightweight responder handshake"
else
    log "FAIL: Node1 did not use lightweight responder handshake"
    log "  Node1 logs (last 20 lines):"
    echo "$NODE1_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# Verify ABSENCE of KEM-related messages for this handshake
# KEM messages would indicate a full PQ handshake, not lightweight
if echo "$NODE2_LOGS" | grep -q "handshake complete (initiator, PQ)"; then
    log "FAIL: Node2 used PQ handshake instead of lightweight"
    FAILURES=$((FAILURES + 1))
else
    pass "No PQ handshake on node2 (KEM bypassed as expected)"
fi

# Verify no PQRequired was sent by node1
if echo "$NODE1_LOGS" | grep -q "peer requires PQ"; then
    log "FAIL: PQRequired exchange detected (should not happen for mutual trust)"
    FAILURES=$((FAILURES + 1))
else
    pass "No PQRequired exchange (mutual trust confirmed)"
fi

# =============================================================================
# Part 2: Sync works over trusted connection
# =============================================================================

log "--- Part 2: Sync over trusted connection ---"

log "Ingesting 3 blobs to node1..."
run_loadgen 172.28.0.2 --count 3 --size 256 --ttl 3600

log "Waiting for sync to node2..."
wait_sync chromatindb-test-node2 3

BLOB_COUNT=$(get_blob_count chromatindb-test-node2)
if [[ "$BLOB_COUNT" -ge 3 ]]; then
    pass "Sync works over trusted connection ($BLOB_COUNT blobs on node2)"
else
    log "FAIL: Only $BLOB_COUNT/3 blobs synced over trusted connection"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Part 3: Wrong identity on trusted IP is rejected
# =============================================================================

log "--- Part 3: Wrong identity on trusted IP rejected ---"

# Discover node2's namespace from its startup logs.
# The node logs "namespace: <64-char hex>" at startup (db/main.cpp:120).
NODE2_NS=$(docker logs chromatindb-test-node2 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
if [[ -z "$NODE2_NS" ]]; then
    fail "Could not discover node2 namespace from logs"
fi
log "Discovered node2 namespace: $NODE2_NS"

# Create a temporary node1 config with both trusted_peers AND allowed_peer_keys.
# allowed_peer_keys restricts which identities are accepted -- the impostor's
# fresh identity will not match node2's namespace, triggering "access denied".
TEMP_CONFIG=$(mktemp /tmp/node1-trusted-restricted-XXXXXX.json)
cat > "$TEMP_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "trusted_peers": ["172.28.0.3"],
  "allowed_peer_keys": ["$NODE2_NS"]
}
EOCFG
chmod 644 "$TEMP_CONFIG"  # Container runs as chromatindb user, needs read access
log "Created restricted config at $TEMP_CONFIG"

# Stop the entire trusted topology and restart node1 with the restricted config.
# We need node1 fresh with the new config; node2 is not needed for this test.
log "Tearing down trusted topology for Part 3 reconfiguration..."
$COMPOSE_TRUSTED stop node2
$COMPOSE_TRUSTED rm -f node2
$COMPOSE_TRUSTED stop node1
$COMPOSE_TRUSTED rm -f node1

# Restart node1 manually with the restricted config mount.
# Use a fresh data volume (no named volume) so node1 gets a clean state.
# The test is about connection-level ACL rejection, not data continuity.
log "Starting node1 with restricted config (trusted_peers + allowed_peer_keys)..."
docker run -d --name chromatindb-test-node1 \
    --network chromatindb-test_test-net \
    --ip 172.28.0.2 \
    -v "$TEMP_CONFIG:/config/node1-trusted.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node1-trusted.json --data-dir /data --log-level debug
wait_healthy chromatindb-test-node1

# Start impostor on 172.28.0.3 (the trusted IP) with a fresh identity.
# The impostor generates a new identity key on startup (fresh data dir = new key).
# Its namespace will NOT match node2's namespace in allowed_peer_keys.
# Use a temp config with IP-based bootstrap (Compose DNS won't resolve for manual docker run).
IMPOSTOR_CONFIG=$(mktemp /tmp/impostor-XXXXXX.json)
cat > "$IMPOSTOR_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.28.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$IMPOSTOR_CONFIG"
log "Starting impostor node on 172.28.0.3 (trusted IP, wrong identity)..."
docker run -d --name "$IMPOSTOR_CONTAINER" \
    --network chromatindb-test_test-net \
    --ip 172.28.0.3 \
    -v "$IMPOSTOR_CONFIG:/config/impostor.json:ro" \
    chromatindb:test \
    run --config /config/impostor.json --data-dir /data --log-level debug

# Wait for the impostor to attempt connection and for node1 to process it
sleep 10

# Verify rejection -- strict pass condition, no soft fallback.
# The "access denied" log comes from peer_manager.cpp:286 when acl_.is_allowed()
# returns false because the impostor's namespace is not in allowed_peer_keys.
NODE1_POST_LOGS=$(docker logs chromatindb-test-node1 2>&1)

if echo "$NODE1_POST_LOGS" | grep -q "access denied"; then
    pass "Part 3: Impostor rejected with 'access denied' on trusted IP (wrong identity)"
else
    log "FAIL: Part 3 - Expected 'access denied' in node1 logs but not found"
    log "  Node1 logs (last 30 lines):"
    docker logs chromatindb-test-node1 2>&1 | tail -30 >&2
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "CRYPT-06: Trusted peer bypass verification PASSED"
    pass "  - Lightweight handshake used (TrustedHello, no KEM)"
    pass "  - Sync functional over trusted connection"
    pass "  - Wrong identity on trusted IP rejected (access denied)"
    exit 0
else
    fail "CRYPT-06: $FAILURES check(s) failed"
fi
