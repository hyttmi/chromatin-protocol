# Phase 1: Foundation - Research

**Researched:** 2026-03-03
**Domain:** Post-quantum cryptography, wire format, configuration, logging
**Confidence:** HIGH

## Summary

Phase 1 establishes the cryptographic primitives, wire format, configuration loading, and logging infrastructure for chromatindb. The stack is liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256) + libsodium (ChaCha20-Poly1305 AEAD, HKDF-SHA256 KDF) + FlatBuffers (wire format) + nlohmann/json (config) + spdlog (logging). All dependencies are well-documented C/C++ libraries with CMake support via FetchContent: liboqs and libsodium use git-based FetchContent, while FlatBuffers, nlohmann/json, and spdlog use URL-based FetchContent with release archives.

The critical architectural decision is that signing operates on a canonical byte concatenation (namespace || data || ttl || timestamp), NOT on FlatBuffer-encoded bytes. This keeps signing independent of wire format evolution. FlatBuffers provides the transport and storage format, but the signing domain is a fixed-layout binary concatenation.

**Primary recommendation:** Build bottom-up: CMake scaffold with all deps first, then crypto wrappers with tests, then FlatBuffers schemas, then config + logging, then identity (keypair + namespace derivation). Each layer is independently testable.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Use **libsodium** for AEAD and KDF
- Use **ChaCha20-Poly1305** (not AES-256-GCM) for channel encryption -- software-fast, constant-time, no hardware dependency
- **liboqs** handles all PQ primitives: ML-DSA-87 (signing), ML-KEM-1024 (key exchange), SHA3-256 (hashing)
- Thin C++ RAII wrappers around liboqs and libsodium C APIs -- don't over-abstract, keep it simple
- Sign **canonical concatenation**: `SHA3-256(namespace || data || ttl || timestamp)` -- independent of wire format
- Do NOT sign raw FlatBuffer bytes -- this would tie signing to serialization format forever
- Blob hash (content-addressed dedup key) covers **full blob including signature**: `SHA3-256(entire blob)` -- different signatures produce different hashes
- **No timestamp validation** on ingest -- accept any timestamp, clock is writer's problem
- FlatBuffers role: **transport AND on-disk storage format** -- single format everywhere, signing is independent
- **Flat src/ layout** with subdirs per module: `src/crypto/`, `src/storage/`, `src/net/`, etc.
- C++ namespace: **`chromatin::`** -- `chromatin::crypto`, `chromatin::storage`, etc.
- Tests in **`tests/` top-level** directory mirroring src/ structure: `tests/crypto/`, `tests/storage/`, etc.
- Keypair stored as **raw binary files**: `node.key` (private) + `node.pub` (public) in data directory
- **Auto-generate keypair on first start** if none exists, PLUS provide `chromatindb keygen` subcommand for manual generation
- **Separate config path from data dir**: `--config` flag for config file, `--data-dir` flag for storage/keys. Both have sensible defaults
- Default log level: **Info** -- connections, sync events, errors. Overridable via config or CLI flag

### Claude's Discretion
- CMake module organization (single vs per-module)
- KDF algorithm choice (HKDF-SHA3-256 vs HKDF-SHA256)
- Exact RAII wrapper design for crypto APIs
- Config file default locations
- spdlog format pattern and sink configuration

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| CRYP-01 | Node can generate ML-DSA-87 keypairs for signing/verification | liboqs OQS_SIG API with ML-DSA-87, key sizes: pubkey 2592B, secret key 4866B, sig 4627B |
| CRYP-02 | Node can perform ML-KEM-1024 key encapsulation/decapsulation for PQ key exchange | liboqs OQS_KEM API with ML-KEM-1024, key sizes: pubkey 1568B, ciphertext 1568B, shared secret 32B |
| CRYP-03 | Node can compute SHA3-256 hashes for namespace derivation and blob IDs | liboqs OQS_SHA3_sha3_256() one-shot API, 32-byte output |
| CRYP-04 | Node can perform ChaCha20-Poly1305 encryption/decryption for transport channel | libsodium crypto_aead_chacha20poly1305_ietf_* API, 32B key, 12B nonce, 16B tag |
| WIRE-01 | All messages use FlatBuffers with deterministic encoding | FlatBuffers C++ API with force_defaults and consistent CreateXxx() usage |
| WIRE-02 | Blob wire format includes: namespace (32B), pubkey (2592B), data (variable), ttl (u32), timestamp (u64), signature (4627B) | FlatBuffers schema with fixed-size arrays and variable-length data field |
| NSPC-01 | Namespace is derived as SHA3-256(pubkey) with no registration or authority | SHA3-256 hash of 2592-byte ML-DSA-87 public key produces 32-byte namespace |
| DAEM-01 | Node reads configuration from JSON file (bind address, storage path, bootstrap peers, default TTL) | nlohmann/json parses JSON file into C++ struct |
| DAEM-02 | Node logs all operations via structured logging (spdlog) | spdlog with console + file sinks, JSON-structured output optional |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs | 0.15.0 | ML-DSA-87 signing, ML-KEM-1024 KEM, SHA3-256 hashing | Only mature C library for NIST PQC standards (FIPS 203/204) |
| libsodium | 1.0.20+ | ChaCha20-Poly1305 AEAD, HKDF-SHA256 KDF | Industry-standard, audited, constant-time, widely deployed |
| FlatBuffers | 25.12.19 | Wire format and storage format | Zero-copy deserialization, schema evolution, deterministic structs |
| nlohmann/json | 3.12.0 | JSON configuration file parsing | De facto C++ JSON library, header-only, intuitive API |
| spdlog | 1.17.0 | Structured logging | Fastest C++ logger, fmt-based formatting, multiple sinks |
| Catch2 | 3.13.0 | Unit testing framework | Modern C++ testing, BDD-style, header-only option |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| libsodium-cmake | latest | CMake wrapper for libsodium FetchContent | Build-time only -- wraps libsodium's autotools build |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| libsodium (AEAD) | OpenSSL | OpenSSL is heavier, more attack surface, user decision says no OpenSSL |
| liboqs-cpp bindings | Raw liboqs C API | liboqs-cpp adds a dependency layer; thin RAII wrappers on C API are simpler and match user decision |
| nlohmann/json | toml++ | JSON is more widely known, already in stack spec |

**Build system:** CMake with FetchContent for all dependencies. No system package requirements except a C++20 compiler.

## Architecture Patterns

### Recommended Project Structure
```
chromatindb/
├── CMakeLists.txt              # Root CMake, FetchContent for all deps
├── schemas/
│   └── blob.fbs                # FlatBuffers schema for blob wire format
├── src/
│   ├── crypto/
│   │   ├── signing.h/cpp       # ML-DSA-87 RAII wrapper (keypair, sign, verify)
│   │   ├── kem.h/cpp           # ML-KEM-1024 RAII wrapper (encaps, decaps)
│   │   ├── hash.h/cpp          # SHA3-256 wrapper
│   │   ├── aead.h/cpp          # ChaCha20-Poly1305 wrapper
│   │   └── kdf.h/cpp           # HKDF-SHA256 wrapper
│   ├── wire/
│   │   ├── blob_generated.h    # FlatBuffers generated code
│   │   └── codec.h/cpp         # Serialize/deserialize helpers, canonical signing
│   ├── config/
│   │   └── config.h/cpp        # JSON config loading + CLI args
│   ├── logging/
│   │   └── logging.h/cpp       # spdlog initialization and helpers
│   └── identity/
│       └── identity.h/cpp      # Keypair generation, namespace derivation, key I/O
├── tests/
│   ├── crypto/
│   │   ├── test_signing.cpp
│   │   ├── test_kem.cpp
│   │   ├── test_hash.cpp
│   │   ├── test_aead.cpp
│   │   └── test_kdf.cpp
│   ├── wire/
│   │   └── test_codec.cpp
│   ├── config/
│   │   └── test_config.cpp
│   └── identity/
│       └── test_identity.cpp
└── include/                    # Public headers (if needed later)
```

### Pattern 1: RAII Crypto Wrappers
**What:** Thin C++ classes that own liboqs/libsodium resources and free them in destructors
**When to use:** Every crypto operation -- prevents memory leaks of sensitive key material
**Example:**
```cpp
// chromatin::crypto::Signer -- owns ML-DSA-87 keypair
class Signer {
    OQS_SIG* sig_;
    std::vector<uint8_t> public_key_;
    SecureBytes secret_key_; // zeroed on destruction
public:
    Signer();  // creates OQS_SIG_new(OQS_SIG_alg_ml_dsa_87)
    ~Signer(); // OQS_SIG_free + secure erase
    void generate_keypair();
    std::vector<uint8_t> sign(std::span<const uint8_t> message);
    static bool verify(std::span<const uint8_t> message,
                       std::span<const uint8_t> signature,
                       std::span<const uint8_t> public_key);
};
```

### Pattern 2: Canonical Signing Domain
**What:** Fixed binary concatenation for signing, independent of FlatBuffers
**When to use:** All blob signing and verification
**Example:**
```cpp
// Build canonical signing input: namespace(32) || data(var) || ttl(4) || timestamp(8)
std::vector<uint8_t> build_signing_input(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp)
{
    std::vector<uint8_t> buf;
    buf.reserve(32 + data.size() + 4 + 8);
    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());
    buf.insert(buf.end(), data.begin(), data.end());
    // Little-endian ttl and timestamp
    auto append_le = [&](auto val) {
        for (size_t i = 0; i < sizeof(val); ++i)
            buf.push_back(static_cast<uint8_t>(val >> (8 * i)));
    };
    append_le(ttl);
    append_le(timestamp);
    return buf;
}
// Then: hash = SHA3-256(signing_input), signature = ML-DSA-87.sign(hash, secret_key)
```

### Pattern 3: FlatBuffers Deterministic Encoding
**What:** Use consistent API (CreateXxx with force_defaults) to get reproducible bytes
**When to use:** All FlatBuffer serialization
**Example:**
```cpp
flatbuffers::FlatBufferBuilder builder(4096);
builder.ForceDefaults(true); // Include zero-value fields for determinism
// Always use Create* methods in same order, never mix add_* calls
```

### Anti-Patterns to Avoid
- **Signing FlatBuffer bytes directly:** Ties signing to serialization format; use canonical concatenation instead
- **Using liboqs-cpp wrapper:** Adds unnecessary dependency; thin RAII on C API is simpler
- **OpenSSL for anything:** Explicitly excluded by user; liboqs + libsodium covers everything
- **Random nonces for AEAD without counter:** Risk of nonce reuse; use counter-based nonce generation

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| AEAD encryption | Custom encrypt-then-MAC | libsodium crypto_aead_chacha20poly1305_ietf_* | Nonce management, constant-time comparison, authenticated |
| Key derivation | Custom KDF | libsodium crypto_kdf_hkdf_sha256_* | RFC 5869 compliant, well-tested extract-expand pattern |
| Secure memory | Manual memset for key erasure | OQS_MEM_secure_free / sodium_memzero | Compiler may optimize away memset; these resist optimization |
| PQ signatures | Custom lattice math | liboqs OQS_SIG_sign/verify | NIST FIPS 204 reference implementation |
| PQ key exchange | Custom key exchange | liboqs OQS_KEM_encaps/decaps | NIST FIPS 203 reference implementation |
| JSON parsing | sscanf/manual parsing | nlohmann/json | Edge cases in JSON spec are endless |
| Logging | printf/iostream | spdlog | Thread safety, log levels, rotation, formatting |

**Key insight:** PQ cryptography is mathematically complex and implementation-sensitive. Any deviation from reference implementations risks subtle vulnerabilities. Use the audited libraries exactly as designed.

## Common Pitfalls

### Pitfall 1: Insecure Key Material Cleanup
**What goes wrong:** Secret keys linger in memory after use because the compiler optimizes away `memset`
**Why it happens:** C/C++ compilers can remove "dead stores" to buffers that are about to be freed
**How to avoid:** Use `OQS_MEM_secure_free()` for liboqs allocations, `sodium_memzero()` for libsodium buffers. For RAII wrappers, create a `SecureBytes` type that calls `sodium_memzero` in its destructor
**Warning signs:** Using `delete[]` or `std::vector::clear()` on secret key buffers

### Pitfall 2: FlatBuffers Non-Determinism
**What goes wrong:** Same logical blob produces different bytes on re-serialization
**Why it happens:** FlatBuffers encoding depends on field order in API calls and whether defaults are forced
**How to avoid:** Always use `ForceDefaults(true)`, always use `CreateXxx()` convenience methods (not manual `add_field()` calls), test roundtrip: serialize -> deserialize -> re-serialize produces identical bytes
**Warning signs:** Blob hashes changing for identical logical content

### Pitfall 3: AEAD Nonce Reuse
**What goes wrong:** Encrypting two messages with the same nonce and key completely breaks ChaCha20-Poly1305 confidentiality
**Why it happens:** Random nonce generation has birthday collision risk at ~2^48 messages for 96-bit nonce
**How to avoid:** Use a counter-based nonce scheme (increment for each message in a session). For Phase 1 we only implement the primitives; the transport layer (Phase 4) will manage nonce state per connection
**Warning signs:** Random nonce generation without collision tracking

### Pitfall 4: Signing the Wrong Thing
**What goes wrong:** Signing FlatBuffer bytes instead of canonical concatenation, or signing data without including namespace/ttl/timestamp
**Why it happens:** Natural instinct to sign "the message" rather than a separate canonical form
**How to avoid:** The signing input is always `SHA3-256(namespace || data || ttl || timestamp)`. Wrap this in a function that takes structured input and returns the digest. Never pass raw FlatBuffer bytes to sign
**Warning signs:** Signature verification failing after FlatBuffer schema changes

### Pitfall 5: ML-DSA-87 Key Size Assumptions
**What goes wrong:** Buffer overflows or truncation because ML-DSA-87 keys are much larger than classical keys
**Why it happens:** ML-DSA-87 pubkey is 2592 bytes, secret key is 4866 bytes, signature is 4627 bytes -- 100x larger than Ed25519
**How to avoid:** Always use `sig->length_public_key`, `sig->length_secret_key`, `sig->length_signature` from the OQS_SIG struct rather than hardcoded constants
**Warning signs:** Hardcoded buffer sizes, static arrays sized for classical crypto

## Code Examples

### ML-DSA-87 Keypair Generation
```cpp
// Source: liboqs API documentation
#include <oqs/oqs.h>

OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
if (!sig) { /* error: algorithm not available */ }

uint8_t* public_key = (uint8_t*)OQS_MEM_malloc(sig->length_public_key);  // 2592 bytes
uint8_t* secret_key = (uint8_t*)OQS_MEM_malloc(sig->length_secret_key);  // 4866 bytes

OQS_STATUS rc = OQS_SIG_keypair(sig, public_key, secret_key);
if (rc != OQS_SUCCESS) { /* error */ }

// Sign a message
uint8_t* signature = (uint8_t*)OQS_MEM_malloc(sig->length_signature);    // 4627 bytes
size_t signature_len;
rc = OQS_SIG_sign(sig, signature, &signature_len, message, message_len, secret_key);

// Verify
rc = OQS_SIG_verify(sig, message, message_len, signature, signature_len, public_key);

// Cleanup
OQS_MEM_secure_free(secret_key, sig->length_secret_key);
OQS_MEM_insecure_free(public_key);
OQS_MEM_insecure_free(signature);
OQS_SIG_free(sig);
```

### ML-KEM-1024 Key Exchange
```cpp
// Source: liboqs API documentation
#include <oqs/oqs.h>

OQS_KEM* kem = OQS_KEM_new(OQS_KEM_alg_ml_kem_1024);
if (!kem) { /* error */ }

uint8_t* public_key = (uint8_t*)OQS_MEM_malloc(kem->length_public_key);     // 1568 bytes
uint8_t* secret_key = (uint8_t*)OQS_MEM_malloc(kem->length_secret_key);     // 3168 bytes
uint8_t* ciphertext = (uint8_t*)OQS_MEM_malloc(kem->length_ciphertext);     // 1568 bytes
uint8_t* shared_secret_e = (uint8_t*)OQS_MEM_malloc(kem->length_shared_secret); // 32 bytes
uint8_t* shared_secret_d = (uint8_t*)OQS_MEM_malloc(kem->length_shared_secret); // 32 bytes

OQS_KEM_keypair(kem, public_key, secret_key);
OQS_KEM_encaps(kem, ciphertext, shared_secret_e, public_key);
OQS_KEM_decaps(kem, shared_secret_d, ciphertext, secret_key);
// shared_secret_e == shared_secret_d

OQS_MEM_secure_free(secret_key, kem->length_secret_key);
OQS_MEM_secure_free(shared_secret_e, kem->length_shared_secret);
OQS_MEM_secure_free(shared_secret_d, kem->length_shared_secret);
OQS_MEM_insecure_free(public_key);
OQS_MEM_insecure_free(ciphertext);
OQS_KEM_free(kem);
```

### SHA3-256 Hashing
```cpp
// Source: liboqs SHA3 API header
#include <oqs/oqs.h>

uint8_t hash[32];
OQS_SHA3_sha3_256(hash, input_data, input_len);
// hash now contains the 32-byte SHA3-256 digest
```

### ChaCha20-Poly1305 AEAD
```cpp
// Source: libsodium documentation
#include <sodium.h>

// Key: 32 bytes, Nonce: 12 bytes, Tag: 16 bytes (appended to ciphertext)
unsigned char key[crypto_aead_chacha20poly1305_IETF_KEYBYTES];
unsigned char nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];
crypto_aead_chacha20poly1305_ietf_keygen(key);
randombytes_buf(nonce, sizeof(nonce));

unsigned char ciphertext[MESSAGE_LEN + crypto_aead_chacha20poly1305_IETF_ABYTES];
unsigned long long ciphertext_len;

crypto_aead_chacha20poly1305_ietf_encrypt(
    ciphertext, &ciphertext_len,
    message, message_len,
    ad, ad_len,      // additional data (authenticated but not encrypted)
    NULL,             // nsec (unused, pass NULL)
    nonce, key);

// Decrypt
unsigned char decrypted[MESSAGE_LEN];
unsigned long long decrypted_len;
if (crypto_aead_chacha20poly1305_ietf_decrypt(
        decrypted, &decrypted_len, NULL,
        ciphertext, ciphertext_len,
        ad, ad_len, nonce, key) != 0) {
    // authentication failed -- tampered or wrong key
}
```

### HKDF-SHA256 Key Derivation
```cpp
// Source: libsodium documentation
#include <sodium.h>

// Extract: IKM (input key material) -> PRK (pseudorandom key)
unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
crypto_kdf_hkdf_sha256_extract(prk, salt, salt_len, ikm, ikm_len);

// Expand: PRK + context -> derived key
unsigned char derived_key[32];
crypto_kdf_hkdf_sha256_expand(derived_key, sizeof(derived_key),
    "chromatindb-channel-key", 23,  // context string
    prk);
```

### Namespace Derivation
```cpp
// Namespace = SHA3-256(public_key)
uint8_t namespace_id[32];
OQS_SHA3_sha3_256(namespace_id, public_key, 2592); // ML-DSA-87 pubkey is 2592 bytes
```

## KDF Recommendation (Claude's Discretion)

**Recommendation: HKDF-SHA256** (via libsodium)

Rationale:
- libsodium has native HKDF-SHA256 support since v1.0.19 (`crypto_kdf_hkdf_sha256_*`)
- libsodium does NOT have HKDF-SHA3-256 -- would require hand-rolling with raw SHA3
- HKDF-SHA256 is RFC 5869 compliant, used in TLS 1.3, Signal Protocol, etc.
- The KDF derives symmetric keys from the ML-KEM shared secret; SHA256 vs SHA3-256 makes no security difference here since the input (32-byte shared secret) is already high-entropy
- Using libsodium's battle-tested implementation is safer than rolling HKDF-SHA3-256

## CMake Recommendation (Claude's Discretion)

**Recommendation: Single root CMakeLists.txt with per-module source grouping**

Rationale:
- Phase 1 has ~10 source files -- per-module CMakeLists.txt is over-engineering
- Single CMakeLists.txt with `target_sources()` grouping keeps it simple
- Can refactor to per-module CMake later if the project grows significantly
- All FetchContent declarations in one place for easy version management

## spdlog Recommendation (Claude's Discretion)

**Recommendation:** Console sink (stderr) by default, file sink optional via config. Pattern: `[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v`

Rationale:
- Console sink for daemon use (systemd captures stderr)
- File sink configurable for standalone use
- Include logger name (`%n`) for module-level filtering (e.g., "crypto", "config")
- Info level default, debug/trace available via config

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Dilithium (CRYSTALS) | ML-DSA (FIPS 204) | Aug 2024 | Dilithium removed from liboqs 0.15.0; use ML-DSA only |
| CRYSTALS-Kyber | ML-KEM (FIPS 203) | Aug 2024 | Kyber renamed to ML-KEM in final NIST standard |
| AES-256-GCM | ChaCha20-Poly1305 | User decision | ChaCha20 is software-fast, no AES-NI dependency |
| OpenSSL for symmetric | libsodium | User decision | Smaller, audited, no OpenSSL dependency |

**Deprecated/outdated:**
- Dilithium: Removed from liboqs 0.15.0, superseded by ML-DSA
- CRYSTALS-Kyber: Renamed to ML-KEM in FIPS 203 final standard

## Open Questions

1. **Endianness of canonical signing format**
   - What we know: The canonical format is `namespace || data || ttl || timestamp`
   - What's unclear: Whether ttl (u32) and timestamp (u64) should be little-endian or big-endian in the signing input
   - Recommendation: Use little-endian (matching x86/ARM defaults and FlatBuffers wire format) and document it explicitly. This is a protocol-level decision that must be consistent forever

2. **FlatBuffers schema: fixed-size arrays vs byte vectors**
   - What we know: Namespace is 32 bytes, pubkey is 2592 bytes, signature is 4627 bytes
   - What's unclear: Whether to use FlatBuffers `[ubyte:32]` fixed arrays or `[ubyte]` variable vectors
   - Recommendation: Use `[ubyte]` vectors for all byte fields. FlatBuffers fixed-size arrays (`[ubyte:N]`) are structs and must be stored inline, which is less flexible. Vectors are the standard FlatBuffers pattern for byte data

## Sources

### Primary (HIGH confidence)
- liboqs 0.15.0 release notes -- ML-DSA-87 and ML-KEM-1024 algorithm support confirmed
- libsodium documentation (doc.libsodium.org) -- ChaCha20-Poly1305 IETF API, HKDF-SHA256 API
- NIST FIPS 203 -- ML-KEM-1024 key sizes (pubkey 1568B, ciphertext 1568B, shared secret 32B)
- NIST FIPS 204 -- ML-DSA-87 key sizes (pubkey 2592B, secret key 4866B, signature 4627B)
- liboqs SHA3 header -- OQS_SHA3_sha3_256() one-shot API confirmed

### Secondary (MEDIUM confidence)
- FlatBuffers deterministic encoding discussion -- confirmed not byte-deterministic across different API usage patterns; force_defaults + consistent API needed
- libsodium-cmake wrapper -- FetchContent integration pattern confirmed

### Tertiary (LOW confidence)
- None -- all findings verified against primary sources

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries verified against official docs and release notes
- Architecture: HIGH -- signing model, RAII patterns, and project structure are well-established patterns
- Pitfalls: HIGH -- PQ key sizes, FlatBuffers non-determinism, and nonce reuse are well-documented issues

**Research date:** 2026-03-03
**Valid until:** 2026-04-03 (stable domain, 30-day validity)
