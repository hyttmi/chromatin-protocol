# Phase 3: Blob Engine - Context

**Gathered:** 2026-03-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Ingest pipeline that verifies namespace ownership and signature before storing blobs, query interface for retrieval by seq_num and namespace listing, and write ACKs. All testable without network — this is the engine layer between raw storage (Phase 2) and networking (Phase 4).

</domain>

<decisions>
## Implementation Decisions

### Rejection behavior
- Specific error codes: namespace_mismatch, invalid_signature, malformed_blob (not opaque "rejected")
- Fail-fast validation order: namespace check (cheap SHA3 comparison) before signature verify (expensive ML-DSA-87)
- Log rejections at warn level with truncated namespace ID
- Accept blobs for ANY valid namespace, not just the local node's — replication requires storing other nodes' blobs

### Write ACK contents
- ACK includes: blob hash, assigned seq_num, status (stored/duplicate), error reason on rejection
- Idempotent on duplicate: returns existing seq_num (same response shape as new storage)
- Replication count field stubbed to 1 (local only) — ACKW-02 (v2) will populate properly
- Rejection ACK carries specific error reason, consistent with the rejection behavior decision

### Query interface
- `get_blobs_since_seq`: optional `max_count` parameter (default returns all, caller can limit)
- `list_namespaces`: returns namespace IDs + latest seq_num per namespace (enables efficient sync negotiation)
- `get_blob_by_hash`: exposed through engine (wraps Storage::get_blob) — sync needs this for requesting specific missing blobs
- Single `BlobEngine` class handles both ingest (write path) and queries (read path)

### Claude's Discretion
- Internal struct naming and layout for WriteAck / IngestError types
- Exact error enum values beyond the three specified
- Whether BlobEngine owns Storage or takes a reference
- Test organization and helper utilities

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Storage` class (`src/storage/storage.h`): store_blob(), get_blob(), has_blob(), get_blobs_by_seq(), run_expiry_scan() — raw storage with no verification
- `wire::BlobData` struct (`src/wire/codec.h`): namespace_id, pubkey, data, ttl, timestamp, signature
- `wire::build_signing_input()`: builds canonical signing input (namespace || data || ttl || timestamp)
- `wire::blob_hash()`: SHA3-256 of encoded FlatBuffer blob for content-addressed dedup
- `crypto::Signer::verify()`: static ML-DSA-87 signature verification
- `crypto::hash::sha3_256()`: namespace derivation check
- `identity::NodeIdentity`: local node keypair + namespace (not needed for engine verification, but useful for tests)

### Established Patterns
- RAII wrappers with move semantics, no copy (Signer, Storage)
- Pimpl idiom for Storage (struct Impl)
- Injectable dependencies (Clock function for Storage)
- `std::span<const uint8_t, 32>` for fixed-size byte arrays (namespace, hash)
- `std::optional` for nullable returns, `std::vector` for variable-length results
- Flat src/ layout with module subdirectories (crypto/, wire/, storage/, identity/)
- `chromatin::` namespace with module sub-namespaces

### Integration Points
- BlobEngine wraps Storage — calls store_blob() after verification passes
- BlobEngine uses crypto::Signer::verify() and crypto::hash::sha3_256() for validation
- BlobEngine uses wire::build_signing_input() to reconstruct the signed content for verification
- Phase 4 (Networking) will call BlobEngine for all blob operations over the network

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. The engine is an internal layer with a clean C++ API.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 03-blob-engine*
*Context gathered: 2026-03-03*
