# Phase 47: Crypto & Transport Verification - Context

**Gathered:** 2026-03-21
**Status:** Ready for planning

<domain>
## Phase Boundary

Docker integration tests verifying cryptographic guarantees: content addressing (CRYPT-01), non-repudiation (CRYPT-02), tamper detection (CRYPT-03), forward secrecy (CRYPT-04), MITM rejection (CRYPT-05), and trusted peer bypass (CRYPT-06). This is the first Docker integration test phase -- the test harness established here is reused by Phases 48-52.

</domain>

<decisions>
## Implementation Decisions

### Test harness organization
- Per-test shell scripts (one per CRYPT-XX requirement), not a monolithic script
- Shared `tests/helpers.sh` with common functions: wait_healthy, ingest_blob, verify_sync, check_logs, cleanup
- Runner script `tests/run-integration.sh` executes all tests in sequence, reports pass/fail summary
- Each test script is self-contained: sets up topology, runs test, verifies, tears down
- Tests live in `tests/integration/` directory at project root (not deploy/)
- deploy/ stays for benchmarks; tests/ is for correctness verification

### Independent crypto verification
- Build a standalone C++ CLI tool `chromatindb_verify` linking against liboqs + libsodium (same libraries as chromatindb)
- Subcommands: `hash` (recompute SHA3-256 digest from blob fields), `sig` (verify ML-DSA-87 signature against digest + pubkey)
- Built as a CMake target alongside chromatindb and loadgen, included in Docker image
- No Python dependency -- exact same crypto code paths as the node itself
- This tool is reused by any test that needs independent crypto verification

### MITM and forward secrecy testing
- CRYPT-04 (forward secrecy): capture traffic with tcpdump during PQ handshake, then attempt to derive session key from long-term identity keys -- must fail. Verification: check that captured ciphertext is not decryptable without ephemeral KEM shared secret.
- CRYPT-05 (MITM rejection): test with a third node that has a different identity key but is network-reachable. The protocol's session fingerprint (derived from KEM shared secret + both identity signatures) means a MITM substituting KEM keys produces a fingerprint mismatch. Verify via log inspection that handshake fails with fingerprint mismatch.
- CRYPT-06 (trusted peer bypass): configure trusted_peers, verify lightweight handshake succeeds (log inspection: no KEM exchange). Then test with wrong identity key on trusted IP -- must be rejected.

### Claude's Discretion
- Docker compose topology details (number of nodes, network config)
- Exact helper function signatures and error handling
- Log parsing approach (grep vs structured JSON parsing)
- Test timeout values
- tcpdump capture and analysis approach for CRYPT-04

</decisions>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches. The test specifications in db/TESTS.md sections 1 and 2 define exactly what each test must verify.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Dockerfile`: Multi-stage build producing chromatindb + chromatindb_loadgen binaries
- `deploy/docker-compose.yml`: 3-node + late-joiner topology with health checks and bridge network
- `deploy/docker-compose.trusted.yml`: Trusted peer topology variant
- `deploy/configs/`: Node config templates (node1.json through node4-latejoin.json)
- `deploy/run-benchmark.sh`: 67KB benchmark script -- patterns for Docker orchestration, log polling, convergence waiting
- `deploy/test-crash-recovery.sh`: Crash recovery test -- pattern for kill/restart Docker tests
- `scripts/run-e2e-reliability.sh`: Reliability validation script

### Established Patterns
- Health check: TCP connect to port 4200
- Config injection: volume-mount JSON config files
- Log inspection: `docker logs` + grep for specific patterns
- Convergence: poll SIGUSR1 metrics until blob counts match
- Named volumes for data persistence across restarts

### Integration Points
- chromatindb_verify (new) built alongside chromatindb in Dockerfile
- loadgen used to inject test blobs with known content
- Docker bridge network for inter-node communication
- tcpdump for traffic capture (needs NET_ADMIN capability)

</code_context>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 47-crypto-transport-verification*
*Context gathered: 2026-03-21*
