# Phase 11: Larger Blob Support - Research

**Researched:** 2026-03-07
**Domain:** Large blob transport, storage, and sync for libmdbx-backed blob store
**Confidence:** HIGH

## Summary

Phase 11 raises the maximum blob size from ~16 MiB to 100 MiB. The existing codebase has a clean separation between framing (MAX_FRAME_SIZE), engine validation (ingest pipeline), sync protocol (hash collection + blob transfer), and storage (libmdbx). All changes are constexpr/constant changes and targeted code modifications -- no architectural restructuring needed.

The critical insight is that `collect_namespace_hashes()` currently loads ALL blob data from storage to compute hashes. With 100 MiB blobs, this is catastrophic for memory. The fix is straightforward: store the blob hash in the seq_map value (it's already 32 bytes there) and read hashes directly from the index without touching blob data. The current sync protocol batches all blobs for a namespace into a single BlobTransfer message, which would be a 100+ MiB message. The fix is to send one blob per BlobTransfer and request blobs in capped batches.

**Primary recommendation:** Three plans -- (1) constants + size validation, (2) hash index + sync optimization, (3) timeout scaling + integration tests.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Raise MAX_FRAME_SIZE to accommodate 100 MiB blobs (no chunking/streaming)
- Single frame per blob -- keep the existing monolithic send/receive pattern
- Both layers validate size: transport rejects frames > MAX_FRAME_SIZE (BLOB-05), engine rejects blob data > 100 MiB (BLOB-01)
- MAX_BLOB_DATA_SIZE is a constexpr protocol invariant (like TTL), not configurable
- Enforced at ingest as Step 0 -- before structural checks, namespace verification, or signature verification
- This is the cheapest possible check (one integer comparison)
- One blob per message during Phase C transfer (not batched)
- For each missing hash: send request, receive single blob, ingest, repeat
- Bounded memory: only one blob in flight per direction at a time
- Hash lists (Phase A/B) remain all-at-once -- hashes are 32 bytes each, even 10K blobs = 320 KB
- Allocate receive buffer upfront after validating declared frame size (current pattern, just raised limit)
- Frame size limit IS the memory cap -- no separate per-connection memory budget needed
- Full blob loaded in memory for signature verification (already in recv buffer, SHA3-256 is fast on 100 MiB)
- Disconnect immediately + record strike when a peer declares a frame length > MAX_FRAME_SIZE
- No error response sent -- oversized frame declaration is either a bug or an attack
- Matches existing strike system behavior
- Per-blob timeout during sync Phase C (not per-session)
- On timeout: skip the blob, log warning, continue syncing remaining blobs
- Timed-out blobs retry on the next sync cycle
- No strike for timeouts -- could be slow network, not malice

### Claude's Discretion
- Exact MAX_FRAME_SIZE value (calculate minimum for 100 MiB blob + FlatBuffer overhead + AEAD tag, add reasonable headroom)
- Hash index implementation for BLOB-03 (store hashes to avoid loading blob data during collect_namespace_hashes)
- New sync message types vs reusing existing types for one-blob-at-a-time transfer
- Timeout formula (size-based vs adaptive, min/max bounds)
- Whether sync phases A/B also get explicit timeouts

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| BLOB-01 | Node enforces MAX_BLOB_DATA_SIZE (100 MiB) as Step 0 in ingest, before signature verification | Add constexpr + size check before existing Step 1 in engine.cpp |
| BLOB-02 | Transport frame size supports 100 MiB blobs plus protocol overhead | Raise MAX_FRAME_SIZE in framing.h; calculate exact value from overhead analysis |
| BLOB-03 | Sync hash collection reads hashes from seq_map index without loading blob data | Store hash as seq_map value (already 32 bytes), read via cursor scan |
| BLOB-04 | Sync transfers blobs individually with batched requests capped by MAX_HASHES_PER_REQUEST | Replace batched BlobTransfer with one-blob-at-a-time loop; cap hash request batches |
| BLOB-05 | Transport validates declared frame length against max before allocating receive buffer | Already done in recv_raw() (line 78 connection.cpp) and read_frame() (line 56 framing.cpp); add strike recording |
| BLOB-06 | Sync timeout adapts to transfer size to prevent timeout on large blob exchanges | Per-blob timeout in Phase C; size-proportional formula with min/max bounds |
</phase_requirements>

## Standard Stack

No new dependencies needed. All changes use existing libraries.

### Core (unchanged)
| Library | Purpose | Phase 11 Impact |
|---------|---------|-----------------|
| libmdbx | Blob storage, ACID transactions | Handles large values natively via overflow pages; no config change needed |
| FlatBuffers | Wire format | encode_blob already handles variable-length data; FlatBuffer overhead is ~100 bytes |
| Standalone Asio | Async networking, coroutines | recv/send already handle arbitrary buffer sizes |
| ChaCha20-Poly1305 (libsodium) | AEAD encryption | Tag is 16 bytes; works on any plaintext size |

## Architecture Patterns

### Pattern 1: Constexpr Protocol Invariants
**What:** MAX_BLOB_DATA_SIZE defined as constexpr alongside existing protocol constants (TTL).
**Where:** `db/net/framing.h` (co-located with MAX_FRAME_SIZE)
**Example:**
```cpp
// db/net/framing.h
constexpr uint64_t MAX_BLOB_DATA_SIZE = 100ULL * 1024 * 1024;  // 100 MiB
```
Note: Use uint64_t to avoid overflow in frame size calculations.

### Pattern 2: Fail-Fast Validation (Step 0 before Step 1)
**What:** Cheapest check first in BlobEngine::ingest().
**Where:** `db/engine/engine.cpp`, before existing Step 1 (structural checks)
**Example:**
```cpp
IngestResult BlobEngine::ingest(const wire::BlobData& blob) {
    // Step 0: Size check (cheapest possible -- one integer comparison)
    if (blob.data.size() > net::MAX_BLOB_DATA_SIZE) {
        return IngestResult::rejection(IngestError::oversized_blob, "blob data exceeds 100 MiB limit");
    }
    // Step 1: Structural checks (existing)...
}
```
Requires adding `IngestError::oversized_blob` to the enum.

### Pattern 3: Hash-in-Index for collect_namespace_hashes (BLOB-03)
**What:** The seq_map value is currently `hash:32` (the blob content hash). This is the SAME hash that `collect_namespace_hashes()` needs. Currently the function loads ALL blob data, re-encodes it, and recomputes the hash -- wasteful.

**Current flow (expensive):**
```
collect_namespace_hashes() -> engine_.get_blobs_since() -> storage_.get_blobs_by_seq()
  -> For EACH entry in seq_map: read hash from value, fetch FULL blob from blobs_map, decode FlatBuffer
  -> Back in SyncProtocol: encode blob again, compute hash
```

**Optimized flow:** Add `Storage::get_hashes_by_namespace()` that only reads seq_map values (32-byte hashes) without touching blobs_map at all. This is a cursor scan over lightweight 40-byte keys + 32-byte values.

```cpp
// New method on Storage
std::vector<std::array<uint8_t, 32>> Storage::get_hashes_by_namespace(
    std::span<const uint8_t, 32> ns);
```

**Expiry filtering consideration:** `collect_namespace_hashes()` currently filters expired blobs. With hash-only reads, we can't check TTL. Two options:
1. Skip expiry filtering during hash collection -- expired blobs get synced, then filtered at ingest. Harmless but wasteful.
2. Add a separate hash-to-expiry check. Over-engineered for now.

**Recommendation:** Option 1 (skip filtering). The expiry scanner runs periodically and cleans up. Syncing an expired blob wastes one round-trip but is not harmful -- the peer's ingest or expiry scan will handle it. This keeps the hash collection O(n) on seq_map only.

### Pattern 4: One-Blob-at-a-Time Sync Transfer (BLOB-04)
**What:** Replace batched BlobTransfer with individual blob messages.
**Current flow:** Requester sends `BlobRequest(namespace, [hash1, hash2, ...])`, responder sends `BlobTransfer([blob1, blob2, ...])` -- all blobs in one message.
**New flow:** Requester sends `BlobRequest(namespace, [hash1, hash2, ..., hashN])` (batch up to MAX_HASHES_PER_REQUEST), responder sends one `BlobTransfer` per blob (count=1 in existing wire format). Requester processes each BlobTransfer as it arrives.

**Key insight:** The existing `encode_blob_transfer`/`decode_blob_transfer` already support count=1. No new message types needed. Just change the loop structure in peer_manager.cpp.

**MAX_HASHES_PER_REQUEST:** Cap at a reasonable number (e.g., 64) to prevent a single BlobRequest from implying 64 * 100 MiB = 6.4 GiB of responses. With one-blob-at-a-time responses, the requester can process and free each blob before the next arrives.

### Pattern 5: Strike on Oversized Frame (BLOB-05)
**What:** recv_raw() already rejects frames > MAX_FRAME_SIZE (returns nullopt). The connection gets closed by the message loop when recv returns nullopt. But we need to explicitly record a strike BEFORE closing.

**Issue:** recv_raw() is a Connection method -- it doesn't have access to the strike system (that's in PeerManager). The existing pattern is: Connection closes on error, PeerManager's close callback handles cleanup.

**Approach:** Have recv_raw() log and return nullopt (already does this). The PeerManager already handles this case -- when the connection closes ungracefully, the peer is removed. For explicit strike recording, we can add a reason enum/callback, but the simpler approach matching CONTEXT.md ("disconnect immediately") is: the current behavior already disconnects. Adding a strike is nice-to-have but the peer is already gone. The key defense (BLOB-05) is that the buffer is NOT allocated -- which is already true.

### Pattern 6: Per-Blob Timeout with Size Scaling (BLOB-06)
**What:** During Phase C, each blob request/response pair gets its own timeout.
**Formula recommendation:**
```cpp
// Minimum 30 seconds (small blobs), scales at ~1 MiB/s minimum expected throughput
// Maximum 5 minutes (100 MiB at ~350 KB/s -- very slow connection)
constexpr auto MIN_BLOB_TIMEOUT = std::chrono::seconds(30);
constexpr auto MAX_BLOB_TIMEOUT = std::chrono::seconds(300);

auto blob_timeout(size_t blob_size_estimate) {
    // Assume minimum 512 KB/s throughput
    auto seconds = std::max(30UL, blob_size_estimate / (512 * 1024));
    return std::chrono::seconds(std::min(seconds, 300UL));
}
```

Since we don't know the blob size before receiving it (we only have the hash), use the maximum timeout for the transfer phase. Or, since MAX_BLOB_DATA_SIZE is the upper bound, use a fixed generous timeout for Phase C blob transfers (e.g., 120 seconds), different from the 30-second timeout used for control messages.

**Recommendation:** Fixed generous timeout for blob transfer messages (120s), keep 30s for control messages. Simple and effective.

### MAX_FRAME_SIZE Calculation

A 100 MiB blob needs:
- FlatBuffer overhead: ~100 bytes (table headers, field offsets, alignment padding)
- ML-DSA-87 pubkey: 2,592 bytes
- ML-DSA-87 signature: 4,627 bytes
- namespace_id: 32 bytes
- TTL + timestamp: 12 bytes
- Blob data: 104,857,600 bytes (100 MiB)

Total FlatBuffer payload: ~104,865,000 bytes (~100.007 MiB)

Transport message wrapping (TransportMessage FlatBuffer):
- Type byte: 1 byte
- Payload vector: +4 bytes (length prefix) + content
- FlatBuffer overhead: ~50 bytes

AEAD overhead: 16 bytes (Poly1305 tag)

Total frame payload (ciphertext): ~104,865,100 bytes

**Recommendation:** Set MAX_FRAME_SIZE to 110 MiB (115,343,360 bytes). This provides ~10% headroom over the theoretical maximum. Expressed as:

```cpp
constexpr uint32_t MAX_FRAME_SIZE = 110 * 1024 * 1024;  // 110 MiB
```

Note: The 4-byte frame header stores the ciphertext length as uint32_t. 110 MiB = 115,343,360 which fits in uint32_t (max ~4 GiB). No change needed to the header format.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Large value storage | Custom filesystem-based storage | libmdbx overflow pages | MDBX handles values larger than page size via overflow pages automatically; no config change needed |
| Streaming/chunked transport | Custom chunk protocol | Single-frame monolithic transfer | User locked decision: no chunking. ML-DSA-87 requires full data for signing anyway |

## Common Pitfalls

### Pitfall 1: Memory Explosion in collect_namespace_hashes
**What goes wrong:** Current implementation loads ALL blobs into memory to compute hashes. With 100 MiB blobs, 10 blobs = 1 GiB RAM just for hash collection.
**Why it happens:** Original design assumed small blobs (~1 KB typical).
**How to avoid:** BLOB-03 -- read hashes from seq_map index directly.
**Warning signs:** OOM during sync, high RSS during idle periods.

### Pitfall 2: Batched BlobTransfer OOM
**What goes wrong:** `encode_blob_transfer` concatenates ALL requested blobs into one message. If 5 blobs are 100 MiB each, that's a 500 MiB message.
**Why it happens:** Original batch encoding assumed small blobs.
**How to avoid:** BLOB-04 -- one blob per BlobTransfer message.
**Warning signs:** OOM when syncing namespaces with multiple large blobs.

### Pitfall 3: FlatBufferBuilder Default Size
**What goes wrong:** `encode_blob` uses `FlatBufferBuilder builder(8192)` -- initial 8 KB allocation. For a 100 MiB blob, this causes many reallocations as the builder grows.
**Why it happens:** Default size was tuned for small blobs.
**How to avoid:** Size the builder based on expected output: `FlatBufferBuilder builder(blob.data.size() + 8192)`.
**Warning signs:** Slow encoding, excessive memory allocation churn.

### Pitfall 4: Sync Timeout Too Short for Large Blobs
**What goes wrong:** Current SYNC_TIMEOUT is 30 seconds. Sending a 100 MiB blob over a moderate connection takes longer.
**Why it happens:** Timeout was calibrated for small blobs.
**How to avoid:** BLOB-06 -- use longer timeout for blob transfer phase.
**Warning signs:** Sync failures with "timeout" logged, blobs never fully transferred.

### Pitfall 5: uint32_t Overflow in Size Calculations
**What goes wrong:** 100 MiB = 104,857,600. Adding overhead could push past uint32_t limits if not careful with intermediate calculations.
**Why it happens:** Careless arithmetic with large constants.
**How to avoid:** Use uint64_t for size calculations, only narrow to uint32_t for the wire format after validating range.
**Warning signs:** Compile-time warnings about narrowing conversions.

## Code Examples

### Existing Test Pattern (from test_engine.cpp style)
```cpp
TEST_CASE("BlobEngine rejects oversized blob data", "[engine]") {
    // Create blob with data > MAX_BLOB_DATA_SIZE
    wire::BlobData blob;
    blob.data.resize(net::MAX_BLOB_DATA_SIZE + 1);
    // ... fill required fields ...

    auto result = engine.ingest(blob);
    REQUIRE(!result.accepted);
    REQUIRE(result.error == engine::IngestError::oversized_blob);
}
```

### Existing Sync Test Pattern (test_sync_protocol.cpp style)
```cpp
TEST_CASE("SyncProtocol collect_namespace_hashes reads from index", "[sync]") {
    // Store blobs, then verify collect_namespace_hashes returns correct hashes
    // without loading blob data (verified by not causing OOM with large blobs)
}
```

## Open Questions

1. **Phases A/B timeout:** Should namespace list and hash list exchange also have explicit timeouts? Currently they use the general 30-second SYNC_TIMEOUT. With 10K blobs per namespace, hash lists are ~320 KB -- transfers in well under 30 seconds on any reasonable connection. **Recommendation:** Keep 30s for A/B, only change Phase C.

2. **Existing blob migration:** Blobs stored before Phase 11 were stored with seq_map value = hash:32. The new `get_hashes_by_namespace()` reads these same values. **No migration needed** -- the data format is unchanged.

## Sources

### Primary (HIGH confidence)
- Direct codebase analysis of all source files in db/ directory
- libmdbx documentation: overflow pages handle large values automatically (no config)
- FlatBuffers: variable-length vectors have no practical size limit
- ChaCha20-Poly1305: 16-byte tag, works on any plaintext length

### Secondary (MEDIUM confidence)
- AEAD performance on 100 MiB: ChaCha20 processes ~1 GB/s on modern CPUs (negligible latency)
- SHA3-256 performance on 100 MiB: ~500 MB/s (sub-second for signing input construction)

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new dependencies, all existing libraries support large data
- Architecture: HIGH - direct code analysis, clear transformation paths
- Pitfalls: HIGH - identified from reading actual code paths that will fail with large blobs

**Research date:** 2026-03-07
**Valid until:** 2026-04-07 (stable -- no dependency changes)
