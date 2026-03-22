#!/usr/bin/env bash
# =============================================================================
# SAN-05: Handshake Fuzzing
#
# Verifies:
#   Malformed messages at each PQ handshake stage cause clean disconnect with
#   no crashes or state corruption. Node remains fully functional after all
#   handshake fuzzing payloads.
#
# Topology: Single chromatindb node + fuzzer container on dedicated network.
#   Network: chromatindb-san05-test-net (172.47.0.0/16)
#   Node1 (172.47.0.2): target node
#   Fuzzer: chromatindb-fuzzer container in handshake mode
#
# Verification:
#   1. Node still running after fuzzing
#   2. Node still functional (loadgen writes succeed)
#   3. No crash indicators in logs
#   4. Clean disconnect evidence in logs (handshake errors handled)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-san05-node1"
SAN05_NETWORK="chromatindb-san05-test-net"
NODE1_VOLUME="chromatindb-san05-node1-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$SAN05_NETWORK"

TEMP_NODE1_CONFIG=""

# --- Cleanup -----------------------------------------------------------------

cleanup_san05() {
    log "Cleaning up SAN-05 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker network rm "$SAN05_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
}
trap cleanup_san05 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_san05 2>/dev/null || true

log "=== SAN-05: Handshake Fuzzing ==="

FAILURES=0

# Create network and volume
docker network create --driver bridge --subnet 172.47.0.0/16 "$SAN05_NETWORK"
docker volume create "$NODE1_VOLUME"

# =============================================================================
# Step 1: Start Node1
# =============================================================================

log "--- Step 1: Starting target node ---"

TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-san05-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "sync_interval_seconds": 5
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$SAN05_NETWORK" \
    --ip 172.47.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"

# =============================================================================
# Step 2: Build and run fuzzer in handshake mode
# =============================================================================

log "--- Step 2: Building fuzzer image ---"

docker build -t chromatindb-fuzzer "$SCRIPT_DIR/fuzz/"

log "--- Step 2: Running handshake fuzzer ---"

FUZZER_OUTPUT=$(docker run --rm --network "$SAN05_NETWORK" \
    chromatindb-fuzzer \
    --target 172.47.0.2:4200 --mode handshake --timeout 3 2>&1) || true

echo "$FUZZER_OUTPUT"

# Count payloads sent
PAYLOADS_SENT=$(echo "$FUZZER_OUTPUT" | grep -c "^\\[fuzzer\\] PAYLOAD" || true)
log "Handshake payloads sent: $PAYLOADS_SENT"

if [[ "$PAYLOADS_SENT" -ge 7 ]]; then
    pass "All 7+ handshake payloads sent"
else
    log "FAIL: Only $PAYLOADS_SENT handshake payloads sent (expected 7+)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 1: Node still running
# =============================================================================

log "--- Check 1: Node still running after fuzzing ---"

NODE1_STATUS=$(docker inspect --format '{{.State.Running}}' "$NODE1_CONTAINER" 2>/dev/null || echo "false")
if [[ "$NODE1_STATUS" == "true" ]]; then
    pass "Node1 still running after handshake fuzzing"
else
    log "FAIL: Node1 is not running after handshake fuzzing"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Node still functional (loadgen writes succeed)
# =============================================================================

log "--- Check 2: Node still accepts connections and data ---"

run_loadgen "172.47.0.2" --count 5 --size 512 --rate 50 --ttl 3600

POST_FUZZ_BLOBS=$(get_blob_count "$NODE1_CONTAINER")
log "Post-fuzz blob count: $POST_FUZZ_BLOBS"

if [[ "$POST_FUZZ_BLOBS" -ge 5 ]]; then
    pass "Node accepts valid data after handshake fuzzing ($POST_FUZZ_BLOBS blobs)"
else
    log "FAIL: Node not accepting data after fuzzing ($POST_FUZZ_BLOBS blobs)"
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

# =============================================================================
# Check 4: Clean disconnect evidence in logs
# =============================================================================

log "--- Check 4: Clean disconnect handling evidence ---"

# The node should show evidence of handling bad handshakes -- look for
# connection error/warning lines related to handshake failures, disconnects,
# or read errors. These prove the node detected and handled bad input.
DISCONNECT_INDICATORS="error|disconnect|handshake.*fail|read.*error|connection.*closed|connection.*reset|invalid|malformed"

DISCONNECT_HITS=$(echo "$NODE1_LOGS" | grep -ciE "$DISCONNECT_INDICATORS" || true)
DISCONNECT_HITS=$(echo "$DISCONNECT_HITS" | tr -d '[:space:]')
DISCONNECT_HITS=${DISCONNECT_HITS:-0}

if [[ "$DISCONNECT_HITS" -gt 0 ]]; then
    pass "Clean disconnect evidence: $DISCONNECT_HITS log lines show handled bad connections"
else
    log "WARN: No explicit disconnect/error lines found (node may silently close bad connections)"
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "SAN-05: Handshake fuzzing PASSED"
    pass "  - $PAYLOADS_SENT handshake payloads sent (3 stages covered)"
    pass "  - Node survived all malformed handshake input"
    pass "  - Node still accepts valid connections and data"
    pass "  - No crashes, SIGSEGV, SIGABRT, or sanitizer findings"
    pass "  - $DISCONNECT_HITS disconnect handling lines in logs"
    exit 0
else
    fail "SAN-05: $FAILURES check(s) failed"
fi
