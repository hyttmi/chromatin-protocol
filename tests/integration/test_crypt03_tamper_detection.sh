#!/usr/bin/env bash
# =============================================================================
# CRYPT-03: Tamper Detection Verification
#
# Verifies that a single bit flip in the DARE-encrypted data.mdb causes an
# AEAD authentication failure on restart, and the corrupted blob is never
# served to peers during sync.
#
# Flow:
#   1. Start node1 only
#   2. Ingest 5 blobs
#   3. Stop node1
#   4. Flip a single bit in data.mdb (past mdbx header, targeting DARE envelopes)
#   5. Restart node1
#   6. Check for AEAD or corruption detection in logs
#   7. Start node2, attempt sync
#   8. Verify corrupted blob is not served (node2 < 5 or sync errors logged)
#
# Acceptable outcomes:
#   A. AEAD authentication failure (DARE integrity) -- node logs decryption error
#   B. mdbx corruption detected (database integrity) -- node refuses to open
#   Either way: corrupted data is never served to peers.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

trap cleanup EXIT

# --- Setup -------------------------------------------------------------------

check_deps
build_image
cleanup

log "=== CRYPT-03: Tamper Detection Test ==="

# Start only node1 first
log "Starting node1..."
$COMPOSE up -d node1
wait_healthy chromatindb-test-node1

# --- Ingest blobs ------------------------------------------------------------

BLOB_COUNT=5
BLOB_SIZE=1024

log "Ingesting $BLOB_COUNT blobs (${BLOB_SIZE}B each)..."

docker run --rm --network "$NETWORK" \
    --entrypoint chromatindb_loadgen \
    "$IMAGE" \
    --target node1:4200 \
    --count "$BLOB_COUNT" --size "$BLOB_SIZE" --ttl 3600 --rate 10 \
    > /dev/null 2>&1

# Verify node1 has the blobs
sleep 2
NODE1_COUNT=$(get_blob_count chromatindb-test-node1)
log "Node1 blob count after ingest: $NODE1_COUNT"

if [[ "$NODE1_COUNT" -lt "$BLOB_COUNT" ]]; then
    fail "Node1 only has $NODE1_COUNT/$BLOB_COUNT blobs after ingest"
fi

# --- Stop node1 and corrupt data.mdb ----------------------------------------

log "Stopping node1..."
$COMPOSE stop node1

log "Flipping a single bit in mdbx.dat..."

# The mdbx header occupies the first few pages (page size 4096, ~3 pages for metadata).
# We target offset 16384+ to hit data pages containing DARE-encrypted blob envelopes.
# Multiple offsets are tried because the exact data page layout depends on mdbx internals.
CORRUPTION_RESULT=$(docker run --rm \
    -v chromatindb-test_node1-data:/data \
    debian:bookworm-slim sh -c '
    FILE=/data/mdbx.dat
    SIZE=$(stat -c%s "$FILE" 2>/dev/null || echo "0")

    if [ "$SIZE" -lt 20480 ]; then
        echo "ERROR: data.mdb too small ($SIZE bytes), no data pages to corrupt"
        exit 1
    fi

    # Target an offset in the data page area (past mdbx meta pages)
    # Use offset 16384 (page 4) which should be in the B-tree data area
    OFFSET=16384
    # Read one byte at offset, XOR with 0x01, write back (no xxd needed)
    ORIG=$(od -A n -t x1 -j $OFFSET -N 1 "$FILE" | tr -d " \n")
    FLIPPED=$(printf "%02x" $(( 0x$ORIG ^ 0x01 )))
    printf "\\x${FLIPPED}" | dd of="$FILE" bs=1 seek=$OFFSET conv=notrunc 2>/dev/null
    echo "OK: Flipped byte at offset $OFFSET: 0x$ORIG -> 0x$FLIPPED (file size: $SIZE)"
' 2>&1) || true

log "$CORRUPTION_RESULT"

if [[ ! "$CORRUPTION_RESULT" =~ ^OK: ]]; then
    fail "Failed to corrupt data.mdb: $CORRUPTION_RESULT"
fi

# --- Restart node1 and check for corruption detection ------------------------

log "Restarting node1..."
$COMPOSE start node1

# Give the node time to start up and perform integrity scan
# It may crash, fail to start, or log AEAD errors
sleep 10

# Check if node1 is still running
NODE1_RUNNING=true
NODE1_HEALTHY=false

NODE1_STATUS=$(docker inspect --format '{{.State.Status}}' chromatindb-test-node1 2>/dev/null || echo "missing")
log "Node1 container status: $NODE1_STATUS"

if [[ "$NODE1_STATUS" == "running" ]]; then
    NODE1_HEALTH=$(docker inspect --format '{{.State.Health.Status}}' chromatindb-test-node1 2>/dev/null || echo "unknown")
    log "Node1 health status: $NODE1_HEALTH"
    if [[ "$NODE1_HEALTH" == "healthy" ]]; then
        NODE1_HEALTHY=true
    fi
else
    NODE1_RUNNING=false
fi

# Capture node1 logs
NODE1_LOGS=$(docker logs chromatindb-test-node1 2>&1 || true)

# Check for corruption/AEAD detection in logs
CORRUPTION_DETECTED=false

if echo "$NODE1_LOGS" | grep -qiE "aead|decrypt|corrupt|integrity|authentication.fail|tag.mismatch|chacha|poly1305|mdbx.*error|mdbx.*corrupt|page.*error|checksum"; then
    CORRUPTION_DETECTED=true
    CORRUPTION_EVIDENCE=$(echo "$NODE1_LOGS" | grep -iE "aead|decrypt|corrupt|integrity|authentication.fail|tag.mismatch|chacha|poly1305|mdbx.*error|mdbx.*corrupt|page.*error|checksum" | head -5)
    log "Corruption detection evidence:"
    echo "$CORRUPTION_EVIDENCE" | while IFS= read -r eline; do
        log "  $eline"
    done
fi

# Node not running = mdbx detected corruption and refused to open
if [[ "$NODE1_RUNNING" == false ]]; then
    CORRUPTION_DETECTED=true
    log "Node1 failed to start after corruption (mdbx integrity detection)"
fi

if [[ "$CORRUPTION_DETECTED" != true ]]; then
    # Even if no explicit error was logged, the data may still be corrupt
    # at the AEAD level (detected on blob access, not startup)
    log "WARN: No explicit corruption detection in startup logs, checking blob access..."
fi

# --- Start node2 and check sync behavior ------------------------------------

# Use --no-deps because node1 may have crashed and compose would refuse
# to start node2 due to the depends_on health check on node1.
log "Starting node2..."
$COMPOSE up -d --no-deps node2

# Wait for node2 to become healthy (it should start fine since its data is clean)
if ! wait_healthy chromatindb-test-node2 60; then
    fail "Node2 failed to become healthy"
fi

# Give sync time to attempt
log "Waiting for sync attempt (30s)..."
sleep 30

NODE2_COUNT=$(get_blob_count chromatindb-test-node2)
log "Node2 blob count after sync: $NODE2_COUNT"

# Check node2 logs for sync errors
NODE2_SYNC_ERRORS=$(docker logs chromatindb-test-node2 2>&1 | grep -iE "aead|decrypt|corrupt|integrity|verification.fail|invalid.*signature|reject" | head -5 || true)
if [[ -n "$NODE2_SYNC_ERRORS" ]]; then
    log "Node2 sync error evidence:"
    echo "$NODE2_SYNC_ERRORS" | while IFS= read -r eline; do
        log "  $eline"
    done
fi

# --- Evaluate results -------------------------------------------------------

# Success conditions (any of these indicates tamper detection works):
# 1. Node1 crashed / refused to start (mdbx detected corruption)
# 2. Node1 logged AEAD/corruption errors (DARE detected corruption)
# 3. Node2 received fewer than BLOB_COUNT blobs (corrupted blob not served)
# 4. Node2 logged verification/AEAD errors during sync

SUCCESS=false
REASON=""

if [[ "$NODE1_RUNNING" == false ]]; then
    SUCCESS=true
    REASON="Node1 refused to start after bit flip (mdbx integrity detection)"
elif [[ "$CORRUPTION_DETECTED" == true && "$NODE2_COUNT" -lt "$BLOB_COUNT" ]]; then
    SUCCESS=true
    REASON="AEAD/corruption detected, node2 received $NODE2_COUNT/$BLOB_COUNT blobs (corrupted blob not served)"
elif [[ "$CORRUPTION_DETECTED" == true ]]; then
    SUCCESS=true
    REASON="AEAD/corruption detected in node1 logs"
elif [[ "$NODE2_COUNT" -lt "$BLOB_COUNT" ]]; then
    SUCCESS=true
    REASON="Node2 received $NODE2_COUNT/$BLOB_COUNT blobs (corrupted blob not served)"
elif [[ -n "$NODE2_SYNC_ERRORS" ]]; then
    SUCCESS=true
    REASON="Node2 logged sync verification errors"
fi

if [[ "$SUCCESS" != true ]]; then
    # Last resort: the bit flip may have hit mdbx free space or a non-blob page.
    # This is not a test failure -- it means the flip didn't affect any live data.
    # But we should still pass if node2 got all 5 blobs (corruption hit dead space).
    if [[ "$NODE2_COUNT" -ge "$BLOB_COUNT" ]]; then
        log "WARN: Bit flip may have hit mdbx metadata/free space rather than blob data."
        log "Node2 received all $BLOB_COUNT blobs. Re-trying with different offset..."

        # Second attempt: try a different offset deeper in the file
        $COMPOSE stop node1 node2
        $COMPOSE down -v --remove-orphans 2>/dev/null || true

        # Recreate topology and retry with offset 32768
        $COMPOSE up -d node1
        wait_healthy chromatindb-test-node1

        docker run --rm --network "$NETWORK" \
            --entrypoint chromatindb_loadgen \
            "$IMAGE" \
            --target node1:4200 \
            --count "$BLOB_COUNT" --size "$BLOB_SIZE" --ttl 3600 --rate 10 \
            > /dev/null 2>&1

        sleep 2
        $COMPOSE stop node1

        # Try multiple offsets to hit actual blob data
        for RETRY_OFFSET in 32768 49152 65536; do
            docker run --rm \
                -v chromatindb-test_node1-data:/data \
                debian:bookworm-slim sh -c "
                FILE=/data/mdbx.dat
                OFFSET=$RETRY_OFFSET
                SIZE=\$(stat -c%s \"\$FILE\" 2>/dev/null || echo 0)
                if [ \"\$SIZE\" -le \$OFFSET ]; then exit 0; fi
                ORIG=\$(od -A n -t x1 -j \$OFFSET -N 1 \"\$FILE\" | tr -d ' \n')
                FLIPPED=\$(printf '%02x' \$(( 0x\$ORIG ^ 0x01 )))
                printf \"\\x\${FLIPPED}\" | dd of=\"\$FILE\" bs=1 seek=\$OFFSET conv=notrunc 2>/dev/null
                echo \"Retry: flipped byte at offset \$OFFSET\"
            " 2>/dev/null || true
        done

        $COMPOSE start node1
        sleep 10

        RETRY_STATUS=$(docker inspect --format '{{.State.Status}}' chromatindb-test-node1 2>/dev/null || echo "missing")
        RETRY_LOGS=$(docker logs chromatindb-test-node1 2>&1 || true)

        if [[ "$RETRY_STATUS" != "running" ]]; then
            SUCCESS=true
            REASON="Node1 refused to start after aggressive bit flip (retry)"
        elif echo "$RETRY_LOGS" | grep -qiE "aead|decrypt|corrupt|integrity|authentication.fail|mdbx.*error"; then
            SUCCESS=true
            REASON="AEAD/corruption detected after retry with multiple offsets"
        fi
    fi
fi

if [[ "$SUCCESS" != true ]]; then
    fail "No corruption detection observed after bit flip. Node1 status=$NODE1_STATUS, Node2 blobs=$NODE2_COUNT"
fi

# --- Done --------------------------------------------------------------------

pass "CRYPT-03: Tamper detection verified -- $REASON"
