# Phase 36: Deletion Benchmarks - Research

**Researched:** 2026-03-18
**Domain:** Benchmark orchestration (bash), C++ load generator extension, Docker-based multi-node measurement
**Confidence:** HIGH

## Summary

Phase 36 adds three new benchmark scenarios to the existing Docker benchmark suite: tombstone creation throughput, tombstone sync propagation latency, and tombstone GC/expiry performance. The implementation extends two files -- `loadgen/loadgen_main.cpp` (add `--delete` mode with hash I/O) and `deploy/run-benchmark.sh` (add three `run_scenario_tombstone_*()` functions plus report integration).

The existing codebase provides all the building blocks. The loadgen already connects as a protocol-compliant peer, performs PQ handshake, and measures ACK latency via pub/sub notifications. The benchmark shell script already has `reset_topology()`, `wait_for_convergence()`, `capture_stats()`, and `generate_report()` patterns. Tombstone creation requires sending `TransportMsgType_Delete` (type 18) instead of `TransportMsgType_Data` (type 10), with the blob data replaced by tombstone data (4-byte magic `0xDEADBEEF` + 32-byte target hash). The server responds with `TransportMsgType_DeleteAck` (type 19) with format `[blob_hash:32][seq_num_be:8][status:1]`.

**Primary recommendation:** Extend loadgen with a `--delete` mode that reads blob hashes from stdin, constructs tombstones, sends Delete messages, and measures DeleteAck latency. The write phase must output hashes for the delete phase to consume. Shell script orchestration follows the exact same `run_scenario_*()` pattern already established.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Extend `chromatindb_loadgen` with `--delete` mode that creates tombstones for previously-written blobs
- Loadgen outputs blob hashes as JSON array in stdout (added to existing stats output) during write runs
- Delete mode reads hashes from stdin (`--hashes-from stdin`)
- Loadgen owns key management -- reuses the same keypair from the write phase (via `--identity-file` or same seed)
- Delete throughput stats use the same JSON output format as write stats (`blobs_per_sec`, `latency_ms.p50/p95/p99`, `scenario: "delete"`)
- Use existing SIGUSR1 metrics pattern: poll blob count until it drops to expected value after TTL expiry
- Blobs use TTL=30 seconds for the GC scenario
- Measure GC convergence across all 3 nodes (per-node GC timing reported)
- Use default expiry scan interval (no benchmark-specific override) -- results reflect production behavior
- Same BLOB_COUNT=200 and RATE=50 as existing ingest scenarios
- Test tombstone creation with all three blob sizes (1 KiB, 100 KiB, 1 MiB)
- Each tombstone scenario gets a fresh topology reset (separate `down -v`, `up -d`) -- no state leakage between scenarios
- Three new sections in REPORT.md: "Tombstone Creation", "Tombstone Sync Propagation", "Tombstone GC/Expiry"
- Each follows existing table format (metrics table + resource usage)
- Executive Summary gets 3 new rows: "Peak tombstone throughput" (blobs/sec), "Tombstone sync (2-hop)" (ms), "GC reclamation (worst)" (ms)
- benchmark-summary.json: flat keys under `scenarios{}` -- `tombstone_creation_1k`, `tombstone_creation_100k`, `tombstone_creation_1m`, `tombstone_sync`, `tombstone_gc`

### Claude's Discretion
- Loadgen internal implementation details (how identity is persisted between write/delete runs)
- Exact SIGUSR1 polling interval for GC detection
- Docker stats capture points within tombstone scenarios
- Error handling for edge cases (e.g., TTL expires before sync completes)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| BENCH-01 | Tombstone creation performance scenario added to Docker benchmark suite | Loadgen `--delete` mode sends `TransportMsgType_Delete` (type 18), measures `DeleteAck` latency. Shell script orchestrates write-then-delete with hash piping. Three blob sizes tested. |
| BENCH-02 | Tombstone sync propagation latency measured across multi-node topology | After delete on node1, `wait_for_convergence()` polls node2/node3 blob counts until they DROP by expected tombstone count. Measures 1-hop and 2-hop propagation. |
| BENCH-03 | Tombstone GC/expiry performance measured under load | Tombstones created with TTL=30. After 30s + expiry scan interval (60s hardcoded), blob count drops to 0. Poll SIGUSR1 metrics until convergence. Per-node timing reported. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | latest | Async I/O in loadgen | Already used by loadgen |
| nlohmann/json | latest | JSON output in loadgen | Already used by loadgen |
| spdlog | latest | Logging in loadgen | Already used by loadgen |

### Supporting
| Tool | Version | Purpose | When to Use |
|------|---------|---------|-------------|
| Docker Compose v2 | latest | Multi-node topology | Benchmark orchestration |
| jq | any | JSON processing in bash | Shell script report generation |
| bash | 5.x | Script orchestration | run-benchmark.sh |

### Alternatives Considered
None -- this phase extends existing infrastructure with no new dependencies.

## Architecture Patterns

### Recommended Project Structure
```
loadgen/
  loadgen_main.cpp        # Extended: --delete mode, --hashes-from, --identity-seed, blob_hashes[] output
deploy/
  run-benchmark.sh        # Extended: 3 new run_scenario_tombstone_*() functions + report sections
  results/
    scenario-tombstone-creation-{1k,100k,1m}.json  # New result files
    scenario-tombstone-sync.json                     # New result file
    scenario-tombstone-gc.json                       # New result file
```

### Pattern 1: Write-Then-Delete Pipeline (Shell Orchestration)
**What:** Tombstone benchmarks require a two-phase pipeline -- first write blobs, then delete them. Hashes from the write phase feed into the delete phase.
**When to use:** All tombstone creation scenarios (BENCH-01).
**Example:**
```bash
# Phase 1: Write blobs, capture output with blob hashes
write_output=$(run_loadgen node1 --count $BLOB_COUNT --rate $RATE --size $size --drain-timeout 10)
hashes=$(echo "$write_output" | jq -r '.blob_hashes[]')

# Phase 2: Delete blobs using hashes from write phase
delete_output=$(echo "$hashes" | run_loadgen node1 --delete --hashes-from stdin \
    --identity-seed $SEED --drain-timeout 10)
```

### Pattern 2: Identity Persistence via Deterministic Seed
**What:** The write and delete phases must use the same cryptographic identity (same keypair = same namespace). The loadgen currently generates an ephemeral identity. A `--identity-seed` parameter provides a deterministic seed to the PRNG used for key generation.
**When to use:** Any benchmark scenario that does write-then-delete.
**Implementation note:** `NodeIdentity::generate()` likely uses a random source. The simplest approach: save the identity to a temp file after the write phase (`--identity-save /tmp/bench-id.bin`) and reload it for the delete phase (`--identity-file /tmp/bench-id.bin`). Alternative: use a fixed seed. The CONTEXT says both are acceptable -- Claude's discretion on which.
**Recommendation:** Use `--identity-file` approach:
  - Write phase: loadgen generates identity, writes serialized keypair to a temp file
  - Delete phase: loadgen reads keypair from the same file
  - Simpler than seed-based determinism (no need to modify NodeIdentity constructor)

### Pattern 3: Convergence Polling for Blob Count Decrease
**What:** For tombstone sync measurement, we poll blob count waiting for it to DECREASE (not increase as in ingest scenarios). The existing `wait_for_convergence()` waits for count >= expected. For tombstones, we need count <= expected.
**When to use:** BENCH-02 (sync propagation) and BENCH-03 (GC reclamation).
**Example:**
```bash
wait_for_count_decrease() {
    local container="$1"
    local expected_count="$2"
    local timeout_seconds="$3"
    local start_ns
    start_ns=$(date +%s%N)

    while true; do
        local count
        count=$(get_blob_count "$container")
        local now_ns
        now_ns=$(date +%s%N)
        local elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))

        log "Waiting for $container: $count -> $expected_count blobs... (${elapsed_ms}ms)"

        if [[ "$count" -le "$expected_count" ]]; then
            echo "$elapsed_ms"
            return 0
        fi

        if [[ $elapsed_ms -gt $(( timeout_seconds * 1000 )) ]]; then
            log "ERROR: Timeout ($count != $expected_count after ${elapsed_ms}ms)"
            echo "$elapsed_ms"
            return 1
        fi

        sleep 2
    done
}
```

### Pattern 4: DeleteAck-Based Latency Measurement in Loadgen
**What:** In delete mode, the loadgen sends `TransportMsgType_Delete` messages and receives `TransportMsgType_DeleteAck` responses. Latency is measured from send to DeleteAck receipt (not from Notification, since tombstone notifications also fire but DeleteAck is the primary ack mechanism).
**When to use:** BENCH-01 tombstone creation throughput.
**Implementation:** The loadgen's `handle_message()` callback already dispatches on message type. Add a `TransportMsgType_DeleteAck` (type 19) case that extracts `[blob_hash:32][seq_num:8][status:1]` and matches against `pending_sends_`.

### Anti-Patterns to Avoid
- **Reusing the same identity for all scenarios:** Each scenario resets topology with `down -v`, so the identity from a previous scenario's write phase is useless. Each write-then-delete pair within a single scenario must share one identity, created fresh each time.
- **Relying on Notification for delete latency:** DeleteAck is the correct ACK for measuring delete throughput. Notifications arrive too, but they follow a different path (pub/sub fanout) and may have different timing characteristics.
- **Polling too frequently for GC:** The expiry scan runs every 60 seconds. Polling SIGUSR1 more often than every 2-3 seconds just wastes log output. Use the existing 2-second sleep in `wait_for_convergence`/`wait_for_count_decrease`.
- **Not outputting blob hashes in write mode:** The existing loadgen computes `blob_hash` via `wire::blob_hash(encoded)` but only uses the hex for notification matching. The hash hex strings must be collected and included in the JSON output as `blob_hashes: ["abcd...", ...]` for the delete phase to consume.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Tombstone data format | Manual byte construction | `wire::make_tombstone_data()` | Already exists, handles magic prefix + hash concatenation |
| Signing input | Manual SHA3 | `wire::build_signing_input()` | Already handles canonical form SHA3-256(ns\|\|data\|\|ttl\|\|ts) |
| Blob encoding | Manual FlatBuffer | `wire::encode_blob()` | Already handles deterministic FlatBuffer encoding |
| Content hash | Manual SHA3 | `wire::blob_hash()` | SHA3-256 of encoded FlatBuffer |
| Notification decoding | Manual parsing | Existing `handle_notification()` format | 77-byte format: [ns:32][hash:32][seq:8][size:4][tombstone:1] |
| Delete message format | Custom wire protocol | `TransportMsgType_Delete` (type 18) | Already defined in transport.fbs, server already handles it |

**Key insight:** The loadgen's delete mode is structurally identical to write mode, except: (1) it sends Delete instead of Data, (2) it constructs tombstone data instead of random data, (3) it matches DeleteAck instead of Notification for latency, and (4) it reads target hashes from input instead of generating random data.

## Common Pitfalls

### Pitfall 1: Blob Count vs Tombstone Count Confusion
**What goes wrong:** The SIGUSR1 metrics report `blobs=N` which is the sum of `latest_seq_num` across namespaces. This counts ALL stored entries including tombstones. After deleting 200 blobs and creating 200 tombstones, the count stays at 200 (200 blobs removed, 200 tombstones added) if stored data counts are based on seq_num.
**Why it happens:** `latest_seq_num` is a high-water mark, not a live count. Tombstones get their own seq_nums.
**How to avoid:** The blob count from SIGUSR1 (`blobs=N`) uses `latest_seq_num` which is monotonically increasing. After writing 200 blobs (seq 1-200) and then creating 200 tombstones (seq 201-400), `blobs=400`. For GC measurement, wait until count drops below a threshold after expiry scan purges both the original blobs AND tombstones.
**Warning signs:** Count never decreasing even after TTL expiry -- means you're reading seq_num (high-water) not actual blob count.

### Pitfall 2: Identity Mismatch Between Write and Delete Phases
**What goes wrong:** Delete fails with "namespace_mismatch" because the delete phase uses a different keypair than the write phase.
**Why it happens:** The loadgen generates a fresh ephemeral identity each time it runs. The delete phase must use the same identity that created the blobs.
**How to avoid:** Persist the identity between write and delete runs via `--identity-file`. Alternatively, use a deterministic seed.
**Warning signs:** All deletes rejected with "SHA3-256(pubkey) != namespace_id".

### Pitfall 3: GC Timing -- 60-Second Scan Interval
**What goes wrong:** Test times out because GC hasn't run yet.
**Why it happens:** The expiry scan runs every 60 seconds (hardcoded in `peer_manager.cpp` line 1412). With TTL=30 and worst-case timing: blob written at t=0, expires at t=30, next scan at t=60+up_to_60 = worst case 120 seconds from write. Plus sync propagation adds 10-20s for multi-hop.
**How to avoid:** Set generous timeouts for GC scenarios. With TTL=30 and 60s scan interval, allow at least 180 seconds timeout for worst-case. Per the CONTEXT decision, we use the default scan interval (no override).
**Warning signs:** Timeout errors in GC convergence wait.

### Pitfall 4: Blob Count Metric Is Not Live Count
**What goes wrong:** The `blobs=N` metric from SIGUSR1 uses `latest_seq_num` which is a high-water mark (monotonically increasing), not a count of blobs currently in storage. After GC purges blobs, `latest_seq_num` does NOT decrease.
**Why it happens:** Code at peer_manager.cpp:1931-1934 sums `ns.latest_seq_num` across namespaces.
**How to avoid:** Need a different metric for GC measurement. Options:
  1. Use `storage=X.XMiB` from the metrics line (tracks actual disk usage, decreases after GC)
  2. Add a `get_live_blob_count()` that counts actual entries in blobs_map
  3. Parse docker logs for "Expiry scan: purged N blobs" messages

**Recommendation:** Use the storage MiB metric from SIGUSR1 output to detect GC completion. When storage drops back to near-baseline, GC has completed. Parse `storage=(\d+\.\d+)MiB` from the metrics log line. This is more reliable than seq_num-based blob count.

**CRITICAL:** This is the most important finding. The existing `get_blob_count()` function parses `blobs=\K[0-9]+` from metrics output, which reads `latest_seq_num` -- a high-water mark that NEVER decreases. GC measurement CANNOT use this function. Must use storage size or a new actual-count metric.

### Pitfall 5: Tombstone Data Size is Always 36 Bytes
**What goes wrong:** Benchmark reports misleading throughput for "1 MiB tombstone" scenario when the tombstone itself is only 36 bytes regardless of the original blob size.
**Why it happens:** Tombstone data is always `4-byte magic + 32-byte target hash = 36 bytes`. The blob size parameter affects the WRITE phase (creating the blobs to be tombstoned) but NOT the DELETE phase (creating tombstones).
**How to avoid:** Clearly separate metrics: write throughput reflects blob size, delete throughput reflects tombstone creation overhead (which is primarily crypto cost, not data size). Report should note that tombstone creation is independent of original blob size.
**Warning signs:** All three tombstone size scenarios showing identical throughput (which is actually correct behavior, but should be documented).

## Code Examples

### Tombstone Construction in Loadgen (C++)
```cpp
// Source: db/wire/codec.h + db/engine/engine.cpp
// Given a target blob hash (from write phase), construct a tombstone BlobData:

#include "db/wire/codec.h"

chromatindb::wire::BlobData make_tombstone_request(
    const chromatindb::identity::NodeIdentity& id,
    std::span<const uint8_t, 32> target_hash,
    uint32_t ttl, uint64_t timestamp)
{
    chromatindb::wire::BlobData tombstone;
    std::memcpy(tombstone.namespace_id.data(), id.namespace_id().data(), 32);
    tombstone.pubkey.assign(id.public_key().begin(), id.public_key().end());
    tombstone.data = chromatindb::wire::make_tombstone_data(target_hash);
    tombstone.ttl = ttl;
    tombstone.timestamp = timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        tombstone.namespace_id, tombstone.data, tombstone.ttl, tombstone.timestamp);
    tombstone.signature = id.sign(signing_input);
    return tombstone;
}
```

### Sending Delete Message (C++)
```cpp
// Source: loadgen_main.cpp pattern + db/wire/transport_generated.h
// In delete mode, send via TransportMsgType_Delete instead of TransportMsgType_Data:

auto tombstone = make_tombstone_request(identity_, target_hash, ttl, now_us);
auto encoded = chromatindb::wire::encode_blob(tombstone);
auto hash = chromatindb::wire::blob_hash(std::span<const uint8_t>(encoded));

auto hash_hex = to_hex(hash);
pending_sends_[hash_hex] = scheduled_time;

auto send_ok = co_await conn_ptr->send_message(
    chromatindb::wire::TransportMsgType_Delete,  // type 18
    std::span<const uint8_t>(encoded));
```

### Handling DeleteAck (C++)
```cpp
// Source: db/peer/peer_manager.cpp:462-475
// DeleteAck format: [blob_hash:32][seq_num_be:8][status:1] = 41 bytes

void handle_delete_ack(std::vector<uint8_t> payload) {
    if (payload.size() != 41) {
        spdlog::warn("unexpected DeleteAck payload size: {}", payload.size());
        return;
    }
    std::array<uint8_t, 32> blob_hash{};
    std::memcpy(blob_hash.data(), payload.data(), 32);
    auto hash_hex = to_hex(blob_hash);

    auto it = pending_sends_.find(hash_hex);
    if (it == pending_sends_.end()) return;

    auto latency_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - it->second).count();
    latencies_.push_back(latency_ms);
    pending_sends_.erase(it);

    if (latencies_.size() == blobs_sent_ && blobs_sent_ > 0) {
        drain_timer_.cancel();
    }
}
```

### Shell: Tombstone Creation Scenario
```bash
# Source: deploy/run-benchmark.sh pattern
run_scenario_tombstone_creation() {
    local sizes=(1024 102400 1048576)
    local labels=("1k" "100k" "1m")

    for i in "${!sizes[@]}"; do
        local size="${sizes[$i]}"
        local label="${labels[$i]}"
        local identity_file="/tmp/bench-identity-${label}.bin"

        reset_topology
        capture_stats "pre" "tombstone-creation-${label}"

        # Phase 1: Write blobs (capture hashes)
        local write_output
        write_output=$(run_loadgen node1 --count "$BLOB_COUNT" --rate "$RATE" \
            --size "$size" --ttl 3600 --drain-timeout 10 \
            --identity-save "$identity_file")

        # Phase 2: Delete blobs (measure tombstone creation throughput)
        local hashes
        hashes=$(echo "$write_output" | jq -r '.blob_hashes | join("\n")')
        local delete_output
        delete_output=$(echo "$hashes" | run_loadgen node1 --delete \
            --hashes-from stdin --identity-file "$identity_file" --drain-timeout 10)

        capture_stats "post" "tombstone-creation-${label}"
        echo "$delete_output" > "$RESULTS_DIR/scenario-tombstone-creation-${label}.json"
    done
}
```

### Shell: GC Measurement with Storage Size Polling
```bash
# Custom function: poll SIGUSR1 for storage size decrease
wait_for_gc_completion() {
    local container="$1"
    local max_storage_mib="$2"   # threshold: GC complete when storage <= this
    local timeout_seconds="$3"
    local start_ns
    start_ns=$(date +%s%N)

    while true; do
        docker kill -s USR1 "$container" >/dev/null 2>&1 || true
        sleep 1
        local storage_mib
        storage_mib=$(docker logs "$container" 2>&1 | grep "metrics:" | tail -1 \
            | grep -oP 'storage=\K[0-9.]+' || echo "999")
        local now_ns
        now_ns=$(date +%s%N)
        local elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))

        log "GC wait $container: storage=${storage_mib}MiB (target<=${max_storage_mib}) (${elapsed_ms}ms)"

        local below
        below=$(echo "$storage_mib <= $max_storage_mib" | bc -l)
        if [[ "$below" == "1" ]]; then
            echo "$elapsed_ms"
            return 0
        fi

        if [[ $elapsed_ms -gt $(( timeout_seconds * 1000 )) ]]; then
            echo "$elapsed_ms"
            return 1
        fi

        sleep 2
    done
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| blob_count via seq_num high-water mark | Need storage_mib for GC measurement | This phase | SIGUSR1 `blobs=N` is NOT a live count -- it's monotonically increasing. Storage size (MiB) reflects actual disk usage. |
| Ephemeral identity per loadgen run | Identity persistence across write/delete | This phase | New `--identity-file` or `--identity-save` CLI flags needed |
| Notification-only ACK matching | DeleteAck (type 19) matching for deletes | This phase | Delete latency uses DeleteAck, not Notification |

**Key observations:**
- Tombstone creation throughput should be FASTER than blob write throughput because tombstone data is only 36 bytes vs 1 KiB-1 MiB for blobs. The crypto cost (ML-DSA-87 sign) is identical regardless of data size (hash-then-sign).
- GC convergence will be dominated by the 60-second expiry scan interval, not actual purge performance. The expiry scan itself is O(N expired blobs) in a single LMDB transaction.
- Tombstone sync propagation should be similar to small blob sync propagation since tombstones are small (36 bytes data).

## Open Questions

1. **Blob count metric reliability for GC**
   - What we know: `blobs=N` in SIGUSR1 output uses `latest_seq_num` sum, which is a high-water mark and never decreases
   - What's unclear: Whether there's an alternative metric that gives actual live blob count without code changes
   - Recommendation: Parse `storage=X.XMiB` from metrics output for GC convergence detection. This tracks actual disk usage and decreases after GC. If storage-based detection proves unreliable (e.g., LMDB doesn't immediately reclaim space), fall back to parsing "Expiry scan: purged N blobs" from docker logs.

2. **LMDB space reclamation after GC**
   - What we know: `run_expiry_scan()` erases entries from blobs_map and expiry_map in a write transaction. LMDB (mdbx) typically reclaims pages internally but may not shrink the file.
   - What's unclear: Whether `used_bytes()` (which reads `mi_geo.current`) decreases after erasure within the same mdbx environment.
   - Recommendation: Test empirically during implementation. If `used_bytes()` doesn't decrease, use docker logs "Expiry scan: purged N blobs" as the GC completion signal instead.

3. **Tombstone TTL for GC scenario**
   - What we know: The CONTEXT says "Blobs use TTL=30 seconds for the GC scenario." For the tombstones themselves, TTL can be 0 (permanent) or >0 (expirable).
   - What's unclear: Whether the GC scenario measures GC of the original blobs (with TTL=30), the tombstones (with their own TTL), or both.
   - Recommendation: Write blobs with TTL=30, create tombstones with TTL=30. Both will expire and be GC'd. This gives the most comprehensive GC measurement. The scenario title "Tombstone GC/Expiry" suggests measuring tombstone expiry specifically.

## Validation Architecture

> Note: `workflow.nyquist_validation` is not set in `.planning/config.json`, treating as enabled.

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Manual integration testing via Docker |
| Config file | deploy/run-benchmark.sh (orchestration script) |
| Quick run command | `bash deploy/run-benchmark.sh --skip-build --report-only` (report generation only) |
| Full suite command | `bash deploy/run-benchmark.sh` (full benchmark run) |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| BENCH-01 | Tombstone creation throughput measured | integration | `bash deploy/run-benchmark.sh` (runs all scenarios) | Scenario functions: Wave 0 |
| BENCH-02 | Tombstone sync propagation latency measured | integration | `bash deploy/run-benchmark.sh` | Scenario functions: Wave 0 |
| BENCH-03 | Tombstone GC/expiry performance measured | integration | `bash deploy/run-benchmark.sh` | Scenario functions: Wave 0 |

### Sampling Rate
- **Per task commit:** Build loadgen and verify `--delete --help` works
- **Per wave merge:** `bash deploy/run-benchmark.sh` (full benchmark run, ~10-15 min)
- **Phase gate:** Full benchmark suite green, REPORT.md contains all tombstone sections, benchmark-summary.json contains all tombstone scenario keys

### Wave 0 Gaps
- [ ] Loadgen `--delete` mode does not exist yet -- must be implemented
- [ ] Loadgen does not output `blob_hashes` in JSON stats -- must be added
- [ ] Loadgen does not support `--identity-file` / `--identity-save` -- must be added
- [ ] `run_scenario_tombstone_*()` functions do not exist in run-benchmark.sh
- [ ] `wait_for_count_decrease()` or storage-based GC polling does not exist
- [ ] Report generation does not include tombstone sections
- [ ] benchmark-summary.json does not include tombstone scenario keys

## Sources

### Primary (HIGH confidence)
- `loadgen/loadgen_main.cpp` -- current loadgen implementation, line-by-line analysis
- `deploy/run-benchmark.sh` -- current benchmark orchestration, all helper functions
- `db/wire/codec.h` -- tombstone data format (TOMBSTONE_MAGIC = 0xDEADBEEF, TOMBSTONE_DATA_SIZE = 36)
- `db/wire/transport_generated.h` -- TransportMsgType_Delete = 18, DeleteAck = 19
- `db/engine/engine.cpp` -- delete_blob() implementation, no TTL enforcement
- `db/peer/peer_manager.cpp` -- SIGUSR1 metrics (blobs= uses latest_seq_num), expiry scan (60s interval), DeleteAck wire format
- `db/storage/storage.cpp` -- run_expiry_scan() implementation, tombstone_map cleanup, expiry index handling

### Secondary (MEDIUM confidence)
- Expiry scan interval (60s) observed hardcoded at peer_manager.cpp:1412 -- not configurable

### Tertiary (LOW confidence)
- LMDB/mdbx space reclamation behavior after erase -- needs empirical validation during implementation

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, extending existing code
- Architecture: HIGH - all patterns derived from existing codebase analysis
- Pitfalls: HIGH - identified critical blob count metric issue through code analysis
- GC measurement: MEDIUM - storage size approach needs empirical validation

**Research date:** 2026-03-18
**Valid until:** 2026-04-18 (stable -- benchmark infrastructure, not fast-moving ecosystem)
