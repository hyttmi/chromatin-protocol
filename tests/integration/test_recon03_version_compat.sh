#!/usr/bin/env bash
# =============================================================================
# RECON-03: Version Byte Forward Compatibility
#
# Verifies that the sync responder does not crash or corrupt state when
# receiving invalid input on the wire. Since all wire traffic is AEAD-encrypted
# after handshake, we cannot inject a malformed ReconcileInit with a wrong
# version byte from outside. The version byte rejection is verified by unit
# tests (decode_reconcile_init returns nullopt for version != 0x01).
#
# This Docker integration test verifies the broader robustness property:
# the connection handler does not crash on garbage input, continues serving,
# and resumes normal sync operations afterward.
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): receives garbage input on port 4200
#   Node2 (172.28.0.3): verifies sync still works after garbage
#
# Flow:
#   1. Start 2-node topology
#   2. Ingest 50 blobs, wait for sync (50 blobs on both)
#   3. Send raw garbage bytes to node1 via netshoot/netcat
#   4. Wait 10s, verify node1 still healthy
#   5. Verify node1 blob count unchanged (no state corruption)
#   6. Ingest 10 more blobs, verify sync to node2 (60 blobs)
#   7. Document: version byte decode rejection verified by unit test
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"

# --- Cleanup -----------------------------------------------------------------

cleanup_recon03() {
    log "Cleaning up RECON-03 test..."
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
}
trap cleanup_recon03 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_recon03 2>/dev/null || true

log "=== RECON-03: Version Byte Forward Compatibility ==="

FAILURES=0

# =============================================================================
# Step 1: Start 2-node topology
# =============================================================================

log "--- Step 1: Start 2-node recon topology ---"

$COMPOSE_RECON up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection
sleep 5

# =============================================================================
# Step 2: Ingest 50 blobs and wait for sync
# =============================================================================

log "--- Step 2: Ingest 50 blobs ---"

run_loadgen 172.28.0.2 --count 50 --size 256 --rate 50 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to node2 (50 blobs)..."
if ! wait_sync "$NODE2_CONTAINER" 50 120; then
    NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
    fail "RECON-03: Initial sync failed -- node2 has $NODE2_COUNT/50 blobs"
fi

NODE1_PRE=$(get_blob_count "$NODE1_CONTAINER")
log "Pre-garbage node1 blob count: $NODE1_PRE"

# =============================================================================
# Step 3: Send garbage bytes to node1
# =============================================================================

log "--- Step 3: Send garbage bytes to node1 port 4200 ---"

# Send multiple rounds of garbage to stress the connection handler.
# Each connection attempt sends invalid data that will fail AEAD decryption.
# The node should silently close each connection without crashing.
for attempt in 1 2 3; do
    docker run --rm --network "$NETWORK" \
        nicolaka/netshoot \
        bash -c "echo 'GARBAGE_VERSION_0xFF_INVALID_RECONCILE_ATTEMPT_${attempt}' | nc -w 2 172.28.0.2 4200" \
        2>/dev/null || true
    sleep 1
done

log "Garbage injection complete (3 attempts)"

# =============================================================================
# Step 4: Verify node1 still healthy
# =============================================================================

log "--- Step 4: Verify node1 health after garbage ---"

sleep 10

if wait_healthy "$NODE1_CONTAINER" 10; then
    pass "Node1 still healthy after garbage input"
else
    log "FAIL: Node1 not healthy after garbage input"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 1: No state corruption (blob count unchanged)
# =============================================================================

log "--- Check 1: No state corruption ---"

NODE1_POST=$(get_blob_count "$NODE1_CONTAINER")
log "Post-garbage node1 blob count: $NODE1_POST"

if [[ "$NODE1_POST" -ge "$NODE1_PRE" ]]; then
    pass "Node1 blob count unchanged ($NODE1_PRE -> $NODE1_POST, no corruption)"
else
    log "FAIL: Node1 blob count changed ($NODE1_PRE -> $NODE1_POST) -- possible corruption"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Sync still works after garbage
# =============================================================================

log "--- Check 2: Sync still works ---"

run_loadgen 172.28.0.2 --count 10 --size 256 --rate 50 --ttl 3600 >/dev/null 2>&1

log "Waiting for sync to node2 (60 blobs)..."
if ! wait_sync "$NODE2_CONTAINER" 60 120; then
    NODE2_POST=$(get_blob_count "$NODE2_CONTAINER")
    log "FAIL: Post-garbage sync failed -- node2 has $NODE2_POST/60 blobs"
    FAILURES=$((FAILURES + 1))
else
    NODE2_POST=$(get_blob_count "$NODE2_CONTAINER")
    pass "Post-garbage sync works ($NODE2_POST blobs on node2)"
fi

# =============================================================================
# Check 3: Node1 still serving correctly
# =============================================================================

log "--- Check 3: Node1 blob count correct ---"

NODE1_FINAL=$(get_blob_count "$NODE1_CONTAINER")
if [[ "$NODE1_FINAL" -ge 60 ]]; then
    pass "Node1 final blob count: $NODE1_FINAL (>= 60)"
else
    log "FAIL: Node1 final blob count: $NODE1_FINAL (expected >= 60)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
log "NOTE: Version byte decode rejection (version != 0x01 -> nullopt) is verified"
log "  by unit test decode_reconcile_init. This integration test confirms node"
log "  resilience to invalid input on port 4200 (no crash, no corruption, sync"
log "  continues normally)."
echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "RECON-03: Version byte forward compatibility PASSED"
    pass "  - Node survived 3 rounds of garbage input"
    pass "  - No state corruption (blob count preserved)"
    pass "  - Sync resumed normally after garbage"
    exit 0
else
    fail "RECON-03: $FAILURES check(s) failed"
fi
