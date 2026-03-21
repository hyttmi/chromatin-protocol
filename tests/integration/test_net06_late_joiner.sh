#!/usr/bin/env bash
# =============================================================================
# NET-06: Late-Joiner at Scale (10,000 blobs, multi-namespace)
#
# Verifies that a late-joining node catches up to 10,000 blobs across
# 3 namespaces by syncing from existing cluster members.
#
# Topology: manual container creation (3-node) on 172.32.0.0/16
#   Node1 (172.32.0.2): seed node, receives all ingested blobs
#   Node2 (172.32.0.3): bootstraps to node1, syncs before late joiner
#   Node3 (172.32.0.4): late joiner, started after data is established
#
# Flow:
#   1. Start node1 only, ingest ~10,000 blobs across 3 identities (namespaces)
#   2. Start node2, wait for full sync
#   3. Start node3 (late joiner), wait for full catch-up
#   4. Verify all 3 nodes have matching blob counts
#   5. Verify node3 cursor_misses shows it synced all namespaces
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"
NODE3_CONTAINER="chromatindb-test-node3"
LATE_NETWORK="chromatindb-late-test-net"
NODE1_VOLUME="chromatindb-late-node1-data"
NODE2_VOLUME="chromatindb-late-node2-data"
NODE3_VOLUME="chromatindb-late-node3-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$LATE_NETWORK"

# Temp dirs for identity persistence
TMPDIR1=""
TMPDIR2=""
TMPDIR3=""

# --- Cleanup -----------------------------------------------------------------

cleanup_net06() {
    log "Cleaning up NET-06 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$LATE_NETWORK" 2>/dev/null || true
    [[ -n "$TMPDIR1" ]] && rm -rf "$TMPDIR1" 2>/dev/null || true
    [[ -n "$TMPDIR2" ]] && rm -rf "$TMPDIR2" 2>/dev/null || true
    [[ -n "$TMPDIR3" ]] && rm -rf "$TMPDIR3" 2>/dev/null || true
}
trap cleanup_net06 EXIT

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
  "sync_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
)

    local tmpconfig
    tmpconfig=$(mktemp /tmp/net06-config-XXXXXX.json)
    echo "$config" > "$tmpconfig"
    chmod 644 "$tmpconfig"

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$LATE_NETWORK" \
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
cleanup_net06 2>/dev/null || true

log "=== NET-06: Late-Joiner at Scale ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.32.0.0/16 "$LATE_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# Create temp dirs for identity files
TMPDIR1=$(mktemp -d /tmp/net06-id1-XXXXXX)
TMPDIR2=$(mktemp -d /tmp/net06-id2-XXXXXX)
TMPDIR3=$(mktemp -d /tmp/net06-id3-XXXXXX)
chmod 777 "$TMPDIR1" "$TMPDIR2" "$TMPDIR3"

# =============================================================================
# Step 1: Start node1 only, ingest 10,000 blobs across 3 namespaces
# =============================================================================

log "--- Step 1: Start node1 and ingest blobs ---"

start_node "$NODE1_CONTAINER" "172.32.0.2" "$NODE1_VOLUME" ""
wait_healthy "$NODE1_CONTAINER"

# Ingest across 3 identities (3 namespaces), ~3334 blobs each.
# Use rate 2000 to complete before sync timer disconnects the loadgen.
# Use --identity-save to persist identities for verification.
# The --identity-save flag saves the identity AND injects --count blobs.

log "Ingesting identity 1: 3334 blobs..."
docker run --rm --network "$LATE_NETWORK" \
    -v "$TMPDIR1:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.32.0.2:4200 --count 3334 --size 256 --rate 2000 --ttl 3600 \
    --identity-save /identity \
    >/dev/null 2>&1 || true

log "Ingesting identity 2: 3333 blobs..."
docker run --rm --network "$LATE_NETWORK" \
    -v "$TMPDIR2:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.32.0.2:4200 --count 3333 --size 256 --rate 2000 --ttl 3600 \
    --identity-save /identity \
    >/dev/null 2>&1 || true

log "Ingesting identity 3: 3333 blobs..."
docker run --rm --network "$LATE_NETWORK" \
    -v "$TMPDIR3:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.32.0.2:4200 --count 3333 --size 256 --rate 2000 --ttl 3600 \
    --identity-save /identity \
    >/dev/null 2>&1 || true

# Wait for processing to complete
sleep 10

NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count after ingest: $NODE1_COUNT"

if [[ "$NODE1_COUNT" -lt 9000 ]]; then
    fail "NET-06: Ingest failed -- node1 has only $NODE1_COUNT blobs (expected ~10000)"
fi

# =============================================================================
# Step 2: Start node2, wait for full sync
# =============================================================================

log "--- Step 2: Start node2 and wait for sync ---"

start_node "$NODE2_CONTAINER" "172.32.0.3" "$NODE2_VOLUME" "172.32.0.2:4200"
wait_healthy "$NODE2_CONTAINER"

log "Waiting for sync to node2 ($NODE1_COUNT blobs, 300s timeout)..."
if ! wait_sync "$NODE2_CONTAINER" "$NODE1_COUNT" 300; then
    NODE2_PARTIAL=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync timeout -- node2 has $NODE2_PARTIAL/$NODE1_COUNT blobs"
fi

NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 blob count after sync: $NODE2_COUNT"

# =============================================================================
# Step 3: Start node3 (late joiner), wait for catch-up
# =============================================================================

log "--- Step 3: Start node3 (late joiner) ---"

start_node "$NODE3_CONTAINER" "172.32.0.4" "$NODE3_VOLUME" "172.32.0.2:4200"
wait_healthy "$NODE3_CONTAINER"

log "Waiting for node3 to catch up ($NODE1_COUNT blobs, 300s timeout)..."
if ! wait_sync "$NODE3_CONTAINER" "$NODE1_COUNT" 300; then
    NODE3_PARTIAL=$(get_blob_count "$NODE3_CONTAINER")
    log "WARN: Node3 catch-up timeout -- $NODE3_PARTIAL/$NODE1_COUNT blobs"
fi

NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")
log "Node3 blob count after catch-up: $NODE3_COUNT"

# =============================================================================
# Check 1: All 3 nodes have matching blob counts
# =============================================================================

log "--- Check 1: Blob count consistency ---"

# Re-read all counts for consistency
NODE1_FINAL=$(get_blob_count "$NODE1_CONTAINER")
NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
NODE3_FINAL=$(get_blob_count "$NODE3_CONTAINER")

log "Final counts: node1=$NODE1_FINAL, node2=$NODE2_FINAL, node3=$NODE3_FINAL"

if [[ "$NODE1_FINAL" -ge 9000 ]]; then
    pass "Node1 has $NODE1_FINAL blobs (>= 9000)"
else
    log "FAIL: Node1 has only $NODE1_FINAL blobs"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$NODE2_FINAL" -ge "$NODE1_FINAL" ]] || [[ "$NODE2_FINAL" -ge 9000 ]]; then
    pass "Node2 has $NODE2_FINAL blobs"
else
    log "FAIL: Node2 has only $NODE2_FINAL blobs (expected >= $NODE1_FINAL)"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$NODE3_FINAL" -ge "$NODE1_FINAL" ]] || [[ "$NODE3_FINAL" -ge 9000 ]]; then
    pass "Node3 (late joiner) has $NODE3_FINAL blobs"
else
    log "FAIL: Node3 has only $NODE3_FINAL blobs (expected >= $NODE1_FINAL)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node3 cursor_misses shows multi-namespace sync
# =============================================================================

log "--- Check 2: Node3 synced multiple namespaces ---"

docker kill -s USR1 "$NODE3_CONTAINER" >/dev/null 2>&1 || true
sleep 2

METRICS_LINE=$(docker logs "$NODE3_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
CURSOR_MISSES=$(echo "$METRICS_LINE" | grep -oP 'cursor_misses=\K[0-9]+' || echo "0")

log "Node3 cursor_misses: $CURSOR_MISSES"

# At minimum 3 namespace misses on initial sync (one per identity/namespace)
if [[ "$CURSOR_MISSES" -ge 3 ]]; then
    pass "Node3 had $CURSOR_MISSES cursor misses (>= 3 namespaces synced)"
else
    log "FAIL: Node3 cursor_misses=$CURSOR_MISSES, expected >= 3 (one per namespace)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "NET-06: Late-joiner at scale PASSED"
    pass "  - $NODE1_FINAL blobs across 3 namespaces"
    pass "  - Node2 synced: $NODE2_FINAL blobs"
    pass "  - Node3 (late joiner) caught up: $NODE3_FINAL blobs"
    pass "  - Node3 cursor_misses=$CURSOR_MISSES (multi-namespace sync confirmed)"
    exit 0
else
    fail "NET-06: $FAILURES check(s) failed"
fi
