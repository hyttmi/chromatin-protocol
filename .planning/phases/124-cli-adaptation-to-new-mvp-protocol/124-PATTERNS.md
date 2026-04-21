# Phase 124: CLI Adaptation to New MVP Protocol — Pattern Map

**Mapped:** 2026-04-21
**Files analyzed:** 9 (7 modify/new source + 1 new test + 1 new artifact)
**Analogs found:** 8 / 9 (the E2E artifact is an operator doc, no code analog)

All analogs cited below are concrete CLI files in `cli/src/` or `cli/tests/`. Planner
should reference these by **file + exact line range** when writing plan actions; do
not paraphrase — copy the excerpt.

---

## File Classification

| New/Modified File | Role | Data Flow | Closest Analog | Match Quality |
|-------------------|------|-----------|----------------|---------------|
| `cli/src/wire.h` (modify) | wire-schema header | transform (schema decl) | `cli/src/wire.h` itself (existing enum + struct; in-place edit) | exact |
| `cli/src/wire.cpp` (modify) | wire-codec | transform (FlatBuffer encode/decode) | `cli/src/wire.cpp:170-234` (`encode_blob`/`decode_blob`) + `cli/src/wire.cpp:54-100` (`encode_transport`/`decode_transport`) | exact |
| `cli/src/pubk_presence.h` (NEW) | utility header | state+probe helper | `cli/src/wire.h:152-158` (pure free-fn signature style) + `cli/src/connection.h:23` (pipeline depth constant pattern) | role-match |
| `cli/src/pubk_presence.cpp` (NEW) | utility + probe | request-response + in-proc cache | `cli/src/commands.cpp:150-198` (`find_pubkey_blob`) | **exact** — this is literally the probe logic generalized |
| `cli/src/commands.cpp` (modify) | controller / subcommand dispatch | request-response + CRUD | `cli/src/commands.cpp:502-568` (`submit_name_blob`/`submit_bomb_blob` already wrap the D-03 pattern) | **exact** |
| `cli/src/chunked.cpp` (modify) | service (chunked upload/download + rm) | streaming + CRUD | `cli/src/chunked.cpp:86-133` (`build_cdat_blob_flatbuf`/`build_tombstone_flatbuf`) | **exact** |
| `cli/tests/test_wire.cpp` (modify) | unit test | transform roundtrip | `cli/tests/test_wire.cpp:145-204` (existing blob + signing_input TEST_CASEs) | exact |
| `cli/tests/test_auto_pubk.cpp` (NEW) | unit test | request-response with mock | `cli/tests/test_connection_pipelining.cpp:1-120` + `cli/tests/pipeline_test_support.h` | **exact** (same `ScriptedSource` mock) |
| `.planning/phases/124-.../124-E2E.md` (NEW) | operator artifact | documentation | — | no analog (prose) |

**Cross-cutting observation:** The D-03 helper `build_owned_blob` generalizes
**three existing pattern instances** already in the codebase:
`submit_name_blob` (`commands.cpp:502`), `submit_bomb_blob` (`commands.cpp:538`),
`build_cdat_blob_flatbuf` (`chunked.cpp:86`). Every one of the 12 construction
sites identified in RESEARCH Q1 collapses to `build_owned_blob()` + envelope emit.

---

## Pattern Assignments

### `cli/src/wire.h` (wire-schema header, modify)

**Analog:** `cli/src/wire.h` (in-place schema edit; the header IS the pattern).

**Change 1 — `MsgType` enum** (lines 69-88):

Current:
```cpp
enum class MsgType : uint8_t {
    Data                  = 8,    // DELETE (D-04a)
    Delete                = 17,   // KEEP (RESEARCH Q3 — node still emits DeleteAck)
    DeleteAck             = 18,
    WriteAck              = 30,
    // ...
    SyncNamespaceAnnounce = 62,
    ErrorResponse         = 63,
    // ADD: BlobWrite      = 64,   // D-04
};
```

Target edits (D-04, D-04a):
- **DELETE** `Data = 8`
- **KEEP** `Delete = 17` (per RESEARCH Risks/Unknowns #2 — DeleteAck routing)
- **ADD** `BlobWrite = 64` after `ErrorResponse = 63`

**Change 2 — `BlobData` struct** (lines 132-139):

Current (POST-shim: remove `namespace_id` + `pubkey`):
```cpp
struct BlobData {
    std::array<uint8_t, 32> namespace_id{};   // DELETE
    std::vector<uint8_t> pubkey;              // DELETE
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> signature;
};
```

Target (D-03a — byte-identical to `db/wire/codec.h:18-24`):
```cpp
struct BlobData {
    std::array<uint8_t, 32> signer_hint{};   // 32 bytes; SHA3(signing_pk)
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> signature;
};
```

**Change 3 — `build_signing_input` parameter rename** (lines 152-158):

Current:
```cpp
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t, 32> namespace_id,   // → target_namespace (D-03)
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);
```

Rename parameter only; byte output IDENTICAL (Phase 122 D-01 invariant).

**Change 4 — ADD new declarations** (insert after the blob block, ~line 146):

```cpp
/// D-03: (target_namespace, BlobData) pair returned by build_owned_blob.
/// Name mirrors db/sync/sync_protocol.h for CLI↔node vocabulary symmetry.
struct NamespacedBlob {
    std::array<uint8_t, 32> target_namespace{};
    BlobData blob;
};

/// D-03: compose signer_hint = SHA3(signing_pk), sign, and return the
/// (target_namespace, signed-blob) pair. Replaces the 12 copy-pasted
/// `blob.namespace_id = ns; blob.pubkey.assign(...); blob.signature = id.sign(...)`
/// sequences across commands.cpp + chunked.cpp.
NamespacedBlob build_owned_blob(
    const Identity& id,
    std::span<const uint8_t, 32> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

/// D-04: encode BlobWriteBody { target_namespace:32, blob:Blob } — matches
/// db/schemas/transport.fbs:83-87 byte-for-byte. Sent under MsgType::BlobWrite.
std::vector<uint8_t> encode_blob_write_body(
    std::span<const uint8_t, 32> target_namespace,
    const BlobData& blob);
```

Include requirement: `build_owned_blob` takes `const Identity&`, so `wire.h` must
add `#include "cli/src/identity.h"` (or the planner can forward-declare `class
Identity;` and pull the include into `wire.cpp` — cheaper for compile graph).

---

### `cli/src/wire.cpp` (wire-codec, modify)

**Analog:** `cli/src/wire.cpp` (in-place codec edit). Pattern symmetric to
`encode_transport` / `decode_transport` (lines 54-100).

**Change 1 — `blob_vt` offsets** (lines 24-31):

Current (6-field layout):
```cpp
namespace blob_vt {
    constexpr flatbuffers::voffset_t NAMESPACE_ID = 4;    // DELETE
    constexpr flatbuffers::voffset_t PUBKEY       = 6;    // DELETE
    constexpr flatbuffers::voffset_t DATA         = 8;    // → 6
    constexpr flatbuffers::voffset_t TTL          = 10;   // → 8
    constexpr flatbuffers::voffset_t TIMESTAMP    = 12;   // → 10
    constexpr flatbuffers::voffset_t SIGNATURE    = 14;   // → 12
}
```

Target (5-field layout matching `db/schemas/blob.fbs`):
```cpp
namespace blob_vt {
    constexpr flatbuffers::voffset_t SIGNER_HINT = 4;
    constexpr flatbuffers::voffset_t DATA        = 6;
    constexpr flatbuffers::voffset_t TTL         = 8;
    constexpr flatbuffers::voffset_t TIMESTAMP   = 10;
    constexpr flatbuffers::voffset_t SIGNATURE   = 12;
}
```

Also rewrite the comment on line 22 to match the new schema:
```cpp
// Blob: table { signer_hint:[ubyte]; data:[ubyte]; ttl:uint32; timestamp:uint64; signature:[ubyte]; }
// VTable field offsets: 4, 6, 8, 10, 12
```

**Change 2 — `encode_blob` rewrite** (lines 170-193):

Current body:
```cpp
auto ns  = builder.CreateVector(blob.namespace_id.data(), blob.namespace_id.size());
auto pk  = builder.CreateVector(blob.pubkey.data(), blob.pubkey.size());
auto dt  = builder.CreateVector(blob.data.data(), blob.data.size());
auto sig = builder.CreateVector(blob.signature.data(), blob.signature.size());

auto start = builder.StartTable();
builder.AddOffset(blob_vt::NAMESPACE_ID, ns);
builder.AddOffset(blob_vt::PUBKEY, pk);
builder.AddOffset(blob_vt::DATA, dt);
builder.AddElement<uint32_t>(blob_vt::TTL, blob.ttl, 0);
builder.AddElement<uint64_t>(blob_vt::TIMESTAMP, blob.timestamp, 0);
builder.AddOffset(blob_vt::SIGNATURE, sig);
```

Target:
```cpp
auto sh  = builder.CreateVector(blob.signer_hint.data(), blob.signer_hint.size());
auto dt  = builder.CreateVector(blob.data.data(), blob.data.size());
auto sig = builder.CreateVector(blob.signature.data(), blob.signature.size());

auto start = builder.StartTable();
builder.AddOffset(blob_vt::SIGNER_HINT, sh);
builder.AddOffset(blob_vt::DATA, dt);
builder.AddElement<uint32_t>(blob_vt::TTL, blob.ttl, 0);
builder.AddElement<uint64_t>(blob_vt::TIMESTAMP, blob.timestamp, 0);
builder.AddOffset(blob_vt::SIGNATURE, sig);
```

Also shrink the estimated-size hint on line 171 — no longer need
`blob.pubkey.size()` (always 2592 bytes saved); replace with
`blob.signer_hint.size()` = 32.

**Change 3 — `decode_blob` rewrite** (lines 195-234):

Current (lines 208-232):
```cpp
auto* ns = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::NAMESPACE_ID);
if (ns && ns->size() == 32) {
    std::memcpy(result.namespace_id.data(), ns->data(), 32);
}

auto* pk = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::PUBKEY);
if (pk) { result.pubkey.assign(pk->begin(), pk->end()); }
```

Target:
```cpp
auto* sh = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::SIGNER_HINT);
if (sh && sh->size() == 32) {
    std::memcpy(result.signer_hint.data(), sh->data(), 32);
}
```

Rest of decode body (data, ttl, timestamp, signature reads) unchanged.

**Change 4 — `build_signing_input` parameter rename** (lines 240-244):

Change parameter name `namespace_id` → `target_namespace`. Body unchanged
(lines 246-264 absorb the span without caring about the name).

**Change 5 — ADD `build_owned_blob` and `encode_blob_write_body`.**

`build_owned_blob` implementation (RESEARCH Q12 — ready to copy):
```cpp
NamespacedBlob build_owned_blob(
    const Identity& id,
    std::span<const uint8_t, 32> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    auto signer_hint   = sha3_256(id.signing_pubkey());  // 32 bytes
    auto signing_input = build_signing_input(target_namespace, data, ttl, timestamp);
    auto signature     = id.sign(signing_input);

    BlobData blob{};
    blob.signer_hint = signer_hint;
    blob.data.assign(data.begin(), data.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;
    blob.signature = std::move(signature);

    std::array<uint8_t, 32> ns_arr{};
    std::memcpy(ns_arr.data(), target_namespace.data(), 32);
    return {ns_arr, std::move(blob)};
}
```

`encode_blob_write_body` pattern: mirror `encode_transport` (lines 54-73) —
same hand-coded FlatBuffer vtable idiom. Add a new `blob_write_body_vt`
namespace (offsets 4, 6 for `target_namespace`, `blob`) near
`blob_vt` / `manifest_vt`. Cross-verify offsets against
`db/schemas/transport.fbs:83-87`. The inner Blob is NOT inlined — it's built
first, offset obtained, then the outer table holds the offset.

**Error handling pattern — already uniform:** `encode_*` returns `std::vector<uint8_t>`
by value (detaches builder ownership, RESEARCH pitfall #3). `decode_*` returns
`std::optional<T>`, nullopt on any parse failure. Copy exactly.

---

### `cli/src/pubk_presence.h` (NEW — utility header)

**Analog:** `cli/src/wire.h:152-158` (free-fn declaration style) +
`cli/src/connection.h:23` (public constant style).

**Imports pattern** (copy from `cli/src/wire.h:1-12`):
```cpp
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace chromatindb::cli {
class Identity;     // fwd-decl; include in .cpp
class Connection;   // fwd-decl; include in .cpp
```

**Core declaration** (new surface, justified by RESEARCH Q5 recommendation +
D-01 invocation-scope cache):
```cpp
/// D-01: Invocation-scoped PUBK presence check. Before the FIRST owner-write
/// (target_ns == SHA3(id.signing_pubkey())) to a given namespace, probe the
/// node for a PUBK blob and emit one if absent. Subsequent calls for the
/// same target_ns within the same process return immediately.
///
/// Delegate writes (target_ns != SHA3(own_signing_pk)) are a no-op and
/// return true — PUBK registration is the owner's responsibility; node
/// will reject via ERROR_PUBK_FIRST_VIOLATION if the namespace is fresh.
///
/// Returns false only on hard transport error from the probe or emit;
/// caller should surface via D-05 error mapping.
///
/// @param rid_counter in/out: caller's rid counter; advanced by up to 2
///                    (probe + optional emit) on owner-write first call.
bool ensure_pubk(Identity& id,
                 Connection& conn,
                 std::span<const uint8_t, 32> target_namespace,
                 uint32_t& rid_counter);

/// Test-only: clear the process-local cache. Kept public so test_auto_pubk
/// can reset state between TEST_CASEs.
void reset_pubk_presence_cache_for_tests();

} // namespace chromatindb::cli
```

---

### `cli/src/pubk_presence.cpp` (NEW — probe + cache)

**Analog:** `cli/src/commands.cpp:150-198` (`find_pubkey_blob`) — the probe half
is the template. The emit half is `cmd::publish` (`commands.cpp:2490-2557`).
Owner-check is from RESEARCH Q12.

**Imports pattern** (copy style from `cli/src/commands.cpp:1-30`):
```cpp
#include "cli/src/pubk_presence.h"
#include "cli/src/connection.h"
#include "cli/src/identity.h"
#include "cli/src/wire.h"

#include <array>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_set>

namespace chromatindb::cli {
```

**Cache pattern** (D-01 — process-lifetime, thread-local-safe-enough since cdb
is single-threaded; RESEARCH Q5 + Risks #1):
```cpp
namespace {

// Hash functor for std::unordered_set<std::array<uint8_t, 32>>.
// Simple: XOR-fold 4 × uint64_t slices. Good enough for a small in-proc set.
struct Ns32Hash {
    size_t operator()(const std::array<uint8_t, 32>& a) const noexcept {
        uint64_t h = 0;
        for (size_t i = 0; i < 32; i += 8) {
            uint64_t w;
            std::memcpy(&w, a.data() + i, 8);
            h ^= w + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return static_cast<size_t>(h);
    }
};

// Process-lifetime cache of namespaces known to have PUBK on some node this
// process has spoken to. Deliberately NOT persisted (CONTEXT deferred).
std::unordered_set<std::array<uint8_t, 32>, Ns32Hash>& pubk_cache() {
    static std::unordered_set<std::array<uint8_t, 32>, Ns32Hash> s;
    return s;
}

} // namespace
```

**Probe core pattern** — copy directly from `commands.cpp:153-173` (the ListRequest
payload construction), adapted for `limit=1`:
```cpp
// Reuse the wire layout documented at commands.cpp:156-161.
//   [0..31]  ns
//   [32..39] since_seq = 0
//   [40..43] limit     = 1   (probe only needs existence boolean)
//   [44]     flags     = 0x02 (type_filter present)
//   [45..48] type      = PUBK_MAGIC
std::vector<uint8_t> list_payload(49, 0);
std::memcpy(list_payload.data(), target_namespace.data(), 32);
store_u32_be(list_payload.data() + 40, 1);
list_payload[44] = 0x02;
std::memcpy(list_payload.data() + 45, PUBKEY_MAGIC.data(), 4);

const uint32_t probe_rid = rid_counter++;
if (!conn.send(MsgType::ListRequest, list_payload, probe_rid)) return false;
auto resp = conn.recv();
if (!resp ||
    resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
    resp->payload.size() < 5) return false;

uint32_t count = load_u32_be(resp->payload.data());
if (count > 0) {
    pubk_cache().insert(ns_arr);
    return true;   // PUBK present; no emit needed
}
```

**Emit pattern** — copy from `cmd::publish` (`commands.cpp:2512-2549`), routed
through the new D-03 helper + new `BlobWrite=64` envelope:
```cpp
auto pubkey_data = make_pubkey_data(id.signing_pubkey(), id.kem_pubkey());
auto timestamp   = static_cast<uint64_t>(std::time(nullptr));
auto ns_blob     = build_owned_blob(id, target_namespace, pubkey_data, 0, timestamp);
auto envelope    = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);

const uint32_t emit_rid = rid_counter++;
if (!conn.send(MsgType::BlobWrite, envelope, emit_rid)) return false;
auto ack = conn.recv();
if (!ack || ack->type != static_cast<uint8_t>(MsgType::WriteAck) ||
    ack->payload.size() < 32) {
    return false;
}
pubk_cache().insert(ns_arr);
return true;
```

**Owner-check (D-01a)** — copy from RESEARCH Q12:
```cpp
// Delegate writes skip auto-PUBK entirely. Owner = target_ns equals own ns.
auto own_ns = id.namespace_id();
if (std::memcmp(own_ns.data(), target_namespace.data(), 32) != 0) {
    return true;   // delegate — PUBK is owner's responsibility
}
```

**Ordering invariant (RESEARCH Pitfall #7):** the probe MUST complete (and the
PUBK emit MUST receive WriteAck) BEFORE the caller sends the user's blob.
Strictly serial. Use `conn.send()` + `conn.recv()`, NOT `send_async` +
`recv_for`, to avoid coroutine/span lifetime hazards (RESEARCH Pitfall #1).

---

### `cli/src/commands.cpp` (controller / subcommand dispatch, modify)

**Analog:** `cli/src/commands.cpp:502-568` (`submit_name_blob` + `submit_bomb_blob`)
already wraps the D-03 pattern — refactor both to call `build_owned_blob`. Use
this as the template for migrating the other 7 sites.

**Pattern 1 — Migrate every blob-construction site to `build_owned_blob`:**

Example BEFORE (from `submit_name_blob` at lines 509-521, TYPICAL of all 9 sites):
```cpp
auto name_data     = make_name_data(name_bytes, target_hash);
auto signing_input = build_signing_input(ns, name_data, ttl, now);
auto signature     = id.sign(signing_input);

BlobData blob{};
std::memcpy(blob.namespace_id.data(), ns.data(), 32);
blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
blob.data = std::move(name_data);
blob.ttl = ttl;
blob.timestamp = now;
blob.signature = std::move(signature);

auto flatbuf = encode_blob(blob);
if (!conn.send(MsgType::Data, flatbuf, rid)) return std::nullopt;
```

AFTER (D-03 + D-04):
```cpp
auto name_data = make_name_data(name_bytes, target_hash);
auto ns_blob   = build_owned_blob(id, ns, name_data, ttl, now);
auto envelope  = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);
if (!conn.send(MsgType::BlobWrite, envelope, rid)) return std::nullopt;
```

**MsgType disposition for each site** (RESEARCH Q3, binding — do not deviate):

| Site | Current MsgType | Post-124 MsgType | Expected Ack |
|------|-----------------|------------------|--------------|
| `:514` `submit_name_blob` | Data → | **BlobWrite** | WriteAck |
| `:547` `submit_bomb_blob` | Delete → | **Delete** (KEEP — BOMB is a batched tombstone) | DeleteAck or WriteAck (existing tolerance at :560) |
| `:715` `cmd::put` (pipelined) | Data → | **BlobWrite** | WriteAck |
| `:1232` `cmd::rm` single-tombstone | Delete → | **Delete** (KEEP) | DeleteAck |
| `:1702` `cmd::reshare` new blob | Data → | **BlobWrite** | WriteAck |
| `:1748` `cmd::reshare` tombstone | Delete → | **Delete** (KEEP) | DeleteAck |
| `:2254` `cmd::delegate` | Data → | **BlobWrite** | WriteAck |
| `:2386` `cmd::revoke` tombstone | Delete → | **Delete** (KEEP) | DeleteAck |
| `:2520` `cmd::publish` | Data → | **BlobWrite** | WriteAck |

**Critical:** the payload always goes through `encode_blob_write_body` now —
even for `MsgType::Delete` — because node `db/peer/message_dispatcher.cpp`
accepts `BlobWriteBody` for BOTH `TransportMsgType_BlobWrite` AND
`TransportMsgType_Delete` (RESEARCH Q3). Only the TransportMsgType byte
distinguishes the ack type. `encode_blob` (the bare Blob emitter) stops being
called from any send path in `commands.cpp` + `chunked.cpp`; it remains
exported because `decode_blob` has non-send callers (e.g., ReadResponse parse
at `commands.cpp:1178`).

**Pattern 2 — Auto-PUBK probe at first owner-write** (D-01):

Insert a call to `ensure_pubk(id, conn, ns_span, rid_counter)` immediately
before the FIRST `conn.send(MsgType::BlobWrite, ...)` within every command
flow that opens a `Connection` for owner writes. Affected commands:

| Command | Insert before line |
|---------|--------------------|
| `cmd::put` | just after `conn.connect(...)` succeeds at `:591` (covers all files + any `--name` NAME emit) |
| `cmd::put --name` path | same — `ensure_pubk` is idempotent per invocation |
| `cmd::delegate` | before the loop at `:2247` (target_ns = own ns for delegation blobs) |
| `cmd::reshare` | before the first `conn.send` on `conn2` at `:1718` (writes to own ns) |
| `cmd::publish` | **SKIP** — `cmd::publish` IS the PUBK writer; bypass the helper (RESEARCH Open Q #1). Populate the cache manually on successful WriteAck. |
| `cmd::rm` / `cmd::rm_batch` / `cmd::revoke` | tombstones are writes to own ns — call `ensure_pubk` once before the first send |
| `chunked::put_chunked` | once, before the Phase-A greedy fill loop enters |

Exact integration site for `cmd::put` (between `conn.connect` and the
`while (completed < files.size())` loop at line 700):
```cpp
auto ns_span = std::span<const uint8_t, 32>(ns.data(), 32);
uint32_t probe_rid = 0x2000;   // reserved range, follows :1443 convention
if (!ensure_pubk(id, conn, ns_span, probe_rid)) {
    std::fprintf(stderr, "Error: failed to ensure namespace is published\n");
    return 1;
}
```

**Pattern 3 — D-05 error-code decoder** (greenfield; RESEARCH Q7):

Add a file-local helper near `find_pubkey_blob` (`commands.cpp:~150`):
```cpp
// D-05: decode an ErrorResponse payload into user-facing wording.
// Payload layout: [error_code:1][original_type:1]. Defensive short reads return
// a generic message; unknown codes include the byte value in a non-identifying way.
// Never leaks phase numbers, PUBK_FIRST_VIOLATION, or PUBK_MISMATCH tokens
// (feedback_no_phase_leaks_in_user_strings.md).
static std::string decode_error_response(std::span<const uint8_t> payload,
                                          const std::string& host_hint,
                                          std::span<const uint8_t, 32> ns_hint) {
    if (payload.size() < 2) return "Error: node returned malformed response";
    uint8_t code = payload[0];
    auto ns_short = to_hex(std::span<const uint8_t>(ns_hint.data(), 8));
    switch (code) {
        case 0x07:
            return "Error: namespace not yet initialized on node " + host_hint +
                   ". Auto-PUBK failed; try running 'cdb publish' first.";
        case 0x08:
            return "Error: namespace " + ns_short +
                   " is owned by a different key on node " + host_hint +
                   ". Cannot write.";
        case 0x09:
            return "Error: batch deletion rejected (BOMB must be permanent).";
        case 0x0A:
            return "Error: batch deletion rejected (malformed BOMB payload).";
        case 0x0B:
            return "Error: delegates cannot perform batch deletion on this node.";
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                "Error: node rejected request (code 0x%02X)", code);
            return buf;
        }
    }
}
```

Replace every `"Error: node rejected request\n"` string currently at
`commands.cpp:1257, 1859, 2043, 2097, 2195` with a call to
`decode_error_response(resp->payload, host_hint, ns)`. `host_hint` can come
from `opts.host`. Existing string `Error: bad response` etc. at other sites
SHOULD also check `resp->type == ErrorResponse` and route through the decoder.

**Pattern 4 — D-06 BOMB cascade in `cmd::rm_batch`** (RESEARCH Q6):

The classification logic already lives in `cmd::rm` single-target at
`commands.cpp:1162-1221`. Factor it into a free function at the top of
the file (below `find_pubkey_blob`, ~line 200):

```cpp
struct RmClassification {
    enum class Kind { Plain, Cdat, CparWithChunks, FetchFailed };
    Kind kind = Kind::Plain;
    std::vector<std::array<uint8_t, 32>> cascade_targets;   // CPAR chunk hashes
};

// Extracted from cmd::rm lines 1162-1221. Given a single target, probe its
// blob type and (for CPAR) decrypt-and-parse the manifest to collect chunk
// hashes. Returns Plain for non-chunked targets; FetchFailed if the ReadRequest
// itself failed (caller decides whether to BOMB manifest-only or abort).
static RmClassification classify_rm_target(
    Identity& id,
    Connection& conn,
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> target_hash,
    uint32_t& rid_counter);
```

Body reuses the exact ReadRequest → `decode_blob` → magic-check →
`envelope::decrypt` → `decode_manifest_payload` sequence from
`commands.cpp:1162-1209`. Non-CPAR falls through with `Kind::Plain`;
CPAR extracts `manifest.chunk_hashes` (32-byte stride) into `cascade_targets`.

Then in `cmd::rm_batch` (current serial ExistsRequest loop at `:1353-1386`),
replace with a classify-per-target pass that also records CPAR chunk hashes
into the same `targets` vector. After classification, emit a single BOMB
(existing `submit_bomb_blob` at `:1395` stays — it already goes via
`MsgType::Delete` which is KEPT).

Partial-failure behavior — adopt RESEARCH recommendation (option 3,
warn-and-continue): on `FetchFailed`, emit `warning: cascade fetch failed for
<hash>; BOMBing manifest only` to stderr, include manifest hash only in the
BOMB. Print summary `cascade: N manifests fully tombstoned, M manifests
failed to fetch (manifest-only)`. This avoids failing a 50-target batch on
one bad manifest.

**Pipelining (Claude's discretion):** RESEARCH Q6 suggests pipelining the
per-target ReadRequests via `send_async` + `recv_for` mirroring `cmd::get`
(`:893-976`). Depth = `Connection::kPipelineDepth = 8`. Keep the first
implementation **serial** (one ReadRequest per target) for simplicity; mark
pipelining as a post-124 optimization if E2E shows latency issues.

**Pattern 5 — Imports** (unchanged; current top-of-file imports at
`commands.cpp:1-30` already cover envelope, wire, connection, identity,
chunked, spdlog, nlohmann/json, filesystem). Add `#include
"cli/src/pubk_presence.h"` near the wire/connection includes.

---

### `cli/src/chunked.cpp` (service, modify)

**Analog:** `cli/src/chunked.cpp:86-133` (`build_cdat_blob_flatbuf` +
`build_tombstone_flatbuf`). These two file-local helpers already encapsulate
the D-03 pattern for chunked — refactor their bodies to call `build_owned_blob`,
then their callers inherit the new envelope automatically.

**Construction site 1 — `build_cdat_blob_flatbuf` body** (lines 94-111):

BEFORE:
```cpp
auto cenv = envelope::encrypt(plaintext_chunk, recipient_spans);

std::vector<uint8_t> blob_data;
blob_data.reserve(4 + cenv.size());
blob_data.insert(blob_data.end(), CDAT_MAGIC.begin(), CDAT_MAGIC.end());
blob_data.insert(blob_data.end(), cenv.begin(), cenv.end());

auto signing_input = build_signing_input(ns, blob_data, ttl, timestamp);
auto signature     = id.sign(signing_input);

BlobData blob{};
std::memcpy(blob.namespace_id.data(), ns.data(), 32);
blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
blob.data      = std::move(blob_data);
blob.ttl       = ttl;
blob.timestamp = timestamp;
blob.signature = std::move(signature);
return encode_blob(blob);
```

AFTER — change the helper's return type from `std::vector<uint8_t>` (bare Blob
bytes) to `std::vector<uint8_t>` (BlobWriteBody envelope bytes), matching what
`conn.send(MsgType::BlobWrite, ...)` now expects:
```cpp
auto cenv = envelope::encrypt(plaintext_chunk, recipient_spans);

std::vector<uint8_t> blob_data;
blob_data.reserve(4 + cenv.size());
blob_data.insert(blob_data.end(), CDAT_MAGIC.begin(), CDAT_MAGIC.end());
blob_data.insert(blob_data.end(), cenv.begin(), cenv.end());

auto ns_blob = build_owned_blob(id, ns, blob_data, ttl, timestamp);
return encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);
```

**Construction site 2 — `build_tombstone_flatbuf` body** (lines 120-133):

Same refactor: `make_tombstone_data` → `build_owned_blob(..., 0, timestamp)` →
`encode_blob_write_body(...)`. Return type unchanged externally.

**Construction site 3 — manifest build in `put_chunked`** (lines 318-329):

BEFORE:
```cpp
auto msi = build_signing_input(ns, manifest_blob_data, ttl, timestamp);
auto msig = id.sign(msi);

BlobData mb{};
std::memcpy(mb.namespace_id.data(), ns.data(), 32);
mb.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
mb.data      = std::move(manifest_blob_data);
mb.ttl       = ttl;
mb.timestamp = timestamp;
mb.signature = std::move(msig);

auto mfb = encode_blob(mb);
```

AFTER:
```cpp
auto ns_blob = build_owned_blob(id, ns, manifest_blob_data, ttl, timestamp);
auto mfb     = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);
```

**MsgType changes in `chunked.cpp` send paths** (RESEARCH Q3):

| Line | Current | Target |
|------|---------|--------|
| `:227, :245, :331` (CDAT + manifest sends) | `MsgType::Data` | **`MsgType::BlobWrite`** |
| `rm_chunked` per-chunk tombstone sends (~`:393`) | `MsgType::Delete` | **`MsgType::Delete` (KEEP)** — payload now BlobWriteBody; ack type stays DeleteAck |
| `rm_chunked` manifest tombstone send (~`:446`) | `MsgType::Delete` | **`MsgType::Delete` (KEEP)** |

**DeleteAck checks in `chunked.cpp`** (lines `:425, :451`) remain unchanged —
node still emits DeleteAck for `TransportMsgType_Delete` regardless of the
BlobWriteBody-shaped payload (RESEARCH Q3 evidence).

---

### `cli/tests/test_wire.cpp` (unit test, modify)

**Analog:** Existing TEST_CASEs at `test_wire.cpp:145-204`. Copy style exactly
(Catch2 v3 `REQUIRE` macros, `[wire]` tag, one TEST_CASE per property).

**Modify existing** (lines 145-181) — `wire: encode_blob/decode_blob roundtrip`
+ `wire: encode_blob/decode_blob with zero ttl`:

BEFORE:
```cpp
BlobData blob;
blob.namespace_id.fill(0xAB);
blob.pubkey.resize(2592, 0x01);
blob.data = {0xCA, 0xFE, 0xBA, 0xBE};
...
REQUIRE(decoded->namespace_id == blob.namespace_id);
REQUIRE(decoded->pubkey == blob.pubkey);
```

AFTER:
```cpp
BlobData blob;
blob.signer_hint.fill(0xAB);
blob.data = {0xCA, 0xFE, 0xBA, 0xBE};
blob.ttl = 3600;
blob.timestamp = 1700000000;
blob.signature.resize(4627, 0x02);

auto encoded = encode_blob(blob);
REQUIRE(!encoded.empty());

auto decoded = decode_blob(encoded);
REQUIRE(decoded.has_value());
REQUIRE(decoded->signer_hint == blob.signer_hint);
REQUIRE(decoded->data == blob.data);
REQUIRE(decoded->ttl == blob.ttl);
REQUIRE(decoded->timestamp == blob.timestamp);
REQUIRE(decoded->signature == blob.signature);
```

**Add new TEST_CASEs** (follow the style of lines 145-204; all `[wire]` tagged):

1. **`wire: encode_blob_write_body roundtrip`** — assert
   `target_namespace` + inner Blob survive an encode/decode cycle. (Decode-side
   helper may not exist yet — planner may need a test-only `decode_blob_write_body`
   or inspect the bytes via flatbuffers::Verifier.)

2. **`wire: build_owned_blob populates signer_hint correctly`** — generate an
   Identity, call `build_owned_blob(id, id.namespace_id(), data, ttl, ts)`,
   assert `ns_blob.blob.signer_hint == sha3_256(id.signing_pubkey())` which
   for owner writes equals `id.namespace_id()`.

3. **`wire: build_owned_blob signature verifies against build_signing_input`** —
   after `build_owned_blob`, use liboqs' `OQS_SIG_verify` with
   `id.signing_pubkey()` over the digest `build_signing_input(ns, data, ttl, ts)`
   and the returned `blob.signature`; REQUIRE verify == true.

4. **`wire: build_owned_blob delegate mode — signer_hint != target_namespace`** —
   owner identity `idA`, write target is `idB.namespace_id()`; REQUIRE
   `ns_blob.blob.signer_hint != ns_blob.target_namespace`.

5. **`wire: make_bomb_data roundtrip`** — 3 target hashes, call `make_bomb_data`,
   assert byte layout: `[0..3] == BOMB`, `[4..7]` = count=3 BE, and three
   32-byte target hashes concatenated. (Parsing helper may not exist CLI-side;
   inline parse.)

6. **`wire: parse_name_payload roundtrip`** — pair `make_name_data` with
   `parse_name_payload` for a 1-byte name, a 65535-byte name, and assert
   magic-mismatch rejection (flip byte 0, REQUIRE `!has_value()`).

7. **`wire: build_signing_input golden vector`** — hand-construct
   `(ns={0x00×32}, data={0x01, 0x02, 0x03}, ttl=3600, timestamp=1700000000)`,
   call `build_signing_input`, assert the returned 32-byte digest matches a
   hardcoded hex string. Planner computes the hex by running the code locally
   and pasting; this guards against accidental signing-input drift. Tag
   `[wire][golden]`.

**Test location:** Insert new TEST_CASEs in `test_wire.cpp` alphabetically by
name (Catch2 doesn't enforce, but existing tests are roughly grouped). Keep
imports unchanged (line 1-8).

---

### `cli/tests/test_auto_pubk.cpp` (NEW — unit test)

**Analog:** `cli/tests/test_connection_pipelining.cpp` (exact structure and
mock pattern). Reuse `ScriptedSource` + `make_reply` from
`cli/tests/pipeline_test_support.h`.

**Imports pattern** (copy from `test_connection_pipelining.cpp:1-14`):
```cpp
#include <catch2/catch_test_macros.hpp>
#include "cli/src/pubk_presence.h"
#include "cli/src/wire.h"
#include "cli/tests/pipeline_test_support.h"

#include <array>
#include <cstring>

using namespace chromatindb::cli;
using chromatindb::cli::testing::ScriptedSource;
using chromatindb::cli::testing::make_reply;
```

**Design decision that blocks direct testing:** `ensure_pubk` takes
`Connection&`, which is asio-backed and cannot be mocked trivially.

**Recommended refactor to enable testing** (RESEARCH Q8 recommendation):
Extract the probe+emit core into a template taking `Sender` + `Receiver`
callables:

```cpp
// In pubk_presence.h — test-exposed, pure:
template <typename Sender, typename Receiver>
bool probe_pubk_impl(std::span<const uint8_t, 32> target_namespace,
                     Sender&& send,
                     Receiver&& recv,
                     uint32_t& rid_counter);
```

The Connection-using `ensure_pubk` becomes a thin wrapper that binds
`conn.send` / `conn.recv` to the template. Tests drive the template with
`ScriptedSource` replies directly.

**TEST_CASE coverage (per RESEARCH Q8 + SC#1)** — follow the style at
`test_connection_pipelining.cpp:16-120` exactly:

1. **`pubk: probe returns true when ListResponse count > 0`** — queue a
   ListResponse reply with `count=1` + a 60-byte entry; REQUIRE probe returns
   true, no emit call made.

2. **`pubk: probe returns false triggers PUBK emit on owner write`** — queue
   a ListResponse with `count=0`, then a WriteAck reply; REQUIRE
   `ensure_pubk` returns true AND the second scripted reply was consumed
   (emit happened).

3. **`pubk: second call to same namespace skips probe (cache hit)`** — call
   once with empty `count=0` + WriteAck; call again on the same namespace
   with an empty source; REQUIRE the second call returns true without
   consuming any messages (cache hit). Requires
   `reset_pubk_presence_cache_for_tests()` at `TEST_CASE` start.

4. **`pubk: different namespace triggers a fresh probe`** — after step 3,
   call with a different 32-byte namespace; REQUIRE probe-and-emit flow
   re-runs.

5. **`pubk: delegate write (target_ns != SHA3(own_sp)) skips probe entirely`** —
   construct an Identity, call with a target namespace DIFFERENT from
   `id.namespace_id()`, empty source; REQUIRE true return with no source calls.

6. **`pubk: transport failure during probe returns false`** — source starts
   `dead = true`; REQUIRE `ensure_pubk` returns false (caller surfaces error).

7. **`pubk: golden ListRequest payload bytes`** — capture the outbound
   `send` call's payload via a mock Sender, assert bytes `[44] == 0x02`,
   `[45..48] == PUBKEY_MAGIC`, `[40..43]` = 1 BE.

---

### `.planning/phases/124-.../124-E2E.md` (NEW — operator artifact)

**No code analog.** This is a plain-prose artifact. Follow the structure of
any existing E2E outputs in `.planning/phases/*/`. The planner should require
it to contain:
- One section per D-08 matrix item (1–7)
- For each: the exact `cdb` commands run (both `--node local` and `--node home`),
  observed output, pass/fail verdict.
- Claude runs these directly per `feedback_self_verify_checkpoints.md`.
- Failing any single item blocks phase completion (D-08a).

---

## Shared Patterns

These apply across multiple modified files. The planner should reference each
shared pattern by its "Apply to" list when writing per-plan action sections.

### Shared Pattern A: D-03 helper replaces 12 blob-construction sites

**Source:** `cli/src/commands.cpp:502-519` (canonical `submit_name_blob`)

**Apply to:** All 9 sites in `commands.cpp` (`:513, 547, 715, 1232, 1702, 1748,
2254, 2386, 2520`) and all 3 sites in `chunked.cpp` (`:104, 125, 321`) — per
RESEARCH Q1.

**Mechanical diff pattern** (every site collapses the same way):
```diff
- auto signing_input = build_signing_input(ns, X_data, ttl, ts);
- auto signature     = id.sign(signing_input);
- BlobData blob{};
- std::memcpy(blob.namespace_id.data(), ns.data(), 32);
- blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
- blob.data      = std::move(X_data);
- blob.ttl       = ttl;
- blob.timestamp = ts;
- blob.signature = std::move(signature);
- auto flatbuf = encode_blob(blob);
- if (!conn.send(MsgType::Data, flatbuf, rid)) return ...;    // or MsgType::Delete
+ auto ns_blob  = build_owned_blob(id, ns, X_data, ttl, ts);
+ auto envelope = encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob);
+ if (!conn.send(MsgType::BlobWrite, envelope, rid)) return ...;   // or MsgType::Delete
```

The **only varying part** per site is: (1) the `X_data` producer
(`make_name_data`, `make_bomb_data`, `make_tombstone_data`,
`make_delegation_data`, `make_pubkey_data`, `envelope::encrypt(...)`, or
CDAT/CPAR prefix-concatenation), (2) `ttl` value (0 for permanent, else
user-supplied), and (3) whether the send MsgType stays `Delete` (tombstones)
or becomes `BlobWrite` (all other writes) per RESEARCH Q3.

### Shared Pattern B: ListRequest + type_filter probe (Phase 117 primitive)

**Source:** `cli/src/commands.cpp:150-198` (`find_pubkey_blob` — full probe +
read template) and `:1437-1489` (`enumerate_name_blobs` — paginated variant)

**Apply to:** `cli/src/pubk_presence.cpp` (auto-PUBK probe), potentially D-06
classification (optional; RESEARCH Q6 notes classification uses ReadRequest
instead, not ListRequest).

**49-byte ListRequest payload layout** (copy verbatim):
```cpp
std::vector<uint8_t> payload(49, 0);
std::memcpy(payload.data(), ns.data(), 32);          // [0..31]  namespace_id
// since_seq = 0 at [32..39] (zero-filled)
store_u32_be(payload.data() + 40, LIMIT);            // [40..43] limit BE
payload[44] = 0x02;                                   // [44]     flags: type_filter
std::memcpy(payload.data() + 45, MAGIC.data(), 4);   // [45..48] type_filter bytes
```

`LIST_ENTRY_SIZE = 60` bytes per entry (hash:32 + seq:8BE + type:4 + size:8BE +
ts:8BE) — see `commands.cpp:109` constant. First 4 bytes of ListResponse
payload = count BE; last byte = `has_more`. Copy exactly.

### Shared Pattern C: D-05 error-code decoder

**Source:** `cli/src/commands.cpp:1256-1258` (current generic-error pattern to
replace) + new `decode_error_response` helper spec above in
`commands.cpp` section.

**Apply to:** Every `ErrorResponse` check site — `commands.cpp:1256, 1859,
2043, 2097, 2195` + every `WriteAck`/`DeleteAck` check that silently falls
through on `ErrorResponse`. Wire it so: on any `resp->type ==
ErrorResponse`, call `decode_error_response(resp->payload, opts.host, ns)` and
print to stderr, return nonzero.

**Never leaks:** Per `feedback_no_phase_leaks_in_user_strings.md`, the error
strings MUST NOT contain `PUBK_FIRST_VIOLATION`, `PUBK_MISMATCH`, phase
numbers, or internal token names. Wording locked by D-05.

### Shared Pattern D: Synchronous send/recv for ordering-critical flows

**Source:** `cli/src/commands.cpp:2536-2549` (`cmd::publish` — canonical
ordered PUBK emit pattern)

**Apply to:** Auto-PUBK probe+emit in `pubk_presence.cpp`. RESEARCH Pitfall #7:
PUBK emit WriteAck MUST land before the user's blob is sent — strictly serial.
Use `conn.send` + `conn.recv`, NOT `conn.send_async` + `conn.recv_for`.
Rationale: coroutine-container-invalidation (MEMORY) + DB's own
PUBK-first-wins semantics.

### Shared Pattern E: Test harness — `ScriptedSource` mock

**Source:** `cli/tests/pipeline_test_support.h:38-57` (+ usage sites in
`test_connection_pipelining.cpp:16-120`)

**Apply to:** `cli/tests/test_auto_pubk.cpp` — every TEST_CASE uses
`ScriptedSource` to feed canned replies. Pattern is "queue up
`DecodedTransport` replies in the order they'll arrive; drain via `operator()`
or via a test-only Sender callback that captures sends."

Caveat: `ensure_pubk` needs either a templated probe-impl (preferred — RESEARCH
Q8) or a thin refactor that exposes probe+emit as free functions taking
callables. Planner decides but the RESEARCH recommendation is clear.

### Shared Pattern F: File header style + imports

**Source:** `cli/src/commands.cpp:1-30` (production) and
`cli/tests/test_wire.cpp:1-8` (tests)

**Apply to:** Every new source/test file. Production files:
`#include` local headers via `"cli/src/..."` prefix (absolute-from-repo-root
style), then standard library, then third-party (flatbuffers, sodium, oqs).
`namespace chromatindb::cli { ... }`. Tests: `#include
<catch2/catch_test_macros.hpp>` first, then local headers, then stdlib; `using
namespace chromatindb::cli;` at file scope.

---

## No Analog Found

None of the 9 files have NO analog. Even the operator E2E artifact (markdown
prose) has structural precedent in prior phases' E2E outputs.

---

## Metadata

**Analog search scope:** `cli/src/*.{h,cpp}`, `cli/tests/*.{h,cpp}`,
`db/wire/codec.h`, `db/schemas/*.fbs`, `db/peer/error_codes.h` (read-only
cross-reference). Approx 15 files read in full or in targeted ranges.

**Files scanned:** 15 primary + headers/tests verified via Grep.

**Pattern extraction date:** 2026-04-21

**Key derivation:** All patterns cite **concrete line ranges** in existing
CLI code. Planner should quote these ranges verbatim when drafting plan
actions; do not paraphrase.
