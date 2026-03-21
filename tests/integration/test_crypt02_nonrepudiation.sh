#!/usr/bin/env bash
# =============================================================================
# CRYPT-02: Non-Repudiation Verification
#
# Verifies that ML-DSA-87 signatures can be independently verified from raw
# field values (signing digest, signature bytes, public key) without needing
# FlatBuffer decoding.
#
# Flow:
#   1. Start 2-node topology
#   2. Ingest 3 blobs via loadgen with --verbose-blobs
#   3. For each blob, verify the ML-DSA-87 signature via chromatindb_verify
#      sig-fields using digest, signature, and pubkey from verbose output
#   4. Negative test: flip one byte of the signature and verify it fails
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

log "=== CRYPT-02: Non-Repudiation Test ==="

start_nodes

# --- Ingest blobs with verbose output ----------------------------------------

BLOB_COUNT=3
BLOB_SIZE=256

log "Ingesting $BLOB_COUNT blobs (${BLOB_SIZE}B each) with --verbose-blobs..."

TMPDIR=$(mktemp -d)
trap 'cleanup; rm -rf "$TMPDIR"' EXIT

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

grep '^BLOB_FIELDS:' "$TMPDIR/stderr.txt" > "$TMPDIR/blob_fields.txt" || true

FIELD_COUNT=$(wc -l < "$TMPDIR/blob_fields.txt")
if [[ "$FIELD_COUNT" -ne "$BLOB_COUNT" ]]; then
    fail "Expected $BLOB_COUNT BLOB_FIELDS lines, got $FIELD_COUNT"
fi

# --- Positive test: verify each blob's signature ----------------------------

VERIFIED=0
while IFS= read -r line; do
    json="${line#BLOB_FIELDS:}"

    INDEX=$(echo "$json" | jq -r '.index')
    DIGEST=$(echo "$json" | jq -r '.signing_digest')
    SIG_HEX=$(echo "$json" | jq -r '.signature_hex')
    PUBKEY_HEX=$(echo "$json" | jq -r '.pubkey_hex')

    log "Blob $INDEX: verifying ML-DSA-87 signature..."

    # Verify signature via chromatindb_verify sig-fields
    VERIFY_JSON=$(docker run --rm "$IMAGE" \
        chromatindb_verify sig-fields \
        --digest-hex "$DIGEST" \
        --signature-hex "$SIG_HEX" \
        --pubkey-hex "$PUBKEY_HEX" 2>/dev/null)

    VALID=$(echo "$VERIFY_JSON" | jq -r '.valid')

    if [[ "$VALID" != "true" ]]; then
        fail "Blob $INDEX: signature verification returned valid=$VALID, expected true"
    fi

    log "Blob $INDEX: signature valid"
    VERIFIED=$((VERIFIED + 1))
done < "$TMPDIR/blob_fields.txt"

if [[ "$VERIFIED" -ne "$BLOB_COUNT" ]]; then
    fail "Only verified $VERIFIED/$BLOB_COUNT blobs (positive test)"
fi

# --- Negative test: tampered signature should fail ---------------------------

log "Running negative test: tampering signature bytes..."

# Use the first blob for the negative test
FIRST_LINE=$(head -1 "$TMPDIR/blob_fields.txt")
json="${FIRST_LINE#BLOB_FIELDS:}"

DIGEST=$(echo "$json" | jq -r '.signing_digest')
SIG_HEX=$(echo "$json" | jq -r '.signature_hex')
PUBKEY_HEX=$(echo "$json" | jq -r '.pubkey_hex')

# Flip one byte in the signature (change first byte)
FIRST_BYTE="${SIG_HEX:0:2}"
# XOR with 0x01 to flip the LSB
FLIPPED=$(printf "%02x" $(( 16#$FIRST_BYTE ^ 1 )))
TAMPERED_SIG="${FLIPPED}${SIG_HEX:2}"

set +e
VERIFY_JSON=$(docker run --rm "$IMAGE" \
    chromatindb_verify sig-fields \
    --digest-hex "$DIGEST" \
    --signature-hex "$TAMPERED_SIG" \
    --pubkey-hex "$PUBKEY_HEX" 2>/dev/null)
EXIT_CODE=$?
set -e

VALID=$(echo "$VERIFY_JSON" | jq -r '.valid')

if [[ "$VALID" != "false" ]]; then
    fail "Tampered signature returned valid=$VALID, expected false"
fi

if [[ "$EXIT_CODE" -eq 0 ]]; then
    fail "Tampered signature exited 0, expected non-zero"
fi

log "Negative test passed: tampered signature correctly rejected (exit=$EXIT_CODE, valid=$VALID)"

# --- Verify sync to node2 ---------------------------------------------------

log "Waiting for sync to node2..."
wait_sync chromatindb-test-node2 "$BLOB_COUNT" 60

NODE2_COUNT=$(get_blob_count chromatindb-test-node2)
log "Node2 blob count: $NODE2_COUNT"

if [[ "$NODE2_COUNT" -lt "$BLOB_COUNT" ]]; then
    fail "Node2 has $NODE2_COUNT blobs, expected $BLOB_COUNT"
fi

# --- Done --------------------------------------------------------------------

pass "CRYPT-02: All $VERIFIED signatures independently verified, tampered signature rejected, sync confirmed ($NODE2_COUNT blobs on node2)"
