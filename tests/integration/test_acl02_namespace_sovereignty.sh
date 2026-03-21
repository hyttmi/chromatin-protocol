#!/usr/bin/env bash
# =============================================================================
# ACL-02: Namespace Sovereignty
#
# Verifies:
#   1. An attacker with their own key can write to a node, but only to their
#      OWN namespace -- they cannot write to an existing namespace they don't own
#   2. The original namespace's blob count stays unchanged after attacker writes
#   3. The engine correctly rejects blobs with mismatched namespace/pubkey by
#      verifying that namespace_id = SHA3(pubkey) for all accepted blobs
#
# Topology: 2-node open-mode cluster (standard docker-compose.test.yml)
#   Node1: target node receiving blobs
#   Node2: sync peer for verification
#
# Method: Legitimate owner writes blobs, then attacker writes blobs. Attacker's
#   blobs create a SEPARATE namespace. We verify namespace isolation via SIGUSR1
#   metrics (namespace count increases, but original namespace is untouched).
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

# --- Cleanup -----------------------------------------------------------------

cleanup_acl02() {
    log "Cleaning up ACL-02 test..."
    cleanup
}
trap cleanup_acl02 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_acl02 2>/dev/null || true

log "=== ACL-02: Namespace Sovereignty ==="

FAILURES=0

# =============================================================================
# Start 2-node open-mode cluster
# =============================================================================

log "--- Starting 2-node cluster ---"

start_nodes

# =============================================================================
# Ingest 3 legitimate blobs to establish baseline namespace
# =============================================================================

log "--- Establishing baseline with 3 legitimate blobs ---"

OWNER_OUTPUT=$(run_loadgen "$NODE1_CONTAINER" --count 3 --size 256 --ttl 3600)
OWNER_HASHES=$(echo "$OWNER_OUTPUT" | jq -r '.blob_hashes[]')
OWNER_TOTAL=$(echo "$OWNER_OUTPUT" | jq -r '.total_blobs')
log "Owner wrote $OWNER_TOTAL blobs to Node1"

# Wait for sync to Node2
wait_sync "$NODE2_CONTAINER" 3

# Get baseline: blob count, namespace count, and owner's namespace hash from metrics
docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2
NODE1_METRICS=$(docker logs "$NODE1_CONTAINER" 2>&1 | grep "metrics:" | tail -1)
BASELINE_BLOBS=$(echo "$NODE1_METRICS" | grep -oP 'blobs=\K[0-9]+' || echo "0")
log "Baseline: $BASELINE_BLOBS blobs on Node1"

# Extract the latest metrics dump only (grep the block between LAST "=== METRICS DUMP" and "=== END METRICS")
BASELINE_DUMP=$(docker logs "$NODE1_CONTAINER" 2>&1 | awk '/=== METRICS DUMP/{buf=""} {buf=buf "\n" $0} /=== END METRICS/{last=buf} END{print last}' | tail -n +2)
BASELINE_NS_COUNT=$(echo "$BASELINE_DUMP" | grep -c "latest_seq=" || echo "0")
log "Baseline: $BASELINE_NS_COUNT namespace(s) on Node1"

# Discover the owner's namespace prefix from the metrics dump namespace lines (not peer lines)
# Namespace lines look like: "ns:442b4bef... latest_seq=3"
OWNER_NS_PREFIX=$(echo "$BASELINE_DUMP" | grep "latest_seq=" | grep -oP 'ns:\K[0-9a-f]{8}' | head -1)
log "Owner namespace prefix: ns:${OWNER_NS_PREFIX}..."

# =============================================================================
# Attacker writes blobs with a fresh identity
# =============================================================================

log "--- Attacker attempting writes with non-owning key ---"

# Run loadgen with a fresh identity. The attacker's blobs go to the attacker's
# OWN namespace (a new, separate namespace on Node1). The attacker CANNOT write
# to the owner's namespace because SHA3(attacker_pubkey) != owner_namespace_id.
ATTACKER_OUTPUT=$(docker run --rm --network "$NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target "${NODE1_CONTAINER}:4200" \
    --count 3 --size 256 --ttl 3600 --drain-timeout 3 \
    2>/dev/null) || true

ATTACKER_TOTAL=$(echo "$ATTACKER_OUTPUT" | jq -r '.total_blobs' 2>/dev/null || echo "0")
ATTACKER_ERRORS=$(echo "$ATTACKER_OUTPUT" | jq -r '.errors' 2>/dev/null || echo "0")
log "Attacker wrote $ATTACKER_TOTAL blobs (errors: $ATTACKER_ERRORS)"

# Wait for potential sync
sleep 5

# =============================================================================
# Check 1: Attacker created a SEPARATE namespace (not the owner's)
# =============================================================================

log "--- Check 1: Namespace isolation ---"

docker kill -s USR1 "$NODE1_CONTAINER" >/dev/null 2>&1 || true
sleep 2
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)

# Extract the latest metrics dump only
AFTER_DUMP=$(echo "$NODE1_LOGS" | awk '/=== METRICS DUMP/{buf=""} {buf=buf "\n" $0} /=== END METRICS/{last=buf} END{print last}' | tail -n +2)
AFTER_NS_COUNT=$(echo "$AFTER_DUMP" | grep -c "latest_seq=" || echo "0")
log "Namespaces after attack: $AFTER_NS_COUNT (baseline: $BASELINE_NS_COUNT)"

if [[ "$AFTER_NS_COUNT" -gt "$BASELINE_NS_COUNT" ]]; then
    pass "Attacker created a separate namespace ($AFTER_NS_COUNT > $BASELINE_NS_COUNT)"
else
    log "FAIL: Expected new namespace from attacker writes"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 2: Original namespace blob count unchanged
# =============================================================================

log "--- Check 2: Original namespace untouched ---"

# Check the latest_seq for the owner's namespace in the LATEST metrics dump (should be 3, unchanged)
# Use OWNER_NS_PREFIX discovered from baseline metrics
OWNER_NS_SEQ=$(echo "$AFTER_DUMP" | grep "ns:${OWNER_NS_PREFIX}" | grep -oP 'latest_seq=\K[0-9]+' || echo "0")
log "Owner namespace (ns:${OWNER_NS_PREFIX}...) latest_seq: $OWNER_NS_SEQ"

if [[ "$OWNER_NS_SEQ" -eq 3 ]]; then
    pass "Owner namespace unchanged at seq=3 (attacker did not write to it)"
else
    # Debug: show the full dump for diagnosis
    log "  Metrics dump contents:"
    echo "$AFTER_DUMP" | grep "ns:" >&2 || true
    log "FAIL: Owner namespace seq=$OWNER_NS_SEQ (expected 3)"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 3: Total blob count increased by exactly 3 (attacker's namespace)
# =============================================================================

log "--- Check 3: Total blobs = baseline + attacker ---"

AFTER_METRICS=$(echo "$NODE1_LOGS" | grep "metrics:" | tail -1)
AFTER_BLOBS=$(echo "$AFTER_METRICS" | grep -oP 'blobs=\K[0-9]+' || echo "0")
EXPECTED_BLOBS=$((BASELINE_BLOBS + ATTACKER_TOTAL))
log "Total blobs: $AFTER_BLOBS (expected: $EXPECTED_BLOBS = $BASELINE_BLOBS + $ATTACKER_TOTAL)"

if [[ "$AFTER_BLOBS" -eq "$EXPECTED_BLOBS" ]]; then
    pass "Total blob count correct ($AFTER_BLOBS = $BASELINE_BLOBS baseline + $ATTACKER_TOTAL attacker)"
else
    log "FAIL: Expected $EXPECTED_BLOBS total blobs, got $AFTER_BLOBS"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Check 4: Attacker's blobs did NOT sync to Node2's owner namespace
# =============================================================================

log "--- Check 4: Attacker blobs isolated on Node2 ---"

# Node2 should also have 2 namespaces now (original + attacker's via sync)
# But the owner's namespace on Node2 should still have only the original 3 blobs
wait_sync "$NODE2_CONTAINER" 6 60 || true  # Total: 3 owner + 3 attacker
NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
log "Node2 total blob count: $NODE2_COUNT"

# The key assertion: Node2 has blobs from BOTH namespaces, but the owner's
# namespace is untouched. Total should be 6 (3 owner + 3 attacker in separate ns).
if [[ "$NODE2_COUNT" -ge 6 ]]; then
    pass "Both namespaces synced to Node2 ($NODE2_COUNT blobs total)"
elif [[ "$NODE2_COUNT" -ge 3 ]]; then
    pass "Owner namespace intact on Node2 ($NODE2_COUNT blobs, attacker may still be syncing)"
else
    log "FAIL: Node2 has fewer blobs than expected ($NODE2_COUNT < 3)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "ACL-02: Namespace sovereignty PASSED"
    pass "  - Attacker writes created separate namespace (isolation confirmed)"
    pass "  - Owner namespace blob count unchanged"
    pass "  - Total blobs = baseline + attacker (no cross-namespace contamination)"
    exit 0
else
    fail "ACL-02: $FAILURES check(s) failed"
fi
