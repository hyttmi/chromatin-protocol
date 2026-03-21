#!/usr/bin/env bash
# =============================================================================
# CRYPT-01: Content Addressing Verification
#
# Verifies that the signing digest (SHA3-256(namespace||data||ttl||timestamp))
# can be independently recomputed from raw blob fields and matches the value
# produced by the node during ingest.
#
# Flow:
#   1. Start 2-node topology
#   2. Ingest 3 small blobs via loadgen with --verbose-blobs
#   3. For each blob, independently recompute signing digest via
#      chromatindb_verify hash-fields using the raw field values
#   4. Compare independently computed digest against loadgen's reported digest
#   5. Verify blobs sync to node2
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

trap cleanup EXIT

# --- Setup -------------------------------------------------------------------

check_deps
build_image
cleanup

log "=== CRYPT-01: Content Addressing Test ==="

start_nodes

# --- Ingest blobs with verbose output ----------------------------------------

BLOB_COUNT=3
BLOB_SIZE=256

log "Ingesting $BLOB_COUNT blobs (${BLOB_SIZE}B each) with --verbose-blobs..."

# Create a temp dir for capturing output
TMPDIR=$(mktemp -d)
trap 'cleanup; rm -rf "$TMPDIR"' EXIT

# Run loadgen: stdout = JSON stats, stderr = BLOB_FIELDS lines + spdlog
docker run --rm --network "$NETWORK" \
    -v test-identity:/identity \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target node1:4200 \
    --count "$BLOB_COUNT" --size "$BLOB_SIZE" --ttl 3600 --rate 10 \
    --identity-save /identity \
    --verbose-blobs \
    > "$TMPDIR/stats.json" 2> "$TMPDIR/stderr.txt"

log "Loadgen complete, parsing BLOB_FIELDS..."

# Extract BLOB_FIELDS lines from stderr
grep '^BLOB_FIELDS:' "$TMPDIR/stderr.txt" > "$TMPDIR/blob_fields.txt" || true

FIELD_COUNT=$(wc -l < "$TMPDIR/blob_fields.txt")
if [[ "$FIELD_COUNT" -ne "$BLOB_COUNT" ]]; then
    fail "Expected $BLOB_COUNT BLOB_FIELDS lines, got $FIELD_COUNT"
fi

# --- Verify each blob's signing digest independently -------------------------

VERIFIED=0
while IFS= read -r line; do
    # Strip "BLOB_FIELDS:" prefix to get JSON
    json="${line#BLOB_FIELDS:}"

    INDEX=$(echo "$json" | jq -r '.index')
    NS=$(echo "$json" | jq -r '.namespace_id')
    DATA_HEX=$(echo "$json" | jq -r '.data_hex')
    TTL=$(echo "$json" | jq -r '.ttl')
    TS=$(echo "$json" | jq -r '.timestamp')
    EXPECTED_DIGEST=$(echo "$json" | jq -r '.signing_digest')

    log "Blob $INDEX: verifying signing digest independently..."

    # Independently recompute signing digest via chromatindb_verify hash-fields
    VERIFY_JSON=$(docker run --rm "$IMAGE" \
        chromatindb_verify hash-fields \
        --namespace-hex "$NS" \
        --data-hex "$DATA_HEX" \
        --ttl "$TTL" \
        --timestamp "$TS" 2>/dev/null)

    ACTUAL_DIGEST=$(echo "$VERIFY_JSON" | jq -r '.signing_digest')

    if [[ "$ACTUAL_DIGEST" != "$EXPECTED_DIGEST" ]]; then
        fail "Blob $INDEX: signing digest mismatch! expected=$EXPECTED_DIGEST actual=$ACTUAL_DIGEST"
    fi

    log "Blob $INDEX: signing digest matches ($ACTUAL_DIGEST)"
    VERIFIED=$((VERIFIED + 1))
done < "$TMPDIR/blob_fields.txt"

if [[ "$VERIFIED" -ne "$BLOB_COUNT" ]]; then
    fail "Only verified $VERIFIED/$BLOB_COUNT blobs"
fi

# --- Verify sync to node2 ---------------------------------------------------

log "Waiting for sync to node2..."
wait_sync chromatindb-test-node2 "$BLOB_COUNT" 60

NODE2_COUNT=$(get_blob_count chromatindb-test-node2)
log "Node2 blob count: $NODE2_COUNT"

if [[ "$NODE2_COUNT" -lt "$BLOB_COUNT" ]]; then
    fail "Node2 has $NODE2_COUNT blobs, expected $BLOB_COUNT"
fi

# --- Done --------------------------------------------------------------------

pass "CRYPT-01: All $VERIFIED signing digests independently verified, sync confirmed ($NODE2_COUNT blobs on node2)"
