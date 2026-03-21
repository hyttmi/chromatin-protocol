#!/usr/bin/env bash
# =============================================================================
# ACL-04: Delegation Revocation Propagation
#
# Verifies:
#   1. Pre-revocation: delegate can write to owner's namespace (delegation works)
#   2. Owner tombstones the delegation blob (TTL=0 permanent revocation)
#   3. Post-revocation: delegate's writes are rejected ("no ownership or delegation")
#   4. Blob count remains stable after revoked delegate's write attempts
#
# Topology: 3-node open-mode cluster via docker-compose.acl.yml
#   Node1 (172.28.0.2): owner creates delegation blob and revokes it here
#   Node2 (172.28.0.3): delegate pre-revocation writes here
#   Node3 (172.28.0.4): revocation syncs here, delegate post-revocation rejected
#
# Flow:
#   A. Generate owner + delegate identities
#   B. Owner creates delegation blob on Node1
#   C. Delegate writes 2 blobs to Node2 (pre-revocation, must succeed)
#   D. Owner tombstones delegation blob on Node1 (TTL=0 permanent)
#   E. Wait for tombstone to sync to all nodes
#   F. Delegate tries to write on Node3 (post-revocation, must be rejected)
#   G. Verify blob count stable on Node1 (no new blobs from revoked delegate)
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

# Host temp dirs for identity persistence
OWNER_DIR=""
DELEGATE_DIR=""

# --- Cleanup -----------------------------------------------------------------

cleanup_acl04() {
    log "Cleaning up ACL-04 test..."
    $COMPOSE_ACL down -v --remove-orphans 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
    [[ -n "$DELEGATE_DIR" ]] && rm -rf "$DELEGATE_DIR" 2>/dev/null || true
}
trap cleanup_acl04 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_acl04 2>/dev/null || true

log "=== ACL-04: Delegation Revocation Propagation ==="

FAILURES=0

# Create host temp dirs for identity files
OWNER_DIR=$(mktemp -d /tmp/acl04-owner-XXXXXX)
DELEGATE_DIR=$(mktemp -d /tmp/acl04-delegate-XXXXXX)
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
sleep 10

# =============================================================================
# Step A: Generate owner + delegate identities
# =============================================================================

log "--- Step A: Generate identities ---"

# Generate owner identity
OWNER_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-save /identity --count 1 --size 256 --ttl 3600 \
    2>/tmp/acl04-owner-stderr.txt)

if [[ ! -f "$OWNER_DIR/node.pub" ]]; then
    fail "Owner identity not saved"
fi

OWNER_NS_HEX=$(grep -oP 'namespace: \K[0-9a-f]{64}' /tmp/acl04-owner-stderr.txt | head -1)
if [[ -z "$OWNER_NS_HEX" ]]; then
    fail "Could not discover owner namespace"
fi
log "Owner namespace: ${OWNER_NS_HEX:0:16}..."

# Generate delegate identity
docker run --rm --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-save /identity --count 1 --size 256 --ttl 3600 \
    2>/dev/null >/dev/null

if [[ ! -f "$DELEGATE_DIR/node.pub" ]]; then
    fail "Delegate identity not saved"
fi

DELEGATE_PUBKEY_HEX=$(xxd -p -c 0 "$DELEGATE_DIR/node.pub")
log "Delegate pubkey: ${DELEGATE_PUBKEY_HEX:0:32}... (${#DELEGATE_PUBKEY_HEX} hex chars)"

# =============================================================================
# Step B: Owner creates delegation blob on Node1
# =============================================================================

log "--- Step B: Owner creates delegation blob ---"

DELEGATION_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-file /identity --delegate "$DELEGATE_PUBKEY_HEX" --count 1 --ttl 0 \
    2>/dev/null)

DELEGATION_ERRORS=$(echo "$DELEGATION_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
DELEGATION_HASH=$(echo "$DELEGATION_OUTPUT" | jq -r '.blob_hashes[0]' 2>/dev/null || echo "")
log "Delegation blob: errors=$DELEGATION_ERRORS, hash=${DELEGATION_HASH:0:16}..."

if [[ "$DELEGATION_ERRORS" -ne 0 || -z "$DELEGATION_HASH" || "$DELEGATION_HASH" == "null" ]]; then
    fail "Failed to create delegation blob"
fi

# Wait for delegation to propagate.
# Total blobs: owner 1 baseline + 1 delegation + delegate 1 identity gen = 3
# Use Node1's blob count (most reliable as hub node) + fixed sleep for propagation.
sleep 30

# =============================================================================
# Step C: Pre-revocation -- delegate writes to Node2 (must succeed)
# =============================================================================

log "--- Step C: Pre-revocation delegate write ---"

PRE_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.3:4200 --identity-file /identity \
    --namespace "$OWNER_NS_HEX" --count 2 --size 256 --ttl 3600 \
    2>/dev/null)

PRE_ERRORS=$(echo "$PRE_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
PRE_TOTAL=$(echo "$PRE_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Pre-revocation delegate writes: $PRE_TOTAL blobs, $PRE_ERRORS errors"

# =============================================================================
# Check 1: Pre-revocation writes succeed
# =============================================================================

log "--- Check 1: Pre-revocation writes succeed ---"

if [[ "$PRE_ERRORS" -eq 0 && "$PRE_TOTAL" -eq 2 ]]; then
    pass "Pre-revocation: delegate wrote 2 blobs with 0 errors"
else
    log "FAIL: Pre-revocation writes: total=$PRE_TOTAL, errors=$PRE_ERRORS (expected 2/0)"
    docker logs "$NODE2_CONTAINER" 2>&1 | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# Wait for pre-revocation delegate blobs to sync to Node1
# Total on Node1: 3 (original) + 2 (delegate writes via sync) = 5
wait_sync "$NODE1_CONTAINER" 5 60 || true

# Record pre-revocation blob count on Node1 (most reliable hub node)
PRE_REVOKE_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Pre-revocation blob count on Node1: $PRE_REVOKE_COUNT"

# =============================================================================
# Step D: Owner tombstones delegation blob (TTL=0 permanent revocation)
# =============================================================================

log "--- Step D: Owner revokes delegation (tombstone TTL=0) ---"

# Owner deletes the delegation blob via tombstone
REVOKE_OUTPUT=$(echo "$DELEGATION_HASH" | docker run --rm -i --network "$NETWORK" \
    -v "$OWNER_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-file /identity --delete --hashes-from stdin --ttl 0 \
    2>/tmp/acl04-revoke-stderr.txt) || true

log "Revocation output: $(echo "$REVOKE_OUTPUT" | head -1 2>/dev/null || echo '(empty)')"

# Verify tombstone was sent: Node1 logs should show the delegation blob was tombstoned
sleep 3
NODE1_REVOKE_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
if echo "$NODE1_REVOKE_LOGS" | grep -q "Blob deleted via tombstone"; then
    log "Tombstone applied on Node1"
else
    log "WARN: Tombstone confirmation not found in Node1 logs (continuing)"
fi

# =============================================================================
# Step E: Wait for tombstone to sync to all nodes
# =============================================================================

log "--- Step E: Waiting for revocation tombstone to propagate ---"

# The tombstone itself is a blob, so total count increases.
# Wait a fixed duration for tombstone propagation across all 3 nodes.
sleep 30

# =============================================================================
# Step F: Delegate tries to write on Node3 (must be rejected)
# =============================================================================

log "--- Step F: Post-revocation delegate write attempt on Node3 ---"

POST_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.4:4200 --identity-file /identity \
    --namespace "$OWNER_NS_HEX" --count 3 --size 256 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

POST_ERRORS=$(echo "$POST_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "unknown")
POST_TOTAL=$(echo "$POST_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "unknown")
log "Post-revocation writes: total=$POST_TOTAL, errors=$POST_ERRORS"

# =============================================================================
# Check 2: Post-revocation writes rejected on Node3
# =============================================================================

log "--- Check 2: Post-revocation writes rejected ---"

NODE3_LOGS=$(docker logs "$NODE3_CONTAINER" 2>&1)
if echo "$NODE3_LOGS" | grep -q "no ownership or delegation"; then
    pass "Post-revocation: delegate writes rejected ('no ownership or delegation')"
else
    if echo "$NODE3_LOGS" | grep -q "Ingest rejected"; then
        pass "Post-revocation: delegate writes rejected (Ingest rejected in logs)"
    else
        log "FAIL: Expected 'no ownership or delegation' in Node3 logs after revocation"
        log "  Node3 logs (last 30 lines):"
        echo "$NODE3_LOGS" | tail -30 >&2
        FAILURES=$((FAILURES + 1))
    fi
fi

# =============================================================================
# Check 3: Blob count stable on Node1 after revoked writes
# =============================================================================

log "--- Check 3: Blob count stable ---"

# Wait for any potential (incorrect) sync from the rejected writes
sleep 8

POST_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Post-revocation blob count on Node1: $POST_COUNT (was $PRE_REVOKE_COUNT pre-revocation)"

# The count should be PRE_REVOKE_COUNT + 1 (tombstone) -- NOT more.
# The tombstone is stored as a blob, incrementing the count by 1.
# Revoked delegate's 3 write attempts should NOT increase the count further.
MAX_EXPECTED=$((PRE_REVOKE_COUNT + 1))

if [[ "$POST_COUNT" -le "$MAX_EXPECTED" ]]; then
    pass "Blob count stable: $POST_COUNT (<= $MAX_EXPECTED, no increase from revoked writes)"
else
    log "FAIL: Blob count increased beyond expected: $POST_COUNT > $MAX_EXPECTED"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "ACL-04: Delegation revocation propagation PASSED"
    pass "  - Pre-revocation: delegate writes accepted (2 blobs, 0 errors)"
    pass "  - Post-revocation: delegate writes rejected ('no ownership or delegation')"
    pass "  - Blob count stable after revoked delegate's write attempts"
    exit 0
else
    fail "ACL-04: $FAILURES check(s) failed"
fi
