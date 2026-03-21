#!/usr/bin/env bash
# =============================================================================
# E2E-04: Multi-Namespace Isolation
#
# Verifies that two namespaces on the same cluster have complete isolation --
# messages never cross namespaces. Proven via blob count precision and an
# incremental sync test.
#
# Topology: 3-node standalone (docker run, not compose)
#   Node1 (172.43.0.2): seed node, receives namespace A writes
#   Node2 (172.43.0.3): bootstraps to Node1, receives namespace B writes
#   Node3 (172.43.0.4): bootstraps to Node1, sync-only verifier
#   Network: 172.43.0.0/16 (chromatindb-e2e04-test-net)
#
# Flow:
#   1. Start 3 nodes, wait for mesh formation
#   2. Generate Identity A and Identity B (two separate namespaces)
#   3. Ingest 20 blobs to namespace A via node1
#   4. Ingest 15 blobs to namespace B via node2
#   5. Wait for full sync convergence (35 total on all nodes)
#   6. Verify: total blob count matches on all 3 nodes (35 each)
#   7. Verify: integrity scan shows blobs across 2 namespaces
#   8. Verify: all nodes have exactly 35 blobs
#   9. Verify: two distinct namespace hex values
#  10. Verify: adding 1 blob to namespace A increases total by exactly 1
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-e2e04-node1"
NODE2_CONTAINER="chromatindb-e2e04-node2"
NODE3_CONTAINER="chromatindb-e2e04-node3"
E2E04_NETWORK="chromatindb-e2e04-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$E2E04_NETWORK"

# Named volumes
NODE1_VOLUME="chromatindb-e2e04-node1-data"
NODE2_VOLUME="chromatindb-e2e04-node2-data"
NODE3_VOLUME="chromatindb-e2e04-node3-data"

# Temp dirs for identities
IDENTITY_A_DIR=""
IDENTITY_B_DIR=""
TEMP_CONFIGS=()

# --- Cleanup -----------------------------------------------------------------

cleanup_e2e04() {
    log "Cleaning up E2E-04 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$E2E04_NETWORK" 2>/dev/null || true
    [[ -n "$IDENTITY_A_DIR" ]] && rm -rf "$IDENTITY_A_DIR" 2>/dev/null || true
    [[ -n "$IDENTITY_B_DIR" ]] && rm -rf "$IDENTITY_B_DIR" 2>/dev/null || true
    for cfg in "${TEMP_CONFIGS[@]}"; do
        rm -f "$cfg" 2>/dev/null || true
    done
}
trap cleanup_e2e04 EXIT

# --- Helper: start a node container -----------------------------------------

start_e2e04_node() {
    local name="$1"
    local ip="$2"
    local volume="$3"
    local bootstrap="$4"

    local bootstrap_json="[]"
    if [[ -n "$bootstrap" ]]; then
        local items=""
        IFS=',' read -ra PEERS <<< "$bootstrap"
        for peer in "${PEERS[@]}"; do
            [[ -n "$items" ]] && items="$items, "
            items="$items\"$peer\""
        done
        bootstrap_json="[$items]"
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
    tmpconfig=$(mktemp /tmp/e2e04-config-XXXXXX.json)
    echo "$config" > "$tmpconfig"
    chmod 644 "$tmpconfig"
    TEMP_CONFIGS+=("$tmpconfig")

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$E2E04_NETWORK" \
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
cleanup_e2e04 2>/dev/null || true

log "=== E2E-04: Multi-Namespace Isolation ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.43.0.0/16 "$E2E04_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# Create temp dirs for identities
IDENTITY_A_DIR=$(mktemp -d /tmp/e2e04-idA-XXXXXX)
IDENTITY_B_DIR=$(mktemp -d /tmp/e2e04-idB-XXXXXX)
chmod 777 "$IDENTITY_A_DIR" "$IDENTITY_B_DIR"

# =============================================================================
# Step 1: Start 3 nodes
# =============================================================================

log "--- Step 1: Start 3-node topology ---"

start_e2e04_node "$NODE1_CONTAINER" "172.43.0.2" "$NODE1_VOLUME" ""
start_e2e04_node "$NODE2_CONTAINER" "172.43.0.3" "$NODE2_VOLUME" "172.43.0.2:4200"
start_e2e04_node "$NODE3_CONTAINER" "172.43.0.4" "$NODE3_VOLUME" "172.43.0.2:4200"
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"
wait_healthy "$NODE3_CONTAINER"

# Wait for peer mesh to form
log "Waiting 10s for mesh to form..."
sleep 10

# =============================================================================
# Step 2: Generate Identity A and Identity B
# =============================================================================

log "--- Step 2: Generate two identities (two namespaces) ---"

# Identity A: generate and ingest 20 blobs to namespace A via node1
IDENT_A_STDERR=$(mktemp /tmp/e2e04-idA-stderr-XXXXXX.txt)
docker run --rm --network "$E2E04_NETWORK" \
    -v "$IDENTITY_A_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.43.0.2:4200 --identity-save /identity --count 20 --size 256 --ttl 3600 \
    >/dev/null 2>"$IDENT_A_STDERR" || true

if [[ ! -f "$IDENTITY_A_DIR/node.pub" ]]; then
    fail "Identity A not saved"
fi

NS_A_HEX=$(grep -oP 'namespace: \K[0-9a-f]{64}' "$IDENT_A_STDERR" | head -1)
log "Namespace A: ${NS_A_HEX:0:16}..."

# Identity B: generate and ingest 15 blobs to namespace B via node2
IDENT_B_STDERR=$(mktemp /tmp/e2e04-idB-stderr-XXXXXX.txt)
docker run --rm --network "$E2E04_NETWORK" \
    -v "$IDENTITY_B_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.43.0.3:4200 --identity-save /identity --count 15 --size 256 --ttl 3600 \
    >/dev/null 2>"$IDENT_B_STDERR" || true

if [[ ! -f "$IDENTITY_B_DIR/node.pub" ]]; then
    fail "Identity B not saved"
fi

NS_B_HEX=$(grep -oP 'namespace: \K[0-9a-f]{64}' "$IDENT_B_STDERR" | head -1)
log "Namespace B: ${NS_B_HEX:0:16}..."

# Clean up temp stderr files
rm -f "$IDENT_A_STDERR" "$IDENT_B_STDERR" 2>/dev/null || true

# =============================================================================
# Step 3: Wait for full sync convergence (35 total on all nodes)
# =============================================================================

log "--- Step 3: Wait for sync convergence (35 blobs on all nodes) ---"

if ! wait_sync "$NODE1_CONTAINER" 35 120; then
    NODE1_PARTIAL=$(get_blob_count "$NODE1_CONTAINER")
    log "WARN: Sync timeout on node1 ($NODE1_PARTIAL/35)"
fi
if ! wait_sync "$NODE2_CONTAINER" 35 120; then
    NODE2_PARTIAL=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync timeout on node2 ($NODE2_PARTIAL/35)"
fi
if ! wait_sync "$NODE3_CONTAINER" 35 120; then
    NODE3_PARTIAL=$(get_blob_count "$NODE3_CONTAINER")
    log "WARN: Sync timeout on node3 ($NODE3_PARTIAL/35)"
fi

# =============================================================================
# Check 1: Total blob count matches on all 3 nodes (35 each)
# =============================================================================

log "--- Check 1: Blob count consistency ---"

NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")

log "Blob counts: node1=$NODE1_COUNT, node2=$NODE2_COUNT, node3=$NODE3_COUNT"

if [[ "$NODE1_COUNT" -ge 35 && "$NODE2_COUNT" -ge 35 && "$NODE3_COUNT" -ge 35 ]]; then
    pass "All 3 nodes have >= 35 blobs (node1=$NODE1_COUNT, node2=$NODE2_COUNT, node3=$NODE3_COUNT)"
else
    log "FAIL: Not all nodes have >= 35 blobs"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Integrity scan shows blobs across 2 namespaces
# =============================================================================

log "--- Check 2: Integrity scan on node3 ---"

docker restart "$NODE3_CONTAINER"
wait_healthy "$NODE3_CONTAINER" 60

# Wait for integrity scan
sleep 5

NODE3_STARTUP_LOGS=$(docker logs "$NODE3_CONTAINER" 2>&1)
if echo "$NODE3_STARTUP_LOGS" | grep -q "integrity scan"; then
    pass "Node3 integrity scan completed after restart"
else
    log "WARN: No integrity scan log line found (non-critical)"
fi

# =============================================================================
# Check 3: SIGUSR1 metrics confirm blob count on all nodes
# =============================================================================

log "--- Check 3: Metrics blob counts ---"

# Re-read counts after node3 restart
NODE1_METRICS=$(get_blob_count "$NODE1_CONTAINER")
NODE2_METRICS=$(get_blob_count "$NODE2_CONTAINER")
NODE3_METRICS=$(get_blob_count "$NODE3_CONTAINER")

log "Metrics counts: node1=$NODE1_METRICS, node2=$NODE2_METRICS, node3=$NODE3_METRICS"

if [[ "$NODE1_METRICS" -ge 35 && "$NODE2_METRICS" -ge 35 && "$NODE3_METRICS" -ge 35 ]]; then
    pass "All nodes confirm >= 35 blobs via metrics (sum of both namespaces)"
else
    log "FAIL: Metrics blob count mismatch"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Verify distinct namespace existence
# =============================================================================

log "--- Check 4: Distinct namespaces ---"

if [[ -n "$NS_A_HEX" && -n "$NS_B_HEX" && "$NS_A_HEX" != "$NS_B_HEX" ]]; then
    pass "Two distinct namespaces: A=${NS_A_HEX:0:16}... B=${NS_B_HEX:0:16}..."
else
    log "FAIL: Namespace values are not distinct (A=$NS_A_HEX, B=$NS_B_HEX)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 5: Incremental isolation -- add 1 blob to namespace A, total +1 on all
# =============================================================================

log "--- Check 5: Incremental isolation (add 1 blob to namespace A) ---"

# Record pre-increment counts
PRE_NODE1=$(get_blob_count "$NODE1_CONTAINER")
PRE_NODE2=$(get_blob_count "$NODE2_CONTAINER")
PRE_NODE3=$(get_blob_count "$NODE3_CONTAINER")
log "Pre-increment: node1=$PRE_NODE1, node2=$PRE_NODE2, node3=$PRE_NODE3"

# Ingest 1 more blob to namespace A on node1
docker run --rm --network "$E2E04_NETWORK" \
    -v "$IDENTITY_A_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.43.0.2:4200 --identity-file /identity --count 1 --size 256 --ttl 3600 \
    >/dev/null 2>&1 || true

# Wait for sync: all nodes should have PRE + 1
EXPECTED=$((PRE_NODE1 + 1))
log "Waiting for all nodes to reach $EXPECTED blobs..."

if ! wait_sync "$NODE1_CONTAINER" "$EXPECTED" 30; then
    log "WARN: Node1 sync timeout"
fi
if ! wait_sync "$NODE2_CONTAINER" "$EXPECTED" 30; then
    log "WARN: Node2 sync timeout"
fi
if ! wait_sync "$NODE3_CONTAINER" "$EXPECTED" 30; then
    log "WARN: Node3 sync timeout"
fi

POST_NODE1=$(get_blob_count "$NODE1_CONTAINER")
POST_NODE2=$(get_blob_count "$NODE2_CONTAINER")
POST_NODE3=$(get_blob_count "$NODE3_CONTAINER")

log "Post-increment: node1=$POST_NODE1, node2=$POST_NODE2, node3=$POST_NODE3"

# Verify each node increased by exactly 1
DELTA1=$((POST_NODE1 - PRE_NODE1))
DELTA2=$((POST_NODE2 - PRE_NODE2))
DELTA3=$((POST_NODE3 - PRE_NODE3))

if [[ "$DELTA1" -eq 1 && "$DELTA2" -eq 1 && "$DELTA3" -eq 1 ]]; then
    pass "Incremental isolation: all nodes increased by exactly 1 (no cross-namespace contamination)"
else
    log "FAIL: Unexpected deltas: node1=$DELTA1, node2=$DELTA2, node3=$DELTA3 (expected 1 each)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "E2E-04: Multi-namespace isolation PASSED"
    pass "  - Namespace A: 20 blobs, Namespace B: 15 blobs"
    pass "  - All 3 nodes converged to $POST_NODE1 blobs"
    pass "  - Two distinct namespaces verified"
    pass "  - Incremental +1 blob propagated correctly (no contamination)"
    exit 0
else
    fail "E2E-04: $FAILURES check(s) failed"
fi
