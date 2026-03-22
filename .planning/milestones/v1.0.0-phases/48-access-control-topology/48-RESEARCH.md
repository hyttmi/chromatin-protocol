# Phase 48: Access Control & Topology - Research

**Researched:** 2026-03-21
**Domain:** Docker integration tests for ACL enforcement, delegation lifecycle, SIGHUP hot-reload, and connection dedup
**Confidence:** HIGH

## Summary

Phase 48 implements Docker multi-node integration tests verifying access control enforcement (closed-garden, namespace ownership, delegation lifecycle, SIGHUP hot-reload) and connection dedup. The codebase already has all ACL, delegation, SIGHUP reload, and config infrastructure implemented and unit-tested. The loadgen tool already has `--identity-file`, `--identity-save`, `--delete`, `--hashes-from stdin`, and `--verbose-blobs` flags -- all the primitives needed for delegation and tombstone testing. The Phase 47 test harness (helpers.sh, run-integration.sh, docker-compose patterns) is fully reusable.

The one significant finding: **connection dedup (TOPO-01) is NOT implemented**. The `on_peer_connected` method in PeerManager does not check for existing connections from the same peer namespace. When two nodes are mutual bootstrap peers, both will establish outbound connections simultaneously, resulting in 2 connections. TOPO-01 requires adding duplicate connection detection in `on_peer_connected` (close the newer connection when the same peer namespace is already connected) before the integration test can pass.

**Primary recommendation:** Build test scripts first for ACL-01 through ACL-05 (all test-only, no code changes needed), then implement connection dedup in PeerManager, then write the TOPO-01 test. Delegation tests (ACL-03, ACL-04) need a new loadgen `--delegate` flag to create delegation blobs via the wire protocol.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- ACL-01: Log inspection only (no tcpdump/packet capture). 3-node topology: Node1+Node2 authorized pair, Node3 intruder with fresh key.
- ACL-02: Loadgen with mismatched key. Belt-and-suspenders: log-based rejection + blob count stays 0 via SIGUSR1.
- ACL-03/04: 3-node cluster. Owner creates delegation blob, delegate writes on Node2, revocation syncs to Node3. Loadgen gets `--key-file` flag (exists as `--identity-file`). Loadgen gets `--delete` flag (already implemented). ACL-04 revocation uses TTL=0 permanent tombstone.
- ACL-05: Config files volume-mounted. Host-side edit + `docker kill -s HUP`. Two phases: add-key + verify, remove-key + verify. Verification via logs + SIGUSR1 peer count.
- TOPO-01: Dedicated docker-compose.dedup.yml. 2 nodes as mutual peers. SIGUSR1 shows peers=1 not peers=2. Sync verified on deduped connection.

### Claude's Discretion
- Exact docker-compose.yml configurations (networks, healthchecks, container names)
- Helper function additions or modifications for new test patterns
- Log message patterns to grep for
- Test timeout values and sync wait intervals
- Loadgen --key-file and --delete implementation details
- Config file templates for ACL and dedup topologies

### Deferred Ideas (OUT OF SCOPE)
- TTL>0 tombstone expiry for delegation revocation -- Phase 51 TTL lifecycle tests
- What happens when revocation tombstone expires and delegation blob re-syncs -- Phase 51
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| ACL-01 | Closed-garden enforcement: unauthorized node disconnected after handshake, no app-layer messages | `on_peer_connected` ACL gating at L280-293 of peer_manager.cpp; log pattern "access denied: namespace="; 3-node topology with allowed_keys config |
| ACL-02 | Namespace sovereignty: write with non-owning key rejected, no data written | Engine Step 2 ownership check at L133-148 of engine.cpp; loadgen with `--identity-file` for mismatched key; SIGUSR1 blob count verification |
| ACL-03 | Delegation write: delegate writes to owner's namespace, cannot delete | Delegation pipeline at L139-163 of engine.cpp; `make_delegation_data()` in codec; loadgen `--identity-file` + `--identity-save` for delegate key management; new `--delegate` flag needed for delegation blob creation |
| ACL-04 | Revocation propagation: tombstoned delegation syncs, revoked delegate rejected | Loadgen `--delete` + `--hashes-from stdin` already implemented; delegation blob tombstone invalidates `has_valid_delegation()` in storage layer |
| ACL-05 | SIGHUP ACL reload: add/remove keys without restart | `reload_config()` at L1614-1729; `disconnect_unauthorized_peers()` at L1731-1746; `clear_reconnect_state()` resets backoff for immediate reconnect |
| TOPO-01 | Connection dedup: mutual peers produce one logical connection | **NOT IMPLEMENTED** -- `on_peer_connected` at L280-369 has no dedup check; needs code change before test can pass |
</phase_requirements>

## Standard Stack

### Core (existing -- no new libraries)
| Component | Version | Purpose | Status |
|-----------|---------|---------|--------|
| Docker Compose | v2 | Multi-node test topologies | Existing patterns from Phase 47 |
| helpers.sh | N/A | Docker orchestration primitives | Existing, may need new helpers |
| chromatindb_loadgen | Built from source | Protocol-compliant blob injection | Has --identity-file, --delete, --verbose-blobs |
| chromatindb:test | Docker image | Node under test | Built by test runner |
| bash/jq | System | Test scripting and JSON parsing | Already required |

### Loadgen Flags (Already Implemented)
| Flag | Purpose | Relevant Test |
|------|---------|---------------|
| `--identity-file DIR` | Load keypair from directory | ACL-02, ACL-03, ACL-04 |
| `--identity-save DIR` | Save generated keypair | ACL-03 (save delegate key) |
| `--delete` | Send tombstones instead of blobs | ACL-03 (delegate delete test), ACL-04 |
| `--hashes-from stdin` | Read target blob hashes for tombstone | ACL-04 (tombstone delegation blob) |
| `--verbose-blobs` | Emit BLOB_FIELDS JSON to stderr | ACL-03 (capture blob hashes for later deletion) |

### New Loadgen Capability Needed
| Flag | Purpose | Relevant Test |
|------|---------|---------------|
| `--delegate PUBKEY_HEX` | Create and ingest a delegation blob for the given delegate pubkey | ACL-03, ACL-04 (owner creates delegation blob) |

## Architecture Patterns

### Test Script Structure (from Phase 47)
```
tests/integration/
  helpers.sh                      # Shared primitives
  run-integration.sh              # Discovery + runner
  docker-compose.test.yml         # 2-node base topology
  docker-compose.mitm.yml         # 3-node topology (fixed IPs)
  docker-compose.trusted.yml      # 2-node trusted topology (fixed IPs)
  docker-compose.acl.yml          # NEW: 3-node ACL topology (fixed IPs)
  docker-compose.dedup.yml        # NEW: 2-node mutual-peer topology
  configs/
    node1.json                    # Existing base configs
    node2.json
    node1-acl.json                # NEW: closed-garden config
    node2-acl.json                # NEW: authorized peer config
    node3-intruder.json           # NEW: unauthorized node config
    node1-dedup.json              # NEW: mutual peer config
    node2-dedup.json              # NEW: mutual peer config
  test_acl01_closed_garden.sh     # NEW
  test_acl02_namespace_sovereignty.sh  # NEW
  test_acl03_delegation_write.sh  # NEW
  test_acl04_revocation.sh        # NEW
  test_acl05_sighup_reload.sh     # NEW
  test_topo01_connection_dedup.sh # NEW
```

### Per-Test Script Pattern (established)
```bash
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/helpers.sh"

COMPOSE_CUSTOM="docker compose -f $SCRIPT_DIR/docker-compose.acl.yml -p chromatindb-test"

# Cleanup function + trap
cleanup_test() { ... }
trap cleanup_test EXIT

check_deps
build_image
cleanup_test 2>/dev/null || true

FAILURES=0
# ... test logic ...
if [[ $FAILURES -eq 0 ]]; then pass "..."; exit 0; else fail "..."; fi
```

### Namespace Discovery Pattern (from CRYPT-06)
```bash
# Extract namespace hash from node startup logs
NODE_NS=$(docker logs container-name 2>&1 | grep -oP 'namespace: \K[0-9a-f]{64}' | head -1)
```

### Metrics Inspection Pattern
```bash
# Trigger metrics dump
docker kill -s USR1 container-name >/dev/null 2>&1 || true
sleep 2
# Extract peer count from metrics line
PEER_COUNT=$(docker logs container-name 2>&1 | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+')
BLOB_COUNT=$(docker logs container-name 2>&1 | grep "metrics:" | tail -1 | grep -oP 'blobs=\K[0-9]+')
```

### SIGHUP Config Reload Pattern
```bash
# Edit config file on host (volume-mounted to container)
cat > "$CONFIG_PATH" <<EOF
{ "bind_address": "0.0.0.0:4200", "allowed_keys": ["$KEY_HEX"], ... }
EOF
# Send SIGHUP to container
docker kill -s HUP container-name
# Wait for reload to take effect
sleep 3
# Verify via logs
docker logs container-name 2>&1 | grep "config reload: +1 keys"
```

### Dynamic Config + Manual docker run Pattern (from CRYPT-06 Part 3)
When tests need runtime-generated configs (with discovered namespaces), use:
```bash
TEMP_CONFIG=$(mktemp /tmp/node-config-XXXXXX.json)
cat > "$TEMP_CONFIG" <<EOCFG
{ "bind_address": "0.0.0.0:4200", "allowed_keys": ["$DISCOVERED_NS"], ... }
EOCFG
chmod 644 "$TEMP_CONFIG"

docker run -d --name container-name \
    --network chromatindb-test_test-net \
    --ip 172.28.0.X \
    -v "$TEMP_CONFIG:/config/node.json:ro" \
    chromatindb:test run --config /config/node.json --data-dir /data --log-level debug
```

### Anti-Patterns to Avoid
- **Read-only config mounts for SIGHUP tests:** ACL-05 requires writable config files. Use `-v path:/config/node.json` (no `:ro`) so host edits are visible to the container.
- **Fixed namespaces in config:** Namespaces are SHA3-256(pubkey), which change every time a node generates a fresh identity. Always discover namespaces from startup logs.
- **Assuming immediate sync:** After injecting blobs or delegation blobs, use `wait_sync` with adequate timeout (30-60s for delegation propagation across 3 nodes).
- **Ignoring loadgen exit code:** Loadgen exits 0 even when all blobs are rejected. Check the JSON output's `errors` field and `blob_hashes` array for actual status.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Delegation blob creation | Custom wire protocol encoder in bash | Loadgen `--delegate PUBKEY_HEX` flag | Blob must be properly signed by owner, FlatBuffer-encoded, and sent via the real PQ handshake protocol |
| Key generation | openssl or manual crypto | Loadgen `--identity-save DIR` | ML-DSA-87 keys must match the node's identity format (node.key + node.pub files) |
| Namespace hash computation | External SHA3-256 tool | Discover from `docker logs ... | grep namespace:` | SHA3-256 of the ML-DSA-87 public key; the node computes and logs it on startup |
| Blob hash extraction | Manual parsing | Loadgen JSON output `blob_hashes` array | Content hashes needed for `--delete --hashes-from stdin` |
| AEAD-encrypted protocol messages | tcpdump + packet parsing | Log inspection for ACL verification | All messages are AEAD-encrypted after handshake; packet capture shows only ciphertext |

## Common Pitfalls

### Pitfall 1: Config Volume Mount Read-Only for SIGHUP Tests
**What goes wrong:** ACL-05 test mounts config with `:ro`, then host edits are not reflected inside the container.
**Why it happens:** Phase 47 compose files all use `:ro` mounts.
**How to avoid:** For SIGHUP tests, bind-mount configs WITHOUT `:ro`. The host file is the source of truth; Docker bind-mounts are live filesystem views.
**Warning signs:** SIGHUP handler logs "config reload failed" with file read errors.

### Pitfall 2: Race Between Node Startup and Namespace Discovery
**What goes wrong:** Script tries to read namespace from logs before the node has started and logged it.
**Why it happens:** `docker run -d` returns immediately; the node takes seconds to initialize identity.
**How to avoid:** Use `wait_healthy` before reading logs for namespace. The namespace log line appears during initialization before the healthcheck even starts, so by the time `wait_healthy` passes, it is available.
**Warning signs:** Empty `$NODE_NS` variable.

### Pitfall 3: Delegation Blob Must Be Ingested Via Owner Connection
**What goes wrong:** Test tries to create delegation blob using delegate's identity.
**Why it happens:** Confusion about who signs the delegation blob.
**How to avoid:** The delegation blob is signed by the OWNER's key. It contains the delegate's public key as data (4-byte magic + 2592-byte ML-DSA-87 pubkey). The owner must connect (via loadgen with owner's identity) and send the delegation blob.
**Warning signs:** "no ownership or delegation" rejection in logs.

### Pitfall 4: Connection Dedup Race Condition (TOPO-01)
**What goes wrong:** Both nodes simultaneously connect to each other. Without dedup, both connections complete the handshake, resulting in 2 connections per node.
**Why it happens:** `on_peer_connected` does not check for existing connections from the same peer namespace.
**How to avoid:** Implement namespace-based connection dedup in `on_peer_connected`. When a new connection arrives from a peer namespace already in `peers_`, close the newer connection (or use a deterministic tie-breaker like comparing namespace hashes).
**Warning signs:** SIGUSR1 metrics show `peers=2` instead of `peers=1` on a 2-node mutual topology.

### Pitfall 5: Loadgen Connection Close After ACL Rejection
**What goes wrong:** Loadgen connects but gets ACL-rejected. The `connection_closed_` flag fires, but loadgen may hang waiting for drain or crash.
**Why it happens:** ACL rejection calls `conn->close()` before any messages are sent. Loadgen's `on_close` callback fires, but `subscribe_and_send` may never start if `on_ready` never fires (handshake fails or connection closes before ready).
**How to avoid:** For ACL-02 (namespace sovereignty), the node accepts the connection (key is allowed) but rejects the blob. Loadgen connects successfully. For ACL-01, loadgen is NOT used -- the intruder node simply tries to connect as a peer (via bootstrap_peers).
**Warning signs:** Loadgen hangs indefinitely.

### Pitfall 6: Auto-Reconnect Interfering with SIGHUP Tests
**What goes wrong:** After removing a key via SIGHUP, the disconnected node's auto-reconnect immediately retries, hitting ACL rejection and entering backoff. When the key is re-added, the reconnect timer is sleeping for 600s.
**Why it happens:** `handle_sighup()` calls `clear_reconnect_state()` which cancels all reconnect timers and resets all ACL rejection counters. But if the removed node was already in an active reconnect loop, the timer cancellation triggers immediate retry.
**How to avoid:** The ACL-05 test uses a SEPARATE initial unauthorized node (Node3) that is NOT in bootstrap_peers. Node3 starts, fails to connect (ACL rejected). Test adds Node3's key + SIGHUP on Node1. Node3's auto-reconnect picks up the connection. This leverages v0.9.0's auto-reconnect feature naturally.
**Warning signs:** "reconnecting to X in 600s" in logs after SIGHUP.

## Code Examples

### ACL-01: Closed Garden Test Flow
```bash
# 3-node topology: Node1+Node2 authorized, Node3 intruder
# Config: node1-acl.json has allowed_keys = [node2_namespace]
# Node3 has fresh identity (not in allowed_keys)

# Start authorized pair first
$COMPOSE up -d node1 node2
wait_healthy chromatindb-test-node1
wait_healthy chromatindb-test-node2

# Start intruder
$COMPOSE up -d node3
sleep 10  # Wait for connection attempt

# Verify ACL rejection in node1 logs
docker logs chromatindb-test-node1 2>&1 | grep "access denied"
# Verify no app-layer messages exchanged
# (no SyncInit, BlobPush, Subscribe, etc. after "access denied")

# Verify authorized pair still works
run_loadgen 172.28.0.2 --count 3 --size 256
wait_sync chromatindb-test-node2 3
```

### ACL-03: Delegation Write Test Flow
```bash
# Step 1: Start 3-node cluster (open mode)
# Step 2: Owner generates identity, saves it
OWNER_OUTPUT=$(run_loadgen 172.28.0.2 --count 1 --size 256 --identity-save /tmp/owner)
# Step 3: Delegate generates identity, saves it
run_loadgen 172.28.0.2 --count 0 --identity-save /tmp/delegate  # connect only
# Step 4: Owner creates delegation blob for delegate (needs --delegate flag)
run_loadgen 172.28.0.2 --identity-file /tmp/owner --delegate $DELEGATE_PUBKEY_HEX --count 1
# Step 5: Wait for delegation to sync to Node2
sleep 15
# Step 6: Delegate writes to owner's namespace on Node2
run_loadgen 172.28.0.3 --identity-file /tmp/delegate --count 5 --size 256
# Loadgen should report 0 errors (delegate writes accepted)
# Step 7: Delegate tries to delete (must fail)
echo "$BLOB_HASH" | run_loadgen 172.28.0.2 --identity-file /tmp/delegate --delete --hashes-from stdin
# Loadgen should report errors (delegate cannot delete)
```

### TOPO-01: Connection Dedup Verification
```bash
# docker-compose.dedup.yml: 2 nodes, each lists the other in bootstrap_peers
# Both use fixed IPs, both configure the other as bootstrap peer

$COMPOSE up -d
wait_healthy chromatindb-test-node1
wait_healthy chromatindb-test-node2
sleep 15  # Wait for mutual connections to settle

# Check peer count -- should be 1, not 2
docker kill -s USR1 chromatindb-test-node1 >/dev/null 2>&1; sleep 2
PEER_COUNT=$(docker logs chromatindb-test-node1 2>&1 | grep "metrics:" | tail -1 | grep -oP 'peers=\K[0-9]+')
[[ "$PEER_COUNT" -eq 1 ]] || fail "Expected peers=1, got peers=$PEER_COUNT"

# Verify sync works on the surviving connection
run_loadgen 172.28.0.2 --count 5 --size 256
wait_sync chromatindb-test-node2 5
```

### Connection Dedup Implementation (PeerManager)
```cpp
// In on_peer_connected(), after ACL check, before adding to peers_:
auto peer_ns = crypto::sha3_256(conn->peer_pubkey());

// Check if already connected to this peer namespace
for (const auto& existing : peers_) {
    auto existing_ns = crypto::sha3_256(existing.connection->peer_pubkey());
    if (existing_ns == peer_ns) {
        // Deterministic tie-break: keep connection where initiator has lower namespace
        auto own_ns = identity_.namespace_id();
        bool we_initiated = conn->is_initiator();
        bool keep_new = we_initiated && (own_ns < peer_ns);
        if (keep_new) {
            // Close existing, keep new
            existing.connection->close();
        } else {
            // Close new, keep existing
            conn->close();
            return;
        }
        break;
    }
}
```

## State of the Art

| Component | Current State | Impact on Phase 48 |
|-----------|--------------|-------------------|
| ACL enforcement | Fully implemented (peer_manager.cpp L280-293) | Tests only -- no code changes |
| Namespace ownership | Fully implemented (engine.cpp L133-148) | Tests only -- no code changes |
| Delegation lifecycle | Fully implemented (engine.cpp L139-163, storage delegation_map) | Tests only -- delegation blob creation via loadgen needs `--delegate` flag |
| SIGHUP reload | Fully implemented (peer_manager.cpp L1614-1729) | Tests only -- no code changes |
| Connection dedup | **NOT IMPLEMENTED** | Requires code change in peer_manager.cpp |
| Loadgen --identity-file | Implemented (loadgen_main.cpp L108-109) | Ready to use |
| Loadgen --delete | Implemented (loadgen_main.cpp L112-113) | Ready to use |
| Loadgen --hashes-from | Implemented (loadgen_main.cpp L114-115) | Ready to use |
| Loadgen --delegate | **NOT IMPLEMENTED** | Needs new flag to create delegation blobs |

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Bash integration tests (shell scripts) |
| Config file | tests/integration/run-integration.sh |
| Quick run command | `bash tests/integration/run-integration.sh --skip-build --filter "acl01"` |
| Full suite command | `bash tests/integration/run-integration.sh --skip-build` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| ACL-01 | Closed-garden enforcement | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl01` | Wave 0 |
| ACL-02 | Namespace sovereignty | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl02` | Wave 0 |
| ACL-03 | Delegation write + write-only | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl03` | Wave 0 |
| ACL-04 | Revocation propagation | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl04` | Wave 0 |
| ACL-05 | SIGHUP ACL reload | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl05` | Wave 0 |
| TOPO-01 | Connection dedup | integration | `bash tests/integration/run-integration.sh --skip-build --filter topo01` | Wave 0 |

### Sampling Rate
- **Per task commit:** `bash tests/integration/run-integration.sh --skip-build --filter <test_name>`
- **Per wave merge:** `bash tests/integration/run-integration.sh --skip-build`
- **Phase gate:** All 6 new tests pass, plus all 6 CRYPT tests still pass (regression)

### Wave 0 Gaps
- [ ] `tests/integration/test_acl01_closed_garden.sh` -- covers ACL-01
- [ ] `tests/integration/test_acl02_namespace_sovereignty.sh` -- covers ACL-02
- [ ] `tests/integration/test_acl03_delegation_write.sh` -- covers ACL-03
- [ ] `tests/integration/test_acl04_revocation.sh` -- covers ACL-04
- [ ] `tests/integration/test_acl05_sighup_reload.sh` -- covers ACL-05
- [ ] `tests/integration/test_topo01_connection_dedup.sh` -- covers TOPO-01
- [ ] `tests/integration/docker-compose.acl.yml` -- 3-node ACL topology
- [ ] `tests/integration/docker-compose.dedup.yml` -- 2-node mutual-peer topology
- [ ] `tests/integration/configs/node*-acl.json` -- ACL test configs
- [ ] `tests/integration/configs/node*-dedup.json` -- dedup test configs
- [ ] Loadgen `--delegate` flag -- creates delegation blob for ACL-03/04
- [ ] Connection dedup in `db/peer/peer_manager.cpp` -- code change for TOPO-01

## Implementation Order Recommendation

### Wave 1: Loadgen Enhancement + ACL-01/02 (pure test-only)
1. Add `--delegate PUBKEY_HEX` flag to loadgen (creates and ingests delegation blob)
2. Create docker-compose.acl.yml (3-node, fixed IPs, allowed_keys)
3. Create ACL config files (node1-acl.json, node2-acl.json, node3-intruder.json)
4. Write test_acl01_closed_garden.sh
5. Write test_acl02_namespace_sovereignty.sh

### Wave 2: Delegation Tests (ACL-03, ACL-04)
1. Write test_acl03_delegation_write.sh (depends on --delegate flag)
2. Write test_acl04_revocation.sh (depends on --delegate + --delete)

### Wave 3: SIGHUP + Dedup (ACL-05, TOPO-01)
1. Write test_acl05_sighup_reload.sh (pure test, no code changes)
2. Implement connection dedup in PeerManager::on_peer_connected
3. Create docker-compose.dedup.yml
4. Write test_topo01_connection_dedup.sh

## Key Log Messages for Test Verification

| Purpose | Log Pattern | Source |
|---------|-------------|--------|
| ACL rejection | `access denied: namespace=` | peer_manager.cpp:286 |
| Ingest rejection (no ownership) | `Ingest rejected: no ownership or delegation` | engine.cpp:145 |
| Delegate tombstone rejected | `Ingest rejected: delegates cannot create tombstone` | engine.cpp:160 |
| Delegation stored | `Ingest accepted` + is_delegation check | engine.cpp |
| SIGHUP received | `SIGHUP received, reloading config` | peer_manager.cpp:1603 |
| Config reload diff | `config reload: +N keys, -N keys` | peer_manager.cpp:1638 |
| Peer revoked | `revoking peer: namespace=` | peer_manager.cpp:1744 |
| Reconnect state cleared | `reconnect state cleared` | peer_manager.cpp:1606 |
| Peer connected | `Connected to peer` | peer_manager.cpp:332 |
| Metrics line | `metrics: peers=N ... blobs=N` | peer_manager.cpp:2299-2318 |
| Namespace at startup | `namespace: <64 hex chars>` | main.cpp:120 |
| Connection closed (ACL) | `Peer ... disconnected` | peer_manager.cpp:373-376 |

## Docker Network Configuration

### ACL 3-Node Topology (docker-compose.acl.yml)
```
Network: 172.28.0.0/16
Node1: 172.28.0.2 (closed-garden, allowed_keys = [node2_ns])
Node2: 172.28.0.3 (bootstrap_peers = [node1:4200])
Node3: 172.28.0.4 (intruder, bootstrap_peers = [node1:4200])
```
Note: node1-acl.json cannot have static allowed_keys because namespace hashes are generated at runtime. The test must:
1. Start Node2 first (open mode), discover its namespace
2. Stop Node2, create node1-acl.json with Node2's namespace
3. Start Node1 with restricted config, start Node2, start Node3

Alternative (simpler): Use a two-phase startup where all nodes start in open mode, then SIGHUP Node1 into closed mode. But this tests SIGHUP, not initial closed-garden. For pure ACL-01, use the CRYPT-06 Part 3 pattern: docker run -d with tmpfile config.

### Dedup 2-Node Topology (docker-compose.dedup.yml)
```
Network: 172.28.0.0/16
Node1: 172.28.0.2 (bootstrap_peers = [172.28.0.3:4200])
Node2: 172.28.0.3 (bootstrap_peers = [172.28.0.2:4200])
```
Both nodes list each other as bootstrap peers -- this creates the mutual connection scenario.

## Open Questions

1. **Delegation blob creation via loadgen**
   - What we know: Loadgen can create and send signed blobs using `--identity-file`. Delegation blobs need `make_delegation_data(delegate_pubkey)` which is in the chromatindb_lib.
   - What's unclear: Whether to add a `--delegate` flag to loadgen or create a separate `chromatindb_delegate` tool.
   - Recommendation: Add `--delegate PUBKEY_HEX` to loadgen. It creates one delegation blob where data = DELEGATION_MAGIC + hex-decoded-pubkey, signed by the loadgen's identity. This keeps tool count low and the flag is reusable for future tests.

2. **Connection dedup tie-breaking strategy**
   - What we know: Need to close one of two connections when same peer namespace appears twice.
   - What's unclear: Whether to use namespace comparison or connection direction (initiator vs responder) for tie-breaking.
   - Recommendation: Use deterministic namespace comparison (lower namespace hash keeps its initiated connection). This is symmetric -- both sides will reach the same conclusion independently.

3. **ACL-01 dynamic config generation**
   - What we know: Namespace hashes are runtime-generated. Config files with allowed_keys must contain the correct namespace.
   - What's unclear: Whether to use static compose files with dynamic config overlay or fully manual docker run.
   - Recommendation: Use the CRYPT-06 Part 3 pattern (manual docker run with tmpfile configs). This is proven and handles dynamic namespace discovery cleanly.

## Sources

### Primary (HIGH confidence)
- `db/peer/peer_manager.cpp` L280-369 -- on_peer_connected with ACL gating
- `db/peer/peer_manager.cpp` L1614-1729 -- reload_config (SIGHUP)
- `db/peer/peer_manager.cpp` L1731-1746 -- disconnect_unauthorized_peers
- `db/engine/engine.cpp` L130-164 -- ownership/delegation verification pipeline
- `db/acl/access_control.cpp` -- AccessControl class with reload support
- `db/net/server.cpp` L342-347 -- clear_reconnect_state
- `loadgen/loadgen_main.cpp` -- full loadgen CLI with --identity-file, --delete, --verbose-blobs
- `tests/integration/helpers.sh` -- Docker orchestration primitives
- `tests/integration/test_crypt06_trusted_bypass.sh` -- Pattern for dynamic config + manual docker run
- `db/wire/codec.h` L66-82 -- Delegation data format (DELEGATION_MAGIC + 2592-byte pubkey)

### Secondary (MEDIUM confidence)
- `db/tests/engine/test_engine.cpp` L111-129 -- make_signed_delegation reference implementation
- `db/tests/sync/test_sync_protocol.cpp` L623-641 -- make_signed_delegation_sync reference

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - all components are existing, proven in Phase 47
- Architecture: HIGH - patterns established by 6 existing CRYPT test scripts
- Pitfalls: HIGH - identified from direct source code analysis
- Connection dedup: HIGH - confirmed NOT IMPLEMENTED via code inspection of on_peer_connected

**Research date:** 2026-03-21
**Valid until:** 2026-04-21 (stable codebase, testing phase)
