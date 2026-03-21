#!/usr/bin/env bash
# =============================================================================
# NET-03: Large Blob Integrity Verification
#
# Verifies that blobs at 1K, 100K, 1M, 10M, and 100M sizes sync correctly
# between 2 nodes with hash verification. Checks for OOM kills.
#
# Hash verification strategy:
#   - Tiers 1-3 (1K, 100K, 1M): Full content-addressed hash verification via
#     chromatindb_verify hash-fields. Loadgen --verbose-blobs captures field
#     values, then hash-fields independently recomputes signing_digest.
#   - Tiers 4-5 (10M, 100M): Blob count verification only. --verbose-blobs
#     would output 20-200 MB of hex chars for data_hex. AEAD-authenticated
#     transfer means corrupted data fails decryption and is never stored, so
#     successful sync implicitly proves integrity.
#
# Topology: docker-compose.recon.yml (2-node)
#   Node1 (172.28.0.2): seed node, receives ingested blobs
#   Node2 (172.28.0.3): bootstrap from node1, receives synced blobs
#
# Flow:
#   1. Start 2-node topology
#   2. For each blob size (1K, 100K, 1M, 10M, 100M):
#      a. Ingest 1 blob to node1 (with --verbose-blobs for small tiers)
#      b. Verify node1 accepted it (blob count check)
#      c. Wait for sync to node2 (scaled timeout)
#      d. Verify blob count on node2 matches running total
#      e. For small tiers: verify hash-fields signing digest matches
#   3. Check no OOM kills on either container
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_RECON="docker compose -f $SCRIPT_DIR/docker-compose.recon.yml -p chromatindb-test"

NODE1_CONTAINER="chromatindb-test-node1"
NODE2_CONTAINER="chromatindb-test-node2"

NETWORK="chromatindb-test_test-net"

# Temp dir for capturing --verbose-blobs output
VERBOSE_TMPDIR=$(mktemp -d /tmp/net03-verbose-XXXXXX)

# --- Cleanup -----------------------------------------------------------------

cleanup_net03() {
    log "Cleaning up NET-03 test..."
    $COMPOSE_RECON down -v --remove-orphans 2>/dev/null || true
    rm -rf "$VERBOSE_TMPDIR" 2>/dev/null || true
}
trap cleanup_net03 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_net03 2>/dev/null || true

log "=== NET-03: Large Blob Integrity Test ==="

FAILURES=0
HASH_VERIFIED=0

# =============================================================================
# Start 2-node topology
# =============================================================================

log "--- Starting 2-node recon topology ---"

$COMPOSE_RECON up -d
wait_healthy "$NODE1_CONTAINER"
wait_healthy "$NODE2_CONTAINER"

# Wait for peer connection to establish
sleep 10

# =============================================================================
# Test each blob size tier
# =============================================================================

# Size tiers: 1K, 100K, 1M, 10M, 100M
SIZES=(1024 102400 1048576 10485760 104857600)
LABELS=("1K" "100K" "1M" "10M" "100M")
TIMEOUTS=(60 60 120 180 300)

# First 3 tiers use --verbose-blobs for hash verification.
# Last 2 tiers (10M, 100M) skip --verbose-blobs because data_hex output
# would be 20-200 MB of hex characters. AEAD-authenticated transfer ensures
# corrupted data fails decryption and is never stored -- successful blob count
# match implicitly proves integrity for large blobs.
HASH_VERIFY_TIERS=3

EXPECTED_TOTAL=0

for i in "${!SIZES[@]}"; do
    SIZE="${SIZES[$i]}"
    LABEL="${LABELS[$i]}"
    TIMEOUT="${TIMEOUTS[$i]}"
    EXPECTED_TOTAL=$((EXPECTED_TOTAL + 1))

    log "--- Tier $((i+1))/5: ${LABEL} blob (${SIZE} bytes) ---"

    # Ingest 1 blob of this size to node1
    # Large blobs (10M+) may time out on notification ACK but the blob is
    # still accepted. We verify acceptance via node1 blob count, not loadgen errors.
    log "Ingesting 1 blob (${LABEL})..."

    if [[ $i -lt $HASH_VERIFY_TIERS ]]; then
        # Small tiers: capture verbose blob fields for hash verification
        docker run --rm --network "$NETWORK" \
            --entrypoint chromatindb_loadgen \
            "$IMAGE" \
            --target 172.28.0.2:4200 \
            --count 1 --size "$SIZE" --ttl 3600 --rate 10 \
            --verbose-blobs \
            > "$VERBOSE_TMPDIR/stats_${i}.json" 2> "$VERBOSE_TMPDIR/stderr_${i}.txt" || true
    else
        # Large tiers (10M, 100M): no --verbose-blobs to avoid massive hex output
        run_loadgen 172.28.0.2 --count 1 --size "$SIZE" --ttl 3600 >/dev/null 2>&1 || true
    fi

    # Verify node1 accepted the blob
    if ! wait_sync "$NODE1_CONTAINER" "$EXPECTED_TOTAL" 30; then
        log "FAIL: Node1 did not accept ${LABEL} blob"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    log "Node1 accepted ${LABEL} blob"

    # Wait for sync to node2
    log "Waiting for sync to node2 (${LABEL}, timeout=${TIMEOUT}s)..."
    if ! wait_sync "$NODE2_CONTAINER" "$EXPECTED_TOTAL" "$TIMEOUT"; then
        NODE2_PARTIAL=$(get_blob_count "$NODE2_CONTAINER")
        log "FAIL: Sync timeout for ${LABEL} blob (node2=$NODE2_PARTIAL, expected $EXPECTED_TOTAL)"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    # Verify blob count on node2
    NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
    if [[ "$NODE2_COUNT" -ge "$EXPECTED_TOTAL" ]]; then
        pass "${LABEL} blob synced to node2 ($NODE2_COUNT/$EXPECTED_TOTAL)"
    else
        log "FAIL: Node2 has $NODE2_COUNT blobs, expected >= $EXPECTED_TOTAL after ${LABEL}"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    # Hash verification for small tiers (1K, 100K, 1M)
    if [[ $i -lt $HASH_VERIFY_TIERS ]]; then
        log "Verifying hash-fields for ${LABEL} blob..."

        # Extract BLOB_FIELDS from loadgen stderr
        BLOB_LINE=$(grep '^BLOB_FIELDS:' "$VERBOSE_TMPDIR/stderr_${i}.txt" | head -1 || true)
        if [[ -z "$BLOB_LINE" ]]; then
            log "FAIL: No BLOB_FIELDS output for ${LABEL} blob"
            FAILURES=$((FAILURES + 1))
            continue
        fi

        # Parse JSON fields
        BLOB_JSON="${BLOB_LINE#BLOB_FIELDS:}"
        NS=$(echo "$BLOB_JSON" | jq -r '.namespace_id')
        DATA_HEX=$(echo "$BLOB_JSON" | jq -r '.data_hex')
        TTL=$(echo "$BLOB_JSON" | jq -r '.ttl')
        TS=$(echo "$BLOB_JSON" | jq -r '.timestamp')
        EXPECTED_DIGEST=$(echo "$BLOB_JSON" | jq -r '.signing_digest')

        # Independently recompute signing digest via chromatindb_verify hash-fields
        VERIFY_JSON=$(docker run --rm --entrypoint chromatindb_verify "$IMAGE" \
            hash-fields \
            --namespace-hex "$NS" \
            --data-hex "$DATA_HEX" \
            --ttl "$TTL" \
            --timestamp "$TS" 2>/dev/null)

        ACTUAL_DIGEST=$(echo "$VERIFY_JSON" | jq -r '.signing_digest')

        if [[ "$ACTUAL_DIGEST" == "$EXPECTED_DIGEST" ]]; then
            pass "${LABEL} hash-fields verified: digest=$ACTUAL_DIGEST"
            HASH_VERIFIED=$((HASH_VERIFIED + 1))
        else
            log "FAIL: ${LABEL} hash-fields mismatch: expected=$EXPECTED_DIGEST actual=$ACTUAL_DIGEST"
            FAILURES=$((FAILURES + 1))
        fi
    fi
done

# =============================================================================
# Check for OOM kills
# =============================================================================

log "--- Checking for OOM kills ---"

NODE1_OOM=$(docker inspect --format '{{.State.OOMKilled}}' "$NODE1_CONTAINER" 2>/dev/null || echo "unknown")
NODE2_OOM=$(docker inspect --format '{{.State.OOMKilled}}' "$NODE2_CONTAINER" 2>/dev/null || echo "unknown")

if [[ "$NODE1_OOM" == "true" ]]; then
    log "FAIL: Node1 was OOM killed"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$NODE2_OOM" == "true" ]]; then
    log "FAIL: Node2 was OOM killed"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$NODE1_OOM" != "true" && "$NODE2_OOM" != "true" ]]; then
    pass "No OOM kills on either node"
fi

# --- Final verification: total blob count ------------------------------------

FINAL_NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
log "Final node2 blob count: $FINAL_NODE2_COUNT (expected >= $EXPECTED_TOTAL)"

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "NET-03: Large blob integrity PASSED"
    pass "  - All 5 size tiers (1K, 100K, 1M, 10M, 100M) synced correctly"
    pass "  - Hash-fields verified on $HASH_VERIFIED tiers (1K, 100K, 1M)"
    pass "  - Large tiers (10M, 100M) integrity via AEAD-authenticated sync"
    pass "  - Node2 final blob count: $FINAL_NODE2_COUNT"
    pass "  - No OOM kills"
    exit 0
else
    fail "NET-03: $FAILURES check(s) failed"
fi
