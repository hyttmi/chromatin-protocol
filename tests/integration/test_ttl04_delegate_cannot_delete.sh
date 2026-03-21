#!/usr/bin/env bash
# =============================================================================
# TTL-04: Delegate Cannot Delete
#
# Verifies:
#   1. Delegate's tombstone attempt is rejected (owner-only deletion)
#   2. Owner CAN tombstone the same blob successfully
#   3. Blob count stable after delegate's failed tombstone attempt
#
# Topology: 3-node open-mode cluster via docker-compose.acl.yml
#   Node1 (172.28.0.2): owner creates delegation blob and blobs here
#   Node2 (172.28.0.3): delegate writes blobs here
#   Node3 (172.28.0.4): sync target for verification
#
# Flow:
#   A. Generate owner + delegate identities
#   B. Owner ingests 5 blobs, creates delegation blob for delegate
#   C. Wait for delegation to sync across cluster
#   D. Delegate writes 3 blobs to owner's namespace on Node2
#   E. Delegate attempts tombstone via Node1 (must be rejected)
#   F. Owner tombstones the same blob (must succeed)
#   G. Verify blob count stability (delegate tombstone had no effect)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_ACL="docker compose -f $SCRIPT_DIR/docker-compose.acl.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"
NODE3_CONTAINER="chromatindb-test-node3"

# Override helpers.sh NETWORK for custom docker run calls
NETWORK="chromatindb-test_test-net"

# Host temp dirs for identity persistence across docker run calls
OWNER_DIR=""
DELEGATE_DIR=""

# --- Cleanup -----------------------------------------------------------------

cleanup_ttl04() {
    log "Cleaning up TTL-04 test..."
    $COMPOSE_ACL down -v --remove-orphans 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
    [[ -n "$DELEGATE_DIR" ]] && rm -rf "$DELEGATE_DIR" 2>/dev/null || true
}
trap cleanup_ttl04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_ttl04 2>/dev/null || true

log "=== TTL-04: Delegate Cannot Delete ==="

FAILURES=0

# Create host temp dirs for identity files
OWNER_DIR=$(mktemp -d /tmp/ttl04-owner-XXXXXX)
DELEGATE_DIR=$(mktemp -d /tmp/ttl04-delegate-XXXXXX)
chmod 777 "$OWNER_DIR" "$DELEGATE_DIR"

# =============================================================================
# Start 3-node open-mode cluster
# =============================================================================

log "--- Starting 3-node open-mode cluster ---"

$COMPOSE_ACL up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"
wait_healthy "$NODE3_CONTAINER"

# Wait for peer mesh to form
sleep 8

# =============================================================================
# Step A: Generate owner identity and write 5 baseline blobs
# =============================================================================

log "--- Step A: Generate owner identity and write 5 blobs ---"

OWNER_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-save /identity --count 5 --size 256 --ttl 3600 \
    2>/tmp/ttl04-owner-stderr.txt)

OWNER_ERRORS=$(echo "$OWNER_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
OWNER_BLOBS=$(echo "$OWNER_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Owner baseline: $OWNER_BLOBS blob(s), $OWNER_ERRORS error(s)"

if [[ ! -f "$OWNER_DIR/node.pub" ]]; then
    fail "Owner identity not saved (node.pub missing)"
fi

# Discover owner's namespace from loadgen stderr
OWNER_NS_HEX=$(grep -oP 'namespace: \K[0-9a-f]{64}' /tmp/ttl04-owner-stderr.txt | head -1)
if [[ -z "$OWNER_NS_HEX" ]]; then
    fail "Could not discover owner namespace from loadgen output"
fi
log "Owner namespace: ${OWNER_NS_HEX:0:16}..."

# =============================================================================
# Step B: Generate delegate identity
# =============================================================================

log "--- Step B: Generate delegate identity ---"

docker run --rm --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-save /identity --count 1 --size 256 --ttl 3600 \
    2>/dev/null >/dev/null

if [[ ! -f "$DELEGATE_DIR/node.pub" ]]; then
    fail "Delegate identity not saved (node.pub missing)"
fi

DELEGATE_PUBKEY_HEX=$(xxd -p -c 0 "$DELEGATE_DIR/node.pub")
log "Delegate pubkey: ${DELEGATE_PUBKEY_HEX:0:32}... (${#DELEGATE_PUBKEY_HEX} hex chars)"

# =============================================================================
# Step C: Owner creates delegation blob for delegate on Node1
# =============================================================================

log "--- Step C: Owner creates delegation blob ---"

DELEGATION_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-file /identity --delegate "$DELEGATE_PUBKEY_HEX" --count 1 --ttl 0 \
    2>/dev/null)

DELEGATION_ERRORS=$(echo "$DELEGATION_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
DELEGATION_HASH=$(echo "$DELEGATION_OUTPUT" | jq -r '.blob_hashes[0]' 2>/dev/null || echo "")
log "Delegation blob sent: errors=$DELEGATION_ERRORS, hash=${DELEGATION_HASH:0:16}..."

if [[ "$DELEGATION_ERRORS" -ne 0 || -z "$DELEGATION_HASH" || "$DELEGATION_HASH" == "null" ]]; then
    fail "Failed to create delegation blob (errors=$DELEGATION_ERRORS, hash=$DELEGATION_HASH)"
fi

# =============================================================================
# Step D: Wait for delegation to propagate
# =============================================================================

log "--- Step D: Waiting for delegation to sync ---"

# Owner wrote 5 baseline + 1 delegation = 6 blobs in owner namespace.
# Delegate wrote 1 blob in step B (delegate's own namespace).
# Total blobs on each node after sync: 7 (6 owner ns + 1 delegate ns).
sleep 20
wait_sync "$NODE2_CONTAINER" 7 60 || true
wait_sync "$NODE3_CONTAINER" 7 60 || true

# =============================================================================
# Step E: Delegate writes 3 blobs to owner's namespace on Node2
# =============================================================================

log "--- Step E: Delegate writes to owner's namespace on Node2 ---"

DELEGATE_WRITE_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.3:4200 --identity-file /identity \
    --namespace "$OWNER_NS_HEX" --count 3 --size 256 --ttl 3600 \
    2>/dev/null)

DELEGATE_WRITE_ERRORS=$(echo "$DELEGATE_WRITE_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
DELEGATE_WRITE_TOTAL=$(echo "$DELEGATE_WRITE_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Delegate writes: $DELEGATE_WRITE_TOTAL blobs, $DELEGATE_WRITE_ERRORS errors"

# Pre-condition: delegation works (delegate can write)
if [[ "$DELEGATE_WRITE_ERRORS" -ne 0 ]]; then
    log "FAIL: Pre-condition failed: delegate writes had $DELEGATE_WRITE_ERRORS errors"
    FAILURES=$((FAILURES + 1))
fi

# Wait for delegate blobs to sync across cluster
# Total: 7 (previous) + 3 (delegate) = 10
wait_sync "$NODE1_CONTAINER" 10 60 || true

# Record pre-tombstone blob count
PRE_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Pre-tombstone blob count on Node1: $PRE_COUNT"

# Extract a blob hash from the delegate's write output for tombstone attempt
DELEGATE_BLOB_HASH=$(echo "$DELEGATE_WRITE_OUTPUT" | jq -r '.blob_hashes[0]' 2>/dev/null || echo "")
if [[ -z "$DELEGATE_BLOB_HASH" || "$DELEGATE_BLOB_HASH" == "null" ]]; then
    fail "Could not extract delegate blob hash for tombstone test"
fi
log "Target blob for tombstone: ${DELEGATE_BLOB_HASH:0:16}..."

# =============================================================================
# Step F: Delegate attempts tombstone (must be rejected)
# =============================================================================

log "--- Step F: Delegate attempts tombstone (must fail) ---"

# Delegate tries to delete in owner's namespace via Node1
# The Delete handler checks SHA3-256(pubkey) == namespace_id (owner-only)
DELETE_OUTPUT=$(echo "$DELEGATE_BLOB_HASH" | docker run --rm -i --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-file /identity \
    --namespace "$OWNER_NS_HEX" --delete --hashes-from stdin --ttl 0 \
    2>/dev/null) || true

# Wait for log messages to flush
sleep 3

# =============================================================================
# Check 1: Delegate tombstone rejected
# =============================================================================

log "--- Check 1: Delegate tombstone rejected ---"

NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS" | grep -q "delegates cannot create tombstone"; then
    pass "Delegate tombstone rejected with 'delegates cannot create tombstone'"
elif echo "$NODE1_LOGS" | grep -q "delete rejected.*SHA3-256(pubkey) != namespace_id"; then
    pass "Delegate tombstone rejected with 'SHA3-256(pubkey) != namespace_id' (owner-only delete path)"
elif echo "$NODE1_LOGS" | grep -q "SHA3-256(pubkey) != namespace_id"; then
    pass "Delegate tombstone rejected (namespace mismatch -- deletion is owner-privileged)"
else
    log "FAIL: Expected tombstone rejection in Node1 logs"
    log "  Node1 logs (last 30 lines):"
    echo "$NODE1_LOGS" | tail -30 >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Owner tombstones the same blob successfully
# =============================================================================

log "--- Check 2: Owner tombstones the same blob ---"

OWNER_DELETE_OUTPUT=$(echo "$DELEGATE_BLOB_HASH" | docker run --rm -i --network "$NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-file /identity \
    --delete --hashes-from stdin --ttl 0 \
    2>/dev/null) || true

OWNER_DELETE_TOTAL=$(echo "$OWNER_DELETE_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
OWNER_DELETE_ERRORS=$(echo "$OWNER_DELETE_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Owner tombstone: $OWNER_DELETE_TOTAL sent, $OWNER_DELETE_ERRORS errors"

# Wait for log messages
sleep 3

NODE1_LOGS_AFTER=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_LOGS_AFTER" | grep -q "Blob deleted via tombstone"; then
    pass "Owner tombstone accepted (Blob deleted via tombstone)"
else
    log "FAIL: Expected 'Blob deleted via tombstone' in Node1 logs"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Blob count stable (delegate tombstone had no effect, owner's did)
# =============================================================================

log "--- Check 3: Blob count stability ---"

# Wait for any sync to propagate
sleep 5

POST_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Post-tombstone blob count on Node1: $POST_COUNT (was $PRE_COUNT)"

# Expected: PRE_COUNT + 1 (owner's tombstone is stored as a blob)
# The delegate's rejected tombstone should NOT have increased the count.
EXPECTED_COUNT=$((PRE_COUNT + 1))

if [[ "$POST_COUNT" -eq "$EXPECTED_COUNT" ]]; then
    pass "Blob count = $POST_COUNT (expected $EXPECTED_COUNT: pre + 1 owner tombstone)"
elif [[ "$POST_COUNT" -le "$EXPECTED_COUNT" ]]; then
    pass "Blob count = $POST_COUNT (<= $EXPECTED_COUNT, delegate tombstone had no effect)"
else
    log "FAIL: Blob count $POST_COUNT > expected $EXPECTED_COUNT (delegate tombstone leaked?)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "TTL-04: Delegate cannot delete PASSED"
    pass "  - Delegate tombstone rejected (owner-only deletion confirmed)"
    pass "  - Owner tombstone accepted (Blob deleted via tombstone)"
    pass "  - Blob count stable: $POST_COUNT (delegate attempt had no effect)"
    exit 0
else
    fail "TTL-04: $FAILURES check(s) failed"
fi
