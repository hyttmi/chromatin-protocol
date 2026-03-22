#!/usr/bin/env bash
# =============================================================================
# SAN-04: Protocol Fuzzing
#
# Verifies:
#   Malformed FlatBuffers, truncated frames, invalid crypto payloads, and
#   garbage bytes are handled gracefully with no crashes or memory corruption.
#   Node remains fully functional after all fuzzing payloads.
#
# Topology: Single chromatindb node + fuzzer container on dedicated network.
#   Network: chromatindb-san04-test-net (172.46.0.0/16)
#   Node1 (172.46.0.2): target node
#   Fuzzer: chromatindb-fuzzer container in protocol mode
#
# Verification:
#   1. Node still running after fuzzing (container alive)
#   2. Node still accepts connections (loadgen writes succeed)
#   3. No crash indicators in logs (SIGSEGV, SIGABRT, sanitizer findings)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-san04-node1"
SAN04_NETWORK="chromatindb-san04-test-net"
NODE1_VOLUME="chromatindb-san04-node1-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$SAN04_NETWORK"

TEMP_NODE1_CONFIG=""

# --- Cleanup -----------------------------------------------------------------

cleanup_san04() {
    log "Cleaning up SAN-04 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker network rm "$SAN04_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
}
trap cleanup_san04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_san04 2>/dev/null || true

log "=== SAN-04: Protocol Fuzzing ==="

FAILURES=0

# Create network and volume
docker network create --driver bridge --subnet 172.46.0.0/16 "$SAN04_NETWORK"
docker volume create "$NODE1_VOLUME"

# =============================================================================
# Step 1: Start Node1
# =============================================================================

log "--- Step 1: Starting target node ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-san04-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$SAN04_NETWORK" \
    --ip 172.46.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"

# =============================================================================
# Step 2: Ingest baseline blobs (proves node functional before fuzzing)
# =============================================================================

log "--- Step 2: Ingesting 10 baseline blobs ---"

run_loadgen "172.46.0.2" --count 10 --size 1024 --rate 50 --ttl 3600

BASELINE_BLOBS=$(get_blob_count "$NODE1_CONTAINER")
log "Baseline blob count: $BASELINE_BLOBS"

if [[ "$BASELINE_BLOBS" -ge 10 ]]; then
    pass "Baseline: $BASELINE_BLOBS blobs ingested"
else
    log "FAIL: Baseline ingestion failed ($BASELINE_BLOBS blobs)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 3: Build and run fuzzer in protocol mode
# =============================================================================

log "--- Step 3: Building fuzzer image ---"

docker build -t chromatindb-fuzzer "$SCRIPT_DIR/fuzz/"

log "--- Step 3: Running protocol fuzzer ---"

FUZZER_OUTPUT=$(docker run --rm --network "$SAN04_NETWORK" \
    chromatindb-fuzzer \
    --target 172.46.0.2:4200 --mode protocol --timeout 3 2>&1) || true

echo "$FUZZER_OUTPUT"

# Count payloads sent
PAYLOADS_SENT=$(echo "$FUZZER_OUTPUT" | grep -c "^\\[fuzzer\\] PAYLOAD" || true)
log "Protocol payloads sent: $PAYLOADS_SENT"

if [[ "$PAYLOADS_SENT" -ge 13 ]]; then
    pass "All 13+ protocol payloads sent"
else
    log "FAIL: Only $PAYLOADS_SENT protocol payloads sent (expected 13+)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 1: Node still running
# =============================================================================

log "--- Check 1: Node still running after fuzzing ---"

NODE1_STATUS=$(docker inspect --format '{{.State.Running}}' "$NODE1_CONTAINER" 2>/dev/null || echo "false")
if [[ "$NODE1_STATUS" == "true" ]]; then
    pass "Node1 still running after protocol fuzzing"
else
    log "FAIL: Node1 is not running after protocol fuzzing"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node still accepts valid data
# =============================================================================

log "--- Check 2: Node still accepts connections and data ---"

run_loadgen "172.46.0.2" --count 5 --size 512 --rate 50 --ttl 3600

POST_FUZZ_BLOBS=$(get_blob_count "$NODE1_CONTAINER")
log "Post-fuzz blob count: $POST_FUZZ_BLOBS"

if [[ "$POST_FUZZ_BLOBS" -gt "$BASELINE_BLOBS" ]]; then
    pass "Node accepts valid data after fuzzing ($POST_FUZZ_BLOBS blobs, was $BASELINE_BLOBS)"
else
    log "FAIL: Node not accepting data after fuzzing ($POST_FUZZ_BLOBS blobs, was $BASELINE_BLOBS)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: No crash indicators in logs
# =============================================================================

log "--- Check 3: No crash indicators in node logs ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
CRASH_PATTERNS="SIGSEGV|SIGABRT|AddressSanitizer|SUMMARY.*Sanitizer|stack-buffer-overflow|heap-buffer-overflow|use-after-free|double-free"

CRASH_HITS=$(echo "$NODE1_LOGS" | grep -cE "$CRASH_PATTERNS" || true)
CRASH_HITS=$(echo "$CRASH_HITS" | tr -d '[:space:]')
CRASH_HITS=${CRASH_HITS:-0}

if [[ "$CRASH_HITS" -eq 0 ]]; then
    pass "No crash indicators in node logs"
else
    log "FAIL: Found $CRASH_HITS crash indicator(s) in node logs"
    echo "$NODE1_LOGS" | grep -E "$CRASH_PATTERNS" || true
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "SAN-04: Protocol fuzzing PASSED"
    pass "  - $PAYLOADS_SENT protocol payloads sent (core + crypto + semantic)"
    pass "  - Node survived all malformed input"
    pass "  - Node still accepts valid connections and data"
    pass "  - No crashes, SIGSEGV, SIGABRT, or sanitizer findings"
    exit 0
else
    fail "SAN-04: $FAILURES check(s) failed"
fi
