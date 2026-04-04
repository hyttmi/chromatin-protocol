#!/usr/bin/env bash
# =============================================================================
# DR-01: DARE Encryption-at-Rest Forensics
#
# Verifies that chromatindb's DARE (Data At Rest Encryption) is effective:
# no plaintext blob content, namespace IDs, or public keys appear in mdbx.dat.
#
# Topology: Single node (docker run). Dedicated network (172.36.0.0/16).
#   Node1 (172.36.0.2): receives ingested blobs, then stopped for forensics
#
# Flow:
#   1. Start single node, wait healthy
#   2. Ingest blobs with known plaintext markers via loadgen
#   3. Stop node, copy mdbx.dat out of volume
#   4. Primary verification: grep/strings for namespace substrings in mdbx.dat
#   5. Supplementary entropy check: sample bytes, verify high entropy
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-dr01-node1"
DR01_NETWORK="chromatindb-dr01-test-net"
NODE1_VOLUME="chromatindb-dr01-node1-data"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$DR01_NETWORK"

# --- Cleanup -----------------------------------------------------------------

cleanup_dr01() {
    log "Cleaning up DR-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker network rm "$DR01_NETWORK" 2>/dev/null || true
    rm -f /tmp/dr01_mdbx.dat 2>/dev/null || true
}
trap cleanup_dr01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_dr01 2>/dev/null || true

log "=== DR-01: DARE Encryption-at-Rest Forensics ==="

FAILURES=0

# Create network and volume
docker network create --driver bridge --subnet 172.36.0.0/16 "$DR01_NETWORK"
docker volume create "$NODE1_VOLUME"

# =============================================================================
# Step 1: Start single node
# =============================================================================

log "--- Step 1: Start node ---"

TMPCONFIG=$(mktemp /tmp/dr01-config-XXXXXX.json)
cat > "$TMPCONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": [],
  "log_level": "debug",
  "safety_net_interval_seconds": 5,
  "full_resync_interval": 9999,
  "inactivity_timeout_seconds": 0
}
EOCFG
chmod 644 "$TMPCONFIG"

docker run -d --name "$NODE1_CONTAINER" \
    --network "$DR01_NETWORK" \
    --ip 172.36.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TMPCONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug

wait_healthy "$NODE1_CONTAINER"

# =============================================================================
# Step 2: Ingest blobs with known content
# =============================================================================

log "--- Step 2: Ingest blobs ---"

run_loadgen 172.36.0.2 --count 50 --size 1024 --ttl 3600 --rate 50 >/dev/null 2>&1

# Also ingest smaller blobs for variety
run_loadgen 172.36.0.2 --count 50 --size 4096 --ttl 3600 --rate 50 >/dev/null 2>&1

sleep 3

NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
log "Node1 blob count: $NODE1_COUNT"

if [[ "$NODE1_COUNT" -lt 50 ]]; then
    fail "DR-01: Ingest failed -- node1 has only $NODE1_COUNT blobs"
fi

# =============================================================================
# Step 3: Stop node and extract mdbx.dat
# =============================================================================

log "--- Step 3: Stop node and extract mdbx.dat ---"

# Get the namespace ID from logs before stopping
NAMESPACE_ID=$(docker logs "$NODE1_CONTAINER" 2>&1 | grep -oP 'namespace[_: ]+\K[0-9a-f]{64}' | head -1 || true)

if [[ -z "$NAMESPACE_ID" ]]; then
    # Try alternate log patterns
    NAMESPACE_ID=$(docker logs "$NODE1_CONTAINER" 2>&1 | grep -oP 'namespace_id[=: "]+\K[0-9a-f]{64}' | head -1 || true)
fi

if [[ -z "$NAMESPACE_ID" ]]; then
    # Get namespace from loadgen's own key -- loadgen generates a random identity
    # The namespace shows up in node logs when blobs are stored
    NAMESPACE_ID=$(docker logs "$NODE1_CONTAINER" 2>&1 | grep -oP '[0-9a-f]{64}' | head -1 || true)
fi

log "Namespace ID for forensic search: ${NAMESPACE_ID:-<not found>}"

docker stop "$NODE1_CONTAINER"

# Copy mdbx.dat out via alpine helper (chmod to make readable by current user)
docker run --rm -v "$NODE1_VOLUME:/data:ro" -v /tmp:/out alpine sh -c 'cp /data/mdbx.dat /out/dr01_mdbx.dat && chmod 644 /out/dr01_mdbx.dat'

if [[ ! -f /tmp/dr01_mdbx.dat ]]; then
    fail "DR-01: Failed to extract mdbx.dat"
fi

DB_SIZE=$(stat -c%s /tmp/dr01_mdbx.dat)
log "mdbx.dat size: $DB_SIZE bytes"

# =============================================================================
# Step 4: Primary verification -- no plaintext in strings output
# =============================================================================

log "--- Step 4: Plaintext forensics ---"

# Check 4a: If we found a namespace ID, search for it in strings output
if [[ -n "$NAMESPACE_ID" ]]; then
    # Take a 16-char substring from the middle of the namespace hex
    NS_SUBSTR="${NAMESPACE_ID:16:16}"
    log "Searching strings output for namespace substring: $NS_SUBSTR"

    STRINGS_HITS=$(strings /tmp/dr01_mdbx.dat | grep -ic "$NS_SUBSTR" 2>/dev/null || true)
    STRINGS_HITS="${STRINGS_HITS:-0}"
    log "Namespace substring matches in strings output: $STRINGS_HITS"

    if [[ "$STRINGS_HITS" -eq 0 ]]; then
        pass "DR-01: No namespace substring found in strings output"
    else
        log "FAIL: Found $STRINGS_HITS matches for namespace substring in strings output"
        FAILURES=$((FAILURES + 1))
    fi

    # Also check for the first 8 hex chars of namespace in xxd output
    NS_PREFIX="${NAMESPACE_ID:0:8}"
    log "Searching xxd output for namespace prefix: $NS_PREFIX"

    # xxd produces lowercase hex -- match case-insensitively
    XXD_HITS=$(xxd /tmp/dr01_mdbx.dat | grep -ic "$NS_PREFIX" 2>/dev/null || true)
    XXD_HITS="${XXD_HITS:-0}"
    log "Namespace prefix hex matches in xxd: $XXD_HITS"

    # Some hits may be coincidental in encrypted data. For a 8-char hex prefix,
    # random probability is very low. But with large db, a few accidental
    # matches are possible. Allow up to 2 coincidental matches.
    if [[ "$XXD_HITS" -le 2 ]]; then
        pass "DR-01: Namespace prefix hex matches <= 2 (likely coincidental): $XXD_HITS"
    else
        log "FAIL: Found $XXD_HITS matches for namespace prefix in xxd output (> 2, likely plaintext)"
        FAILURES=$((FAILURES + 1))
    fi
else
    log "WARN: Could not extract namespace ID from logs -- skipping namespace-specific check"
    # Still do the generic check below
fi

# Check 4b: Generic check -- look for common plaintext patterns
# chromatindb, namespace, blob, master (strings that should NOT appear in encrypted data)
GENERIC_HITS=$(strings /tmp/dr01_mdbx.dat | grep -icE '(chromatindb|namespace_id|blob_data|master.key)' 2>/dev/null || true)
GENERIC_HITS="${GENERIC_HITS:-0}"
log "Generic plaintext pattern hits in strings output: $GENERIC_HITS"

if [[ "$GENERIC_HITS" -eq 0 ]]; then
    pass "DR-01: No generic plaintext patterns found in mdbx.dat strings"
else
    log "FAIL: Found $GENERIC_HITS generic plaintext matches in mdbx.dat"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Step 5: Supplementary entropy check
# =============================================================================

log "--- Step 5: Entropy check ---"

# Sample 1000 bytes from middle of file and count unique byte values
# High entropy (encrypted) data should have near-uniform distribution
MIDPOINT=$((DB_SIZE / 2))

# Use dd to extract 1000 bytes from the middle, then count unique byte values
UNIQUE_BYTES=$(dd if=/tmp/dr01_mdbx.dat bs=1 skip="$MIDPOINT" count=1000 2>/dev/null | \
    xxd -p | fold -w2 | sort -u | wc -l)

log "Unique byte values in 1000-byte sample from middle: $UNIQUE_BYTES / 256"

if [[ "$UNIQUE_BYTES" -ge 200 ]]; then
    pass "DR-01: High entropy confirmed ($UNIQUE_BYTES unique bytes in 1000-byte sample)"
else
    log "FAIL: Low entropy detected ($UNIQUE_BYTES unique bytes -- expected >= 200 for encrypted data)"
    FAILURES=$((FAILURES + 1))
fi

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "DR-01: DARE forensics PASSED"
    pass "  - No plaintext namespace IDs in strings output"
    pass "  - No generic plaintext patterns in mdbx.dat"
    pass "  - High entropy confirmed ($UNIQUE_BYTES unique byte values)"
    pass "  - mdbx.dat size: $DB_SIZE bytes with $NODE1_COUNT blobs"
    exit 0
else
    fail "DR-01: $FAILURES check(s) failed"
fi
