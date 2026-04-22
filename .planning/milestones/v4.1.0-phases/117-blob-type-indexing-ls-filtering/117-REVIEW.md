---
phase: 117-blob-type-indexing-ls-filtering
reviewed: 2026-04-16T09:13:58Z
depth: standard
files_reviewed: 10
files_reviewed_list:
  - cli/src/commands.cpp
  - cli/src/commands.h
  - cli/src/main.cpp
  - cli/src/wire.h
  - db/peer/message_dispatcher.cpp
  - db/PROTOCOL.md
  - db/storage/storage.cpp
  - db/storage/storage.h
  - db/tests/storage/test_storage.cpp
  - db/wire/codec.h
findings:
  critical: 0
  warning: 2
  info: 2
  total: 4
status: issues_found
---

# Phase 117: Code Review Report

**Reviewed:** 2026-04-16T09:13:58Z
**Depth:** standard
**Files Reviewed:** 10
**Status:** issues_found

## Summary

Phase 117 adds blob type indexing to the storage layer (first 4 bytes of blob data stored in seq_map as a 36-byte value), extends the ListRequest/ListResponse wire format to include type prefix bytes and optional flags (include_all, type_filter), adds CLI ls flags (--raw, --type), and implements a schema v1->v2 migration for existing seq_map entries.

The core implementation is solid. The storage layer correctly extracts and stores the 4-byte type prefix on both `store_blob` and `store_blobs_atomic` paths. The schema migration safely zero-pads existing 32-byte values to 36 bytes. The wire format changes are correctly documented in PROTOCOL.md and consistently implemented across server (message_dispatcher.cpp) and client (commands.cpp, wire.h). The `extract_blob_type` utility in codec.h handles short data gracefully.

Two warnings are flagged below: one logic issue where `--type TOMB` (or `--type DLGT`) will silently return no results due to interaction between default server-side filtering and the type filter, and one pagination issue where the server applies filtering AFTER the pagination limit, which can cause the client to terminate early and miss entries.

## Warnings

### WR-01: --type filter for server-hidden types (TOMB, DLGT) always returns empty results

**File:** `db/peer/message_dispatcher.cpp:480-502` and `cli/src/commands.cpp:725-734`
**Issue:** When the CLI sends `--type TOMB` without `--raw`, the flags byte is `0x02` (type_filter present, but NOT include_all). The server first applies default filtering at line 480-496 (skipping tombstones and delegations because `!include_all` is true), then applies the type filter at line 498-502. Since tombstones were already removed in step 1, the type filter never matches anything. The same problem affects `--type DLGT`. The user would see zero results with no error, which is confusing.

The CLI comment at line 812 says "Per D-21: --type bypasses hide list" but this only applies to the CLIENT-side hide list (PUBK, CDAT, DLGT). The SERVER-side filtering of tombstones and delegations is not bypassed.

**Fix:** When `has_type_filter` is true on the server, the type filter should supersede the default tombstone/delegation exclusion. Move the type filter check before the default filter, or force `include_all` semantics when a type filter is present:

```cpp
// In message_dispatcher.cpp, ListRequest handler:
for (const auto& ref : refs) {
    // Apply type filter first if requested (supersedes default filtering)
    if (has_type_filter) {
        if (std::memcmp(ref.blob_type.data(), type_filter.data(), 4) != 0) {
            continue;
        }
        // Type filter match -- skip default tombstone/delegation/expiry filter
        // (caller explicitly asked for this type)
    } else if (!include_all) {
        // Default filtering: skip tombstones, delegations, expired
        if (std::memcmp(ref.blob_type.data(),
                        wire::TOMBSTONE_MAGIC.data(), 4) == 0) {
            continue;
        }
        if (std::memcmp(ref.blob_type.data(),
                        wire::DELEGATION_MAGIC.data(), 4) == 0) {
            continue;
        }
        auto blob = storage_.get_blob(ns, ref.blob_hash);
        if (!blob || wire::is_blob_expired(*blob, now)) {
            continue;
        }
    }
    filtered_refs.push_back(ref);
}
```

Alternatively, in the CLI, automatically set `include_all` when `--type` is specified (simpler, keeps server logic unchanged):

```cpp
// In commands.cpp ls():
if (send_type_filter) {
    flags |= 0x01;  // include_all -- let type_filter do the narrowing
    flags |= 0x02;
    payload_size = 49;
}
```

### WR-02: Pagination can terminate early when server-side filtering removes all entries in a page

**File:** `db/peer/message_dispatcher.cpp:469-504` and `cli/src/commands.cpp:807-829`
**Issue:** The server fetches `limit + 1` entries from `get_blob_refs_since`, then filters. If all 100 entries in a page are tombstones/delegations/expired, the response has `count=0` but `has_more=1`. The client at line 827 breaks when `count == 0`, correctly preventing an infinite loop but potentially missing entries beyond the current page.

This is a pre-existing design issue (the same filtering logic existed before Phase 117 for tombstones/delegations/expiry). Phase 117 did not introduce it but did not fix it either. For correctness, the server should continue fetching until it has enough filtered entries to fill the page, or until the namespace is exhausted.

**Fix:** This is a known pagination design limitation. A proper fix would require the server to loop internally, fetching additional batches until the filtered result set reaches `limit` entries or the namespace is exhausted. Given this is pre-existing and affects edge cases (pages entirely composed of filtered types), consider deferring to a dedicated fix phase. Noting it here for awareness.

## Info

### IN-01: Expiry check in ListRequest still requires full blob load

**File:** `db/peer/message_dispatcher.cpp:491-495`
**Issue:** While Phase 117 avoids full blob loads for tombstone/delegation type checks (using the new `blob_type` field from `BlobRef`), the expiry check at line 492-495 still calls `storage_.get_blob(ns, ref.blob_hash)` to access the blob's TTL and timestamp. This means every non-tombstone, non-delegation entry in a list response still requires a full blob decrypt+decode. The type prefix optimization only helps for tombstones and delegations.

**Fix:** Consider adding TTL and timestamp to the seq_map value in a future phase to eliminate the full blob load for expiry checks. Not actionable for Phase 117 as this would require a schema v3 migration.

### IN-02: Duplicate Tombstone section header in wire.h (CLI)

**File:** `cli/src/wire.h:160-163`
**Issue:** There are two "Tombstone" section comment headers. Lines 160-161 contain an empty Tombstone section, followed by the SHA3-256 section, and then another Tombstone section at line 172. This appears to be a copy-paste artifact that was already present.

**Fix:** Remove the empty Tombstone section comment at lines 160-161.

```cpp
// Remove these two lines:
// =============================================================================
// Tombstone
// =============================================================================
```

---

_Reviewed: 2026-04-16T09:13:58Z_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
