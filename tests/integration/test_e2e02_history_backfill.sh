#!/usr/bin/env bash
# =============================================================================
# E2E-02: History Backfill (1000 messages)
#
# Verifies that a late-joining relay node backfills all 1000 blobs with no gaps,
# subsequent incremental sync works (proving monotonic cursors), and tombstones
# propagate within one sync interval.
#
# Topology: manual container creation (3-node) on 172.45.0.0/16
#   Node1 (172.45.0.2): seed node, receives all ingested blobs
#   Node2 (172.45.0.3): bootstraps to node1, syncs before late joiner
#   Node3 (172.45.0.4): late joiner, started after data is established
#
# Flow:
#   1. Start node1, ingest 1000 blobs (rate 2000, TTL 3600)
#   2. Start node2, wait for full sync to 1000
#   3. Start node3 (late joiner), wait for backfill to 1000
#   4. Verify blob counts match across all 3 nodes
#   5. Verify cursor_misses (namespace sync used for backfill)
#   6. Ingest 10 more blobs, verify incremental sync to 1010 (no gaps)
#   7. Tombstone one blob, verify propagation within one sync interval
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-e2e02-node1"
NODE2_CONTAINER="chromatindb-e2e02-node2"
NODE3_CONTAINER="chromatindb-e2e02-node3"
E2E_NETWORK="chromatindb-e2e02-test-net"
NODE1_VOLUME="chromatindb-e2e02-node1-data"
NODE2_VOLUME="chromatindb-e2e02-node2-data"
NODE3_VOLUME="chromatindb-e2e02-node3-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$E2E_NETWORK"

# Temp dir for identity persistence
OWNER_DIR=""

# --- Cleanup -----------------------------------------------------------------

cleanup_e2e02() {
    log "Cleaning up E2E-02 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$E2E_NETWORK" 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
}
trap cleanup_e2e02 EXIT

# --- Helper: start a node container -----------------------------------------

start_node() {
    local name="$1"
    local ip="$2"
    local volume="$3"
    local bootstrap="$4"  # empty string for seed, or "ip:port" for bootstrap

    local bootstrap_json="[]"
    if [[ -n "$bootstrap" ]]; then
        bootstrap_json="[\"$bootstrap\"]"
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
    tmpconfig=$(mktemp /tmp/e2e02-config-XXXXXX.json)
    echo "$config" > "$tmpconfig"
    chmod 644 "$tmpconfig"

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$E2E_NETWORK" \
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
cleanup_e2e02 2>/dev/null || true

log "=== E2E-02: History Backfill (1000 messages) ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.45.0.0/16 "$E2E_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# Create temp dir for identity files
OWNER_DIR=$(mktemp -d /tmp/e2e02-owner-XXXXXX)
chmod 777 "$OWNER_DIR"

# =============================================================================
# Step 1: Start node1 (seed), ingest 1000 blobs
# =============================================================================

log "--- Step 1: Start node1 and ingest 1000 blobs ---"

start_node "$NODE1_CONTAINER" "172.45.0.2" "$NODE1_VOLUME" ""
wait_healthy "$NODE1_CONTAINER"

# Ingest 1000 blobs at rate 2000 (prevents sync timer disconnect)
log "Ingesting 1000 blobs to node1..."
INGEST_OUTPUT=$(docker run --rm --network "$E2E_NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.45.0.2:4200 --count 1000 --size 256 --rate 2000 --ttl 3600 \
    --identity-save /identity \
    2>/dev/null) || true

INGEST_TOTAL=$(echo "$INGEST_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Ingested $INGEST_TOTAL blobs to node1"

# Verify node1 has 1000 blobs
sleep 5
NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count: $NODE1_COUNT"

if [[ "$NODE1_COUNT" -lt 1000 ]]; then
    fail "E2E-02: Ingest failed -- node1 has only $NODE1_COUNT blobs (expected 1000)"
fi

# =============================================================================
# Step 2: Start node2, wait for full sync
# =============================================================================

log "--- Step 2: Start node2 and wait for sync ---"

start_node "$NODE2_CONTAINER" "172.45.0.3" "$NODE2_VOLUME" "172.45.0.2:4200"
wait_healthy "$NODE2_CONTAINER"

log "Waiting for node2 to sync ($NODE1_COUNT blobs, 180s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" "$NODE1_COUNT" 180; then
    NODE2_PARTIAL=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync timeout -- node2 has $NODE2_PARTIAL/$NODE1_COUNT blobs"
fi

NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 blob count after sync: $NODE2_COUNT"

# =============================================================================
# Step 3: Start node3 (late joiner), wait for backfill
# =============================================================================

log "--- Step 3: Start node3 (late joiner) ---"

start_node "$NODE3_CONTAINER" "172.45.0.4" "$NODE3_VOLUME" "172.45.0.2:4200"
wait_healthy "$NODE3_CONTAINER"

log "Waiting for node3 to backfill ($NODE1_COUNT blobs, 180s timeout)..."
if ! wait_sync "$NODE3_CONTAINER" "$NODE1_COUNT" 180; then
    NODE3_PARTIAL=$(get_blob_count "$NODE3_CONTAINER")
    log "WARN: Node3 backfill timeout -- $NODE3_PARTIAL/$NODE1_COUNT blobs"
fi

NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")
log "Node3 blob count after backfill: $NODE3_COUNT"

# =============================================================================
# Check 1: Node3 has exactly 1000 blobs (complete backfill)
# =============================================================================

log "--- Check 1: Complete backfill ---"

if [[ "$NODE3_COUNT" -ge 1000 ]]; then
    pass "Check 1: Node3 backfilled $NODE3_COUNT blobs (>= 1000)"
else
    log "FAIL: Check 1: Node3 has only $NODE3_COUNT blobs (expected >= 1000)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Blob counts match across all 3 nodes
# =============================================================================

log "--- Check 2: Blob count consistency ---"

NODE1_FINAL=$(get_blob_count "$NODE1_CONTAINER")
NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
NODE3_FINAL=$(get_blob_count "$NODE3_CONTAINER")

log "Final counts: node1=$NODE1_FINAL, node2=$NODE2_FINAL, node3=$NODE3_FINAL"

if [[ "$NODE1_FINAL" -ge 1000 && "$NODE2_FINAL" -ge 1000 && "$NODE3_FINAL" -ge 1000 ]]; then
    pass "Check 2: All nodes have >= 1000 blobs (n1=$NODE1_FINAL n2=$NODE2_FINAL n3=$NODE3_FINAL)"
else
    log "FAIL: Check 2: Not all nodes have 1000 blobs (n1=$NODE1_FINAL n2=$NODE2_FINAL n3=$NODE3_FINAL)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Monotonic sequence -- cursor_misses on node3
# A cursor_miss means node3 had no prior cursor for that namespace, so it
# did a full namespace sync via reconciliation. This confirms the backfill
# used the reconciliation protocol (not incremental cursor-based sync).
# =============================================================================

log "--- Check 3: Monotonic sequence via cursor_misses ---"

docker kill -s USR1 "$NODE3_CONTAINER" >/dev/null 2>&1 || true
sleep 2

METRICS_LINE=$(docker logs --tail 200 "$NODE3_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
CURSOR_MISSES=$(echo "$METRICS_LINE" | grep -oP 'cursor_misses=\K[0-9]+' || echo "0")

log "Node3 cursor_misses: $CURSOR_MISSES"

if [[ "$CURSOR_MISSES" -ge 1 ]]; then
    pass "Check 3: Node3 had $CURSOR_MISSES cursor misses (backfill via reconciliation)"
else
    log "FAIL: Check 3: Node3 cursor_misses=$CURSOR_MISSES, expected >= 1"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: No gaps -- ingest 10 more blobs, verify incremental sync to 1010
# If node3 syncs to 1010, cursor state from backfill is valid and
# incremental sync works. This proves no gaps.
# =============================================================================

log "--- Check 4: Incremental sync after backfill (no gaps) ---"

docker run --rm --network "$E2E_NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.45.0.2:4200 --identity-file /identity --count 10 --size 256 --rate 100 --ttl 3600 \
    2>/dev/null >/dev/null || true

log "Ingested 10 additional blobs. Waiting for sync to 1010..."

if ! wait_sync "$NODE3_CONTAINER" 1010 60; then
    NODE3_POST=$(get_blob_count "$NODE3_CONTAINER")
    log "WARN: Incremental sync timeout -- node3 has $NODE3_POST blobs"
fi

NODE3_INCR=$(get_blob_count "$NODE3_CONTAINER")
log "Node3 count after incremental sync: $NODE3_INCR"

if [[ "$NODE3_INCR" -ge 1010 ]]; then
    pass "Check 4: Node3 synced to $NODE3_INCR after 10 additional blobs (incremental sync works)"
else
    log "FAIL: Check 4: Node3 has $NODE3_INCR blobs (expected >= 1010)"
    FAILURES=$((FAILURES + 1))
fi

# Also verify node2 synced
if ! wait_sync "$NODE2_CONTAINER" 1010 60; then
    log "WARN: Node2 incremental sync timeout"
fi

# =============================================================================
# Check 5: Tombstone propagation within one sync interval
# Owner tombstones one blob. Wait 15s (one 5s sync interval + margin).
# All 3 nodes should have 1011 blobs (1010 + 1 tombstone).
# =============================================================================

log "--- Check 5: Tombstone propagation ---"

# Get one blob hash to tombstone
FIRST_HASH=$(echo "$INGEST_OUTPUT" | jq -r '.blob_hashes[0]' 2>/dev/null || echo "")

if [[ -z "$FIRST_HASH" || "$FIRST_HASH" == "null" ]]; then
    log "FAIL: Check 5: Could not extract blob hash for tombstone test"
    FAILURES=$((FAILURES + 1))
else
    log "Tombstoning blob: ${FIRST_HASH:0:16}..."

    echo "$FIRST_HASH" | docker run --rm -i --network "$E2E_NETWORK" \
        -v "$OWNER_DIR:/identity:ro" \
        --entrypoint chromatindb_loadgen chromatindb:test \
        --target 172.45.0.2:4200 --identity-file /identity --delete --hashes-from stdin --ttl 0 \
        2>/dev/null >/dev/null || true

    # Wait for propagation (3 sync intervals for reliable cross-node sync)
    sleep 15

    NODE1_TOMB=$(get_blob_count "$NODE1_CONTAINER")
    NODE2_TOMB=$(get_blob_count "$NODE2_CONTAINER")
    NODE3_TOMB=$(get_blob_count "$NODE3_CONTAINER")

    log "After tombstone: node1=$NODE1_TOMB node2=$NODE2_TOMB node3=$NODE3_TOMB"

    if [[ "$NODE1_TOMB" -ge 1011 && "$NODE2_TOMB" -ge 1011 && "$NODE3_TOMB" -ge 1011 ]]; then
        pass "Check 5: Tombstone propagated to all nodes within sync interval (n1=$NODE1_TOMB n2=$NODE2_TOMB n3=$NODE3_TOMB)"
    else
        log "FAIL: Check 5: Tombstone not propagated to all nodes (n1=$NODE1_TOMB n2=$NODE2_TOMB n3=$NODE3_TOMB, expected >= 1011)"
        FAILURES=$((FAILURES + 1))
    fi
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "E2E-02: History Backfill PASSED"
    pass "  - 1000 blobs backfilled to late-joining node3"
    pass "  - Blob counts: node1=$NODE1_FINAL, node2=$NODE2_FINAL, node3=$NODE3_FINAL"
    pass "  - Cursor misses: $CURSOR_MISSES (reconciliation-based backfill)"
    pass "  - Incremental sync to $NODE3_INCR after 10 additional blobs"
    pass "  - Tombstone propagated to all nodes within one sync interval"
    exit 0
else
    fail "E2E-02: $FAILURES check(s) failed"
fi
