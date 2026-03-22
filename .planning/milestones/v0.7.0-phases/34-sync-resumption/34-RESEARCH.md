# Phase 34: Sync Resumption - Research

**Researched:** 2026-03-17
**Domain:** Sync protocol optimization (cursor-based incremental sync)
**Confidence:** HIGH

## Summary

Phase 34 transforms sync cost from O(total_blobs) to O(new_blobs) by introducing per-peer per-namespace sequence cursors. The existing sync protocol already exchanges namespace lists with `latest_seq_num` per namespace (Phase A), making seq_num comparison the natural optimization point. Cursors are persisted in a new libmdbx sub-database and used to skip hash-list exchange for unchanged namespaces.

The implementation is entirely self-contained within the existing codebase. The storage layer already has `get_blobs_by_seq(ns, since_seq)` demonstrating the cursor-seek pattern in `seq_map`. A new `get_hashes_since_seq()` variant (hashes only, no blob data) provides the cursor-based hash retrieval. The sync orchestration in `PeerManager::run_sync_with_peer()` and `handle_sync_as_responder()` gains cursor lookup/update logic. No new wire message types are needed -- the existing NamespaceList/HashList/SyncComplete messages suffice with cursor logic layered on top.

**Primary recommendation:** Use the existing `seq_num` already exchanged in NamespaceList messages to drive cursor decisions. Store cursors in a sixth libmdbx sub-database with flat composite keys `[peer_pubkey_hash:32][namespace:32] -> [seq_num:8][round_count:4]`. The cursor decision is purely local -- if our stored cursor for a peer+namespace equals the remote's `latest_seq_num`, skip that namespace's hash exchange entirely.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Cursors keyed by SHA3-256(peer_pubkey) (32 bytes), not by address -- survives IP changes, port changes, reconnects
- Flat composite key in single libmdbx sub-database: `[peer_pubkey_hash:32][namespace:32] -> [seq_num:8][round_count:4]`
- Consistent with existing sub-db patterns (blobs, delegation, tombstone)
- On-demand reads from libmdbx during sync round (no in-memory cursor cache in PeerInfo)
- Round counter persisted alongside seq_num to survive restarts -- avoids unnecessary full resync after restart
- **Periodic (SYNC-04):** Every Nth round, configurable via `full_resync_interval` in config.json, default 10, SIGHUP-reloadable
- **SIGHUP config reload:** Reset all round counters to force next sync with each peer to be full. Do NOT wipe cursor seq_nums -- if full resync confirms they're still valid, no work wasted
- **Large time gap:** If time since last sync with a peer exceeds a threshold, force full resync. Claude's discretion on the threshold value
- **Cursor mismatch detection:** During cursor-based sync, if remote seq_num < our stored cursor for that namespace, reset that namespace's cursor only (per-namespace, not per-peer). Auto-recover via full diff for that namespace
- Startup cleanup scan: on node start, scan all cursor entries, compare peer_pubkey_hashes against connected + persisted peers (peers.json), delete entries for unknown peer hashes
- No GC-to-cursor coupling: tombstone GC and blob expiry do NOT trigger cursor adjustments. Cursors track seq_num, gaps from GC are handled by existing get_blobs_by_seq skip pattern. Periodic full resync catches edge cases
- Key rotation: old pubkey's cursors cleaned up by startup scan. New pubkey starts fresh (full sync on first round)
- Two-level logging: info-level summary line (extend existing sync log with cursor stats) + debug-level per-namespace lines
- Info line format includes: skipped-via-cursor count, full-diff count, round N/M
- NodeMetrics gets three new counters: `cursor_hits`, `cursor_misses`, `full_resyncs` -- visible in SIGUSR1 dump and periodic metrics
- Log levels: info for periodic full resync (expected), warn for event-triggered full resync (SIGHUP, time gap, mismatch)
- Startup cursor cleanup: info-level summary ("removed N entries for M unknown peers"), no per-entry detail

### Claude's Discretion
- Time gap threshold for full resync trigger (reasonable default, configurable)
- Wire protocol changes needed to support cursor-based sync (new message types vs extending existing)
- Exact cursor value encoding in libmdbx (endianness, packing)
- How cursor-based sync integrates into existing Phase A/B/C flow in run_sync_with_peer

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SYNC-01 | Per-peer per-namespace seq_num cursors tracked during sync rounds | Cursor sub-database with composite key `[peer_hash:32][ns:32]`; cursor read/write methods in Storage; cursor lookup in PeerManager sync orchestration |
| SYNC-02 | Hash-list exchange skipped for namespaces where remote seq_num equals cursor (unchanged since last sync) | NamespaceList already carries `latest_seq_num`; cursor comparison in Phase A determines skip vs full-diff; new `get_hashes_since_seq()` for partial hash lists |
| SYNC-03 | Sync cursors persisted across node restarts via dedicated libmdbx sub-database | Sixth sub-database `cursor_map` in Storage::Impl; round counter persisted alongside seq_num; startup cleanup scan |
| SYNC-04 | Periodic full hash-diff resync as fallback for cursor drift (configurable interval, default every 10th round) | Round counter in cursor value; `full_resync_interval` config field; SIGHUP resets round counters |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libmdbx | latest (FetchContent) | Cursor persistence sub-database | Already used for 5 sub-dbs; ACID, crash-safe |
| Standalone Asio | latest | Async sync orchestration | Already used for all networking/coroutines |
| spdlog | latest | Observability logging | Already used project-wide |
| nlohmann/json | latest | Config parsing for `full_resync_interval` | Already used for all config fields |

### Supporting
No new libraries needed. This phase is entirely within existing dependencies.

## Architecture Patterns

### Recommended Changes (by file)
```
db/
├── storage/
│   ├── storage.h          # + cursor_map handle, cursor CRUD methods
│   └── storage.cpp        # + cursor sub-db, cursor read/write/delete/scan
├── config/
│   ├── config.h           # + full_resync_interval, cursor_stale_seconds
│   └── config.cpp         # + JSON parsing for new fields
├── peer/
│   ├── peer_manager.h     # + cursor metrics in NodeMetrics
│   └── peer_manager.cpp   # + cursor logic in sync orchestration, SIGHUP extension
├── sync/
│   ├── sync_protocol.h    # + collect_namespace_hashes_since_seq()
│   └── sync_protocol.cpp  # + cursor-aware hash collection
└── tests/
    ├── storage/test_storage.cpp  # + cursor CRUD tests
    └── sync/test_sync_protocol.cpp  # + cursor-based sync tests
```

### Pattern 1: Cursor Storage (libmdbx Sub-Database)
**What:** A sixth sub-database `cursor_map` with flat composite keys
**When to use:** Every sync round reads/writes cursor data
**Key format:** `[peer_pubkey_hash:32][namespace_id:32]` = 64 bytes
**Value format:** `[seq_num_be:8][round_count_be:4][last_sync_ts_be:8]` = 20 bytes
**Encoding:** Big-endian for all integers, consistent with existing `encode_be_u64` / `make_seq_key` patterns in storage.cpp

```cpp
// Key construction (follows existing make_blob_key pattern)
static std::array<uint8_t, 64> make_cursor_key(
    const uint8_t* peer_hash, const uint8_t* ns) {
    std::array<uint8_t, 64> key;
    std::memcpy(key.data(), peer_hash, 32);
    std::memcpy(key.data() + 32, ns, 32);
    return key;
}

// Value layout: [seq_num:8][round_count:4][last_sync_ts:8]
struct CursorValue {
    uint64_t seq_num = 0;
    uint32_t round_count = 0;
    uint64_t last_sync_timestamp = 0;
};
```

**Rationale for including `last_sync_timestamp` in value:** The user decision requires a time-gap trigger for full resync. Storing the last sync timestamp alongside seq_num and round_count in the cursor value (20 bytes total) avoids a separate lookup. This is Claude's discretion for the encoding.

### Pattern 2: Cursor-Aware Sync Flow Integration
**What:** Layer cursor decisions on top of existing Phase A/B/C protocol
**When to use:** Every sync round in `run_sync_with_peer` and `handle_sync_as_responder`

The key insight is that **no wire protocol changes are needed**. The existing NamespaceList message already carries `latest_seq_num` per namespace. Cursor decisions are purely local:

```
Phase A (unchanged on wire):
  Both sides exchange NamespaceList with [namespace_id:32][seq_num:8]

Phase B (cursor-aware, local decision):
  For each namespace in peer's NamespaceList:
    1. Look up cursor: storage_.get_sync_cursor(peer_hash, ns_id)
    2. IF cursor.seq_num == peer_seq_num AND NOT full_resync_round:
       → SKIP: don't send HashList for this namespace (cursor hit)
    3. IF cursor.seq_num < peer_seq_num AND NOT full_resync_round:
       → PARTIAL: send only hashes since cursor.seq_num
    4. IF cursor.seq_num > peer_seq_num (mismatch):
       → RESET + FULL: reset cursor for this namespace, full hash diff
    5. IF full_resync_round:
       → FULL: send all hashes (existing behavior)

Phase C (unchanged):
  BlobRequest/BlobTransfer as before

Post-sync cursor update:
  For each namespace synced, update cursor to peer's latest_seq_num
  Increment round counter
```

**Critical detail:** Both sides of a sync (initiator and responder) execute the same cursor logic independently. Each node's cursor tracks what it knows about the other node's state.

### Pattern 3: Partial Hash Exchange
**What:** Send only hashes added since cursor's seq_num instead of all hashes
**When to use:** When cursor exists but peer has new blobs (cursor.seq_num < peer.seq_num)

This requires a new `get_hashes_since_seq()` method in Storage/SyncProtocol that returns hashes from `seq_map` with seq_num > cursor_seq_num. The implementation mirrors the existing `get_blobs_by_seq()` but reads only the 32-byte hash values from `seq_map` without touching `blobs_map`:

```cpp
// New method in Storage (mirrors get_blobs_by_seq but returns hashes only)
std::vector<std::array<uint8_t, 32>> get_hashes_since_seq(
    std::span<const uint8_t, 32> ns,
    uint64_t since_seq);
```

### Pattern 4: Full Resync Round Decision
**What:** Determine whether this sync round should be a full resync
**When to use:** At the start of every sync round, before Phase A

```
is_full_resync(peer_hash, round_count, last_sync_ts) {
    // 1. Periodic: every Nth round
    if (round_count % full_resync_interval == 0) return PERIODIC;

    // 2. Time gap: too long since last sync
    uint64_t now = clock_();
    if (now - last_sync_ts > cursor_stale_seconds) return TIME_GAP;

    // 3. SIGHUP forced: all round counters were reset to 0
    //    (handled implicitly: round_count=0 triggers periodic check)
    return NO;
}
```

**Time gap threshold recommendation:** 3600 seconds (1 hour) as default, configurable via `cursor_stale_seconds` in config.json. Rationale: sync_interval_seconds defaults to 60, so 1 hour = 60 missed rounds. If a peer disappears for over an hour, a full resync on reconnection is prudent.

### Pattern 5: Startup Cursor Cleanup
**What:** Scan and prune cursor entries for unknown peers on startup
**When to use:** During `PeerManager::start()`, after `load_persisted_peers()`

```
cleanup_stale_cursors():
    known_hashes = set()
    // Add all persisted peer pubkey hashes
    // (Note: peers.json stores addresses, not pubkeys.
    //  So we use cursor scan to identify peer_hashes and prune
    //  those NOT currently in connected peers or bootstrap list)

    // Actually: the startup scan cross-references cursor peer_hashes
    // against peers we expect to connect to. Since peers.json stores
    // addresses (not pubkeys), we can only prune cursors for
    // peer_hashes that we've NEVER seen. This is handled by deleting
    // cursor entries where we have no corresponding PersistedPeer
    // or bootstrap entry.
```

**Important nuance:** `peers.json` stores addresses, not public keys. The startup cleanup cannot directly cross-reference cursor peer_pubkey_hashes against persisted peers by address. Two approaches:

1. **Store peer_pubkey_hash alongside address in peers.json** (additional field)
2. **Skip startup cleanup entirely and rely on natural expiry** (simpler, accept some cursor bloat)
3. **Add a pubkey_hash field to peers.json** (minimal change, enables cleanup)

**Recommendation:** Add `pubkey_hash` field (hex string) to `PersistedPeer` in peers.json. This is a non-breaking addition (old files without the field default to empty, meaning "unknown" -- don't prune). The startup cleanup only prunes entries where the cursor's peer_hash doesn't match ANY known peer's pubkey_hash.

### Anti-Patterns to Avoid
- **In-memory cursor cache:** The user decision explicitly states on-demand reads from libmdbx. Don't cache cursors in PeerInfo -- libmdbx reads are fast (memory-mapped) and this avoids stale-cache bugs
- **Coupling cursors to GC/expiry:** Tombstone GC and blob expiry create seq_num gaps. Cursors must NOT be adjusted when blobs are deleted -- the periodic full resync catches any drift
- **Per-blob cursors:** Explicitly out of scope (REQUIREMENTS.md). Per-namespace seq_num is the right granularity
- **New wire message types:** Not needed. The existing NamespaceList already carries seq_num. Cursor decisions are local, not negotiated
- **Resetting cursors on disconnect:** Cursors persist across disconnections/reconnections (keyed by pubkey_hash, not connection). Only reset on explicit triggers (mismatch, time gap, SIGHUP)

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Cursor persistence | Custom file format | libmdbx sub-database | ACID transactions, crash-safe, consistent with existing 5 sub-dbs |
| Peer identity hashing | Custom hash | `crypto::sha3_256(peer_pubkey)` | Already used for namespace derivation, consistent |
| Big-endian encoding | New helpers | Existing `encode_be_u64` / `decode_be_u64` in storage.cpp | Already proven correct, same patterns |
| Config parsing | Manual JSON | `j.value("full_resync_interval", default)` | Existing pattern in config.cpp, handles missing keys |

## Common Pitfalls

### Pitfall 1: Cursor Update Before Sync Completion
**What goes wrong:** Updating the cursor seq_num before all blobs are successfully transferred. If sync fails mid-transfer, the cursor points past blobs that weren't received.
**Why it happens:** Natural impulse to update cursor eagerly.
**How to avoid:** Update cursor ONLY after Phase C completes successfully. If sync fails at any point (timeout, disconnect, error), leave cursor at its previous value. The next sync round will pick up where it left off.
**Warning signs:** Missing blobs after restart, cursor seq_num higher than actual stored blobs.

### Pitfall 2: Both Sides Skipping Same Namespace
**What goes wrong:** If both initiator and responder independently decide to skip a namespace (because both see no change from their own perspective), they might miss blobs that were added between sync rounds on the other side.
**Why it happens:** Misunderstanding cursor semantics. Each side's cursor tracks the OTHER peer's last-known seq_num.
**How to avoid:** The cursor stores what we last saw the PEER's seq_num as. If the peer's NamespaceList shows the same seq_num as our cursor for that peer+namespace, we know the peer has no new blobs for us. Our own new blobs are handled by the peer's cursor logic on their side.
**Warning signs:** Blobs not propagating despite sync rounds completing.

### Pitfall 3: max_maps Limit in libmdbx
**What goes wrong:** Adding a 6th sub-database but `operate_params.max_maps = 6` is already at `5 sub-databases + 1 spare`.
**Why it happens:** The current max_maps = 6 was set for 5 sub-dbs + 1 spare.
**How to avoid:** Increase `max_maps` to 7 (6 sub-dbs + 1 spare) when adding the cursor sub-database. This is a one-line change in Storage::Impl constructor.
**Warning signs:** "MDBX_DBS_FULL" error on startup.

### Pitfall 4: Stale Iterator After co_await in Cursor-Aware Sync
**What goes wrong:** Reading cursor data, then co_await, then using the cursor data -- but the cursor may have been updated by another coroutine (expiry scan, incoming sync).
**Why it happens:** co_await is a suspension point; other coroutines can run.
**How to avoid:** Single io_context thread means no true concurrency, but `peer->syncing = true` flag already prevents concurrent sync with the same peer. Cursor reads are point-in-time snapshots from libmdbx, which is safe. Just don't hold libmdbx read transactions across co_await points.
**Warning signs:** Assertion failures, unexpected cursor values.

### Pitfall 5: SIGHUP Round Counter Reset vs Cursor Wipe
**What goes wrong:** Wiping seq_num cursors on SIGHUP instead of just resetting round counters.
**Why it happens:** Over-aggressive reset logic.
**How to avoid:** SIGHUP resets round_count to 0 for all cursors (triggering periodic resync on next round). It does NOT change seq_num values. If the full resync confirms the cursor seq_nums are still valid, no redundant work.
**Warning signs:** Unnecessary full data transfer after every SIGHUP.

### Pitfall 6: Cursor Mismatch with Partial Hash Exchange
**What goes wrong:** During partial hash exchange, sending hashes since cursor seq_num, but the peer has GC'd or expired blobs that created seq_num gaps. The peer's current seq_num is lower than expected.
**Why it happens:** Seq_num gaps from blob expiry/deletion.
**How to avoid:** The mismatch detection rule handles this: if remote seq_num < our stored cursor, reset that namespace's cursor and do a full hash diff for that namespace only. This is per-namespace, not per-peer.
**Warning signs:** Sync rounds that should be incremental but always fall back to full diff.

## Code Examples

### Cursor CRUD in Storage
```cpp
// storage.h additions (public API)
struct SyncCursor {
    uint64_t seq_num = 0;
    uint32_t round_count = 0;
    uint64_t last_sync_timestamp = 0;
};

std::optional<SyncCursor> get_sync_cursor(
    std::span<const uint8_t, 32> peer_hash,
    std::span<const uint8_t, 32> namespace_id);

void set_sync_cursor(
    std::span<const uint8_t, 32> peer_hash,
    std::span<const uint8_t, 32> namespace_id,
    const SyncCursor& cursor);

void delete_sync_cursor(
    std::span<const uint8_t, 32> peer_hash,
    std::span<const uint8_t, 32> namespace_id);

/// Delete all cursors for a given peer.
size_t delete_peer_cursors(std::span<const uint8_t, 32> peer_hash);

/// Reset round counters for all cursors to 0 (SIGHUP).
size_t reset_all_round_counters();

/// Scan all cursor entries, return unique peer hashes.
std::vector<std::array<uint8_t, 32>> list_cursor_peers();
```

### Cursor Value Encoding (Big-Endian, Consistent with Existing Patterns)
```cpp
// Value layout: [seq_num:8][round_count:4][last_sync_ts:8] = 20 bytes
static constexpr size_t CURSOR_VALUE_SIZE = 20;

static std::array<uint8_t, CURSOR_VALUE_SIZE> encode_cursor_value(
    const SyncCursor& cursor) {
    std::array<uint8_t, CURSOR_VALUE_SIZE> val;
    encode_be_u64(cursor.seq_num, val.data());
    // round_count as 4-byte big-endian
    val[8]  = static_cast<uint8_t>((cursor.round_count >> 24) & 0xFF);
    val[9]  = static_cast<uint8_t>((cursor.round_count >> 16) & 0xFF);
    val[10] = static_cast<uint8_t>((cursor.round_count >> 8) & 0xFF);
    val[11] = static_cast<uint8_t>(cursor.round_count & 0xFF);
    encode_be_u64(cursor.last_sync_timestamp, val.data() + 12);
    return val;
}
```

### Hashes Since Seq (New Storage Method)
```cpp
// Mirrors get_blobs_by_seq but returns only 32-byte hashes from seq_map
std::vector<std::array<uint8_t, 32>> Storage::get_hashes_since_seq(
    std::span<const uint8_t, 32> ns,
    uint64_t since_seq) {
    std::vector<std::array<uint8_t, 32>> hashes;
    try {
        auto txn = impl_->env.start_read();
        auto cursor = txn.open_cursor(impl_->seq_map);
        auto lower_key = make_seq_key(ns.data(), since_seq + 1);
        auto seek_result = cursor.lower_bound(to_slice(lower_key));
        if (!seek_result.done) return hashes;
        do {
            auto key_data = cursor.current(false).key;
            if (key_data.length() != 40) break;
            if (std::memcmp(key_data.data(), ns.data(), 32) != 0) break;
            auto val_data = cursor.current(false).value;
            if (val_data.length() == 32) {
                std::array<uint8_t, 32> hash;
                std::memcpy(hash.data(), val_data.data(), 32);
                hashes.push_back(hash);
            }
            auto next = cursor.to_next(false);
            if (!next.done) break;
        } while (true);
    } catch (const std::exception& e) {
        spdlog::error("Storage error in get_hashes_since_seq: {}", e.what());
    }
    return hashes;
}
```

### Config Extension
```cpp
// config.h additions
struct Config {
    // ... existing fields ...
    uint32_t full_resync_interval = 10;     // Full resync every Nth round (SYNC-04)
    uint64_t cursor_stale_seconds = 3600;   // Force full resync after this gap (1 hour)
};

// config.cpp additions (inside load_config)
cfg.full_resync_interval = j.value("full_resync_interval", cfg.full_resync_interval);
cfg.cursor_stale_seconds = j.value("cursor_stale_seconds", cfg.cursor_stale_seconds);
```

### Cursor-Aware Sync Orchestration (Pseudocode)
```cpp
// In run_sync_with_peer, after Phase A NamespaceList exchange:

auto peer_hash = crypto::sha3_256(conn->peer_pubkey());

// Determine if this is a full resync round
bool is_full_round = false;
// Check any cursor for this peer to get round_count
// (All cursors for same peer have same round_count)
auto cursor_peers = ... ; // check first namespace cursor

for (const auto& ns_info : peer_namespaces) {
    auto cursor = storage_.get_sync_cursor(peer_hash, ns_info.namespace_id);

    if (!is_full_round && cursor.has_value()) {
        if (cursor->seq_num == ns_info.latest_seq_num) {
            // CURSOR HIT: skip this namespace entirely
            ++cursor_hits;
            continue;
        }
        if (ns_info.latest_seq_num < cursor->seq_num) {
            // MISMATCH: remote went backwards, reset + full diff
            // (per-namespace reset, not per-peer)
            ++cursor_misses;
            // Fall through to full hash diff
        } else {
            // PARTIAL: peer has new blobs since cursor
            auto partial_hashes = storage_.get_hashes_since_seq(
                ns_info.namespace_id, cursor->seq_num);
            // Use partial hashes for this namespace's hash exchange
            ++cursor_misses;
        }
    } else {
        // No cursor or full resync round: full hash diff (existing behavior)
        ++cursor_misses;
    }
}

// After successful sync, update cursors:
for (const auto& ns_info : peer_namespaces) {
    SyncCursor updated;
    updated.seq_num = ns_info.latest_seq_num;
    updated.round_count = old_round_count + 1;
    updated.last_sync_timestamp = clock_();
    storage_.set_sync_cursor(peer_hash, ns_info.namespace_id, updated);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Full hash-list exchange every sync | Cursor-based incremental | Phase 34 | O(total_blobs) -> O(new_blobs) per round |
| No sync state across restarts | Persisted cursors in libmdbx | Phase 34 | No full resync needed after clean restart |
| No sync observability | cursor_hits/misses/full_resyncs metrics | Phase 34 | Visible in SIGUSR1 dump and periodic log |

## Open Questions

1. **Partial hash exchange on the responder side**
   - What we know: The initiator can decide locally whether to send partial or full hash lists. But the responder also sends hash lists -- should the responder also use cursor logic?
   - What's unclear: The sync protocol is symmetric (both sides send NamespaceList + HashList). Both sides should independently use cursor logic.
   - Recommendation: Yes, both `run_sync_with_peer` and `handle_sync_as_responder` should apply cursor logic. Each side independently decides whether to send full or partial hash lists based on its own cursor state for the other peer.

2. **Peer hash derivation from pubkey in responder context**
   - What we know: `conn->peer_pubkey()` is available after authentication. `crypto::sha3_256(conn->peer_pubkey())` gives the peer's namespace_id, which doubles as the cursor key.
   - What's unclear: Nothing -- this is straightforward.
   - Recommendation: Derive peer_hash at the start of sync coroutine, use throughout.

3. **Interaction between cursor-based partial hash exchange and the diff algorithm**
   - What we know: If we send partial hashes (only hashes since cursor seq_num), the peer computes diff against our partial set. This is correct only if we also know the peer has our older hashes.
   - What's unclear: In the current protocol, BOTH sides send ALL their hashes, and each side diffs. With cursors, if we send only NEW hashes, the peer will see them as "theirs minus ours" which works for requesting new blobs FROM us. But the peer's diff of "their full list minus our partial list" would incorrectly think we're missing old blobs.
   - Recommendation: Cursor skipping should be at the NAMESPACE level, not partial-hash-within-namespace level. For a namespace: either skip entirely (cursor hit -- no hash exchange at all), or do full hash exchange (cursor miss). The `get_hashes_since_seq` method is useful for optimization in a later iteration, but for this phase, keep it simple: skip entire namespaces or full-diff entire namespaces. This avoids the partial-diff correctness issue entirely.

   **REVISED APPROACH:** Skip entire namespaces via cursor (SYNC-02), or fall back to full hash diff for that namespace. Do NOT attempt within-namespace partial hash exchange in this phase. This keeps the existing diff algorithm correct and unchanged.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 (latest via FetchContent) |
| Config file | db/CMakeLists.txt (BUILD_TESTING block) |
| Quick run command | `cd build && ctest -R "sync\|storage" --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SYNC-01 | Cursor CRUD: get/set/delete per peer+namespace | unit | `cd build && ctest -R test_storage --output-on-failure` | Needs new tests |
| SYNC-01 | Cursor lookup during sync | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | Needs new tests |
| SYNC-02 | Namespace skipped when cursor matches seq_num | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | Needs new tests |
| SYNC-02 | No hash exchange for unchanged namespaces | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | Needs new tests |
| SYNC-03 | Cursors survive restart (persist in libmdbx) | unit | `cd build && ctest -R test_storage --output-on-failure` | Needs new tests |
| SYNC-03 | Round counter persisted and survives restart | unit | `cd build && ctest -R test_storage --output-on-failure` | Needs new tests |
| SYNC-04 | Full resync triggered on Nth round | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | Needs new tests |
| SYNC-04 | Full resync after SIGHUP resets round counters | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | Needs new tests |
| SYNC-04 | Full resync on time gap exceeding threshold | unit | `cd build && ctest -R test_sync_protocol --output-on-failure` | Needs new tests |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R "sync\|storage" --output-on-failure`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] New test cases in `db/tests/storage/test_storage.cpp` -- cursor CRUD, persistence, cleanup scan
- [ ] New test cases in `db/tests/sync/test_sync_protocol.cpp` -- cursor-based sync, skip logic, full resync triggers
- [ ] `get_hashes_since_seq` method tests in test_storage.cpp

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/storage/storage.cpp` -- existing libmdbx sub-database patterns, key construction, big-endian encoding
- Codebase analysis: `db/peer/peer_manager.cpp` -- sync orchestration (run_sync_with_peer, handle_sync_as_responder), SIGHUP handler, metrics
- Codebase analysis: `db/sync/sync_protocol.cpp` -- hash collection, diff algorithm, message encoding
- Codebase analysis: `db/config/config.cpp` -- config parsing patterns, JSON field defaults
- Codebase analysis: `db/schemas/transport.fbs` -- existing message types (26 types, no new ones needed)

### Secondary (MEDIUM confidence)
- CONTEXT.md user decisions -- all design choices locked and verified against codebase feasibility

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all within existing libs
- Architecture: HIGH -- patterns directly mirror existing sub-db and sync code
- Pitfalls: HIGH -- derived from actual codebase analysis of co_await patterns, libmdbx config, and sync flow
- Open question on partial hash exchange: RESOLVED -- simplified to namespace-level skip/full-diff only

**Research date:** 2026-03-17
**Valid until:** 2026-04-17 (stable domain, no external dependencies changing)
