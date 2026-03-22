# Phase 50: Operations, Disaster Recovery & Resource Limits - Context

**Gathered:** 2026-03-21
**Status:** Ready for planning

<domain>
## Phase Boundary

Docker integration tests verifying operational signals (SIGHUP/SIGUSR1/SIGTERM), DARE forensics and master key isolation, data directory migration, crash recovery with cursor resumption, and DoS resistance (write/sync rate limiting, session limits, storage full, namespace quotas, thread pool saturation). Requirements: OPS-01..03, DR-01..05, DOS-01..06. Test harness from Phase 47 is reused (helpers.sh, run-integration.sh, Docker compose patterns).

</domain>

<decisions>
## Implementation Decisions

### Test overlap handling
- New standalone tests for OPS-01 and DR-05, NOT extending ACL-05 or NET-05. Clean separation by domain — ACL-05 stays for ACL concerns, NET-05 stays for sync convergence. OPS-01 tests SIGHUP for rate_limit changes specifically. DR-05 tests MDBX transaction integrity + cursor resumption post-crash.
- OPS-03 (SIGTERM graceful shutdown): SIGTERM during active ingest. Start continuous ingest, send SIGTERM mid-stream, verify clean exit code + no corruption on restart + integrity scan passes. Not during sync — ingest scenario is sufficient.

### DARE forensics (DR-01, DR-02, DR-03)
- DR-01 verification: dual approach. Primary: grep for known strings (ingest blobs with known content like 'PLAINTEXT_MARKER_12345', then hexdump data.mdb and grep for that marker + known namespace hex + known pubkey bytes — absence = proof of encryption). Supplementary: entropy analysis (byte distribution should be near-random for encrypted data).
- DR-02 + DR-03: two-node volume swap. Node A writes blobs. Copy Node A's data.mdb to Node B (different master.key). Node B fails to start or can't read blobs (AEAD failure). Also: remove master.key from Node A entirely, verify it refuses to start.
- DR-04 (data directory migration): copy data_dir to new container with same master.key. Verify: node starts, accepts peer connections, serves existing blobs via sync, cursor state preserved (new blobs only synced, not full resync).

### Storage full & quotas (DOS-04, DOS-05)
- DOS-04 storage full: use max_storage_bytes config as primary mechanism (it's what triggers StorageFull in the protocol). tmpfs with size limit as supplementary check if needed. Recovery: SIGHUP with higher max_storage_bytes — node re-evaluates storage, clears StorageFull state, peers resume pushing.
- DOS-05 namespace quotas: 3 namespaces. Namespace A with tight quota (e.g. 10 blobs/100KB), Namespace B with generous quota, Namespace C with no quota as control. Fill A past quota, verify rejection. Write to B and C, verify success. Proves namespace isolation.

### Rate limiting & sessions (DOS-01, DOS-02, DOS-03)
- DOS-03 concurrent session limit: 5+ nodes all bootstrapping to one target with session limit set low (e.g. 2). Excess sessions get SyncRejected. Verify the first sessions complete and the rest are rejected.

### Thread pool saturation (DOS-06)
- Saturation method: 4-8 concurrent loadgen instances at max rate against one node. Sheer volume of concurrent ingests saturates ML-DSA-87 verify on the thread pool.
- "Event loop not starved" verification: (1) a fresh node successfully connects (handshake completes) during saturation, (2) SIGUSR1 metrics dump responds within timeout. Both prove the event loop is still responsive.

### Claude's Discretion
- Plan grouping (3 plans by domain vs 4 plans with DOS split) — group based on test complexity and dependencies
- DOS-01/DOS-02 compose topology sharing — separate tests may share compose infrastructure or not
- Exact rate limit values, blob sizes, timeouts, and threshold configurations for all tests
- Docker compose file structure (new files vs reusing existing topologies)
- Helper function additions for new test patterns (hexdump, entropy check, multi-loadgen orchestration)
- Log message patterns to grep for verification

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. All 14 requirements have precise verification criteria in REQUIREMENTS.md. Test scripts should follow established patterns from phases 47-49 (source helpers.sh, trap cleanup EXIT, log inspection, SIGUSR1 metrics).

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `tests/integration/helpers.sh`: wait_healthy, get_blob_count, wait_sync, run_loadgen, run_verify, cleanup
- `tests/integration/run-integration.sh`: Test runner with --skip-build and --filter
- `tests/integration/docker-compose.recon.yml`: 2-node recon topology (good base for rate limit and crash tests)
- `tests/integration/test_acl05_sighup_reload.sh`: SIGHUP pattern (writable config mount, docker kill -s HUP, log inspection)
- `tests/integration/test_net05_crash_recovery.sh`: Kill-9 + restart pattern (background loadgen, exit code check, sync convergence)
- `chromatindb_loadgen`: Blob injection with --count, --size, --ttl, --rate, --key-file, --namespace, --delete flags
- `chromatindb_verify`: hash-fields, sig-fields subcommands for crypto verification

### Established Patterns
- Per-test shell scripts sourcing helpers.sh, trap cleanup EXIT
- Log inspection via `docker logs $container 2>&1 | grep`
- Metrics via `docker kill -s USR1` + log parsing
- Config injection via volume-mounted JSON files (writable for SIGHUP tests)
- Fixed-IP Docker networks for deterministic peer configs
- Named Docker volumes for identity persistence across container restarts

### Integration Points
- SIGHUP config reload: already proven in ACL-05 (writable mount + docker kill -s HUP)
- StorageFull: protocol-level rejection triggered by max_storage_bytes config
- Rate limiting: rate_limit_bytes_per_sec config, verified via peer disconnect + log inspection
- Thread pool: asio::thread_pool for ML-DSA-87 verify + SHA3-256 hash offload
- Namespace quotas: per-namespace byte/count limits configured in node JSON

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 50-operations-disaster-recovery-resource-limits*
*Context gathered: 2026-03-21*
