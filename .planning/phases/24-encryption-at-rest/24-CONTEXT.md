# Phase 24: Encryption at Rest - Context

**Gathered:** 2026-03-14
**Status:** Ready for planning

<domain>
## Phase Boundary

All blob payloads stored on disk are encrypted with ChaCha20-Poly1305 and decrypted transparently on read. A node-local master key is auto-generated on first run and used via HKDF-SHA256 to derive the encryption key. This phase does NOT include key rotation logic, external key management, or migration from unencrypted databases.

</domain>

<decisions>
## Implementation Decisions

### Encryption scope
- Encrypt the full `wire::encode_blob()` output (namespace, data, TTL, timestamp, signature — everything)
- mdbx keys (namespace||content_hash) stay plaintext — needed for indexing and dedup
- Index maps (expiry, sequence, delegation, tombstone) are key-only references — no encryption needed
- Always-on: no config toggle, no unencrypted code path. Every node encrypts at rest.

### Nonce strategy
- Random 12-byte nonce per blob via `randombytes_buf`
- Nonce stored prepended to ciphertext in mdbx (format: `[version][nonce][ciphertext+tag]`)
- Check-then-encrypt for dedup: check if blob_key exists in mdbx BEFORE encrypting (matches existing `store_blob` flow)

### Master key lifecycle
- File: `data_dir/master.key` (alongside `node.key` and `node.pub`)
- Format: raw 32 bytes, no header or metadata
- Permissions: 0600 (restricted, same as identity keys)
- Auto-generate on first run (same `load_or_generate` pattern as identity)
- If master.key is missing but encrypted data exists: fatal error, refuse to start ("master.key missing, cannot decrypt existing data")

### Version header
- Prepend a 1-byte version tag (e.g., `0x01`) to all encrypted values in mdbx
- Serves dual purpose: (1) forward-compat for future key rotation, (2) marker to detect unencrypted legacy data
- On startup, sample existing blob values — if any lack the version header, refuse to start with clear error: "Database contains unencrypted data. Delete data_dir and restart."

### Existing data migration
- No migration path — this is pre-release
- Unencrypted databases must be wiped (delete data_dir) before running with encryption
- Detection via version header check on startup prevents accidental mixed state

### Claude's Discretion
- HKDF context/info label for deriving blob encryption key from master key
- AEAD associated data (AD) binding strategy
- Exact version byte value and format
- Startup sampling strategy for unencrypted data detection (how many blobs to check)

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/crypto/aead.h`: ChaCha20-Poly1305 encrypt/decrypt with nonce+key+AD — ready to use directly
- `db/crypto/kdf.h`: HKDF-SHA256 extract/expand/derive — ready for master key → blob key derivation
- `db/crypto/secure_bytes.h`: SecureBytes type for key material (mlock'd memory)

### Established Patterns
- `identity::NodeIdentity::load_or_generate(data_dir)`: Pattern for auto-generating key files on first run
- `wire::encode_blob()` / `wire::decode_blob()`: Serialization boundary — encrypt output of encode, decrypt before decode
- Content-addressed dedup via SHA3-256 hash computed BEFORE storage — hash stays on plaintext, encryption happens after

### Integration Points
- `Storage::store_blob()`: Insert encryption between `wire::encode_blob()` and mdbx `put` (after dedup check)
- `Storage::get_blob()`: Insert decryption between mdbx `get` and `wire::decode_blob()`
- `Storage::Impl` constructor: Load master key, derive blob encryption key via HKDF
- `main.cpp`: Master key load/generate before `Storage` construction
- All other storage methods that read blob values (sync, expiry GC, tombstone checks) must also decrypt

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 24-encryption-at-rest*
*Context gathered: 2026-03-14*
