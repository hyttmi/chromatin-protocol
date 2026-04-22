# Phase 119: Chunked Large Files — Research

**Researched:** 2026-04-18
**Domain:** C++20 CLI fan-out over PQ transport; FlatBuffers blob schema extension; streaming disk I/O; client-side chunk manifest with truncation-resistant integrity
**Confidence:** HIGH (codebase is small and every touched surface was read directly; no external libs being added)

## Summary

Phase 119 is a pure CLI feature. The node is untouched — a CDAT chunk and a CPAR manifest are just signed `Blob`s whose 4-byte `data` prefix differs, and the node's type-indexing from Phase 117 already buckets them with zero node code change. The entire phase lives in `cli/`.

Every non-trivial primitive is already in place:
- `Connection::send_async` + `conn.recv()` arrival-order drain (Phase 120) does fan-out.
- `envelope::encrypt` / `::decrypt` handles the per-chunk encryption.
- `encode_blob` / `decode_blob` + `build_signing_input` + `id.sign()` wrap the signed-Blob shape.
- `sha3_256` exists as a one-shot; the incremental variant (`OQS_SHA3_sha3_256_inc_*`) is already used by `build_signing_input` in `cli/src/wire.cpp:229-245` — so streaming SHA3 for the whole-file `plaintext_sha3` field is a 4-line reuse.
- CDAT magic is already defined in `cli/src/wire.h:218`. Adding CPAR is two lines plus a `type_label` branch and a `--type` flag acceptance.

The new build is: a `Manifest` FlatBuffers table (or hand-coded, see below), a 4-byte CPAR prefix, a streaming-read loop for upload, a `pwrite`-based out-of-order writer for download, and a cascade-delete loop for `cdb rm`.

**Primary recommendation:** Mirror Phase 120-02 exactly — two-phase greedy-fill/arrival-order-drain on `send_async` / `recv()` with a `rid → chunk_index` map — and add one new encoder/decoder for the CPAR manifest. Use hand-coded FlatBuffers encoding in `cli/src/wire.cpp` to match the style already used for `Blob` and `TransportMessage` (don't introduce `flatc` code generation for the CLI — the project has deliberately kept the CLI flatc-free). Read the file with `std::ifstream` 16 MiB at a time; write the output with POSIX `pwrite(2)` so out-of-order chunk arrivals land at the right offset without intermediate buffering.

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Blob-level chunking decision (size threshold) | CLI command layer (`cmd::put`) | — | D-05: threshold is a CLI choice; node is oblivious |
| Per-chunk encryption (CDAT payload) | CLI envelope layer (`envelope::encrypt`) | — | Identical to existing CENV recipient mechanism |
| Per-chunk Blob signing | CLI identity layer (`Identity::sign`) | — | Each CDAT is a regular Blob |
| Manifest construction / encoding | CLI wire layer (`cli/src/wire.cpp`, new helpers) | — | Parallel to `encode_blob` / `decode_blob` |
| Fan-out over PQ connection | CLI transport (`Connection::send_async`) | — | Phase 120 primitive, not extended here |
| Streaming file read (16 MiB chunks) | CLI command layer (new helper in `commands.cpp`) | — | Bounded memory per D-11 |
| Offset-write reassembly (`pwrite`) | CLI command layer (new helper in `commands.cpp`) | — | Arrival order ≠ chunk order per D-12 |
| Whole-file SHA3-256 verify | CLI wire layer (reuses `OQS_SHA3_sha3_256_inc_*`) | — | Already-proven incremental hasher |
| Cascade delete fan-out | CLI command layer (`cmd::rm`) | — | D-08/D-09: chunks first, manifest last |
| Node blob ingest / storage | Node (`db/`) | — | No changes — CDAT/CPAR flow through existing Data/WriteAck path |
| Type labeling in `ls --type CDAT` / `--type CPAR` | CLI wire layer (`wire.h` `type_label`) + main.cpp `--type` acceptance | — | Additive: add CPAR_MAGIC + branch in `type_label`, `is_hidden_type`, and `--type` flag parsing |

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CHUNK-01 | Upload files >500 MiB without full memory buffering | §Streaming Disk I/O — `std::ifstream` + 16 MiB reusable buffer. Max working set ≤ ~400 MiB (8 in-flight × ~50 MiB per in-flight-encoded-chunk peak). |
| CHUNK-02 | Upload splits into CDAT chunk blobs + CPAR manifest blob | §Manifest Schema, §Chunk Encryption Integration — each CDAT is a signed Blob with 4-byte `0x43 0x44 0x41 0x54` prefix on its `data`; manifest is a signed Blob with `0x43 0x50 0x41 0x52` prefix. |
| CHUNK-03 | Download detects CPAR and reassembles automatically | §Download Offset-Write Strategy, §cmd::get dispatch — after envelope decrypt, inspect first 4 bytes of plaintext; if CPAR, decode manifest and fan out chunk fetches. |
| CHUNK-04 | `cdb rm` of manifest deletes associated chunks | §Cascade Delete — `cmd::rm` fetches target, if CPAR parse manifest and tombstone each `chunk_hashes[i]` before tombstoning the manifest. Idempotent on retry (D-09). |
| CHUNK-05 | Envelope v2 includes segment count for truncation prevention | §Manifest Schema — `segment_count : uint32` in the manifest table is the truncation guard. Attacker cannot silently drop trailing chunks because the manifest is ML-DSA-87 signed by the owner. Defense-in-depth: `plaintext_sha3` over the full reassembled plaintext. |

---

## Focus Area 1: Manifest FlatBuffers Schema

### Recommendation: hand-coded encoder/decoder in `cli/src/wire.cpp`, NOT `flatc`-generated

**Rationale — [VERIFIED: `cli/src/wire.cpp`, `cli/CMakeLists.txt`]:** The CLI does not invoke `flatc` anywhere. Both `Blob` and `TransportMessage` are hand-coded in `cli/src/wire.cpp:37-217` using `flatbuffers::FlatBufferBuilder` and named VTable offsets (`transport_vt::*`, `blob_vt::*`). Only the node's `db/CMakeLists.txt:127-149` runs `flatc`. Introducing code generation to the CLI for one small table would add a CMake custom command and a generated header dependency for marginal benefit. Match the existing style.

**Schema definition (for documentation + optional node reuse):** Add a new file `db/schemas/manifest.fbs` mirroring `db/schemas/blob.fbs`:

```fbs
namespace chromatindb.wire;

// CPAR manifest: payload of a CPAR-typed Blob.
// Wire encoding: 4-byte "CPAR" magic prefix + this FlatBuffer.
// Semantics:
//   - chunk_size_bytes: plaintext bytes per chunk (16 MiB for Phase 119 writers;
//     carried for forward-compat so readers don't hard-code it).
//   - segment_count: number of CDAT chunks. Truncation guard (CHUNK-05).
//   - total_plaintext_bytes: full original file size for bounds + preallocate.
//   - plaintext_sha3: SHA3-256 of the concatenated plaintext (integrity guard).
//   - chunk_hashes: ordered blob_hash of each CDAT, 32 bytes each.
//     Flat [[uint8:32]] vector (not a nested ChunkRef table) — smaller wire
//     footprint, simpler encoder, and fields that might one day need a nested
//     table (per-chunk ciphertext size, per-chunk tweak) aren't in scope.
table Manifest {
  version: uint32;               // = 1; bump on breaking schema change
  chunk_size_bytes: uint32;
  segment_count: uint32;
  total_plaintext_bytes: uint64;
  plaintext_sha3: [ubyte];       // exactly 32 bytes
  chunk_hashes: [ubyte];         // flat concatenation of 32-byte hashes:
                                  //   size == segment_count * 32
}

root_type Manifest;
```

**Why a flat `[ubyte]` for `chunk_hashes` instead of `[[ubyte]]` or a nested table:**

- `[[ubyte]]` in FlatBuffers produces one Vector-of-Vector-offsets — for 640 chunks (10 GiB file) that's 640 × 4 bytes of offset overhead + 640 vector-size prefixes, plus indirection on every access.
- A flat concatenation is `segment_count × 32` bytes with no overhead. 64 chunks = 2 KiB, 640 chunks = 20 KiB, 3200 chunks (50 GiB file) = 100 KiB.
- Decoder trivially validates `chunk_hashes.size() == segment_count * 32`. If not, reject.
- Matches the style of `Blob.data` (a flat `[ubyte]`).

**[VERIFIED: `db/schemas/blob.fbs`, `cli/src/wire.cpp:22-31`]:** `Blob` defines 6 fields; VTable offsets are at 4, 6, 8, 10, 12, 14. The Manifest table above has 6 fields in the same pattern — VTable offsets 4, 6, 8, 10, 12, 14.

**Hand-coded encoder in `cli/src/wire.cpp`** (new functions, mirror `encode_blob` at line 153):

```cpp
// manifest_vt namespace parallel to blob_vt / transport_vt
namespace manifest_vt {
    constexpr flatbuffers::voffset_t VERSION               = 4;
    constexpr flatbuffers::voffset_t CHUNK_SIZE_BYTES      = 6;
    constexpr flatbuffers::voffset_t SEGMENT_COUNT         = 8;
    constexpr flatbuffers::voffset_t TOTAL_PLAINTEXT_BYTES = 10;
    constexpr flatbuffers::voffset_t PLAINTEXT_SHA3        = 12;
    constexpr flatbuffers::voffset_t CHUNK_HASHES          = 14;
}

struct ManifestData {
    uint32_t version = 1;
    uint32_t chunk_size_bytes = 0;
    uint32_t segment_count = 0;
    uint64_t total_plaintext_bytes = 0;
    std::array<uint8_t, 32> plaintext_sha3{};
    std::vector<uint8_t> chunk_hashes;   // size == segment_count * 32
};

std::vector<uint8_t> encode_manifest_payload(const ManifestData& m);
// Returns: [CPAR magic:4][FlatBuffer-encoded Manifest]

std::optional<ManifestData> decode_manifest_payload(std::span<const uint8_t> data);
// Expects leading CPAR magic; strips it, decodes the FlatBuffer, validates
// chunk_hashes.size() == segment_count * 32 and plaintext_sha3.size() == 32.
```

The `data` field of the enclosing signed `Blob` is then exactly what `encode_manifest_payload()` returns — 4 bytes of CPAR magic followed by the FlatBuffer. Same pattern as how CENV envelopes put their 4-byte `CENV` magic at the head of the envelope bytes.

**[CITED: `cli/src/wire.h:217-218`]:** `CDAT_MAGIC = {0x43, 0x44, 0x41, 0x54}` exists. Add `CPAR_MAGIC = {0x43, 0x50, 0x41, 0x52}` right next to it.

**Confidence:** HIGH — the pattern is literally copy-paste-adapt from existing `encode_blob`.

---

## Focus Area 2: Streaming Disk I/O for 5+ GiB Uploads

### Recommendation: `std::ifstream` + reusable 16 MiB buffer

**What exists today — [VERIFIED: `cli/src/commands.cpp:30-36, 516-538`]:**

```cpp
static std::vector<uint8_t> read_file_bytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
```

This reads the whole file into RAM. It's used at `commands.cpp:536` inside the existing small-file path. **The chunked path cannot use it.**

**Comparison:**

| Approach | Memory footprint | Portability | Complexity | Verdict |
|----------|------------------|-------------|------------|---------|
| `std::ifstream::read()` into reusable buffer | 16 MiB × 1 buffer + in-flight buffers | Portable C++ | Trivial | **RECOMMENDED** |
| POSIX `pread(2)` with manual fd | Same as ifstream | POSIX only | Lower-level — no win | Skip |
| `mmap(2)` | Kernel keeps 16 MiB × 8 pages hot | POSIX only | Faulting pages in on first access; no improvement over explicit read for sequential scan | Skip |
| Asio `posix::stream_descriptor` | Async but blocking CLI has nothing to overlap | POSIX only | Introduces reactor for one reader | Skip |

**Memory analysis (per D-11):**

Working set at peak, for an in-flight chunk:

1. Plaintext read buffer: 16 MiB (reusable; one buffer, reused for each read)
2. `build_put_payload` allocation (metadata + chunk plaintext): ~16 MiB — but for chunks we do NOT wrap in the JSON metadata envelope. See §Focus Area 4 — chunks are raw plaintext to `envelope::encrypt`. So this step is skipped for chunks. Savings: 16 MiB.
3. `envelope::encrypt` output: 16 MiB ciphertext + ~1648 bytes recipient stanzas + 16 MiB × (segments × 16-byte tags). With 1-MiB envelope segments inside a 16-MiB chunk = 16 segments × 16-byte tag = 256 B overhead per chunk. Call it 16 MiB + tiny.
4. `encode_blob` FlatBuffer: 16 MiB envelope + 2592 pubkey + 4627 signature + namespace + overhead ≈ 16 MiB + 8 KiB.
5. `send_chunked` inside `Connection::send` → serializes through the already-serialized send queue; no new buffer held by caller.

Peak caller-side buffers per in-flight chunk: ~32 MiB (one for read, one for encrypted Blob being sent) × 8 in flight = **~256 MiB working set**.

That's more than D-11's 128 MiB estimate but well under available memory on any modern machine. The 128 MiB figure in CONTEXT.md counts only the read buffers (`8 × 16 MiB`). Planner should note this inflated real number.

**Optimization to hit 128 MiB:** release the plaintext read buffer immediately after `envelope::encrypt` completes (it's consumed). Then only the encoded Blob buffer (~16 MiB) is in flight per slot = 8 × 16 MiB = 128 MiB. Straightforward — `envelope::encrypt` already moves its `plaintext` span, doesn't retain a reference.

**File structure:**

```cpp
// In a new helper (cli/src/chunked.h or inline in commands.cpp):
struct ChunkStream {
    std::ifstream f;
    std::vector<uint8_t> buf;   // reusable, resized to chunk_size once
    uint64_t bytes_read = 0;
    uint64_t total_size = 0;

    // Returns span of the next plaintext chunk (may be short on the last one).
    // Returns empty span on EOF.
    std::span<const uint8_t> next_chunk(size_t chunk_size);
};
```

**[VERIFIED: `cli/src/commands.cpp:514`]:** Existing `MAX_FILE_SIZE = 500 MiB` guard must be RAISED for the chunked path. Suggested new absolute cap: **1 TiB** (reasonable headroom, keeps chunk_count in `uint32` range: 1 TiB / 16 MiB = 65536 chunks = 2 MiB manifest). Planner should pick an explicit maximum.

**Confidence:** HIGH

---

## Focus Area 3: `pwrite` Offset-Write Strategy on Download

### Recommendation: POSIX `pwrite(2)` via `<fcntl.h>` / `<unistd.h>`

**[VERIFIED: `grep -r pwrite cli/`]:** No existing `pwrite` calls in the CLI. This is new.

**Why `pwrite` over the alternatives:**

| Approach | Portability | Sparse-file support | Out-of-order write | Complexity |
|----------|-------------|---------------------|--------------------|------------|
| `std::ofstream` + `seekp` + `write` | Portable | OS-level | Yes but stateful | Low |
| POSIX `pwrite(2)` | POSIX (Linux / macOS) | Yes (fs-dependent) | Yes, atomic per call | Very low |
| Asio `random_access_file` | Asio 1.26+ POSIX only | Yes | Yes | New dependency surface |

`pwrite(2)` is the minimum machinery. It takes an fd, buffer, length, and absolute offset — perfect for "this chunk goes at `chunk_index * chunk_size_bytes`". Phase 120's infrastructure proved that chunks arrive out of order (D-08); the writer must tolerate this by construction.

The project targets Linux (liboqs, asio, tmpfiles, systemd unit). macOS may or may not be a stated target; chromatindb as a protocol daemon is Linux-first. `pwrite` is POSIX-standard since 1995 and works on all platforms the project has ever targeted. Non-portability risk: essentially zero.

**Preallocate vs. truncate-at-end:**

- **Preallocate:** `ftruncate(fd, total_plaintext_bytes)` right after open. Filesystem reserves space (or creates sparse file on ext4/xfs/btrfs). Subsequent `pwrite`s fill in blocks.
- **Truncate-at-end:** don't preallocate; `pwrite` naturally extends the file. Final chunk's `pwrite` reaches the end.

**Recommendation: preallocate via `ftruncate`.** Two reasons:
1. If the filesystem runs out of space during download, `ftruncate` fails upfront and the user gets a clean "disk full" error before signing work. Without it, failure happens mid-stream with partial output.
2. Downstream tools that `stat()` the file while it's being written see the final size from the start, which is the expected behavior for a known-size transfer.

**Sparse-file fallback:** on filesystems that don't support sparse files (FAT, old NFS), `ftruncate` fills with zeros and uses full disk space. That's acceptable — the user knows they're downloading a 10 GiB file. No fallback needed.

**Atomicity on mid-download failure (D-12):**

```cpp
// Pseudocode inside cmd::get chunked path:
int fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0644);  // EXCL unless --force
ftruncate(fd, manifest.total_plaintext_bytes);
bool ok = drive_chunk_batch(...);  // pipelined pwrite loop
close(fd);
if (!ok) {
    unlink(out_path);  // D-12: remove partial file
    return 1;
}
```

`O_EXCL` gives the same "don't overwrite unless --force" semantics already present for the small-file path at `commands.cpp:790-795` (`if (!force_overwrite && fs::exists(out_path))`).

**[ASSUMED]** macOS portability: `pwrite(2)` is POSIX-1.2001, present on all Unix systems chromatindb is likely to run on. Not tested in this research session, but the risk is low enough that the planner should not introduce platform shims preemptively.

**Confidence:** HIGH for Linux; MEDIUM for macOS (defer to user if cross-platform is in scope).

---

## Focus Area 4: Chunk Encryption Integration

### Recommendation: reuse `envelope::encrypt` per chunk, skip `build_put_payload`

**What `envelope::encrypt` takes — [VERIFIED: `cli/src/envelope.h:12-15`]:**

```cpp
std::vector<uint8_t> encrypt(
    std::span<const uint8_t> plaintext,
    std::vector<std::span<const uint8_t>> recipient_kem_pubkeys);
```

Takes a plaintext span + a vector of KEM pubkey spans. Output is a CENV envelope (header + stanzas + segmented AEAD ciphertext). Good: this is exactly what we need per chunk.

**Critical design decision: what's the "plaintext" of a CDAT chunk?**

Two options:

| Option | Plaintext input to envelope::encrypt | Header metadata | Bytes wasted |
|--------|--------------------------------------|-----------------|---------------|
| **A: raw chunk bytes** | 16 MiB of file | Stored in CPAR manifest only | Zero |
| B: `build_put_payload(filename, chunk_bytes)` | ~16 MiB + JSON metadata | Duplicated on every chunk | 60-100 bytes × segment_count |

**Recommend Option A.** Reasons:
1. The existing `build_put_payload` (at `commands.cpp:197-221`) exists because the single-blob CENV path needs to carry filename+metadata INSIDE the envelope (there's no other place for it). For chunked uploads, the manifest carries metadata (filename lives on the CPAR's payload at a higher level, or in the CENV-wrapped manifest's metadata).
3. The manifest is itself a signed Blob whose `data` field is `[CPAR magic][Manifest FlatBuffer]`. The Manifest should carry filename so `cdb get <manifest_hash>` can save to the right filename. Add a `filename: string` field to the Manifest table.

**Revised Manifest schema — add filename:**

```fbs
table Manifest {
  version: uint32;
  chunk_size_bytes: uint32;
  segment_count: uint32;
  total_plaintext_bytes: uint64;
  plaintext_sha3: [ubyte];       // 32 bytes
  chunk_hashes: [ubyte];         // segment_count * 32 bytes
  filename: string;              // original filename; may be empty for stdin input
}
```

**But wait — where's the metadata envelope for the manifest itself?**

The manifest is ML-DSA-87 signed but on the current single-blob put path it's also envelope-encrypted to recipients (CENV-wrapped), which is how recipients' KEM pubkeys are stored. Two approaches:

| Option | Manifest Blob's `data` field layout | Who can read filename |
|--------|-------------------------------------|-----------------------|
| **M1: manifest plaintext is CPAR, then wrapped in CENV** | `[CENV header+stanzas+ciphertext(wrapping [CPAR magic][Manifest FB]))]` | Only recipients (matches chunks) |
| M2: manifest plaintext is CPAR directly, no CENV wrapping | `[CPAR magic][Manifest FB]` (signed, not encrypted) | Anyone who can read the node |

**Recommend M1.** Chunks are CENV-encrypted; manifest MUST be too, otherwise an observer learns the filename, file size, and chunk hash list. This means:
- `data = envelope::encrypt([CPAR magic][Manifest FB], recipient_spans)` — so `data` starts with the CENV 4-byte magic, not CPAR.
- When `cmd::get` receives the manifest, it envelope::decrypt's first (existing code does this already — see `commands.cpp:760-770`), and *the plaintext* then starts with CPAR. The existing type check "is it CENV?" still works for the detection branch; the new check "is the decrypted plaintext CPAR?" is where the chunked reassembly kicks in.
- Same for chunks: `CDAT_data = envelope::encrypt([CDAT magic][raw_chunk_bytes], recipient_spans)`. After decrypt, first 4 bytes identify it as a chunk.

**But do we need the 4-byte CDAT/CPAR magic INSIDE the encrypted plaintext?** For chunks, no — once the envelope decrypts, we already know it's a chunk because we fetched it as part of a manifest's `chunk_hashes` list. The 4-byte magic is on the Blob's `data` field (visible to the NODE for type indexing per Phase 117). The node sees: `blob.data` starts with CENV magic (envelope). But TYPE-01 indexes `data[0..4]` as the blob_type, which would be CENV for both the manifest AND the chunks.

This creates a problem: **`cdb ls` can't distinguish a chunk from a manifest from a regular file — all three look like CENV to the node.**

**Resolution: put the CDAT/CPAR magic on the Blob's OUTER data field, NOT encrypted.** The plaintext inside the envelope is then raw bytes. The wire shape is:

```
Blob.data = [CDAT magic:4][CENV envelope of raw chunk bytes]
Blob.data = [CPAR magic:4][CENV envelope of Manifest FlatBuffer]
Blob.data = [CENV magic:4][envelope of build_put_payload output]   (existing small-file)
```

**[VERIFIED: `cli/src/envelope.cpp:30`]** `MAGIC = {0x43, 0x45, 0x4E, 0x56} // "CENV"` — envelope always starts with CENV magic. So with the new design, the first byte of CENV envelope is offset +4 in `blob.data` for CDAT/CPAR blobs.

**Dispatch at `cmd::get`:**

1. Fetch blob, decode, read `blob.data`.
2. Inspect first 4 bytes of `blob.data`:
   - `CDAT`: error — asking for a chunk directly is a mistake (or a `--force` tool). Standard user error.
   - `CPAR`: strip 4 bytes, envelope::decrypt the rest → get Manifest FlatBuffer → enter chunked reassembly.
   - `CENV`: existing path — envelope::decrypt → parse_put_payload → write file.
   - Other (PUBK, TOMB, DLGT, DATA): existing handling / error.

**This is a CLEANER design than the CONTEXT.md implied.** CONTEXT.md says "after decrypting the blob's payload, the CLI checks the 4-byte type prefix". That works for `CENV`-wrapping-`CPAR` IF the node's Phase 117 type index sees the CPAR byte. But it cannot, because the bytes are encrypted. The type index would index CENV for every chunked-large-file blob, breaking `cdb ls --type CPAR` and breaking server-side filtering in `cdb rm` (which wants to know "is this a manifest?" before fetching it).

**Recommendation: put CDAT / CPAR magic as the first 4 bytes of `blob.data`, FOLLOWED by the CENV envelope.** The planner should confirm this with the user — it's a material change from CONTEXT.md's implicit assumption. **[ASSUMED]: user wants node-visible blob types for CDAT and CPAR so `ls` and `rm` work against type prefixes.**

This adds **T-119-09: filename leaks to the node** (the CENV envelope still hides payload; the 4-byte magic leaks only the *fact* that this is a chunked-file blob, same as PUBK/DLGT leak their role today). Acceptable tradeoff.

**Alternative if the user prefers hidden types:** keep the magic inside the envelope, accept that `cdb ls --raw` lumps chunks+manifest+regular-files as "CENV", and do the chunked-vs-regular distinction only after decrypt. This works but makes `cdb ls --type CDAT` and server-side filtering unavailable.

**Per-chunk sign + encrypt flow (CDAT):**

```cpp
// For each chunk:
auto plaintext_chunk = stream.next_chunk(16 MiB);
auto cenv_bytes     = envelope::encrypt(plaintext_chunk, recipient_spans);

std::vector<uint8_t> blob_data;
blob_data.reserve(4 + cenv_bytes.size());
blob_data.insert(blob_data.end(), CDAT_MAGIC.begin(), CDAT_MAGIC.end());
blob_data.insert(blob_data.end(), cenv_bytes.begin(), cenv_bytes.end());

auto ts  = now_seconds();
auto sig_input = build_signing_input(ns, blob_data, ttl, ts);
auto sig = id.sign(sig_input);

BlobData blob{};
blob.namespace_id = ns;  // same for every chunk
blob.pubkey       = id.signing_pubkey_vec();
blob.data         = std::move(blob_data);
blob.ttl          = ttl;
blob.timestamp    = ts;
blob.signature    = std::move(sig);

auto flatbuf = encode_blob(blob);
auto this_rid = rid++;
conn.send_async(MsgType::Data, flatbuf, this_rid);
rid_to_chunk_index[this_rid] = chunk_idx;
```

**Per-chunk timestamps and TTL:** all chunks and the manifest must share the same `timestamp` and `ttl`. Otherwise chunks with a shorter TTL expire before the manifest. Planner should lock this in the plan.

**Name disambiguation for "segment":**

- `envelope::SEGMENT_SIZE = 1048576` at `cli/src/envelope.cpp:35` — the 1 MiB AEAD segment INSIDE one envelope.
- `Manifest.segment_count` — the number of CDAT chunks across the logical file.

These are different layers. Recommend naming in RESEARCH and the plan:
- **envelope segment:** the 1 MiB AEAD-sealed block inside one CENV payload.
- **chunk:** one CDAT blob (== 16 MiB of plaintext).
- The manifest field could be named `chunk_count` instead of `segment_count` to eliminate ambiguity. **But CHUNK-05 requires `segment count` to be the v2 truncation guard name.** Keep `segment_count` in the manifest schema (matches the requirement), but in code comments and variable names use `chunk_count` / `num_chunks` to keep the envelope "segment" meaning separate.

**Confidence:** HIGH on the mechanics; MEDIUM-HIGH on the "CDAT magic on outer blob.data" design choice (needs user confirmation since it materially changes how Phase 117's type index sees these blobs).

---

## Focus Area 5: Incremental per-chunk SHA3 for `plaintext_sha3`

### Recommendation: reuse the `OQS_SHA3_sha3_256_inc_*` wrappers already called by `build_signing_input`

**[VERIFIED: `cli/src/wire.cpp:229-245`]:** The incremental SHA3-256 state machine is already used:

```cpp
OQS_SHA3_sha3_256_inc_ctx ctx;
OQS_SHA3_sha3_256_inc_init(&ctx);
OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());
OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());
// ... more absorbs ...
std::array<uint8_t, 32> hash{};
OQS_SHA3_sha3_256_inc_finalize(hash.data(), &ctx);
OQS_SHA3_sha3_256_inc_ctx_release(&ctx);
```

This is exactly the incremental API Phase 119 needs. **Reuse, don't re-implement, and don't introduce a wrapper struct** — per project memory "No copy-paste of utilities" but also "utilities go into shared headers". Two clean options:

| Option | Shape | Fit |
|--------|-------|-----|
| A | Add `Sha3Hasher` class in `cli/src/wire.h` (init in ctor, absorb method, finalize method returning `std::array<32>`) | Cleanest RAII; reusable for future streaming hash needs |
| B | Inline the 5 lines in `cmd::put` chunked path | Lowest surface; one use site |

**Recommend Option A** — the phase will want to compute `plaintext_sha3` during upload (as chunks are read) AND during download (as chunks are decrypted and written). Two use sites = not one-off. The wrapper is 15 lines and lives in `wire.cpp` next to `sha3_256()`.

**Proposed addition to `wire.h`:**

```cpp
/// Incremental SHA3-256 hasher. RAII wrapper around OQS_SHA3_sha3_256_inc_ctx.
class Sha3Hasher {
public:
    Sha3Hasher();
    ~Sha3Hasher();

    Sha3Hasher(const Sha3Hasher&) = delete;
    Sha3Hasher& operator=(const Sha3Hasher&) = delete;

    void absorb(std::span<const uint8_t> data);
    std::array<uint8_t, 32> finalize();   // single-shot; object is done after

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // hides OQS type from the header
};
```

**Upload use:** absorb each 16 MiB plaintext chunk as it's read; finalize after last chunk; the result goes into `Manifest.plaintext_sha3`.

**Download use:** absorb each decrypted chunk plaintext before writing it; at end, compare to `manifest.plaintext_sha3`. Mismatch → `unlink` + error per D-12.

**Caveat for download:** because chunks arrive OUT OF ORDER, you cannot absorb "as they arrive". Buffer strategy options:

| Strategy | Memory | Correctness |
|----------|--------|-------------|
| **Post-write re-read loop:** after all chunks written to disk, open(O_RDONLY) + read sequentially + absorb + finalize | 16 MiB read buffer | Correct; one extra disk pass |
| Keep decrypted chunks in RAM until in-order | Up to `segment_count × 16 MiB` | UNACCEPTABLE for 10 GiB files |
| Absorb in chunk-index order only when the next-in-order chunk arrives | Up to `kPipelineDepth × 16 MiB` + buffering | Correct; adds an in-order-absorb gate |

**Recommend the in-order absorb gate.** Keep a `std::map<size_t, std::vector<uint8_t>>` of "chunks I've received but haven't absorbed yet because earlier ones are still outstanding". On each decrypted chunk:
1. Store in the map.
2. While `map.contains(next_absorb_index)`: absorb, `pwrite`, advance `next_absorb_index`, erase from map.
3. Early-free the plaintext buffer after absorb+pwrite.

Max map size = `kPipelineDepth - 1 = 7` slots × 16 MiB = 112 MiB. Acceptable.

**Alternative — the "post-write re-read loop"** is simpler to reason about (and fits "YAGNI, keep it clean" from project prefs):
1. pwrite chunks as they arrive (no buffering).
2. After the last chunk lands, `lseek(fd, 0, SEEK_SET)` + read + absorb + finalize.
3. Compare to manifest. Mismatch → unlink + error.

Disadvantage: one extra full file read (10 GiB @ ~1 GB/s NVMe = 10s). Advantage: no map, no in-order gate, no extra code.

**Recommend the post-write re-read loop.** Simpler, and the extra read is a rounding-error on top of the network download time. Planner picks.

**Confidence:** HIGH

---

## Focus Area 6: ML-DSA-87 Signing Cost

**Claim from the task:** ~4.6 ms per chunk × 640 chunks (10 GiB @ 16 MiB) = ~3 seconds of CPU-bound signing on the caller thread.

**[ASSUMED: training-data estimate]:** ML-DSA-87 sign cost is in the range of 1-5 ms on modern x86_64 hardware (Tier-2 NIST PQC level, signature size 4627 bytes). No benchmark in this codebase. Actual cost varies by CPU; liboqs 0.15.0 uses reference implementations unless a SIMD-optimized build is selected.

**To verify in practice:** one-off microbenchmark = 1000 signs of a fixed 32-byte input; divide elapsed by 1000.

**Why it's acceptable for Phase 119:**
- 3 seconds on a 10 GiB upload that would otherwise take 10+ seconds of network time (even on a 10 Gb/s LAN, 10 GiB = 80 Gb = 8s) is overlapped with the network work once pipelining starts.
- 16 MiB is the sweet spot: smaller chunks mean more signatures (higher amortized CPU), larger chunks mean fewer but each signature-for-a-16-MiB-blob is the same 1-5 ms (signature is over the 32-byte signing input hash, not the blob).

**UX messaging:** per-chunk progress line already lands at completion (D-07) — user sees chunks flowing. No "signing…" messaging needed unless profiling reveals a visible pause before the first chunk ships. For a 100 GiB file (6400 chunks → ~30 seconds of aggregate signing), a pre-upload "preparing X chunks" line could be useful but is discretionary — defer to the planner.

**Deferred item already captured:** CONTEXT.md §Deferred Ideas includes "Parallel chunk signing on a worker pool" — correctly scoped as a future phase only if profiling shows signing dominates.

**Confidence:** MEDIUM (exact timings not measured) — documented as an ASSUMED claim so the user can verify.

---

## Focus Area 7: `request_id` Space During Chunked Upload

**[VERIFIED: `cli/src/commands.cpp:554, 669`]:** `rid` is a plain batch-local `uint32_t` in `cmd::put` and `cmd::get`:

```cpp
uint32_t rid = 1;
std::unordered_map<uint32_t, size_t> rid_to_index;
// ... rid++ after each send_async
```

**Connection state for rid:** there is NONE. `Connection` has `send_counter_` for AEAD nonce but does not track rid. The rid is owned by the command layer, scoped to one `cdb put` / `cdb get` batch.

**[VERIFIED: `cli/src/connection.h:104`]:** `pending_replies_` is keyed by `uint32_t` rid but only tracks in-flight correlations, not a running counter.

**Implication for chunked upload inside `cdb put`:**

`cdb put a b c largefile.bin` is one invocation with four "files". The chunked path for `largefile.bin` must use rids that:
1. Don't collide with rids for `a`, `b`, `c` (they share the same connection and pipeline depth).
2. Are monotonically increasing (the existing pattern).

**Simplest design: keep the single batch-local `rid = 1` counter at `cmd::put`-level, advance it for every `send_async` regardless of whether the blob is a small file or a chunk.** One counter, many kinds of sends.

Chunked file with 64 chunks + 1 manifest = 65 rids consumed for that "file entry" in the batch. The small-file counterparts consume 1 rid each. Total for `cdb put a b c largefile.bin` = 3 + 65 = 68 rids.

**How this affects `rid_to_index` tracking:**

Today, `rid_to_index` maps rid → "file index in the batch". For a chunked upload, we need rid → "file index AND chunk index (or sentinel for manifest)". Two designs:

| Design | Shape | Pros | Cons |
|--------|-------|------|------|
| **A: unified map, per-file substructure** | `rid_to_position[rid] = {file_idx, chunk_idx}` where `chunk_idx = -1` means "small-file or manifest" | One map, one drain loop | Extra dispatch logic per reply |
| **B: nested batches** | The chunked-upload helper opens its own inner `rid_to_chunk_index` map, drains inside its own phase B, doesn't interleave with other files | Clean separation | Large files serialize behind each other — but they already do since pipeline depth bounds the whole connection to 8 in-flight |

**Recommend Design B — per-file chunked upload as a self-contained helper.** Inside `cmd::put`, each `FileEntry` gets dispatched to either `put_small(conn, f, ...)` (existing code) or `put_chunked(conn, f, manifest_ttl, ...)`. The chunked helper owns its own rid counter and drain loop. Caller's outer loop sees "one result per file", unchanged.

This keeps `cmd::put` simple and mirrors Phase 120's pattern (two-phase-fan-out-per-batch). The inner chunked batch IS the batch — pipeline depth still bounds to 8 in flight, since we share `Connection::kPipelineDepth` via `send_async`.

**Edge case: the outer rid counter in `cmd::put` should pick up where the inner `put_chunked` left off.** Two options:
- Pass `rid` by reference into `put_chunked`; it advances it.
- Return `next_rid` from `put_chunked`.

Either works. Planner picks the ergonomic shape.

**Confidence:** HIGH

---

## Focus Area 8: Test Strategy

### Recommendation: unit tests for manifest encoder/decoder + chunked helpers; live-node integration follows the Phase 120 pattern

**[VERIFIED: `cli/tests/CMakeLists.txt`]:** Existing test infrastructure:
- Catch2 v3.7.1 via FetchContent (project root CMakeLists.txt FetchContent for Catch2).
- Tests in `cli/tests/test_*.cpp`, auto-discovered via `catch_discover_tests(cli_tests)`.
- The 120-01 plan extracted pipeline pump logic into `cli/src/pipeline_pump.h` (header-only) so it could be tested without a socket. Same pattern works here.
- 49 existing tests pass (Phase 120 checkpoint summary).

**Unit tests to add:**

**`cli/tests/test_manifest.cpp`** (new, tag `[chunked]` or `[manifest]`):
1. `encode_manifest_payload` + `decode_manifest_payload` round-trip (small, 2 chunks).
2. Round-trip with 64 chunks (1 GiB-scale manifest), verify `chunk_hashes.size() == 64 * 32`.
3. Round-trip with 0 chunks — reject on decode (degenerate manifest).
4. Decode rejects truncated chunk_hashes (size not a multiple of 32).
5. Decode rejects missing CPAR magic.
6. Decode rejects wrong plaintext_sha3 size (≠ 32).
7. Version mismatch handling (decoder returns the version field; caller decides).

**`cli/tests/test_chunked.cpp`** (new, tag `[chunked]`):
1. `ChunkStream::next_chunk` reads exact 16 MiB from a fixture file, returns short span on EOF.
2. `Sha3Hasher` absorb+finalize matches one-shot `sha3_256` for short input.
3. `Sha3Hasher` incremental matches one-shot for a large input split across many absorbs.
4. Auto-detect-chunked: given a file ≥ 400 MiB (use a sparse file or stub `stat`), returns "chunked" path; below threshold returns "single-blob" path.
5. Round-trip: build manifest from a fake chunk list, decode, confirm fields preserved.
6. `plaintext_sha3` mismatch detection: corrupt one chunk's plaintext, final compare fails.

**Integration (live-node) tests:**

Phase 120-02 established the pattern: `/tmp/seq` vs `/tmp/pipe` output dirs, `diff -r`, wall-clock measurement against `192.168.1.73`. Phase 119 checkpoint should verify:
1. Upload 1 GiB file → 64 CDAT + 1 CPAR blobs on node (list via `cdb ls --raw --type CDAT`).
2. `cdb get <manifest_hash>` reassembles byte-identical.
3. `cdb rm <manifest_hash>` removes all 65 blobs (verify via subsequent `cdb ls`).
4. Upload 500 MiB and 400 MiB files — confirm 400 MiB triggers chunked path, 399 MiB does not.
5. Mid-download failure: kill cdb mid-transfer, verify output file is unlinked.

**Catch2 tag convention:** Phase 120 used `[pipeline]`. Phase 119 should use `[chunked]` for the core tests and `[manifest]` for the encoder/decoder specifically. Both tags on the chunked end-to-end tests is fine.

**Confidence:** HIGH

---

## Focus Area 9: Cross-Plan Boundaries (2-plan split)

### Recommendation: confirm the suggested split with one refinement

**Suggested (from task):**
- Plan 119-01: Protocol primitives + schema + write path
- Plan 119-02: Read path + rm cascade + live-node integration

**Refinement — move `rm_chunked` to Plan 01** because it exercises the same manifest-decode path that the read path needs, and having it in 01 lets Plan 01 be end-to-end testable against the live node. Otherwise Plan 01 ships code that can upload but not clean up, and Plan 02 is the one that has to build the `rm` cascade. That creates a window where users have orphan chunks and no delete path.

**Revised split:**

**Plan 119-01 — Write path + cascade + primitives** (~5-7 tasks):
1. `CPAR_MAGIC` constant + `type_label` update + `is_hidden_type` update + `--type CPAR` flag acceptance in `main.cpp`.
2. `Sha3Hasher` class in `wire.h` / `wire.cpp`.
3. `ManifestData` struct + `encode_manifest_payload` + `decode_manifest_payload` in `wire.h` / `wire.cpp`.
4. Manifest round-trip unit tests (tag `[manifest]`).
5. `put_chunked` helper in `commands.cpp` (streaming read, per-chunk envelope+sign, fan-out, manifest build+send).
6. `cmd::put` dispatcher — stat file, branch to `put_chunked` if ≥ 400 MiB.
7. `rm_chunked` helper in `commands.cpp` + `cmd::rm` branch on CPAR type (fetch manifest, decrypt, tombstone each chunk, tombstone manifest).
8. Live-node smoke: upload 1 GiB file, `ls --raw --type CDAT` shows 64 chunks, `rm` removes all.

**Plan 119-02 — Read path + integrity + checkpoint** (~4-5 tasks):
1. `get_chunked` helper in `commands.cpp` (parse manifest, preallocate output file, fan-out ReadRequests, pwrite on arrival).
2. `cmd::get` dispatcher — inspect decrypted plaintext, branch to `get_chunked` if CPAR.
3. Post-download `plaintext_sha3` verification (re-read + Sha3Hasher + compare); unlink on mismatch (D-12).
4. Unit tests for the in-order absorb + pwrite logic (tag `[chunked]`).
5. Live-node checkpoint: upload 1 GiB, download, `diff` byte-identical; kill mid-download, verify unlink.

**Why this split is natural:**
- Plan 01 is "can users put large files and clean up". Useful even without the reassembly path — upload works and nothing is orphaned.
- Plan 02 is "can users get large files back". Depends on 01's manifest schema.
- Both plans are testable end-to-end on their own (plan 01 can test a round-trip via `cdb put largefile && cdb rm <manifest>`; plan 02 adds the read path).

**Alternative split (rejected):**
- 01 = protocol primitives only (schema, magic, tests); 02 = all command integration.
- Rejected because 01 would be trivial and 02 huge. Better to split by user-visible capability.

**Confidence:** HIGH

---

## Focus Area 10: Pitfalls and Threat Model

### Pitfalls

**[SEV: HIGH] P-119-01: `CDAT_MAGIC` vs `CPAR_MAGIC` on blob.data outer layer vs inside envelope**
- **Issue:** CONTEXT.md reads as though the type prefix sits inside the encrypted envelope. But Phase 117's type index (TYPE-01) reads `blob.data[0..4]` on the NODE side, pre-decryption. Putting magics inside the envelope means `cdb ls --type CDAT` / `--type CPAR` cannot distinguish them.
- **Fix:** outer layout is `[CDAT or CPAR magic:4][CENV envelope]` on `blob.data`. The planner should confirm with user.
- **Impact if missed:** Phase 117 type filtering doesn't work for chunked-file types; `cdb ls --raw` shows all chunked-file blobs as CENV; orphan detection via type filter impossible; `rm` cascade needs extra probe to distinguish manifest from chunk.

**[SEV: HIGH] P-119-02: Mid-upload orphan chunks on crash**
- **Issue:** If cdb crashes after uploading 50/64 chunks, the node has 50 CDAT orphans in the user's namespace. CONTEXT.md D-10 "accept + retry" means the user just re-runs; the second run uploads FRESH chunks (ML-DSA-87 non-determinism = different hashes) + a fresh manifest. The first 50 remain orphaned forever.
- **Fix:** accept per D-10 but document clearly. `cdb gc` is deferred. **Verify that orphans don't cause the node to run out of disk.** 500 MiB of failed uploads leaves 500 MiB of orphans.
- **Impact:** namespace bloat on aborted uploads. Acceptable for first ship per CONTEXT.md.

**[SEV: MEDIUM] P-119-03: Partial download mid-stream tampering**
- **Issue:** If attacker-controlled node serves 63 of 64 valid chunks and then a subtly-corrupt chunk 64, the envelope decrypts and AEAD authenticates (the attacker has a valid chunk they legitimately authored), but the plaintext is "wrong". Per-chunk signature passes. Only `plaintext_sha3` catches it.
- **Fix:** D-04's `plaintext_sha3` IS the defense. Make sure it is actually computed and compared (not just declared in the schema). Test for it.
- **Impact:** without the check, defense-in-depth is broken; with it, CHUNK-05 is satisfied.

**[SEV: MEDIUM] P-119-04: Manifest decrypts to wrong structure, read arbitrary bytes**
- **Issue:** Malicious CPAR manifest could claim `segment_count = 2^32 - 1` → malloc 128 GiB for `chunk_hashes`. Or `total_plaintext_bytes = 2^63` → ftruncate 8 EiB.
- **Fix:** `decode_manifest_payload` MUST validate:
  - `segment_count <= MAX_CHUNKS` (suggest 65536, same as 1 TiB / 16 MiB).
  - `total_plaintext_bytes <= segment_count * chunk_size_bytes` and `>= (segment_count-1) * chunk_size_bytes + 1`.
  - `chunk_size_bytes` in a sane range (1 MiB to 256 MiB).
  - `chunk_hashes.size() == segment_count * 32`.
  - `plaintext_sha3.size() == 32`.
- **Impact:** without validation, malicious manifest = DoS on reader.

**[SEV: MEDIUM] P-119-05: TTL mismatch between chunks and manifest**
- **Issue:** If chunks have shorter TTL than manifest, chunks expire first; `cdb get` on a 6-day-old file with 7-day-TTL manifest fetches a live manifest and gets "blob not found" on expired chunks.
- **Fix:** chunk TTL == manifest TTL, set once at `cmd::put` time. Same `ttl` parameter applied to every signed Blob in the batch. Same `timestamp`.
- **Impact:** silent data loss if not enforced.

**[SEV: MEDIUM] P-119-06: `connection.cpp:685` unchecked `total_size` in chunked reassembly**
- **Issue:** STATE.md flags this. `total_size` is read from the chunked-framing header and passed to `vector::reserve(total_size)` at `connection.cpp:689`. A malicious node could send a header claiming `total_size = 2^64 - 1` → huge alloc.
- **Fix:** clamp `total_size` to `MAX_FRAME_SIZE = 110 MiB` (already defined at `connection.cpp:35`) before reserve. This is orthogonal to Phase 119 blob chunking — it's about the transport-level sub-frame reassembly. But STATE.md says "fix in Phase 119" so it's scoped here.
- **Impact:** without fix, malicious node = DoS.

**[SEV: LOW] P-119-07: Name collision between `Connection::send_chunked` (transport 1-MiB sub-frames) and "blob-level chunking" (CDAT+CPAR 16-MiB)**
- **Issue:** Two different "chunking" concepts live in the codebase. Cognitive confusion for readers.
- **Fix:** in Phase 119 code + comments, refer to the CDAT concept as "CDAT chunk" or "segment" (in the CHUNK-05 sense), never just "chunk". `Connection::send_chunked` keeps its name. Don't introduce a `send_chunked_blob` or similar.
- **Impact:** code-review clarity only.

**[SEV: LOW] P-119-08: 400 MiB threshold straddles single-blob boundary**
- **Issue:** After envelope encryption, a 400 MiB plaintext becomes ~400 MiB + 6.4 KiB (16 × 1-MiB-segment × 16-byte tag) + ~1648 B recipient stanzas + 20 B envelope header = ~400 MiB + 8 KiB. Plus the 4-byte outer type magic. Plus FlatBuffer overhead. Still well under 500 MiB.
- **Fix:** no fix needed — 400 MiB is intentionally below 500 MiB by CONTEXT.md D-02. Just document the margin calculation in the plan so the planner doesn't accidentally change the threshold to 450 MiB and tip over.
- **Impact:** planner awareness.

**[SEV: LOW] P-119-09: Recipient set change on "reshare" of a chunked blob**
- **Issue:** `cdb reshare` today operates on a single CENV blob (`commands.cpp:931`). For a chunked file, reshare needs to re-encrypt all N chunks + the manifest with the new recipient set.
- **Fix:** CONTEXT.md §Deferred Ideas correctly defers this. Phase 119 ships without reshare-of-chunked support. Document: `cdb reshare <manifest_hash>` should produce "reshare of a chunked file is not yet supported; see backlog".
- **Impact:** user surprise. Document clearly.

### Threat Model (T-119-01 through T-119-09)

Inherits all of Phase 120's T-120-01..T-120-13 unchanged (they're about the pipelining substrate). Phase 119 additions:

| ID | Threat | STRIDE | Mitigation |
|----|--------|--------|------------|
| T-119-01 | Attacker drops trailing chunks from a download | Tampering | Manifest's `segment_count` is ML-DSA-87 signed; reader receives full count; `recv` timeout on missing reply → error, no partial save |
| T-119-02 | Attacker substitutes a valid chunk from a different file | Tampering | Each chunk has ML-DSA-87 signature over `(ns || data || ttl || ts)`; a swapped chunk has a different signing_input → signature fails → reader rejects |
| T-119-03 | Attacker feeds a valid-looking but wrong chunk to exhaust `plaintext_sha3` | Tampering | Per-chunk envelope AEAD rejects forged ciphertext; this requires an authentic chunk by a different author at the same namespace — prevented by signature check |
| T-119-04 | Mid-download tampering of the output file by a concurrent local process | Tampering | Out of scope — local filesystem integrity is not part of chromatindb's threat model; user runs `cdb get` as themselves |
| T-119-05 | Orphaned CDAT chunks from aborted uploads fill the user's namespace | DoS (on own node) | D-10: accept + retry; deferred `cdb gc` if it becomes a problem |
| T-119-06 | Malicious manifest triggers OOM via oversized `segment_count` | DoS | `decode_manifest_payload` clamps `segment_count ≤ MAX_CHUNKS` (65536 = 1 TiB) |
| T-119-07 | Malicious manifest triggers huge `ftruncate` | DoS (local disk) | `decode_manifest_payload` clamps `total_plaintext_bytes ≤ MAX_CHUNKS × chunk_size_bytes` |
| T-119-08 | Manifest leaks filename / size to on-path observer | Information Disclosure | Manifest is CENV-envelope-encrypted; only the CPAR 4-byte outer magic leaks (P-119-01 tradeoff) |
| T-119-09 | Partial chunks visible to an on-path observer during upload reveal file size | Information Disclosure | All chunks fixed 16 MiB (last is padded to 16 MiB with zero-sized AEAD segments? No — last chunk is short). File size leaks as `segment_count × 16 MiB ± 16 MiB`. Accepted — CENV envelope leaks size anyway per existing protocol |

---

## Project Constraints (from project memory / `CLAUDE.md`-equivalent)

The project has no `CLAUDE.md` in the repo but user memory documents directives that apply to Phase 119:

- **No copy-paste of utilities.** Shared helpers into headers (`cli/src/` for CLI). → Put `Sha3Hasher`, manifest encoder, and chunked helpers in `cli/src/wire.{h,cpp}` and `cli/src/chunked.{h,cpp}`, not inline in commands.cpp.
- **No `|| true` to suppress errors.** → Every POSIX call (`open`, `pwrite`, `ftruncate`, `unlink`) checks return; on failure, log with spdlog and propagate.
- **`-j$(nproc)`, never `--parallel`** for cmake builds. → Planner should write verification commands using `-j$(nproc)`.
- **No backward compat.** → Manifest v1 is the only version. `version` field exists for future evolution but v1 is the hard floor; readers reject `version != 1`.
- **Honest product filter: "would I use this myself?"** → Write a real 10 GiB file to the live node and pull it back as part of Plan 02's checkpoint.
- **YAGNI.** → No `cdb put --chunk-size N` flag. No progress bar. No resume. All in Deferred Ideas.
- **No inefficient code / shortcuts.** → Use `pwrite` not `seekp`+`write`. Use incremental SHA3 not string-concat-then-hash. Release plaintext buffers eagerly.
- **Pick the right fix the first time.** → Address the `connection.cpp:685` unchecked `total_size` issue in this phase (flagged in STATE.md).

---

## Standard Stack

### Core (in use — [VERIFIED: `cli/CMakeLists.txt`])

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs | 0.15.0 | ML-DSA-87 sign, ML-KEM-1024, SHA3-256 | Only PQ-NIST library integrated; used across db/ and cli/ |
| libsodium | system pkg | ChaCha20-Poly1305 AEAD, HKDF-SHA256 | Already in envelope.cpp; paired with liboqs per project memory |
| FlatBuffers | 25.2.10 | Blob / TransportMessage wire format; CPAR Manifest | Already in use; hand-coded encoders in cli/src/wire.cpp |
| Standalone Asio | 1.38.0 | TCP + UDS sockets, coroutines in net/ | Not touched by this phase |
| spdlog | 1.15.1 | Logging | Already in use |
| nlohmann/json | 3.11.3 | Metadata JSON in build_put_payload (existing small-file path only) | Not used by new chunked paths |
| Catch2 | 3.7.1 | Test framework | Already in use for `cli_tests` |

### Supporting (stdlib only)

| Header | Purpose | Use in Phase 119 |
|--------|---------|------------------|
| `<fstream>` | Stream-read upload file | `std::ifstream` with `read()` into 16 MiB buffer |
| `<fcntl.h>`, `<unistd.h>` | POSIX `open` / `pwrite` / `ftruncate` / `close` / `unlink` | Download offset-writes |
| `<span>` | Pass plaintext chunks into `envelope::encrypt` | Existing pattern |
| `<array>`, `<vector>` | Fixed-32-byte hashes + variable chunk_hashes | Existing |
| `<unordered_map>` | `rid → chunk_index` inside `put_chunked` / `get_chunked` | Matches Phase 120-02 |

**No new libraries added.**

**Version verification:** [VERIFIED: `cli/CMakeLists.txt` FetchContent declarations, all tags pinned]. Versions are locked in `cli/CMakeLists.txt` and were stable as of last build (Phase 120 completion, 2026-04-19).

---

## Architecture Patterns

### System Data Flow — Upload

```
cdb put largefile.bin
       │
       ▼
  stat() → size >= 400 MiB? ──NO──► existing single-blob path (unchanged)
       │YES
       ▼
  put_chunked(conn, file_path, recipients, ttl):
       │
       │ segment_count = ceil(size / 16 MiB)
       │ timestamp     = now()
       │ rid           = next_rid
       │
       ▼
  For chunk_idx in [0 .. segment_count):
       │
       ├─► Read 16 MiB plaintext from disk (reusable buffer)
       ├─► sha3_hasher.absorb(plaintext)       ┐
       ├─► cenv = envelope::encrypt(plaintext) │  per-chunk
       ├─► blob_data = [CDAT_MAGIC:4] || cenv  │  work
       ├─► sig  = id.sign(signing_input(...))  │  (CPU-bound)
       ├─► blob_bytes = encode_blob({...})     ┘
       ├─► conn.send_async(Data, blob_bytes, this_rid)   ← backpressures at depth=8
       ├─► rid_to_chunk_idx[this_rid] = chunk_idx
       │
       │  Interleaved with drain (recv() arrival-order):
       ├─► resp = conn.recv()
       ├─► chunk_idx = rid_to_chunk_idx[resp.rid]
       ├─► chunk_hashes[chunk_idx] = resp.blob_hash
       ├─► stderr: "chunk N/total saved" (unless --quiet)
       │
       ▼
  plaintext_sha3 = sha3_hasher.finalize()
       │
       ▼
  Build Manifest { segment_count, total_bytes, plaintext_sha3, chunk_hashes, filename }
       │
       ▼
  manifest_cenv    = envelope::encrypt([CPAR||flatbuf], recipients)
  manifest_outer   = [CPAR_MAGIC:4] || manifest_cenv
  manifest_blob    = encode_blob({ns, pubkey, manifest_outer, ttl, timestamp, sign(...)})
       │
       ▼
  conn.send_async(Data, manifest_blob, this_rid)
  conn.recv()  → WriteAck contains manifest_hash
       │
       ▼
  stdout: "<manifest_hash>  <filename>"
```

### System Data Flow — Download

```
cdb get <hash>
       │
       ▼
  conn.send_async(ReadRequest, {ns, hash}, rid=1)
  resp = conn.recv()  → blob
       │
       ▼
  Inspect blob.data[0..4]:
       │
       ├─► CENV ──► existing path (envelope::decrypt → parse_put_payload → write)
       ├─► CDAT ──► error: "cannot fetch a chunk directly"
       │
       └─► CPAR
              │
              ▼
         envelope::decrypt(blob.data[4..]) → plaintext starts with CPAR magic
              │
              ▼
         decode_manifest_payload(plaintext) → ManifestData m
              │
              ▼
         open(out_path, O_WRONLY|O_CREAT|O_EXCL, 0644)
         ftruncate(fd, m.total_plaintext_bytes)
              │
              ▼
         For chunk_idx in [0 .. m.segment_count):  greedy-fill window
              │
              ├─► conn.send_async(ReadRequest, {ns, m.chunk_hashes[idx]}, rid)
              ├─► rid_to_chunk_idx[rid] = chunk_idx
              │
              │  Drain (arrival order):
              ├─► resp = conn.recv()
              ├─► chunk_idx = rid_to_chunk_idx[resp.rid]
              ├─► blob_data = resp.payload.blob.data
              ├─► assert blob_data[0..4] == CDAT_MAGIC
              ├─► cenv = blob_data[4..]
              ├─► plaintext = envelope::decrypt(cenv) → 16 MiB bytes
              ├─► pwrite(fd, plaintext, chunk_idx * m.chunk_size_bytes)
              │
              ▼
         close(fd)
              │
              ▼
         Re-open + read + Sha3Hasher → compare to m.plaintext_sha3
              │
              ├─► Match   ──► stderr "saved: <path> (<size>, <chunks> chunks)"
              └─► Mismatch ──► unlink(out_path) + stderr "integrity check failed"
```

### Recommended Project Structure

```
cli/src/
├── chunked.h          (NEW — ChunkStream, put_chunked / get_chunked / rm_chunked decls)
├── chunked.cpp        (NEW — helpers' implementation)
├── wire.h             (extend: CPAR_MAGIC, Sha3Hasher class, ManifestData, encode/decode_manifest_payload)
├── wire.cpp           (add: manifest_vt, encode_manifest_payload, decode_manifest_payload, Sha3Hasher)
├── commands.cpp       (modify: cmd::put branch, cmd::get branch, cmd::rm branch)
├── main.cpp           (modify: --type CPAR acceptance in ls)
├── connection.cpp     (modify: clamp total_size in recv chunked-reassembly — STATE.md pitfall)
└── envelope.{h,cpp}   (unchanged — reuse encrypt/decrypt as-is)

db/schemas/
└── manifest.fbs       (NEW — for documentation / future node use; no code-gen path yet)

cli/tests/
├── test_manifest.cpp          (NEW — [manifest] tests)
├── test_chunked.cpp           (NEW — [chunked] tests)
└── CMakeLists.txt             (modify: add new test files)
```

### Patterns

**Pattern 1: Two-phase greedy-fill + arrival-order drain**
- **What:** Same shape as Phase 120-02's `cmd::put` / `cmd::get`.
- **When:** Any batch of N items over one PQ connection where items are independent.
- **Example:** [VERIFIED: `cli/src/commands.cpp:559-642`]

**Pattern 2: Hand-coded FlatBuffers encoding with named VTable offsets**
- **What:** `namespace {table_name}_vt { constexpr flatbuffers::voffset_t FIELD = N; }` + `FlatBufferBuilder` + `StartTable`/`EndTable`.
- **When:** Encoding CLI-side wire shapes without a `flatc` dependency.
- **Example:** [VERIFIED: `cli/src/wire.cpp:24-31` for `Blob`, lines 16-20 for `TransportMessage`]

**Pattern 3: RAII wrapper over C library resource**
- **What:** `struct Impl` PIMPL in `.cpp` hides C headers.
- **When:** Wrapping liboqs incremental state.
- **Example:** new `Sha3Hasher` — liboqs `OQS_SHA3_sha3_256_inc_ctx` is a plain struct, `_release` must be called.

### Anti-patterns to Avoid

- **Buffering the whole file for upload.** `std::vector<uint8_t> all_bytes = read_file_bytes(path)` is wrong for chunked — violates CHUNK-01.
- **Buffering all downloaded chunks in memory.** Hold at most `kPipelineDepth` in flight; `pwrite` to disk immediately after decrypt.
- **Computing `plaintext_sha3` over ciphertext.** The field name says "plaintext" — hash the decrypted bytes, not the envelope. Otherwise a valid alternate envelope for the same plaintext would produce a different hash.
- **Re-signing the same data repeatedly.** Each chunk is a distinct Blob — one sign per chunk. Don't attempt to "bulk sign" by signing once and copying — that's protocol abuse.
- **Using transport-level `send_chunked` directly.** That's for payloads ≥ 1 MiB; `send_async` already dispatches to it internally. Callers of `send_async` never invoke `send_chunked` themselves.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Streaming SHA3-256 | Custom incremental hasher, sponge implementation | `OQS_SHA3_sha3_256_inc_*` (wrap in `Sha3Hasher` class) | Already used correctly at `wire.cpp:229-245` |
| Chunk encryption | Hand-rolled AEAD loop | `envelope::encrypt` (per chunk) | Handles recipient stanzas + segmented AEAD correctly; security-critical |
| Signed Blob construction | Manual FlatBuffer bytes | `encode_blob(BlobData)` + `build_signing_input` + `id.sign()` | Already used correctly in `cmd::put` small-file path |
| Request pipelining / backpressure | New threads, async runtime, condvar | `Connection::send_async` | Phase 120 shipped this; reuse |
| rid correlation | Stateful rid allocator on Connection | Batch-local `uint32_t rid = 1` + `rid_to_index` map | Phase 120-02 pattern; verified at `commands.cpp:554, 669` |
| Progress reporting | Curses TTY detection, progress bar | One-line-per-chunk stderr output, `opts.quiet` gated | Matches existing D-07 / D-08 convention |

**Key insight:** everything Phase 119 needs is already assembled. The phase is plumbing, not cryptography or protocol design.

---

## Runtime State Inventory

| Category | Items Found | Action Required |
|----------|-------------|------------------|
| Stored data | None — all state is blobs on the node (signed, verifiable) | None |
| Live service config | None — no new config keys | None |
| OS-registered state | None | None |
| Secrets / env vars | None — existing identity files cover all crypto needs | None |
| Build artifacts | `cli/build/` binaries will be rebuilt; no stale artifacts that embed the old "no chunked support" version | Run `cmake --build build --target cdb -j$(nproc)` |

**Nothing found in most categories — verified by direct grep of `cli/` and `db/` for `chunk`, `CDAT`, `CPAR`, `manifest`.**

---

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Live test node @ 192.168.1.73 | Plan 02 live-node checkpoint | [VERIFIED: Phase 120 used it 2026-04-19] | — | Run local `chromatindb` on random port |
| `cmake` | Build | Assumed ✓ | — | — |
| `flatc` (for node `manifest.fbs` generation) | Node build only | Assumed ✓ (already builds blob.fbs) | — | Skip manifest.fbs code-gen; CLI uses hand-coded encoder regardless |
| C++20 compiler | Build | [VERIFIED: `cmake_minimum_required 3.20`, `cxx_std_20`] | — | — |
| POSIX `pwrite`, `ftruncate`, `unlink` | Download path | Linux yes; macOS yes | POSIX.1-2001 | None needed |
| liboqs, libsodium, FlatBuffers, Asio, spdlog, nlohmann_json, Catch2 | Build | [VERIFIED: `cli/CMakeLists.txt`] | pinned | None |

**Missing dependencies with no fallback:** None.
**Missing dependencies with fallback:** None.

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | `cli/CMakeLists.txt` (FetchContent) + `cli/tests/CMakeLists.txt` (executable + discovery) |
| Quick run command | `ctest --test-dir build -R '\[chunked\]\|\[manifest\]' --output-on-failure` |
| Full suite command | `ctest --test-dir build --output-on-failure` |
| Build command | `cmake --build build --target cli_tests -j$(nproc)` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CHUNK-01 | Upload > 500 MiB w/o full-memory buffering | unit + live integration | `ctest -R '\[chunked\].*stream' -x` + Plan 02 live-node checkpoint | ❌ Wave 0 (new test file) |
| CHUNK-02 | Split into CDAT + CPAR | unit | `ctest -R '\[chunked\].*split' -x` | ❌ Wave 0 |
| CHUNK-02 | CPAR manifest round-trip encode/decode | unit | `ctest -R '\[manifest\]' -x` | ❌ Wave 0 |
| CHUNK-03 | Download reassembles from CPAR | unit + live | `ctest -R '\[chunked\].*reassemble' -x` + live checkpoint | ❌ Wave 0 |
| CHUNK-04 | `cdb rm` of manifest cascades | unit + live | `ctest -R '\[chunked\].*cascade' -x` + live checkpoint | ❌ Wave 0 |
| CHUNK-05 | `segment_count` prevents truncation; `plaintext_sha3` catches tampering | unit | `ctest -R '\[manifest\].*truncate\|sha3_mismatch'` | ❌ Wave 0 |

### Sampling Rate

- **Per task commit:** `ctest --test-dir build -R '\[chunked\]\|\[manifest\]' --output-on-failure` (targeted, ~1 s)
- **Per wave merge:** `ctest --test-dir build --output-on-failure` (full 49 + new tests, ~5 s)
- **Phase gate:** Full suite green + live-node checkpoint passed before `/gsd-verify-work`

### Wave 0 Gaps

- [ ] `cli/tests/test_manifest.cpp` — covers CHUNK-02 (encode/decode) and CHUNK-05 (validation)
- [ ] `cli/tests/test_chunked.cpp` — covers CHUNK-01, CHUNK-03, CHUNK-04
- [ ] `cli/tests/CMakeLists.txt` — add the two new test files
- [ ] Fixture: a 400-KiB scratch file plus a mocked "stat() returns 400 MiB" seam for CHUNK-02's split-threshold test (don't actually write 400 MiB to disk in CI; either sparse file or parameterize the threshold for testing)

---

## Security Domain

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | yes | PQ handshake + ML-DSA-87 identity (established protocol; Phase 119 doesn't change auth) |
| V3 Session Management | yes | Single connection per `cdb put`/`get`; AEAD counter mode; unchanged |
| V4 Access Control | yes | Writer owns namespace (ML-DSA-87 key); unchanged |
| V5 Input Validation | yes | `decode_manifest_payload` validates sizes, ranges — P-119-04 mitigation |
| V6 Cryptography | yes | Reuses liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256) + libsodium (ChaCha20-Poly1305). No hand-rolled crypto. |

### Known Threat Patterns for C++20 / PQ-crypto CLI

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Buffer overflow in decode_manifest_payload | Tampering | Bounds-check every size field; reject on failure (clamp) |
| Integer overflow in `chunk_idx * chunk_size_bytes` | Tampering | Use `uint64_t` for the offset computation; validate total < MAX_FILE_CAP |
| AEAD nonce reuse on chunked send | Tampering | [VERIFIED] `Connection::send_counter_` is the sole nonce source; `send_async` serializes through the existing single-sender queue. Preserved. |
| Truncation attack on manifest | Tampering | `segment_count` ML-DSA-87-signed; missing chunks cause fetch failure, not silent truncation |
| Chunk substitution | Tampering | Per-chunk ML-DSA-87 signature |
| Chosen-chunk attack | Tampering | `plaintext_sha3` catches mis-ordered or swapped plaintext |

---

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | ML-DSA-87 sign cost is 1-5 ms on typical hardware | Focus Area 6 | If 10x higher, 10 GiB upload blocks 30s on CPU before first chunk ships — planner may want explicit "preparing…" messaging |
| A2 | User wants node-visible blob type for CDAT/CPAR (outer magic) | Focus Area 4 | If wrong, revert to inner-magic design; `cdb ls --type CDAT` becomes unavailable but security equivalent |
| A3 | `pwrite` is available on all target platforms | Focus Area 3 | macOS untested in session; Linux confirmed. If macOS fails, use `seekp`/`write` fallback |
| A4 | 1 TiB is a reasonable max file size cap | Focus Area 2 | If users want >1 TiB, raise MAX_CHUNKS and MAX_FILE_CAP; 1 TiB fits in `uint32_t segment_count` (65536 @ 16 MiB) |
| A5 | Post-write re-read for `plaintext_sha3` is acceptable (adds ~10s to a 10 GiB download) | Focus Area 5 | If throughput-critical, switch to in-order absorb gate; more code but no extra read |
| A6 | STATE.md "connection.cpp:626 unchecked total_size" should be fixed in Phase 119 | Pitfalls P-119-06 | If fixed elsewhere first, skip it in 119 |
| A7 | The 16 MiB chunk size (D-01) is final — chunk_size_bytes in manifest is documentation only for v1 | Focus Area 1 | If planner wants to expose a flag, add `--chunk-size` but YAGNI per CONTEXT |

**Tagged `[ASSUMED]` claims that need user confirmation:**
- **A2 is the most important** — the research recommends a small deviation from the implicit reading of CONTEXT.md's D-06 (type prefix location). Planner should raise this at plan-kickoff, or the planner+discuss-phase user MUST decide before Task 1.

---

## Open Questions

1. **Outer vs inner CDAT/CPAR magic (A2).**
   - What we know: CONTEXT.md D-06 reads "after decrypting the blob's payload, check type prefix" which implies inner magic. Phase 117's type index operates on `blob.data[0..4]` pre-decrypt.
   - What's unclear: user intent — hide chunked-file types from on-path observer (inner magic, can't use `ls --type CDAT`) vs expose them for filtering (outer magic, small metadata leak).
   - Recommendation: outer magic (filter-friendly; metadata leak equivalent to existing PUBK/DLGT).

2. **Max file size cap (A4).**
   - What we know: CHUNK-01 says >500 MiB. No upper bound in requirements.
   - What's unclear: whether to cap at 1 TiB, 10 TiB, or nothing.
   - Recommendation: 1 TiB for Phase 119. Phase follow-up can raise.

3. **Retry policy for a single chunk's transient failure (CONTEXT.md §Claude's Discretion).**
   - What we know: CONTEXT.md says "today there's none — one bad chunk fails the whole batch".
   - What's unclear: whether to add a 1-retry-per-chunk budget.
   - Recommendation: no retry for v1. Every chunk is signed non-deterministically — a retry produces a different blob_hash and would require updating the manifest post-build, which is complexity. Re-run the whole upload if it fails.

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Sequential multi-blob fetches | Pipelined via `send_async` + arrival-order drain | Phase 120 (2026-04-19) | Chunked download uses this directly — no sequential fetch loops |
| Single-file up to 500 MiB | Single-file up to 400 MiB, chunked beyond | Phase 119 (this) | Clear boundary; 400 MiB stays on single-blob path |
| `Blob.data` type visible only after envelope decrypt | 4-byte type prefix indexed server-side at ingest (TYPE-01) | Phase 117 (2026-04-16) | CDAT/CPAR slot into the existing index mechanism (outer magic per A2) |

**Deprecated / outdated:**
- The `read_file_bytes` whole-file-to-`vector` approach remains valid for the small-file path; NOT used by the chunked path.
- No deprecations — Phase 119 is additive.

---

## Sources

### Primary (HIGH confidence — direct file reads)

- `cli/src/wire.h` — `CDAT_MAGIC`, `type_label`, `is_hidden_type`, `sha3_256` declaration, Blob/Transport encode declarations
- `cli/src/wire.cpp` — hand-coded FlatBuffers encoders, `build_signing_input` incremental SHA3 pattern, `sha3_256` one-shot
- `cli/src/connection.h` + `connection.cpp` — `send_async`, `recv`, `send_chunked` transport framing, `kPipelineDepth`, `pending_replies_`
- `cli/src/envelope.h` + `envelope.cpp` — `encrypt` / `decrypt` recipient-set mechanism; CENV magic at `envelope.cpp:30`; SEGMENT_SIZE = 1 MiB at line 35
- `cli/src/commands.cpp` — `cmd::put` / `cmd::get` / `cmd::rm` current shape; `build_put_payload` + `parse_put_payload`; MAX_FILE_SIZE = 500 MiB at line 514
- `cli/src/identity.h` — `Identity::sign`, key sizes
- `cli/CMakeLists.txt` — dependency versions, test setup
- `cli/tests/CMakeLists.txt` — Catch2 test layout
- `db/schemas/blob.fbs` — canonical Blob schema for mirroring
- `db/schemas/transport.fbs` — TransportMsgType enum
- `db/CMakeLists.txt` — existing `flatc` invocation pattern
- `db/net/framing.h` — `MAX_BLOB_DATA_SIZE = 500 MiB`
- `.planning/phases/117-blob-type-indexing-ls-filtering/117-CONTEXT.md` — Phase 117 decisions D-10, D-13
- `.planning/phases/120-request-pipelining/120-CONTEXT.md` — Phase 120 D-01..D-09 pipelining primitives
- `.planning/phases/120-request-pipelining/120-01-SUMMARY.md` — `pipeline_pump.h`, `pending_replies_` surface
- `.planning/phases/120-request-pipelining/120-02-SUMMARY.md` — `cmd::put` / `cmd::get` two-phase pattern at `commands.cpp:554, 669`
- `.planning/STATE.md` — `connection.cpp:626 unchecked total_size in chunked reassembly` pitfall
- `.planning/REQUIREMENTS.md` — CHUNK-01..CHUNK-05
- `.planning/ROADMAP.md` — Phase 119 success criteria + dependency on Phase 120

### Secondary (MEDIUM confidence — inferred from codebase patterns, not direct verification)

- liboqs 0.15.0 ML-DSA-87 sign cost estimate (A1 is ASSUMED)
- macOS `pwrite` portability (A3 is ASSUMED)

### Tertiary (LOW confidence — no sources consulted)

None — every claim in this research is traceable to a file in the repo or a directly-read CONTEXT.md entry. No external web sources were needed; the phase is pure internal plumbing.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all libraries read directly from CMakeLists.txt with pinned versions
- Architecture: HIGH — mirrors existing Phase 120 pattern verified in commands.cpp
- Pitfalls: HIGH for in-code items (verified by direct grep); MEDIUM for A2 (design choice about node-visible type) — needs user confirmation
- Manifest schema: HIGH — mirrors existing Blob schema

**Research date:** 2026-04-18
**Valid until:** 2026-05-18 (30 days — codebase is stable at Phase 120 completion; no upcoming ecosystem changes to liboqs / Asio / FlatBuffers that would affect this phase)

---

*Phase: 119-chunked-large-files*
*Research gathered: 2026-04-18*
