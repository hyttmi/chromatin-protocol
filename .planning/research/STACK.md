# Technology Stack: v2.0 Closed Node Model + Larger Blobs

**Project:** chromatindb v2.0
**Researched:** 2026-03-05
**Confidence:** HIGH (all claims verified against source code, official docs, and library limits)

## Executive Summary

No new dependencies are needed. Both features (access control and larger blob support) are achievable through configuration changes and code modifications to the existing stack. The v1.0 architecture was well-chosen -- every existing library handles the v2.0 requirements natively.

## Recommended Stack Changes

### Zero New Dependencies

| Category | Decision | Rationale |
|----------|----------|-----------|
| Access control | Pure code change | Pubkey whitelist is a config array + one check after handshake. nlohmann/json already parses arrays (see `bootstrap_peers` pattern in config.cpp). |
| Larger blobs | Constant + buffer changes | libmdbx supports values up to ~2 GiB. FlatBuffers supports buffers up to ~2 GiB. ChaCha20-Poly1305 supports messages up to ~256 GiB. Only frame size caps and buffer allocation hints need bumping. |
| Config format | Keep nlohmann/json | Already handles string arrays. No reason to add TOML/YAML for one new field. |

### Existing Components: What Changes

| Component | Current | Change To | Why |
|-----------|---------|-----------|-----|
| `MAX_FRAME_SIZE` (framing.h) | `16 * 1024 * 1024` (16 MiB) | `110 * 1024 * 1024` (110 MiB) | Must fit 100 MiB blob data + FlatBuffer overhead (~7.3 KiB) + AEAD tag (16 B). 110 MiB gives ~10% headroom. |
| `FlatBufferBuilder` initial size (codec.cpp) | Fixed 8192 bytes | `std::max(size_t(8192), blob.data.size() + 16384)` | Avoids ~17 reallocations (each copying the full buffer) when encoding a 100 MiB blob. Builder doubles on overflow. |
| `Config` struct (config.h) | No access control fields | Add `std::vector<std::string> allowed_keys`, `bool closed_mode = false` | Stores hex-encoded 32-byte namespace IDs (64 hex chars each). Empty vector = open mode (backward compatible). |
| Blob size limit | No explicit limit (implicit 16 MiB from frame size) | Add `constexpr uint32_t MAX_BLOB_DATA_SIZE = 100 * 1024 * 1024` | Checked in `BlobEngine::ingest()` before expensive ML-DSA-87 signature verification. |
| Sync blob transfer (sync_protocol.cpp) | Packs all requested blobs into one message | Send one blob per `BlobTransfer` message when blob exceeds 1 MiB | Prevents multi-hundred-MiB messages that would exceed frame size and spike memory. |

### Existing Components: No Changes Needed

| Component | Current Value | Why No Change |
|-----------|---------------|---------------|
| libmdbx geometry `size_upper` | 64 GiB | Max value size is ~2 GiB (`MDBX_MAXDATASIZE = 0x7FFF0000`). 100 MiB values use overflow pages automatically. 64 GiB holds ~640 max-size blobs -- sufficient for a closed node. |
| libmdbx page size | Default 4096 | Overflow pages handle large values transparently. No page size tuning needed. |
| Frame header format | 4-byte BE uint32 length prefix | Supports up to ~4 GiB frames. Well above 110 MiB max. |
| ChaCha20-Poly1305 AEAD | IETF variant, max msg ~256 GiB | 100 MiB is trivial. No changes. |
| AEAD nonce counter | uint64, per direction | At 100 MiB per frame, even 2^32 frames per session is safe. |
| Asio recv_raw buffer | `std::vector<uint8_t> data(len)` | Single allocation up to 110 MiB. Acceptable for server daemon. |
| nlohmann/json | v3.11.3 | Already supports array parsing. |
| libmdbx | v0.13.11 | No new features needed. |
| FlatBuffers | v25.2.10 | 32-bit offsets support up to ~2 GiB buffers. |
| Asio | 1.38.0 | No new features needed. |

## Detailed Technical Analysis

### 1. Access Control: Config and Check Point

**No new libraries.** This is a config struct addition + one conditional check.

**Config format -- use namespace IDs, not full pubkeys:**

```json
{
  "closed_mode": true,
  "allowed_keys": [
    "a1b2c3d4e5f6...64-hex-chars...namespace-id-1",
    "f0e1d2c3b4a5...64-hex-chars...namespace-id-2"
  ]
}
```

ML-DSA-87 public keys are 2,592 bytes = 5,184 hex characters. Namespace IDs (SHA3-256 of pubkey) are 32 bytes = 64 hex characters. Using namespace IDs because:
- 64 chars per entry vs 5,184 -- manageable in a config file
- The namespace IS the canonical identity in chromatindb
- The handshake already verifies the pubkey; config just gates which identities are allowed
- The check is: `SHA3-256(peer_pubkey) in allowed_set?` -- computed at runtime

**Check point:** `PeerManager::on_peer_connected()`. This fires after the PQ handshake succeeds (via `ready_cb_`), when `peer_pubkey_` is available. If the peer's derived namespace is not in the allowed set, call `close_gracefully()` immediately.

Why NOT check during handshake:
- Handshake code (`Connection::do_handshake()`) is untouched -- clean separation
- The handshake must complete to learn the peer's pubkey
- Access control is a policy decision (PeerManager), not a protocol decision (Connection)

**Data structure for lookup:** `std::unordered_set<std::string>` of hex-encoded namespace IDs. Parsed once at config load. O(1) lookup per connection attempt.

### 2. libmdbx: Large Value Storage

**Confidence: HIGH** -- verified via [libmdbx C API docs](https://libmdbx.dqdkfa.ru/group__c__api.html) and [source](https://github.com/erthink/libmdbx/blob/master/mdbx.h).

**Max value size:** `MDBX_MAXDATASIZE = 0x7FFF0000` = 2,147,418,112 bytes (~2 GiB). A 100 MiB value is 4.7% of this limit.

**How it works:** Values larger than one page (4,096 bytes) are stored using "overflow pages" -- contiguous sequences of pages allocated for a single value. This is transparent to the application; `txn.upsert()` and `txn.get()` work identically for 1 KiB and 100 MiB values.

**Performance characteristics for 100 MiB values:**
- **Write:** Allocates ~25,600 contiguous overflow pages, writes in a single ACID transaction. Slower than small writes but transactional.
- **Read:** Returns an `mdbx::slice` pointing into the mmap'd file. Zero-copy read. OS pages in the 100 MiB range on demand.
- **Expiry/delete:** Frees the overflow pages. libmdbx's page reclamation (GC) handles this automatically.

**No code changes to storage.cpp.** The existing `txn.upsert()` / `txn.get()` calls handle 100 MiB values without modification.

**Database size consideration:** With `size_upper = 64 GiB` and 100 MiB blobs, the database holds ~640 max-size blobs. For a closed node (private data), this is likely sufficient. If more capacity is needed, bump `size_upper` -- this is a runtime parameter, no recompile needed.

### 3. FlatBuffers: Large Buffer Encoding

**Confidence: HIGH** -- verified via [FlatBuffers internals docs](https://flatbuffers.dev/internals/).

FlatBuffers uses 32-bit offsets (`uoffset_t = uint32_t`). Maximum buffer size is `2^31 - 1` bytes (~2 GiB). A 100 MiB blob is 4.7% of this limit.

**`CreateVector` for large data:** `builder.CreateVector(data.data(), data.size())` copies the data into the builder's internal buffer. For 100 MiB:

1. Builder starts with `initial_size` bytes (currently 8,192)
2. On overflow, doubles the buffer and copies everything
3. With 8 KiB initial: 8K -> 16K -> 32K -> ... -> 128M = 14 reallocations, each copying increasing amounts

**Fix:** Set initial buffer size dynamically:
```cpp
size_t initial = std::max(size_t(8192), blob.data.size() + 16384);
flatbuffers::FlatBufferBuilder builder(initial);
```

This eliminates all reallocations for the data vector. The 16 KiB headroom covers FlatBuffer metadata, pubkey (2,592 B), signature (up to 4,627 B), and alignment padding.

**Memory during encode:** Peak is ~2x blob size (builder buffer + returned `std::vector<uint8_t>`). For 100 MiB: ~200 MiB peak. Acceptable for a server daemon.

**Decode side:** `decode_blob` receives a `std::span<const uint8_t>` pointing to the FlatBuffer data. FlatBuffers verification (`VerifyBlobBuffer`) is O(n) in buffer size. For 100 MiB this takes a few milliseconds -- negligible compared to ML-DSA-87 signature verification.

### 4. Network Framing: MAX_FRAME_SIZE

**Confidence: HIGH** -- verified via code review of framing.h/framing.cpp.

The 4-byte big-endian uint32 length prefix supports frames up to ~4 GiB. The `MAX_FRAME_SIZE` constant is a policy limit, not a protocol limit.

**New value: 110 MiB.** Calculation:
- 100 MiB blob data (user payload)
- 2,592 bytes ML-DSA-87 pubkey
- up to 4,627 bytes ML-DSA-87 signature
- 32 bytes namespace ID
- 12 bytes TTL + timestamp
- ~100 bytes FlatBuffer framing (blob + transport wrapper)
- 16 bytes AEAD tag
- Total: ~100.007 MiB + AEAD tag

110 MiB provides ~10% headroom. This is a single `constexpr` change.

**recv_raw impact:** `Connection::recv_raw()` allocates `std::vector<uint8_t>(len)` for each frame. A 100 MiB frame means a single 100 MiB heap allocation. This is fine because:
- Sequential sync protocol: only one blob in flight per connection at a time
- Server daemon: not memory-constrained like embedded systems
- At max 32 peers: worst-case ~3.5 GiB if all peers send max frames simultaneously (extremely unlikely; TCP flow control throttles this)

### 5. ChaCha20-Poly1305 AEAD Limits

**Confidence: HIGH** -- verified via [libsodium AEAD docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305).

The IETF ChaCha20-Poly1305 variant (used by libsodium) has a maximum message size of `64 * (2^32) - 64` bytes = ~256 GiB. A 100 MiB message is 0.04% of this limit. No changes needed.

Nonce counter: the codebase uses `uint64_t` counters per direction. Even with maximum-size frames (110 MiB), the counter would overflow after ~1.7 exabytes transferred on a single connection session. No risk.

### 6. Sync Protocol: Large Blob Transfer

**Current problem:** `encode_blob_transfer` packs ALL requested blobs into a single message:
```cpp
for (const auto& blob : blobs) {
    auto encoded = wire::encode_blob(blob);
    buf.insert(buf.end(), encoded.begin(), encoded.end());
}
```

If 5 blobs of 100 MiB are requested, this creates a ~500 MiB message -- exceeding `MAX_FRAME_SIZE` and spiking memory.

**Solution:** When any blob in the transfer set exceeds a threshold (e.g., 1 MiB), send each blob as a separate `BlobTransfer` message. The responder sends N `BlobTransfer` messages instead of 1, and the initiator's `pending_responses` count tracks them.

This is a code change in `peer_manager.cpp` (sync orchestration), not a library change. The existing `BlobTransfer` message type and wire format are reused.

### 7. Blob Size Validation in Engine

**New constant** in config.h or a dedicated constants header:
```cpp
constexpr uint32_t MAX_BLOB_DATA_SIZE = 100 * 1024 * 1024;  // 100 MiB
```

Checked in `BlobEngine::ingest()` after structural checks but BEFORE namespace verification and signature verification. This prevents a 1 GiB rogue blob from consuming CPU time on SHA3-256 hashing and ML-DSA-87 verification.

```cpp
// In engine.cpp, after pubkey size check:
if (blob.data.size() > MAX_BLOB_DATA_SIZE) {
    return IngestResult::rejection(IngestError::malformed_blob,
        "blob data exceeds max size");
}
```

### 8. Memory Budget for v2.0

| Scenario | Peak Memory | Notes |
|----------|------------|-------|
| Idle (no blobs in flight) | ~50 MiB | libmdbx mmap, process overhead |
| Single 100 MiB blob ingest | ~300 MiB | recv buffer + FlatBuffer decode + signing input copy + storage write |
| Sync: 10 large blobs, one-at-a-time | ~350 MiB | Sequential transfer, one blob decoded/ingested at a time |
| Worst case: 32 peers, all sending 100 MiB | ~3.5 GiB | Extremely unlikely; TCP backpressure limits concurrent large transfers |

**Recommendation:** Document minimum 4 GiB RAM for nodes handling large blobs. 1 GiB sufficient for typical small-blob usage.

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Access control identity | Namespace IDs (64 hex chars) | Full ML-DSA-87 pubkeys (5,184 hex chars) | Namespace ID is the canonical identity; 80x shorter in config |
| Access control check point | After handshake in `on_peer_connected` | During handshake | Pubkey is only known after handshake completes; cleaner separation of concerns |
| Large blob transport | One-per-message for blobs >1 MiB | Pack all into single BlobTransfer | Exceeds frame size; memory explosion |
| FlatBufferBuilder sizing | Dynamic based on data size | Fixed 128 MiB initial | Wastes memory for small blobs (common case) |
| Blob storage | Inline in libmdbx values | Separate filesystem files referenced by hash | libmdbx handles 100 MiB natively; filesystem adds consistency domain; YAGNI |
| Transfer protocol | Single-frame per blob | Chunked/streaming protocol | ML-DSA-87 requires full data for signature verification; chunking means full buffering anyway, adds complexity for zero benefit |

## What NOT to Add

| Technology | Why Not |
|------------|---------|
| Chunked transfer protocol | ML-DSA-87 signs `SHA3-256(namespace \|\| data \|\| ttl \|\| timestamp)`. The full `data` field must be in memory for both hashing and signature verification. Chunking still requires reassembly before verification. No benefit, added complexity. |
| Filesystem blob storage | libmdbx stores 100 MiB values efficiently via overflow pages. Adding filesystem storage creates two consistency domains (DB + files), complicates backup/restore, and adds crash-recovery edge cases. |
| Streaming AEAD | ChaCha20-Poly1305 handles 100 MiB messages trivially (~256 GiB max). The bottleneck is network bandwidth, not crypto throughput. |
| TOML/YAML config parser | One new config field (`allowed_keys`) does not justify a new dependency. nlohmann/json handles it. |
| Rate limiting library | Closed mode means only trusted nodes connect. Rate limiting is unnecessary when the peer set is explicitly authorized. |
| Memory-mapped file I/O for blobs | libmdbx already uses mmap internally. Adding our own mmap layer is redundant. |
| Config file watcher (inotify) | YAGNI. Restart the daemon to reload config. Config changes (adding/removing allowed keys) are rare operational events. |

## Integration Point Summary

```
Config (no new deps):
  config.h   -> Add: allowed_keys vector, closed_mode bool
  config.cpp -> Parse "allowed_keys" JSON array, "closed_mode" bool
               Validate: each entry is 64 hex characters

Access control (no new deps):
  peer_manager.h   -> Add: std::unordered_set<std::string> allowed_set_
  peer_manager.cpp -> In on_peer_connected(): derive namespace from
                      peer_pubkey, check allowed_set_, disconnect if rejected

Larger blobs (no new deps):
  framing.h         -> MAX_FRAME_SIZE = 110 * 1024 * 1024
  config.h          -> MAX_BLOB_DATA_SIZE = 100 * 1024 * 1024
  codec.cpp         -> Dynamic FlatBufferBuilder initial size
  engine.cpp        -> Add data size check in ingest() before sig verify
  peer_manager.cpp  -> One-blob-per-transfer for large blobs during sync
```

## Installation

No changes to build process. No new dependencies.

```bash
# Same as v1.0
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Sources

- [libmdbx C API documentation](https://libmdbx.dqdkfa.ru/group__c__api.html) -- `MDBX_MAXDATASIZE = 0x7FFF0000`, overflow page handling -- **HIGH confidence**
- [libmdbx GitHub](https://github.com/erthink/libmdbx) -- page size defaults, geometry configuration -- **HIGH confidence**
- [libmdbx overflow pages issue #192](https://github.com/erthink/libmdbx/issues/192) -- large value accounting and performance -- **HIGH confidence**
- [FlatBuffers Internals](https://flatbuffers.dev/internals/) -- 32-bit offset design, ~2 GiB buffer limit -- **HIGH confidence**
- [FlatBuffers 64-bit support discussion](https://github.com/google/flatbuffers/issues/7537) -- confirms 2 GiB limit for standard offsets -- **HIGH confidence**
- [FlatBuffers 2 GiB limitation discussion](https://github.com/google/flatbuffers/issues/7391) -- confirms limit applies to total buffer, not individual vectors -- **HIGH confidence**
- [libsodium ChaCha20-Poly1305 docs](https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305) -- IETF variant max ~256 GiB per message -- **HIGH confidence**
- [RFC 7539 ChaCha20-Poly1305](https://datatracker.ietf.org/doc/html/rfc7539) -- IETF AEAD construction, 32-bit block counter -- **HIGH confidence**
- [Reth libmdbx page size discussion](https://github.com/paradigmxyz/reth/issues/19546) -- real-world usage of libmdbx with large values -- **MEDIUM confidence** (third-party usage, not official docs)
- chromatindb v1.0 source code review (all files in src/) -- existing implementation details -- **HIGH confidence**

---
*Stack research for: chromatindb v2.0 -- Closed Node Model + Larger Blobs*
*Researched: 2026-03-05*
