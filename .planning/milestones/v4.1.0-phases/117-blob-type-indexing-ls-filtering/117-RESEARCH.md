# Phase 117: Blob Type Indexing + ls Filtering - Research

**Researched:** 2026-04-16
**Domain:** libmdbx storage indexing, wire protocol extension, CLI output filtering
**Confidence:** HIGH

## Summary

This phase adds a type index to the node's storage layer (extracting the first 4 bytes of blob data on ingest), extends the ListRequest/ListResponse wire format to include type information and control flags, and updates `cdb ls` to filter infrastructure blobs by default.

The implementation touches three layers: storage (new sub-database + seq_map value extension), wire protocol (ListRequest flags byte, ListResponse entry size change 40->44), and CLI (output formatting, hide list, new flags). All changes are well-constrained by CONTEXT.md decisions and follow established patterns already present in the codebase (tombstone/delegation magic checks, length-based optional fields, big-endian encoding).

**Primary recommendation:** Extend the seq_map value from 32 bytes (hash only) to 36 bytes (hash:32 + type:4) rather than creating a separate type_map sub-database. This avoids an additional sub-database, keeps type retrieval on the hot path (get_blob_refs_since already reads seq_map), and eliminates cross-index consistency concerns. The schema migration is straightforward: blobs with 32-byte seq_map values get type 0x00000000 (treated as unknown/DATA) until backfilled.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- D-01: Node indexes first 4 bytes of blob data as `blob_type` on ingest. Stored alongside blob_hash in a type index. No size field.
- D-03: Extensible -- any 4-byte prefix works without node code changes (TYPE-04). The node treats all types equally.
- D-04: Node keeps existing server-side filtering for protocol internals: tombstones (0xDEADBEEF), delegations (0xDE1E6A7E), and expired blobs.
- D-05: Node does NOT filter application-level types (PUBK, CENV, CDAT, user-defined). That's the client's decision.
- D-06: The type index eliminates the need to load full blob data for tombstone/delegation checks in ListResponse.
- D-07: ListRequest extended with optional 1-byte flags field at offset 44. Length-based detection: 44 bytes = no flags, 45+ bytes = flags present.
- D-08: Flags bit 0: `include_all` -- skip tombstone/delegation/expired filtering, return everything.
- D-10: ListResponse entry size changes from 40 to 44 bytes: `[hash:32][seq:8BE][type:4]`.
- D-11: No size field in ListResponse.
- D-12: `cdb ls` shows hash + type label per line. Infrastructure blobs silently hidden (no summary footer).
- D-13: CLI-side hide list: PUBK (0x50554B42), CDAT (future, Phase 119), DLGT (0xDE1E6A7E defense-in-depth).
- D-14: CENV blobs (0x43454E56) are user data -- always shown in default mode.
- D-15: Unrecognized type prefixes labeled as `DATA` in output.
- D-16: `cdb ls --raw` shows all blobs (sends `include_all` flag to node) with same hash + type label format.
- D-17: No change in label format between default and --raw. `--raw` only changes which blobs are shown.
- D-18: Human-readable 4-char labels: `CENV`, `PUBK`, `DATA`, `TOMB`, `DLGT`, `CDAT`.
- D-19: No raw hex display mode. Labels are always human-readable.
- D-20: `cdb ls --type CENV` filters output to only blobs matching that type label. Uses ListRequest type filter server-side.
- D-21: `--type` bypasses the CLI hide list.
- D-22: Hardcoded in CLI. No config file for hidden types (YAGNI).
- D-23: Silent filtering -- no "N blobs hidden" footer.

### Claude's Discretion
- Short blob type handling (zero-padding approach for <4 byte blobs)
- TYPE-03 server-side type filter wire format details (flags bit + field, or client-side only)
- Hide list implementation (compile-time array of magic prefixes)

### Deferred Ideas (OUT OF SCOPE)
- Blob size in ls output -- deferred to avoid node architecture drift.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| TYPE-01 | Node indexes first 4 bytes of blob data as `blob_type` on ingest | Extend seq_map value to 36 bytes (hash:32 + type:4), extract type in store_blob and store_blobs_atomic |
| TYPE-02 | ListResponse includes 4-byte type per entry | Entry size 40->44 bytes, type appended from seq_map, no blob data load needed |
| TYPE-03 | ListRequest supports optional type filter | Flags byte at offset 44 (bit 1) + 4-byte type_filter at offset 45, server-side filtering in message_dispatcher |
| TYPE-04 | Extensible -- any 4-byte prefix works without node changes | Node stores raw 4 bytes, no type enum, no validation -- just memcpy first 4 bytes of blob.data |
| ERGO-02 | `cdb ls` hides infrastructure blobs by default | CLI-side hide list: PUBK, CDAT, DLGT magic prefixes compared against ListResponse type field |
| ERGO-03 | `cdb ls --raw` shows all blobs including infrastructure types | Sends include_all flag (bit 0) in ListRequest, bypasses CLI hide list |
</phase_requirements>

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Type extraction on ingest | Database / Storage | -- | Storage layer owns blob indexing; type is extracted from blob.data at write time |
| Type index storage | Database / Storage | -- | libmdbx seq_map value extension; single-write-transaction consistency |
| ListRequest flags parsing | API / Backend | -- | message_dispatcher.cpp owns request handling |
| ListResponse type inclusion | API / Backend | Database / Storage | Dispatcher reads from storage (seq_map), formats wire response |
| Server-side type filtering | API / Backend | -- | Dispatcher applies type filter before building response |
| CLI hide list filtering | Frontend / Client | -- | Client decides which application-level types to show |
| CLI output formatting | Frontend / Client | -- | Type label mapping is purely a display concern |

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libmdbx | existing (FetchContent) | Type storage in seq_map sub-database | Already used for all 8 sub-databases [VERIFIED: storage.cpp] |
| Catch2 | v3.7.1 | Unit tests | Already used for 699 tests [VERIFIED: db/CMakeLists.txt] |

### Supporting
No new libraries needed. All work uses existing dependencies. [VERIFIED: codebase inspection]

## Architecture Patterns

### System Architecture Diagram

```
Blob Ingest Path (store_blob / store_blobs_atomic):
  blob.data ──> extract first 4 bytes as type_prefix
                    │
                    ▼
  seq_map upsert: key=[ns:32][seq_be:8] value=[hash:32][type:4]
                    │
                    ▼
  (existing: blobs_map, expiry_map, delegation_map, tombstone_map, quota_map)

ListRequest Handler (message_dispatcher.cpp):
  Client ──> [ns:32][since_seq:8BE][limit:4BE][flags:1?][type_filter:4?]
               │
               ▼
         parse flags (length-based: 44=no flags, 45+=flags present)
               │
               ▼
         get_blob_refs_since() ──> returns {hash, seq_num, type} per entry
               │
               ▼
         filter: if !include_all → skip tombstones/delegations/expired (using type prefix)
         filter: if type_filter set → skip entries where type != filter
               │
               ▼
         build ListResponse: [count:4BE][entries: N * (hash:32 + seq:8BE + type:4)][has_more:1]
               │
               ▼
  Client <── response

CLI ls Output:
  ListResponse entries ──> for each entry:
      │
      ├── map type bytes to label (CENV/PUBK/TOMB/DLGT/CDAT/DATA)
      ├── if default mode: skip if type in hide_list
      └── print: "<hash_hex>  <LABEL>"
```

### Recommended Changes by File

```
db/storage/storage.h
├── BlobRef struct: add std::array<uint8_t, 4> blob_type{} field

db/storage/storage.cpp
├── store_blob(): extract type from blob.data, store in seq_map value (36 bytes)
├── store_blobs_atomic(): same type extraction
├── get_blob_refs_since(): read 36-byte values, populate BlobRef.blob_type
├── get_hashes_by_namespace(): handle 36-byte values (ignore type, backward compat with 32-byte)
├── Schema version: 1 → 2, migration logic for existing 32-byte entries
├── max_maps: stays at 9 (no new sub-database)
├── integrity_scan(): no changes needed (seq_map stats still work)

db/peer/message_dispatcher.cpp
├── ListRequest handler: parse optional flags byte at offset 44
├── ListRequest handler: parse optional type_filter at offset 45-48
├── ListRequest handler: use BlobRef.blob_type instead of loading full blob for tombstone/delegation check
├── ListRequest handler: build 44-byte entries in response (append type)
├── TimeRangeRequest handler: same tombstone/delegation optimization (use type from BlobRef)

db/wire/codec.h
├── Add extract_blob_type() helper: returns first 4 bytes of data, zero-padded if <4

db/PROTOCOL.md
├── Update ListRequest payload: document optional flags byte and type_filter
├── Update ListResponse entry: document 44-byte entries with type field

cli/src/commands.h
├── ls() signature: add bool raw, std::string type_filter params

cli/src/commands.cpp
├── ls(): build 45-byte ListRequest (with flags) or 49-byte (with type filter)
├── ls(): parse 44-byte entries in ListResponse
├── ls(): type label mapping function
├── ls(): hide list filtering

cli/src/main.cpp
├── ls command: parse --raw and --type flags

cli/src/wire.h
├── Add CENV_MAGIC, TOMBSTONE_MAGIC, DELEGATION_MAGIC, CDAT_MAGIC constexprs
├── (PUBKEY_MAGIC already exists)
```

### Pattern 1: Type Extraction at Ingest
**What:** Extract first 4 bytes of blob.data as type prefix during store_blob
**When to use:** Every blob ingest (both single and atomic batch)
**Example:**
```cpp
// Source: [VERIFIED: codec.h pattern for is_tombstone/is_delegation]
// In storage.cpp store_blob(), after dedup check, before seq_map upsert:

// Extract 4-byte type prefix from blob data
std::array<uint8_t, 4> blob_type{};
if (blob.data.size() >= 4) {
    std::memcpy(blob_type.data(), blob.data.data(), 4);
}
// else: blob_type stays {0,0,0,0} -- zero-padded for short blobs

// Store in seq_map: value is now [hash:32][type:4] = 36 bytes
std::array<uint8_t, 36> seq_value;
std::memcpy(seq_value.data(), precomputed_hash.data(), 32);
std::memcpy(seq_value.data() + 32, blob_type.data(), 4);
txn.upsert(impl_->seq_map, to_slice(seq_key),
            mdbx::slice(seq_value.data(), seq_value.size()));
```

### Pattern 2: Length-Based Optional Fields in ListRequest
**What:** Detect optional flags and type_filter by payload length
**When to use:** ListRequest handler in message_dispatcher.cpp
**Example:**
```cpp
// Source: [VERIFIED: existing length-based check at line 441 of message_dispatcher.cpp]
// Payload layout:
// 44 bytes: [namespace:32][since_seq:8BE][limit:4BE] -- backward compatible
// 45 bytes: [namespace:32][since_seq:8BE][limit:4BE][flags:1]
// 49 bytes: [namespace:32][since_seq:8BE][limit:4BE][flags:1][type_filter:4]

uint8_t flags = 0;
std::array<uint8_t, 4> type_filter{};
bool has_type_filter = false;

if (payload.size() >= 45) {
    flags = payload[44];
}
if (payload.size() >= 49) {
    std::memcpy(type_filter.data(), payload.data() + 45, 4);
    has_type_filter = (flags & 0x02) != 0;  // bit 1 = type filter present
}

bool include_all = (flags & 0x01) != 0;  // bit 0 = include all
```

### Pattern 3: Type Label Mapping in CLI
**What:** Map 4-byte type prefix to human-readable 4-char label
**When to use:** CLI ls output formatting
**Example:**
```cpp
// Source: [VERIFIED: existing PUBKEY_MAGIC pattern in cli/src/wire.h]
inline constexpr std::array<uint8_t, 4> CENV_MAGIC    = {0x43, 0x45, 0x4E, 0x56};
inline constexpr std::array<uint8_t, 4> PUBKEY_MAGIC_4 = {0x50, 0x55, 0x42, 0x4B}; // already exists as PUBKEY_MAGIC
inline constexpr std::array<uint8_t, 4> TOMBSTONE_MAGIC = {0xDE, 0xAD, 0xBE, 0xEF};
inline constexpr std::array<uint8_t, 4> DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E};
inline constexpr std::array<uint8_t, 4> CDAT_MAGIC    = {0x43, 0x44, 0x41, 0x54};

const char* type_label(std::span<const uint8_t, 4> type) {
    if (std::memcmp(type.data(), CENV_MAGIC.data(), 4) == 0) return "CENV";
    if (std::memcmp(type.data(), PUBKEY_MAGIC_4.data(), 4) == 0) return "PUBK";
    if (std::memcmp(type.data(), TOMBSTONE_MAGIC.data(), 4) == 0) return "TOMB";
    if (std::memcmp(type.data(), DELEGATION_MAGIC.data(), 4) == 0) return "DLGT";
    if (std::memcmp(type.data(), CDAT_MAGIC.data(), 4) == 0) return "CDAT";
    return "DATA";
}
```

### Pattern 4: CLI Hide List
**What:** Compile-time array of type prefixes to hide in default `cdb ls`
**When to use:** CLI ls filtering before output
**Example:**
```cpp
// Source: [VERIFIED: D-13 decisions, PUBKEY_MAGIC in cli/src/wire.h]
static constexpr std::array<std::array<uint8_t, 4>, 3> HIDDEN_TYPES = {{
    {0x50, 0x55, 0x42, 0x4B},  // PUBK
    {0x43, 0x44, 0x41, 0x54},  // CDAT
    {0xDE, 0x1E, 0x6A, 0x7E},  // DLGT (defense-in-depth)
}};

bool is_hidden_type(std::span<const uint8_t, 4> type) {
    for (const auto& hidden : HIDDEN_TYPES) {
        if (std::memcmp(type.data(), hidden.data(), 4) == 0) return true;
    }
    return false;
}
```

### Anti-Patterns to Avoid
- **Separate type_map sub-database:** Adds cross-index consistency burden, requires max_maps bump, and means get_blob_refs_since needs a second lookup. Extending seq_map value is simpler and faster. [VERIFIED: seq_map already on the hot path in get_blob_refs_since]
- **Enum-based type system:** Violates TYPE-04 extensibility. The node must treat any 4-byte prefix as valid without code changes.
- **Loading full blob data for type checks in ListResponse:** D-06 explicitly says the type index eliminates this need. The current code at line 464 does `storage_.get_blob(ns, ref.blob_hash)` which is expensive.
- **Breaking existing ListRequest clients:** Length-based detection (D-07) ensures 44-byte requests from old clients still work.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Schema migration | Custom migration framework | SCHEMA_VERSION check + inline migration code | Already have version stamp in meta_map (line 121 of storage.cpp) with "Future: run migrations here" comment [VERIFIED: storage.cpp line 214] |
| Big-endian encoding | Bit-shift helpers | Existing `store_u32_be` / `read_u32_be` / `store_u64_be` / `read_u64_be` | Both node (`db/util/endian.h`) and CLI (`cli/src/wire.h`) have these [VERIFIED: codebase] |
| Type magic checks | Per-type boolean functions | Generic 4-byte memcmp | Node already has `is_tombstone()`, `is_delegation()`, `is_pubkey_blob()` as patterns to follow [VERIFIED: codec.h] |

**Key insight:** The existing codebase has all the primitives needed. This phase is pure composition -- wiring existing patterns (big-endian helpers, magic prefix checks, length-based optional fields) into a new feature.

## Common Pitfalls

### Pitfall 1: seq_map Value Size Backward Compatibility
**What goes wrong:** Existing blobs have 32-byte seq_map values. Code that reads seq_map values and expects exactly 36 bytes will crash on existing data.
**Why it happens:** Schema migration runs at open_env time but existing entries are not automatically re-keyed.
**How to avoid:** In `get_blob_refs_since`, handle both 32-byte (old) and 36-byte (new) values. For 32-byte values, use zero type `{0,0,0,0}`. Run a one-time backfill migration at startup (iterate seq_map, load blob from blobs_map, extract type, update value to 36 bytes).
**Warning signs:** Tests with pre-existing databases failing with "unexpected value length" errors.

### Pitfall 2: ListResponse Entry Size Change Breaks Existing Clients
**What goes wrong:** Any client parsing ListResponse with hardcoded `count * 40` will misparse 44-byte entries.
**Why it happens:** ListResponse entry size is implicit in the count field, not self-describing.
**How to avoid:** This is a breaking wire format change. Since the project has no backward compatibility requirement (per REQUIREMENTS.md "Out of Scope"), both node and CLI ship together. The CLI must be updated in the same phase. Verify no other code parses ListResponse (check: `list_hashes` in commands.cpp also parses ListResponse).
**Warning signs:** `cdb ls` showing garbled hashes or wrong entry counts.

### Pitfall 3: get_hashes_by_namespace Reads seq_map Values
**What goes wrong:** `get_hashes_by_namespace()` reads 32-byte hash values from seq_map. After the extension to 36 bytes, it will read into a 32-byte array from a 36-byte value.
**Why it happens:** The function assumes `val_data.length() == 32`.
**How to avoid:** Update `get_hashes_by_namespace` to accept 36-byte values (read first 32 bytes, ignore last 4). Keep the `if (val_data.length() != 32)` check as `if (val_data.length() < 32)`.
**Warning signs:** Sync failures or empty hash lists after upgrade.

### Pitfall 4: store_blobs_atomic Must Also Extract Type
**What goes wrong:** If only `store_blob` extracts type but `store_blobs_atomic` does not, blobs stored via atomic batch (future chunked uploads, Phase 119) will have zero type.
**Why it happens:** Two separate code paths for blob storage.
**How to avoid:** Apply type extraction in both `store_blob` (line 478) and `store_blobs_atomic` (line 689). Both write to seq_map with the same pattern.
**Warning signs:** Atomic-stored blobs showing as `DATA` instead of their actual type.

### Pitfall 5: Tombstone/Delegation Check Using Type vs Full Data
**What goes wrong:** D-06 says type index replaces full blob load for tombstone/delegation checks. But tombstone check currently validates both magic AND data size (`data.size() != TOMBSTONE_DATA_SIZE`). A 4-byte type prefix check alone could false-positive on user data that happens to start with 0xDEADBEEF.
**Why it happens:** The is_tombstone() function checks both prefix AND exact size. Type index only stores 4 bytes.
**How to avoid:** The tombstone magic (0xDEADBEEF) with exact 36-byte size is highly unlikely to collide with user data. For the ListResponse filter, a 4-byte prefix check is sufficient because: (a) the node already validates tombstone structure at ingest time, and (b) false positives would only cause a user blob to be hidden in default mode (visible with --raw). Accept this trade-off per D-06.
**Warning signs:** User blobs with data starting with 0xDEADBEEF being hidden.

### Pitfall 6: Overflow in ListResponse Size Calculation
**What goes wrong:** The existing code uses `checked_mul(count, 40)`. Changing to `checked_mul(count, 44)` is needed.
**Why it happens:** Entry size constant is embedded in arithmetic, not a named constant.
**How to avoid:** Define `constexpr size_t LIST_ENTRY_SIZE = 44;` and use it consistently in both size calculation and serialization loop.
**Warning signs:** ListResponse overflow errors for large result sets.

### Pitfall 7: list_hashes in commands.cpp Also Parses ListResponse
**What goes wrong:** The `list_hashes()` function (commands.cpp line 792+) also sends ListRequest and parses ListResponse with 40-byte entries. If not updated, it will break.
**Why it happens:** Two separate ListResponse parsing sites in the CLI.
**How to avoid:** Update both `ls()` and `list_hashes()` to parse 44-byte entries. Consider extracting a shared ListResponse parser.
**Warning signs:** `cdb rm` or other commands that use `list_hashes()` failing after this phase.

## Code Examples

### Seq Map Value Migration (Schema v1 -> v2)

```cpp
// Source: [VERIFIED: storage.cpp existing schema version pattern at line 200-224]
// In open_env(), after schema version check, before txn.commit():

if (on_disk < 2) {
    // Migrate seq_map values from 32 bytes (hash only) to 36 bytes (hash + type)
    spdlog::info("Migrating seq_map to schema v2 (adding blob_type)...");
    auto cursor = txn.open_cursor(seq_map);
    auto seek = cursor.to_first(false);
    uint64_t migrated = 0;
    while (seek.done) {
        auto val = cursor.current(false).value;
        if (val.length() == 32) {
            // Old format: hash only. Read blob to extract type.
            auto key = cursor.current(false).key;
            if (key.length() == 40) {
                const uint8_t* ns_ptr = static_cast<const uint8_t*>(key.data());
                const uint8_t* hash_ptr = static_cast<const uint8_t*>(val.data());

                // Look up blob to get type prefix
                auto blob_key = make_blob_key(ns_ptr, hash_ptr);
                auto blob_data = txn.get(blobs_map, to_slice(blob_key), not_found_sentinel);

                std::array<uint8_t, 36> new_val{};
                std::memcpy(new_val.data(), val.data(), 32);  // hash

                if (blob_data.data() != nullptr) {
                    // Decrypt and decode to get blob.data for type extraction
                    // ... (decrypt_value + decode_blob + extract first 4 bytes)
                }
                // else: zero type (deleted blob sentinel or missing)

                cursor.upsert(cursor.current(false).key,
                             mdbx::slice(new_val.data(), new_val.size()));
                ++migrated;
            }
        }
        seek = cursor.to_next(false);
    }
    spdlog::info("Migrated {} seq_map entries to v2", migrated);

    // Update schema version to 2
    std::array<uint8_t, 4> ver_buf;
    chromatindb::util::store_u32_be(ver_buf.data(), 2);
    txn.upsert(meta_map, key_slice,
               mdbx::slice(ver_buf.data(), ver_buf.size()));
}
```

### ListResponse Building with Type Field

```cpp
// Source: [VERIFIED: message_dispatcher.cpp existing ListResponse builder at line 474-489]
constexpr size_t LIST_ENTRY_SIZE = 44;  // hash:32 + seq:8 + type:4

uint32_t count = static_cast<uint32_t>(filtered_refs.size());
auto body_size = chromatindb::util::checked_mul(static_cast<size_t>(count), LIST_ENTRY_SIZE);
if (!body_size) { /* overflow error */ }
auto resp_size = chromatindb::util::checked_add(*body_size, size_t{5});
if (!resp_size) { /* overflow error */ }

std::vector<uint8_t> response(*resp_size);
chromatindb::util::store_u32_be(response.data(), count);
for (uint32_t i = 0; i < count; ++i) {
    size_t off = 4 + i * LIST_ENTRY_SIZE;
    std::memcpy(response.data() + off, filtered_refs[i].blob_hash.data(), 32);
    chromatindb::util::store_u64_be(response.data() + off + 32, filtered_refs[i].seq_num);
    std::memcpy(response.data() + off + 40, filtered_refs[i].blob_type.data(), 4);
}
response[*resp_size - 1] = has_more ? 1 : 0;
```

### CLI ListResponse Parsing with Type

```cpp
// Source: [VERIFIED: commands.cpp existing ListResponse parsing at line 755-777]
constexpr size_t ENTRY_SIZE = 44;  // hash:32 + seq:8 + type:4

uint32_t count = load_u32_be(p.data());
size_t entries_size = static_cast<size_t>(count) * ENTRY_SIZE;
if (p.size() < 4 + entries_size + 1) {
    std::fprintf(stderr, "Error: ListResponse truncated\n");
    return 1;
}

const uint8_t* entries = p.data() + 4;
for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = entries + static_cast<size_t>(i) * ENTRY_SIZE;
    auto hash_span = std::span<const uint8_t>(entry, 32);
    auto type_span = std::span<const uint8_t, 4>(entry + 40, 4);

    // Skip hidden types in default mode
    if (!raw && is_hidden_type(type_span)) continue;

    // Apply --type filter
    if (!type_filter.empty()) {
        if (std::strcmp(type_label(type_span), type_filter.c_str()) != 0) continue;
    }

    std::printf("%s  %s\n", to_hex(hash_span).c_str(), type_label(type_span));

    since_seq = load_u64_be(entry + 32);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Load full blob to check tombstone/delegation in ListResponse | Check 4-byte type prefix from seq_map | Phase 117 | Eliminates N get_blob() calls per ListRequest -- O(1) type check instead of O(N) blob loads |
| ListResponse entries: 40 bytes (hash + seq) | 44 bytes (hash + seq + type) | Phase 117 | CLI can display and filter by type without additional ReadRequest calls |
| `cdb ls` shows all blobs | Default hides infrastructure blobs | Phase 117 | Cleaner output for end users |

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | Zero-padding (<4 byte blobs) with `{0,0,0,0}` is acceptable | Architecture Patterns | Very low -- blobs <4 bytes are edge cases (empty data fields); zero type maps to "DATA" label which is correct |
| A2 | CDAT magic is `{0x43, 0x44, 0x41, 0x54}` ("CDAT" ASCII) | Pattern 3 / Hide List | Medium -- Phase 119 has not defined CDAT magic yet. If different, CLI hide list needs update. But D-13 says "CDAT (future, Phase 119)" so the exact value may change. |
| A3 | Backfill migration at startup is acceptable for existing deployments | Pitfall 1 | Low -- single local network deployment, one node. Migration runs once. |
| A4 | ListResponse entry size change does not require a protocol version bump | Pitfall 2 | None -- backward compatibility explicitly out of scope per REQUIREMENTS.md |

## Open Questions

1. **Backfill migration performance for large databases**
   - What we know: Migration iterates all seq_map entries and loads blobs to extract types. For each entry, requires decrypt + decode + type extraction.
   - What's unclear: How many blobs exist on the live node at 192.168.1.73? If thousands, migration could take seconds. If millions, could take minutes.
   - Recommendation: Log progress during migration. This is a one-time cost and acceptable given single-node deployment.

2. **CDAT magic prefix exact value**
   - What we know: D-13 includes CDAT in the hide list. Phase 119 (Chunked Large Files) will define the exact CDAT prefix.
   - What's unclear: Whether CDAT will use ASCII "CDAT" = `{0x43, 0x44, 0x41, 0x54}` or a different magic.
   - Recommendation: Use `{0x43, 0x44, 0x41, 0x54}` (ASCII "CDAT") and document it. Phase 119 can adjust if needed. [ASSUMED]

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| cmake | Build system | Yes | 4.3.1 | -- |
| libmdbx | Storage | Yes | FetchContent | -- |
| Catch2 | Tests | Yes | v3.7.1 (FetchContent) | -- |

No missing dependencies.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (lines 220+), cli/tests/CMakeLists.txt |
| Quick run command | `cd build && ./db/chromatindb_tests "[storage]" -c "BlobRef type"` |
| Full suite command | `cd build && ./db/chromatindb_tests && ./cli/tests/cli_tests` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| TYPE-01 | Type extracted on ingest, stored in seq_map | unit | `./db/chromatindb_tests "[storage][type_index]"` | No -- Wave 0 |
| TYPE-02 | ListResponse includes type per entry | unit | `./db/chromatindb_tests "[protocol][list_type]"` | No -- Wave 0 |
| TYPE-03 | ListRequest type filter | unit | `./db/chromatindb_tests "[protocol][list_filter]"` | No -- Wave 0 |
| TYPE-04 | Any 4-byte prefix works without code changes | unit | `./db/chromatindb_tests "[storage][type_extensible]"` | No -- Wave 0 |
| ERGO-02 | ls hides infrastructure blobs | integration/E2E | Manual against live node | -- |
| ERGO-03 | ls --raw shows all blobs | integration/E2E | Manual against live node | -- |

### Sampling Rate
- **Per task commit:** `cd build && cmake --build . -j$(nproc) && ./db/chromatindb_tests "[storage]" && ./cli/tests/cli_tests`
- **Per wave merge:** Full test suite
- **Phase gate:** All 699+ tests green, new type_index tests pass

### Wave 0 Gaps
- [ ] `db/tests/storage/test_storage.cpp` -- new test cases for TYPE-01, TYPE-04 (type extraction, BlobRef.blob_type population, migration)
- [ ] `db/tests/wire/test_codec.cpp` -- new test for extract_blob_type helper
- [ ] CLI type label/hide list tests would go in `cli/tests/` but are lower priority (pure output formatting)

## Security Domain

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | No | -- (no auth changes) |
| V3 Session Management | No | -- (no session changes) |
| V4 Access Control | No | -- (namespace ACL unchanged) |
| V5 Input Validation | Yes | Length-based payload validation, flags bounds check |
| V6 Cryptography | No | -- (encryption at rest unchanged, type stored in seq_map alongside encrypted blob) |

### Known Threat Patterns

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Malformed ListRequest flags byte | Tampering | Length-based detection; unknown flag bits ignored (forward compatible) |
| Oversized type_filter payload | Tampering | Fixed 4-byte read; excess bytes ignored |
| Integer overflow in ListResponse size calculation | Tampering | Existing checked_mul/checked_add pattern (already in codebase) |

## Sources

### Primary (HIGH confidence)
- `db/storage/storage.cpp` -- Storage::Impl structure, store_blob implementation, get_blob_refs_since, schema versioning pattern
- `db/storage/storage.h` -- BlobRef struct definition, Storage API
- `db/peer/message_dispatcher.cpp` -- ListRequest handler (lines 437-500), TimeRangeRequest handler (lines 1196-1291)
- `db/wire/codec.h` -- TOMBSTONE_MAGIC, DELEGATION_MAGIC, PUBKEY_MAGIC definitions
- `db/wire/codec.cpp` -- is_tombstone(), is_delegation() implementations
- `cli/src/commands.cpp` -- ls() implementation (lines 710-786), list_hashes() (line 792+)
- `cli/src/commands.h` -- ls() function signature
- `cli/src/main.cpp` -- ls command parsing (lines 531-557)
- `cli/src/wire.h` -- PUBKEY_MAGIC, big-endian helpers, MsgType enum
- `cli/src/envelope.cpp` -- CENV magic (0x43454E56) at line 30
- `db/PROTOCOL.md` -- ListRequest/ListResponse wire format (lines 754-776)
- `db/schemas/transport.fbs` -- TransportMsgType enum

### Secondary (MEDIUM confidence)
- `.planning/phases/117-blob-type-indexing-ls-filtering/117-CONTEXT.md` -- All D-01 through D-23 decisions

### Tertiary (LOW confidence)
- None

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, all existing deps [VERIFIED: codebase inspection]
- Architecture: HIGH -- all code paths read, data structures understood, patterns established [VERIFIED: storage.cpp, message_dispatcher.cpp, commands.cpp]
- Pitfalls: HIGH -- identified 7 concrete pitfalls from code analysis, each with specific line references [VERIFIED: codebase inspection]

**Research date:** 2026-04-16
**Valid until:** 2026-05-16 (stable codebase, frozen MVP node)
