# Phase 123: Tombstone Batching + Name-Tagged Overwrite — Pattern Map

**Mapped:** 2026-04-20
**Files analyzed:** 17 (8 MODIFIED, 6 NEW, 3 MODIFIED-IF-NEEDED)
**Analogs found:** 17 / 17 (all exact or role-match; nothing new under the sun — every delta mirrors an existing Phase 117/122 shape)

## File Classification

| New/Modified File | Role | Data Flow | Closest Analog | Match Quality |
|-------------------|------|-----------|----------------|---------------|
| `db/wire/codec.h` (MODIFY) | codec-header / shared-utility | transform | `db/wire/codec.h:92-152` (existing TOMB / DLGT / PUBK blocks in same file) | exact (same file, same idiom) |
| `db/wire/codec.cpp` (MODIFY) | codec-impl | transform | `db/wire/codec.cpp:137-176` (tombstone + delegation `is_*` / `extract_*` / `make_*`) | exact |
| `cli/src/wire.h` (MODIFY) | codec-header mirror | transform | `cli/src/wire.h:196-308` (mirror constants + `type_label` + `is_hidden_type`) | exact |
| `cli/src/wire.cpp` (MODIFY) | codec-impl mirror | transform | `cli/src/wire.cpp:277-308` (`make_tombstone_data`, `make_delegation_data`, `make_pubkey_data`) | exact |
| `db/engine/engine.cpp` (MODIFY) | ingest-pipeline | request-response / validation | Phase 122 Step 1.5 at `engine.cpp:181-195` (PUBK-first gate) + Step 3.5 at `engine.cpp:349-366` (tombstone side-effect) + Step 2 trailer at `engine.cpp:266-280` (delegate-rejection) + Step 0e at `engine.cpp:160-172` (max_ttl exemption) | exact (multi-site; each new block has a one-for-one template) |
| `db/peer/error_codes.h` (MODIFY) | enum-constants | N/A | `db/peer/error_codes.h:16-17` (`ERROR_PUBK_FIRST_VIOLATION = 0x07`, `ERROR_PUBK_MISMATCH = 0x08`) | exact |
| `db/peer/message_dispatcher.cpp` (PROBABLY NO CHANGE) | transport-dispatcher | request-response | `db/peer/message_dispatcher.cpp:473-585` (`ListRequest` already supports `type_filter` per Phase 117) | reuse-unchanged (per RESEARCH §Primary Recommendation) |
| `db/storage/storage.{h,cpp}` (PROBABLY NO CHANGE) | storage-layer | CRUD | `db/storage/storage.cpp:463-471` (seq_map stores `[hash:32][type:4]`) | reuse-unchanged |
| `cli/src/commands.h` (MODIFY) | command-header | N/A | `cli/src/commands.h:30-47` (current `put` / `get` / `rm` signatures) | exact (signature change only) |
| `cli/src/commands.cpp` (MODIFY) | command-impl | request-response pipeline | `cli/src/commands.cpp:483-674` (`put` fan-out pipeline) + `cli/src/commands.cpp:676-900` (`get` fan-out pipeline) + `cli/src/commands.cpp:907-1092` (`rm` single-tombstone flow) | exact |
| `cli/src/main.cpp` (MODIFY) | argv-dispatcher | N/A | `cli/src/main.cpp:419-495` (`cdb get` multi-hash argv loop — the target shape for `rm`) + `main.cpp:358-411` (`put` loop — target shape for `--name`/`--replace`) | exact |
| `db/tests/test_helpers.h` (MODIFY) | test-fixture | N/A | `db/tests/test_helpers.h:99-117` (`make_signed_tombstone`) + `:165-202` (`make_pubk_blob`) + `:120-137` (`make_signed_delegation`) | exact |
| `db/tests/engine/test_bomb_validation.cpp` (NEW) | engine-test | validation | `db/tests/engine/test_pubk_first.cpp` (Step 1.5 gate tests) | exact structural template |
| `db/tests/engine/test_bomb_side_effects.cpp` (NEW) | engine-test | side-effect | Existing tombstone-delete side-effect tests in `test_engine.cpp` (implied — same Step 3.5 code path) + `test_pubk_first.cpp` setup scaffolding | role-match |
| `db/tests/engine/test_name_resolution.cpp` (NEW) | engine-test (resolution helper) | transform | `db/tests/engine/test_verify_signer_hint.cpp` (in-process engine + storage + identity setup) | role-match |
| `db/tests/engine/test_bomb_delegate.cpp` (NEW — or fold into test_bomb_validation) | engine-test | delegate-rejection | `db/tests/engine/test_delegate_replay.cpp` (owner A + owner B + delegate D scaffolding) | exact structural template |
| `db/tests/wire/test_name_bomb_codec.cpp` (NEW) or extend `test_codec.cpp` | wire-test | transform | `db/tests/wire/test_codec.cpp:1-77` (round-trip tests, deterministic encode) | exact |
| `db/CMakeLists.txt` (MODIFY) | build-config | N/A | `db/CMakeLists.txt:220-261` (explicit file list in `add_executable(chromatindb_tests ...)`) — NOT GLOB | exact (append new test file paths) |

---

## Pattern Assignments

### `db/wire/codec.h` + `db/wire/codec.cpp` (MODIFY) — new NAME + BOMB magics, helpers, encoders

**Analog:** `db/wire/codec.h:87-152` + `db/wire/codec.cpp:133-176` (existing TOMBSTONE + DELEGATION + PUBKEY blocks).

**Magic-constant idiom** (codec.h:92, :112, :135):

```cpp
/// 4-byte magic prefix identifying tombstone data.
inline constexpr std::array<uint8_t, 4> TOMBSTONE_MAGIC = {0xDE, 0xAD, 0xBE, 0xEF};
/// Tombstone data size: 4-byte magic + 32-byte target hash.
inline constexpr size_t TOMBSTONE_DATA_SIZE = 36;
```

**Helper signature idiom** (codec.h:97-105):

```cpp
/// Check if blob data is a tombstone (magic prefix + 32-byte target hash).
bool is_tombstone(std::span<const uint8_t> data);

/// Extract the 32-byte target blob hash from tombstone data.
/// @pre is_tombstone(data) must be true.
std::array<uint8_t, 32> extract_tombstone_target(std::span<const uint8_t> data);

/// Create 36-byte tombstone data: magic prefix + target hash.
std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash);
```

**PUBK inline variant** (codec.h:140-152) — template for `is_name()` / `is_bomb()` if we choose inline:

```cpp
inline bool is_pubkey_blob(std::span<const uint8_t> data) {
    return data.size() == PUBKEY_DATA_SIZE &&
           data[0] == PUBKEY_MAGIC[0] && data[1] == PUBKEY_MAGIC[1] &&
           data[2] == PUBKEY_MAGIC[2] && data[3] == PUBKEY_MAGIC[3];
}
```

**Implementation idiom** (codec.cpp:137-176):

```cpp
bool is_tombstone(std::span<const uint8_t> data) {
    if (data.size() != TOMBSTONE_DATA_SIZE) return false;
    return std::memcmp(data.data(), TOMBSTONE_MAGIC.data(), TOMBSTONE_MAGIC.size()) == 0;
}

std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash) {
    std::vector<uint8_t> result;
    result.reserve(TOMBSTONE_DATA_SIZE);
    result.insert(result.end(), TOMBSTONE_MAGIC.begin(), TOMBSTONE_MAGIC.end());
    result.insert(result.end(), target_hash.begin(), target_hash.end());
    return result;
}
```

**What to replicate verbatim:**
- `inline constexpr std::array<uint8_t, 4> NAME_MAGIC = {0x4E, 0x41, 0x4D, 0x45};`
- `inline constexpr std::array<uint8_t, 4> BOMB_MAGIC = {0x42, 0x4F, 0x4D, 0x42};`
- Out-of-line declarations for `is_*` / `extract_*` / `make_*` / validator helpers. Choose out-of-line (matches TOMB/DLGT) over inline (PUBK) because NAME/BOMB validators are non-trivial (BE-length reads + variable-length payload validation).
- `NamePayload` struct + `parse_name_payload` / `make_name_data` / `validate_bomb_structure` / `extract_bomb_targets` / `make_bomb_data` per RESEARCH §Code Examples Example 1.

**What must differ:**
- **Not a fixed-size payload.** NAME payload is `[magic:4][name_len:2 BE][name:N][target_hash:32]` — variable length. `is_name(data)` must do `data.size() >= NAME_MIN_DATA_SIZE (= 4+2+32=38)` AND read `name_len` BE from `data[4..6]` AND require `data.size() == 4 + 2 + name_len + 32`. Tombstone's `data.size() != TOMBSTONE_DATA_SIZE` one-liner does NOT apply.
- **BOMB size is count-derived.** `is_bomb(data)` must `data.size() >= 8 && read_u32_be(data+4) * 32 + 8 == data.size()`.
- **Need big-endian load helpers.** Use the project's existing `chromatindb::util::read_u32_be` (per engine.cpp:307 in the RESEARCH excerpt) and `read_u16_be` from `db/util/endian.h` — do NOT hand-roll.

---

### `cli/src/wire.h` + `cli/src/wire.cpp` (MODIFY) — mirror NAME + BOMB magics on the CLI side

**Analog:** `cli/src/wire.h:196-308` (`make_tombstone_data` decl at :197, `make_delegation_data` at :204, PUBK magic + size + `is_pubkey_blob` + `make_pubkey_data` at :210-223, `type_label` at :286-294, `is_hidden_type` at :303-308).

**Magic mirror idiom** (wire.h:267-282):

```cpp
/// CENV (client envelope) magic: "CENV" in ASCII
inline constexpr std::array<uint8_t, 4> CENV_MAGIC = {0x43, 0x45, 0x4E, 0x56};

/// CDAT (chunk data) magic: "CDAT" in ASCII (Phase 119)
inline constexpr std::array<uint8_t, 4> CDAT_MAGIC = {0x43, 0x44, 0x41, 0x54};

/// CPAR (chunked manifest) magic: "CPAR" in ASCII (Phase 119, CHUNK-02).
inline constexpr std::array<uint8_t, 4> CPAR_MAGIC = {0x43, 0x50, 0x41, 0x52};
```

**`type_label` extension point** (wire.h:286-294):

```cpp
inline const char* type_label(const uint8_t* type) {
    if (std::memcmp(type, CENV_MAGIC.data(), 4) == 0) return "CENV";
    if (std::memcmp(type, PUBKEY_MAGIC.data(), 4) == 0) return "PUBK";
    if (std::memcmp(type, TOMBSTONE_MAGIC_CLI.data(), 4) == 0) return "TOMB";
    if (std::memcmp(type, DELEGATION_MAGIC_CLI.data(), 4) == 0) return "DLGT";
    if (std::memcmp(type, CDAT_MAGIC.data(), 4) == 0) return "CDAT";
    if (std::memcmp(type, CPAR_MAGIC.data(), 4) == 0) return "CPAR";
    return "DATA";
}
```

**`is_hidden_type`** (wire.h:303-308) — target site for RESEARCH Pitfall 6:

```cpp
inline bool is_hidden_type(const uint8_t* type) {
    if (std::memcmp(type, PUBKEY_MAGIC.data(), 4) == 0) return true;
    if (std::memcmp(type, CDAT_MAGIC.data(), 4) == 0) return true;
    if (std::memcmp(type, DELEGATION_MAGIC_CLI.data(), 4) == 0) return true;
    return false;
}
```

**CLI impl idiom** (wire.cpp:277):

```cpp
std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash) {
    std::vector<uint8_t> result;
    result.reserve(36);
    result.insert(result.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    result.insert(result.end(), target_hash.begin(), target_hash.end());
    return result;
}
```

**What to replicate:**
- `NAME_MAGIC_CLI = {0x4E, 0x41, 0x4D, 0x45}` and `BOMB_MAGIC_CLI = {0x42, 0x4F, 0x4D, 0x42}` under the same "phase 117 type labels" block (wire.h:264-308).
- Add `"NAME"` and `"BOMB"` branches to `type_label`.
- Add NAME to `is_hidden_type` (per RESEARCH Pitfall 6 + A4; BOMB stays visible like TOMB).
- Declare `make_name_data` / `make_bomb_data` in wire.h (alongside `make_tombstone_data` at :197).
- Implement both in wire.cpp following the existing `make_tombstone_data` idiom; use `store_u16_be` / `store_u32_be` helpers already at wire.h:18-28.

**What must differ:**
- **Name bytes are caller-opaque.** The `name` parameter is `std::span<const uint8_t>` (NOT `std::string`) — per D-04, names may contain non-UTF-8 bytes; treat as raw bytes throughout.
- **Length prefix is 2-byte BE** for NAME (matches D-03) vs. no length prefix for tombstone/delegation.
- **CLI BlobData is still pre-122** (`namespace_id`, `pubkey` fields — wire.h:132-139). Per RESEARCH constraint #1 + A5, Phase 123 CLI stays on pre-122 wire; Phase 124 adapts both 122 and 123 CLI deltas together. Do NOT upgrade CLI wire format in this phase.

---

### `db/engine/engine.cpp` (MODIFY) — BOMB structural validation + delegate-reject + side-effect + max_ttl exemption

**Four surgical insertion sites**, each with a distinct analog block already in the file.

**Site 1: Step 0e max_ttl exemption** (engine.cpp:160-172, current):

```cpp
// Step 0e: Max TTL enforcement (tombstones exempt — must be permanent)
if (max_ttl_seconds_ > 0 && !wire::is_tombstone(blob.data)) {
    if (blob.ttl == 0) {
        co_return IngestResult::rejection(IngestError::invalid_ttl,
            "permanent blobs not allowed (max_ttl_seconds=" +
            std::to_string(max_ttl_seconds_) + ")");
    }
    if (blob.ttl > max_ttl_seconds_) {
        co_return IngestResult::rejection(IngestError::invalid_ttl,
            "TTL " + std::to_string(blob.ttl) +
            " exceeds max " + std::to_string(max_ttl_seconds_));
    }
}
```

**Replicate:** Extend predicate to `!wire::is_tombstone(blob.data) && !wire::is_bomb(blob.data)` so BOMB is exempted alongside tombstone (both are permanent by D-13(1)).

**Site 2: NEW Step 1.7 BOMB structural validation** — insert between existing Step 1.5 (engine.cpp:181-195) and Step 2 (engine.cpp:197).

**Analog for the PRE-OFFLOAD DISCIPLINE:** Step 1.5 PUBK-first gate at engine.cpp:181-195:

```cpp
// Step 1.5 (Phase 122 D-03): PUBK-first invariant.
// Runs BEFORE any crypto::offload — the adversarial-flood defense (Pitfall #6):
// no registered owner for target_namespace AND inbound blob is not a PUBK => reject
// without burning ML-DSA-87 verify CPU.
if (!storage_.has_owner_pubkey(target_namespace)) {
    if (!wire::is_pubkey_blob(blob.data)) {
        spdlog::warn("Ingest rejected: PUBK-first violation (ns {:02x}{:02x}... has no registered owner)",
                     target_namespace[0], target_namespace[1]);
        co_return IngestResult::rejection(IngestError::pubk_first_violation,
            "first write to namespace must be PUBK");
    }
}
```

**Replicate:** Structural template only. The BOMB block:

```cpp
// Step 1.7 (Phase 123 D-13): BOMB structural validation.
// Runs BEFORE Step 2/3 signer resolution + verify — adversarial-flood defense.
if (wire::is_bomb(blob.data)) {
    if (blob.ttl != 0) {
        co_return IngestResult::rejection(IngestError::invalid_ttl,
            "BOMB must have ttl=0 (permanent)");
    }
    if (blob.data.size() < 8) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "BOMB data too short for header");
    }
    uint32_t count = chromatindb::util::read_u32_be(blob.data.data() + 4);
    size_t expected = size_t{8} + size_t{count} * 32u;
    if (blob.data.size() != expected) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "BOMB data size mismatch");
    }
}
```

**What must differ from PUBK-first gate:**
- PUBK-first guards a *namespace-level* precondition; BOMB block guards a *blob-level* structural invariant (ttl + size + count consistency).
- No `has_owner_pubkey` lookup — BOMB's checks are pure integer math on `blob.data`.
- Uses `IngestError::invalid_ttl` / `IngestError::malformed_blob` (existing enum values — confirm against engine.h; re-use same errors the tombstone/PUBK checks use rather than adding new IngestError variants).

**Site 3: Step 2 delegate-rejection extension** (engine.cpp:266-280, current):

```cpp
if (is_delegate) {
    // Delegates cannot create delegation blobs (only owners can)
    if (wire::is_delegation(blob.data)) {
        spdlog::warn("Ingest rejected: delegates cannot create delegation blobs");
        co_return IngestResult::rejection(IngestError::no_delegation,
            "delegates cannot create delegation blobs");
    }

    // Delegates cannot create tombstone blobs (deletion is owner-privileged)
    if (wire::is_tombstone(blob.data)) {
        spdlog::warn("Ingest rejected: delegates cannot create tombstone blobs");
        co_return IngestResult::rejection(IngestError::no_delegation,
            "delegates cannot create tombstone blobs");
    }
}
```

**Replicate:** Append a third branch:

```cpp
    // Delegates cannot create BOMB blobs (batched deletion is owner-privileged; D-12)
    if (wire::is_bomb(blob.data)) {
        spdlog::warn("Ingest rejected: delegates cannot create BOMB blobs");
        co_return IngestResult::rejection(IngestError::no_delegation,
            "delegates cannot create BOMB blobs");
    }
```

**What must differ:** nothing. This is a structural-identity copy; only the magic-check function and error message string change.

**Site 4: Step 3.5 BOMB side-effect** (engine.cpp:349-366, current tombstone side-effect):

```cpp
// Step 3.5: Tombstone handling for incoming blobs
if (wire::is_tombstone(blob.data)) {
    auto target_hash = wire::extract_tombstone_target(blob.data);
    storage_.delete_blob_data(target_namespace, target_hash);
    spdlog::debug("Tombstone received: deleting target blob in ns {:02x}{:02x}...",
                   target_namespace[0], target_namespace[1]);
} else {
    if (storage_.has_tombstone_for(target_namespace, content_hash)) {
        co_return IngestResult::rejection(IngestError::tombstoned,
            "blocked by tombstone");
    }
}
```

**Replicate:** Convert the `if/else` to `if/else if/else`:

```cpp
if (wire::is_tombstone(blob.data)) {
    auto target_hash = wire::extract_tombstone_target(blob.data);
    storage_.delete_blob_data(target_namespace, target_hash);
} else if (wire::is_bomb(blob.data)) {
    auto targets = wire::extract_bomb_targets(blob.data);  // vector<array<u8,32>>
    for (const auto& th : targets) {
        storage_.delete_blob_data(target_namespace, th);
    }
    spdlog::debug("BOMB received: deleting {} target blobs in ns {:02x}{:02x}...",
                   targets.size(), target_namespace[0], target_namespace[1]);
} else {
    if (storage_.has_tombstone_for(target_namespace, content_hash)) {
        co_return IngestResult::rejection(IngestError::tombstoned,
            "blocked by tombstone");
    }
}
```

**What must differ:**
- BOMB iterates N `delete_blob_data` calls on the ioc thread (Phase 121 STORAGE_THREAD_CHECK; engine is already on executor after CONC-04 post-back at :340). No new coroutine / offload.
- **Has-tombstone check is skipped for BOMBs** — a BOMB itself is a batched tombstone and is never "blocked by tombstone".
- RESEARCH A3: per-target MDBX txns; YAGNI a batch helper until measured.

**Error-handling pattern** (engine.cpp:113-116, :176-178) — applies to all four sites:

```cpp
co_return IngestResult::rejection(IngestError::<variant>, "<human-readable reason>");
```

All four BOMB-related rejections follow this one-line shape — no try/catch needed.

---

### `db/peer/error_codes.h` (MODIFY) — add BOMB rejection codes

**Analog:** `db/peer/error_codes.h:16-17, :21-33` (full file is the template; ~40 LOC).

**Enum-addition pattern** (error_codes.h:10-17):

```cpp
constexpr uint8_t ERROR_MALFORMED_PAYLOAD    = 0x01;
// ... gap ...
constexpr uint8_t ERROR_PUBK_FIRST_VIOLATION = 0x07;  // Phase 122 D-03
constexpr uint8_t ERROR_PUBK_MISMATCH        = 0x08;  // Phase 122 D-04
```

**Switch-table extension** (error_codes.h:21-33):

```cpp
constexpr std::string_view error_code_string(uint8_t code) {
    switch (code) {
        // ...existing cases...
        case ERROR_PUBK_FIRST_VIOLATION: return "pubk_first_violation";
        case ERROR_PUBK_MISMATCH:        return "pubk_mismatch";
        default:                         return "unknown";
    }
}
```

**Replicate:**
- `ERROR_BOMB_TTL_NONZERO = 0x09;  // Phase 123 D-13(1): BOMB must have ttl=0`
- `ERROR_BOMB_MALFORMED = 0x0A;    // Phase 123 D-13(2): BOMB structural sanity failed`
- `ERROR_BOMB_DELEGATE_NOT_ALLOWED = 0x0B;  // Phase 123 D-12: delegates cannot BOMB`
- Three new `case ...: return "...";` rows in both `error_code_string` and (implicitly) `error_code_name` alias at :37.

**What must differ:** nothing — pure append.

**Caveat:** RESEARCH notes that the engine rejection uses `IngestError::invalid_ttl` / `malformed_blob` / `no_delegation` (existing `IngestError` enum values). These three BOMB-specific **wire** codes are for the `ErrorResponse(63)` path emitted by the dispatcher when it serializes an engine rejection for the client. The planner must confirm whether the dispatcher's rejection translator (`IngestError → wire code`) needs any new branches — or whether re-using `ERROR_VALIDATION_FAILED` / `ERROR_MALFORMED_PAYLOAD` is acceptable per `feedback_no_backward_compat.md` / minimal-surface.

---

### `db/peer/message_dispatcher.cpp` (PROBABLY NO CHANGE)

**Analog / reason for no-change:** `db/peer/message_dispatcher.cpp:473-585` (existing `ListRequest` branch with `type_filter`).

**The already-built enumeration mechanism** (dispatcher:486-535):

```cpp
// Optional flags byte at offset 44 (length-based detection)
uint8_t flags = 0;
std::array<uint8_t, 4> type_filter{};
bool has_type_filter = false;
bool include_all = false;

if (payload.size() >= 45) {
    flags = payload[44];
    include_all = (flags & 0x01) != 0;  // bit 0: include_all
}
if (payload.size() >= 49 && (flags & 0x02) != 0) {
    std::memcpy(type_filter.data(), payload.data() + 45, 4);
    has_type_filter = true;  // bit 1: type_filter present
}
// ...
if (has_type_filter) {
    if (std::memcmp(ref.blob_type.data(), type_filter.data(), 4) != 0) {
        continue;
    }
}
```

**Apply per D-10:** The CLI's `cdb get <name>` sends `ListRequest` with `flags = 0x02` (type_filter present) + `type_filter = NAME_MAGIC`. Server filters on `seq_map.value[32..36]` — already pre-indexed from Phase 117 TYPE-03 at storage.cpp:463-471 (`seq_map: [hash:32][type:4]`).

**What to replicate:** nothing server-side. CLI issues `MsgType::ListRequest` with the flag-byte-plus-4-bytes trailer; server already knows what to do.

**Caveat:** RESEARCH A1 flags this as a planner decision vs. adding a dedicated `ListByMagic=65` type. Recommendation is reuse (zero new dispatcher branch, zero new FlatBuffers table). Planner should make this call explicit in PLAN.md and surface it to the user.

---

### `db/storage/storage.{h,cpp}` (PROBABLY NO CHANGE)

**Analog:** `db/storage/storage.cpp:463-471` (seq_map value layout `[hash:32][type:4]` from Phase 117).

```cpp
// Extract 4-byte type prefix from blob data
auto blob_type = wire::extract_blob_type(std::span<const uint8_t>(blob.data));

// Store in seq_map: value is [hash:32][type:4] = 36 bytes
std::array<uint8_t, 36> seq_value;
std::memcpy(seq_value.data(), precomputed_hash.data(), 32);
std::memcpy(seq_value.data() + 32, blob_type.data(), 4);
txn.upsert(impl_->seq_map, to_slice(seq_key),
            mdbx::slice(seq_value.data(), seq_value.size()));
```

**Why no change:** NAME and BOMB blobs flow through `store_blob` unchanged; `extract_blob_type` (codec.h:160) already reads the first 4 bytes of `blob.data` and writes them to seq_map. NAME and BOMB enumeration thus rides the existing `get_blob_refs_since` + dispatcher filter without any storage surface extension.

**What to replicate:** nothing.

**Caveat:** If planner decides to add a batch-delete optimization (`Storage::delete_blobs_data_batch`) to make big BOMBs atomic within one MDBX txn, the analog is `storage.cpp:440-520` store_blob txn shape (open txn, upsert N keys, commit). RESEARCH A3 flags this as YAGNI.

---

### `cli/src/commands.h` + `cli/src/commands.cpp` (MODIFY) — `put --name [--replace]`, new `get_by_name`, multi-target `rm`

**Signature change (commands.h:30-47):**

Current:
```cpp
int put(const std::string& identity_dir, const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin, const ConnectOpts& opts);

int rm(const std::string& identity_dir, const std::string& hash_hex,
       const std::string& namespace_hex, bool force, const ConnectOpts& opts);
```

Target:
```cpp
int put(const std::string& identity_dir, const std::vector<std::string>& file_paths,
        const std::vector<std::string>& share_pubkey_files,
        uint32_t ttl, bool from_stdin,
        const std::string& name, bool replace,   // NEW
        const ConnectOpts& opts);

int rm(const std::string& identity_dir,
       const std::vector<std::string>& hash_hexes,  // was: const std::string& hash_hex
       const std::string& namespace_hex, bool force, const ConnectOpts& opts);

// NEW
int get_by_name(const std::string& identity_dir, const std::string& name,
                const std::string& namespace_hex, bool to_stdout,
                const std::string& output_dir, bool force_overwrite,
                const ConnectOpts& opts);
```

**`put` pipelining pattern** (commands.cpp:580-674) — the analog for how a NAME blob should be emitted alongside the content blob in the SAME connection:

```cpp
while (completed < files.size()) {
    // Phase A: greedy fill the window.
    if (next_to_send < files.size() &&
        rid_to_index.size() < Connection::kPipelineDepth) {
        // ...
        auto flatbuf = encode_blob(blob);
        uint32_t this_rid = rid++;
        if (!conn.send_async(MsgType::Data, flatbuf, this_rid)) { /* ... */ }
        rid_to_index[this_rid] = next_to_send;
        ++next_to_send;
        continue;
    }
    // Phase B: drain one reply in arrival order.
    auto resp = conn.recv_next();
    // ...
}
```

**Single-blob signing pattern** (commands.cpp:591-603):

```cpp
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
```

**`rm` tombstone build + send pattern** (commands.cpp:1029-1048) — the analog for the BOMB send inside the new multi-target `rm`:

```cpp
// Build and send tombstone
auto tombstone_data = make_tombstone_data(
    std::span<const uint8_t, 32>(target_hash.data(), 32));

auto ns_span = std::span<const uint8_t, 32>(ns.data(), 32);
auto timestamp = static_cast<uint64_t>(std::time(nullptr));
auto signing_input = build_signing_input(ns_span, tombstone_data, 0, timestamp);
auto signature = id.sign(signing_input);

BlobData blob{};
blob.namespace_id = ns;
blob.pubkey.assign(id.signing_pubkey().begin(), id.signing_pubkey().end());
blob.data = std::move(tombstone_data);
blob.ttl = 0;
blob.timestamp = timestamp;
blob.signature = std::move(signature);

auto flatbuf = encode_blob(blob);

if (!conn.send(MsgType::Delete, flatbuf, rid_counter++)) { /* ... */ }
```

**`get` fan-out pattern** (commands.cpp:699-775) — the analog for the winner-fetch step inside `get_by_name`:

```cpp
std::vector<uint8_t> payload(64);
std::memcpy(payload.data(), ns.data(), 32);
std::memcpy(payload.data() + 32, hash.data(), 32);
uint32_t this_rid = rid++;
if (!conn.send_async(MsgType::ReadRequest, payload, this_rid)) { /* ... */ }
// ... later ...
auto blob_bytes = std::span<const uint8_t>(
    resp->payload.data() + 1, resp->payload.size() - 1);
auto blob = decode_blob(blob_bytes);
```

**What to replicate:**
- **`put --name`:** after building the content blob (commands.cpp:588-612), build a second BlobData with `data = make_name_data(name_bytes, content_blob_hash)`, `ttl = ttl` (RESEARCH: NAME may be TTL-bounded per the writer's choice; D-03 doesn't require ttl=0), same timestamp, fresh signature over `build_signing_input(ns, name_data, ttl, timestamp)`. Send in the same pipeline window.
- **`put --name --replace`:** BEFORE building the new NAME, issue a `get_by_name` to discover the current NAME's `target_content_hash`, then produce a BOMB-of-1 (per RESEARCH recommendation over single-tombstone; `feedback_no_duplicate_code.md`) for that prior content hash. Three blobs emitted in one pipelined window: content, NAME, BOMB.
- **`get_by_name`:** (1) send `ListRequest` with `type_filter = NAME_MAGIC`, collect `BlobRef[]`. (2) Pipeline N × ReadRequest for candidate NAME bodies (reuse the `get` fan-out loop verbatim). (3) For each decoded NAME blob, call `parse_name_payload(blob.data)`; discard if `parsed.name != requested_name` (memcmp on raw bytes — D-04). (4) Sort: `timestamp DESC`, then `content_hash DESC` (D-01 / D-02). (5) ReadRequest the winner's `target_hash` and reuse `cmd::get` to render output.
- **multi-target `rm`:** replace the per-target tombstone build with a single BOMB build covering all `hash_hexes`. Use `make_bomb_data(targets)` instead of `make_tombstone_data`. Send via `MsgType::Delete` envelope — the envelope type is unchanged (BOMB rides the same Delete=17 dispatch path per RESEARCH §Architecture Diagram).

**What must differ:**
- **Skip the CPAR/CDAT cascade for multi-target rm.** The current `rm` at commands.cpp:968-1027 does a per-target ReadRequest to detect CPAR manifest and delegates to `chunked::rm_chunked`. For multi-target rm, either (a) skip the cascade entirely — `cdb rm A B C` requires user to know they're deleting user-content hashes, not chunked manifests — or (b) run the CPAR cascade per-target before batching the remaining hashes into one BOMB. Planner decides; RESEARCH §State of the Art recommends option (a), leaving the single-target CPAR cascade to the current `rm` path via a `cdb rm <single-hash>` invocation (count=1).
- **Pre-check existence is incompatible with batch efficiency.** Current code at commands.cpp:927-953 does a per-target ExistsRequest. For multi-target rm with `--force`, skip. For multi-target rm without `--force`, either pipeline N ExistsRequests first (RESEARCH A7) or hard-require `--force` when `hash_hexes.size() > 1`. Planner picks the UX.
- **CLI stays on pre-122 wire.** All `BlobData` constructions use `blob.namespace_id` + `blob.pubkey` (pre-122 fields at wire.h:132-139). Per RESEARCH constraint #1 + A5, do NOT upgrade to post-122 shape; Phase 124 does that uniformly.

---

### `cli/src/main.cpp` (MODIFY) — argv dispatcher for `put --name [--replace]`, new `get` name path, multi-target `rm`

**Analog 1: multi-hash argv loop** (main.cpp:430-467, `cdb get` currently):

```cpp
std::vector<std::string> hash_hexes;
while (arg_idx < argc) {
    const char* a = argv[arg_idx];
    if (std::strcmp(a, "--stdout") == 0) {
        to_stdout = true;
        ++arg_idx;
    } else if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 || std::strcmp(a, "--from") == 0) {
        if (arg_idx + 1 >= argc) { /* error */ return 1; }
        namespace_hex = argv[++arg_idx];
        ++arg_idx;
    } else if (/* other flags */) { /* ... */ }
    else if (a[0] != '-') {
        if (std::strlen(a) != 64) {
            std::fprintf(stderr, "Error: invalid hash: %s\n", a);
            return 1;
        }
        hash_hexes.emplace_back(a);
        ++arg_idx;
    } else {
        std::fprintf(stderr, "Unknown get option: %s\n", a);
        return 1;
    }
}
```

**Replicate for multi-target `rm`:** current `rm` loop at main.cpp:513-535 has the known-bug (RESEARCH Pitfall 4) `} else if (a[0] != '-' && hash_hex.empty()) { hash_hex = a; ++arg_idx; }` — only accepts one. Replace with the `cdb get` multi-hash shape above, producing `std::vector<std::string> hash_hexes`. Tweak the confirmation prompt to list all targets.

**Analog 2: flag-plus-value parse** (main.cpp:377-383, `cdb put` `--ttl`):

```cpp
} else if (std::strcmp(a, "--ttl") == 0) {
    if (arg_idx + 1 >= argc) {
        std::fprintf(stderr, "Error: --ttl requires a value\n");
        return 1;
    }
    ttl = parse_ttl(argv[++arg_idx]);
    ++arg_idx;
}
```

**Replicate for `--name <NAME>`:** add a `--name` branch that stores the value into a `std::string name_arg`; `--replace` is a bare boolean flag (no value) following the `--stdin` pattern at main.cpp:384-386.

**Analog 3: flag-only boolean** (main.cpp:384-386):

```cpp
} else if (std::strcmp(a, "--stdin") == 0) {
    from_stdin = true;
    ++arg_idx;
}
```

**Replicate for `--replace`:**

```cpp
} else if (std::strcmp(a, "--replace") == 0) {
    replace = true;
    ++arg_idx;
}
```

**What to replicate:**
- `cdb put` argv loop (main.cpp:368-402): add `--name <str>` and `--replace` branches. Pass both to `cmd::put`.
- Usage string update (main.cpp:360): `Usage: cdb put <file>... [--name <name>] [--replace] [--share <...>]...`
- `cdb rm` argv loop (main.cpp:513-535): replace single-target parse with multi-target vector (copy from `cdb get` at :430-467).
- `cdb get` branch: detect "is this a hash (64 hex) or a name (anything else)?" — if not 64 hex, route to `cmd::get_by_name`. Alternative: add a separate `cdb getbyname <name>` subcommand for zero-ambiguity. RESEARCH §Recommended Project Structure floats both; planner's call. User D-04 names may contain arbitrary bytes (no hex restriction), so sniffing "64-char lowercase hex" works reliably for the disambiguation test.

**What must differ:**
- `--name`'s value is stored as `std::string` at the argv layer, but the value is interpreted as `std::span<const uint8_t>` opaque bytes downstream (D-04). Shells pass bytes fine; no encoding conversion needed.
- **Multi-target rm confirmation:** the current prompt is `"Delete blob <hash>? [y/N]"`. Multi-target should say `"Delete N blobs: <list-or-summary>? [y/N]"`. Planner decides verbosity.

---

### `db/tests/test_helpers.h` (MODIFY) — add `make_name_blob` and `make_bomb_blob`

**Analog:** `db/tests/test_helpers.h:99-117` (`make_signed_tombstone`) + `:120-137` (`make_signed_delegation`) + `:165-202` (`make_pubk_blob`).

**`make_signed_tombstone` template** (test_helpers.h:99-117):

```cpp
inline wire::BlobData make_signed_tombstone(
    const identity::NodeIdentity& id,
    const std::array<uint8_t, 32>& target_blob_hash,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData tombstone;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(tombstone.signer_hint.data(), hint.data(), 32);
    tombstone.data = wire::make_tombstone_data(target_blob_hash);
    tombstone.ttl = 0;  // Permanent
    tombstone.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = wire::build_signing_input(
        id.namespace_id(), tombstone.data, tombstone.ttl, tombstone.timestamp);
    tombstone.signature = id.sign(signing_input);

    return tombstone;
}
```

**What to replicate:**

```cpp
inline wire::BlobData make_name_blob(
    const identity::NodeIdentity& id,
    std::span<const uint8_t> name,
    std::span<const uint8_t, 32> target_hash,
    uint32_t ttl = 0,  // NAME defaults permanent; caller may override
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_name_data(name, target_hash);
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;
    auto si = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(si);
    return blob;
}

inline wire::BlobData make_bomb_blob(
    const identity::NodeIdentity& id,
    std::span<const std::array<uint8_t, 32>> targets,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_bomb_data(targets);
    blob.ttl = 0;  // D-13(1) mandatory
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;
    auto si = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(si);
    return blob;
}
```

**What must differ:**
- `make_name_blob` accepts `name` as `std::span<const uint8_t>` (opaque bytes per D-04) — no `std::string` convenience overload unless tests insist.
- `make_bomb_blob` has no `ttl` parameter — always 0 (D-13(1)). A test that wants to build a ttl!=0 BOMB to prove ingest rejection should hand-build the BlobData (not use this helper).
- A delegate-BOMB helper — if needed for test_bomb_delegate.cpp — follows the `make_delegate_blob` shape at test_helpers.h:142-163 (signer_hint = SHA3(delegate_pk), target_namespace = owner.namespace_id()).

---

### `db/tests/engine/test_bomb_validation.cpp` (NEW) — D-13 ingest rejection tests

**Analog:** `db/tests/engine/test_pubk_first.cpp` (entire file — same Catch2 pattern, same Step 1.5/1.7 pre-offload rejection shape).

**Test-case scaffolding** (test_pubk_first.cpp:40-57):

```cpp
TEST_CASE("non-PUBK first write to fresh namespace rejected with pubk_first_violation",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    // Deliberately DO NOT register_pubk or ingest a PUBK first.
    auto blob = make_signed_blob(id, "no-pubk-first");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));

    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::pubk_first_violation);
    REQUIRE(store.count_owner_pubkeys() == 0);
}
```

**What to replicate:**
- File header block: `// Phase 123-... BOMB validation coverage — D-13.` + VALIDATION.md anchor comment + `[phase123][bomb][engine]` tag.
- Imports: same set (`engine.h`, `identity.h`, `storage.h`, `codec.h`, `test_helpers.h`).
- Scaffolding: `TempDir tmp; Storage store(tmp.path.string()); asio::thread_pool pool{1}; BlobEngine engine(store, pool);` — identical.
- `register_pubk(store, id)` (test_helpers.h:229) at the top of every BOMB test — BOMB-first-to-a-fresh-ns would be caught by PUBK-first gate BEFORE D-13 fires; call register_pubk to bypass and isolate the D-13 behavior.
- Test cases:
  - D-13(1): BOMB with `ttl != 0` → `IngestError::invalid_ttl` (build with `make_bomb_blob` then mutate `.ttl = 3600` and re-sign via a manual signing step, or construct BlobData manually).
  - D-13(2a): BOMB with `data.size() < 8` → `IngestError::malformed_blob` (construct BlobData, set `data = {0x42, 0x4F, 0x4D, 0x42, 0x00}` — truncated — then re-sign manually).
  - D-13(2b): BOMB with `data.size() != 8 + count*32` → same error (construct with `count = 5` but `data.size() = 8 + 4*32`).
  - D-13(3): BOMB from delegate → `IngestError::no_delegation`. Use the owner-A / owner-B / delegate scaffolding from test_delegate_replay.cpp (see next entry).
  - Happy-path BOMB(count=3): accepted, blob stored, 3 targets tombstoned. (Or split into test_bomb_side_effects.cpp.)

**What must differ:**
- D-13 rejections fire BEFORE Step 2 signer resolution, so `register_pubk` is the minimum setup. No need to ingest PUBK or delegation for the ttl/malformed tests — a mal-formed BOMB should be rejected pre-offload regardless of signer status. (The delegate-reject test IS an exception — it requires owner PUBK + delegation registered first.)
- Tag convention: `[phase123][bomb][engine]` (RESEARCH §Validation Architecture).

---

### `db/tests/engine/test_bomb_side_effects.cpp` (NEW) — BOMB(count=N) deletes N targets

**Analog:** `db/tests/engine/test_pubk_first.cpp:61-83` (PUBK-then-blob accepted scaffolding) + an existing BOMB-of-1 happy-path analog would live in `db/tests/engine/test_engine.cpp` (single-tombstone delete side-effect) — planner inspect.

**What to replicate:**
- PUBK-ingest-first scaffolding to clear the PUBK-first gate.
- Ingest N content blobs, collect their blob_hashes.
- Build `make_bomb_blob(id, {hash1, hash2, ..., hashN})`.
- Ingest the BOMB → `REQUIRE(result.accepted)`.
- For each target hash: `REQUIRE_FALSE(store.has_blob(ns, hash_i))` (storage side-effect proven).
- Subsequent re-ingest of a content blob matching one of the tombstoned hashes → rejected with `IngestError::tombstoned` (BOMB acts as a tombstone for has_tombstone_for lookups — confirm via storage method call chain or live engine ingest).

**What must differ:** idempotent re-ingest of the same BOMB should short-circuit at the dedup check at engine.cpp:315 (content_hash match) — the second BOMB does NOT re-iterate the delete loop. Exercise this as one of the test cases.

---

### `db/tests/engine/test_name_resolution.cpp` (NEW) — D-01 + D-02 resolution

**Analog:** `db/tests/engine/test_verify_signer_hint.cpp` (in-process engine + storage + identity) — same scaffolding.

**Resolution is a CLIENT-SIDE algorithm**, not engine logic (see RESEARCH §Architecture Pattern 1). However, the algorithm can be tested at the engine layer by:
1. Ingesting 2+ NAME blobs with different timestamps pointing at different target hashes.
2. Enumerating via `storage_.get_blob_refs_since(ns, 0, limit)` with a manual type_filter on `blob_type == NAME_MAGIC`.
3. For each ref, `storage_.get_blob(ns, ref.blob_hash)`, decode payload, apply the D-01/D-02 sort, assert the winner.

**What to replicate from test_verify_signer_hint.cpp:30-60:**
- TempDir / Storage / BlobEngine / NodeIdentity setup.
- `make_pubk_blob(id)` + `run_async(pool, engine.ingest(...))` to establish namespace.
- Then a sequence of `engine.ingest(ns_span(id), make_name_blob(id, name, target_hash, ttl=0, timestamp=T))` with varying T and varying target_hash.
- Helper function `resolve_name(store, ns, name) -> optional<array<u8,32>>` that replicates the client algorithm (enumerate via `get_blob_refs_since`, filter by NAME_MAGIC, fetch bodies, parse, sort, winner).
- Test cases:
  - Two NAME blobs for "foo" with T1 < T2, target_hash_1 vs. target_hash_2 → winner = target_hash_2.
  - Two NAME blobs for "foo" with T1 == T2, different signatures (so different content_hash): winner = the one with higher content_hash bytes (memcmp DESC).
  - NAME blob for "foo" + NAME blob for "bar" → `resolve_name("foo")` returns foo's target, `resolve_name("bar")` returns bar's target.
  - `resolve_name("no-such-name")` returns nullopt.
  - Non-UTF-8 bytes in name (e.g., `{0xFF, 0x00, 0xFE}`) round-trip correctly through make_name_blob / parse_name_payload.

**What must differ:**
- No `engine.ingest` side-effect for NAME blobs — NAME is just a regular signed blob at the engine level (D-11: delegates can even write NAME). All resolution logic is in the test's `resolve_name` helper + later in `cli/src/commands.cpp:get_by_name`. This test primarily exercises the codec + storage enumeration primitive; the CLI path is covered in cli/tests/ (if present) or deferred to 999.4.

---

### `db/tests/engine/test_bomb_delegate.cpp` (NEW — or fold into test_bomb_validation.cpp)

**Analog:** `db/tests/engine/test_delegate_replay.cpp` (entire file — owner A / owner B / delegate D scaffolding, though phase 123 needs just one owner + one delegate).

**Scaffolding** (test_delegate_replay.cpp:40-59):

```cpp
TEST_CASE("delegate-signed blob for N_A submitted as N_B is rejected (cross-namespace replay)",
          "[phase122][engine][delegate]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner_a = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // Establish namespaces (clear PUBK-first gate + populate owner_pubkeys).
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner_a), make_pubk_blob(owner_a))).accepted);

    // Each owner delegates to D in their own namespace.
    auto del_a = make_signed_delegation(owner_a, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner_a), del_a)).accepted);

    // ... test body ...
}
```

**What to replicate:**
- Owner + delegate setup + PUBK + delegation registration (identical).
- Test: build a BOMB signed by the delegate (use a `make_delegate_bomb_blob` helper following `make_delegate_blob` shape at test_helpers.h:142-163 OR inline the signing).
- Ingest via `engine.ingest(ns_span(owner_a), delegate_bomb)` → `REQUIRE_FALSE(result.accepted)` → `REQUIRE(result.error.value() == IngestError::no_delegation)`.
- Sanity: delegate can write a regular blob (existing test_verify_signer_hint.cpp shape) but not a BOMB.
- (If RESEARCH A4 is accepted:) delegate CAN write a NAME blob — confirm with a second test case: `auto name = make_delegate_name_blob(...); REQUIRE(engine.ingest(..., name).accepted)`.

**What must differ:** only one owner needed (not two like test_delegate_replay.cpp's cross-namespace replay setup).

---

### `db/tests/wire/test_name_bomb_codec.cpp` (NEW) — or extend `db/tests/wire/test_codec.cpp`

**Analog:** `db/tests/wire/test_codec.cpp:22-76` (round-trip + deterministic encode + empty-data tests).

**Round-trip idiom** (test_codec.cpp:22-34):

```cpp
TEST_CASE("Encode/decode round-trip preserves all fields", "[codec]") {
    auto blob = make_test_blob();
    auto encoded = encode_blob(blob);
    REQUIRE(encoded.size() > 0);

    auto decoded = decode_blob(encoded);

    REQUIRE(decoded.signer_hint == blob.signer_hint);
    REQUIRE(decoded.data == blob.data);
    REQUIRE(decoded.ttl == blob.ttl);
    REQUIRE(decoded.timestamp == blob.timestamp);
    REQUIRE(decoded.signature == blob.signature);
}
```

**What to replicate:**
- `TEST_CASE` tags: `[phase123][wire][codec]`.
- Magic-byte sanity: NAME_MAGIC bytes = `{0x4E, 0x41, 0x4D, 0x45}`; BOMB_MAGIC bytes = `{0x42, 0x4F, 0x4D, 0x42}`.
- `is_name` / `is_bomb` truth table: valid payload → true; short payload → false; magic mismatch → false; length inconsistency → false.
- `parse_name_payload` / `make_name_data` round-trip: arbitrary bytes including `\x00` and `\xFF` sequences.
- `validate_bomb_structure` / `extract_bomb_targets` / `make_bomb_data` round-trip with counts 0, 1, 7, 100.
- Pairwise-distinctness of all known magics (CENV, PUBK, TOMB, DLGT, CDAT, CPAR, NAME, BOMB) — RESEARCH Pitfall 5 warning sign.

**What must differ:** wire-layer tests don't use NodeIdentity / sign — they test the byte-layout helpers in isolation. Faster than engine-layer tests.

---

### `db/CMakeLists.txt` (MODIFY) — register new test files

**Analog:** `db/CMakeLists.txt:220-261` (explicit file list in `add_executable(chromatindb_tests ...)`).

```cmake
add_executable(chromatindb_tests
    # ...existing...
    tests/engine/test_engine.cpp
    tests/engine/test_verify_signer_hint.cpp
    tests/engine/test_pubk_first.cpp
    tests/engine/test_delegate_replay.cpp
    # ...
    tests/wire/test_codec.cpp
    # ...
)
```

**What to replicate:** pure append — add the new test paths:

```cmake
    tests/engine/test_bomb_validation.cpp
    tests/engine/test_bomb_side_effects.cpp
    tests/engine/test_bomb_delegate.cpp          # or fold into test_bomb_validation
    tests/engine/test_name_resolution.cpp
    tests/wire/test_name_bomb_codec.cpp          # or extend test_codec.cpp
```

**What must differ:** nothing. No new library targets; all new code lives in `chromatindb_lib` (via codec.cpp / engine.cpp / error_codes.h) or the test binary.

---

## Shared Patterns

### Pattern A: Magic-prefix gate BEFORE `crypto::offload`

**Source:** `db/engine/engine.cpp:181-195` (Phase 122 Step 1.5 PUBK-first gate).

**Applies to:** All new engine.cpp validation inserted at Step 1.7 (BOMB structural checks). Per RESEARCH Pitfall 2, ANY BOMB check that burns ML-DSA-87 verify CPU before rejecting is a DoS vector.

```cpp
if (!storage_.has_owner_pubkey(target_namespace)) {
    if (!wire::is_pubkey_blob(blob.data)) {
        spdlog::warn("Ingest rejected: PUBK-first violation ...");
        co_return IngestResult::rejection(IngestError::pubk_first_violation,
            "first write to namespace must be PUBK");
    }
}
```

**Rule:** Any new structural check on BOMB (ttl=0, count match, delegate-reject) must appear BEFORE `Step 3: co_await crypto::offload(pool_, ... verify ...)` at engine.cpp:331.

---

### Pattern B: Signing canonical form (Phase 122)

**Source:** `db/wire/codec.h:49-59` + `db/tests/test_helpers.h:91-94`.

**Applies to:** Every `make_name_blob` / `make_bomb_blob` test helper, every CLI NAME/BOMB build site (commands.cpp).

```cpp
// For owner writes, target_namespace = SHA3(owner_pk) = signer_hint = id.namespace_id().
auto signing_input = wire::build_signing_input(
    id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
blob.signature = id.sign(signing_input);
```

**Rule:** `signing_input = SHA3(target_namespace || data || ttl_be32 || timestamp_be64)`. For delegate writes, `target_namespace = OWNER's namespace_id` (NOT delegate's) to defeat cross-namespace replay — see test_helpers.h:155-163 make_delegate_blob.

---

### Pattern C: CONC-04 post-back-to-ioc after `crypto::offload`

**Source:** `db/engine/engine.cpp:216, :236, :311, :340` (Phase 121 STORAGE_THREAD_CHECK discipline).

**Applies to:** Only if the planner decides to add a new `crypto::offload` site on the BOMB path. RESEARCH Pattern 4 notes the BOMB side-effect does NOT require offload — `storage_.delete_blob_data` is already on the executor. No new offload needed for Phase 123.

```cpp
auto [signing_input, verify_ok] = co_await crypto::offload(pool_, [...]() { ... });
// CONC-04 (Phase 121): post back to ioc_ before Storage access.
co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
```

**Rule:** If ANY BOMB-path change introduces a `crypto::offload`, it MUST be followed immediately by the `asio::post` pattern. Never touch `storage_.*` after offload without posting back.

---

### Pattern D: Shared helpers live in `db/wire/codec.*` (NOT inline in engine.cpp / message_dispatcher.cpp)

**Source:** `feedback_no_duplicate_code.md` (memory) + `db/wire/codec.h:137-176` (existing is_*/extract_*/make_* all in codec.cpp).

**Applies to:** `is_name`, `is_bomb`, `parse_name_payload`, `make_name_data`, `validate_bomb_structure`, `extract_bomb_targets`, `make_bomb_data`.

**Rule:** All NAME/BOMB codec helpers declared in `db/wire/codec.h` and implemented in `db/wire/codec.cpp`. Engine and dispatcher call into them; never copy the logic. Mirror the same helpers in `cli/src/wire.{h,cpp}` (the two wire modules are separately-compiled but logically paired — same rule within CLI).

---

### Pattern E: Big-endian everywhere

**Source:** Project memory `YAGNI / keep it clean`; `cli/src/wire.h:18-28` `store_u{16,32,64}_be` helpers; `db/util/endian.h` `read_u{16,32,64}_be`.

**Applies to:** NAME `name_len:2` and BOMB `count:4` integer fields.

**Rule:** Use existing `chromatindb::util::read_u32_be(ptr)` / `store_u32_be(out, val)` — never hand-roll byte shifts. The CLI's `load_u32_be` at wire.h:47-52 is a separate module's mirror of the same idiom.

---

### Pattern F: `[phase123]` Catch2 tag convention

**Source:** Phase 122 convention (`[phase122][pubk_first][engine]`, `[phase122][engine][delegate]`, `[phase122][engine][verify]`) at test_pubk_first.cpp:41 etc.

**Applies to:** Every new test case.

**Rule:** Tag every TEST_CASE with at least `[phase123]` + component tag (`[wire]`, `[engine]`, `[bomb]`, `[name]`). The orchestrator runs `./build/db/chromatindb_tests "[phase123]"` as the phase-subset fast lane (RESEARCH §Validation Architecture).

---

## No Analog Found

*(none — every Phase 123 file has at least a role-match analog in the current tree)*

Phase 123 is a composition phase, not an invention phase. Every new capability is structural-identity with an existing Phase 117/122 pattern:
- NAME/BOMB magics mirror TOMB/DLGT/PUBK
- Step 1.7 BOMB validation mirrors Step 1.5 PUBK-first
- Step 3.5 BOMB side-effect mirrors existing tombstone side-effect
- Step 2 BOMB delegate-reject extends existing tombstone / delegation delegate-reject list
- CLI multi-target rm mirrors CLI multi-hash get
- get_by_name resolution mirrors the same ListRequest+ReadRequest fan-out pattern
- test_bomb_validation mirrors test_pubk_first
- test_bomb_delegate mirrors test_delegate_replay

---

## Metadata

**Analog search scope:**
- `db/wire/codec.{h,cpp}` — magic constants, is_*/extract_*/make_* helpers
- `db/engine/engine.cpp` — Step 0/0e/1/1.5/1.7/2/2.5/3/3.5/4/4.5 ingest pipeline
- `db/peer/error_codes.h` — wire error codes
- `db/peer/message_dispatcher.cpp` — ListRequest + type_filter (Phase 117 TYPE-03)
- `db/storage/storage.{h,cpp}` — seq_map blob_type indexing
- `cli/src/wire.{h,cpp}` — CLI codec mirror
- `cli/src/commands.{h,cpp}` — put/get/rm command impls
- `cli/src/main.cpp` — argv parsing
- `db/tests/test_helpers.h` — make_*_blob fixtures
- `db/tests/engine/` — test_pubk_first.cpp, test_delegate_replay.cpp, test_verify_signer_hint.cpp
- `db/tests/wire/` — test_codec.cpp
- `db/CMakeLists.txt` — test-target registration

**Files scanned:** ~15 source files read in full or relevant excerpts; 5 directories enumerated.

**Pattern extraction date:** 2026-04-20
