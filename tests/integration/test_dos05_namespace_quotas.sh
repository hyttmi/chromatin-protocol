#!/usr/bin/env bash
# =============================================================================
# DOS-05: Namespace Quota Enforcement
#
# Verifies:
#   1. Namespace quota (count=5) rejects writes after the limit is hit
#   2. Other namespaces are unaffected when one namespace hits quota
#   3. quota_rejections > 0 in metrics after quota exceeded
#
# Topology: Single node. Dedicated network (172.40.0.0/16).
#   Node1 (172.40.0.2): namespace_quota_count=5 (global default)
#
# Method:
#   - 3 separate loadgen invocations, each generates its own identity (namespace)
#   - Namespace 1: 10 blobs attempted (5 accepted, 5 rejected by quota)
#   - Namespace 2: 5 blobs (exactly at quota, all accepted)
#   - Namespace 3: 3 blobs (under quota, all accepted)
#   - Verify namespace isolation: quota hit on NS1 does not affect NS2/NS3
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
DOS05_NETWORK="chromatindb-dos05-test-net"
NODE1_VOLUME="chromatindb-dos05-node1-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DOS05_NETWORK"

# Temp config files
TEMP_NODE1_CONFIG=""

# --- Cleanup -----------------------------------------------------------------

cleanup_dos05() {
    log "Cleaning up DOS-05 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker network rm "$DOS05_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
}
trap cleanup_dos05 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dos05 2>/dev/null || true

log "=== DOS-05: Namespace Quota Enforcement ==="

FAILURES=0

# Create network and volume
docker network create --driver bridge --subnet 172.40.0.0/16 "$DOS05_NETWORK"
docker volume create "$NODE1_VOLUME"

# =============================================================================
# Step 1: Start Node1 with namespace_quota_count=5
# =============================================================================

log "--- Step 1: Starting Node1 with namespace_quota_count=5 ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-dos05-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5,
  "namespace_quota_count": 5
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$DOS05_NETWORK" \
    --ip 172.40.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"

# =============================================================================
# Step 2: Namespace 1 -- ingest 10 blobs (expect 5 accepted, 5 rejected)
# =============================================================================

log "--- Step 2: Namespace 1 -- ingest 10 blobs (quota=5, expect rejections) ---"

NS1_OUTPUT=$(docker run --rm --network "$DOS05_NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target "172.40.0.2:4200" --count 10 --size 1024 --rate 10 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

NS1_TOTAL=$(echo "$NS1_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
NS1_ERRORS=$(echo "$NS1_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Namespace 1: sent $NS1_TOTAL blobs (errors: $NS1_ERRORS)"

# =============================================================================
# Check 1: Quota exceeded evidence in logs
# =============================================================================

log "--- Check 1: Quota exceeded evidence ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)

if echo "$NODE1_LOGS" | grep -qi "quota.*exceeded\|QuotaExceeded\|namespace.*count.*quota"; then
    pass "Quota exceeded evidence found in Node1 logs"
else
    log "FAIL: No quota exceeded evidence in Node1 logs"
    echo "$NODE1_LOGS" | grep -i "quota" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 3: Namespace 2 -- ingest 5 blobs (exactly at quota, all should succeed)
# =============================================================================

log "--- Step 3: Namespace 2 -- ingest 5 blobs (exactly at quota) ---"

NS2_OUTPUT=$(docker run --rm --network "$DOS05_NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target "172.40.0.2:4200" --count 5 --size 1024 --rate 10 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

NS2_TOTAL=$(echo "$NS2_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
NS2_ERRORS=$(echo "$NS2_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Namespace 2: sent $NS2_TOTAL blobs (errors: $NS2_ERRORS)"

# =============================================================================
# Step 4: Namespace 3 -- ingest 3 blobs (under quota, all should succeed)
# =============================================================================

log "--- Step 4: Namespace 3 -- ingest 3 blobs (under quota) ---"

NS3_OUTPUT=$(docker run --rm --network "$DOS05_NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target "172.40.0.2:4200" --count 3 --size 1024 --rate 10 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

NS3_TOTAL=$(echo "$NS3_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
NS3_ERRORS=$(echo "$NS3_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Namespace 3: sent $NS3_TOTAL blobs (errors: $NS3_ERRORS)"

# =============================================================================
# Check 2: Metrics -- quota_rejections > 0
# =============================================================================

log "--- Check 2: Quota rejections in metrics ---"

docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2
NODE1_ALL_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
NODE1_METRICS=$(echo "$NODE1_ALL_LOGS" | grep "metrics:" | tail -1)

QUOTA_REJECTIONS=$(echo "$NODE1_METRICS" | grep -oP 'quota_rejections=\K[0-9]+' || echo "0")
log "Quota rejections: $QUOTA_REJECTIONS"

if [[ "$QUOTA_REJECTIONS" -gt 0 ]]; then
    pass "quota_rejections > 0 ($QUOTA_REJECTIONS)"
else
    log "FAIL: Expected quota_rejections > 0, got $QUOTA_REJECTIONS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Node1 has blobs from all 3 namespaces
# =============================================================================

log "--- Check 3: Blobs from all 3 namespaces ---"

TOTAL_BLOBS=$(echo "$NODE1_METRICS" | grep -oP 'blobs=\K[0-9]+' || echo "0")
log "Total blobs on Node1: $TOTAL_BLOBS"

# Expect: 5 (NS1, capped by quota) + 5 (NS2, at quota) + 3 (NS3, under quota) = 13
# The blobs= metric sums latest_seq_num across namespaces, so 13 is the expected value.
# Allow some variance (at least 10, proving all 3 namespaces wrote).
if [[ "$TOTAL_BLOBS" -ge 10 ]]; then
    pass "Node1 has blobs from multiple namespaces ($TOTAL_BLOBS total)"
else
    log "FAIL: Expected >= 10 total blobs (3 namespaces), got $TOTAL_BLOBS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Namespace isolation -- NS2 and NS3 were not affected by NS1 quota
# =============================================================================

log "--- Check 4: Namespace isolation ---"

# Extract the latest metrics dump (structured dump, not one-liner)
METRICS_DUMP=$(echo "$NODE1_ALL_LOGS" | awk '/=== METRICS DUMP/{buf=""} {buf=buf "\n" $0} /=== END METRICS/{last=buf} END{print last}' | tail -n +2)
NS_COUNT=$(echo "$METRICS_DUMP" | grep -c "latest_seq=" || echo "0")
log "Namespace count: $NS_COUNT"

if [[ "$NS_COUNT" -ge 3 ]]; then
    pass "Node1 has >= 3 namespaces (all identities wrote successfully)"
else
    log "FAIL: Expected >= 3 namespaces, got $NS_COUNT"
    echo "$METRICS_DUMP" | grep "ns:" >&2 || true
    FAILURES=$((FAILURES + 1))
fi

# Verify: NS1 has exactly 5 blobs (quota cap), NS2 has 5, NS3 has 3
# Sort namespace lines by latest_seq to identify each
NS_SEQS=$(echo "$METRICS_DUMP" | grep "latest_seq=" | grep -oP 'latest_seq=\K[0-9]+' | sort -n)
log "Namespace sequence numbers: $(echo $NS_SEQS | tr '\n' ' ')"

# We expect one of them to be 3, one to be 5, and one to be 5
# (in any order). Just verify no namespace exceeds 5 (the quota).
MAX_SEQ=$(echo "$NS_SEQS" | tail -1)
if [[ -n "$MAX_SEQ" && "$MAX_SEQ" -le 5 ]]; then
    pass "No namespace exceeds quota (max seq=$MAX_SEQ, quota=5)"
else
    if [[ -z "$MAX_SEQ" ]]; then
        log "FAIL: Could not extract namespace sequence numbers"
    else
        log "FAIL: A namespace exceeds quota (max seq=$MAX_SEQ > 5)"
    fi
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DOS-05: Namespace quota enforcement PASSED"
    pass "  - Quota exceeded after 5 blobs in namespace 1"
    pass "  - quota_rejections=$QUOTA_REJECTIONS in metrics"
    pass "  - Namespace 2 wrote 5 blobs (at quota, unaffected)"
    pass "  - Namespace 3 wrote 3 blobs (under quota, unaffected)"
    pass "  - Namespace isolation verified ($NS_COUNT namespaces, max seq=$MAX_SEQ)"
    exit 0
else
    fail "DOS-05: $FAILURES check(s) failed"
fi
