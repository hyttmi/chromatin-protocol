# Phase 119: Chunked Large Files - Pattern Map

**Mapped:** 2026-04-19
**Files analyzed:** 7 (2 new source, 1 new test, 1 new schema, 3 modified)
**Analogs found:** 7 / 7 (one schema is greenfield-ish, see Gaps)

---

## File Classification

| New/Modified File                          | Role                | Data Flow                   | Closest Analog                                | Match Quality                   |
|--------------------------------------------|---------------------|-----------------------------|-----------------------------------------------|---------------------------------|
| **NEW** `cli/src/chunked.h`                | helper header       | request-response / batch    | `cli/src/pipeline_pump.h`                     | exact (header-only free funcs)  |
| **NEW** `cli/src/chunked.cpp`              | helper impl         | CRUD / batch pipelined I/O  | `cli/src/commands.cpp` (cmd::put/get bodies)  | exact (two-phase pump shape)    |
| **NEW** `cli/schemas/manifest.fbs` *(see Gap 1 for location)* | schema | serialization | `db/schemas/blob.fbs`                      | role-match (new namespace)      |
| **NEW** `cli/tests/test_chunked.cpp`       | unit test           | n/a                         | `cli/tests/test_connection_pipelining.cpp`    | exact (freshest Catch2 analog)  |
| **MOD** `cli/src/wire.h`                   | constants + helpers | n/a                         | existing `CDAT_MAGIC` / `type_label` block    | exact (same file, add 1 line each) |
| **MOD** `cli/src/commands.cpp`             | commands            | CRUD                        | itself: `cmd::put` / `cmd::get` / `cmd::rm`   | exact (add branch at top)       |
| **MOD** `cli/src/main.cpp`                 | arg parse           | n/a                         | ls `--type` handling (lines 637-642)          | exact (extend string list)      |
| **MOD** `cli/tests/CMakeLists.txt`         | build config        | n/a                         | itself                                        | exact (append line)             |

---

## Pattern Assignments

### `cli/src/chunked.h` — NEW (helper header)

**Analog:** `cli/src/pipeline_pump.h` (the freshest header-only helper in the same layer, created in Phase 120-01)

**Mirror exactly:**

1. **File header preamble** — `#pragma once`, includes relative to repo root (`#include "cli/src/wire.h"`), narrow std includes only.
2. **Namespace:** nested sub-namespace under `chromatindb::cli`. Pipeline helpers live in `chromatindb::cli::pipeline`. Chunked helpers should live in `chromatindb::cli::chunked` for symmetry — NOT in `chromatindb::cli::cmd` (that namespace is for user-facing commands in `commands.h`).
3. **Doc comment style** — `// =========...\n// Section name\n// =========...` dividers, Contracts block at the top of the namespace explaining invariants (single-sender, single-reader, bounded memory).
4. **Free functions, not classes.** `pipeline_pump.h` exposes two free templates (`pump_one_for_backpressure`, `pump_recv_for`) that take references to the caller's state. `chunked.h` should expose `build_manifest`, `reassemble`, `put_chunked`, `get_chunked`, `rm_chunked` the same way — no helper class, no state on the helper itself. All state (connection, identity, recipient_spans, rid counter, rid_to_index map) is passed by reference from the caller in `commands.cpp`.
5. **Template the pipelining source** *only if* you extract the pump-loop into a helper (optional — chunked can just reuse the two-phase pattern inline in `chunked.cpp`). If templated, follow the `Source&& source` convention from `pump_recv_for`.

**Concrete excerpt — include/namespace/preamble to copy verbatim in style** (`cli/src/pipeline_pump.h` lines 1-38):

```cpp
#pragma once

#include "cli/src/wire.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

namespace chromatindb::cli::pipeline {

// =============================================================================
// Pipelining pump helpers (header-only, single-threaded)
// =============================================================================
//
// These free functions contain the pure routing logic behind
// Connection::send_async and Connection::recv_for. Extracting them into a
// header-only namespace makes the logic testable without instantiating a real
// Connection (which requires a socket and a completed PQ handshake).
```

**Anti-pattern (explicit NO):** Do NOT declare `class ChunkedUploader { ... };`. The project has zero "helper classes with mutable state" in the CLI layer — everything is free functions + POD structs.

---

### `cli/src/chunked.cpp` — NEW (helper implementation)

**Analog:** `cli/src/commands.cpp` lines 540-642 (`cmd::put` greedy-fill + arrival-order drain loop) and lines 664-813 (`cmd::get` symmetric pattern).

**Mirror exactly — the two-phase shape:**

Phase 120-02 locked this in. Reuse verbatim for `put_chunked` (CDAT fan-out) and `get_chunked` (CDAT fan-in):

```cpp
// Phase A: greedy fill the window
if (next_to_send < N && rid_to_index.size() < Connection::kPipelineDepth) {
    // ... build blob, sign, send_async ...
    rid_to_index[this_rid] = next_to_send;
    ++next_to_send;
    continue;  // keep filling before draining
}

// Phase B: drain one reply in arrival order
auto resp = conn.recv();
if (!resp) { /* transport dead: flush pending to errors, break */ }
auto it = rid_to_index.find(resp->request_id);
if (it == rid_to_index.end()) {
    spdlog::debug("discarding reply for unknown rid {}", resp->request_id);
    continue;
}
// ... process, erase, ++completed ...
```

Reference: `cli/src/commands.cpp:559-642` (put) and `cli/src/commands.cpp:675-809` (get).

**Imports to mirror** (`cli/src/commands.cpp:1-20`):

```cpp
#include "cli/src/chunked.h"
#include "cli/src/connection.h"
#include "cli/src/envelope.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <nlohmann/json.hpp>    // only if you need JSON metadata
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
```

**Per-blob construction pattern** — copy `cmd::put` lines 567-582:

```cpp
auto payload = build_put_payload(/*name or empty*/, chunk_bytes);
auto envelope_data = envelope::encrypt(payload, recipient_spans);
auto timestamp = static_cast<uint64_t>(std::time(nullptr));
auto signing_input = build_signing_input(ns, envelope_data, ttl, timestamp);
auto signature = id.sign(signing_input);

BlobData blob{};
std::memcpy(blob.namespace_id.data(), ns.data(), 32);
blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
blob.data = std::move(envelope_data);
blob.ttl = ttl;
blob.timestamp = timestamp;
blob.signature = std::move(signature);

auto flatbuf = encode_blob(blob);
// conn.send_async(MsgType::Data, flatbuf, this_rid);
```

**Important deviation for CPAR (D-13 outer magic):** chunk plaintext is the raw 16 MiB file slice, but the BLOB's `data` field after envelope-encrypting must have the CDAT magic prepended **outside** the CENV envelope:

```cpp
std::vector<uint8_t> blob_data;
blob_data.reserve(4 + envelope_data.size());
blob_data.insert(blob_data.end(), CDAT_MAGIC.begin(), CDAT_MAGIC.end());
blob_data.insert(blob_data.end(), envelope_data.begin(), envelope_data.end());
blob.data = std::move(blob_data);
```

Same shape for CPAR manifest — `CPAR_MAGIC` + envelope-encrypted FlatBuffers `Manifest` bytes.

**Streaming file read pattern (D-11) — no direct analog; reference for style:**
`cli/src/commands.cpp:30-36` (`read_file_bytes`) reads whole file. For chunked, replace with `std::ifstream` + `f.read(buf.data(), CHUNK_SIZE)` loop. Buffer ownership stays on the caller's stack to keep working memory bounded at `kPipelineDepth * CHUNK_SIZE`.

**Error reporting style** — `std::fprintf(stderr, "Error: ...")` verbatim. Examples at `commands.cpp:502, 519, 527, 531, 586, 605, 630, 685, 699, 716`. Never use `spdlog::error` for user-facing errors — debug-level only for dropped-rid / discarded-reply scenarios (`commands.cpp:618, 727`).

**Progress line (D-07)** — mirror `commands.cpp:805-807` `saved:` pattern but per-chunk:

```cpp
if (!opts.quiet) {
    std::fprintf(stderr, "chunk %zu/%zu saved\n", completed, total);
}
```

Final line (after all chunks), matching `commands.cpp:806`:

```cpp
if (!opts.quiet) {
    std::fprintf(stderr, "saved: %s (%zu bytes, %zu chunks)\n",
                 out_path.c_str(), total_bytes, chunk_count);
}
```

**Per-chunk retry (D-15)** — **NO existing analog.** Current code has zero retry logic (one transport error = one error++). Planner must design this fresh. Suggested: wrap the `send_async` + `recv_for(this_rid)` exchange in a `for (int attempt = 0; attempt < 3; ++attempt)` loop with `std::this_thread::sleep_for(std::chrono::milliseconds(250 << attempt))` between attempts. Fresh sign per attempt (ML-DSA-87 is non-deterministic → new hash) — manifest builder binds `chunk_index → final_hash` at the drain site (post-WriteAck), NOT at send time.

---

### `cli/schemas/manifest.fbs` — NEW (or wherever Blob schema lives, see Gap 1)

**Analog:** `db/schemas/blob.fbs` (9 lines, minimal).

**Mirror:**

1. **Namespace:** `namespace chromatindb.wire;` — identical to `blob.fbs:1`. Do not invent a new wire namespace.
2. **Table comment style** — inline `// 32 bytes: SHA3-256(...)` after each field describing the semantic size. See `blob.fbs:3-9`.
3. **`root_type`** — declare at bottom, e.g. `root_type Manifest;`
4. **Field types:** prefer `ubyte` / `uint32` / `uint64` (as in `blob.fbs`), NOT `byte` / `int32`. `TransportMsgType: byte` in `transport.fbs` is only for the enum discriminant.

**Concrete template (style-match `db/schemas/blob.fbs`):**

```fbs
namespace chromatindb.wire;

table Manifest {
  chunk_size_bytes:uint32;     // plaintext bytes per CDAT chunk
  segment_count:uint32;        // number of CDAT chunks
  total_plaintext_bytes:uint64;  // full reassembled size
  plaintext_sha3:[ubyte];      // 32 bytes: SHA3-256 of reassembled plaintext
  chunk_hashes:[ubyte];        // N*32 bytes: flat concat of chunk SHA3-256 hashes
  filename:[ubyte];            // original filename bytes (optional, can be empty)
}

root_type Manifest;
```

**Flat-vs-nested design note:** `blob.fbs` uses all `[ubyte]` vectors (no nested tables). The CLI's hand-coded encoder (`cli/src/wire.cpp:155-175`) depends on this simplicity. Follow suit: prefer a flat `[ubyte]` concat of 32-byte hashes over `[[ubyte]]` or a nested `ChunkRef` table. This keeps the hand-coded encoder a 6-field `StartTable`/`AddOffset` sequence, mirroring `encode_blob` line-for-line. `[[ubyte]]` would require a more complex CreateVectorOfStrings-style encode.

**CMake flatc invocation (Gap 1 decision affects this):**
- Node already auto-generates via `db/CMakeLists.txt:127-137` (`add_custom_command` + `flatc --cpp --gen-object-api`).
- CLI does **NOT** use flatc-generated headers — it hand-codes vtable offsets in `cli/src/wire.cpp:22-31` (`namespace blob_vt`). So the CLI can either:
  - (a) Put schema in `cli/schemas/manifest.fbs`, NO CMake flatc invocation needed, hand-code the `manifest_vt` offsets alongside `blob_vt`. **Recommended — matches existing CLI pattern.**
  - (b) Put schema in `db/schemas/manifest.fbs`, let node's CMake generate `manifest_generated.h`, ignore the generated header on the CLI side and hand-code anyway.

**Mirror the hand-coded VTable style** (`cli/src/wire.cpp:22-31`):

```cpp
// Manifest: table { chunk_size_bytes:uint32; segment_count:uint32; total_plaintext_bytes:uint64;
//                   plaintext_sha3:[ubyte]; chunk_hashes:[ubyte]; filename:[ubyte]; }
// VTable field offsets: 4, 6, 8, 10, 12, 14
namespace manifest_vt {
    constexpr flatbuffers::voffset_t CHUNK_SIZE_BYTES      = 4;
    constexpr flatbuffers::voffset_t SEGMENT_COUNT         = 6;
    constexpr flatbuffers::voffset_t TOTAL_PLAINTEXT_BYTES = 8;
    constexpr flatbuffers::voffset_t PLAINTEXT_SHA3        = 10;
    constexpr flatbuffers::voffset_t CHUNK_HASHES          = 12;
    constexpr flatbuffers::voffset_t FILENAME              = 14;
}
```

**Disambiguation (CONTEXT D-04 warning):** `Manifest.segment_count` here counts INTER-blob CDAT chunks. `envelope.cpp:210` has an internal `segment_count` counting INTRA-blob AEAD segments (1 MiB each). Do NOT rename either — but doc-comment the manifest field as `// number of CDAT chunks (NOT envelope AEAD segments)` to head off the confusion.

---

### `cli/tests/test_chunked.cpp` — NEW (Catch2 unit tests)

**Analog:** `cli/tests/test_connection_pipelining.cpp` (freshest, created Phase 120-01) and `cli/tests/test_wire.cpp` (the schema-roundtrip analog).

**Mirror exactly:**

1. **Includes** (copy `test_connection_pipelining.cpp:1-10`):
   ```cpp
   #include <catch2/catch_test_macros.hpp>
   #include "cli/src/chunked.h"
   #include "cli/src/wire.h"

   #include <deque>
   #include <optional>
   #include <unordered_map>
   #include <vector>
   ```

2. **Using-directive** (copy `test_connection_pipelining.cpp:12`):
   ```cpp
   using namespace chromatindb::cli;
   ```
   And if you use envelope helpers: `using namespace chromatindb::cli::envelope;` (see `test_envelope.cpp:11-12`).

3. **Test tag convention** — `[chunked]` or `[cdat]`. Existing tags from test files:
   - `[wire]` — `test_wire.cpp`
   - `[envelope]` — `test_envelope.cpp`
   - `[pipeline]` — `test_connection_pipelining.cpp`
   - `[identity]` — `test_identity.cpp`
   - `[contacts]` — `test_contacts.cpp`
   
   **Pick `[chunked]`** for symmetry (noun, one word, lowercase).

4. **Test case titles** — `"chunked: <scenario>", "[chunked]"` format, matching `test_connection_pipelining.cpp:49, 62, 94, 109, 121, 134, 143, 161`:
   ```cpp
   TEST_CASE("chunked: build_manifest produces correct segment_count", "[chunked]") { ... }
   TEST_CASE("chunked: reassemble rejects truncated manifest", "[chunked]") { ... }
   TEST_CASE("chunked: reassemble rejects plaintext_sha3 mismatch", "[chunked]") { ... }
   ```

5. **Fixture pattern** — use an anonymous namespace with small local factory functions / POD fixtures. See `test_connection_pipelining.cpp:14-45` (`ScriptedSource`, `make_reply`). For chunked, you'll likely want `make_fake_chunk(size_t idx, size_t size)` and a `ScriptedConnection` or just drive `build_manifest` + `reassemble` with in-memory vectors (no network needed).

6. **Assertion style** — `REQUIRE(...)`, not `CHECK`. See `test_connection_pipelining.cpp:56-59`, `test_wire.cpp:15-18`. The project uses REQUIRE exclusively.

7. **No testing of network layer** — like `test_connection_pipelining.cpp`, drive the chunked helpers with **scripted sources / in-memory buffers**, not a live `Connection`. The `Connection` class requires a PQ handshake and a real socket; that's why the pipeline tests extract the pump logic into `pipeline_pump.h`. If `chunked.h` similarly exposes pure functions (`build_manifest`, `reassemble_chunks_to_file`), they're testable without a socket.

---

### `cli/src/wire.h` — MOD (add CPAR_MAGIC + update helpers)

**Analog:** itself, lines 217-238 (the existing CDAT block added in earlier Phase 119 work — yes, some scaffolding is already here).

**Terminology reconciliation (IMPORTANT):** Context doc references `extract_blob_type_name` and `is_infrastructure_type`. **These names do not exist in the codebase.** The actual functions are:
- `type_label(const uint8_t* type)` — `cli/src/wire.h:222-229`
- `is_hidden_type(const uint8_t* type)` — `cli/src/wire.h:233-238`

Planner must use the real names. If the planner wants to rename them to match the context doc's naming, that's a separate refactor — do it in its own plan step if at all.

**Mirror the 3-line add pattern already used for CDAT** (`wire.h:217-218, 227, 235`):

```cpp
/// CPAR (chunked manifest) magic: "CPAR" in ASCII (Phase 119)
inline constexpr std::array<uint8_t, 4> CPAR_MAGIC = {0x43, 0x50, 0x41, 0x52};

// ... then add ONE line to type_label():
    if (std::memcmp(type, CPAR_MAGIC.data(), 4) == 0) return "CPAR";

// ... and ONE line to is_hidden_type() IF CPAR should be hidden from default ls.
// D-13 decision: CPAR is the user-visible entrypoint for chunked files,
// so it should NOT be hidden (user needs to `cdb get <manifest_hash>`).
// Confirm with planner — context does not explicitly state CPAR hide status.
```

**Decision to surface to planner:** Is CPAR hidden from default `cdb ls`? CDAT (chunks) is hidden — user never directly addresses them. CPAR is the user-facing handle, so it should be **visible by default**. Context doc doesn't state this explicitly; planner must decide and document in the PLAN.md.

---

### `cli/src/commands.cpp` — MOD (put/get/rm branching)

**Analog:** itself, lines 482-646 (`cmd::put`), 652-813 (`cmd::get`), 819-925 (`cmd::rm`).

**Mirror:**

1. **Size-check branch at top of `cmd::put`** — insert after line 514 (`MAX_FILE_SIZE` check). For each file in the loop at 524:
   ```cpp
   if (fsize >= CHUNK_THRESHOLD) {  // 400 MiB per D-02
       int rc = chunked::put_chunked(id, ns, recipient_spans, fp, fname, ttl, conn, opts);
       if (rc != 0) ++errors;
       continue;  // skip the single-blob path
   }
   ```
   Reject > 1 TiB (D-14) in the same stanza:
   ```cpp
   if (fsize > MAX_CHUNKED_FILE_SIZE) {
       std::fprintf(stderr, "Error: %s too large (%.2f GiB, max 1 TiB)\n", ...);
       return 1;
   }
   ```

2. **Type-prefix check in `cmd::get`** — after decrypting `plaintext` at line 767, check the 4-byte prefix:
   ```cpp
   if (plaintext.size() >= 4 &&
       std::memcmp(plaintext.data(), CPAR_MAGIC.data(), 4) == 0) {
       int rc = chunked::get_chunked(id, ns, plaintext, output_dir, force_overwrite, conn, opts);
       if (rc != 0) ++errors;
       continue;
   }
   // existing parse_put_payload path unchanged
   ```
   *But D-13 says the CPAR magic is OUTER (in `blob.data` before envelope decryption), not inner. Confirm with planner — the outer-magic placement means the check is on `blob->data[0..4]`, BEFORE envelope::decrypt. This affects the branch location in the function.*

3. **Type-prefix check in `cmd::rm`** — after existing `ExistsRequest` probe (line 834-860), add a ReadRequest + decrypt + type-check before the Delete. If CPAR, hand to `chunked::rm_chunked`. Otherwise continue existing path.

4. **Error counter pattern** — keep `++errors; continue;` for per-file/per-chunk errors. Chunk-level errors inside `put_chunked` should NOT propagate as `++errors` per chunk — per CONTEXT.md:295-297, chunked upload is all-or-nothing: one bad chunk sinks the whole file but not sibling files in a multi-file `cdb put a b c` batch. So `put_chunked` returns 0/1 and the outer loop bumps `errors` once.

---

### `cli/src/main.cpp` — MOD (extend --type acceptance)

**Analog:** `cli/src/main.cpp:637-642` (existing `--type` argument handling) and `cli/src/commands.cpp:1135-1144` (the type-string-to-magic mapping).

**Mirror — two trivial extensions:**

1. Update the `--type` help string (`main.cpp:618`, `main.cpp:639`):
   ```cpp
   "  --type <TYPE>               Filter by type: CENV, PUBK, TOMB, DLGT, CDAT, CPAR\n"
   ```
   and the error message at line 639:
   ```cpp
   "Error: --type requires a type name (CENV, PUBK, TOMB, DLGT, CDAT, CPAR)\n"
   ```

2. Add CPAR case to the string-to-magic map (`commands.cpp:1135-1144`):
   ```cpp
   else if (type_filter == "CPAR") filter_bytes = CPAR_MAGIC;
   ```
   And update the "unknown type" fprintf at line 1141 to include CPAR.

**No structural change — just appending "CPAR" to 4 string lists.**

---

### `cli/tests/CMakeLists.txt` — MOD (register new test)

**Analog:** itself, lines 4-14.

**Mirror — append `test_chunked.cpp`** after `test_connection_pipelining.cpp`:

```cmake
add_executable(cli_tests
  test_identity.cpp
  test_wire.cpp
  test_envelope.cpp
  test_contacts.cpp
  test_connection_pipelining.cpp
  test_chunked.cpp                                           # NEW
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/identity.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/wire.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/envelope.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/contacts.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/chunked.cpp             # NEW — if chunked.cpp is not header-only
)
```

**Rule of thumb:** if `chunked.h` is pure header (like `pipeline_pump.h`), no `.cpp` addition needed. If `chunked.cpp` has FlatBuffers encode/decode for the Manifest + retry loop, it DOES need to be linked into `cli_tests` so tests can call `build_manifest` / `parse_manifest`.

**`cli/CMakeLists.txt`** — also add `src/chunked.cpp` to the `cdb` executable at line 87-94 (the main-binary list). Same style — append one line. No new `target_link_libraries` needed; chunked uses the same deps (flatbuffers, sodium, oqs via wire.h).

---

## Shared Patterns (apply across all new files)

### Namespace convention
**Source:** `cli/src/wire.h:11`, `envelope.h:8`, `connection.h:14`, `pipeline_pump.h:11`
**Apply to:** `chunked.h`, `chunked.cpp`, `test_chunked.cpp`

All CLI source uses `namespace chromatindb::cli { ... }` at the top level. Sub-namespaces for focused modules: `chromatindb::cli::envelope` (envelope.h), `chromatindb::cli::pipeline` (pipeline_pump.h), `chromatindb::cli::cmd` (commands.h). Chunked helpers → `chromatindb::cli::chunked`.

### Include-path style
**Source:** all `cli/src/*.cpp` files
**Apply to:** all new .cpp / .h files

Repo-root-anchored paths: `#include "cli/src/wire.h"` — NOT `#include "wire.h"` and NOT `#include "../src/wire.h"`. This works because `cli/CMakeLists.txt:97` sets `target_include_directories(cdb PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)` and `tests/CMakeLists.txt:17` does the same for tests.

### Error reporting
**Source:** `cli/src/commands.cpp` passim (60+ call sites)
**Apply to:** all user-facing code paths in chunked helpers

```cpp
std::fprintf(stderr, "Error: <what went wrong>: %s\n", detail.c_str());
```

Never `std::cerr << ...`. Never `spdlog::error(...)` for user-facing paths. `spdlog::debug(...)` is OK for internal "dropped rid" / "unexpected state" cases — see `commands.cpp:618, 727`.

### Big-endian + FlatBuffers integer handling
**Source:** `cli/src/wire.h:17-62`
**Apply to:** Manifest encode/decode, any new length-prefix fields

Always use `store_u32_be` / `load_u32_be` for length prefixes in hand-rolled payloads. FlatBuffers scalar fields go through `AddElement<uint32_t>` / `table->GetField<uint32_t>` which handle endianness internally (FlatBuffers is little-endian on the wire but all the CLI's **internal framing** is big-endian).

### Payload construction for put
**Source:** `cli/src/commands.cpp:198-222` (`build_put_payload`)
**Apply to:** CDAT chunk payload (with or without filename metadata) and CPAR manifest payload

The existing `build_put_payload` uses `[metadata_len:4BE][metadata_json][file_data]`. For CDAT chunks, the "file_data" portion is the raw 16 MiB slice. Decision for planner: do chunks carry per-chunk metadata (e.g. chunk_index)? Probably NO — the manifest carries all ordering, so chunks can use `build_put_payload("", chunk_bytes)` with empty filename metadata. For the CPAR manifest blob, "file_data" is the FlatBuffers-encoded `Manifest`.

### Arrival-order drain + stray-rid logging
**Source:** `cli/src/commands.cpp:597-621` (put), `672-731` (get)
**Apply to:** `put_chunked`, `get_chunked`

Copy verbatim, only changing the `files[idx]` references to `chunks[idx]` (or `chunk_hashes[idx]` in the download case). The `spdlog::debug("cmd::X: discarding reply for unknown rid {}")` stray-rid handler is mandatory — it's the T-120-02 mitigation.

### Quiet-mode gating
**Source:** `cli/src/commands.cpp:637, 805`
**Apply to:** per-chunk progress lines + final `saved:` line

```cpp
if (!opts.quiet) {
    std::fprintf(stderr, "saved: %s\n", out_path.c_str());
}
```

---

## Gaps — No Close Analog (planner must design fresh)

### Gap 1: Schema file location (`cli/schemas/` vs `db/schemas/`)

**Problem:** `db/schemas/` exists and the node's CMake (`db/CMakeLists.txt:127-137`) has a flatc invocation for blob.fbs. `cli/` has no `schemas/` directory today. CLI uses hand-coded vtables in `cli/src/wire.cpp:22-31` — no flatc dep in the CLI build.

**Options:**
- (a) Create `cli/schemas/manifest.fbs`, no CMake change needed, hand-code encoder in `chunked.cpp` (following `encode_blob` style).
- (b) Put it in `db/schemas/manifest.fbs` alongside blob.fbs (even though only the CLI uses it) — node's flatc auto-picks it up if added to `db/CMakeLists.txt:139` block. CLI ignores the generated header, still hand-codes.
- (c) Create a shared `schemas/` at repo root.

**Recommendation (based on existing pattern):** (a) — schema lives where the code that uses it lives. The node never touches the Manifest wire format (dumb-DB principle, CONTEXT.md line 12), so there's no reason for it to sit in `db/schemas/`.

### Gap 2: Streaming file read (chunked reads from disk)

**No existing analog.** Every CLI code path today reads whole files into memory (`read_file_bytes`, `read_stdin_bytes` at `commands.cpp:30-42`). D-11/D-12 require incremental read + incremental pwrite. Planner designs this from scratch — `std::ifstream` with `f.read(buf, CHUNK_SIZE)` per iteration, `pread`/`pwrite` for output offsets (need POSIX `<unistd.h>` — already included at `commands.cpp:20`).

### Gap 3: Per-chunk retry with exponential backoff (D-15)

**No existing analog.** All current code paths have zero retries — one bad ack = one error++. Planner must design the retry wrapper. Key constraint from D-15: fresh signature per retry attempt (ML-DSA-87 non-determinism → new blob_hash each attempt), so the `rid_to_index` → `final_chunk_hash` binding must happen at drain time (after WriteAck), NOT at send time. This requires a second map: `rid → chunk_index` (same as today) PLUS `chunk_index → final_hash` populated on WriteAck. Only after all chunks have final hashes is the manifest built.

### Gap 4: Type dispatch switch in `cmd::get` (CPAR vs CENV vs PUBK vs raw)

**Partial analog.** Today `cmd::get` at `commands.cpp:759-770` checks `envelope::is_envelope(blob->data)` and branches to decrypt-or-raw. The Phase 119 addition is a 4-byte-prefix dispatch BEFORE that envelope check (per D-13 outer-magic). This is mildly novel structurally — the planner should document the branch order clearly:

```
blob->data starts with:
  CPAR magic → strip magic, decrypt envelope, parse Manifest, call get_chunked
  CENV magic → (no outer magic, CENV is already the envelope magic)  [current path]
  CDAT magic → error: user tried to cdb get a raw chunk (defense-in-depth)
  other      → treat as raw unencrypted blob  [current path]
```

---

## Metadata

**Analog search scope:**
- `cli/src/*.{h,cpp}` — 14 files
- `cli/tests/*.cpp` — 5 files
- `db/schemas/*.fbs` — 2 files
- `db/CMakeLists.txt` + `cli/CMakeLists.txt` + `cli/tests/CMakeLists.txt` — build configs

**Files scanned:** 22

**Pattern extraction date:** 2026-04-19

**Key references used in this map:**
- `cli/src/pipeline_pump.h` (98 lines, Phase 120-01) — canonical header-only helper shape
- `cli/src/commands.cpp:482-925` (cmd::put / cmd::get / cmd::rm, Phase 120-02) — canonical two-phase pipelined shape
- `cli/src/wire.h:217-238` — existing CDAT scaffolding (Phase 117/119 predecessor work)
- `cli/src/wire.cpp:22-31, 153-220` — hand-coded FlatBuffers vtable pattern (what Manifest follows)
- `cli/tests/test_connection_pipelining.cpp` (177 lines, Phase 120-01) — freshest Catch2 analog
- `db/schemas/blob.fbs` (13 lines) — schema style to match
