---
phase: 117-blob-type-indexing-ls-filtering
verified: 2026-04-16T10:30:00Z
status: human_needed
score: 13/13
overrides_applied: 0
human_verification:
  - test: "Rebuild CLI binary and run cdb ls --help to confirm --raw and --type flags appear in output"
    expected: "Output includes '--raw' and '--type <TYPE>' in usage"
    why_human: "Compiled binary at cli/build/chromatindb-cli was built 2026-04-15, before Plan 02 commits on 2026-04-16. Source code is correct but binary is stale."
  - test: "Run cdb ls against live node at 192.168.1.73 — verify infrastructure blobs are hidden"
    expected: "Default output shows only CENV/DATA blobs; PUBK/CDAT/DLGT are absent"
    why_human: "End-to-end behavior against real node with populated type index cannot be verified statically"
  - test: "Run cdb ls --raw against live node — verify all blobs including PUBK/DLGT appear with type labels"
    expected: "Output shows all blob types including infrastructure types with 4-character labels"
    why_human: "Wire-level behavior (include_all flag sent, 44-byte entries decoded) requires live node"
  - test: "Run cdb ls --type PUBK against live node — verify PUBK blobs are shown despite being in hide list"
    expected: "Only PUBK blobs listed; hide list is bypassed when --type is given"
    why_human: "Server-side type filter behavior requires live node"
---

# Phase 117: Blob Type Indexing + ls Filtering Verification Report

**Phase Goal:** Users can filter blob listings by type, and `cdb ls` presents a clean view by hiding infrastructure blobs (CDAT chunks, PUBK, delegations)
**Verified:** 2026-04-16T10:30:00Z
**Status:** human_needed
**Re-verification:** No — initial verification

## Goal Achievement

All source code implementation is complete and verified. The compiled CLI binary is stale (built 2026-04-15 before Plan 02 commits). Automated source checks pass 13/13. Human verification required to confirm rebuilt binary behavior and end-to-end behavior against live node.

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Node extracts first 4 bytes of blob data as blob_type on every ingest (store_blob and store_blobs_atomic) | VERIFIED | `db/storage/storage.cpp` lines 525, 744: `extract_blob_type(std::span<const uint8_t>(blob.data))` called in both paths |
| 2 | Node stores blob_type in seq_map alongside blob_hash (36-byte values) | VERIFIED | `storage.cpp` line 527-530: `seq_value` is 36 bytes, `std::memcpy(seq_value.data() + 32, blob_type.data(), 4)` |
| 3 | Existing 32-byte seq_map entries are migrated to 36 bytes at startup with correct type extraction | VERIFIED | `storage.cpp` line 216-252: schema v2 migration loop; `SCHEMA_VERSION = 2` at line 121 |
| 4 | ListResponse entries are 44 bytes each: hash:32 + seq:8BE + type:4 | VERIFIED | `db/peer/message_dispatcher.cpp` line 506: `constexpr size_t LIST_ENTRY_SIZE = 44;`, line 518: type written at offset 40 |
| 5 | ListRequest accepts optional flags byte at offset 44 with include_all (bit 0) and type_filter (bit 1 + 4 bytes at offset 45) | VERIFIED | `message_dispatcher.cpp` lines 451-462: `include_all = (flags & 0x01) != 0`, `has_type_filter` set when `(flags & 0x02) != 0` |
| 6 | When include_all is NOT set, tombstone and delegation blobs are filtered using type prefix instead of loading full blob data | VERIFIED | `message_dispatcher.cpp` lines 480-492: `memcmp(ref.blob_type.data(), TOMBSTONE_MAGIC.data(), 4)` and `DELEGATION_MAGIC` — no `get_blob()` call for filtering |
| 7 | Any 4-byte prefix works as a type without node code changes | VERIFIED | `extract_blob_type` stores first 4 bytes verbatim; `type_label` returns "DATA" for unknown prefixes; test "Arbitrary 4-byte prefix stored and retrieved" at test_storage.cpp line 2460 |
| 8 | cdb ls shows hash + type label per line, hiding infrastructure blobs (PUBK, CDAT, DLGT) by default | VERIFIED | `cli/src/commands.cpp` lines 814-822: condition `!raw && type_filter.empty() && is_hidden_type(type_ptr)` silently skips hidden types; `printf("%s  %s\n", ..., type_label(type_ptr))` |
| 9 | cdb ls --raw shows all blobs including infrastructure types with type labels | VERIFIED | `commands.cpp` line 726: `flags |= 0x01` for raw/include_all; hide condition requires `!raw` — when raw=true all blobs shown with labels |
| 10 | cdb ls --type CENV filters to only CENV blobs | VERIFIED | `commands.cpp` lines 729-733: `flags |= 0x02` sets server-side filter; lines 747-754: type string mapped to magic bytes for wire |
| 11 | cdb ls --type PUBK shows PUBK blobs even though they are normally hidden | VERIFIED | `commands.cpp` line 814: hide condition includes `type_filter.empty()` — when --type is set, hide list is bypassed |
| 12 | Unrecognized type prefixes display as DATA | VERIFIED | `cli/src/wire.h` line 228: `return "DATA";` as final fallback in `type_label()` |
| 13 | list_hashes correctly parses 44-byte entries without breaking rm/reshare commands | VERIFIED | `commands.cpp` line 874: `constexpr size_t ENTRY_SIZE = 44;` in `list_hashes()` function |

**Score:** 13/13 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/storage/storage.h` | BlobRef struct with blob_type field | VERIFIED | Line 77: `std::array<uint8_t, 4> blob_type{}; // First 4 bytes of blob data` |
| `db/storage/storage.cpp` | Type extraction on ingest, 36-byte seq_map values, schema v2 migration | VERIFIED | SCHEMA_VERSION=2 (line 121), extract_blob_type called in store_blob/store_blobs_atomic |
| `db/wire/codec.h` | extract_blob_type helper function | VERIFIED | Line 130: `inline std::array<uint8_t, 4> extract_blob_type(std::span<const uint8_t> data)` |
| `db/peer/message_dispatcher.cpp` | Flags parsing, type-based filtering, 44-byte ListResponse entries | VERIFIED | LIST_ENTRY_SIZE=44 (line 506), flags parsing (lines 450-462), type filter (lines 498-499) |
| `db/PROTOCOL.md` | Updated wire format documentation with flags field | VERIFIED | Lines 754-780: ListRequest 44-49 bytes with flags table, ListResponse count*44 entries with type field |
| `db/tests/storage/test_storage.cpp` | Tests for type indexing, migration, BlobRef population | VERIFIED | 5 new test cases with `[type_index]` and `[type_extensible]` tags at lines 2420-2523 |
| `cli/src/wire.h` | Magic constants, type_label function, is_hidden_type function | VERIFIED | CENV_MAGIC (209), TOMBSTONE_MAGIC_CLI (212), DELEGATION_MAGIC_CLI (215), CDAT_MAGIC (218), type_label (222), is_hidden_type (233) |
| `cli/src/commands.h` | Updated ls() signature with raw and type_filter params | VERIFIED | Lines 41-43: `bool raw = false, const std::string& type_filter = ""` |
| `cli/src/commands.cpp` | ls() with 44-byte entry parsing, hide list, type labels; list_hashes() with 44-byte entries | VERIFIED | ENTRY_SIZE=44 at lines 797 and 874; is_hidden_type and type_label in ls() loop |
| `cli/src/main.cpp` | ls --raw and --type flag parsing | VERIFIED | Lines 555-563: strcmp for --raw and --type; line 574: `cmd::ls(..., raw, type_filter)` |

### Key Link Verification

| From | To | Via | Status | Details |
|------|-----|-----|--------|---------|
| `db/storage/storage.cpp` | `db/wire/codec.h` | `extract_blob_type` called in store_blob and store_blobs_atomic | WIRED | Lines 525, 744 in storage.cpp call `wire::extract_blob_type` |
| `db/peer/message_dispatcher.cpp` | `db/storage/storage.h` | `BlobRef.blob_type` used for tombstone/delegation filtering | WIRED | Lines 482, 487, 499, 1273, 1274: `ref.blob_type.data()` used in memcmp |
| `db/peer/message_dispatcher.cpp` | `db/storage/storage.cpp` | `get_blob_refs_since` returns BlobRef with populated blob_type | WIRED | Line 469 calls `get_blob_refs_since`; storage.cpp lines 960-976 populate blob_type from 36-byte seq_map values |
| `cli/src/main.cpp` | `cli/src/commands.cpp` | `cmd::ls(identity_dir_str, namespace_hex, opts, raw, type_filter)` | WIRED | Line 574 in main.cpp calls cmd::ls with all 5 params |
| `cli/src/commands.cpp` | `cli/src/wire.h` | `type_label()` and `is_hidden_type()` called in ls() loop | WIRED | Lines 814, 821 in commands.cpp call is_hidden_type and type_label |

### Data-Flow Trace (Level 4)

This phase modifies storage and wire protocol (no React/UI components). Data flow traced through the pipeline:

| Stage | Data Variable | Source | Real Data | Status |
|-------|--------------|--------|-----------|--------|
| Ingest | `blob_type` | `extract_blob_type(blob.data)` — first 4 bytes of actual blob data | Yes | FLOWING |
| seq_map write | 36-byte value `[hash:32][type:4]` | Written in store_blob/store_blobs_atomic after extraction | Yes | FLOWING |
| get_blob_refs_since | `ref.blob_type` | Read from seq_map value bytes 32-35 if length >= 36 | Yes | FLOWING |
| ListResponse | type field at entry offset 40 | Copied from `filtered_refs[i].blob_type.data()` (line 518) | Yes | FLOWING |
| CLI ls() | `type_ptr = entry + 40` | Decoded from 44-byte ListResponse entry | Yes | FLOWING |

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| `cdb ls --help` shows --raw and --type flags | `cli/build/cdb ls --help` | Output shows only `Usage: chromatindb-cli ls [host[:port]] [--namespace <hex>]` — old binary | FAIL (stale binary) |
| `--raw` flag string in source | `grep -n '\"--raw\"' cli/src/main.cpp` | Lines 534, 538, 555 confirm --raw in source | PASS |
| `--type` flag string in source | `grep -n '\"--type\"' cli/src/main.cpp` | Lines 534, 539, 558 confirm --type in source | PASS |
| type_label function exported | `grep -n 'type_label' cli/src/wire.h` | Lines 222-228 show complete implementation | PASS |
| is_hidden_type checks PUBK/CDAT/DLGT | `grep -n 'is_hidden_type' cli/src/wire.h` | Lines 233-237 check all three hidden types | PASS |
| ENTRY_SIZE=44 in both ls and list_hashes | `grep -n 'ENTRY_SIZE = 44' cli/src/commands.cpp` | Lines 797 and 874 — two matches | PASS |

**Note:** The stale binary is the only spot-check failure. The source code correctly implements all behaviors as verified above. A rebuild will resolve this.

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| TYPE-01 | 117-01 | Node indexes first 4 bytes of blob data as blob_type on ingest | SATISFIED | extract_blob_type called in store_blob (line 525) and store_blobs_atomic (line 744) in storage.cpp |
| TYPE-02 | 117-01 | ListResponse includes 4-byte type per entry | SATISFIED | LIST_ENTRY_SIZE=44, type written at entry offset 40 in message_dispatcher.cpp line 518 |
| TYPE-03 | 117-01 | ListRequest supports optional type filter | SATISFIED | Flags byte at offset 44 (bit 1 + 4-byte type_filter at 45-48) parsed in message_dispatcher.cpp lines 450-462 |
| TYPE-04 | 117-01 | Extensible — any 4-byte prefix works without node changes | SATISFIED | extract_blob_type is generic; type_label returns "DATA" for unknowns; test at test_storage.cpp line 2460 proves arbitrary prefix |
| ERGO-02 | 117-02 | cdb ls hides infrastructure blobs (CDAT, PUBK, delegations) by default | SATISFIED | is_hidden_type() called in commands.cpp ls() loop line 814 silently filters PUBK/CDAT/DLGT |
| ERGO-03 | 117-02 | cdb ls --raw shows all blobs including infrastructure types | SATISFIED | --raw sets include_all flag (bit 0) in ListRequest; commands.cpp hide condition requires `!raw` |

**No orphaned requirements.** REQUIREMENTS.md maps exactly TYPE-01, TYPE-02, TYPE-03, TYPE-04, ERGO-02, ERGO-03 to Phase 117 — all claimed and satisfied.

### Anti-Patterns Found

None. Scanned all 8 modified files for TODO/FIXME/HACK/PLACEHOLDER patterns — zero hits.

### Human Verification Required

#### 1. Rebuild CLI Binary

**Test:** Run `cmake --build cli/build -j$(nproc)` then `./cli/build/cdb ls --help`
**Expected:** Output includes `--raw` for showing all blobs and `--type <TYPE>` for filtering
**Why human:** Binary at `cli/build/chromatindb-cli` was compiled 2026-04-15 14:50, before Plan 02 commits (de957650, 51d2eace) on 2026-04-16. Source is correct; binary needs rebuild.

#### 2. Verify Default ls Hides Infrastructure Blobs Against Live Node

**Test:** Run `cdb ls` against live node at 192.168.1.73 with a namespace that has both PUBK blobs and CENV blobs
**Expected:** Only CENV/DATA blobs appear; PUBK, CDAT, DLGT blobs are silently absent
**Why human:** End-to-end wire behavior (44-byte ListResponse decoded correctly, hide list applied) requires a live populated node

#### 3. Verify cdb ls --raw Shows All Types

**Test:** Run `cdb ls --raw` against live node
**Expected:** All blobs appear with 4-character type labels (e.g., `<hash>  PUBK`, `<hash>  CENV`); include_all flag (0x01) sent to node
**Why human:** Requires live node with populated type index after schema v2 migration

#### 4. Verify cdb ls --type PUBK Bypasses Hide List

**Test:** Run `cdb ls --type PUBK` against live node
**Expected:** Only PUBK blobs listed; type is displayed even though it's in the default hide list
**Why human:** Server-side filtering behavior (type_filter bytes sent in ListRequest) requires live node

### Gaps Summary

No gaps. All 13 source-level truths are verified. The stale binary is a rebuild issue, not a code correctness issue — the implementation exists and is correct in source. Human verification covers runtime confirmation.

---

_Verified: 2026-04-16T10:30:00Z_
_Verifier: Claude (gsd-verifier)_
