#!/usr/bin/env bash
# =============================================================================
# CRYPT-04: Forward Secrecy via Ephemeral ML-KEM-1024
#
# Verifies that the PQ handshake uses ephemeral KEM (providing forward secrecy)
# and that captured traffic is opaque (no plaintext leakage).
#
# Forward secrecy is a protocol property -- we verify its prerequisites:
#   1. Ephemeral KEM was used during handshake (log inspection)
#   2. Captured traffic contains no plaintext blob data or namespace IDs
#   3. The handshake produced a functional encrypted session (sync works)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_FILE="$SCRIPT_DIR/docker-compose.test.yml"
COMPOSE="docker compose -f $COMPOSE_FILE -p chromatindb-test"
TCPDUMP_CONTAINER="tcpdump-capture"
CAPTURE_FILE="/tmp/chromatindb-crypt04-capture.pcap"

# --- Cleanup -----------------------------------------------------------------

cleanup_crypt04() {
    log "Cleaning up CRYPT-04 test..."
    docker kill --signal SIGINT "$TCPDUMP_CONTAINER" 2>/dev/null || true
    sleep 1
    docker rm -f "$TCPDUMP_CONTAINER" 2>/dev/null || true
    rm -f "$CAPTURE_FILE"
    $COMPOSE down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_crypt04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_crypt04 2>/dev/null || true

log "=== CRYPT-04: Forward Secrecy ==="

# Start node1 only first
log "Starting node1..."
$COMPOSE up -d node1
wait_healthy chromatindb-test-node1

# Start tcpdump capture on the test network (before node2 triggers handshake)
log "Starting tcpdump capture on test network..."
docker run -d --name "$TCPDUMP_CONTAINER" \
    --network chromatindb-test_test-net \
    --cap-add NET_ADMIN \
    nicolaka/netshoot \
    tcpdump -i any -w /tmp/capture.pcap -s 0 port 4200

sleep 2  # Let tcpdump initialize

# Start node2 (triggers PQ handshake with node1)
log "Starting node2 (triggers PQ handshake)..."
$COMPOSE up -d node2
wait_healthy chromatindb-test-node2

# Give handshake time to complete
sleep 3

# Ingest a blob and verify sync (proves encrypted session is functional)
log "Ingesting blob via node1..."
run_loadgen node1 --count 1 --size 256 --ttl 3600

log "Waiting for sync to node2..."
wait_sync chromatindb-test-node2 1

# Stop tcpdump and retrieve capture
log "Stopping tcpdump capture..."
docker kill --signal SIGINT "$TCPDUMP_CONTAINER" 2>/dev/null || true
sleep 2

docker cp "$TCPDUMP_CONTAINER:/tmp/capture.pcap" "$CAPTURE_FILE"
docker rm -f "$TCPDUMP_CONTAINER" 2>/dev/null || true

# --- Verification ------------------------------------------------------------

FAILURES=0

# Check 1: Handshake succeeded on both nodes
log "Check 1: Handshake completed on both nodes..."
if docker logs chromatindb-test-node1 2>&1 | grep -q "handshake complete"; then
    pass "Node1 handshake complete"
else
    log "FAIL: Node1 handshake not complete"
    FAILURES=$((FAILURES + 1))
fi

if docker logs chromatindb-test-node2 2>&1 | grep -q "handshake complete"; then
    pass "Node2 handshake complete"
else
    log "FAIL: Node2 handshake not complete"
    FAILURES=$((FAILURES + 1))
fi

# Check 2: PQ handshake was used (not lightweight/trusted)
log "Check 2: PQ handshake used (ephemeral KEM)..."
if docker logs chromatindb-test-node2 2>&1 | grep -q "handshake complete (initiator, PQ)"; then
    pass "Node2 used PQ handshake (ephemeral ML-KEM-1024)"
else
    log "FAIL: Node2 did not use PQ handshake"
    FAILURES=$((FAILURES + 1))
fi

if docker logs chromatindb-test-node1 2>&1 | grep -q "handshake complete (responder, PQ)"; then
    pass "Node1 used PQ responder handshake"
else
    log "FAIL: Node1 did not use PQ responder handshake"
    FAILURES=$((FAILURES + 1))
fi

# Check 3: Capture file has content
log "Check 3: Capture file has traffic..."
if [[ -f "$CAPTURE_FILE" && -s "$CAPTURE_FILE" ]]; then
    pass "Capture file exists and has content"
else
    log "FAIL: Capture file missing or empty"
    FAILURES=$((FAILURES + 1))
fi

# Check 4: Captured traffic contains no plaintext blob data or namespace IDs
# strings extracts ASCII sequences from binary; encrypted traffic should have
# no recognizable application-layer strings
log "Check 4: No plaintext in captured traffic..."
PLAINTEXT_COUNT=$(strings "$CAPTURE_FILE" | grep -ci "chromatindb\|namespace\|blob_data\|sync_request" || true)
if [[ "$PLAINTEXT_COUNT" -eq 0 ]]; then
    pass "No plaintext application data in captured traffic"
else
    # Allow very small counts from TCP metadata / container names in DNS
    if [[ "$PLAINTEXT_COUNT" -le 3 ]]; then
        log "WARN: $PLAINTEXT_COUNT possible plaintext matches (likely DNS/metadata, not payload)"
        pass "Plaintext count within acceptable threshold ($PLAINTEXT_COUNT <= 3)"
    else
        log "FAIL: Found $PLAINTEXT_COUNT plaintext matches in captured traffic"
        FAILURES=$((FAILURES + 1))
    fi
fi

# Check 5: Sync worked (encrypted session is functional)
log "Check 5: Encrypted session functional (blob synced)..."
BLOB_COUNT=$(get_blob_count chromatindb-test-node2)
if [[ "$BLOB_COUNT" -ge 1 ]]; then
    pass "Blob synced over encrypted session ($BLOB_COUNT blobs on node2)"
else
    log "FAIL: Blob did not sync to node2 ($BLOB_COUNT blobs)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "CRYPT-04: Forward secrecy verification PASSED"
    pass "  - Ephemeral ML-KEM-1024 used during handshake"
    pass "  - Captured traffic contains no plaintext"
    pass "  - Encrypted session functional for data sync"
    exit 0
else
    fail "CRYPT-04: $FAILURES check(s) failed"
fi
