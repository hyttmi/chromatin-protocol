# Phase 48: Access Control & Topology - Context

**Gathered:** 2026-03-21
**Status:** Ready for planning

<domain>
## Phase Boundary

Docker integration tests verifying ACL enforcement (closed-garden, namespace ownership, delegation lifecycle, SIGHUP hot-reload) and connection dedup. Requirements: ACL-01 through ACL-05, TOPO-01. Test harness from Phase 47 is reused (helpers.sh, run-integration.sh, chromatindb_verify, Docker compose patterns).

</domain>

<decisions>
## Implementation Decisions

### ACL verification method (ACL-01, ACL-02)
- Log inspection only for ACL-01 (no tcpdump/packet capture) — grep for handshake completion + immediate disconnect, verify no app-layer messages (SyncInit, BlobPush, etc.) in unauthorized node's logs
- 3-node topology for ACL-01: Node1 + Node2 with mutual allowed_keys (authorized pair), Node3 with fresh key NOT in allowed_keys (intruder). Proves closed garden works while legitimate peers still sync
- ACL-02 uses loadgen with mismatched key (key A targeting namespace owned by key B) — loadgen reports rejection
- ACL-02 verifies both log-based rejection confirmation AND blob count stays at 0 via SIGUSR1 metrics (belt and suspenders)

### Delegation test topology (ACL-03, ACL-04)
- 3-node cluster: Node1 (owner writes delegation blob), Node2 (delegate writes here to prove cross-node delegation), Node3 (revocation syncs here, delegate's writes rejected)
- Loadgen gets `--key-file` flag to use pre-generated key files instead of generating fresh keypairs. Test script pre-generates owner + delegate keys, creates delegation blob via owner, then loadgen uses delegate's key file
- Loadgen gets `--delete` flag that sends a tombstone for a specific blob hash. Delegate runs loadgen --delete targeting owner's namespace — must be rejected (write-only). Reusable for Phase 51 TTL tests
- ACL-04 revocation: owner uses loadgen --delete --key-file owner.key to tombstone the delegation blob (TTL=0 permanent). Then delegate's subsequent write attempt must fail
- Revocation tombstone uses TTL=0 (permanent) — TTL>0 tombstone expiry edge cases deferred to Phase 51

### SIGHUP ACL reload (ACL-05)
- Config files volume-mounted from host (tests/integration/configs/). Test script edits the file on the host, then sends SIGHUP via `docker kill -s HUP`
- Single test script with two phases: Phase 1 adds a key + SIGHUP + verifies new connection, Phase 2 removes a key + SIGHUP + verifies drop
- Verification: log inspection + peer count via SIGUSR1 metrics (confirm both add and removal)
- Add-key flow: Node3 (initially unauthorized) starts but can't connect to Node1. Test adds Node3's key to Node1's allowed_keys + SIGHUP. Node3's auto-reconnect picks up the connection. Verify via logs + peer count increase
- Remove-key flow: Remove Node3's key from allowed_keys + SIGHUP. Verify disconnect in logs + peer count decrease

### Connection dedup (TOPO-01)
- Dedicated docker-compose.dedup.yml with exactly 2 nodes configured as mutual peers (each lists the other in its peers array)
- New configs: node1-dedup.json, node2-dedup.json
- Verification: SIGUSR1 metrics on both nodes should show peers=1 (not peers=2) after connection stabilizes
- Also verify sync works on the deduped connection: ingest a blob on Node1, confirm it syncs to Node2. Proves the surviving connection is functional

### Claude's Discretion
- Exact docker-compose.yml configurations (networks, healthchecks, container names)
- Helper function additions or modifications needed for new test patterns
- Log message patterns to grep for (ACL rejection, disconnect, dedup events)
- Test timeout values and sync wait intervals
- How to handle loadgen --key-file and --delete flag implementation details
- Config file templates for ACL and dedup topologies

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. The test specifications in db/TESTS.md section 3 define exactly what each test must verify. Loadgen enhancements (--key-file, --delete) should be reusable for Phase 51 TTL tests.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `tests/integration/helpers.sh`: Docker orchestration primitives (wait_healthy, get_blob_count, wait_sync, run_loadgen, run_verify, cleanup)
- `tests/integration/run-integration.sh`: Test runner with --skip-build and --filter
- `tests/integration/docker-compose.test.yml`: 2-node base topology with health checks
- `tests/integration/docker-compose.mitm.yml`: 3-node topology (from CRYPT-05) — pattern for multi-node compose files
- `tests/integration/docker-compose.trusted.yml`: Trusted peer topology (from CRYPT-06)
- `chromatindb_verify`: CLI tool for independent crypto verification (hash-fields, sig-fields subcommands)
- `chromatindb_loadgen`: Blob injection tool with --verbose-blobs, --count, --size, --ttl, --rate

### Established Patterns
- Per-test shell scripts sourcing helpers.sh, trap cleanup EXIT
- Log inspection via `docker logs $container 2>&1 | grep`
- Metrics via `docker kill -s USR1` + log parsing for blob counts
- Config injection via volume-mounted JSON files
- Fixed-IP Docker networks for deterministic peer configs

### Integration Points
- Loadgen needs --key-file and --delete flags (new functionality for this phase)
- ACL-05 requires host-side config file editing + docker kill -s HUP
- Node3 auto-reconnect (from v0.9.0) exercises the SIGHUP add-key flow naturally

</code_context>

<deferred>
## Deferred Ideas

- TTL>0 tombstone expiry for delegation revocation — Phase 51 TTL lifecycle tests
- What happens when revocation tombstone expires and delegation blob re-syncs — Phase 51

</deferred>

---

*Phase: 48-access-control-topology*
*Context gathered: 2026-03-21*
