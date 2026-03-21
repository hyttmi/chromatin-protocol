#!/usr/bin/env bash
# =============================================================================
# OPS-02: SIGUSR1 Metrics Dump
#
# Verifies:
#   SIGUSR1 outputs a single metrics line containing all 15 expected fields
#   with correct values (blobs > 0 after ingest, uptime > 0).
#
# Topology: Single node (docker run). Simplest possible setup.
#
# Expected fields (15 total):
#   peers, connected_total, disconnected_total, blobs, storage,
#   syncs, ingests, rejections, rate_limited, cursor_hits, cursor_misses,
#   full_resyncs, quota_rejections, sync_rejections, uptime
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
OPS02_NETWORK="chromatindb-ops02-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$OPS02_NETWORK"

# Temp files and volumes
TEMP_NODE1_CONFIG=""
NODE1_VOLUME="chromatindb-ops02-node1-data"

# --- Cleanup -----------------------------------------------------------------

cleanup_ops02() {
    log "Cleaning up OPS-02 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker network rm "$OPS02_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
}
trap cleanup_ops02 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ops02 2>/dev/null || true

log "=== OPS-02: SIGUSR1 Metrics Dump ==="

FAILURES=0

# Create network and volume
docker network create --driver bridge --subnet 172.30.1.0/24 "$OPS02_NETWORK"
docker volume create "$NODE1_VOLUME"

# =============================================================================
# Step 1: Start single node, ingest blobs
# =============================================================================

log "--- Step 1: Start node and ingest blobs ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-ops02-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$OPS02_NETWORK" \
    --ip 172.30.1.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Ingest 10 blobs to produce some metrics
log "Ingesting 10 blobs..."
run_loadgen 172.30.1.2 --count 10 --size 1024 --rate 50 --ttl 3600 >/dev/null 2>&1

# =============================================================================
# Step 2: Send SIGUSR1 and read metrics
# =============================================================================

log "--- Step 2: Sending SIGUSR1 ---"

docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2

# Get the last metrics line from logs
METRICS_LINE=$(docker logs --tail 200 "$NODE1_CONTAINER" 2>&1 | grep "metrics:" | tail -1)

if [[ -z "$METRICS_LINE" ]]; then
    fail "No metrics line found in logs after SIGUSR1"
fi

log "Metrics line: $METRICS_LINE"

# =============================================================================
# Step 3: Verify all 15 expected fields are present
# =============================================================================

log "--- Step 3: Verifying metric fields ---"

EXPECTED_FIELDS=(
    "peers"
    "connected_total"
    "disconnected_total"
    "blobs"
    "storage"
    "syncs"
    "ingests"
    "rejections"
    "rate_limited"
    "cursor_hits"
    "cursor_misses"
    "full_resyncs"
    "quota_rejections"
    "sync_rejections"
    "uptime"
)

FOUND=0
MISSING=()

for field in "${EXPECTED_FIELDS[@]}"; do
    if echo "$METRICS_LINE" | grep -qP "${field}="; then
        FOUND=$((FOUND + 1))
    else
        MISSING+=("$field")
    fi
done

log "Fields found: $FOUND/${#EXPECTED_FIELDS[@]}"

if [[ ${#MISSING[@]} -eq 0 ]]; then
    pass "All 15 metric fields present"
else
    log "FAIL: Missing fields: ${MISSING[*]}"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 4: Verify blobs > 0 and uptime > 0
# =============================================================================

log "--- Step 4: Verifying field values ---"

BLOB_COUNT=$(echo "$METRICS_LINE" | grep -oP 'blobs=\K[0-9]+' || echo "0")
UPTIME_VAL=$(echo "$METRICS_LINE" | grep -oP 'uptime=\K[0-9]+' || echo "0")
INGESTS_VAL=$(echo "$METRICS_LINE" | grep -oP 'ingests=\K[0-9]+' || echo "0")

log "blobs=$BLOB_COUNT uptime=$UPTIME_VAL ingests=$INGESTS_VAL"

if [[ "$BLOB_COUNT" -gt 0 ]]; then
    pass "blobs > 0 ($BLOB_COUNT)"
else
    log "FAIL: blobs should be > 0 after ingesting 10 blobs"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$UPTIME_VAL" -gt 0 ]]; then
    pass "uptime > 0 ($UPTIME_VAL seconds)"
else
    log "FAIL: uptime should be > 0"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$INGESTS_VAL" -gt 0 ]]; then
    pass "ingests > 0 ($INGESTS_VAL)"
else
    log "FAIL: ingests should be > 0 after ingesting blobs"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "OPS-02: SIGUSR1 metrics dump PASSED"
    pass "  - All 15 metric fields present"
    pass "  - blobs=$BLOB_COUNT, uptime=$UPTIME_VAL, ingests=$INGESTS_VAL"
    exit 0
else
    fail "OPS-02: $FAILURES check(s) failed"
fi
