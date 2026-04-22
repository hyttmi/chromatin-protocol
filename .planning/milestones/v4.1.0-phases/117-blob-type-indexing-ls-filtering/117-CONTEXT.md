# Phase 117: Blob Type Indexing + ls Filtering - Context

**Gathered:** 2026-04-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Node indexes the first 4 bytes of blob data as `blob_type` on ingest, includes type in ListResponse entries, adds optional flags byte to ListRequest, and `cdb ls` hides infrastructure blobs by default showing hash + type label per entry.

</domain>

<decisions>
## Implementation Decisions

### Node — Type Index
- **D-01:** Node indexes first 4 bytes of blob data as `blob_type` on ingest. Stored alongside blob_hash in a type index. No size field — just the 4-byte type prefix.
- **D-02:** Blobs with <4 bytes of data: Claude's discretion on zero-padding vs sentinel.
- **D-03:** Extensible — any 4-byte prefix works without node code changes (TYPE-04). The node treats all types equally.

### Node — Filtering Architecture
- **D-04:** Node keeps existing server-side filtering for protocol internals: tombstones (0xDEADBEEF), delegations (0xDE1E6A7E), and expired blobs. These are protocol machinery, not application data — no client should need to know about them.
- **D-05:** Node does NOT filter application-level types (PUBK, CENV, CDAT, user-defined). That's the client's decision.
- **D-06:** The type index eliminates the need to load full blob data for tombstone/delegation checks in ListResponse — the 4-byte prefix is sufficient to identify them.

### Node — ListRequest Wire Format
- **D-07:** ListRequest extended with optional 1-byte flags field at offset 44. Length-based detection: 44 bytes = no flags (current behavior), 45+ bytes = flags present.
- **D-08:** Flags bit 0: `include_all` — skip tombstone/delegation/expired filtering, return everything. Used by `cdb ls --raw` and admin/debug tools.
- **D-09:** TYPE-03 type filter implementation: Claude's discretion on whether to use flags bit 1 + 4-byte type_filter field, or client-side filtering via ListResponse type field. Both approaches satisfy TYPE-03.

### Node — ListResponse Wire Format
- **D-10:** ListResponse entry size changes from 40 to 44 bytes: `[hash:32][seq:8BE][type:4]`. The 4-byte type is appended to each entry.
- **D-11:** No size field in ListResponse. Kept minimal to avoid node architecture drift.

### CLI — ls Default Output
- **D-12:** `cdb ls` shows hash + type label per line. Infrastructure blobs silently hidden (no summary footer).
- **D-13:** CLI-side hide list: PUBK (0x50554B42), CDAT (future, Phase 119), DLGT (0xDE1E6A7E defense-in-depth).
- **D-14:** CENV blobs (0x43454E56) are user data — always shown in default mode.
- **D-15:** Unrecognized type prefixes labeled as `DATA` in output.

### CLI — ls --raw Output
- **D-16:** `cdb ls --raw` shows all blobs (sends `include_all` flag to node) with same hash + type label format.
- **D-17:** No change in label format between default and --raw. `--raw` only changes which blobs are shown.

### CLI — Type Labels
- **D-18:** Human-readable 4-char labels in both default and --raw modes: `CENV`, `PUBK`, `DATA` (unknown), `TOMB` (tombstone), `DLGT` (delegation), `CDAT` (future).
- **D-19:** No raw hex display mode. Labels are always human-readable.

### CLI — --type Flag
- **D-20:** `cdb ls --type CENV` filters output to only blobs matching that type label. Uses ListRequest type filter server-side for efficiency.
- **D-21:** `--type` bypasses the CLI hide list — `cdb ls --type PUBK` shows PUBK blobs even though they're normally hidden.

### CLI — Hide List
- **D-22:** Hardcoded in CLI. No config file for hidden types (YAGNI). New types require a code change.
- **D-23:** Silent filtering — no "N blobs hidden" footer.

### Claude's Discretion
- Short blob type handling (zero-padding approach for <4 byte blobs)
- TYPE-03 server-side type filter wire format details (flags bit + field, or client-side only)
- Hide list implementation (compile-time array of magic prefixes)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Wire Protocol
- `db/PROTOCOL.md` — ListRequest/ListResponse wire format (lines 754-776), tombstone/delegation filtering rules (line 776), ErrorResponse types (line 436)
- `db/schemas/transport.fbs` — TransportMsgType enum (ListRequest=33, ListResponse=34)

### Node Implementation
- `db/peer/message_dispatcher.cpp` — ListRequest handler (lines 437-500), current tombstone/delegation filtering (lines 463-471)
- `db/storage/storage.h` — BlobRef struct (lines 74-77), get_blob_refs_since (line 163)
- `db/storage/storage.cpp` — Storage layer implementation
- `db/wire/codec.h` — TOMBSTONE_MAGIC (0xDEADBEEF), DELEGATION_MAGIC (0xDE1E6A7E), is_tombstone(), is_delegation()
- `db/wire/codec.cpp` — Magic prefix check implementations

### CLI Implementation
- `cli/src/main.cpp` — ls command parsing (lines 531-557), argument dispatch
- `cli/src/commands.h` — ls() function signature (line 41)
- `cli/src/commands.cpp` — ls() implementation (lines 710-786), ListRequest build (44-byte payload), ListResponse parse (40 bytes per entry)
- `cli/src/wire.h` — PUBKEY_MAGIC (0x50554B42), MsgType enum, wire helpers
- `cli/src/envelope.cpp` — CENV magic (0x43454E56)

### Requirements
- `.planning/REQUIREMENTS.md` — TYPE-01 through TYPE-04, ERGO-02, ERGO-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `wire::is_tombstone()` / `wire::is_delegation()`: Existing 4-byte magic prefix checks. Pattern to follow for type identification.
- `TOMBSTONE_MAGIC` / `DELEGATION_MAGIC` / `PUBKEY_MAGIC`: Existing constexpr magic arrays in codec.h and wire.h.
- `BlobRef` struct: Currently {blob_hash, seq_num}. Needs extending with type field.
- `get_blob_refs_since()`: Storage query used by ListRequest handler. Needs to return type data.

### Established Patterns
- Wire format uses big-endian encoding everywhere. New fields must follow.
- Length-based optional fields: precedent exists in the protocol (various request types have variable lengths).
- ListResponse handler in message_dispatcher.cpp: single coroutine with overflow-checked arithmetic.
- CLI commands.cpp: raw byte manipulation for request/response building, `store_u64_be`/`load_u32_be` helpers.

### Integration Points
- `storage.h` / `storage.cpp`: New type index (libmdbx sub-database), BlobRef extension, backfill for existing blobs.
- `message_dispatcher.cpp`: ListRequest handler needs flags parsing, type inclusion in response entries.
- `commands.cpp` ls(): ListResponse parsing changes (44-byte entries), type label mapping, hide list filtering.
- `main.cpp`: New `--raw` and `--type` flags for ls command.
- `PROTOCOL.md`: Updated ListRequest/ListResponse wire format documentation.

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<deferred>
## Deferred Ideas

- **Blob size in ls output** — Adding data_size to ListResponse would require a new index field and backfill. Deferred to avoid node architecture drift. Could be added in a future phase if needed.

</deferred>

---

*Phase: 117-blob-type-indexing-ls-filtering*
*Context gathered: 2026-04-16*
