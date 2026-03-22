# Phase 39: Set Reconciliation (Custom Range-Based) - Research

**Researched:** 2026-03-19
**Domain:** Custom XOR-fingerprint range-based set reconciliation protocol
**Confidence:** HIGH

## Summary

Phase 39 replaces the current O(N) full hash list exchange in sync Phase B with a custom range-based set reconciliation protocol. The core algorithm is well-understood: both peers sort their 32-byte blob hashes for a namespace, exchange XOR fingerprints for ranges, and recursively split mismatched ranges until differences are isolated. Below a configurable split threshold (16 items), items are sent directly instead of recursing further.

The approach is a direct implementation of the range-based set reconciliation technique described by Aljoscha Meyer and used (with modifications) by the negentropy protocol in Nostr (NIP-77). The user has decided to build a custom implementation (~400-500 lines) rather than vendor negentropy, avoiding the SHA3-256 patching problem that made negentropy undesirable. XOR fingerprinting is safe in this codebase because blob hashes are SHA3-256 digests (uniformly distributed) and blob authorship requires ML-DSA-87 signature verification, preventing an attacker from injecting crafted XOR-canceling values.

**Primary recommendation:** Implement as a standalone `db/sync/reconciliation.h` / `reconciliation.cpp` module with pure functions for fingerprint computation, range splitting, and message encode/decode. Integrate into `run_sync_with_peer()` and `handle_sync_as_responder()` by replacing the Phase B hash list exchange section. Keep the module testable in isolation (no network dependencies in the core algorithm).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Custom XOR-fingerprint range-based set reconciliation (NOT negentropy)
- Both sides sort their 32-byte blob hashes. Exchange XOR fingerprints for ranges. If fingerprints match, range identical, skip. If not, split range in half, recurse
- XOR fingerprint: XOR of all hashes in a range. SHA3-256 blob hashes are uniformly distributed, XOR gives excellent collision resistance with zero hash function calls
- Split threshold: 16 items -- below this, send items directly instead of recursing (16 * 32 = 512 bytes vs one more round-trip)
- Initiator drives all reconciliation rounds (matches existing initiator-driven sync pattern)
- 3 new message types: ReconcileInit (27), ReconcileRanges (28), ReconcileItems (29)
- ReconcileInit: starts reconciliation for a namespace, first byte of payload is version byte (SYNC-09 forward compatibility)
- ReconcileRanges: range fingerprints exchanged back and forth during recursion
- ReconcileItems: below-threshold items sent directly (resolved leaf ranges)
- HashList (12) removed entirely from enum and all code -- clean break, pre-MVP no backward compat needed
- Remove all HashList encode/decode helpers and tests
- Replace Phase B only -- Phase A (namespace list exchange + cursor logic) and Phase C (blob transfer one-at-a-time) stay unchanged
- After reconciliation resolves missing hashes, stream diffs into Phase C blob requests (interleaving strategy at Claude's discretion)
- Output of reconciliation feeds into existing BlobRequest/BlobTransfer flow
- Cursor-hit namespaces still skip reconciliation entirely (unchanged)
- Cursor-miss triggers reconciliation on the FULL hash set for that namespace (not incremental since-seq)
- After successful reconciliation + blob transfer, store peer's advertised seq_num (unchanged cursor update logic)
- Full resync mechanism unchanged -- full resync = reconcile all namespaces regardless of cursor

### Claude's Discretion
- Internal module structure (single file vs separate reconciliation.h/.cpp)
- Exact message interleaving strategy for streaming diffs into Phase C
- How sorted hash vectors are built and cached during reconciliation
- Whether to offload XOR fingerprint computation to thread pool (likely unnecessary -- XOR is trivially fast)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SYNC-06 | Custom reconciliation module built with no external dependency (reinterpreted from "negentropy vendored") | Reconciliation module in db/sync/ with XOR fingerprint algorithm. No external deps -- pure C++20 with existing sorted hash vectors from Storage. |
| SYNC-07 | Per-namespace reconciliation replaces full hash list exchange (O(diff) not O(total_blobs)) | Range-based reconciliation algorithm: matching ranges skip, only mismatched ranges recurse. O(diff * log(N)) messages vs O(N) hash list. |
| SYNC-08 | Existing sync cursors coexist with reconciliation (unchanged namespaces skipped via cursor) | Cursor logic in Phase A unchanged. Cursor-hit skips reconciliation entirely. Cursor-miss triggers full-set reconciliation for that namespace. |
| SYNC-09 | Reconciliation wire messages include version byte for forward compatibility | ReconcileInit payload starts with version byte (0x01). ReconcileRanges/ReconcileItems can use implicit versioning from the ReconcileInit that started the session. |
</phase_requirements>

## Architecture Patterns

### Recommended Module Structure
```
db/
  sync/
    sync_protocol.h        # Existing -- remove encode/decode_hash_list, add reconciliation integration
    sync_protocol.cpp       # Existing -- remove hash_list methods
    reconciliation.h        # NEW: Reconciliation data types, encode/decode, XOR fingerprint logic
    reconciliation.cpp      # NEW: Implementation
  peer/
    peer_manager.h          # Existing -- no changes needed
    peer_manager.cpp        # Existing -- Phase B sections replaced with reconciliation calls
  schemas/
    transport.fbs           # Modified -- add new enum values, remove HashList
  wire/
    transport_generated.h   # Regenerated from transport.fbs
```

### Pattern 1: Range-Based Reconciliation Algorithm

**What:** Binary search on sorted arrays with XOR checksums. Both sides maintain sorted vectors of 32-byte blob hashes per namespace. The initiator starts reconciliation by sending a full-range fingerprint. If fingerprints match, the range is synchronized. If not, the range is split in half and both sub-range fingerprints are sent. Below the split threshold (16 items), items are sent directly.

**When to use:** Every namespace that needs syncing (cursor-miss or full-resync).

**Algorithm pseudocode:**
```
// Initiator starts: send ReconcileInit with full-range fingerprint
initiator_send(namespace_id, version=1, [{lower=0, upper=MAX, fingerprint=xor_all(our_hashes)}])

// Responder receives: for each range
for each range in received_ranges:
    our_fp = xor_fingerprint(our_hashes, range.lower, range.upper)
    if our_fp == range.fingerprint:
        skip  // range is identical
    else if count_in_range(our_hashes, range.lower, range.upper) <= SPLIT_THRESHOLD:
        send ReconcileItems(our items in this range)
    else:
        mid = midpoint(range.lower, range.upper)
        send ReconcileRanges([
            {lower=range.lower, upper=mid, fingerprint=xor_fp(our_hashes, range.lower, mid)},
            {lower=mid, upper=range.upper, fingerprint=xor_fp(our_hashes, mid, range.upper)}
        ])

// Initiator processes response: same logic, back and forth until all ranges resolved
// When both sides have exchanged items for all leaf ranges, reconciliation is complete
```

**Key implementation detail:** The XOR fingerprint of a range [lower, upper) is computed by XOR-ing all 32-byte blob hashes that fall within that sorted range. Since the vectors are sorted, this is a simple linear scan over a subrange.

**Range bounds:** Ranges are defined by indices into the sorted hash vector, not by hash values. The initiator sends the full range [0, N). Splitting is by midpoint index. This avoids encoding variable-length hash bounds in the wire format.

### Pattern 2: Initiator-Driven Multi-Round Exchange

**What:** The initiator drives all reconciliation rounds. After each ReconcileRanges/ReconcileItems response from the responder, the initiator processes it and either:
- Sends more ReconcileRanges for still-unresolved ranges
- Collects resolved diff hashes
- Sends a terminal empty ReconcileRanges to signal reconciliation complete

**When to use:** Every reconciliation session.

**Flow:**
```
Initiator                              Responder
    |--- ReconcileInit(ns, v1, fp) ---->|  Start namespace reconciliation
    |<-- ReconcileRanges(ranges) -------|  Responder splits mismatched ranges
    |--- ReconcileRanges(ranges) ------->|  Initiator refines (may include items)
    |<-- ReconcileItems(items) ---------|  Responder sends leaf items
    |    ... (multiple rounds) ...      |
    |--- ReconcileRanges(empty) -------->|  Initiator signals reconciliation done
    |                                    |
    |--- BlobRequest(missing_hashes) --->|  Phase C: request missing blobs
    |<-- BlobTransfer(blob) ------------|  Phase C: receive blobs one at a time
```

### Pattern 3: Wire Message Format

**ReconcileInit (type 27):**
```
[version: 1 byte]
[namespace_id: 32 bytes]
[count: u32 BE]             // our total item count for this namespace
[fingerprint: 32 bytes]     // XOR of all our hashes in this namespace
```

The count field allows the responder to know the initiator's set size, which combined with the fingerprint gives enough information to decide how to partition ranges.

**ReconcileRanges (type 28):**
```
[namespace_id: 32 bytes]
[range_count: u32 BE]
for each range:
    [lower_idx: u32 BE]     // start index in initiator's sorted array (inclusive)
    [upper_idx: u32 BE]     // end index in initiator's sorted array (exclusive)
    [count: u32 BE]         // sender's item count in this range
    [fingerprint: 32 bytes] // XOR fingerprint of sender's items in range
```

Wait -- index-based ranges won't work across peers because each peer has different items. The ranges need to be defined by hash-value bounds (the actual 32-byte hash that divides ranges), not indices. Let me correct this.

**Revised ReconcileRanges (type 28):**
```
[namespace_id: 32 bytes]
[range_count: u32 BE]
for each range:
    [upper_bound: 32 bytes]  // exclusive upper bound hash (0xFF...FF = end)
    [mode: 1 byte]           // 0=Skip, 1=Fingerprint, 2=ItemList
    if mode == 1 (Fingerprint):
        [count: u32 BE]      // sender's item count in this range
        [fingerprint: 32 bytes]
    if mode == 2 (ItemList):
        [count: u32 BE]
        [hash: 32 bytes] * count
```

Lower bound is implicit: it's the upper bound of the previous range, or 0x00...00 for the first range. This matches the negentropy wire encoding pattern and is more efficient.

**ReconcileItems (type 29):**
Actually, ReconcileItems can be folded into ReconcileRanges as mode=2 (ItemList). But per the CONTEXT.md decision, there are 3 distinct message types. Keep ReconcileItems separate:

```
[namespace_id: 32 bytes]
[count: u32 BE]
[hash: 32 bytes] * count     // resolved missing hashes the receiver needs to request
```

This carries the final diff output -- the list of hashes the other side has that we don't. These feed directly into Phase C BlobRequests.

### Pattern 4: Integration with Existing Sync Flow

**What:** The reconciliation replaces the Phase B section in both `run_sync_with_peer()` (initiator) and `handle_sync_as_responder()` (responder). Phase A (namespace list + cursor) and Phase C (BlobRequest/BlobTransfer) remain unchanged.

**Current Phase B code to replace (initiator, lines ~664-767):**
```cpp
// Currently: send all hash lists, then SyncComplete, then receive peer's hash lists
// REPLACE WITH: for each cursor-miss namespace, run reconciliation protocol
```

**Current Phase B code to replace (responder, lines ~961-1044):**
```cpp
// Currently: send all hash lists, then SyncComplete, then receive peer's hash lists
// REPLACE WITH: participate in reconciliation protocol driven by initiator
```

**Integration approach:**
1. After Phase A namespace exchange + cursor decisions, instead of both sides sending all hash lists and a SyncComplete:
2. For each namespace needing sync (cursor-miss), the initiator sends ReconcileInit and drives the multi-round reconciliation
3. Reconciliation produces a diff: hashes the local node is missing
4. These hashes feed directly into Phase C BlobRequest flow (existing one-blob-at-a-time pattern)
5. After all namespaces reconciled, exchange SyncComplete as before

### Anti-Patterns to Avoid

- **Persistent Merkle tree for fingerprints:** Write amplification on every blob ingest. Use in-memory sorted vectors built on demand from Storage::get_hashes_by_namespace(). The seq_map index scan is already efficient.
- **Hash-value midpoint splitting:** Don't split ranges by computing the midpoint hash value (e.g., averaging two 256-bit numbers). Split by count: take the item at position N/2 in the sorted array and use its hash as the bound. This ensures balanced splits regardless of hash distribution.
- **Sending items AND fingerprints in the same range:** A range is either a Fingerprint (needs more recursion) or an ItemList (resolved). Never both.
- **Building full sorted vectors for cursor-hit namespaces:** Cursor-hit namespaces skip reconciliation entirely. Don't even call collect_namespace_hashes() for them.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Sorted hash collection | Custom storage scan | `Storage::get_hashes_by_namespace()` | Already reads seq_map efficiently, returns sorted-by-seq-num vectors. Note: may need explicit sort by hash value for reconciliation. |
| Big-endian encoding | New encoding helpers | Existing `write_u32_be`/`read_u32_be` in sync_protocol.cpp | Already proven, tested. Copy pattern or extract to shared header. |
| Blob retrieval after diff | Custom blob fetch | Existing Phase C `BlobRequest`/`BlobTransfer` + `SyncProtocol::encode_single_blob_transfer` | Phase C is unchanged. Reconciliation just produces the diff hash list. |
| Message routing | Custom routing logic | Existing `on_peer_message` + `route_sync_message` pattern | Just add the 3 new message types (27, 28, 29) to the routing switch. |

**Key insight:** The reconciliation module should be a pure algorithm that takes sorted hash vectors and produces diff lists. All network I/O stays in PeerManager. All storage access stays through existing Storage/SyncProtocol APIs.

## Common Pitfalls

### Pitfall 1: Hash Vector Sort Order Mismatch
**What goes wrong:** `Storage::get_hashes_by_namespace()` returns hashes in seq_num order (insertion order), not lexicographic hash order. Range-based reconciliation requires both sides to agree on sort order.
**Why it happens:** The seq_map index is keyed by `[namespace:32][seq_num_be:8]`, so cursor iteration naturally produces seq-order hashes.
**How to avoid:** Explicitly sort the hash vector by hash value (lexicographic byte comparison) after collecting from storage. Both sides must use identical sort order for range bounds to be meaningful. Use `std::sort` with default `operator<` on `std::array<uint8_t, 32>` (lexicographic).
**Warning signs:** Reconciliation never converges, or converges to wrong diff.

### Pitfall 2: Off-by-One in Range Bounds
**What goes wrong:** Ranges use exclusive upper bounds. If upper bound logic is wrong, items at range boundaries get counted in the wrong range or double-counted.
**Why it happens:** The boundary hash is the hash of the item at the split point. Is that item in the lower or upper range?
**How to avoid:** Convention: lower bound is inclusive, upper bound is exclusive. The full range is [0x00...00, 0xFF...FF+1). Use sentinel values: MIN_HASH = all zeros, MAX_HASH = conceptual infinity (any hash compares less). In practice, the first message uses the full universe range. Splitting at item N/2: lower = [range.lower, items[N/2]), upper = [items[N/2], range.upper).
**Warning signs:** One extra or missing blob after reconciliation.

### Pitfall 3: Initiator/Responder Role Asymmetry
**What goes wrong:** Both sides try to split ranges independently, causing protocol desync.
**Why it happens:** The current Phase B is symmetric (both send hash lists). The new protocol is asymmetric (initiator drives).
**How to avoid:** Initiator always sends first (ReconcileInit), then ReconcileRanges. Responder only responds to received ranges, never initiates splits. Clear state machine: initiator sends, waits for response, processes, sends again or completes.
**Warning signs:** Deadlock (both sides waiting), or protocol hangs.

### Pitfall 4: Not Handling Empty Namespace on One Side
**What goes wrong:** One side has zero hashes for a namespace. The XOR fingerprint is 0x00...00 (identity element). If the other side also happens to have a fingerprint of 0x00...00 (possible for even-count sets with canceling hashes, though extremely unlikely with SHA3-256), reconciliation incorrectly declares the range identical.
**Why it happens:** XOR of empty set = identity. XOR is not a cryptographic MAC.
**How to avoid:** Include item count in the fingerprint comparison. Two ranges match only if BOTH the XOR fingerprint AND the item count are equal. This eliminates the empty-set ambiguity and the (astronomically unlikely) XOR collision case.
**Warning signs:** Namespace with items on one side and none on the other never syncs.

### Pitfall 5: Runaway Recursion on Large Diffs
**What goes wrong:** If two peers share very few blobs, nearly every range will mismatch, causing deep recursion with many rounds.
**Why it happens:** Range-based reconciliation is optimized for mostly-synchronized sets.
**How to avoid:** The split threshold of 16 items bounds recursion depth to O(log(N/16)). For 10M items, that's ~20 levels. Each level is one round-trip. This is acceptable but worth a max-rounds safety limit (e.g., 64 rounds). If exceeded, fall back to full item list exchange.
**Warning signs:** Reconciliation takes many round-trips for a new peer with no shared data.

### Pitfall 6: Stale Hash Vector During Multi-Round Reconciliation
**What goes wrong:** New blobs arrive during reconciliation, changing the local hash set. The sorted vector used for fingerprint computation becomes stale.
**Why it happens:** Reconciliation runs as a coroutine with co_await points (each message send/receive). Blob ingests from other peers or clients can happen between rounds.
**How to avoid:** Collect the sorted hash vector ONCE at the start of reconciliation for each namespace, and use that snapshot throughout all rounds. Do not re-read from storage mid-reconciliation. New blobs arriving during reconciliation will be caught in the next sync round.
**Warning signs:** Incorrect fingerprints that oscillate between rounds.

### Pitfall 7: Message Type Routing for New Types
**What goes wrong:** New message types (27, 28, 29) arrive but are not routed to sync_inbox, so recv_sync_msg() never sees them.
**Why it happens:** `on_peer_message()` has an explicit list of types that get routed (line 398-404). New types must be added to this list.
**How to avoid:** Add ReconcileInit, ReconcileRanges, ReconcileItems to the routing block alongside existing sync types. ReconcileInit should also trigger responder-side reconciliation (similar to how SyncRequest triggers handle_sync_as_responder).
**Warning signs:** Reconciliation messages silently dropped, timeouts.

## Code Examples

### XOR Fingerprint Computation
```cpp
// Pure function: XOR all 32-byte hashes in a sorted range [begin, end)
std::array<uint8_t, 32> xor_fingerprint(
    const std::vector<std::array<uint8_t, 32>>& sorted_hashes,
    size_t begin, size_t end) {
    std::array<uint8_t, 32> fp{};  // Identity element: all zeros
    for (size_t i = begin; i < end; ++i) {
        for (size_t b = 0; b < 32; ++b) {
            fp[b] ^= sorted_hashes[i][b];
        }
    }
    return fp;
}
```

### Range Splitting
```cpp
// Split a range at the midpoint. Returns the hash value at the split point.
// Lower range: [begin, mid), Upper range: [mid, end)
// The bound hash is sorted_hashes[mid] -- used as the upper bound of lower range.
size_t split_range(size_t begin, size_t end) {
    return begin + (end - begin) / 2;
}
```

### Sorted Hash Vector Preparation
```cpp
// Collect and sort hashes for reconciliation.
// Storage returns seq-order; we need lexicographic order.
auto hashes = storage_.get_hashes_by_namespace(namespace_id);
std::sort(hashes.begin(), hashes.end());
```

### Existing Big-Endian Helpers (reuse pattern from sync_protocol.cpp)
```cpp
// Already in sync_protocol.cpp anonymous namespace -- extract or duplicate
void write_u32_be(std::vector<uint8_t>& buf, uint32_t val);
uint32_t read_u32_be(const uint8_t* p);
```

### ReconcileInit Encode (example)
```cpp
static std::vector<uint8_t> encode_reconcile_init(
    std::span<const uint8_t, 32> namespace_id,
    uint32_t item_count,
    std::span<const uint8_t, 32> fingerprint) {
    std::vector<uint8_t> buf;
    buf.reserve(1 + 32 + 4 + 32);  // version + ns + count + fp
    buf.push_back(0x01);  // version byte (SYNC-09)
    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());
    write_u32_be(buf, item_count);
    buf.insert(buf.end(), fingerprint.begin(), fingerprint.end());
    return buf;
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Full hash list exchange (O(N)) | Range-based set reconciliation (O(diff * log N)) | Phase 39 | Eliminates 3.4M blob cliff, sync cost proportional to differences |
| negentropy (vendored header, SHA-256) | Custom XOR fingerprint module | Phase 39 discussion | Zero external deps, native SHA3-256 compatible, ~400-500 lines owned code |
| Symmetric Phase B (both send full lists) | Asymmetric initiator-driven reconciliation | Phase 39 | Cleaner protocol, matches existing initiator-driven sync pattern |

**Deprecated/outdated after this phase:**
- `HashList (12)` message type: Removed entirely from transport.fbs enum and all code
- `SyncProtocol::encode_hash_list()` / `decode_hash_list()`: Removed
- `SyncProtocol::diff_hashes()`: No longer needed (reconciliation computes diff internally)
- Both-sides-send-hash-lists-then-SyncComplete pattern in Phase B: Replaced with initiator-driven reconciliation rounds

## Open Questions

1. **Encoding efficiency for range bounds**
   - What we know: Ranges need hash-value bounds so both sides agree. Full 32-byte bounds per range is unambiguous.
   - What's unclear: Whether truncated/delta-encoded bounds (as in negentropy) are worth the complexity.
   - Recommendation: Start with full 32-byte bounds. At split threshold 16, max depth is ~20. Each round has at most ~2^depth ranges but in practice far fewer (mismatched ranges only). Overhead is manageable. Optimize later if needed.

2. **Phase B symmetry change impact**
   - What we know: Currently both initiator and responder send hash lists and receive hash lists. The new protocol is initiator-driven.
   - What's unclear: Whether the responder also needs to discover items the initiator has that it doesn't (bidirectional diff).
   - Recommendation: The reconciliation protocol naturally produces a bidirectional diff. When the responder sends its items in a range and the initiator sends its items in the same range, both sides learn what the other has. Each side can then issue BlobRequests for what they're missing. The reconciliation output is TWO diff lists: "hashes I need from you" and "hashes you need from me."

3. **SyncComplete timing**
   - What we know: Currently SyncComplete is sent after Phase B (hash list exchange). The new flow needs a signal that all namespace reconciliations are done.
   - What's unclear: Whether SyncComplete should be sent after all reconciliations but before Phase C, or after Phase C.
   - Recommendation: Keep SyncComplete semantics: sent after all blob transfers are done (end of Phase C), as currently. Reconciliation per-namespace terminates naturally (empty response = namespace done). A simple "all namespaces reconciled" transition to Phase C blob requests can be implicit.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (lines 195-236) |
| Quick run command | `cd build && ctest --test-dir . -R sync -j1 --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir . -j1 --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SYNC-06 | Custom reconciliation module: XOR fingerprint, range splitting, encode/decode | unit | `cd build && ctest -R sync -j1 --output-on-failure` | Needs new tests in test_sync_protocol.cpp or new test_reconciliation.cpp |
| SYNC-07 | Per-namespace reconciliation produces correct diff (O(diff)) | unit + integration | `cd build && ctest -R sync -j1 --output-on-failure` | Needs new tests |
| SYNC-08 | Cursor-hit skips reconciliation, cursor-miss triggers full reconciliation | unit | `cd build && ctest -R sync -j1 --output-on-failure` | Existing cursor tests in test_sync_protocol.cpp cover cursor logic; new tests needed for reconciliation integration |
| SYNC-09 | Version byte in ReconcileInit, forward-compatible decode | unit | `cd build && ctest -R sync -j1 --output-on-failure` | Needs new test |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . && ctest -R sync -j1 --output-on-failure`
- **Per wave merge:** `cd build && cmake --build . && ctest -j1 --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `db/tests/sync/test_reconciliation.cpp` (or extend test_sync_protocol.cpp) -- covers SYNC-06, SYNC-07, SYNC-09
- [ ] Tests for: XOR fingerprint correctness, range split correctness, encode/decode round-trip, full reconciliation simulation (two sorted hash vectors -> correct diff), version byte handling
- [ ] Tests for: empty namespace on one side, identical sets, completely disjoint sets, single-item difference
- [ ] Integration test: cursor-hit skips reconciliation (SYNC-08) -- may extend existing cursor tests

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/sync/sync_protocol.h`, `db/sync/sync_protocol.cpp`, `db/peer/peer_manager.cpp`, `db/schemas/transport.fbs`, `db/storage/storage.h` -- direct code reading
- [Range-Based Set Reconciliation (logperiodic.com)](https://logperiodic.com/rbsr.html) -- detailed algorithm description, fingerprint security analysis
- [AljoschaMeyer/set-reconciliation](https://github.com/AljoschaMeyer/set-reconciliation) -- original algorithm description with XOR fingerprint approach

### Secondary (MEDIUM confidence)
- [hoytech/negentropy](https://github.com/hoytech/negentropy) -- reference implementation showing wire format patterns (mode-based ranges, implicit lower bounds)
- [NIP-77 Negentropy Syncing](https://nips.nostr.com/77) -- production deployment of range-based reconciliation in Nostr

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new dependencies, all existing C++20 stack
- Architecture: HIGH -- algorithm is well-understood, codebase integration points clearly identified from code reading
- Pitfalls: HIGH -- identified from both algorithm literature (sort order, range bounds, empty sets) and codebase specifics (message routing, hash vector ordering, coroutine snapshots)

**Research date:** 2026-03-19
**Valid until:** 2026-04-19 (stable algorithm, no moving parts)
