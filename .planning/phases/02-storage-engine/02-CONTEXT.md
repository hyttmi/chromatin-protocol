# Phase 2: Storage Engine - Context

**Gathered:** 2026-03-03
**Status:** Ready for planning

<domain>
## Phase Boundary

libmdbx wrapper with persistent blob storage, sequence indexing, TTL expiry, and crash recovery. Node can store, retrieve, deduplicate, index, and expire blobs with ACID guarantees. No networking, no blob validation (signature/namespace verification is Phase 3), no event loop.

</domain>

<decisions>
## Implementation Decisions

### Sub-database scope
- Only 3 sub-databases: blobs, sequence, expiry. Peers sub-database deferred to Phase 5 (YAGNI -- nothing tests it until sync exists)
- Eager initialization: create all 3 sub-databases in a single write transaction at startup
- Auto-grow map size: start with a reasonable default, let libmdbx geometry auto-grow as needed
- Storage engine owns data directory lifecycle: creates data_dir if missing, validates write permissions on open

### Expiry scanner design
- Expose `run_expiry_scan()` as a public method call -- no background thread, no timer. Tests call it directly. Phase 4 wires it to an event loop timer
- Purge all expired blobs in one go per scan call (no batch limit)
- Clean deletion from all 3 indexes (blobs, sequence, expiry) in a single write transaction. Seq_nums will have gaps after expiry -- callers must handle this
- Injectable clock function (defaults to system time). Tests inject fake clock to control time for deterministic expiry testing

### Storage API surface
- BlobData-aware API: takes/returns `wire::BlobData` directly. Storage internally encodes to FlatBuffers for persistence
- Single-blob writes only: `store_blob()` handles one blob per call. Batch writes added later when Phase 3/5 needs them
- Minimal query set: `get_blob(ns, hash)`, `get_blobs_by_seq(ns, since_seq)`, `has_blob(ns, hash)`. Additional queries (list_namespaces, stats) added in Phase 3
- Result types for error handling: `std::optional` for lookups, enum result codes (Stored, Duplicate, Error) for writes. No exceptions in the hot path

### Value storage format
- Full FlatBuffer-encoded blob as value in blobs sub-database (complete `wire::encode_blob()` output). Includes all fields. Zero-copy read potential with mmap
- Sequence index value: blob hash only (32 bytes). No denormalization -- to check expiry, do a second lookup
- Big-endian uint64 for seq_num (sequence keys) and expiry_timestamp (expiry keys). Lexicographic order == numeric order with libmdbx default comparator
- Derive seq_num from DB on each write: cursor to last entry for namespace, read, increment. Stateless -- crash recovery is trivial (no in-memory counter to lose)

### Claude's Discretion
- Exact libmdbx environment flags and configuration (NOSUBDIR, WRITEMAP, etc.)
- Initial map size and growth increment values
- Internal key construction helpers and byte serialization utilities
- Error message wording and spdlog integration points
- Test fixture design and helper utilities

</decisions>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches. The architecture research document (.planning/research/ARCHITECTURE.md) has detailed storage schema, key formats, and transaction discipline patterns that should be followed.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `chromatin::wire::BlobData`: Structured blob with namespace_id, pubkey, data, ttl, timestamp, signature -- storage API input/output type
- `chromatin::wire::encode_blob()` / `decode_blob()`: FlatBuffer serialization -- used to produce values stored in blobs sub-database
- `chromatin::wire::blob_hash()`: SHA3-256 of encoded blob -- produces the content-addressed hash used as part of the blob key
- `chromatin::crypto::sha3_256()`: General-purpose hashing -- available if needed for any additional hashing
- `chromatin::config::Config::data_dir`: Where the libmdbx database files should live
- `chromatin::config::BLOB_TTL_SECONDS = 604800`: Protocol constant for default TTL (7 days)

### Established Patterns
- `chromatin::` namespace with flat src/ layout (src/crypto/, src/wire/, src/config/, src/identity/)
- Catch2 for tests, spdlog for logging
- CMake with FetchContent for all dependencies (latest versions)
- RAII wrappers for external library resources (see crypto module)
- Header + source file pairs per module

### Integration Points
- New `src/storage/` directory following existing flat layout pattern
- CMake FetchContent to add libmdbx dependency
- Links against existing chromatin_wire and chromatin_crypto targets for encode/decode/hash
- Tests in `tests/storage/` following existing `tests/{module}/` pattern
- Config::data_dir provides the storage path

</code_context>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 02-storage-engine*
*Context gathered: 2026-03-03*
