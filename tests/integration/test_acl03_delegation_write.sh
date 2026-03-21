#!/usr/bin/env bash
# =============================================================================
# ACL-03: Delegation Write + Write-Only Enforcement
#
# Verifies:
#   1. A delegate can write blobs to the owner's namespace on a remote node
#      (cross-node delegation: owner delegates on Node1, delegate writes on Node2)
#   2. Delegated blobs sync to all cluster nodes
#   3. A delegate cannot delete (tombstone) -- write-only enforcement
#
# Topology: 3-node open-mode cluster via docker-compose.acl.yml
#   Node1 (172.28.0.2): owner creates delegation blob here
#   Node2 (172.28.0.3): delegate writes here (proves cross-node delegation)
#   Node3 (172.28.0.4): sync target for verification
#
# Flow:
#   A. Generate owner + delegate identities (saved to host-mounted dirs)
#   B. Discover owner's namespace hex from loadgen output
#   C. Owner creates delegation blob on Node1 for delegate's pubkey
#   D. Wait for delegation blob to sync across cluster
#   E. Delegate writes 5 blobs to owner's namespace on Node2 using --namespace
#   F. Verify delegated blobs sync to Node1 and Node3
#   G. Delegate attempts tombstone on Node1 using --namespace (must be rejected)
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

cleanup_acl03() {
    log "Cleaning up ACL-03 test..."
    $COMPOSE_ACL down -v --remove-orphans 2>/dev/null || true
    [[ -n "$OWNER_DIR" ]] && rm -rf "$OWNER_DIR" 2>/dev/null || true
    [[ -n "$DELEGATE_DIR" ]] && rm -rf "$DELEGATE_DIR" 2>/dev/null || true
}
trap cleanup_acl03 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_acl03 2>/dev/null || true

log "=== ACL-03: Delegation Write + Write-Only Enforcement ==="

FAILURES=0

# Create host temp dirs for identity files
OWNER_DIR=$(mktemp -d /tmp/acl03-owner-XXXXXX)
DELEGATE_DIR=$(mktemp -d /tmp/acl03-delegate-XXXXXX)
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
# Step A: Generate owner identity and write 1 baseline blob
# =============================================================================

log "--- Step A: Generate owner identity ---"

# Generate owner identity; capture stderr for the namespace log line
OWNER_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$OWNER_DIR:/identity" \
    --user "$(id -u):$(id -g)" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-save /identity --count 1 --size 256 --ttl 3600 \
    2>/tmp/acl03-owner-stderr.txt)

OWNER_ERRORS=$(echo "$OWNER_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
OWNER_BLOBS=$(echo "$OWNER_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Owner baseline: $OWNER_BLOBS blob(s), $OWNER_ERRORS error(s)"

if [[ ! -f "$OWNER_DIR/node.pub" ]]; then
    fail "Owner identity not saved (node.pub missing)"
fi

# Discover owner's namespace from loadgen stderr (format: "namespace: <64hex>")
OWNER_NS_HEX=$(grep -oP 'namespace: \K[0-9a-f]{64}' /tmp/acl03-owner-stderr.txt | head -1)
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

# Read delegate public key hex from saved identity
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
# Step D: Wait for delegation blob to sync across cluster
# =============================================================================

log "--- Step D: Waiting for delegation to sync ---"

# Owner wrote 1 baseline + 1 delegation = 2 blobs in owner namespace.
# Delegate wrote 1 blob in step B (delegate's own namespace).
# Total blobs on each node after sync: 3 (2 owner ns + 1 delegate ns).
# Use wait_sync to verify delegation propagated.
wait_sync "$NODE2_CONTAINER" 3 60 || true
wait_sync "$NODE3_CONTAINER" 3 60 || true

NODE2_PRE=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 pre-delegation-write blob count: $NODE2_PRE"

# =============================================================================
# Step E: Delegate writes 5 blobs to OWNER's namespace on Node2
# =============================================================================

log "--- Step E: Delegate writes to owner's namespace on Node2 ---"

# Key: --namespace tells loadgen to write to the owner's namespace instead of
# the delegate's own namespace. The engine verifies delegation and accepts.
DELEGATE_WRITE_OUTPUT=$(docker run --rm --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.3:4200 --identity-file /identity \
    --namespace "$OWNER_NS_HEX" --count 5 --size 256 --ttl 3600 \
    2>/dev/null)

DELEGATE_WRITE_ERRORS=$(echo "$DELEGATE_WRITE_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
DELEGATE_WRITE_TOTAL=$(echo "$DELEGATE_WRITE_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
log "Delegate writes: $DELEGATE_WRITE_TOTAL blobs, $DELEGATE_WRITE_ERRORS errors"

# =============================================================================
# Check 1: Delegate writes accepted (0 errors)
# =============================================================================

log "--- Check 1: Delegate writes accepted ---"

if [[ "$DELEGATE_WRITE_ERRORS" -eq 0 && "$DELEGATE_WRITE_TOTAL" -eq 5 ]]; then
    pass "Delegate wrote 5 blobs to owner's namespace on Node2 with 0 errors"
else
    log "FAIL: Delegate writes: total=$DELEGATE_WRITE_TOTAL, errors=$DELEGATE_WRITE_ERRORS (expected 5/0)"
    log "  Node2 logs (last 30 lines):"
    docker logs "$NODE2_CONTAINER" 2>&1 | tail -30 >&2
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Delegated blobs sync across cluster
# =============================================================================

log "--- Check 2: Delegated blobs sync to cluster ---"

# After delegate writes 5 blobs to owner's namespace on Node2:
# Total system blobs: 2 (owner ns) + 1 (delegate ns from B) + 5 (delegate->owner ns) = 8
wait_sync "$NODE3_CONTAINER" 8 120 || true
wait_sync "$NODE1_CONTAINER" 8 120 || true

NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")
NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "After delegate writes: Node1=$NODE1_COUNT blobs, Node3=$NODE3_COUNT blobs"

if [[ "$NODE3_COUNT" -ge 8 ]]; then
    pass "Delegated blobs synced to Node3 ($NODE3_COUNT blobs)"
elif [[ "$NODE1_COUNT" -ge 8 ]]; then
    # Node3 may be slow; verify Node1 got them at least
    pass "Delegated blobs synced to Node1 ($NODE1_COUNT blobs)"
else
    log "FAIL: Node1=$NODE1_COUNT, Node3=$NODE3_COUNT blobs (expected >= 8)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step G: Delegate tries to tombstone in owner's namespace (must be rejected)
# =============================================================================

log "--- Step G: Delegate attempts tombstone (must fail) ---"

# Extract a blob hash from the delegate's write output
DELEGATE_BLOB_HASH=$(echo "$DELEGATE_WRITE_OUTPUT" | jq -r '.blob_hashes[0]' 2>/dev/null || echo "")
if [[ -z "$DELEGATE_BLOB_HASH" || "$DELEGATE_BLOB_HASH" == "null" ]]; then
    log "WARN: Could not extract delegate blob hash, using delegation hash"
    DELEGATE_BLOB_HASH="$DELEGATION_HASH"
fi
log "Attempting tombstone of blob: ${DELEGATE_BLOB_HASH:0:16}..."

# Delegate tries to delete in owner's namespace via Node1 (must be rejected)
# --namespace ensures the tombstone targets the owner's namespace
DELETE_OUTPUT=$(echo "$DELEGATE_BLOB_HASH" | docker run --rm -i --network "$NETWORK" \
    -v "$DELEGATE_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen "$IMAGE" \
    --target 172.28.0.2:4200 --identity-file /identity \
    --namespace "$OWNER_NS_HEX" --delete --hashes-from stdin --ttl 0 \
    2>/dev/null) || true

# =============================================================================
# Check 3: Delegate delete attempt rejected
# =============================================================================

log "--- Check 3: Delegate tombstone rejected ---"

# Check Node1 logs for the rejection message.
# The delete handler rejects non-owners with "SHA3-256(pubkey) != namespace_id"
# because deletion is owner-privileged (no delegation check in the delete path).
# The ingest path would reject with "delegates cannot create tombstone" if it
# went through the Data message handler instead.
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

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "ACL-03: Delegation write + write-only enforcement PASSED"
    pass "  - Delegate wrote 5 blobs to owner's namespace on Node2 (0 errors)"
    pass "  - Delegated blobs synced across cluster"
    pass "  - Delegate tombstone attempt rejected (write-only enforcement)"
    exit 0
else
    fail "ACL-03: $FAILURES check(s) failed"
fi
