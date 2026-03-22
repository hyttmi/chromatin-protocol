---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: milestone
status: completed
last_updated: "2026-03-22T04:22:38.178Z"
last_activity: 2026-03-22 -- Completed 52-03 (STRESS-01 soak test + STRESS-04 concurrent ops)
progress:
  total_phases: 7
  completed_phases: 7
  total_plans: 23
  completed_plans: 23
  percent: 100
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-21)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** v1.0.0 Phase 52 -- Stress, Chaos & Fuzzing

## Current Position

Phase: 52 of 52 (Stress, Chaos & Fuzzing)
Plan: 3 of 3 complete
Status: Phase 52 complete
Last activity: 2026-03-22 -- Completed 52-03 (STRESS-01 soak test + STRESS-04 concurrent ops)

Progress: [██████████] 100%

## Performance Metrics

**Velocity:**
- Total plans completed: 92 (across v1.0 - v1.0.0)
- Average duration: ~17 min (historical)
- Total execution time: ~27 hours

**By Milestone:**

| Milestone | Phases | Plans | Timeline | Avg/Plan |
|-----------|--------|-------|----------|----------|
| v1.0 MVP | 8 | 21 | 3 days | ~25 min |
| v2.0 Closed Node | 3 | 8 | 2 days | ~20 min |
| v3.0 Real-time | 4 | 8 | 2 days | ~15 min |
| v0.4.0 Production | 6 | 13 | 5 days | ~15 min |
| v0.5.0 Hardening | 5 | 6 | 2 days | ~19 min |
| v0.6.0 Validation | 5 | 6 | 2 days | ~5 min |
| v0.7.0 Production Readiness | 6 | 12 | 2 days | ~13 min |
| v0.8.0 Protocol Scalability | 4 | 8 | 1 day | ~24 min |
| v0.9.0 Connection Resilience | 4 | 8 | 1 day | ~19 min |
| Phase 47 P02 | 5min | 2 tasks | 5 files |
| Phase 47 P04 | 2min | 1 tasks | 1 files |
| Phase 48 P01 | 28min | 2 tasks | 7 files |
| Phase 48 P03 | 24min | 1 tasks | 5 files |
| Phase 48 P02 | 32min | 2 tasks | 5 files |
| Phase 49 P01 | 23min | 2 tasks | 12 files |
| Phase 49 P03 | 48min | 2 tasks | 5 files |
| Phase 49 P02 | 54min | 2 tasks | 2 files |
| Phase 49 P04 | 3min | 2 tasks | 4 files |
| Phase 50 P01 | 10min | 2 tasks | 3 files |
| Phase 50 P04 | 22min | 2 tasks | 6 files |
| Phase 50 P03 | 26min | 2 tasks | 3 files |
| Phase 50 P02 | 22min | 2 tasks | 4 files |
| Phase 51 P02 | 19min | 2 tasks | 3 files |
| Phase 51 P01 | 23min | 2 tasks | 3 files |
| Phase 51 P03 | 32min | 2 tasks | 8 files |
| Phase 52 P02 | 3min | 2 tasks | 2 files |
| Phase 52 P01 | 4min | 2 tasks | 4 files |
| Phase 52 P03 | 4min | 2 tasks | 3 files |

## Accumulated Context

### Decisions

All decisions logged in PROJECT.md Key Decisions table.

- Phase 46 sanitizer findings: coroutine params by value, recv_sync_msg executor transfer, silent SyncRequest drop, UBSAN nonnull exclusion
- Phase 47-01: chromatindb_verify links against chromatindb_lib (same crypto paths), JSON output, test-net isolation, 5s sync interval
- Phase 47-02: hash-fields/sig-fields subcommands for field-level crypto verification, --verbose-blobs stderr output, CRYPT-03 handles both mdbx and DARE failure modes
- Phase 47-03: nicolaka/netshoot for tcpdump, MITM tested via ACL rejection + session fingerprint uniqueness, fixed-IP Docker networks for deterministic config
- [Phase 47]: hash-fields/sig-fields subcommands for field-level crypto verification, --verbose-blobs stderr output, CRYPT-03 handles both mdbx and DARE failure modes
- [Phase 47]: Phase 47-04: Dynamic config with namespace discovery + allowed_keys for strict CRYPT-06 Part 3 identity rejection test
- [Phase 48]: ACL-02 tests namespace isolation (separate namespace creation) rather than engine rejection path, since loadgen always writes to its own namespace
- [Phase 48]: Named Docker volume pattern for preserving node identity across container restarts in multi-phase ACL tests
- [Phase 48]: Connection dedup: lower namespace_id keeps its initiated connection (deterministic tie-break). Server::stop_reconnect() prevents reconnect-dedup infinite cycles.
- [Phase 48]: Added --namespace flag to loadgen for delegation writes (delegate must target owner's namespace_id, not its own)
- [Phase 49]: full_resync_interval=9999 (not 0) for test configs -- validation requires >= 1
- [Phase 49]: depends_on in recon compose for reliable startup order -- simultaneous boot causes sync deadlocks
- [Phase 49]: Large blob (100M) notification ACK timeout is cosmetic -- verify via node blob count not loadgen errors
- [Phase 49]: Rate 2000/sec for bulk loadgen ingest (>1000 blobs) -- sync timer at 5s disconnects loadgen at lower rates
- [Phase 49]: RECON-01 baseline restored to 10,000 blobs after helpers.sh --tail fix resolved get_blob_count unreliability at scale
- [Phase 49]: Fixed IP for tcpdump capture containers to avoid Docker address conflicts with stopped nodes
- [Phase 49]: docker network disconnect/connect over iptables for partition tests -- simpler, no NET_ADMIN needed
- [Phase 49]: Relative blob count comparison for partition isolation -- blobs= metric overcounts across namespaces
- [Phase 49]: Dual-network topology for split-brain (NET_A + NET_B) -- group B internal comms on NET_B during partition
- [Phase 49]: Auto-reconnect backoff for healing instead of SIGHUP -- clear_reconnect_state kills reconnect_loop coroutines
- [Phase 49]: Hash verification skips --verbose-blobs for 10M/100M blobs (data_hex = 20-200 MB hex) -- AEAD integrity is implicit
- [Phase 49]: Single sample blob hash verification on late-joiner is sufficient -- crypto path + AEAD + fingerprint convergence

- [Phase 50]: Unique container names per test (chromatindb-ops0N-nodeN) to prevent cross-test collisions when running via --filter ops
- [Phase 50]: rate_limit_bytes_per_sec=1024 (config validation minimum) with burst=2048 for tightest valid rate limit test
- [Phase 50]: max_storage_bytes minimum 2 MiB for DOS-04 (mdbx file starts at ~1 MiB empty, 200KB below config minimum)
- [Phase 50]: Added Engine::set_max_storage_bytes() for SIGHUP reload -- was missing from reload path, needed for storage full recovery
- [Phase 50]: DOS-06 checks handshake completion count on Node1 (not live peer count) -- loadgens disconnect before verification
- [Phase 50]: METRICS DUMP marker for SIGUSR1 response counting (avoids metrics: prefix ambiguity with quota_rejections/sync_rejections)
- [Phase 50]: mdbx.dat is actual database filename (not data.mdb); DR-02 auto-generates new master.key but old DARE data unreadable; DR-03 manifests as SIGSEGV during integrity scan
- [Phase 50]: DOS-03 uses sync_cooldown=5s (not 0) -- per-peer syncing flag is initiator-checked so overlapping sessions from same peer don't generate server-side rejections without cooldown
- [Phase 50]: Dedicated Docker networks per DoS test (172.36/37/38.0.0/16) prevent interference between concurrent test runs

- [Phase 51]: E2E-03 propagation timing margin 20s (5s sync + 15s overhead jitter) prevents false failures from clock jitter
- [Phase 51]: Unique Docker subnets per E2E test (172.41/42/43) and unique container names (chromatindb-e2eNN-nodeN) prevent cross-test collisions
- [Phase 51]: Pub/sub notification check is soft (WARN not FAIL) -- async delivery proven by blob count convergence
- [Phase 51]: TTL-03 verifies structural expiry exemption (0 expiry entries for TTL=0) rather than actual GC timing due to pre-existing timestamp/clock units mismatch
- [Phase 51]: TTL-01 uses integrity scan restart to verify tombstone_map entries (restart triggers startup integrity_scan)
- [Phase 51]: Fixed timestamp/TTL units mismatch: timestamps are microseconds, TTL/clock are seconds. Expiry computation now normalizes via timestamp/1000000
- [Phase 51]: Integrity scan output (blobs= from startup) used for GC verification -- SIGUSR1 metrics sum latest_seq_num which never decreases
- [Phase 51]: grep "integrity scan: blobs=" to filter info line from warning lines in integrity scan log output
- [Phase 51]: TTL-04 reuses docker-compose.acl.yml topology for consistency with ACL-03/ACL-04 delegation tests
- [Phase 51]: Dedicated Docker subnets: 172.39.0.0/16 (TTL-01), 172.40.0.0/16 (TTL-03)

- [Phase 52]: Inline ingest per churn iteration (20 blobs per cycle) instead of background container -- avoids dead-node targeting complexity
- [Phase 52]: 5% convergence tolerance for STRESS-02 (in-flight blobs during final kills); 1% for STRESS-03 (no chaos)
- [Phase 52]: info log level for stress tests (30 min debug logs would be enormous)
- [Phase 52]: Dedicated Docker subnets: 172.48.0.0/16 (STRESS-02), 172.49.0.0/16 (STRESS-03)
- [Phase 52]: Dedicated Docker subnets: 172.46.0.0/16 (SAN-04), 172.47.0.0/16 (SAN-05)
- [Phase 52]: Self-contained Python fuzzer using only stdlib -- no pip installs, single Dockerfile
- [Phase 52]: Mixed-size ingest via round-robin loadgen batches (5 sizes) for STRESS-01; EXCLUDED_TESTS array for extensible long-running test exclusion

### Pending Todos

None.

### Blockers/Concerns

- Pre-existing full-suite hang in release build when running all 469 tests together (port conflict in test infrastructure) -- deferred
- [Phase 51] Timestamp/TTL units mismatch was silently preventing all GC in production -- fixed in a22d611
