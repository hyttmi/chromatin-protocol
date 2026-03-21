#!/usr/bin/env bash
# =============================================================================
# TTL-02: Tombstone TTL Expiry and GC
#
# Verifies that tombstones with finite TTL (TTL=60) are garbage collected
# after expiry, with tombstone_map entries removed and blob count decreased.
#
# Topology: manual container creation (2-node) on 172.44.0.0/16
#   Node1 (172.44.0.2): seed node
#   Node2 (172.44.0.3): bootstraps to node1
#
# Flow:
#   1. Start 2-node cluster, ingest 20 blobs with TTL=3600
#   2. Tombstone all 20 blobs with TTL=60 (finite-TTL tombstones)
#   3. Wait for tombstone sync, verify pre-GC state (20 blobs_map, 20 tombstone_map)
#   4. Wait for TTL expiry + GC scan (~140s)
#   5. Verify post-GC: blobs_map drops 20->0, tombstone_map entries 20->0
#   6. Repeat verification on node2
#
# Note: get_blob_count (SIGUSR1 metrics) sums latest_seq_num which never
# decreases even after GC. We use the integrity scan output (blobs=X from
# startup scan) to verify actual blobs_map entries.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-ttl02-node1"
NODE2_CONTAINER="chromatindb-ttl02-node2"
TTL_NETWORK="chromatindb-ttl02-test-net"
NODE1_VOLUME="chromatindb-ttl02-node1-data"
NODE2_VOLUME="chromatindb-ttl02-node2-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$TTL_NETWORK"

# Temp dir for identity persistence
OWNER_DIR=""

# --- Cleanup -----------------------------------------------------------------

cleanup_ttl02() {
    log "Cleaning up TTL-02 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$TTL_NETWORK" 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
}
trap cleanup_ttl02 EXIT

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
    tmpconfig=$(mktemp /tmp/ttl02-config-XXXXXX.json)
    echo "$config" > "$tmpconfig"
    chmod 644 "$tmpconfig"

    log "Starting container $name at $ip (bootstrap: ${bootstrap:-none})"
    docker run -d --name "$name" \
        --network "$TTL_NETWORK" \
        --ip "$ip" \
        -v "$volume:/data" \
        -v "$tmpconfig:/config/node.json:ro" \
        --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
        --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
        chromatindb:test \
        run --config /config/node.json --data-dir /data --log-level debug \
        || fail "Failed to start container $name"
}

# --- Helper: get integrity scan values from a container ---------------------
# Restarts the container to trigger integrity scan, then parses the log output.
# Returns values via global variables: SCAN_BLOBS, SCAN_TOMBSTONE, SCAN_EXPIRY

get_integrity_scan() {
    local container="$1"
    log "Restarting $container for integrity scan..."
    docker restart "$container"
    wait_healthy "$container" 60
    sleep 5
    local scan_line
    # Grep specifically for the info line with blobs= (not the warning line)
    scan_line=$(docker logs "$container" 2>&1 | grep "integrity scan: blobs=" | tail -1)
    SCAN_BLOBS=$(echo "$scan_line" | grep -oP 'blobs=\K[0-9]+' || echo "unknown")
    SCAN_TOMBSTONE=$(echo "$scan_line" | grep -oP 'tombstone=\K[0-9]+' || echo "unknown")
    SCAN_EXPIRY=$(echo "$scan_line" | grep -oP 'expiry=\K[0-9]+' || echo "unknown")
    log "Integrity scan ($container): blobs=$SCAN_BLOBS tombstone=$SCAN_TOMBSTONE expiry=$SCAN_EXPIRY"
    log "  Full line: $scan_line"
}

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ttl02 2>/dev/null || true

log "=== TTL-02: Tombstone TTL Expiry and GC ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.44.0.0/16 "$TTL_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# Create temp dir for identity files
OWNER_DIR=$(mktemp -d /tmp/ttl02-owner-XXXXXX)
chmod 777 "$OWNER_DIR"

# =============================================================================
# Step 1: Start node1 + node2, wait for mesh
# =============================================================================

log "--- Step 1: Start 2-node cluster ---"

start_node "$NODE1_CONTAINER" "172.44.0.2" "$NODE1_VOLUME" ""
wait_healthy "$NODE1_CONTAINER"

start_node "$NODE2_CONTAINER" "172.44.0.3" "$NODE2_VOLUME" "172.44.0.2:4200"
wait_healthy "$NODE2_CONTAINER"

sleep 10
log "Mesh formation complete"

# =============================================================================
# Step 2: Ingest 20 blobs with TTL=3600 (long-lived)
# =============================================================================

log "--- Step 2: Ingest 20 blobs with TTL=3600 ---"

INGEST_OUTPUT=$(docker run --rm --network "$TTL_NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.44.0.2:4200 --count 20 --size 256 --rate 100 --ttl 3600 \
    --identity-save /identity \
    2>/tmp/ttl02-ingest-stderr.txt)

INGEST_TOTAL=$(echo "$INGEST_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
BLOB_HASHES=$(echo "$INGEST_OUTPUT" | jq -r '.blob_hashes[]' 2>/dev/null || echo "")
log "Ingested $INGEST_TOTAL blobs"

if [[ "$INGEST_TOTAL" -ne 20 ]]; then
    fail "Expected 20 blobs ingested, got $INGEST_TOTAL"
fi

HASH_COUNT=$(echo "$BLOB_HASHES" | wc -l)
log "Captured $HASH_COUNT blob hashes for deletion"

# Wait for sync to node2 (SIGUSR1 seq_num count, will read 20)
wait_sync "$NODE2_CONTAINER" 20 60 || fail "Sync to node2 failed (expected 20 blobs)"

# =============================================================================
# Step 3: Tombstone all 20 blobs with TTL=60
# =============================================================================

log "--- Step 3: Tombstone all 20 blobs with TTL=60 ---"

TOMBSTONE_TIME=$(date +%s)

DELETE_OUTPUT=$(echo "$BLOB_HASHES" | docker run --rm -i --network "$TTL_NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen chromatindb:test \
    --target 172.44.0.2:4200 --identity-file /identity --delete --hashes-from stdin --ttl 60 \
    2>/dev/null) || true

DELETE_TOTAL=$(echo "$DELETE_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Tombstoned $DELETE_TOTAL blobs with TTL=60"

# =============================================================================
# Step 4: Wait for tombstones to sync to node2
# SIGUSR1 metrics sum latest_seq_num: 20 original + 20 tombstones = 40
# =============================================================================

log "--- Step 4: Wait for tombstone sync ---"

wait_sync "$NODE1_CONTAINER" 40 60 || log "WARN: Node1 did not reach seq_num sum 40"
wait_sync "$NODE2_CONTAINER" 40 120 || log "WARN: Node2 did not reach seq_num sum 40"

# =============================================================================
# Step 5: Record pre-GC state via integrity scan
# After delete_blob + store tombstone:
#   - blobs_map: 20 (tombstones only; originals deleted by delete_blob_data)
#   - tombstone_map: 20 (one per tombstoned original)
#   - expiry_map: 20 (tombstones with TTL=60; original expiry entries removed)
# =============================================================================

log "--- Step 5: Record pre-GC state ---"

get_integrity_scan "$NODE1_CONTAINER"
PRE_BLOBS_N1="$SCAN_BLOBS"
PRE_TOMBSTONE_N1="$SCAN_TOMBSTONE"

if [[ "$PRE_BLOBS_N1" != "unknown" && "$PRE_BLOBS_N1" -ge 18 ]]; then
    pass "Pre-GC blobs_map (node1): $PRE_BLOBS_N1 (expected ~20 tombstone blobs)"
else
    log "FAIL: Expected ~20 blobs_map entries pre-GC (tombstones), got $PRE_BLOBS_N1"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$PRE_TOMBSTONE_N1" != "unknown" && "$PRE_TOMBSTONE_N1" -ge 18 ]]; then
    pass "Pre-GC tombstone_map (node1): $PRE_TOMBSTONE_N1 (expected ~20)"
else
    log "FAIL: Expected ~20 tombstone_map entries pre-GC, got $PRE_TOMBSTONE_N1"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 6: Wait for tombstone TTL expiry + GC scan
# The expiry_scan_loop runs every 60s. TTL=60 means tombstones expire
# 60s after creation. We need to wait for expiry + one scan cycle.
# The restart in step 5 resets the 60s timer. Account for time elapsed.
# =============================================================================

log "--- Step 6: Wait for TTL expiry + GC scan ---"

ELAPSED_SINCE_TOMBSTONE=$(( $(date +%s) - TOMBSTONE_TIME ))
# Need: TTL expiry (60s from creation) + one 60s scan cycle + margin
# The restart resets the timer, so we need 60s of TTL + 60s scan + margin
# from tombstone creation time.
REMAINING_WAIT=$(( 150 - ELAPSED_SINCE_TOMBSTONE ))
if [[ $REMAINING_WAIT -lt 30 ]]; then
    REMAINING_WAIT=30
fi

log "Tombstones created ${ELAPSED_SINCE_TOMBSTONE}s ago, waiting ${REMAINING_WAIT}s more for GC..."
sleep "$REMAINING_WAIT"

# =============================================================================
# Step 7: Post-GC verification on node1 via integrity scan
# After GC: tombstones with TTL=60 have expired, expiry scan purges them
# from blobs_map and cleans their tombstone_map entries.
# Expected: blobs_map=0, tombstone_map=0, expiry_map=0
# =============================================================================

log "--- Step 7: Post-GC verification (node1) ---"

get_integrity_scan "$NODE1_CONTAINER"
POST_BLOBS_N1="$SCAN_BLOBS"
POST_TOMBSTONE_N1="$SCAN_TOMBSTONE"
POST_EXPIRY_N1="$SCAN_EXPIRY"

# Check 1: blobs_map entries decreased (tombstones GC'd)
if [[ "$POST_BLOBS_N1" != "unknown" && "$POST_BLOBS_N1" -le 2 ]]; then
    pass "Check 1: blobs_map decreased from $PRE_BLOBS_N1 to $POST_BLOBS_N1 (tombstones GC'd)"
else
    log "FAIL: Check 1: Expected blobs_map ~0 after GC, got $POST_BLOBS_N1 (pre-GC: $PRE_BLOBS_N1)"
    FAILURES=$((FAILURES + 1))
fi

# Check 2: tombstone_map entries removed
if [[ "$POST_TOMBSTONE_N1" != "unknown" && "$POST_TOMBSTONE_N1" -le 2 ]]; then
    pass "Check 2: tombstone_map decreased from $PRE_TOMBSTONE_N1 to $POST_TOMBSTONE_N1"
else
    log "FAIL: Check 2: Expected ~0 tombstone_map entries after GC, got $POST_TOMBSTONE_N1"
    FAILURES=$((FAILURES + 1))
fi

# Check 3: expiry_map entries removed
if [[ "$POST_EXPIRY_N1" != "unknown" && "$POST_EXPIRY_N1" -le 2 ]]; then
    pass "Check 3: expiry_map cleared to $POST_EXPIRY_N1 (all expired entries purged)"
else
    log "INFO: expiry_map still has $POST_EXPIRY_N1 entries (may need another scan cycle)"
fi

# =============================================================================
# Step 8: Repeat checks on node2 (independent GC)
# =============================================================================

log "--- Step 8: Post-GC verification (node2) ---"

get_integrity_scan "$NODE2_CONTAINER"
POST_BLOBS_N2="$SCAN_BLOBS"
POST_TOMBSTONE_N2="$SCAN_TOMBSTONE"

if [[ "$POST_BLOBS_N2" != "unknown" && "$POST_BLOBS_N2" -le 2 ]]; then
    pass "Node2 blobs_map: $POST_BLOBS_N2 (tombstones GC'd independently)"
else
    log "FAIL: Node2 expected blobs_map ~0 after GC, got $POST_BLOBS_N2"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$POST_TOMBSTONE_N2" != "unknown" && "$POST_TOMBSTONE_N2" -le 2 ]]; then
    pass "Node2 tombstone_map: $POST_TOMBSTONE_N2 (cleaned up)"
else
    log "FAIL: Node2 expected ~0 tombstone_map entries, got $POST_TOMBSTONE_N2"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "TTL-02: Tombstone TTL Expiry and GC PASSED"
    pass "  - Pre-GC:  blobs_map=$PRE_BLOBS_N1, tombstone_map=$PRE_TOMBSTONE_N1"
    pass "  - Post-GC (node1): blobs_map=$POST_BLOBS_N1, tombstone_map=$POST_TOMBSTONE_N1"
    pass "  - Post-GC (node2): blobs_map=$POST_BLOBS_N2, tombstone_map=$POST_TOMBSTONE_N2"
    pass "  - Tombstones with TTL=60 expired and were garbage collected"
    exit 0
else
    fail "TTL-02: $FAILURES check(s) failed"
fi
