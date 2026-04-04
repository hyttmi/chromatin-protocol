#!/usr/bin/env bash
# =============================================================================
# OPS-01: SIGHUP Config Reload (Rate Limit)
#
# Verifies:
#   Phase 1: With rate_limit=0 (unlimited), 50 blobs are ingested successfully.
#   Phase 2: After SIGHUP with rate_limit=1024 B/s, high-rate ingest triggers
#     rate limiting -- peer disconnected, rate_limited counter incremented.
#
# Topology: 2-node standalone (docker run, not compose)
#   Node1 (172.30.0.2): writable config mount, receives ingest
#   Node2 (172.30.0.3): bootstraps to Node1
#   Network: 172.30.0.0/16 (chromatindb-ops01-test-net)
#
# Method: Volume-mounted writable config (NOT :ro) so host edits propagate.
#   SIGHUP via `docker kill -s HUP`. Verification via log inspection + SIGUSR1
#   metrics.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-ops01-node1"
NODE2_CONTAINER="chromatindb-ops01-node2"
OPS01_NETWORK="chromatindb-ops01-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$OPS01_NETWORK"

# Temp files and volumes
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
NODE1_VOLUME="chromatindb-ops01-node1-data"
NODE2_VOLUME="chromatindb-ops01-node2-data"

# --- Cleanup -----------------------------------------------------------------

cleanup_ops01() {
    log "Cleaning up OPS-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$OPS01_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
}
trap cleanup_ops01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ops01 2>/dev/null || true

log "=== OPS-01: SIGHUP Config Reload (Rate Limit) ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.30.0.0/16 "$OPS01_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# =============================================================================
# Phase 1: Start nodes with unlimited rate, ingest 50 blobs
# =============================================================================

log "--- Phase 1: Unlimited rate ingest ---"

# Node1 config: no rate limit (0 = disabled), writable mount
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-ops01-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "rate_limit_bytes_per_sec": 0,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Node2 config: bootstrap to Node1
TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-ops01-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.30.0.2:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Start Node1 with writable config mount (no :ro)
docker run -d --name "$NODE1_CONTAINER" \
    --network "$OPS01_NETWORK" \
    --ip 172.30.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Start Node2
docker run -d --name "$NODE2_CONTAINER" \
    --network "$OPS01_NETWORK" \
    --ip 172.30.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# Ingest 50 blobs at moderate rate (unlimited, should all succeed)
log "Ingesting 50 blobs to Node1 (unlimited rate)..."
run_loadgen 172.30.0.2 --count 50 --size 1024 --rate 100 --ttl 3600 >/dev/null 2>&1

# Verify all 50 arrived
NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count after unlimited ingest: $NODE1_COUNT"

if [[ "$NODE1_COUNT" -ge 50 ]]; then
    pass "Phase 1: All 50 blobs ingested with unlimited rate"
else
    log "FAIL: Expected >= 50 blobs, got $NODE1_COUNT"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 2: SIGHUP to set tight rate limit
# =============================================================================

log "--- Phase 2: SIGHUP to set rate_limit=1024 B/s ---"

# Edit Node1 config to set tight rate limit
# rate_limit_bytes_per_sec >= 1024 (validation minimum)
# rate_limit_burst >= rate_limit_bytes_per_sec (validation requirement)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "rate_limit_bytes_per_sec": 1024,
  "rate_limit_burst": 2048,
  "full_resync_interval": 9999
}
EOCFG

# Send SIGHUP to Node1
log "Sending SIGHUP to Node1..."
docker kill -s HUP "$NODE1_CONTAINER"

# Wait for config reload
sleep 3

# Check Node1 logs for reload confirmation
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS" | grep -q "SIGHUP received"; then
    pass "SIGHUP received by Node1"
else
    log "FAIL: SIGHUP not received"
    echo "$NODE1_LOGS" | tail -10 >&2
    FAILURES=$((FAILURES + 1))
fi

if echo "$NODE1_LOGS" | grep -q "config reload"; then
    pass "Config reload processed"
else
    log "FAIL: No config reload in logs"
    FAILURES=$((FAILURES + 1))
fi

if echo "$NODE1_LOGS" | grep -q "rate_limit=1024B/s"; then
    pass "Rate limit updated to 1024 B/s"
else
    log "FAIL: Expected 'rate_limit=1024B/s' in logs"
    echo "$NODE1_LOGS" | grep "rate_limit" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 3: High-rate ingest should trigger rate limiting
# =============================================================================

log "--- Phase 3: High-rate ingest (should be rate-limited) ---"

# Send high-rate large blobs: 10KB each at rate 50/s = 500KB/s >> 1KB/s limit
# This should exceed the token bucket immediately
run_loadgen 172.30.0.2 --size 10240 --rate 50 --count 100 --ttl 3600 >/dev/null 2>&1 || true

# Wait for rate limit disconnects to be logged
sleep 3

# Check for rate limiting in logs
NODE1_LOGS_AFTER=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS_AFTER" | grep -q "rate limit exceeded"; then
    pass "Rate limit exceeded detected in logs"
else
    log "FAIL: Expected 'rate limit exceeded' in logs"
    echo "$NODE1_LOGS_AFTER" | grep -i "rate" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# Check rate_limited counter via SIGUSR1
docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2

NODE1_METRICS=$(docker logs --tail 200 "$NODE1_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
RATE_LIMITED=$(echo "$NODE1_METRICS" | grep -oP 'rate_limited=\K[0-9]+' || echo "0")
log "rate_limited counter: $RATE_LIMITED"

if [[ "$RATE_LIMITED" -gt 0 ]]; then
    pass "rate_limited counter incremented ($RATE_LIMITED)"
else
    log "FAIL: rate_limited counter is 0 (expected > 0)"
    echo "$NODE1_METRICS" >&2
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "OPS-01: SIGHUP config reload (rate limit) PASSED"
    pass "  - Phase 1: 50 blobs ingested with unlimited rate"
    pass "  - Phase 2: SIGHUP applied rate_limit=1024B/s"
    pass "  - Phase 3: High-rate ingest triggered rate limiting (rate_limited=$RATE_LIMITED)"
    exit 0
else
    fail "OPS-01: $FAILURES check(s) failed"
fi
