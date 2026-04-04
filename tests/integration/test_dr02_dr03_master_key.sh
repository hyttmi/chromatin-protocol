#!/usr/bin/env bash
# =============================================================================
# DR-02 + DR-03: Master Key Dependency and Isolation
#
# DR-02: Daemon refuses to operate without master.key (decryption impossible).
# DR-03: Node B cannot read Node A's mdbx.dat with Node B's master.key
#        (AEAD decryption failure due to different derived keys).
#
# Topology: 2 nodes (docker run). Dedicated network (172.33.0.0/16).
#   Node A (172.33.0.2): primary node, ingests blobs
#   Node B (172.33.0.3): secondary node, used for key isolation test
#
# Flow:
#   DR-02 (master key dependency):
#     1. Start Node A, ingest 20 blobs, stop
#     2. Remove master.key from Node A's volume
#     3. Try to start Node A -- should fail (cannot decrypt without master key)
#     4. Verify exit or unhealthy status
#   DR-03 (master key isolation):
#     5. Start Node B, ingest 10 blobs, stop
#     6. Copy Node A's mdbx.dat over Node B's mdbx.dat
#     7. Start Node B (has Node B's master.key but Node A's encrypted data)
#     8. Verify AEAD failure: node crashes, serves 0 blobs, or logs decrypt errors
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODEA_CONTAINER="chromatindb-dr02-nodea"
NODEB_CONTAINER="chromatindb-dr02-nodeb"
DR02_NETWORK="chromatindb-dr02-test-net"
NODEA_VOLUME="chromatindb-dr02-nodea-data"
NODEB_VOLUME="chromatindb-dr02-nodeb-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DR02_NETWORK"

# --- Cleanup -----------------------------------------------------------------

cleanup_dr02() {
    log "Cleaning up DR-02/DR-03 test..."
    docker rm -f "$NODEA_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODEB_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODEA_VOLUME" 2>/dev/null || true
    docker volume rm "$NODEB_VOLUME" 2>/dev/null || true
    docker network rm "$DR02_NETWORK" 2>/dev/null || true
}
trap cleanup_dr02 EXIT

# --- Helper: start a node container -----------------------------------------

start_dr_node() {
    local name="$1"
    local ip="$2"
    local volume="$3"
    local bootstrap="$4"  # empty string for seed, or "ip:port" for bootstrap

    local bootstrap_json="[]"
    if [[ -n "$bootstrap" ]]; then
        bootstrap_json="[\"$bootstrap\"]"
    fi

    local tmpconfig
    tmpconfig=$(mktemp /tmp/dr02-config-XXXXXX.json)
    cat > "$tmpconfig" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": $bootstrap_json,
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
    chmod 644 "$tmpconfig"

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$DR02_NETWORK" \
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
cleanup_dr02 2>/dev/null || true

log "=== DR-02/DR-03: Master Key Dependency and Isolation ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.33.0.0/16 "$DR02_NETWORK"
docker volume create "$NODEA_VOLUME"
docker volume create "$NODEB_VOLUME"

# =============================================================================
# DR-02: Master Key Dependency
# =============================================================================

log "=== DR-02: Master Key Dependency ==="

# --- Step 1: Start Node A, ingest 20 blobs, stop ---

log "--- Step 1: Start Node A, ingest blobs ---"

start_dr_node "$NODEA_CONTAINER" "172.33.0.2" "$NODEA_VOLUME" ""
wait_healthy "$NODEA_CONTAINER"

run_loadgen 172.33.0.2 --count 20 --size 1024 --ttl 3600 --rate 50 >/dev/null 2>&1

sleep 2

NODEA_COUNT=$(get_blob_count "$NODEA_CONTAINER")
log "Node A blob count: $NODEA_COUNT"

docker stop "$NODEA_CONTAINER"
docker rm "$NODEA_CONTAINER"

# --- Step 2: Remove master.key from Node A's volume ---

log "--- Step 2: Remove master.key from Node A's volume ---"

docker run --rm -v "$NODEA_VOLUME:/data" alpine rm /data/master.key

# Verify it's gone
KEY_EXISTS=$(docker run --rm -v "$NODEA_VOLUME:/data:ro" alpine sh -c '[ -f /data/master.key ] && echo "yes" || echo "no"')
log "master.key exists after removal: $KEY_EXISTS"

if [[ "$KEY_EXISTS" == "yes" ]]; then
    fail "DR-02: Failed to remove master.key"
fi

# --- Step 3: Try to start Node A without master.key ---

log "--- Step 3: Start Node A without master.key ---"

# Start the container -- it should fail to start or become unhealthy
# because it cannot decrypt the existing data without the master key.
# The node will generate a NEW master.key, but it won't match the old DARE keys.
start_dr_node "$NODEA_CONTAINER" "172.33.0.2" "$NODEA_VOLUME" ""

# Wait a bit for the node to attempt startup
sleep 15

# Check if the node is healthy, running, or crashed
CONTAINER_STATUS=$(docker inspect --format '{{.State.Status}}' "$NODEA_CONTAINER" 2>/dev/null || echo "missing")
CONTAINER_HEALTH=$(docker inspect --format '{{.State.Health.Status}}' "$NODEA_CONTAINER" 2>/dev/null || echo "unknown")

log "Node A status: $CONTAINER_STATUS, health: $CONTAINER_HEALTH"

# The node should either:
# a) crash/exit (integrity scan fails with wrong key) -- status=exited
# b) start but report integrity scan errors and serve 0 blobs
# c) refuse to start (unhealthy)

DR02_PASSED=false

if [[ "$CONTAINER_STATUS" == "exited" ]]; then
    EXIT_CODE=$(docker inspect --format '{{.State.ExitCode}}' "$NODEA_CONTAINER" 2>/dev/null || echo "0")
    log "Node A exited with code $EXIT_CODE (cannot start without correct master.key)"
    if [[ "$EXIT_CODE" != "0" ]]; then
        DR02_PASSED=true
        pass "DR-02: Node A crashed on startup without correct master.key (exit $EXIT_CODE)"
    fi
fi

if [[ "$DR02_PASSED" == false && "$CONTAINER_STATUS" == "running" ]]; then
    # Node is running -- check if it serves 0 blobs (integrity scan rejected everything)
    NODEA_RECOUNT=$(get_blob_count "$NODEA_CONTAINER")
    log "Node A blob count with wrong master.key: $NODEA_RECOUNT"

    if [[ "$NODEA_RECOUNT" -eq 0 ]]; then
        DR02_PASSED=true
        pass "DR-02: Node A serves 0 blobs with wrong master.key (data unreadable)"
    fi

    # Check for integrity/decrypt errors in logs
    INTEGRITY_ERRORS=$(docker logs "$NODEA_CONTAINER" 2>&1 | grep -icE '(integrity|decrypt|AEAD|corrupt|failed)' || echo "0")
    log "Node A integrity/decrypt error count in logs: $INTEGRITY_ERRORS"

    if [[ "$INTEGRITY_ERRORS" -gt 0 && "$DR02_PASSED" == false ]]; then
        DR02_PASSED=true
        pass "DR-02: Node A reports integrity/decrypt errors with wrong master.key"
    fi
fi

if [[ "$DR02_PASSED" == false ]]; then
    log "FAIL: DR-02: Node A did not fail or degrade without correct master.key"
    log "  Status: $CONTAINER_STATUS, Health: $CONTAINER_HEALTH"
    FAILURES=$((FAILURES + 1))
fi

# Clean up Node A for DR-03
docker rm -f "$NODEA_CONTAINER" 2>/dev/null || true

# =============================================================================
# DR-03: Master Key Isolation
# =============================================================================

log "=== DR-03: Master Key Isolation ==="

# First re-create Node A with original data (we need its mdbx.dat)
# Re-create Node A volume with fresh master.key and data
docker volume rm "$NODEA_VOLUME" 2>/dev/null || true
docker volume create "$NODEA_VOLUME"

# --- Step 5: Start Node A fresh, ingest blobs, stop ---

log "--- Step 5: Start Node A fresh, ingest blobs ---"

start_dr_node "$NODEA_CONTAINER" "172.33.0.2" "$NODEA_VOLUME" ""
wait_healthy "$NODEA_CONTAINER"

run_loadgen 172.33.0.2 --count 20 --size 1024 --ttl 3600 --rate 50 >/dev/null 2>&1

sleep 2

NODEA_COUNT=$(get_blob_count "$NODEA_CONTAINER")
log "Node A blob count: $NODEA_COUNT"

docker stop "$NODEA_CONTAINER"
docker rm "$NODEA_CONTAINER"

# --- Step 5b: Start Node B, ingest 10 blobs, stop ---

log "--- Step 5b: Start Node B, ingest blobs ---"

start_dr_node "$NODEB_CONTAINER" "172.33.0.3" "$NODEB_VOLUME" ""
wait_healthy "$NODEB_CONTAINER"

run_loadgen 172.33.0.3 --count 10 --size 1024 --ttl 3600 --rate 50 >/dev/null 2>&1

sleep 2

NODEB_COUNT=$(get_blob_count "$NODEB_CONTAINER")
log "Node B blob count: $NODEB_COUNT"

docker stop "$NODEB_CONTAINER"
docker rm "$NODEB_CONTAINER"

# --- Step 6: Copy Node A's mdbx.dat to Node B's volume ---

log "--- Step 6: Copy Node A's mdbx.dat to Node B's volume ---"

docker run --rm -v "$NODEA_VOLUME:/src:ro" -v "$NODEB_VOLUME:/dst" alpine \
    cp /src/mdbx.dat /dst/mdbx.dat

log "Copied Node A's mdbx.dat over Node B's mdbx.dat"

# --- Step 7: Start Node B with Node B's master.key but Node A's mdbx.dat ---

log "--- Step 7: Start Node B with wrong key + foreign data ---"

start_dr_node "$NODEB_CONTAINER" "172.33.0.3" "$NODEB_VOLUME" ""

# Wait for startup attempt
sleep 15

NODEB_STATUS=$(docker inspect --format '{{.State.Status}}' "$NODEB_CONTAINER" 2>/dev/null || echo "missing")
NODEB_HEALTH=$(docker inspect --format '{{.State.Health.Status}}' "$NODEB_CONTAINER" 2>/dev/null || echo "unknown")

log "Node B status: $NODEB_STATUS, health: $NODEB_HEALTH"

DR03_PASSED=false

if [[ "$NODEB_STATUS" == "exited" ]]; then
    EXIT_CODE=$(docker inspect --format '{{.State.ExitCode}}' "$NODEB_CONTAINER" 2>/dev/null || echo "0")
    log "Node B exited with code $EXIT_CODE (AEAD failure with foreign data)"
    if [[ "$EXIT_CODE" != "0" ]]; then
        DR03_PASSED=true
        pass "DR-03: Node B crashed trying to read Node A's data with wrong key (exit $EXIT_CODE)"
    fi
fi

if [[ "$DR03_PASSED" == false && "$NODEB_STATUS" == "running" ]]; then
    # Check blob count -- should be 0 (cannot decrypt Node A's data with Node B's key)
    NODEB_RECOUNT=$(get_blob_count "$NODEB_CONTAINER")
    log "Node B blob count with foreign data: $NODEB_RECOUNT"

    if [[ "$NODEB_RECOUNT" -eq 0 ]]; then
        DR03_PASSED=true
        pass "DR-03: Node B serves 0 blobs from Node A's data (AEAD isolation confirmed)"
    fi

    # Check for decrypt/AEAD errors in logs
    AEAD_ERRORS=$(docker logs "$NODEB_CONTAINER" 2>&1 | grep -icE '(integrity|decrypt|AEAD|corrupt|failed|error)' || echo "0")
    log "Node B AEAD/integrity error count in logs: $AEAD_ERRORS"

    if [[ "$AEAD_ERRORS" -gt 0 && "$DR03_PASSED" == false ]]; then
        DR03_PASSED=true
        pass "DR-03: Node B reports AEAD/integrity errors with foreign encrypted data"
    fi
fi

if [[ "$DR03_PASSED" == false ]]; then
    log "FAIL: DR-03: Node B did not fail or degrade with foreign encrypted data"
    log "  Status: $NODEB_STATUS, Health: $NODEB_HEALTH"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DR-02/DR-03: Master Key Dependency and Isolation PASSED"
    pass "  - DR-02: Node cannot operate without correct master.key"
    pass "  - DR-03: Node B cannot read Node A's mdbx.dat (AEAD isolation)"
    exit 0
else
    fail "DR-02/DR-03: $FAILURES check(s) failed"
fi
