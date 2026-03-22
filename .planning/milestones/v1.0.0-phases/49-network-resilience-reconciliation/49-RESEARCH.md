# Phase 49: Network Resilience & Reconciliation - Research

**Researched:** 2026-03-21
**Domain:** Docker integration tests for network partition healing, sync cursor resumption, crash recovery, large blob integrity, late-joiner at scale, and set reconciliation protocol correctness
**Confidence:** HIGH

## Summary

Phase 49 implements 10 Docker multi-node integration tests verifying that the sync protocol delivers eventual consistency under network partitions, node crashes, scale, and edge cases. The codebase already has all sync, reconciliation, cursor, and crash recovery infrastructure fully implemented and unit-tested (408+ Catch2 tests). The Phase 47-48 test harness (helpers.sh, run-integration.sh, docker-compose patterns, loadgen with all needed flags) is fully reusable. No code changes are needed -- this is pure test-only work.

The key technical challenge is **network partitioning via iptables**. Docker containers run as non-root by default (the Dockerfile sets `USER chromatindb`), so iptables cannot run inside the application containers. The solution is to use a sidecar container (nicolaka/netshoot, already proven in Phase 47 CRYPT-04) with `NET_ADMIN` capability to inject iptables rules on the shared Docker bridge network. This approach was validated by the tcpdump pattern in test_crypt04_forward_secrecy.sh.

The second challenge is **traffic measurement** for RECON-01 (O(diff) scaling) and NET-04 (sync cursor resumption). Since all wire traffic is AEAD-encrypted, we cannot parse payloads via tcpdump. Instead, measure total bytes transferred on port 4200 via iptables byte counters (available on the bridge network) or tcpdump file sizes during isolated sync rounds. The cursor_hits/cursor_misses metrics from SIGUSR1 provide the primary verification mechanism -- a cursor hit means the namespace was skipped entirely (zero traffic).

**Primary recommendation:** Use a single 5-node docker-compose.mesh.yml topology that serves NET-01, NET-02, NET-06, and partially NET-03. Separate 2-node topologies for NET-04, NET-05, RECON-01 through RECON-04. Use nicolaka/netshoot sidecars for iptables partitioning (NET-01, NET-02) and traffic measurement (RECON-01, NET-04). All tests are pure bash scripts following the established Phase 47-48 patterns.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| NET-01 | 5-node mesh partition healing via iptables, converge to identical blob set | XOR fingerprint comparison via ReconcileInit; iptables DROP rules via netshoot sidecar; SIGUSR1 blob count verification across all 5 nodes |
| NET-02 | 4-node split-brain with independent writes, heal produces union of all blobs | Same iptables approach; content-addressed blobs are inherently conflict-free; blob count = sum of both partitions' writes |
| NET-03 | Large blob integrity at 1K/100K/1M/10M/100M sizes with hash verification | Loadgen `--size` flag for exact sizes; chromatindb_verify `hash-fields` for independent hash verification; `--verbose-blobs` for capturing signing digests |
| NET-04 | Sync cursor resumption: stopped/restarted node syncs only new blobs | SyncCursor persisted in MDBX cursor sub-database; cursor_hits metric proves namespace skip; iptables byte counters or tcpdump file size for traffic proportionality |
| NET-05 | Kill-9 during reconciliation recovers cleanly on restart | MDBX ACID transactions survive SIGKILL; integrity_scan on startup; deploy/test-crash-recovery.sh pattern already proven |
| NET-06 | Late-joiner catches up to 10,000 blobs across multiple namespaces | Loadgen with multiple identities (--identity-save/--identity-file) for multi-namespace; SIGUSR1 blob count and fingerprint matching |
| RECON-01 | O(diff) wire traffic: 10 new blobs on 10,000-blob namespace proportional to ~10 blobs | XOR fingerprint reconciliation with SPLIT_THRESHOLD=16; measure bytes via iptables counters or tcpdump capture size during isolated sync |
| RECON-02 | Identical namespaces skip reconciliation (no ReconcileInit sent) | cursor_hits metric in SIGUSR1; log inspection for "sync cursor hit" debug message; namespace skip happens when cursor seq_num matches peer's latest_seq_num |
| RECON-03 | Unknown version byte in ReconcileInit rejected gracefully | decode_reconcile_init returns nullopt when version != RECONCILE_VERSION (0x01); responder logs "invalid ReconcileInit" and continues; no crash or state corruption |
| RECON-04 | 5000-blob full transfer with zero duplicates | Content-addressed dedup in storage (StoreResult::Duplicate); SIGUSR1 blob count must equal exactly 5000; no missed blobs verified via count match |
</phase_requirements>

## Standard Stack

### Core (existing -- no new libraries)
| Component | Version | Purpose | Status |
|-----------|---------|---------|--------|
| Docker Compose | v2 | Multi-node test topologies | Existing patterns from Phase 47-48 |
| helpers.sh | N/A | Docker orchestration primitives | Existing, fully reusable |
| chromatindb_loadgen | Built from source | Protocol-compliant blob injection | Has --size, --count, --rate, --identity-file, --identity-save, --namespace, --verbose-blobs |
| chromatindb_verify | Built from source | Independent crypto verification | Has hash-fields, sig-fields subcommands |
| chromatindb:test | Docker image | Node under test | Built by test runner |
| nicolaka/netshoot | Latest | Network toolbox (iptables, tcpdump) | Proven in Phase 47 (CRYPT-04 tcpdump capture) |
| bash/jq | System | Test scripting and JSON parsing | Already required |

### Supporting Tools
| Tool | Purpose | When to Use |
|------|---------|-------------|
| iptables (via netshoot) | Network partitioning (DROP rules between specific IPs) | NET-01, NET-02 partition tests |
| tcpdump (via netshoot) | Traffic capture for byte measurement | RECON-01 O(diff) verification, NET-04 traffic proportionality |
| docker kill --signal=KILL | SIGKILL for crash simulation | NET-05 crash recovery test |
| docker start | Restart killed container (preserves volume) | NET-04, NET-05 restart after stop/kill |

### No New Tools Needed
All required capabilities exist in the current toolchain. The loadgen `--namespace` flag (added in Phase 48) enables multi-namespace testing for NET-06. The `--identity-save`/`--identity-file` flags enable multi-identity testing. The netshoot container pattern is proven from CRYPT-04.

## Architecture Patterns

### Test Script Structure (from Phase 47-48)
```
tests/integration/
  helpers.sh                          # Shared primitives (reuse as-is)
  run-integration.sh                  # Discovery + runner (auto-discovers test_*.sh)
  docker-compose.mesh.yml             # NEW: 5-node mesh topology (fixed IPs)
  docker-compose.partition.yml        # NEW: 4-node partition topology (fixed IPs)
  docker-compose.recon.yml            # NEW: 2-node reconciliation topology (fixed IPs)
  configs/
    node{1-5}-mesh.json               # NEW: 5-node mesh configs
    node{1-4}-partition.json           # NEW: 4-node partition configs
    node{1-2}-recon.json               # NEW: 2-node recon configs
  test_net01_partition_healing.sh      # NEW
  test_net02_split_brain.sh            # NEW
  test_net03_large_blob_integrity.sh   # NEW
  test_net04_cursor_resumption.sh      # NEW
  test_net05_crash_recovery.sh         # NEW
  test_net06_late_joiner.sh            # NEW
  test_recon01_diff_scaling.sh         # NEW
  test_recon02_empty_skip.sh           # NEW
  test_recon03_version_compat.sh       # NEW
  test_recon04_large_transfer.sh       # NEW
```

### Per-Test Script Pattern (established)
```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

# Custom cleanup for this test's topology
cleanup_test() { ... }
trap cleanup_test EXIT

check_deps
build_image
cleanup_test 2>/dev/null || true

FAILURES=0
# ... test logic ...
if [[ $FAILURES -eq 0 ]]; then pass "..."; exit 0; else fail "..."; fi
```

### Network Partitioning Pattern (NEW for Phase 49)
```bash
# Start netshoot container on the test network with NET_ADMIN
docker run -d --name netshoot-iptables \
    --network "$TEST_NETWORK" \
    --cap-add NET_ADMIN \
    nicolaka/netshoot \
    sleep infinity

# Create partition: drop traffic between node2 (172.28.0.3) and node3 (172.28.0.4)
docker exec netshoot-iptables iptables -I FORWARD -s 172.28.0.3 -d 172.28.0.4 -j DROP
docker exec netshoot-iptables iptables -I FORWARD -s 172.28.0.4 -d 172.28.0.3 -j DROP

# Heal partition: remove iptables rules
docker exec netshoot-iptables iptables -D FORWARD -s 172.28.0.3 -d 172.28.0.4 -j DROP
docker exec netshoot-iptables iptables -D FORWARD -s 172.28.0.4 -d 172.28.0.3 -j DROP
```

**Important:** iptables FORWARD rules on the bridge network affect inter-container traffic. The netshoot container must be on the same Docker bridge network. The `--cap-add NET_ADMIN` flag gives the container permission to manipulate iptables rules on the host's network namespace (which the bridge shares).

**Alternative (simpler, more reliable):** Use `docker network disconnect` / `docker network connect` to partition. This is deterministic and does not require iptables capabilities. However, it breaks the container's network attachment entirely (including to peers not in the partition). For precise partition control (block only specific pairs), iptables is required.

**Recommended approach for NET-01/NET-02:** Use `docker network disconnect` / `docker network connect` because:
1. It is deterministic and does not require NET_ADMIN
2. The partition topology in NET-01/NET-02 is "group A cannot reach group B" which maps cleanly to disconnecting from the shared network
3. Reconnect restores full connectivity

For NET-01 (5-node mesh, partition between subgroups):
- Create two bridge networks: `mesh-a` and `mesh-b`
- Initially all nodes on one network
- Partition: move some nodes to a separate network
- Heal: reconnect to original network

Actually, the cleanest pattern for iptables-based partitioning that works with Docker bridge networks:

```bash
# The netshoot container shares the HOST network namespace (--net=host)
# and can manipulate iptables rules affecting the bridge
docker run -d --name partition-controller \
    --net=host \
    --cap-add NET_ADMIN \
    --privileged \
    nicolaka/netshoot \
    sleep infinity

# Block traffic between specific container IPs on the docker bridge
docker exec partition-controller iptables -I FORWARD -s 172.28.0.3 -d 172.28.0.4 -j DROP
docker exec partition-controller iptables -I FORWARD -s 172.28.0.4 -d 172.28.0.3 -j DROP
```

### Traffic Measurement Pattern (NEW for Phase 49)
```bash
# Method 1: tcpdump capture file size during a single sync round
docker run -d --name traffic-capture \
    --network "$TEST_NETWORK" \
    --cap-add NET_ADMIN \
    nicolaka/netshoot \
    tcpdump -i any -w /tmp/sync-traffic.pcap -s 0 port 4200

# Wait for sync round to complete
sleep $SYNC_INTERVAL

# Stop capture, copy file, measure size
docker kill --signal SIGINT traffic-capture
sleep 1
docker cp traffic-capture:/tmp/sync-traffic.pcap ./sync-traffic.pcap
TRAFFIC_BYTES=$(stat -c%s ./sync-traffic.pcap)

# Method 2: iptables byte counters (more precise)
docker exec partition-controller iptables -Z FORWARD  # Reset counters
# ... wait for sync ...
BYTES=$(docker exec partition-controller iptables -L FORWARD -v -n | grep "172.28.0.2.*172.28.0.3" | awk '{print $2}')
```

### Multi-Namespace Test Pattern (NEW for NET-06)
```bash
# Generate 3 different identities for multi-namespace blobs
for i in 1 2 3; do
    TMPDIR=$(mktemp -d)
    run_loadgen 172.28.0.2 --count 0 --identity-save "$TMPDIR/id$i"
    # Write blobs under this namespace
    run_loadgen 172.28.0.2 --identity-file "$TMPDIR/id$i" --count 3333 --size 256 --rate 100
done
```

### Anti-Patterns to Avoid
- **Running iptables inside the application container:** The Dockerfile sets `USER chromatindb` (non-root). iptables requires root/NET_ADMIN. Use a separate netshoot container.
- **Assuming immediate convergence after partition heal:** Auto-reconnect has exponential backoff. After partition heal, nodes may take up to `reconnect_backoff` seconds to reconnect. Use `sync_interval_seconds: 5` and wait adequate time (30-60s).
- **Using `docker compose down -v` between test phases:** This destroys volumes (and thus node identity/state). Use `docker stop`/`docker start` for cursor resumption tests. Use named volumes to preserve identity across restarts.
- **Measuring traffic during multi-round reconciliation:** Reconciliation for 10K blobs involves multiple message round-trips. Capture the entire sync session, not just the first packet.
- **Expecting exact byte counts for traffic proportionality:** AEAD encryption adds overhead per message. Compare orders of magnitude (10 blobs of traffic << 10,000 blobs of traffic), not exact byte ratios.
- **Running all 10 tests in sequence without cleanup:** Each test creates Docker networks/containers. Stale resources from one test can interfere with the next. Each test must have independent cleanup.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Network partitioning | Custom proxy or traffic shaper | iptables via netshoot sidecar or docker network disconnect/connect | iptables is the standard Linux network partitioning tool; docker network disconnect is simpler for coarse partitions |
| Blob hash verification | Manual SHA3-256 in bash | chromatindb_verify hash-fields | Exact same hash algorithm (SHA3-256 of namespace\|\|data\|\|ttl\|\|timestamp) |
| Multi-namespace blob injection | Custom protocol client | loadgen with --identity-save/--identity-file/--namespace | Each identity generates a unique namespace (SHA3-256 of ML-DSA-87 pubkey) |
| Traffic measurement | Custom packet parser | tcpdump file size or iptables byte counters | AEAD-encrypted traffic cannot be parsed; total byte volume is the meaningful metric |
| Crash simulation | Custom signal injection | docker kill --signal=KILL + docker start | Standard Docker lifecycle; SIGKILL is not trappable |
| XOR fingerprint verification | External fingerprint calculator | SIGUSR1 metrics blob count + log inspection for reconciliation results | Fingerprints are computed internally by the reconciliation protocol; we verify convergence via blob counts |

## Common Pitfalls

### Pitfall 1: iptables Rules on Docker Bridge
**What goes wrong:** iptables rules applied inside a container on a custom bridge network only affect that container's namespace, not inter-container traffic on the bridge.
**Why it happens:** Docker bridge network creates veth pairs. Container iptables rules apply to the container's own network namespace, not the bridge.
**How to avoid:** Use `--net=host --privileged` for the iptables controller container, which gives access to the host's FORWARD chain. The host's FORWARD chain controls traffic between containers on bridge networks.
**Warning signs:** iptables rules applied but containers still communicate.

### Pitfall 2: Auto-Reconnect Backoff After Partition Heal
**What goes wrong:** After healing a partition (removing iptables rules), nodes don't reconnect immediately. The auto-reconnect timer may be sleeping for 30-600 seconds due to exponential backoff from failed connection attempts during the partition.
**Why it happens:** v0.9.0 auto-reconnect uses exponential backoff (5s, 10s, 20s, 40s, ... up to 600s).
**How to avoid:** Set `sync_interval_seconds: 5` in test configs. After healing, wait up to 60-90 seconds for reconnection. Alternatively, the test can check for "Connected to peer" log lines to confirm reconnection before proceeding. If convergence is too slow, use SIGHUP to force `clear_reconnect_state()` which resets all backoff timers.
**Warning signs:** Tests timeout waiting for convergence after partition heal.

### Pitfall 3: full_resync_interval Interfering with Cursor Tests
**What goes wrong:** The `full_resync_interval` config (default: 10) forces a full resync every 10th sync round, which defeats cursor-based skip detection in RECON-02 and NET-04.
**Why it happens:** Even if cursors match, the round counter triggers periodic full resyncs for safety.
**How to avoid:** Set `full_resync_interval: 0` in test configs for cursor/skip tests (0 = disable periodic full resync). This isolates the cursor mechanism from the periodic override.
**Warning signs:** cursor_misses metric increases even when no new blobs were added.

### Pitfall 4: Blob Count Metric Overcounting in Multi-Namespace Tests
**What goes wrong:** The `blobs=` metric in SIGUSR1 sums `latest_seq_num` across all namespaces, which can overcount when tombstones create sequence gaps (tombstone gets a seq_num but the original blob's seq entry is replaced with zero-hash sentinel).
**Why it happens:** `latest_seq_num` is the high watermark, not the actual live blob count.
**How to avoid:** For NET-06 (10,000 blobs across multiple namespaces), the loadgen injects only new blobs (no tombstones), so `latest_seq_num` equals actual blob count. For tests involving tombstones (none in Phase 49), this would need different verification.
**Warning signs:** Blob counts don't match expected values despite successful sync.

### Pitfall 5: Large Blob Test Memory Pressure
**What goes wrong:** NET-03 requires syncing a 100 MiB blob. The one-blob-at-a-time sync protocol keeps memory bounded, but the Docker container's default memory limits may cause OOM kills.
**Why it happens:** The container has no explicit memory limit by default, but the system may be constrained.
**How to avoid:** Don't set docker memory limits for NET-03. The one-blob-at-a-time sync design (Phase 39) specifically prevents memory blowup. Monitor for OOM kill via `docker inspect --format '{{.State.OOMKilled}}'`.
**Warning signs:** Container exits with code 137 (SIGKILL from OOM killer).

### Pitfall 6: RECON-03 Version Byte Test Requires Custom Wire Injection
**What goes wrong:** There is no loadgen flag to send malformed ReconcileInit messages with a wrong version byte.
**Why it happens:** Loadgen and chromatindb nodes always use RECONCILE_VERSION (0x01).
**How to avoid:** Use `docker exec` to inject a raw TCP payload into a running node via a small script, OR rely on the unit test coverage of `decode_reconcile_init` which already tests version mismatch (returns nullopt). For the Docker integration test, the simplest approach is to verify the responder behavior: send a raw malformed ReconcileInit via a simple TCP client (netcat/socat from the netshoot container). However, since all wire traffic is AEAD-encrypted after handshake, injecting a malformed ReconcileInit into an active session is not possible from outside. The practical test: verify via unit test coverage that `decode_reconcile_init` returns nullopt for wrong version, and verify via log inspection that a future version does not crash the node. An alternative: modify the test to start two nodes where one is configured to skip reconciliation but the other initiates -- but this doesn't test the version byte. The realistic approach is to verify the behavior through the existing unit test (`decode_reconcile_init` with wrong version returns nullopt) and document that the Docker test confirms no crash/corruption when the responder encounters invalid ReconcileInit (which `continue` handles in handle_sync_as_responder).
**Warning signs:** Test tries to inject plaintext into AEAD-encrypted connection.

## Code Examples

### NET-01: Partition Healing Test Flow
```bash
# 5-node mesh, all connected via bootstrap_peers chain
# node1 -> node2 -> node3 -> node4 -> node5 (plus some cross-links)

# Start all 5 nodes on 172.28.0.0/16
# Ingest 1000 blobs to node1, wait for full cluster sync
run_loadgen 172.28.0.2 --count 1000 --size 256 --rate 100
wait_sync chromatindb-test-node5 1000 120  # Verify full propagation

# Create partition: disconnect node3/node4/node5 from node1/node2
docker exec partition-ctrl iptables -I FORWARD -s 172.28.0.2 -d 172.28.0.4 -j DROP
docker exec partition-ctrl iptables -I FORWARD -s 172.28.0.4 -d 172.28.0.2 -j DROP
docker exec partition-ctrl iptables -I FORWARD -s 172.28.0.3 -d 172.28.0.4 -j DROP
docker exec partition-ctrl iptables -I FORWARD -s 172.28.0.4 -d 172.28.0.3 -j DROP
# ... (similar for all cross-partition pairs)

# Ingest 200 more blobs to node1 (only node1/node2 receive them)
run_loadgen 172.28.0.2 --count 200 --size 256 --rate 50

# Verify partition works: node5 should NOT have the 200 new blobs
BLOB_COUNT_5=$(get_blob_count chromatindb-test-node5)
[[ "$BLOB_COUNT_5" -eq 1000 ]] || fail "Partition leak: node5 has $BLOB_COUNT_5 blobs"

# Heal partition: remove all iptables DROP rules
docker exec partition-ctrl iptables -F FORWARD

# Wait for convergence (auto-reconnect + sync)
sleep 90

# Verify all 5 nodes have 1200 blobs
for node in node1 node2 node3 node4 node5; do
    COUNT=$(get_blob_count "chromatindb-test-$node")
    [[ "$COUNT" -ge 1200 ]] || fail "$node has $COUNT blobs, expected 1200"
done
```

### NET-04: Cursor Resumption Test Flow
```bash
# 2-node topology, full_resync_interval: 0 (disable periodic full resync)
# Ingest 500 blobs, wait for sync, verify cursor stored
run_loadgen 172.28.0.2 --count 500 --size 256 --rate 100
wait_sync chromatindb-test-node2 500

# Stop node2 (preserves volumes)
docker stop chromatindb-test-node2

# Ingest 100 more blobs to node1
run_loadgen 172.28.0.2 --count 100 --size 256 --rate 50

# Start tcpdump to measure traffic during resumption sync
docker run -d --name traffic-monitor ...

# Restart node2
docker start chromatindb-test-node2
wait_healthy chromatindb-test-node2

# Wait for sync round
sleep 30

# Stop traffic capture
docker kill --signal SIGINT traffic-monitor
docker cp traffic-monitor:/tmp/capture.pcap ./resume-traffic.pcap
RESUME_BYTES=$(stat -c%s ./resume-traffic.pcap)

# Compare against baseline: full sync of 600 blobs
# (Captured earlier or computed from known blob sizes)
# Resume traffic should be << full sync traffic
# Additional verification: SIGUSR1 metrics show cursor_hits > 0
docker kill -s USR1 chromatindb-test-node2
sleep 2
CURSOR_HITS=$(docker logs chromatindb-test-node2 2>&1 | grep "metrics:" | tail -1 | grep -oP 'cursor_hits=\K[0-9]+')
[[ "$CURSOR_HITS" -gt 0 ]] || fail "No cursor hits detected"

# Verify node2 has all 600 blobs
COUNT=$(get_blob_count chromatindb-test-node2)
[[ "$COUNT" -ge 600 ]] || fail "Node2 has $COUNT blobs, expected 600"
```

### RECON-01: O(diff) Scaling Test Flow
```bash
# 2-node topology, preload 10,000 blobs on both nodes via sync
run_loadgen 172.28.0.2 --count 10000 --size 256 --rate 200
wait_sync chromatindb-test-node2 10000 300

# Baseline: capture traffic for a sync round with zero diffs
docker run -d --name capture-zero ... tcpdump -i any -w /tmp/zero-diff.pcap -s 0 port 4200
sleep $((SYNC_INTERVAL + 5))
docker kill --signal SIGINT capture-zero
docker cp capture-zero:/tmp/zero-diff.pcap ./zero-diff.pcap
ZERO_DIFF_BYTES=$(stat -c%s ./zero-diff.pcap)

# Add 10 new blobs to node1 only (stop node2 first, then restart)
docker stop chromatindb-test-node2
run_loadgen 172.28.0.2 --count 10 --size 256
docker start chromatindb-test-node2

# Capture traffic for the diff sync
docker run -d --name capture-diff ... tcpdump -i any -w /tmp/small-diff.pcap -s 0 port 4200
wait_sync chromatindb-test-node2 10010 120
docker kill --signal SIGINT capture-diff
docker cp capture-diff:/tmp/small-diff.pcap ./small-diff.pcap
SMALL_DIFF_BYTES=$(stat -c%s ./small-diff.pcap)

# O(diff) verification: small-diff traffic should be much less than
# transferring all 10,000 blobs. It should be closer to zero-diff + ~10 blobs
# worth of data. A reasonable threshold: small-diff < 10 * zero-diff
```

### RECON-03: Version Byte Forward Compat
```bash
# Since AEAD-encrypted connections prevent injecting malformed messages,
# this test verifies the behavior indirectly:
# 1. Start 2 nodes, let them sync normally
# 2. Check that decode_reconcile_init properly rejects unknown versions
#    (already covered by unit tests)
# 3. Docker integration test verifies: the sync responder logs
#    "invalid ReconcileInit" and continues without crash when receiving
#    a message that fails decode (handles nullopt with continue, not crash)
# 4. Verify the node stays healthy after the sync round completes

# Practical integration test approach:
# Send a valid ReconcileInit payload directly via socat to the raw TCP port.
# This will fail at AEAD decryption (not at ReconcileInit decode), but proves
# the connection handler does not crash on garbage input.
# The real version byte test is the unit test; the Docker test confirms
# overall robustness.
```

## State of the Art

| Component | Current State | Impact on Phase 49 |
|-----------|--------------|-------------------|
| Set Reconciliation | Fully implemented (reconciliation.cpp, Phase 39) | Tests only -- RECONCILE_VERSION=0x01, SPLIT_THRESHOLD=16, MAX_RECONCILE_ROUNDS=64 |
| Sync Cursors | Fully implemented (cursor sub-database, Phase 34) | Tests only -- persisted in MDBX, survives restarts, cursor_hits/misses in metrics |
| Auto-Reconnect | Fully implemented (Phase 42-44, v0.9.0) | Tests rely on this for partition healing -- exponential backoff, ACL suppression |
| One-Blob-at-a-Time Sync | Fully implemented (Phase 39) | Bounded memory for large blob tests (NET-03 100 MiB) |
| Connection Dedup | Fully implemented (Phase 48) | 5-node mesh will produce exactly 4 logical connections per node |
| XOR Fingerprint | Fully implemented (xor_fingerprint in reconciliation.cpp) | Used internally by reconciliation; verified via convergence (blob count match) |
| Crash Recovery | MDBX ACID (inherent) | Already verified in deploy/test-crash-recovery.sh; NET-05 adds mid-reconciliation kill |
| SIGUSR1 Metrics | Fully implemented (peers, blobs, storage, syncs, cursor_hits, cursor_misses, full_resyncs) | Primary verification mechanism for most tests |
| Loadgen --identity-save/--identity-file | Fully implemented (Phase 48) | Multi-namespace support for NET-06 |
| Loadgen --namespace | Fully implemented (Phase 48) | Delegation writes to specific namespace |

## Docker Topologies

### 5-Node Mesh (docker-compose.mesh.yml) -- NET-01, NET-06
```
Network: 172.28.0.0/16
Node1: 172.28.0.2 (no bootstrap -- seed node)
Node2: 172.28.0.3 (bootstrap: [node1:4200])
Node3: 172.28.0.4 (bootstrap: [node1:4200, node2:4200])
Node4: 172.28.0.5 (bootstrap: [node2:4200, node3:4200])
Node5: 172.28.0.6 (bootstrap: [node3:4200, node4:4200])

All configs:
  sync_interval_seconds: 5
  full_resync_interval: 0    # Disable periodic full resync for cursor tests
  log_level: debug
```

### 4-Node Partition (docker-compose.partition.yml) -- NET-02
```
Network: 172.28.0.0/16
Node1: 172.28.0.2 (bootstrap: [])
Node2: 172.28.0.3 (bootstrap: [node1:4200])
Node3: 172.28.0.4 (bootstrap: [node1:4200])
Node4: 172.28.0.5 (bootstrap: [node3:4200])

Partition groups: {node1, node2} | {node3, node4}
```

### 2-Node Recon (docker-compose.recon.yml) -- NET-03, NET-04, NET-05, RECON-01-04
```
Network: 172.28.0.0/16
Node1: 172.28.0.2 (no bootstrap)
Node2: 172.28.0.3 (bootstrap: [node1:4200])

Configs:
  sync_interval_seconds: 5
  full_resync_interval: 0
```

## Config Considerations

### Key Config Parameters for Testing
| Parameter | Test Default | Why |
|-----------|-------------|-----|
| `sync_interval_seconds` | 5 | Fast sync cycles for quicker test convergence |
| `full_resync_interval` | 0 | Disable periodic full resync so cursor tests are deterministic |
| `cursor_stale_seconds` | 3600 | Default is fine -- tests won't hit the 1-hour stale threshold |
| `log_level` | debug | Enables cursor hit/miss debug logs and reconciliation details |
| `inactivity_timeout_seconds` | 0 | Disable inactivity timeout -- partitioned nodes should not be timed out |
| `max_peers` | 32 | Default is sufficient for 5-node mesh |

### Critical: Disable inactivity_timeout for Partition Tests
The v0.9.0 inactivity timeout (default 120s) will disconnect idle peers. During a partition, the partitioned peers become "idle" from the connected peers' perspective. If inactivity_timeout fires during the partition, the nodes will drop their connections and need to re-establish them after healing. Set `inactivity_timeout_seconds: 0` for partition tests.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Bash integration tests (shell scripts) |
| Config file | tests/integration/run-integration.sh |
| Quick run command | `bash tests/integration/run-integration.sh --skip-build --filter "net01"` |
| Full suite command | `bash tests/integration/run-integration.sh --skip-build` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| NET-01 | Partition healing, eventual consistency | integration | `bash tests/integration/run-integration.sh --skip-build --filter net01` | Wave 0 |
| NET-02 | Split-brain writes, merge to union | integration | `bash tests/integration/run-integration.sh --skip-build --filter net02` | Wave 0 |
| NET-03 | Large blob integrity (1K-100M) | integration | `bash tests/integration/run-integration.sh --skip-build --filter net03` | Wave 0 |
| NET-04 | Sync cursor resumption | integration | `bash tests/integration/run-integration.sh --skip-build --filter net04` | Wave 0 |
| NET-05 | Crash recovery during reconciliation | integration | `bash tests/integration/run-integration.sh --skip-build --filter net05` | Wave 0 |
| NET-06 | Late-joiner at scale (10K blobs) | integration | `bash tests/integration/run-integration.sh --skip-build --filter net06` | Wave 0 |
| RECON-01 | O(diff) scaling (10 on 10K) | integration | `bash tests/integration/run-integration.sh --skip-build --filter recon01` | Wave 0 |
| RECON-02 | Empty namespace skip | integration | `bash tests/integration/run-integration.sh --skip-build --filter recon02` | Wave 0 |
| RECON-03 | Version byte forward compat | integration | `bash tests/integration/run-integration.sh --skip-build --filter recon03` | Wave 0 |
| RECON-04 | Large difference set (5000 blobs) | integration | `bash tests/integration/run-integration.sh --skip-build --filter recon04` | Wave 0 |

### Sampling Rate
- **Per task commit:** `bash tests/integration/run-integration.sh --skip-build --filter <test_name>`
- **Per wave merge:** `bash tests/integration/run-integration.sh --skip-build --filter "net\|recon"`
- **Phase gate:** All 10 new tests pass, plus all 12 existing tests still pass (regression)

### Wave 0 Gaps
- [ ] `tests/integration/test_net01_partition_healing.sh` -- covers NET-01
- [ ] `tests/integration/test_net02_split_brain.sh` -- covers NET-02
- [ ] `tests/integration/test_net03_large_blob_integrity.sh` -- covers NET-03
- [ ] `tests/integration/test_net04_cursor_resumption.sh` -- covers NET-04
- [ ] `tests/integration/test_net05_crash_recovery.sh` -- covers NET-05
- [ ] `tests/integration/test_net06_late_joiner.sh` -- covers NET-06
- [ ] `tests/integration/test_recon01_diff_scaling.sh` -- covers RECON-01
- [ ] `tests/integration/test_recon02_empty_skip.sh` -- covers RECON-02
- [ ] `tests/integration/test_recon03_version_compat.sh` -- covers RECON-03
- [ ] `tests/integration/test_recon04_large_transfer.sh` -- covers RECON-04
- [ ] `tests/integration/docker-compose.mesh.yml` -- 5-node mesh topology
- [ ] `tests/integration/docker-compose.partition.yml` -- 4-node partition topology (optional, can reuse mesh with iptables)
- [ ] `tests/integration/configs/node{1-5}-mesh.json` -- 5-node mesh configs

## Implementation Order Recommendation

### Wave 1: Topology + Simple Tests (NET-03, NET-04, NET-05)
1. Create docker-compose.recon.yml (2-node, fixed IPs, fast sync, no periodic full resync)
2. Create 2-node recon configs (node1-recon.json, node2-recon.json)
3. Write test_net03_large_blob_integrity.sh (2-node, no partitioning, straightforward)
4. Write test_net04_cursor_resumption.sh (2-node, stop/restart, cursor_hits verification)
5. Write test_net05_crash_recovery.sh (2-node, kill -9 during sync, based on deploy/test-crash-recovery.sh pattern)

### Wave 2: Partition Tests (NET-01, NET-02)
1. Create docker-compose.mesh.yml (5-node, fixed IPs, bootstrap chain)
2. Create 5-node mesh configs
3. Write test_net01_partition_healing.sh (iptables or docker network disconnect)
4. Write test_net02_split_brain.sh (4-node partition with independent writes)

### Wave 3: Scale + Reconciliation (NET-06, RECON-01 through RECON-04)
1. Write test_net06_late_joiner.sh (3-node + late joiner, 10K blobs, multi-namespace)
2. Write test_recon01_diff_scaling.sh (10K preload + 10 new, traffic measurement)
3. Write test_recon02_empty_skip.sh (cursor_hits verification, log inspection)
4. Write test_recon03_version_compat.sh (verify no crash on invalid decode)
5. Write test_recon04_large_transfer.sh (5000-blob full transfer, count verification)

## Key Log Messages for Test Verification

| Purpose | Log Pattern | Source |
|---------|-------------|--------|
| Sync round complete | `sync complete.*blobs_sent=\|blobs_received=` | peer_manager.cpp |
| Cursor hit (namespace skip) | `sync cursor hit: ns=` | peer_manager.cpp:870 |
| Cursor miss | `sync cursor miss: ns=` | peer_manager.cpp:878 |
| Full resync triggered | `full resync (periodic\|time gap)` | peer_manager.cpp:851-856 |
| Peer connected | `Connected to peer` | peer_manager.cpp:332 |
| Peer disconnected | `Peer.*disconnected` | peer_manager.cpp:373 |
| Integrity scan clean | `integrity scan:` | storage.cpp (startup) |
| Reconciliation stats | `sync complete.*namespaces=` | peer_manager.cpp:1155 |
| Invalid ReconcileInit | `invalid ReconcileInit` | peer_manager.cpp:1312 |
| Metrics line | `metrics: peers=.*blobs=` | peer_manager.cpp:2353 |
| Auto-reconnect | `reconnecting to` | peer_manager.cpp |
| Sync cursor stored | `sync cursor updated` | peer_manager.cpp (debug) |

## Open Questions

1. **iptables vs docker network disconnect for partitioning**
   - What we know: iptables FORWARD rules on --net=host can block inter-container traffic on Docker bridges. docker network disconnect/connect is simpler but coarser.
   - What's unclear: Whether iptables FORWARD rules on the host reliably intercept Docker bridge traffic on all Linux kernel versions. The DOCKER-USER chain is the recommended insertion point.
   - Recommendation: Start with `docker network disconnect`/`connect` for NET-01/NET-02 as the simpler, more portable approach. Fall back to iptables only if fine-grained partition control is needed. For the 5-node partition test, `docker network disconnect` is sufficient since the partition is "group A" vs "group B" (not selective pair-wise blocking).

2. **RECON-03 version byte test within AEAD-encrypted connection**
   - What we know: All wire messages after handshake are AEAD-encrypted. We cannot inject a malformed ReconcileInit without completing a valid handshake first.
   - What's unclear: Whether to build a custom test client that completes handshake then sends malformed reconciliation, or rely on unit test coverage.
   - Recommendation: The `decode_reconcile_init` function already returns nullopt for wrong versions (line 268 of reconciliation.cpp), and the responder handles nullopt with `continue` (line 1311-1313 of peer_manager.cpp). Write a minimal Docker test that verifies the node stays healthy through multiple sync rounds (proving the continue path works without accumulating state corruption). Document that the actual version byte rejection is verified by unit tests. If a more rigorous test is desired, it would require a custom test client binary -- out of scope for this phase.

3. **10,000-blob ingest time for NET-06 and RECON-01**
   - What we know: v0.8.0 benchmarks show 33.1 blobs/sec for 1 MiB blobs. Small blobs (256 bytes) should be much faster. At --rate 200, 10,000 blobs = 50 seconds.
   - What's unclear: Whether the test infrastructure (Docker networking, SIGUSR1 metrics extraction) adds enough overhead to make 10K-blob tests flaky.
   - Recommendation: Use `--rate 200 --size 256` for bulk ingest. Set sync wait timeout to 300 seconds (5 minutes) for 10K-blob convergence. This is conservative but prevents flakiness.

## Sources

### Primary (HIGH confidence)
- `db/sync/reconciliation.h` L11-13 -- RECONCILE_VERSION=0x01, SPLIT_THRESHOLD=16, MAX_RECONCILE_ROUNDS=64
- `db/sync/reconciliation.cpp` L262-275 -- decode_reconcile_init with version check (returns nullopt for mismatch)
- `db/peer/peer_manager.cpp` L785-1160 -- run_sync_with_peer (initiator): cursor decision, reconciliation loop, blob transfer
- `db/peer/peer_manager.cpp` L1199-1555 -- handle_sync_as_responder: ReconcileInit decode, cursor skip, reconciliation
- `db/peer/peer_manager.cpp` L771-783 -- check_full_resync (periodic + time gap)
- `db/peer/peer_manager.cpp` L2340-2373 -- log_metrics_line (all SIGUSR1 metric fields)
- `db/storage/storage.h` L37-42 -- SyncCursor struct (seq_num, round_count, last_sync_timestamp)
- `db/config/config.h` L28-29 -- full_resync_interval, cursor_stale_seconds defaults
- `tests/integration/helpers.sh` -- All Docker orchestration primitives
- `tests/integration/test_crypt04_forward_secrecy.sh` -- Proven netshoot/tcpdump pattern
- `tests/integration/test_acl05_sighup_reload.sh` -- Proven dynamic config + manual docker run pattern
- `deploy/test-crash-recovery.sh` -- Proven kill-9 + restart + integrity verification pattern

### Secondary (MEDIUM confidence)
- `db/TESTS.md` sections 5 and 9 -- Detailed test specifications for network resilience and set reconciliation

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - all components are existing, proven in Phase 47-48
- Architecture: HIGH - patterns established by 12 existing integration test scripts
- Pitfalls: HIGH - identified from direct source code analysis of sync protocol and config
- Network partitioning: MEDIUM - iptables approach is standard but Docker bridge behavior varies; docker network disconnect is the safer alternative
- RECON-03 version byte: MEDIUM - cannot be tested via pure integration test due to AEAD encryption; relies on unit test coverage

**Research date:** 2026-03-21
**Valid until:** 2026-04-21 (stable codebase, testing phase)
