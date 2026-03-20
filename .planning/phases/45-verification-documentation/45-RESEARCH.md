# Phase 45: Verification & Documentation - Research

**Researched:** 2026-03-20
**Domain:** Crash recovery testing, delegation quota verification, README/protocol documentation
**Confidence:** HIGH

## Summary

Phase 45 is the final phase of v0.9.0 -- it empirically validates two storage properties (STOR-04: libmdbx crash recovery, STOR-05: delegation quota enforcement) and updates both README and protocol documentation to reflect all v0.8.0 and v0.9.0 changes. This phase involves zero new dependencies and no production code changes. All four requirements are verification/documentation tasks that build on the existing Docker test infrastructure and Catch2 test suite.

The crash recovery testing (STOR-04) leverages libmdbx's built-in ACID guarantees (shadow paging with `robust_synchronous` durability mode, no WAL) and validates them empirically via Docker kill-9 scenarios. The delegation quota verification (STOR-05) confirms the already-correct code path (`blob.namespace_id` for quota lookup, which is always the owner's namespace) with explicit Catch2 test cases. Documentation updates (DOCS-01, DOCS-02) extend existing README sections and add missing protocol reference material for SyncRejected(30), rate limiting, and inactivity detection.

**Primary recommendation:** Structure as four plans -- crash test script (STOR-04), delegation quota tests (STOR-05), README update (DOCS-01), protocol doc update (DOCS-02). The two verification plans should precede documentation since they may surface edge cases worth documenting.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Crash recovery testing via Docker script at `deploy/test-crash-recovery.sh`, standalone alongside `run-benchmark.sh`
- Two kill scenarios: kill-9 during idle (after ingest) AND kill-9 during active sync (mid-reconciliation)
- Four integrity checks: data intact, no stale readers, cursors resumable, clean startup logs
- Delegation quota tests as Catch2 unit tests (not Docker integration)
- Three explicit delegation quota scenarios: delegate counts against owner, owner at limit rejects delegate, mixed writes
- README extends existing sections (Configuration, Features, Scenarios) -- no structural changes
- Full default config JSON example with all v0.9.0 fields
- New features to document: config validation, structured JSON logging, file logging, cursor compaction, startup integrity scan, auto-reconnect with backoff, ACL-aware reconnection suppression, inactivity timeout
- Two new deployment scenarios: logging config, resilient node
- Protocol doc: add SyncRejected(30) to message table, new Rate Limiting subsection, new Inactivity Detection subsection
- Update SIGHUP section with new reloadable fields

### Claude's Discretion
- Crash test script implementation details (timing, blob counts, wait durations)
- Additional delegation quota edge cases beyond the three specified
- README wording and feature description prose
- Protocol doc section organization within "Additional Interactions"
- Any additional protocol gaps identified during implementation

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STOR-04 | libmdbx crash recovery verified via kill-9 test scenarios with data integrity checks post-restart | Docker script pattern from run-benchmark.sh; libmdbx robust_synchronous durability; env.check_readers() for stale slot detection; integrity_scan() for startup validation |
| STOR-05 | Delegate writes correctly counted against namespace owner's quota | Existing engine.cpp:172-188 quota path uses blob.namespace_id (owner's NS); existing make_delegate_blob/make_signed_delegation test helpers in test_engine.cpp; Catch2 run_async pattern from test_peer_manager.cpp |
| DOCS-01 | README updated with all v0.9.0 features | Existing README.md structure: Features bold-header paragraphs, Config bullet list, Scenarios code blocks, Signals section; seven new config fields; eight new features; two new scenarios |
| DOCS-02 | Protocol documentation current with v0.8.0 wire changes and v0.9.0 keepalive behavior | PROTOCOL.md missing SyncRejected(30), rate limiting, inactivity detection; SyncRejected payload: 1-byte reason code (0x01=cooldown, 0x02=session_limit, 0x03=byte_rate) |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Catch2 | v3 (latest via FetchContent) | Unit test framework for STOR-05 | Already used by all 408 existing tests |
| Docker Compose v2 | latest | Container orchestration for STOR-04 | Already used by deploy/docker-compose.yml |
| Bash | 5.x | Crash test script | Matches existing run-benchmark.sh pattern |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| jq | latest | JSON parsing in crash test script | Parsing loadgen output for blob counts |
| chromatindb_loadgen | current | Protocol-compliant load generation | Ingesting blobs before kill-9 in crash test |

### Alternatives Considered
None -- this phase uses only existing infrastructure and tooling.

**Installation:**
No new dependencies. All tools already available in the Docker build and test environments.

## Architecture Patterns

### Crash Test Script Pattern (STOR-04)

The crash test script follows the established `run-benchmark.sh` pattern:

```
deploy/
├── run-benchmark.sh          # Existing benchmark script
├── test-crash-recovery.sh    # New crash test script (STOR-04)
├── docker-compose.yml        # Existing topology (reused)
└── configs/                  # Existing node configs (reused)
```

**Script lifecycle:**
1. Build image (or `--skip-build`)
2. Start topology via compose
3. Wait for health checks
4. Ingest known blobs via loadgen
5. Record pre-crash state (blob count, cursor positions)
6. Kill container with `docker kill --signal=KILL` (equivalent to kill -9)
7. Restart container
8. Wait for health check
9. Verify integrity (4 checks)
10. Compose down

**Two scenarios run sequentially:**
- Scenario A: Kill during idle (after ingest completes, before next sync)
- Scenario B: Kill during active sync (start loadgen on node1, kill node2 while syncing)

### Delegation Quota Test Pattern (STOR-05)

Follow existing test_engine.cpp patterns. Key test helpers already exist:

```cpp
// Already in test_engine.cpp anonymous namespace:
make_signed_blob(id, payload, ttl, timestamp)
make_signed_delegation(owner, delegate, timestamp)
make_delegate_blob(owner, delegate, payload, ttl, timestamp)
```

Test structure pattern:
```cpp
TEST_CASE("Delegate write counts against owner quota", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // Set count quota of N on owner's namespace
    BlobEngine engine(store, pool, 0, 0, N);

    // Create delegation
    auto deleg = make_signed_delegation(owner, delegate);
    auto r_deleg = run_async(pool, engine.ingest(deleg));
    REQUIRE(r_deleg.status == IngestStatus::stored);

    // Delegate writes blob -- should count against OWNER's quota
    auto blob = make_delegate_blob(owner, delegate, "delegate-data");
    auto r = run_async(pool, engine.ingest(blob));
    REQUIRE(r.status == IngestStatus::stored);

    // Verify quota usage on owner's namespace
    auto quota = store.get_namespace_quota(owner.namespace_id());
    REQUIRE(quota.blob_count == 2);  // delegation blob + delegate's blob
}
```

### Documentation Update Pattern

**README (DOCS-01):** Extend in-place. The README has clear sections:
- **Configuration** section: Add 7 new fields to the JSON example and bullet list
- **Features** section: Add 8 new bold-header paragraphs
- **Scenarios** section: Add 2 new deployment scenarios
- **Signals** section: Update SIGHUP reloadable fields list

**Protocol doc (DOCS-02):** Add to existing structure:
- **Message Type Reference** table: Add SyncRejected (30) row
- **Additional Interactions** section: Add Rate Limiting and Inactivity Detection subsections
- Update "29 message types" count to 30 in README wire protocol summary

### Anti-Patterns to Avoid
- **Flaky timing in crash test:** Do NOT rely on fixed `sleep` durations for sync timing. Use health check polling and log grepping instead.
- **Testing crash recovery in Catch2:** libmdbx crash recovery requires actual process termination (kill -9), not in-process simulation. Docker is the right tool.
- **Testing quota in Docker:** Delegation quota is a pure unit test -- Docker adds nothing but complexity. Catch2 is correct.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Load generation for crash test | Custom blob sender | chromatindb_loadgen | Protocol-compliant, handles PQ handshake, measures ACK |
| Container health polling | Custom TCP probe | Docker healthcheck + `docker inspect` | Already configured in compose |
| Stale reader detection | Manual `/proc` inspection | `env.check_readers()` (libmdbx C++ API) | Built-in libmdbx function, returns dead count |
| Test helpers for delegation | New helper functions | Existing `make_delegate_blob()`, `make_signed_delegation()` | Already in test_engine.cpp |

**Key insight:** The project has mature Docker and test infrastructure. This phase reuses everything -- the only new artifact is a bash script and a few test cases.

## Common Pitfalls

### Pitfall 1: Stale Reader Slots Are Not a Problem for Single-Process
**What goes wrong:** Spending time verifying stale reader slots when chromatindb is single-process.
**Why it happens:** libmdbx stale readers are primarily a multi-process concern. In single-process mode (chromatindb's architecture), all reader slots are owned by the same PID. After kill-9 and restart, the new process opens the environment fresh -- old reader slots are automatically detected and cleared by libmdbx on env open.
**How to avoid:** The crash test should verify zero stale readers post-restart (confirming libmdbx cleanup works), but this check is informational, not a failure indicator.
**Warning signs:** If stale readers are found after restart, it indicates a libmdbx bug, not a chromatindb bug.

### Pitfall 2: Kill During Sync Timing
**What goes wrong:** The kill-during-sync scenario fires too early (before sync starts) or too late (after sync completes).
**Why it happens:** Sync timing depends on `sync_interval_seconds` and network conditions.
**How to avoid:** Configure a short sync interval (5-10 seconds) for the test topology. Start loadgen on one node, wait for the other node's logs to show sync activity ("reconciliation" or "SyncRequest"), then kill immediately. Alternatively, use a tight loop polling `docker logs --tail 5` for sync-related messages before killing.
**Warning signs:** If the killed node has zero blobs from the peer, sync hadn't started yet.

### Pitfall 3: Delegation Blob Counts Against Quota
**What goes wrong:** Forgetting that the delegation blob itself also counts against the owner's namespace quota.
**Why it happens:** Delegation blobs are stored in the owner's namespace (same `namespace_id`). They consume both byte and count quota.
**How to avoid:** Account for delegation blob(s) in quota arithmetic. If count quota is 3 and there's 1 delegation blob, only 2 data blobs fit.
**Warning signs:** Off-by-one errors in quota tests because delegation blob wasn't counted.

### Pitfall 4: README Config JSON Example Drift
**What goes wrong:** The README config example doesn't match the actual Config struct defaults.
**Why it happens:** Config fields are added to config.h but not to README.
**How to avoid:** Cross-reference every field in `Config` struct (config.h) against the README JSON example. There are now 25 fields total. Verify default values match.
**Warning signs:** A field exists in config.h but not in README, or vice versa.

### Pitfall 5: Protocol Doc Message Count
**What goes wrong:** README says "29 message types" but with SyncRejected(30) there are now 30.
**Why it happens:** The count in README was written before v0.8.0 rate limiting.
**How to avoid:** Update the count in the README Wire Protocol section when adding SyncRejected to PROTOCOL.md.

## Code Examples

### Crash Test: Docker Kill and Verify Pattern
```bash
# Source: deploy/run-benchmark.sh pattern + Docker documentation
# Kill node2 with SIGKILL (equivalent to kill -9)
docker kill --signal=KILL chromatindb-node2

# Restart the container
docker start chromatindb-node2

# Wait for health check
wait_for_health() {
    local container=$1 timeout=30 start=$(date +%s)
    while true; do
        status=$(docker inspect --format '{{.State.Health.Status}}' "$container" 2>/dev/null || echo "missing")
        [[ "$status" == "healthy" ]] && return 0
        elapsed=$(( $(date +%s) - start ))
        [[ $elapsed -ge $timeout ]] && return 1
        sleep 1
    done
}
wait_for_health chromatindb-node2
```

### Crash Test: Verify Data Integrity
```bash
# Source: existing loadgen --count pattern
# Pre-crash: record blob count
PRE_COUNT=$(docker exec chromatindb-node2 chromatindb_loadgen --target 127.0.0.1:4200 --count 0 2>&1 | ... )

# Alternative: use SIGUSR1 metrics dump to get blob count
docker kill --signal=USR1 chromatindb-node2
docker logs --tail 20 chromatindb-node2 2>&1 | grep "blobs_ingested"
```

### Delegation Quota Test: Core Assertion Pattern
```cpp
// Source: Existing test_engine.cpp patterns (lines 111-154, 1457-1650)
TEST_CASE("Delegate write counts against owner namespace quota", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // Count quota of 3 (delegation blob + 2 data blobs = full)
    BlobEngine engine(store, pool, 0, 0, 3);

    // Owner creates delegation (counts as blob #1)
    auto deleg = make_signed_delegation(owner, delegate);
    auto r0 = run_async(pool, engine.ingest(deleg));
    REQUIRE(r0.status == IngestStatus::stored);

    // Delegate writes blob (counts as blob #2 in OWNER's namespace)
    auto blob1 = make_delegate_blob(owner, delegate, "d1", 604800, 4000);
    auto r1 = run_async(pool, engine.ingest(blob1));
    REQUIRE(r1.status == IngestStatus::stored);

    // Owner writes blob (blob #3 -- at quota)
    auto blob2 = make_signed_blob(owner, "o1", 604800, 4001);
    auto r2 = run_async(pool, engine.ingest(blob2));
    REQUIRE(r2.status == IngestStatus::stored);

    // Delegate's 2nd write should be rejected (quota full)
    auto blob3 = make_delegate_blob(owner, delegate, "d2", 604800, 4002);
    auto r3 = run_async(pool, engine.ingest(blob3));
    REQUIRE(r3.error.has_value());
    REQUIRE(r3.error.value() == IngestError::quota_exceeded);
}
```

### README: New Config Fields Pattern
```json
{
  "sync_cooldown_seconds": 30,
  "max_sync_sessions": 1,
  "log_file": "",
  "log_max_size_mb": 10,
  "log_max_files": 3,
  "log_format": "text",
  "inactivity_timeout_seconds": 120
}
```

### Protocol Doc: SyncRejected Message Format
```
SyncRejected (type 30):
[reason: 1 byte]

Reason codes:
  0x01 = Cooldown (peer initiated sync before cooldown elapsed)
  0x02 = Session limit (max concurrent sync sessions reached)
  0x03 = Byte rate (sync traffic exceeded rate limit)
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| HashList(12) sync | ReconcileInit(27)/Ranges(28)/Items(29) | v0.8.0 (Phase 39) | PROTOCOL.md already updated for reconciliation; HashList marked removed |
| No sync rate limiting | SyncRejected(30) with cooldown + session limits | v0.8.0 (Phase 40) | Missing from PROTOCOL.md -- DOCS-02 adds it |
| No inactivity detection | Receiver-side inactivity timeout | v0.9.0 (Phase 44) | Missing from PROTOCOL.md -- DOCS-02 adds it |
| Manual version.h | CMake configure_file version injection | v0.9.0 (Phase 42) | README version command already documented |
| Console-only logging | Console + file + JSON format | v0.9.0 (Phase 43) | Missing from README -- DOCS-01 adds it |

**Deprecated/outdated:**
- BLOB_TTL_SECONDS config field: removed in v0.5.0 (TTL is writer-controlled per-blob)
- HashList(12) message type: removed in v0.8.0 (replaced by reconciliation)
- version.h: removed in v0.9.0 (replaced by CMake injection)

## Open Questions

1. **Crash test: How to verify cursor resumption?**
   - What we know: Cursors are stored in libmdbx (persist across restarts). After crash + restart, the node should resume sync from last committed cursor, not from zero.
   - What's unclear: The test script can't directly query cursor state from outside the process. Must infer from sync behavior (e.g., second sync after restart doesn't re-transfer all blobs).
   - Recommendation: Verify indirectly -- after restart, trigger a sync round and confirm via logs that reconciliation finds zero or near-zero differences (meaning cursor was intact and the pre-crash data was preserved). Alternatively, check integrity_scan output which logs cursor entry count.

2. **Crash test: Blob count verification method**
   - What we know: SIGUSR1 dumps metrics including blobs_ingested. Loadgen reports sent count.
   - What's unclear: No direct "query blob count" CLI command exists.
   - Recommendation: Use SIGUSR1 metrics dump to capture `blobs_ingested` count, or use loadgen in read/count mode if available. Failing that, parse integrity_scan output at startup which logs `blobs=N`.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3 (latest via FetchContent) |
| Config file | CMakeLists.txt (test targets defined inline) |
| Quick run command | `cd build && ctest -R "delegation.*quota\|quota.*delegation" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| STOR-04 | Crash recovery data integrity | Docker integration | `bash deploy/test-crash-recovery.sh` | Wave 0 (new) |
| STOR-05 | Delegation counts against owner quota | unit | `cd build && ctest -R "delegation.*quota" --output-on-failure` | Wave 0 (new) |
| DOCS-01 | README accuracy | manual review | N/A (prose content) | manual-only -- documentation correctness requires human review |
| DOCS-02 | Protocol doc accuracy | manual review | N/A (prose content) | manual-only -- documentation correctness requires human review |

### Sampling Rate
- **Per task commit:** `cd build && ctest --output-on-failure` (unit tests only -- STOR-05)
- **Per wave merge:** Full suite + crash test script
- **Phase gate:** Full suite green + crash test passes before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `deploy/test-crash-recovery.sh` -- covers STOR-04
- [ ] New delegation quota test cases in `db/tests/engine/test_engine.cpp` -- covers STOR-05
- [ ] No framework gaps -- Catch2 and Docker already configured

## Sources

### Primary (HIGH confidence)
- Codebase inspection: `db/storage/storage.cpp:161-206` -- libmdbx env open with `robust_synchronous` durability
- Codebase inspection: `db/engine/engine.cpp:169-188` -- quota enforcement uses `blob.namespace_id` (owner's NS for delegates)
- Codebase inspection: `db/tests/engine/test_engine.cpp:111-154` -- existing delegation + quota test helpers
- Codebase inspection: `db/peer/peer_manager.cpp:69-73` -- SyncRejected reason codes
- Codebase inspection: `db/config/config.h:14-46` -- all 25 config fields with defaults
- Codebase inspection: `db/PROTOCOL.md` -- confirmed missing SyncRejected, rate limiting, inactivity detection
- Codebase inspection: `db/README.md` -- confirmed missing v0.9.0 config fields and features
- libmdbx C++ API: `build/_deps/libmdbx-src/mdbx.h++:3742-3744` -- `env::check_readers()` for stale slot detection

### Secondary (MEDIUM confidence)
- [libmdbx GitHub README](https://github.com/erthink/libmdbx/blob/master/README.md) -- ACID guarantees, no WAL, crash-proof claims
- [libmdbx official site](https://libmdbx.dqdkfa.ru/) -- durability mode documentation
- libmdbx C API: `mdbx.h:6208-6216` -- `mdbx_reader_check()` clears stale entries, returns dead count

### Tertiary (LOW confidence)
None -- all findings verified against codebase or official libmdbx documentation.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing infrastructure
- Architecture: HIGH -- patterns directly observed from codebase (run-benchmark.sh, test_engine.cpp)
- Pitfalls: HIGH -- derived from actual code paths and prior milestone decisions
- Documentation gaps: HIGH -- confirmed by grep against README.md and PROTOCOL.md

**Research date:** 2026-03-20
**Valid until:** 2026-04-20 (stable -- no external dependency changes expected)
