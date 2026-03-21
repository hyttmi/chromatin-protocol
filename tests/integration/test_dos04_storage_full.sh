#!/usr/bin/env bash
# =============================================================================
# DOS-04: Storage Full Signaling & Recovery
#
# Verifies:
#   1. StorageFull is signaled when max_storage_bytes is exceeded
#   2. SIGHUP with a higher max_storage_bytes limit recovers the node
#   3. After recovery, new writes succeed and sync to peers
#
# Topology: 2-node standalone. Dedicated network (172.39.0.0/16).
#   Node1 (172.39.0.2): max_storage_bytes=2097152 (2 MiB tight limit), writable config
#   Node2 (172.39.0.3): bootstraps to Node1
#
# Method:
#   1. Ingest blobs until storage fills (loadgen auto-stops on StorageFull)
#   2. Verify rejections in metrics and storage_full evidence in logs
#   3. Verify Node2 received only the blobs Node1 accepted
#   4. SIGHUP Node1 with max_storage_bytes=10485760 (10 MiB)
#   5. Ingest more blobs, verify they succeed and sync to Node2
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"
DOS04_NETWORK="chromatindb-dos04-test-net"
NODE1_VOLUME="chromatindb-dos04-node1-data"
NODE2_VOLUME="chromatindb-dos04-node2-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DOS04_NETWORK"

# Temp config files
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""

# --- Cleanup -----------------------------------------------------------------

cleanup_dos04() {
    log "Cleaning up DOS-04 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker network rm "$DOS04_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
}
trap cleanup_dos04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dos04 2>/dev/null || true

log "=== DOS-04: Storage Full Signaling & Recovery ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.39.0.0/16 "$DOS04_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"

# =============================================================================
# Step 1: Start Node1 with tight storage limit (1 MiB)
# =============================================================================

log "--- Step 1: Starting 2-node topology with tight storage ---"

# mdbx file starts at ~1 MiB even empty, so 2 MiB gives ~1 MiB headroom
# At ~4KB/blob (data + encoding), this fits ~200 blobs before filling
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-dos04-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "max_storage_bytes": 2097152
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-dos04-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["172.39.0.2:4200"],
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Node1 with writable config (no :ro)
docker run -d --name "$NODE1_CONTAINER" \
    --network "$DOS04_NETWORK" \
    --ip 172.39.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

# Node2 with read-only config
docker run -d --name "$NODE2_CONTAINER" \
    --network "$DOS04_NETWORK" \
    --ip 172.39.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 8

# =============================================================================
# Step 2: Ingest blobs until storage fills
# =============================================================================

log "--- Step 2: Ingesting blobs to fill 1 MiB storage ---"

# At 4096 bytes/blob + encoding overhead, ~200 blobs should fill the ~1 MiB headroom.
# Loadgen auto-stops when StorageFull is received.
LOADGEN_OUTPUT=$(docker run --rm --network "$DOS04_NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target "172.39.0.2:4200" --count 500 --size 4096 --rate 50 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

LOADGEN_TOTAL=$(echo "$LOADGEN_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
LOADGEN_ERRORS=$(echo "$LOADGEN_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Loadgen sent: $LOADGEN_TOTAL blobs (errors: $LOADGEN_ERRORS)"

# =============================================================================
# Check 1: Storage full evidence in logs/metrics
# =============================================================================

log "--- Check 1: Storage full evidence ---"

# Get metrics
docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)

# Check for storage full log message
if echo "$NODE1_LOGS" | grep -qi "storage.*full\|storage.*capacity.*exceeded\|StorageFull"; then
    pass "Storage full evidence found in Node1 logs"
else
    log "FAIL: No storage full evidence in Node1 logs"
    FAILURES=$((FAILURES + 1))
fi

# Check rejections in metrics (use word boundary to avoid matching quota_rejections/sync_rejections)
NODE1_METRICS=$(echo "$NODE1_LOGS" | grep "metrics:" | tail -1)
REJECTIONS=$(echo "$NODE1_METRICS" | grep -oP '(?<= )rejections=\K[0-9]+' || echo "0")
log "Node1 rejections: $REJECTIONS"

if [[ "$REJECTIONS" -gt 0 ]]; then
    pass "Node1 has rejections > 0 ($REJECTIONS)"
else
    log "FAIL: Expected rejections > 0, got $REJECTIONS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node1 accepted fewer than all requested blobs
# =============================================================================

log "--- Check 2: Node1 accepted fewer than 500 blobs ---"

NODE1_BLOBS=$(echo "$NODE1_METRICS" | grep -oP 'blobs=\K[0-9]+' || echo "0")
log "Node1 blob count: $NODE1_BLOBS"

if [[ "$NODE1_BLOBS" -gt 0 && "$NODE1_BLOBS" -lt 500 ]]; then
    pass "Node1 accepted some but not all blobs ($NODE1_BLOBS < 500)"
else
    log "FAIL: Expected 0 < blobs < 500, got $NODE1_BLOBS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Sync suppression -- Node2 has <= Node1's blobs
# =============================================================================

log "--- Check 3: Sync suppression ---"

# Wait for any in-flight sync to complete
sleep 10
NODE2_BLOBS=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 blob count: $NODE2_BLOBS (Node1: $NODE1_BLOBS)"

if [[ "$NODE2_BLOBS" -le "$NODE1_BLOBS" ]]; then
    pass "Sync suppression: Node2 ($NODE2_BLOBS) <= Node1 ($NODE1_BLOBS)"
else
    log "FAIL: Node2 ($NODE2_BLOBS) > Node1 ($NODE1_BLOBS) -- unexpected"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 5: Recovery -- SIGHUP with higher limit
# =============================================================================

log "--- Step 5: Recovery via SIGHUP (raising max_storage_bytes to 10 MiB) ---"

cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "max_storage_bytes": 10485760
}
EOCFG

docker kill -s HUP "$NODE1_CONTAINER"
sleep 3

# Verify SIGHUP received
NODE1_LOGS_AFTER=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS_AFTER" | grep -q "SIGHUP received"; then
    pass "SIGHUP received by Node1"
else
    log "FAIL: SIGHUP not received"
    FAILURES=$((FAILURES + 1))
fi

if echo "$NODE1_LOGS_AFTER" | grep -q "max_storage_bytes=10485760"; then
    pass "max_storage_bytes reloaded to 10485760"
else
    log "FAIL: max_storage_bytes reload not confirmed in logs"
    echo "$NODE1_LOGS_AFTER" | grep "max_storage" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 6: Post-recovery ingest
# =============================================================================

log "--- Step 6: Post-recovery ingest (20 blobs) ---"

RECOVERY_OUTPUT=$(docker run --rm --network "$DOS04_NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target "172.39.0.2:4200" --count 20 --size 4096 --rate 20 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

RECOVERY_TOTAL=$(echo "$RECOVERY_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
RECOVERY_ERRORS=$(echo "$RECOVERY_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Recovery ingest: $RECOVERY_TOTAL blobs sent (errors: $RECOVERY_ERRORS)"

# Verify Node1 blob count increased
NODE1_BLOBS_AFTER=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count after recovery: $NODE1_BLOBS_AFTER (was: $NODE1_BLOBS)"

if [[ "$NODE1_BLOBS_AFTER" -gt "$NODE1_BLOBS" ]]; then
    pass "Node1 accepting writes after recovery ($NODE1_BLOBS_AFTER > $NODE1_BLOBS)"
else
    log "FAIL: Node1 blob count did not increase after recovery"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Post-recovery sync to Node2
# =============================================================================

log "--- Check 4: Post-recovery sync to Node2 ---"

if ! wait_sync "$NODE2_CONTAINER" "$NODE1_BLOBS_AFTER" 60; then
    NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
    log "WARN: Sync timeout (Node2=$NODE2_FINAL, target=$NODE1_BLOBS_AFTER)"
fi

NODE2_FINAL=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 final blob count: $NODE2_FINAL (Node1: $NODE1_BLOBS_AFTER)"

if [[ "$NODE2_FINAL" -gt "$NODE2_BLOBS" ]]; then
    pass "Node2 received post-recovery blobs ($NODE2_FINAL > $NODE2_BLOBS)"
else
    log "FAIL: Node2 did not receive post-recovery blobs"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DOS-04: Storage full signaling & recovery PASSED"
    pass "  - Storage full signaled when 2 MiB limit exceeded"
    pass "  - Rejections tracked in metrics ($REJECTIONS rejections)"
    pass "  - Node1 accepted $NODE1_BLOBS blobs before storage full"
    pass "  - Sync suppression verified (Node2 <= Node1 blob count)"
    pass "  - SIGHUP recovery: max_storage_bytes raised to 10 MiB"
    pass "  - Post-recovery writes succeeded ($NODE1_BLOBS_AFTER blobs total)"
    pass "  - Post-recovery sync to Node2 verified ($NODE2_FINAL blobs)"
    exit 0
else
    fail "DOS-04: $FAILURES check(s) failed"
fi
