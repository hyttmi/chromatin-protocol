# Phase 52: Stress, Chaos & Fuzzing - Context

**Gathered:** 2026-03-22
**Status:** Ready for planning

<domain>
## Phase Boundary

Prove chromatindb survives adversarial conditions: long-running load (4h), random node churn, rapid namespace creation, concurrent mixed operations, and malformed protocol input — without crashes, memory leaks, deadlocks, or corruption. Six requirements: STRESS-01, STRESS-02, STRESS-03, STRESS-04, SAN-04, SAN-05.

</domain>

<decisions>
## Implementation Decisions

### Fuzzing approach (SAN-04, SAN-05)
- Custom crafted payloads, not a fuzzing framework (AFL++/libFuzzer)
- Python script in Docker (python:3-slim) using raw sockets, run on the test network
- Payload categories — full depth: core protocol + crypto-specific + semantic:
  - Core: truncated length prefix, oversized length, zero-length frame, garbage bytes, valid-length garbage payload, truncated FlatBuffer mid-field
  - Crypto: invalid AEAD tag, wrong nonce length, truncated ciphertext, replay of encrypted frame with modified bytes
  - Semantic: valid FlatBuffer with wrong message type enum, valid structure but impossible field values (negative sizes, future version bytes)
- SAN-05 handshake fuzzing: each PQ handshake stage tested independently (malformed ClientHello, malformed ServerHello/KEM ciphertext, malformed session key confirmation), each verifies clean disconnect
- Verification: node stays alive after all fuzzing, no crash, no memory corruption (check process still running + can accept valid connections after)

### Long-running test ergonomics (STRESS-01)
- STRESS-01 excluded from default run-integration.sh run — invoked explicitly only
- Other stress tests (STRESS-02/03/04) and fuzz tests (SAN-04/05) included in default runner
- Supports --duration flag: default 4h for official runs, accepts shorter durations (e.g., --duration 10m) for dev/debug
- Memory monitoring via docker stats RSS snapshots every 60s, logged to file. Verify max RSS < 2x initial RSS
- Convergence checks every 5 minutes via SIGUSR1 metrics dump — logs blob counts across all 3 nodes. Final convergence check at end

### Churn orchestration (STRESS-02)
- docker kill (SIGKILL) — hardest crash scenario, no graceful shutdown
- Kill one or two nodes randomly per cycle (random choice each iteration)
- 5-node cluster, 30-minute churn duration, kill/restart every 30s
- Continuous ingest to surviving nodes during churn — tests write availability under instability
- 120-second convergence window after churn stops, then verify identical blob sets via XOR fingerprints

### Concurrent operations (STRESS-04)
- Background bash jobs: multiple background processes running simultaneously (loadgen ingest, loadgen delete, SIGHUP loop, SIGUSR1 loop)
- 5-minute duration for the concurrent ops phase
- Verification: restart all nodes after ops stop (triggers integrity scan), verify clean startup, then check XOR fingerprints match across cluster

### Namespace scaling (STRESS-03)
- 1000 fresh identities, each writes 10 blobs to its own namespace
- Loadgen runs sequentially or in small batches
- Verify all sync correctly across cluster, cursor storage bounded

### Claude's Discretion
- Docker compose topology details (network subnets, container naming)
- Exact Python fuzzer script structure and payload byte sequences
- Loadgen batch sizing for 1000-namespace test
- RSS multiplier threshold (2x suggested, Claude can adjust based on baseline measurements)
- Exact background job timing in STRESS-04 (SIGHUP frequency, metrics dump frequency)

</decisions>

<specifics>
## Specific Ideas

- STRESS-01 should feel like a soak test — steady state, not burst. 10 blobs/sec mixed sizes for the full duration.
- Churn test should be genuinely chaotic — random kill count (1 or 2), random node selection, no predictable pattern.
- Python fuzzer should be self-contained: one script, one Dockerfile (or use python:3-slim directly), clear pass/fail per payload category.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `helpers.sh`: get_blob_count, wait_sync, wait_healthy, run_loadgen, run_verify, cleanup primitives
- `run-integration.sh`: test runner with --skip-build and --filter support
- `chromatindb_loadgen`: --mixed, --rate, --count, --ttl, --delete, --identity-save, --identity-file, --namespace flags
- `chromatindb_verify`: hash-fields/sig-fields subcommands for integrity verification
- Docker compose topologies: test, mesh, recon, acl, dedup, trusted, mitm — all with established patterns

### Established Patterns
- Dedicated Docker network per test with unique subnets (172.XX.0.0/16)
- Unique container names per test (chromatindb-{testid}-nodeN)
- Named Docker volumes for persistent identity across container restarts
- SIGUSR1 + grep for metrics verification
- Integrity scan output (blobs= from startup) for post-restart verification
- XOR fingerprints for convergence verification

### Integration Points
- New test scripts in tests/integration/test_stress*.sh and test_san04*.sh, test_san05*.sh
- Python fuzzer script + Dockerfile in tests/integration/fuzz/ (or similar)
- Existing run-integration.sh discovers test_*.sh automatically (STRESS-01 excluded by naming convention or explicit skip)

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 52-stress-chaos-fuzzing*
*Context gathered: 2026-03-22*
