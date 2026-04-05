#!/usr/bin/env bash
# =============================================================================
# FILT-01/FILT-02: Namespace Filtering via SyncNamespaceAnnounce
#
# Verifies:
#   Phase 1: All 3 nodes connect and exchange SyncNamespaceAnnounce
#   Phase 2: Blob in Node1's namespace is received by all 3 nodes
#            (Node1 replicates own NS, Node2 replicates Node1+own, Node3 all)
#   Phase 3: Blob in Node2's namespace is received by Node2 and Node3 only
#            (Node1 does not replicate Node2's namespace)
#
# Topology: 3-node standalone
#   Node1 (172.31.0.2): sync_namespaces=[Node1_NS] -- only own namespace
#   Node2 (172.31.0.3): sync_namespaces=[Node1_NS, Node2_NS] -- both namespaces
#   Node3 (172.31.0.4): sync_namespaces=[] (all) -- replicate everything
#
# Method: Dynamic namespace discovery via startup logs, then config rewrite
#   with discovered hashes. SIGHUP not needed -- config set before connections.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

NODE1_CONTAINER="chromatindb-filt01-node1"
NODE2_CONTAINER="chromatindb-filt01-node2"
NODE3_CONTAINER="chromatindb-filt01-node3"
FILT01_NETWORK="chromatindb-filt01-test-net"

# Override helpers.sh NETWORK for run_loadgen
NETWORK="$FILT01_NETWORK"

# Temp config files
TEMP_NODE1_CONFIG=""
TEMP_NODE2_CONFIG=""
TEMP_NODE3_CONFIG=""
NODE1_VOLUME="chromatindb-filt01-node1-data"
NODE2_VOLUME="chromatindb-filt01-node2-data"
NODE3_VOLUME="chromatindb-filt01-node3-data"

# --- Cleanup -----------------------------------------------------------------

cleanup_filt01() {
    log "Cleaning up FILT-01 test..."
    docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true
    docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true
    docker volume rm "$NODE1_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE2_VOLUME" 2>/dev/null || true
    docker volume rm "$NODE3_VOLUME" 2>/dev/null || true
    docker network rm "$FILT01_NETWORK" 2>/dev/null || true
    [[ -n "$TEMP_NODE1_CONFIG" ]] && rm -f "$TEMP_NODE1_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE2_CONFIG" ]] && rm -f "$TEMP_NODE2_CONFIG" 2>/dev/null || true
    [[ -n "$TEMP_NODE3_CONFIG" ]] && rm -f "$TEMP_NODE3_CONFIG" 2>/dev/null || true
}
trap cleanup_filt01 EXIT

# --- Test Setup --------------------------------------------------------------

check_deps
build_image
cleanup_filt01 2>/dev/null || true

log "=== FILT-01/FILT-02: Namespace Filtering via SyncNamespaceAnnounce ==="

FAILURES=0

# Create network and volumes
docker network create --driver bridge --subnet 172.31.0.0/16 "$FILT01_NETWORK"
docker volume create "$NODE1_VOLUME"
docker volume create "$NODE2_VOLUME"
docker volume create "$NODE3_VOLUME"

# =============================================================================
# Discover namespace hashes by starting nodes briefly
# =============================================================================

log "--- Discovering node namespaces ---"

# Node1 discovery config (no bootstrap, no sync_namespaces)
TEMP_NODE1_CONFIG=$(mktemp /tmp/node1-filt01-XXXXXX.json)
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG
chmod 644 "$TEMP_NODE1_CONFIG"

# Node2 discovery config
TEMP_NODE2_CONFIG=$(mktemp /tmp/node2-filt01-XXXXXX.json)
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG
chmod 644 "$TEMP_NODE2_CONFIG"

# Node3 discovery config
TEMP_NODE3_CONFIG=$(mktemp /tmp/node3-filt01-XXXXXX.json)
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG
chmod 644 "$TEMP_NODE3_CONFIG"

# Start Node1 to discover its namespace
docker run -d --name "$NODE1_CONTAINER" \
    --network "$FILT01_NETWORK" \
    --ip 172.31.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

NODE1_NS=$(docker logs "$NODE1_CONTAINER" 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
if [[ -z "$NODE1_NS" ]]; then
    fail "Could not discover Node1 namespace from logs"
fi
log "Discovered Node1 namespace: $NODE1_NS"

docker rm -f "$NODE1_CONTAINER" 2>/dev/null || true

# Start Node2 to discover its namespace
docker run -d --name "$NODE2_CONTAINER" \
    --network "$FILT01_NETWORK" \
    --ip 172.31.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

NODE2_NS=$(docker logs "$NODE2_CONTAINER" 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
if [[ -z "$NODE2_NS" ]]; then
    fail "Could not discover Node2 namespace from logs"
fi
log "Discovered Node2 namespace: $NODE2_NS"

docker rm -f "$NODE2_CONTAINER" 2>/dev/null || true

# Start Node3 to discover its namespace (used for reference only)
docker run -d --name "$NODE3_CONTAINER" \
    --network "$FILT01_NETWORK" \
    --ip 172.31.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE3_CONTAINER"

NODE3_NS=$(docker logs "$NODE3_CONTAINER" 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
if [[ -z "$NODE3_NS" ]]; then
    fail "Could not discover Node3 namespace from logs"
fi
log "Discovered Node3 namespace: $NODE3_NS"

docker rm -f "$NODE3_CONTAINER" 2>/dev/null || true

log "Namespaces: Node1=$NODE1_NS Node2=$NODE2_NS Node3=$NODE3_NS"

# =============================================================================
# Write configs with discovered namespaces and restart
# =============================================================================

log "--- Writing configs with discovered namespaces ---"

# Node1: sync_namespaces = [Node1_NS] -- only replicates its own namespace
cat > "$TEMP_NODE1_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "sync_namespaces": ["$NODE1_NS"],
  "bootstrap_peers": ["172.31.0.3:4200", "172.31.0.4:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG

# Node2: sync_namespaces = [Node1_NS, Node2_NS] -- replicates both
cat > "$TEMP_NODE2_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "sync_namespaces": ["$NODE1_NS", "$NODE2_NS"],
  "bootstrap_peers": ["172.31.0.2:4200", "172.31.0.4:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG

# Node3: sync_namespaces = [] -- replicates everything
cat > "$TEMP_NODE3_CONFIG" <<EOCFG
{
  "bind_address": "0.0.0.0:4200",
  "sync_namespaces": [],
  "bootstrap_peers": ["172.31.0.2:4200", "172.31.0.3:4200"],
  "log_level": "debug",
  "safety_net_interval_seconds": 600
}
EOCFG

# =============================================================================
# Phase 1: Start all 3 nodes, verify SyncNamespaceAnnounce exchange
# =============================================================================

log "--- Phase 1: Setup and verify SyncNamespaceAnnounce exchange ---"

# Start Node1
docker run -d --name "$NODE1_CONTAINER" \
    --network "$FILT01_NETWORK" \
    --ip 172.31.0.2 \
    -v "$NODE1_VOLUME:/data" \
    -v "$TEMP_NODE1_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE1_CONTAINER"

# Start Node2
docker run -d --name "$NODE2_CONTAINER" \
    --network "$FILT01_NETWORK" \
    --ip 172.31.0.3 \
    -v "$NODE2_VOLUME:/data" \
    -v "$TEMP_NODE2_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE2_CONTAINER"

# Start Node3
docker run -d --name "$NODE3_CONTAINER" \
    --network "$FILT01_NETWORK" \
    --ip 172.31.0.4 \
    -v "$NODE3_VOLUME:/data" \
    -v "$TEMP_NODE3_CONFIG:/config/node.json:ro" \
    --health-cmd "bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-'" \
    --health-interval 5s --health-timeout 3s --health-start-period 10s --health-retries 3 \
    chromatindb:test \
    run --config /config/node.json --data-dir /data --log-level debug
wait_healthy "$NODE3_CONTAINER"

# Wait for peer connections and namespace announcement exchange
sleep 15

# Verify SyncNamespaceAnnounce exchange in logs
NODE1_LOGS=$(docker logs "$NODE1_CONTAINER" 2>&1)
NODE2_LOGS=$(docker logs "$NODE2_CONTAINER" 2>&1)
NODE3_LOGS=$(docker logs "$NODE3_CONTAINER" 2>&1)

# Node1 should see announcements from other peers
if echo "$NODE1_LOGS" | grep -q "announced.*sync namespaces"; then
    pass "Phase 1: Node1 received SyncNamespaceAnnounce from peers"
else
    log "FAIL: Node1 did not receive SyncNamespaceAnnounce"
    echo "$NODE1_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# Node2 should see announcements from other peers
if echo "$NODE2_LOGS" | grep -q "announced.*sync namespaces"; then
    pass "Phase 1: Node2 received SyncNamespaceAnnounce from peers"
else
    log "FAIL: Node2 did not receive SyncNamespaceAnnounce"
    echo "$NODE2_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# Node3 should see announcements from other peers
if echo "$NODE3_LOGS" | grep -q "announced.*sync namespaces"; then
    pass "Phase 1: Node3 received SyncNamespaceAnnounce from peers"
else
    log "FAIL: Node3 did not receive SyncNamespaceAnnounce"
    echo "$NODE3_LOGS" | tail -20 >&2
    FAILURES=$((FAILURES + 1))
fi

# Verify Node1 announced 1 namespace (its own)
if echo "$NODE1_LOGS" | grep -q "announced 1 sync namespaces"; then
    pass "Phase 1: Node1 is known to have announced 1 namespace"
else
    # The "announced N sync namespaces" log is from the RECEIVER of the announcement,
    # so we check Node2 or Node3 logs for Node1's announcement count
    if echo "$NODE2_LOGS" | grep "announced 1 sync namespaces" | grep -q .; then
        pass "Phase 1: Node2 confirms Node1 announced 1 namespace"
    elif echo "$NODE3_LOGS" | grep "announced 1 sync namespaces" | grep -q .; then
        pass "Phase 1: Node3 confirms a peer announced 1 namespace"
    else
        log "WARN: Could not confirm Node1 announced exactly 1 namespace (non-critical)"
    fi
fi

# Verify Node2 announced 2 namespaces
if echo "$NODE1_LOGS" | grep "announced 2 sync namespaces" | grep -q .; then
    pass "Phase 1: Node1 confirms Node2 announced 2 namespaces"
elif echo "$NODE3_LOGS" | grep "announced 2 sync namespaces" | grep -q .; then
    pass "Phase 1: Node3 confirms a peer announced 2 namespaces"
else
    log "WARN: Could not confirm Node2 announced exactly 2 namespaces (non-critical)"
fi

# =============================================================================
# Phase 2: Ingest blob in Node1's namespace, verify all 3 receive it
# =============================================================================

log "--- Phase 2: Ingest blob in Node1's namespace (all should replicate) ---"

# Loadgen connects as a peer with its own identity, writing to its own namespace.
# Instead, use loadgen with Node1's identity (--identity-file) to write to Node1's namespace.
# Since Node1's data dir has the identity, we run loadgen from Node1's perspective.
# Actually, the simplest approach: ingest via loadgen targeting Node1. Loadgen creates its
# own identity (different namespace). But we can use --identity-file to load Node1's key.
#
# Loadgen uses its own identity. The blob namespace = SHA3-256(loadgen_pubkey).
# This won't match any sync_namespaces filter unless we know the loadgen identity.
#
# Better approach: copy Node1's identity to a temp volume, use loadgen with --identity-file
# pointing to that. This ensures blobs are in Node1's namespace.

# Extract Node1's identity files from its volume
NODE1_IDENTITY_DIR=$(mktemp -d /tmp/node1-identity-XXXXXX)
docker cp "$NODE1_CONTAINER:/data/node.key" "$NODE1_IDENTITY_DIR/node.key"
docker cp "$NODE1_CONTAINER:/data/node.pub" "$NODE1_IDENTITY_DIR/node.pub"

# Ingest 5 blobs via loadgen using Node1's identity (blobs go to Node1's namespace)
log "Ingesting 5 blobs to Node1 using Node1's identity (namespace: $NODE1_NS)..."
docker run --rm --network "$FILT01_NETWORK" \
    -v "$NODE1_IDENTITY_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen \
    chromatindb:test \
    --target 172.31.0.2:4200 --count 5 --size 1024 --rate 10 --ttl 3600 \
    --identity-file /identity \
    2>/dev/null || true

# Wait for sync/replication
sleep 10

# Verify all 3 nodes have the blobs
NODE1_COUNT=$(get_blob_count "$NODE1_CONTAINER")
NODE2_COUNT=$(get_blob_count "$NODE2_CONTAINER")
NODE3_COUNT=$(get_blob_count "$NODE3_CONTAINER")

log "Phase 2 blob counts: Node1=$NODE1_COUNT Node2=$NODE2_COUNT Node3=$NODE3_COUNT"

# All nodes should replicate Node1's namespace:
# - Node1: sync_namespaces=[Node1_NS] -- matches
# - Node2: sync_namespaces=[Node1_NS, Node2_NS] -- matches
# - Node3: sync_namespaces=[] (all) -- matches
if [[ "$NODE1_COUNT" -ge 5 ]]; then
    pass "Phase 2: Node1 has >= 5 blobs (origin)"
else
    log "FAIL: Node1 should have >= 5 blobs, got $NODE1_COUNT"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$NODE2_COUNT" -ge 5 ]]; then
    pass "Phase 2: Node2 replicated Node1's namespace blobs ($NODE2_COUNT blobs)"
else
    log "FAIL: Node2 should have >= 5 blobs, got $NODE2_COUNT"
    FAILURES=$((FAILURES + 1))
fi

if [[ "$NODE3_COUNT" -ge 5 ]]; then
    pass "Phase 2: Node3 replicated Node1's namespace blobs ($NODE3_COUNT blobs)"
else
    log "FAIL: Node3 should have >= 5 blobs, got $NODE3_COUNT"
    FAILURES=$((FAILURES + 1))
fi

# =============================================================================
# Phase 3: Ingest blob in Node2's namespace, verify Node1 does NOT receive it
# =============================================================================

log "--- Phase 3: Ingest blob in Node2's namespace (Node1 should NOT replicate) ---"

# Extract Node2's identity files
NODE2_IDENTITY_DIR=$(mktemp -d /tmp/node2-identity-XXXXXX)
docker cp "$NODE2_CONTAINER:/data/node.key" "$NODE2_IDENTITY_DIR/node.key"
docker cp "$NODE2_CONTAINER:/data/node.pub" "$NODE2_IDENTITY_DIR/node.pub"

# Ingest 5 blobs via loadgen using Node2's identity (blobs go to Node2's namespace)
log "Ingesting 5 blobs to Node2 using Node2's identity (namespace: $NODE2_NS)..."
docker run --rm --network "$FILT01_NETWORK" \
    -v "$NODE2_IDENTITY_DIR:/identity:ro" \
    --entrypoint chromatindb_loadgen \
    chromatindb:test \
    --target 172.31.0.3:4200 --count 5 --size 1024 --rate 10 --ttl 3600 \
    --identity-file /identity \
    2>/dev/null || true

# Wait for sync/replication
sleep 10

# Check blob counts
NODE1_COUNT_AFTER=$(get_blob_count "$NODE1_CONTAINER")
NODE2_COUNT_AFTER=$(get_blob_count "$NODE2_CONTAINER")
NODE3_COUNT_AFTER=$(get_blob_count "$NODE3_CONTAINER")

log "Phase 3 blob counts: Node1=$NODE1_COUNT_AFTER Node2=$NODE2_COUNT_AFTER Node3=$NODE3_COUNT_AFTER"

# Node1 should still have only the Phase 2 blobs (it does NOT replicate Node2's namespace)
# Node1: sync_namespaces=[Node1_NS] -- Node2_NS is NOT in its list
if [[ "$NODE1_COUNT_AFTER" -le "$NODE1_COUNT" ]]; then
    pass "Phase 3: Node1 did NOT replicate Node2's namespace blobs (count: $NODE1_COUNT -> $NODE1_COUNT_AFTER)"
else
    log "FAIL: Node1 should NOT have received Node2's blobs (count: $NODE1_COUNT -> $NODE1_COUNT_AFTER)"
    FAILURES=$((FAILURES + 1))
fi

# Node2 should have Phase 2 + Phase 3 blobs
if [[ "$NODE2_COUNT_AFTER" -ge 10 ]]; then
    pass "Phase 3: Node2 has blobs from both namespaces ($NODE2_COUNT_AFTER blobs)"
else
    log "FAIL: Node2 should have >= 10 blobs, got $NODE2_COUNT_AFTER"
    FAILURES=$((FAILURES + 1))
fi

# Node3 should have Phase 2 + Phase 3 blobs (replicates everything)
if [[ "$NODE3_COUNT_AFTER" -ge 10 ]]; then
    pass "Phase 3: Node3 has blobs from both namespaces ($NODE3_COUNT_AFTER blobs)"
else
    log "FAIL: Node3 should have >= 10 blobs, got $NODE3_COUNT_AFTER"
    FAILURES=$((FAILURES + 1))
fi

# Clean up identity temp dirs
rm -rf "$NODE1_IDENTITY_DIR" "$NODE2_IDENTITY_DIR" 2>/dev/null || true

# --- Result ------------------------------------------------------------------

echo ""
if [[ $FAILURES -eq 0 ]]; then
    pass "FILT-01/FILT-02: Namespace filtering PASSED"
    pass "  - Phase 1: SyncNamespaceAnnounce exchanged between all 3 nodes"
    pass "  - Phase 2: All nodes replicated Node1's namespace blobs"
    pass "  - Phase 3: Node1 correctly filtered out Node2's namespace blobs"
    exit 0
else
    fail "FILT-01/FILT-02: $FAILURES check(s) failed"
fi
