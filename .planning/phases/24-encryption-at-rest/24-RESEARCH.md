# Phase 24: Encryption at Rest - Research

**Researched:** 2026-03-14
**Domain:** Data-at-rest encryption using existing libsodium primitives
**Confidence:** HIGH

## Summary

Phase 24 adds transparent encryption of blob payloads stored in libmdbx. The entire crypto stack already exists in the codebase: `crypto::AEAD` (ChaCha20-Poly1305) and `crypto::KDF` (HKDF-SHA256) are shipped, tested, and used by the transport layer. The implementation is a storage-layer concern only -- encrypt after `wire::encode_blob()`, decrypt before `wire::decode_blob()`, with a master key loaded at startup.

The scope is narrow and well-bounded. All integration points are in `storage.cpp` (6 methods read blob values from mdbx). The master key lifecycle follows the exact same `load_or_generate` pattern as `NodeIdentity`. No new dependencies, no new crypto, no protocol changes.

**Primary recommendation:** Add an `EncryptionKey` class that wraps master key I/O and HKDF derivation, then add encrypt/decrypt helper functions in `storage.cpp` that handle the `[version][nonce][ciphertext+tag]` envelope format. Modify all 6 blob-reading paths in Storage::Impl to decrypt transparently.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Encrypt the full `wire::encode_blob()` output (namespace, data, TTL, timestamp, signature -- everything)
- mdbx keys (namespace||content_hash) stay plaintext -- needed for indexing and dedup
- Index maps (expiry, sequence, delegation, tombstone) are key-only references -- no encryption needed
- Always-on: no config toggle, no unencrypted code path. Every node encrypts at rest.
- Random 12-byte nonce per blob via `randombytes_buf`
- Nonce stored prepended to ciphertext in mdbx (format: `[version][nonce][ciphertext+tag]`)
- Check-then-encrypt for dedup: check if blob_key exists in mdbx BEFORE encrypting (matches existing `store_blob` flow)
- File: `data_dir/master.key` (alongside `node.key` and `node.pub`)
- Format: raw 32 bytes, no header or metadata
- Permissions: 0600 (restricted, same as identity keys)
- Auto-generate on first run (same `load_or_generate` pattern as identity)
- If master.key is missing but encrypted data exists: fatal error, refuse to start
- Prepend a 1-byte version tag (e.g., `0x01`) to all encrypted values in mdbx
- On startup, sample existing blob values -- if any lack the version header, refuse to start with clear error
- No migration path -- unencrypted databases must be wiped
- Detection via version header check on startup prevents accidental mixed state

### Claude's Discretion
- HKDF context/info label for deriving blob encryption key from master key
- AEAD associated data (AD) binding strategy
- Exact version byte value and format
- Startup sampling strategy for unencrypted data detection (how many blobs to check)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| EAR-01 | Blob payloads encrypted with ChaCha20-Poly1305 before writing to libmdbx | `crypto::AEAD::encrypt()` exists, tested. Encrypt `wire::encode_blob()` output in `store_blob()` before mdbx `put`. |
| EAR-02 | Encryption key derived from node-local master key via HKDF-SHA256 | `crypto::KDF::derive()` exists, tested. One-time derivation at Storage construction. |
| EAR-03 | Master key stored in separate file with restricted permissions | Follow `NodeIdentity::load_or_generate` pattern. File: `data_dir/master.key`, 32 bytes, 0600. |
| EAR-04 | Encrypted blobs decrypted transparently on read | Decrypt in all 6 blob-value-reading paths in `storage.cpp` before `wire::decode_blob()`. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| libsodium | latest (FetchContent) | ChaCha20-Poly1305 AEAD, HKDF-SHA256, randombytes_buf | Already in project, proven in transport layer |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| crypto::AEAD | existing | Encrypt/decrypt blobs | Every store/retrieve |
| crypto::KDF | existing | Derive blob key from master key | Once at startup |
| crypto::SecureBytes | existing | Secure key storage (mlock, zero-on-destruct) | Master key + derived key |

### Alternatives Considered
None -- all required primitives exist in the codebase.

## Architecture Patterns

### Encryption Envelope Format
```
[version:1][nonce:12][ciphertext+tag:N+16]
```
Total overhead per blob: 29 bytes (1 + 12 + 16).

- **Version byte** (`0x01`): Identifies encryption scheme version. Future key rotation can use `0x02`.
- **Nonce**: Random 12 bytes via `randombytes_buf()`. Random nonces are safe here because each blob gets a unique derived key (see below) OR because the nonce space (2^96) makes collision probability negligible for per-node storage volumes.
- **Ciphertext + tag**: Standard ChaCha20-Poly1305 IETF output.

### Key Derivation Strategy
```
master_key (32 bytes, from file)
    |
    v
HKDF-SHA256(salt=empty, ikm=master_key, info="chromatindb-dare-v1", len=32)
    |
    v
blob_encryption_key (32 bytes, stored in SecureBytes in Impl)
```

Single derived key for all blobs (not per-blob derivation). Rationale:
- CONTEXT.md says "one key per node, not per blob" (EAR-02)
- Per-blob derivation would require storing the derivation input alongside the blob, adding complexity
- Random nonces ensure each encryption is unique
- The HKDF step separates the master key from the working key, enabling future key rotation by changing the context label

### AEAD Associated Data (AD) Binding
Use the mdbx key (namespace:32 || content_hash:32 = 64 bytes) as the AEAD associated data.

Rationale:
- Binds ciphertext to its storage location -- prevents an attacker from swapping encrypted blob values between different keys in the database
- The mdbx key is already available at both encrypt and decrypt time (it's the lookup key)
- Zero additional storage cost
- Standard practice for database encryption

### Storage Integration Points

Six methods in `storage.cpp` read blob values from `blobs_map`:

| Method | Operation | Encryption Action |
|--------|-----------|-------------------|
| `store_blob()` | Write | Encrypt after `wire::encode_blob()`, before `txn.upsert()` |
| `get_blob()` | Read | Decrypt after `txn.get()`, before `wire::decode_blob()` |
| `get_blobs_by_seq()` | Read | Decrypt each blob value before `wire::decode_blob()` |
| `delete_blob_data()` | Read+Delete | Decrypt existing value (for TTL/tombstone/delegation checks), then delete |
| `run_expiry_scan()` | Read+Delete | Decrypt blob value (for tombstone check), then delete |
| `has_blob()` | Read (existence only) | No decryption needed -- only checks `data() != nullptr` |

Note: `has_blob()` does NOT need decryption since it only checks existence, not value content.

### Master Key Lifecycle

```
Startup:
  1. Check if master.key exists in data_dir
     - YES: Load raw 32 bytes, validate size, set permissions if needed
     - NO:
       a. Check if blobs_map has any entries
          - YES (encrypted data exists): Fatal error, refuse to start
          - NO (fresh database): Generate 32 random bytes, write to master.key, set 0600
  2. Derive blob_encryption_key via HKDF
  3. Validate existing data (version header check on sample of blobs)
  4. Storage ready
```

### Startup Validation Strategy

Sample first N blob values from `blobs_map` to detect unencrypted data:
- **Sample size**: Check ALL entries (cursor scan, bail on first mismatch). The database is opened in a read txn, this is cheap.
- **Check**: First byte must be `0x01` (version header). If any value has a different first byte, it's either unencrypted FlatBuffer data or a different version.
- **On failure**: Fatal error with message: "Database contains unencrypted data. Delete data_dir and restart."

Why check all, not sample: This is pre-release software with no migration path. A full scan is a one-time startup cost that prevents any possibility of mixed encrypted/unencrypted state. For a typical node with thousands of blobs, scanning first bytes of all values takes milliseconds.

### Pattern: Encrypt/Decrypt Helpers in Impl

Add two private methods to `Storage::Impl`:

```cpp
// Encrypt encoded blob -> [version][nonce][ciphertext+tag]
std::vector<uint8_t> encrypt_value(
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> blob_key_bytes);  // 64-byte mdbx key as AD

// Decrypt [version][nonce][ciphertext+tag] -> encoded blob
std::vector<uint8_t> decrypt_value(
    std::span<const uint8_t> encrypted,
    std::span<const uint8_t> blob_key_bytes);  // 64-byte mdbx key as AD
```

These encapsulate the envelope format and are called at every read/write point.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| AEAD encryption | Custom cipher wrapper | `crypto::AEAD::encrypt/decrypt` | Already tested, handles nonce/key validation |
| Key derivation | Manual HMAC chain | `crypto::KDF::derive` | RFC 5869 compliant, handles extract+expand |
| Random nonce | Custom PRNG | `randombytes_buf` (via libsodium) | CSPRNG, no seeding concerns |
| Secure key memory | Manual memset | `crypto::SecureBytes` | Zeroes on destruct, move-only |
| Key file I/O | Custom file handling | Follow `identity.cpp` pattern | Proven pattern, correct permissions |

## Common Pitfalls

### Pitfall 1: Encrypting Before Dedup Check
**What goes wrong:** Encrypting the blob value, computing the hash of ciphertext, and using that for dedup -- every store of the same plaintext would produce different ciphertext (random nonce), breaking content-addressed dedup.
**Why it happens:** Temptation to encrypt early in the pipeline.
**How to avoid:** The existing flow already does dedup check BEFORE writing. Keep it: hash plaintext, check existence, THEN encrypt for storage. CONTEXT.md explicitly mandates this ("check-then-encrypt for dedup").
**Warning signs:** Test `Storage deduplicates by content hash` would fail.

### Pitfall 2: Forgetting a Read Path
**What goes wrong:** One of the 6 blob-reading paths returns encrypted data instead of decrypted, causing `wire::decode_blob()` to throw or return garbage.
**Why it happens:** Not auditing all read paths in storage.cpp.
**How to avoid:** Audit all 6 methods listed above. Add a helper function and call it consistently. Tests will catch missing paths.
**Warning signs:** Existing tests fail with decode errors.

### Pitfall 3: Not Passing AD Consistently
**What goes wrong:** Encrypting with AD but decrypting without (or vice versa) causes authentication failure.
**Why it happens:** The mdbx key is available in different forms at different call sites.
**How to avoid:** Both `encrypt_value` and `decrypt_value` take the 64-byte blob_key as parameter. Build it once, pass it through.
**Warning signs:** `crypto::AEAD::decrypt` returns nullopt.

### Pitfall 4: Master Key File Without Restricted Permissions
**What goes wrong:** Key file is world-readable, violating security requirement.
**Why it happens:** Default file creation umask may allow group/other read.
**How to avoid:** Use `std::filesystem::permissions()` to set 0600 after writing, OR use platform-specific `open(path, O_WRONLY|O_CREAT, 0600)`.
**Warning signs:** `ls -la data_dir/master.key` shows wrong permissions.

### Pitfall 5: Missing Version Header in Decrypt Path
**What goes wrong:** Decrypt tries to treat the version byte as part of the nonce, producing garbage.
**Why it happens:** Forget to skip the first byte when parsing the encrypted envelope.
**How to avoid:** `decrypt_value` must: validate `encrypted[0] == 0x01`, extract nonce from `[1..13)`, extract ciphertext from `[13..end)`.
**Warning signs:** All decrypt operations fail.

## Code Examples

### Master Key Load/Generate (following identity pattern)
```cpp
// In a new file: db/crypto/master_key.h / .cpp
crypto::SecureBytes load_or_generate_master_key(const std::filesystem::path& data_dir) {
    auto key_path = data_dir / "master.key";

    if (std::filesystem::exists(key_path)) {
        // Load existing
        std::ifstream f(key_path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open master.key");

        crypto::SecureBytes key(32);
        f.read(reinterpret_cast<char*>(key.data()), 32);
        if (f.gcount() != 32) throw std::runtime_error("Invalid master.key size");

        return key;
    }

    // Generate new
    crypto::SecureBytes key(32);
    randombytes_buf(key.data(), 32);

    // Write with restricted permissions
    {
        std::ofstream f(key_path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot write master.key");
        f.write(reinterpret_cast<const char*>(key.data()), 32);
    }
    std::filesystem::permissions(key_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    return key;
}
```

### Envelope Encrypt/Decrypt
```cpp
static constexpr uint8_t ENCRYPTION_VERSION = 0x01;
static constexpr size_t ENVELOPE_OVERHEAD = 1 + crypto::AEAD::NONCE_SIZE + crypto::AEAD::TAG_SIZE; // 29

std::vector<uint8_t> encrypt_value(
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> ad) {

    std::array<uint8_t, crypto::AEAD::NONCE_SIZE> nonce;
    randombytes_buf(nonce.data(), nonce.size());

    auto ciphertext = crypto::AEAD::encrypt(plaintext, ad, nonce, blob_key_.span());

    // Assemble envelope: [version][nonce][ciphertext+tag]
    std::vector<uint8_t> envelope;
    envelope.reserve(1 + nonce.size() + ciphertext.size());
    envelope.push_back(ENCRYPTION_VERSION);
    envelope.insert(envelope.end(), nonce.begin(), nonce.end());
    envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());

    return envelope;
}

std::vector<uint8_t> decrypt_value(
    std::span<const uint8_t> encrypted,
    std::span<const uint8_t> ad) {

    if (encrypted.size() < ENVELOPE_OVERHEAD) {
        throw std::runtime_error("Encrypted value too short");
    }
    if (encrypted[0] != ENCRYPTION_VERSION) {
        throw std::runtime_error("Unknown encryption version");
    }

    auto nonce = encrypted.subspan(1, crypto::AEAD::NONCE_SIZE);
    auto ciphertext = encrypted.subspan(1 + crypto::AEAD::NONCE_SIZE);

    auto plaintext = crypto::AEAD::decrypt(ciphertext, ad, nonce, blob_key_.span());
    if (!plaintext) {
        throw std::runtime_error("AEAD decryption failed (authentication error)");
    }

    return *plaintext;
}
```

### HKDF Key Derivation
```cpp
// In Storage::Impl constructor, after loading master_key:
auto blob_key = crypto::KDF::derive(
    {},  // empty salt (HKDF extract with no salt uses zero-filled salt)
    master_key.span(),
    "chromatindb-dare-v1",  // context/info label
    crypto::AEAD::KEY_SIZE);  // 32 bytes
```

## Open Questions

1. **None** -- All decisions are locked by CONTEXT.md or within Claude's discretion with clear recommendations above.

## Sources

### Primary (HIGH confidence)
- Codebase: `db/crypto/aead.h` + `aead.cpp` -- ChaCha20-Poly1305 API verified
- Codebase: `db/crypto/kdf.h` + `kdf.cpp` -- HKDF-SHA256 API verified
- Codebase: `db/crypto/secure_bytes.h` -- SecureBytes API verified
- Codebase: `db/storage/storage.cpp` -- All 6 blob-reading paths identified and audited
- Codebase: `db/identity/identity.cpp` -- `load_or_generate` pattern for key files
- Codebase: `db/main.cpp` -- Storage construction flow, identity loading

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all primitives already exist and are tested in the codebase
- Architecture: HIGH -- integration points clearly identified in storage.cpp, envelope format follows standard DARE practices
- Pitfalls: HIGH -- derived from actual code analysis, not hypothetical scenarios

**Research date:** 2026-03-14
**Valid until:** Indefinite -- all findings are based on codebase analysis, not external library versions
